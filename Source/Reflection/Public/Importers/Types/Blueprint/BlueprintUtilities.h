/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Settings/ReflectionSettings.h"
#include "Settings/Redirector.h"
#include "Settings/Runtime.h"
#include "Engine/EngineUtilities.h"
#include "Containers/ExportContainer.h"
#include "Importers/Constructor/Importer.h"
#include "Modules/Toolbar/Tools/ImportFromPath.h"
#include "UObject/CoreRedirects.h"
#include "Importers/Constructor/Asset.h"
#include "Modules/Cloud/Cloud.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "Engine/Log.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the blueprint function library type used to come in from */
#if UE4_25_BELOW
#include "Kismet/BlueprintFunctionLibrary.h"
#endif

/* What a write into a struct member names, said either way round.
 *
 * A build spells the member as a way down to it and the thing it is in as a property of a property;
 * an older one spells the member as a reference to the property itself and the thing it is in as a
 * plain name. Read one way only, the other comes back naming nothing, and a write that names
 * nothing matches nothing. */
/* The struct a property is on and what it is called on it, said either way round.
 *
 * A build says the two apart the way down to the member, and the struct resolved beside it. An
 * older one says them together, as one reference to the property itself: the struct before the
 * colon and the member after it. Read the first way only, the older spelling answers nothing for
 * both, and a member of nothing on a struct of nothing is not something that can be laid out. */
inline void ReadPropertyReference(const FUObjectJsonValueExport& Property, FString& Owner, FString& Member) {
	if (!Property.JsonObject.IsValid()) return;

	if (Member.IsEmpty()) {
		Property.JsonObject->TryGetStringField(TEXT("Name"), Member);
	}

	if (Member.IsEmpty()) {
		if (const TArray<TSharedPtr<FJsonValue>>* Path = nullptr; Property.JsonObject->TryGetArrayField(TEXT("Path"), Path) && Path->Num() > 0) {
			Member = (*Path)[Path->Num() - 1]->AsString();
		}
	}

	/* Or as the property written out in full beside what holds it: the member is the inner one's
	 * name, and the struct is its owner. A member of a struct a blueprint declared is said this way,
	 * and read any other way it names nothing. */
	if (const TSharedPtr<FJsonObject>* Inner = nullptr; Property.JsonObject->TryGetObjectField(TEXT("Property"), Inner)) {
		if (Member.IsEmpty()) {
			(*Inner)->TryGetStringField(TEXT("Name"), Member);
		}

		const TSharedPtr<FJsonObject>* Holder = nullptr;
		FString Spelled;

		if (Owner.IsEmpty() && Property.JsonObject->TryGetObjectField(TEXT("Owner"), Holder) && (*Holder)->TryGetStringField(TEXT("ObjectName"), Spelled)) {
			/* UserDefinedStruct'SomeStruct' -> SomeStruct */
			if (FString Held; Spelled.Split(TEXT("'"), nullptr, &Held)) {
				Held.RemoveFromEnd(TEXT("'"));

				Owner = Held;
			}
		}
	}

	if (!Owner.IsEmpty() && !Member.IsEmpty()) return;

	FString Spelled;

	if (!Property.JsonObject->TryGetStringField(TEXT("ObjectName"), Spelled)) return;

	/* FloatProperty'SomeStruct:Member' -> SomeStruct, Member */
	FString Held;

	if (!Spelled.Split(TEXT("'"), nullptr, &Held)) return;

	Held.RemoveFromEnd(TEXT("'"));

	FString Before, After;

	if (!Held.Split(TEXT(":"), &Before, &After)) return;

	if (Owner.IsEmpty()) Owner = Before;
	if (Member.IsEmpty()) Member = After;
}

inline void ReadStructMemberContext(const FUObjectJsonValueExport& Variable, FString& Owner, FString& Member) {
	Owner.Empty();
	Member.Empty();

	const FUObjectJsonValueExport Inside = Variable.GetObject(TEXT("StructExpression")).GetObject(TEXT("Variable"));

	if (Inside.JsonObject.IsValid() && !Inside.JsonObject->TryGetStringField(TEXT("Name"), Owner)) {
		if (const TSharedPtr<FJsonObject> Held = Inside.GetObject(TEXT("Property")).JsonObject; Held.IsValid()) {
			Held->TryGetStringField(TEXT("Name"), Owner);
		}
	}

	const TSharedPtr<FJsonObject> Named = Variable.GetObject(TEXT("Property")).JsonObject;

	if (!Named.IsValid()) return;

	if (const TArray<TSharedPtr<FJsonValue>>* Path = nullptr; Named->TryGetArrayField(TEXT("Path"), Path) && Path->Num() == 1) {
		Member = (*Path)[0]->AsString();

		return;
	}

	/* Spelled as a reference, of which the member is whatever comes after the class it is on */
	if (FString Spelled; Named->TryGetStringField(TEXT("ObjectName"), Spelled)) {
		Spelled.Split(TEXT(":"), nullptr, &Spelled);

		Member = Spelled.Replace(TEXT("'"), TEXT(""));

		return;
	}

	/* Or spelled out in full, which is how a member of a struct the blueprint itself made is said:
	 * an animation blueprint's own slots for the values it folds away are one of those, and a write
	 * into one of them names nothing else */
	if (const TSharedPtr<FJsonObject>* Held = nullptr; Named->TryGetObjectField(TEXT("Property"), Held)) {
		(*Held)->TryGetStringField(TEXT("Name"), Member);
	}
}

inline TSubclassOf<UObject> LoadClassFromPath(const FString& ObjectName, const FString& ObjectPath) {
	const FString FullPath = ObjectPath + TEXT(".") + ObjectName;

	if (UObject* LoadedObject = LoadObjectByPath<UObject>(FullPath)) {
		if (UClass* LoadedClass = Cast<UClass>(LoadedObject)) {
			return LoadedClass;
		}
	}

	/* A class this engine has renamed, which the class redirects say and an object load never looks at */
	const FCoreRedirectObjectName Named(*FullPath);
	const FCoreRedirectObjectName Now = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Class, Named);

	if (Now != Named) {
		if (UClass* Renamed = LoadObjectByPath<UClass>(Now.ToString())) {
			return Renamed;
		}
	}

	/* A native class is already in memory once its module is, whatever the path lookup made of
	 * the spelling it was given, so it is asked for by name before giving up */
	if (UClass* NamedClass = FindClassByType(ObjectName)) {
		if (NamedClass->IsNative()) return NamedClass;
	}

	if (UClass* Loaded = StaticLoadClass(UObject::StaticClass(), nullptr, *FullPath, nullptr, LOAD_NoWarn | LOAD_Quiet)) {
		return Loaded;
	}

	UE_LOG(LogReflection, Error, TEXT("no class answers to \"%s\" (asked for as \"%s\", then by name, then loaded)"), *FullPath, *ObjectName);

	return nullptr;
}

inline TSubclassOf<UObject> LoadBlueprintClass(FString& ObjectPath) {
	const FString& ProjectName = GReflectionRuntime.Profile.ProjectName;

	if (!ProjectName.IsEmpty()) {
		ObjectPath = ObjectPath.Replace(*(ProjectName + "/Content"), TEXT("/Game"));
	}
	
	FString FullPath = ObjectPath; 
	if (FullPath.EndsWith(TEXT(".1"))) {
		FullPath = FullPath.LeftChop(2);
	}

	if (UObject* LoadedObject = LoadObjectByPath<UObject>(FullPath)) {
		const UBlueprint* LoadedBlueprint = Cast<UBlueprint>(LoadedObject);
		
		if (LoadedBlueprint && LoadedBlueprint->GeneratedClass) {
			return LoadedBlueprint->GeneratedClass;
		}

		/* The class it generated, where the blueprint cannot say: answering nothing reads as an asset the project has not got */
		if (FString Leaf = FullPath; Leaf.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
			if (UClass* Generated = LoadObjectByPath<UClass>(FullPath + TEXT(".") + Leaf + TEXT("_C"))) {
				return Generated;
			}
		}
	}

	return nullptr;
}

inline UClass* LoadClass(const TSharedPtr<FJsonObject>& SuperStruct) {
	FString ObjectName = SuperStruct->GetStringField(TEXT("ObjectName")).Replace(TEXT("Class'"), TEXT("")).Replace(TEXT("'"), TEXT(""));
	FString ObjectPath = SuperStruct->GetStringField(TEXT("ObjectPath"));

	/* Redirected by the same rules an asset path is, since a class is named the same way */
	{
		FString Named = ObjectPath.Contains(TEXT(".")) ? ObjectPath : ObjectPath + TEXT(".") + ObjectName;

		FRRedirects::Redirect(Named);

		if (int32 Dot; Named.FindLastChar(TEXT('.'), Dot)) {
			ObjectPath = Named.Left(Dot);
			ObjectName = Named.RightChop(Dot + 1);

			/* A blueprint names its generated class, the asset with _C after it */
			if (!ObjectPath.Contains(TEXT("/Script/")) && !ObjectName.EndsWith(TEXT("_C"))) {
				ObjectPath = Named;
			}
		}
	}

	/* It's a C++ class if it has Script in it */
	if (ObjectPath.Contains("/Script/")) {
		return LoadClassFromPath(ObjectName, ObjectPath);
	}
	
	ObjectPath.Split(".", &ObjectPath, nullptr);

	if (UClass* Made = LoadBlueprintClass(ObjectPath)) {
		return Made;
	}

	/* The blueprint a class says it comes from, which the project has not got yet.
	 *
	 * A class read without finding its parent is left parented to nothing, and a blueprint in that
	 * state is one the editor can make nothing of: everything it inherited resolves to nothing and
	 * the asset opens broken. The parent is an asset like any other, so it is asked for, and the
	 * class it generates is the one this one comes from.
	 *
	 * It reaches back as far as it needs to. A parent that is itself a blueprint asks for its own
	 * parent as it is read, and the chain ends where it meets a class written in C++, which is
	 * already here and never asked for. */
	/* Only where the project has not got it: one that is here and did not answer is written over every time anything names it */

	if (!PackageHoldsAsset(ObjectPath) && TToolImportFromPath::Import(ObjectPath)) {
		return LoadBlueprintClass(ObjectPath);
	}

	return nullptr;
}

/* What a class or a struct says it comes from, under whichever name it is written as.
 *
 * A struct carries Next, a class made from C++ carries SuperStruct, and one made from another
 * blueprint carries Super. All three name the same thing, and a class read without finding it is
 * left parented to nothing at all, which is not something the compiler survives. */
inline TSharedPtr<FJsonObject> GetSuperStructJsonObject(const TSharedPtr<FJsonObject>& JsonObject) {
	if (!JsonObject.IsValid()) return nullptr;

	for (const TCHAR* Named : { TEXT("Next"), TEXT("SuperStruct"), TEXT("Super") }) {
		if (const TSharedPtr<FJsonObject>* Found = nullptr; JsonObject->TryGetObjectField(Named, Found)) {
			return *Found;
		}
	}

	return nullptr;
}

/* The kind of blueprint an asset was, as the asset itself says.
 *
 * A cooked class keeps what the editor knew it as in its tags, and that is the only place some of
 * them are said at all: an animation layer interface is a class like any other with an ordinary
 * parent, and read off the parent it comes back as a normal blueprint with its layers turned into
 * functions nobody can call. Where nothing says, the parent is still the best that can be done. */
inline EBlueprintType GetBlueprintTypeSaid(const TSharedPtr<FJsonObject>& AssetData, const UClass* Class);

inline EBlueprintType GetBlueprintType(const UClass* Class) {
	EBlueprintType BlueprintType = BPTYPE_Normal;

	if (Class->HasAnyClassFlags(CLASS_Const)) {
		BlueprintType = BPTYPE_Const;
	}
	
	if (Class == UBlueprintFunctionLibrary::StaticClass()) {
		BlueprintType = BPTYPE_FunctionLibrary;
	}
	
	if (Class == UInterface::StaticClass()) {
		BlueprintType = BPTYPE_Interface;
	}
	
	return BlueprintType;
}

inline EBlueprintType GetBlueprintTypeSaid(const TSharedPtr<FJsonObject>& AssetData, const UClass* Class) {
	const TSharedPtr<FJsonObject>* Tags = nullptr;

	if (AssetData.IsValid() && AssetData->TryGetObjectField(TEXT("EditorTags"), Tags)) {
		FString Says;

		if ((*Tags)->TryGetStringField(TEXT("BlueprintType"), Says) && !Says.IsEmpty()) {
			/* Spelled the way the engine spells it, so the engine is asked what it means */
			if (const UEnum* Kinds = StaticEnum<EBlueprintType>()) {
				const int64 Which = Kinds->GetValueByNameString(Says);

				if (Which != INDEX_NONE) return static_cast<EBlueprintType>(Which);
			}
		}
	}

	return GetBlueprintType(Class);
}

inline FUObjectExport* GetClassDefaultObject(FUObjectExportContainer* AssetContainer, const FUObjectJsonValueExport& JsonObject) {
	FUObjectExport* Export = AssetContainer->GetExportByObjectPath(JsonObject.GetObject("ClassDefaultObject"));
	if (!Export->IsJsonValid()) {
		Export = AssetContainer->GetExportStartingWith("Name", "Default__");
	}

	return Export;
}

/* Classes and structs cannot live under an object whose class is reinstanced.
 *
 * Compiling a blueprint reinstances its generated class's CDO. That reinstancer copies every
 * subobject the old CDO owned under the new one (FObjectReinstancer::SaveClassData,
 * DuplicateObject, StaticAllocateObject). StaticAllocateObject will not make a UStruct or a
 * UClass under anything that is not a package, so the moment a CDO owns a struct or a class
 * DuplicateObject asserts:
 *
 *     "Objects of type 'UserDefinedStruct' may only be created in packages"
 *
 * The things that trigger it:
 *
 *   - A component whose outer is the CDO and which owns an anonymous class or a struct
 *   - A deserialized UObject whose property value was a UUserDefinedStruct* and which got
 *     instanced under the CDO rather than referenced in its package
 *   - Any child object created with the CDO as its outer that is, or contains, a struct
 *
 * Before every compile of a blueprint that has had its CDO populated, we walk every object
 * whose outer is that CDO. Anything that is a UStruct is renamed into the transient package,
 * severed from the CDO's properties, and marked for garbage collection. */
inline int32 EvictStructsFromDefaults(UBlueprint* Blueprint, const TCHAR* Stage) {
	if (Blueprint == nullptr || Blueprint->GeneratedClass == nullptr) return 0;

	UObject* Defaults = Blueprint->GeneratedClass->GetDefaultObject(false);

	if (Defaults == nullptr) return 0;

	UE_LOG(LogReflection, Verbose, TEXT("%s: blueprint \"%s\" (class %s, outer %s, super %s, skel %s): defaults are at \"%s\" (a %s%s)"),
		Stage,
		*Blueprint->GetName(),
		*Blueprint->GeneratedClass->GetClass()->GetName(),
		*GetNameSafe(Blueprint->GeneratedClass->GetOuter()),
		*GetPathNameSafe(Blueprint->GeneratedClass->GetSuperClass()),
		*GetPathNameSafe(Blueprint->SkeletonGeneratedClass),
		*Defaults->GetPathName(),
		*Defaults->GetClass()->GetPathName(),
		Defaults->IsA<UStruct>() ? TEXT(" - WHICH IS A STRUCT") : TEXT(""));

	/* Every class kept anywhere but in a package, since a class under an object is the one thing
	 * reinstancing cannot copy */
	for (TObjectIterator<UClass> It; It; ++It) {
		UClass* Class = *It;

		if (Class == nullptr || Class->GetOuter() == nullptr || Class->GetOuter()->IsA<UPackage>()) continue;

		UE_LOG(LogReflection, Warning, TEXT("%s: class \"%s\" (a %s, size %d) is kept under \"%s\" (a %s)"),
			Stage,
			*Class->GetName(),
			*Class->GetClass()->GetName(),
			Class->GetStructureSize(),
			*Class->GetOuter()->GetPathName(),
			*Class->GetOuter()->GetClass()->GetName());
	}

	TArray<UObject*> Kept;
	GetObjectsWithOuter(Defaults, Kept, true);

	int32 Evicted = 0;

	for (UObject* One : Kept) {
		if (One == nullptr) continue;

		/* Every referrer among the defaults' own properties, so the log says which one it came in through */
		FString Through;

		for (TFieldIterator<FObjectPropertyBase> It(Defaults->GetClass()); It; ++It) {
			for (int32 Index = 0; Index < It->ArrayDim; ++Index) {
				if (It->GetObjectPropertyValue_InContainer(Defaults, Index) == One) {
					Through += (Through.IsEmpty() ? TEXT("") : TEXT(", ")) + It->GetName();
				}
			}
		}

		const bool bStruct = One->IsA<UStruct>();

		const FString Said = FString::Printf(TEXT("%s: the defaults of \"%s\" keep \"%s\" (a %s%s%s%s)"),
			Stage,
			*Blueprint->GetName(),
			*One->GetPathName(Defaults),
			*One->GetClass()->GetName(),
			One->IsDefaultSubobject() ? TEXT(", default subobject") : TEXT(""),
			Through.IsEmpty() ? TEXT("") : TEXT(", through "),
			*Through);

		if (bStruct) {
			UE_LOG(LogReflection, Warning, TEXT("%s"), *Said);
		} else {
			UE_LOG(LogReflection, Display, TEXT("%s"), *Said);
		}

		if (!bStruct) continue;

		UE_LOG(LogReflection, Warning, TEXT("%s: \"%s\" cannot be reinstanced under the defaults, so it was moved aside"), Stage, *One->GetName());

		/* Every reference from the defaults is cleared first, so the reinstancer does not find it
		 * through a property and go looking for it where it no longer is */
		TMap<UObject*, UObject*> Cleared;
		Cleared.Add(One, nullptr);

		FArchiveReplaceObjectRef<UObject> Clear(Defaults, Cleared, EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);

		One->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional | REN_ForceNoResetLoaders);
		One->ClearFlags(RF_Public | RF_Standalone);
		One->SetFlags(RF_Transient);
		One->MarkAsGarbage();

		Evicted++;
	}

	return Evicted;
}

/* A blueprint's class, loaded from the package the project keeps it in.
 *
 * Given where the export says the asset is ("/Game/Path/Asset.0" or "/Game/Path/Asset.Asset_C")
 * and what its class is called, loads the package if it is on disk and hands back the generated
 * class. Nothing is fetched: an asset that is not in the project is somebody else's to bring in. */
inline UClass* LoadBlueprintClassFromDisk(FString Where, const FString& ClassName) {
	/* An export is named by its package and the number it sits at, and the number is no part of
	 * where the editor keeps it */
	if (int32 Sits; Where.FindLastChar(TEXT('.'), Sits)) {
		const FString After = Where.RightChop(Sits + 1);

		if (!After.IsEmpty() && (After.IsNumeric() || After.EndsWith(TEXT("_C")))) Where.LeftInline(Sits);
	}

	if (Where.IsEmpty() || !FPackageName::IsValidLongPackageName(Where) || !FPackageName::DoesPackageExist(Where)) {
		return nullptr;
	}

	FString Leaf = Where;

	if (int32 Slash; Where.FindLastChar(TEXT('/'), Slash)) Leaf = Where.RightChop(Slash + 1);

	if (const UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *(Where + TEXT(".") + Leaf))) {
		if (Blueprint->GeneratedClass != nullptr) return Blueprint->GeneratedClass.Get();
	}

	FString Called = ClassName;

	if (!Called.EndsWith(TEXT("_C"))) Called += TEXT("_C");

	return LoadObject<UClass>(nullptr, *(Where + TEXT(".") + Called));
}
