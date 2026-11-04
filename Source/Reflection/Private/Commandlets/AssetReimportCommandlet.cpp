/* Copyright Reflection Contributors 2024-2026 */

#include "AssetReimportCommandlet.h"

#include "FileHelpers.h"
#include "Importers/Constructor/Asset.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Importer.h"
#include "Misc/FileHelper.h"
#include "Modules/Cloud/Cloud.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

DECLARE_LOG_CATEGORY_CLASS(LogAssetReimport, All, All);

namespace {
	int32 ReimportOne(const FString& Path, const FString& Params, bool bDryRun);
}

int32 UAssetReimportCommandlet::Main(const FString& Params) {
	FString Path, List;
	TArray<FString> Paths;

	if (FParse::Value(*Params, TEXT("list="), List) && !List.IsEmpty()) {
		FFileHelper::LoadFileToStringArray(Paths, *List);
		Paths.RemoveAll([](const FString& Line) { return Line.TrimStartAndEnd().IsEmpty(); });
	} else if (FParse::Value(*Params, TEXT("path="), Path) && !Path.IsEmpty()) {
		Paths.Add(Path);
	} else {
		UE_LOG(LogAssetReimport, Error, TEXT("-path=/Game/... or -list=<file of paths> is required"));

		return 1;
	}

	const bool bDryRun = FParse::Param(*Params, TEXT("dryrun"));

	/* Only this asset: anything it names that is not already in the project stays a bare path
	 * instead of being reflected along with it */
	if (FParse::Param(*Params, TEXT("nographs"))) {
		FAssetUtilities::bConstructGraphs = false;
	}

	if (FParse::Param(*Params, TEXT("norefs"))) {
		FAssetUtilities::bImportReferences = false;

		UE_LOG(LogAssetReimport, Display, TEXT("not reflecting referenced assets"));
	}

	/* The runtime carries the project name a cloud path is turned into an editor one with, and
	 * nothing has fetched it in a run with no editor around it */
	Cloud::EnsureMetadataBlocking();

	int32 Failed = 0;

	for (int32 Index = 0; Index < Paths.Num(); ++Index) {
		const FString& One = Paths[Index].TrimStartAndEnd();

		if (Paths.Num() > 1) {
			UE_LOG(LogAssetReimport, Display, TEXT("[%d/%d] %s"), Index + 1, Paths.Num(), *One);
		}

		if (ReimportOne(One, Params, bDryRun) != 0) Failed++;

		/* A long list holds every import in memory unless it is let go now and then */
		if ((Index + 1) % 25 == 0) {
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	if (Paths.Num() > 1) {
		UE_LOG(LogAssetReimport, Display, TEXT("%d of %d imported, %d failed"), Paths.Num() - Failed, Paths.Num(), Failed);
	}

	return Failed > 0 ? 1 : 0;
}

namespace {
int32 ReimportOne(const FString& Path, const FString& Params, const bool bDryRun) {
	const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(Path);

	if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
		UE_LOG(LogAssetReimport, Error, TEXT("the cloud had nothing at \"%s\""), *Path);

		return 1;
	}

	const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));

	UE_LOG(LogAssetReimport, Display, TEXT("\"%s\": %d export(s) from the cloud"), *Path, Exports.Num());

	/* What the import is made from, kept so the result can be checked against it rather than against
	 * whatever is sitting in an export folder from some other build */
	if (FString Dump; FParse::Value(*Params, TEXT("json="), Dump) && !Dump.IsEmpty()) {
		FString Written;

		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Written);

		if (FJsonSerializer::Serialize(Response.ToSharedRef(), Writer) && FFileHelper::SaveStringToFile(Written, *Dump)) {
			UE_LOG(LogAssetReimport, Display, TEXT("kept what the cloud gave back at \"%s\""), *Dump);
		} else {
			UE_LOG(LogAssetReimport, Error, TEXT("could not keep what the cloud gave back at \"%s\""), *Dump);
		}
	}

	if (bDryRun) {
		UE_LOG(LogAssetReimport, Display, TEXT("dry run, nothing imported"));

		return 0;
	}

	IImporter* Importer = nullptr;

	if (!IImportReader::ReadExportsAndImport(Exports, Path, Importer, true, false) || Importer == nullptr) {
		UE_LOG(LogAssetReimport, Error, TEXT("nothing was made from \"%s\""), *Path);

		return 1;
	}

	TArray<UPackage*> Dirty;
	FEditorFileUtils::GetDirtyContentPackages(Dirty);
	FEditorFileUtils::GetDirtyWorldPackages(Dirty);

	int32 Saved = 0;

	for (UPackage* Package : Dirty) {
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
			Saved++;

			UE_LOG(LogAssetReimport, Display, TEXT("saved %s"), *FileName);
		} else {
			UE_LOG(LogAssetReimport, Warning, TEXT("could not save %s"), *FileName);
		}
	}

	UE_LOG(LogAssetReimport, Display, TEXT("%d package(s) saved"), Saved);

	return 0;
}
}
