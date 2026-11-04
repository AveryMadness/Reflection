/* Copyright Reflection Contributors 2024-2026 */

#include "AnimCurveReimportCommandlet.h"

#include "Engine/Compatibility.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "FileHelpers.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Tools/AnimationData.h"
#include "Serializers/SerializerContainer.h"
#include "UObject/SavePackage.h"

DECLARE_LOG_CATEGORY_CLASS(LogAnimCurveReimport, All, All);

namespace {
	/* The cloud answers with every export in the package. The sequence is the one whose name matches the
	 * asset; falling back to the type, because a few packages name the export something else. */
	TSharedPtr<FJsonObject> FindSequenceExport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& AssetName) {
		TSharedPtr<FJsonObject> ByType;

		for (const TSharedPtr<FJsonValue>& Value : Exports) {
			const TSharedPtr<FJsonObject> Export = Value->AsObject();
			if (!Export.IsValid() || !Export->HasTypedField<EJson::Object>(TEXT("Properties"))) {
				continue;
			}

			FString Type;
			Export->TryGetStringField(TEXT("Type"), Type);

			if (!Type.Contains(TEXT("AnimSequence")) && !Type.Contains(TEXT("AnimMontage"))) {
				continue;
			}

			FString Name;
			if (Export->TryGetStringField(TEXT("Name"), Name) && Name == AssetName) {
				return Export;
			}

			if (!ByType.IsValid()) {
				ByType = Export;
			}
		}

		return ByType;
	}

	int32 SaveDirtyPackages(int32& OutFailed) {
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyContentPackages(Dirty);

		int32 Saved = 0;

		for (UPackage* Package : Dirty) {
			const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

			if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
				Saved++;
			} else {
				UE_LOG(LogAnimCurveReimport, Warning, TEXT("could not save %s"), *FileName);
				OutFailed++;
			}
		}

		return Saved;
	}
}

int32 UAnimCurveReimportCommandlet::Main(const FString& Params) {
	FString Root = TEXT("/Game/Animation/Game/MainPlayer");
	FParse::Value(*Params, TEXT("path="), Root);

	int32 Limit = MAX_int32;
	FParse::Value(*Params, TEXT("limit="), Limit);

	const bool bDryRun = FParse::Param(*Params, TEXT("dryrun"));
	const bool bSkipWithCurves = FParse::Param(*Params, TEXT("skipwithcurves"));

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.ScanPathsSynchronous({ Root }, true);

	FARFilter Filter;
	Filter.PackagePaths.Add(*Root);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	UE_LOG(LogAnimCurveReimport, Display, TEXT("%d sequence(s) under %s"), Assets.Num(), *Root);

	/* The runtime carries the project name a cloud path is turned into an editor one with, and nothing
	 * has fetched it in a run with no editor around it */
	Cloud::EnsureMetadataBlocking();

	int32 Considered = 0;
	int32 Applied = 0;
	int32 Skipped = 0;
	int32 Failed = 0;
	int32 Saved = 0;
	int32 CurvesWritten = 0;

	for (const FAssetData& Asset : Assets) {
		if (Considered >= Limit) {
			break;
		}

		UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset());
		if (!Sequence) {
			UE_LOG(LogAnimCurveReimport, Warning, TEXT("could not load %s"), *Asset.GetObjectPathString());
			Failed++;

			continue;
		}

		if (bSkipWithCurves && Sequence->GetCurveData().FloatCurves.Num() > 0) {
			Skipped++;

			continue;
		}

		Considered++;

		const FString Path = Asset.PackageName.ToString();

		if (bDryRun) {
			UE_LOG(LogAnimCurveReimport, Display, TEXT("would read curves for %s"), *Path);

			continue;
		}

		const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(Path);
		if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
			UE_LOG(LogAnimCurveReimport, Warning, TEXT("the cloud had nothing at \"%s\""), *Path);
			Failed++;

			continue;
		}

		const TSharedPtr<FJsonObject> Export = FindSequenceExport(Response->GetArrayField(TEXT("exports")), Sequence->GetName());
		if (!Export.IsValid()) {
			UE_LOG(LogAnimCurveReimport, Warning, TEXT("no sequence export in \"%s\""), *Path);
			Failed++;

			continue;
		}

		/* Only the curves. ReadAnimationCurves goes through the data controller and touches nothing but
		 * RCT_Float, so the bone tracks, length, sync markers and notifies the asset already has are left
		 * exactly as they are - running the whole importer here is what added camera/warp_point/attach
		 * bone tracks to 134 sequences and wrecked the aim offsets. */
		USerializerContainer Container;
		Container.AssetExport->JsonObject = Export;

		const int32 Before = Sequence->GetCurveData().FloatCurves.Num();

		if (!ReadAnimationCurves(&Container, Sequence)) {
			UE_LOG(LogAnimCurveReimport, Warning, TEXT("curves would not read for \"%s\""), *Path);
			Failed++;

			continue;
		}

		const int32 After = Sequence->GetCurveData().FloatCurves.Num();
		CurvesWritten += After;

		if (After != Before) {
			UE_LOG(LogAnimCurveReimport, Display, TEXT("%s: %d -> %d curve(s)"), *Sequence->GetName(), Before, After);
		}

		Sequence->GetOutermost()->MarkPackageDirty();
		Applied++;

		/* Saved in batches: a run over the whole player set is a thousand packages, and holding every one
		 * of them dirty in memory is what makes the commandlet fall over rather than the importing */
		if (Applied % 50 == 0) {
			Saved += SaveDirtyPackages(Failed);
			CollectGarbage(RF_NoFlags);

			UE_LOG(LogAnimCurveReimport, Display, TEXT("%d applied, %d saved so far"), Applied, Saved);
		}
	}

	if (!bDryRun) {
		Saved += SaveDirtyPackages(Failed);
	}

	UE_LOG(LogAnimCurveReimport, Display, TEXT("%d considered, %d had curves applied (%d curve(s) total), %d skipped, %d saved, %d failed"),
		Considered, Applied, CurvesWritten, Skipped, Saved, Failed);

	return Failed > 0 ? 1 : 0;
}
