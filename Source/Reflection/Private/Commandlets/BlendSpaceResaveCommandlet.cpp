/* Copyright Reflection Contributors 2024-2026 */

#include "BlendSpaceResaveCommandlet.h"

#include "Engine/Compatibility.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#include "Animation/BlendSpace.h"
#include "Animation/AnimSequence.h"
#include "UObject/SavePackage.h"

DECLARE_LOG_CATEGORY_CLASS(LogBlendSpaceResave, All, All);

int32 UBlendSpaceResaveCommandlet::Main(const FString& Params) {
	FString Root = TEXT("/Game");
	FParse::Value(*Params, TEXT("path="), Root);

	const bool bDryRun = FParse::Param(*Params, TEXT("dryrun"));

	/* Names the one blend space to describe sample by sample instead of resaving anything, because a
	 * direction axis that lost a sample blends toward nothing and only shows up while turning */
	FString Dump;
	const bool bDump = FParse::Value(*Params, TEXT("dump="), Dump) && !Dump.IsEmpty();

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.ScanPathsSynchronous({ Root }, true);

	FARFilter Filter;
	Filter.PackagePaths.Add(*Root);
	Filter.bRecursivePaths = true;
	/* Aim offsets and the 1D variants are all UBlendSpace underneath, and all of them are sampled
	 * through the same triangulation, so all of them are broken the same way */
	Filter.ClassPaths.Add(UBlendSpace::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	UE_LOG(LogBlendSpaceResave, Display, TEXT("%d blend space(s) under %s"), Assets.Num(), *Root);

	int32 Rebuilt = 0;
	int32 Saved = 0;
	int32 Failed = 0;

	for (const FAssetData& Asset : Assets) {
		UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset.GetAsset());
		if (!BlendSpace) {
			UE_LOG(LogBlendSpaceResave, Warning, TEXT("could not load %s"), *Asset.GetObjectPathString());
			Failed++;

			continue;
		}

		if (bDump) {
			if (!BlendSpace->GetName().Contains(Dump)) {
				continue;
			}

			const FBlendParameter& AxisX = BlendSpace->GetBlendParameter(0);
			const FBlendParameter& AxisY = BlendSpace->GetBlendParameter(1);

			UE_LOG(LogBlendSpaceResave, Display, TEXT("%s: X[%s %.1f..%.1f grid=%d snap=%d] Y[%s %.1f..%.1f grid=%d snap=%d] runtimeDataEmpty=%d"),
				*BlendSpace->GetName(), *AxisX.DisplayName, AxisX.Min, AxisX.Max, AxisX.GridNum, AxisX.bSnapToGrid,
				*AxisY.DisplayName, AxisY.Min, AxisY.Max, AxisY.GridNum, AxisY.bSnapToGrid,
				BlendSpace->GetBlendSpaceData().IsEmpty());

			const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
			for (int32 Index = 0; Index < Samples.Num(); ++Index) {
				const FBlendSample& Sample = Samples[Index];

				UE_LOG(LogBlendSpaceResave, Display, TEXT("   [%2d] valid=%d at (%7.1f,%7.1f) rate=%.2f markers=%2d %s"),
					Index, Sample.bIsValid, Sample.SampleValue.X, Sample.SampleValue.Y, Sample.RateScale,
					Sample.Animation ? Sample.Animation->AuthoredSyncMarkers.Num() : -1,
					*GetNameSafe(Sample.Animation));
			}

			Rebuilt++;

			continue;
		}

		const bool bWasEmpty = BlendSpace->GetBlendSpaceData().IsEmpty();

		BlendSpace->ResampleData();
		Rebuilt++;

		UE_LOG(LogBlendSpaceResave, Display, TEXT("%s: samples=%d grid=%d runtimeData %s"), *BlendSpace->GetName(),
			BlendSpace->GetNumberOfBlendSamples(), BlendSpace->GetGridSamples().Num(),
			bWasEmpty ? (BlendSpace->GetBlendSpaceData().IsEmpty() ? TEXT("still empty") : TEXT("rebuilt")) : TEXT("was already built"));

		if (bDryRun) {
			continue;
		}

		UPackage* Package = BlendSpace->GetOutermost();
		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
			Saved++;
		} else {
			UE_LOG(LogBlendSpaceResave, Warning, TEXT("could not save %s"), *FileName);
			Failed++;
		}
	}

	UE_LOG(LogBlendSpaceResave, Display, TEXT("%d rebuilt, %d saved, %d failed"), Rebuilt, Saved, Failed);

	return Failed > 0 ? 1 : 0;
}
