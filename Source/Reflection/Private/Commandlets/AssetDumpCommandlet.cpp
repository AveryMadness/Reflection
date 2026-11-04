/* Copyright Reflection Contributors 2024-2026 */

#include "AssetDumpCommandlet.h"

#include "Engine/DataTable.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

DECLARE_LOG_CATEGORY_CLASS(LogAssetDump, All, All);

static bool DumpObject(const FString& Path, FString OutFile) {
	UObject* Asset = LoadObject<UObject>(nullptr, *Path);

	if (Asset == nullptr) {
		UE_LOG(LogAssetDump, Error, TEXT("nothing loadable at \"%s\""), *Path);

		return false;
	}

	if (OutFile.IsEmpty()) {
		OutFile = FPaths::ProjectSavedDir() / (Asset->GetName() + TEXT(".json"));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("asset"), Asset->GetName());
	Root->SetStringField(TEXT("package"), Asset->GetOutermost()->GetName());
	Root->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	FJsonObjectConverter::UStructToJsonObject(Asset->GetClass(), Asset, Properties.ToSharedRef(), 0, 0);

	Root->SetObjectField(TEXT("properties"), Properties);

	/* A table's rows are not properties of it, so nothing above reaches them */
	if (const UDataTable* Table = Cast<UDataTable>(Asset)) {
		TArray<TSharedPtr<FJsonValue>> Rows;

		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Table->GetTableAsJSON()), Rows)) {
			Root->SetArrayField(TEXT("rows"), Rows);
		}
	}

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	if (!FFileHelper::SaveStringToFile(Text, *OutFile)) {
		UE_LOG(LogAssetDump, Error, TEXT("could not write \"%s\""), *OutFile);

		return false;
	}

	UE_LOG(LogAssetDump, Display, TEXT("wrote \"%s\" (%s)"), *OutFile, *Asset->GetClass()->GetName());

	return true;
}

int32 UAssetDumpCommandlet::Main(const FString& Params) {
	FString Path, OutFile, OutDir;

	if (!FParse::Value(*Params, TEXT("path="), Path, false) || Path.IsEmpty()) {
		UE_LOG(LogAssetDump, Error, TEXT("-path=/Game/... is required"));

		return 1;
	}

	FParse::Value(*Params, TEXT("out="), OutFile);
	FParse::Value(*Params, TEXT("outdir="), OutDir);

	/* Several objects in one run (paths split by ';'), each written as <outdir>/<object name>.json */
	TArray<FString> Paths;
	Path.ParseIntoArray(Paths, TEXT(";"));

	int32 Failed = 0;

	for (const FString& One : Paths) {
		FString Target = OutFile;

		if (!OutDir.IsEmpty()) {
			FString Name = FPaths::GetExtension(One);
			Name.ReplaceCharInline(TEXT(':'), TEXT('.'));

			Target = OutDir / (Name.IsEmpty() ? FPaths::GetBaseFilename(One) : Name) + TEXT(".json");
		}

		if (!DumpObject(One, Paths.Num() == 1 || !OutDir.IsEmpty() ? Target : FString())) {
			Failed++;
		}
	}

	return Failed == 0 ? 0 : 1;
}
