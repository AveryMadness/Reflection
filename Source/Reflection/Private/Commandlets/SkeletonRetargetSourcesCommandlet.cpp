/* Copyright Reflection Contributors 2024-2026 */

#include "SkeletonRetargetSourcesCommandlet.h"

#include "Animation/Skeleton.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

DECLARE_LOG_CATEGORY_CLASS(LogSkeletonRetargetSources, All, All);

namespace {
	/* The Cloud writes a transform as Rotation/Translation/Scale3D objects; read straight through so the
	 * commandlet does not depend on the importer's property serializer */
	bool ReadVector(const TSharedPtr<FJsonObject>& Object, const FString& Field, FVector& OutVector) {
		const TSharedPtr<FJsonObject>* Inner = nullptr;
		if (!Object->TryGetObjectField(Field, Inner) || !Inner) {
			return false;
		}

		return (*Inner)->TryGetNumberField(TEXT("X"), OutVector.X)
			&& (*Inner)->TryGetNumberField(TEXT("Y"), OutVector.Y)
			&& (*Inner)->TryGetNumberField(TEXT("Z"), OutVector.Z);
	}

	bool ReadTransform(const TSharedPtr<FJsonObject>& Object, FTransform& OutTransform) {
		const TSharedPtr<FJsonObject>* RotationObject = nullptr;
		if (!Object->TryGetObjectField(TEXT("Rotation"), RotationObject) || !RotationObject) {
			return false;
		}

		FQuat Rotation;
		if (!(*RotationObject)->TryGetNumberField(TEXT("X"), Rotation.X)
			|| !(*RotationObject)->TryGetNumberField(TEXT("Y"), Rotation.Y)
			|| !(*RotationObject)->TryGetNumberField(TEXT("Z"), Rotation.Z)
			|| !(*RotationObject)->TryGetNumberField(TEXT("W"), Rotation.W)) {
			return false;
		}

		FVector Translation;
		if (!ReadVector(Object, TEXT("Translation"), Translation)) {
			return false;
		}

		FVector Scale(1.0, 1.0, 1.0);
		ReadVector(Object, TEXT("Scale3D"), Scale);

		Rotation.Normalize();
		OutTransform = FTransform(Rotation, Translation, Scale);

		return true;
	}
}

int32 USkeletonRetargetSourcesCommandlet::Main(const FString& Params) {
	FString SkeletonPath;
	FString JsonPath;

	if (!FParse::Value(*Params, TEXT("skeleton="), SkeletonPath) || SkeletonPath.IsEmpty()
		|| !FParse::Value(*Params, TEXT("json="), JsonPath) || JsonPath.IsEmpty()) {
		UE_LOG(LogSkeletonRetargetSources, Error, TEXT("need -skeleton=/Game/... and -json=<file>"));

		return 1;
	}

	const bool bDryRun = FParse::Param(*Params, TEXT("dryrun"));

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *JsonPath)) {
		UE_LOG(LogSkeletonRetargetSources, Error, TEXT("could not read %s"), *JsonPath);

		return 1;
	}

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Root) || !Root) {
		UE_LOG(LogSkeletonRetargetSources, Error, TEXT("%s is not json"), *JsonPath);

		return 1;
	}

	/* The Cloud answers with every export in the package; the skeleton is the one carrying the map */
	const TSharedPtr<FJsonObject>* Sources = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Exports = nullptr;

	if (Root->TryGetArrayField(TEXT("exports"), Exports)) {
		for (const TSharedPtr<FJsonValue>& Value : *Exports) {
			const TSharedPtr<FJsonObject> Export = Value->AsObject();
			if (Export && Export->TryGetObjectField(TEXT("AnimRetargetSources"), Sources) && Sources) {
				break;
			}

			Sources = nullptr;
		}
	} else {
		Root->TryGetObjectField(TEXT("AnimRetargetSources"), Sources);
	}

	if (!Sources) {
		UE_LOG(LogSkeletonRetargetSources, Error, TEXT("no AnimRetargetSources in %s"), *JsonPath);

		return 1;
	}

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton) {
		UE_LOG(LogSkeletonRetargetSources, Error, TEXT("could not load skeleton %s"), *SkeletonPath);

		return 1;
	}

	const int32 SkeletonBones = Skeleton->GetReferenceSkeleton().GetRawBoneNum();

	UE_LOG(LogSkeletonRetargetSources, Display, TEXT("%s: %d raw bone(s), %d retarget source(s) before"),
		*Skeleton->GetName(), SkeletonBones, Skeleton->AnimRetargetSources.Num());

	{
		TArray<FName> Existing;
		Skeleton->AnimRetargetSources.GetKeys(Existing);
		Existing.Sort(FNameLexicalLess());

		for (const FName& Name : Existing) {
			UE_LOG(LogSkeletonRetargetSources, Display, TEXT("   have: %s (%d bone(s))"),
				*Name.ToString(), Skeleton->AnimRetargetSources[Name].ReferencePose.Num());
		}
	}

	int32 Added = 0;
	int32 Skipped = 0;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Sources)->Values) {
		const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* PoseArray = nullptr;

		if (!Entry || !Entry->TryGetArrayField(TEXT("ReferencePose"), PoseArray) || !PoseArray) {
			UE_LOG(LogSkeletonRetargetSources, Warning, TEXT("   %s has no ReferencePose, skipped"), *Pair.Key);
			Skipped++;

			continue;
		}

		FReferencePose Pose;
		Pose.PoseName = FName(*Entry->GetStringField(TEXT("PoseName")));
		if (Pose.PoseName.IsNone()) {
			Pose.PoseName = FName(*Pair.Key);
		}

		Pose.ReferencePose.Reserve(PoseArray->Num());

		bool bReadAll = true;
		for (const TSharedPtr<FJsonValue>& TransformValue : *PoseArray) {
			FTransform Transform;
			if (const TSharedPtr<FJsonObject> TransformObject = TransformValue->AsObject(); !TransformObject || !ReadTransform(TransformObject, Transform)) {
				bReadAll = false;

				break;
			}

			Pose.ReferencePose.Add(Transform);
		}

		if (!bReadAll) {
			UE_LOG(LogSkeletonRetargetSources, Warning, TEXT("   %s has a transform that would not read, skipped"), *Pair.Key);
			Skipped++;

			continue;
		}

		/* A pose shorter than the skeleton would be read past by the retargeting, and one that is longer
		 * is the shipped skeleton carrying bones this import does not have - the extra tail is unused */
		if (Pose.ReferencePose.Num() < SkeletonBones) {
			UE_LOG(LogSkeletonRetargetSources, Warning, TEXT("   %s has %d bone(s) for a %d bone skeleton, skipped"),
				*Pair.Key, Pose.ReferencePose.Num(), SkeletonBones);
			Skipped++;

			continue;
		}

		UE_LOG(LogSkeletonRetargetSources, Display, TEXT("   %s: %d bone(s)"), *Pair.Key, Pose.ReferencePose.Num());

		Skeleton->AnimRetargetSources.Add(FName(*Pair.Key), Pose);
		Added++;
	}

	UE_LOG(LogSkeletonRetargetSources, Display, TEXT("%d added, %d skipped"), Added, Skipped);

	if (bDryRun || Added == 0) {
		return 0;
	}

	UPackage* Package = Skeleton->GetOutermost();
	Package->MarkPackageDirty();

	const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
		UE_LOG(LogSkeletonRetargetSources, Error, TEXT("could not save %s"), *FileName);

		return 1;
	}

	UE_LOG(LogSkeletonRetargetSources, Display, TEXT("saved %s"), *FileName);

	return 0;
}
