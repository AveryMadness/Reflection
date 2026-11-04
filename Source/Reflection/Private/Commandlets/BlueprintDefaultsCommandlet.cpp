/* Copyright Reflection Contributors 2024-2026 */

#include "BlueprintDefaultsCommandlet.h"

#include "Engine/Blueprint.h"
#include "Importers/Constructor/Asset.h"
#include "Misc/PackageName.h"
#include "Modules/Cloud/Cloud.h"
#include "Serializers/ObjectSerializer.h"
#include "Serializers/SerializerContainer.h"
#include "UObject/SavePackage.h"

DECLARE_LOG_CATEGORY_CLASS(LogBlueprintDefaults, All, All);

namespace {
	/* Every package a json value names, from either reference shape the Cloud writes */
	void CollectReferencedPackages(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutPackages) {
		if (!Value.IsValid()) return;

		if (Value->Type == EJson::Array) {
			for (const TSharedPtr<FJsonValue>& Element : Value->AsArray()) {
				CollectReferencedPackages(Element, OutPackages);
			}

			return;
		}

		if (Value->Type != EJson::Object) return;

		const TSharedPtr<FJsonObject> Object = Value->AsObject();

		FString Path;

		if (Object->TryGetStringField(TEXT("AssetPathName"), Path) || Object->TryGetStringField(TEXT("ObjectPath"), Path)) {
			FString Package = Path;
			Package.Split(TEXT("."), &Package, nullptr);

			if (!Package.IsEmpty() && Package != TEXT("None")) {
				OutPackages.Add(Package);
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values) {
			CollectReferencedPackages(Pair.Value, OutPackages);
		}
	}

	TSharedPtr<FJsonObject> FindExport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& Name, const FString& OuterName) {
		for (const TSharedPtr<FJsonValue>& Value : Exports) {
			const TSharedPtr<FJsonObject> Export = Value->AsObject();

			if (Export->GetStringField(TEXT("Name")) != Name) continue;

			if (OuterName.IsEmpty()) return Export;

			const TSharedPtr<FJsonObject>* Outer = nullptr;

			if (Export->TryGetObjectField(TEXT("Outer"), Outer) && (*Outer)->GetStringField(TEXT("ObjectName")).Contains(OuterName)) {
				return Export;
			}
		}

		return nullptr;
	}

	void ResetToArchetype(UObject* Target) {
		const UObject* Archetype = Target->GetArchetype();

		if (Archetype == nullptr || Archetype->GetClass() != Target->GetClass()) {
			UE_LOG(LogBlueprintDefaults, Warning, TEXT("  %s: no archetype of the same class, not reset"), *Target->GetName());

			return;
		}

		int32 Reset = 0;

		for (FProperty* Property = Target->GetClass()->PropertyLink; Property; Property = Property->PropertyLinkNext) {
			/* Instanced references would end up pointing at the archetype's own subobjects */
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_Deprecated)) continue;

			if (!Property->Identical_InContainer(Target, Archetype)) {
				Property->CopyCompleteValue_InContainer(Target, Archetype);
				Reset++;
			}
		}

		UE_LOG(LogBlueprintDefaults, Display, TEXT("  %s: %d value(s) reset to the archetype"), *Target->GetName(), Reset);
	}

	int32 Apply(UObject* Target, const TSharedPtr<FJsonObject>& Properties, const TSet<FString>& Skip, const FString& OwnPackage, const bool bDryRun) {
		USerializerContainer Container;

		int32 Applied = 0;

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values) {
			FString Name = Pair.Key;

			if (Skip.Contains(Name)) continue;

			if (Pair.Value->Type == EJson::Object && Pair.Value->AsObject()->Values.Num() == 0) continue;

			if (Target->GetClass()->FindPropertyByName(*Name) == nullptr) {
				UE_LOG(LogBlueprintDefaults, Display, TEXT("  %s.%s: no such property in %s, skipped"), *Target->GetName(), *Name, *Target->GetClass()->GetName());

				continue;
			}

			TArray<FString> Packages;
			CollectReferencedPackages(Pair.Value, Packages);

			/* The blueprint's own components are named by export index, which only means something to the cook */
			if (Packages.Contains(OwnPackage)) {
				UE_LOG(LogBlueprintDefaults, Display, TEXT("  %s.%s: refers to the blueprint's own subobjects, skipped"), *Target->GetName(), *Name);

				continue;
			}

			const FString* Missing = Packages.FindByPredicate([](const FString& Package) {
				return !Package.StartsWith(TEXT("/Script/")) && !FPackageName::DoesPackageExist(Package);
			});

			if (Missing != nullptr) {
				UE_LOG(LogBlueprintDefaults, Display, TEXT("  %s.%s: names %s, which is not in the project, skipped"), *Target->GetName(), *Name, **Missing);

				continue;
			}

			UE_LOG(LogBlueprintDefaults, Display, TEXT("  %s.%s"), *Target->GetName(), *Name);

			if (!bDryRun) {
				const TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();
				One->SetField(Name, Pair.Value);

				Container.GetObjectSerializer()->DeserializeObjectProperties(One, Target);
			}

			Applied++;
		}

		return Applied;
	}
}

int32 UBlueprintDefaultsCommandlet::Main(const FString& Params) {
	FString PathList, SubobjectList, ResetList, SkipList = TEXT("AttachParent,AutoAttachParent,UberGraphFrame");

	if (!FParse::Value(*Params, TEXT("path="), PathList, false) || PathList.IsEmpty()) {
		UE_LOG(LogBlueprintDefaults, Error, TEXT("-path=/Game/...[;/Game/...] is required"));

		return 1;
	}

	FParse::Value(*Params, TEXT("subobjects="), SubobjectList, false);
	FParse::Value(*Params, TEXT("reset="), ResetList, false);
	FParse::Value(*Params, TEXT("skip="), SkipList, false);

	const bool bDryRun = FParse::Param(*Params, TEXT("dryrun"));

	TArray<FString> Paths, Subobjects, Resets, SkipArray;
	PathList.ParseIntoArray(Paths, TEXT(";"));
	SubobjectList.ParseIntoArray(Subobjects, TEXT(","));
	ResetList.ParseIntoArray(Resets, TEXT(","));
	SkipList.ParseIntoArray(SkipArray, TEXT(","));

	const TSet<FString> Skip(SkipArray);

	/* Only what the blueprint itself says; the assets it names are left as they are */
	FAssetUtilities::bImportReferences = false;

	Cloud::EnsureMetadataBlocking();

	TArray<UPackage*> Touched;
	int32 Failed = 0;

	for (const FString& Path : Paths) {
		const FString AssetName = FPackageName::GetShortName(Path);

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *(Path + TEXT(".") + AssetName));

		if (Blueprint == nullptr || Blueprint->GeneratedClass == nullptr) {
			UE_LOG(LogBlueprintDefaults, Error, TEXT("no blueprint at \"%s\""), *Path);
			Failed++;

			continue;
		}

		const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(Path);

		if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
			UE_LOG(LogBlueprintDefaults, Error, TEXT("the cloud had nothing at \"%s\""), *Path);
			Failed++;

			continue;
		}

		const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));

		UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
		const FString DefaultName = DefaultObject->GetName();

		UE_LOG(LogBlueprintDefaults, Display, TEXT("%s"), *Path);

		/* Templates first, so a reset never undoes what the class defaults were just given */
		for (const FString& Subobject : Subobjects) {
			UObject* Target = DefaultObject->GetDefaultSubobjectByName(*Subobject);

			if (Target == nullptr) {
				UE_LOG(LogBlueprintDefaults, Warning, TEXT("  %s has no subobject %s"), *DefaultName, *Subobject);

				continue;
			}

			if (Resets.Contains(Subobject) && !bDryRun) {
				ResetToArchetype(Target);
			}

			const TSharedPtr<FJsonObject> Export = FindExport(Exports, Subobject, DefaultName);

			if (Export.IsValid() && Export->HasField(TEXT("Properties"))) {
				Apply(Target, Export->GetObjectField(TEXT("Properties")), Skip, Path, bDryRun);
			}

			Target->Modify();
		}

		const TSharedPtr<FJsonObject> DefaultExport = FindExport(Exports, DefaultName, FString());

		if (DefaultExport.IsValid() && DefaultExport->HasField(TEXT("Properties"))) {
			Apply(DefaultObject, DefaultExport->GetObjectField(TEXT("Properties")), Skip, Path, bDryRun);
		}

		DefaultObject->Modify();
		Blueprint->Modify();

		Touched.Add(Blueprint->GetOutermost());
	}

	if (bDryRun) {
		UE_LOG(LogBlueprintDefaults, Display, TEXT("dry run, nothing saved"));

		return Failed == 0 ? 0 : 1;
	}

	for (UPackage* Package : Touched) {
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
			UE_LOG(LogBlueprintDefaults, Display, TEXT("saved %s"), *FileName);
		} else {
			UE_LOG(LogBlueprintDefaults, Error, TEXT("could not save %s"), *FileName);
			Failed++;
		}
	}

	return Failed == 0 ? 0 : 1;
}
