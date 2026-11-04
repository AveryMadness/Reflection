/* Copyright Reflection Contributors 2024-2026 */

#include "SkeletonDumpCommandlet.h"

#include "Animation/BlendProfile.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

DECLARE_LOG_CATEGORY_CLASS(LogSkeletonDump, All, All);

namespace {
	TSharedPtr<FJsonObject> TransformAsJson(const FTransform& Transform) {
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

		const FVector Translation = Transform.GetTranslation();
		const FQuat Rotation = Transform.GetRotation();
		const FVector Scale = Transform.GetScale3D();

		Out->SetNumberField(TEXT("tx"), Translation.X);
		Out->SetNumberField(TEXT("ty"), Translation.Y);
		Out->SetNumberField(TEXT("tz"), Translation.Z);
		Out->SetNumberField(TEXT("rx"), Rotation.X);
		Out->SetNumberField(TEXT("ry"), Rotation.Y);
		Out->SetNumberField(TEXT("rz"), Rotation.Z);
		Out->SetNumberField(TEXT("rw"), Rotation.W);
		Out->SetNumberField(TEXT("sx"), Scale.X);
		Out->SetNumberField(TEXT("sy"), Scale.Y);
		Out->SetNumberField(TEXT("sz"), Scale.Z);

		return Out;
	}

	void BonesAsJson(const USkeleton* Skeleton, const TSharedPtr<FJsonObject>& Root) {
		const FReferenceSkeleton& Reference = Skeleton->GetReferenceSkeleton();

		const TArray<FMeshBoneInfo>& Info = Reference.GetRefBoneInfo();
		const TArray<FTransform>& Pose = Reference.GetRefBonePose();

		const TArray<FMeshBoneInfo>& RawInfo = Reference.GetRawRefBoneInfo();

		Root->SetNumberField(TEXT("numBones"), Info.Num());
		Root->SetNumberField(TEXT("numRawBones"), RawInfo.Num());

		const UEnum* Retargeting = StaticEnum<EBoneTranslationRetargetingMode::Type>();

		TArray<TSharedPtr<FJsonValue>> Bones;

		for (int32 At = 0; At < Info.Num(); ++At) {
			TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

			One->SetNumberField(TEXT("index"), At);
			One->SetStringField(TEXT("bone"), Info[At].Name.ToString());
			One->SetNumberField(TEXT("parent"), Info[At].ParentIndex);
			One->SetStringField(TEXT("parentName"), Info.IsValidIndex(Info[At].ParentIndex) ? Info[Info[At].ParentIndex].Name.ToString() : FString());

			/* How the animation's translation for this bone is applied. A bone set to Animation where
			 * the game has Skeleton plays the translation raw instead of the skeleton's fixed one,
			 * which is what makes a limb stretch. */
			const int64 Mode = static_cast<int64>(Skeleton->GetBoneTranslationRetargetingMode(At));
			One->SetStringField(TEXT("retargeting"), Retargeting != nullptr ? Retargeting->GetNameStringByValue(Mode) : FString::FromInt(static_cast<int32>(Mode)));

			if (Pose.IsValidIndex(At)) {
				One->SetObjectField(TEXT("refPose"), TransformAsJson(Pose[At]));
			}

			Bones.Add(MakeShared<FJsonValueObject>(One));
		}

		Root->SetArrayField(TEXT("bones"), Bones);
	}
}

int32 USkeletonDumpCommandlet::Main(const FString& Params) {
	FString Path, OutFile;

	if (!FParse::Value(*Params, TEXT("path="), Path) || Path.IsEmpty()) {
		UE_LOG(LogSkeletonDump, Error, TEXT("-path=/Game/... is required"));

		return 1;
	}

	/* A skeletal mesh carries its own reference skeleton, and that is the pose OrientAndScale
	 * retargeting scales an animation's translation up to - so it is the one that decides where a
	 * bone ends up, not the skeleton asset's */
	if (FString MeshPath; FParse::Value(*Params, TEXT("mesh="), MeshPath) && !MeshPath.IsEmpty()) {
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);

		if (Mesh == nullptr) {
			UE_LOG(LogSkeletonDump, Error, TEXT("no skeletal mesh at \"%s\""), *MeshPath);

			return 1;
		}

		if (!FParse::Value(*Params, TEXT("out="), OutFile) || OutFile.IsEmpty()) {
			OutFile = FPaths::ProjectSavedDir() / (Mesh->GetName() + TEXT(".json"));
		}

		TSharedPtr<FJsonObject> MeshRoot = MakeShared<FJsonObject>();

		MeshRoot->SetStringField(TEXT("asset"), Mesh->GetName());
		MeshRoot->SetStringField(TEXT("package"), Mesh->GetOutermost()->GetName());
		MeshRoot->SetStringField(TEXT("skeleton"), Mesh->GetSkeleton() != nullptr ? Mesh->GetSkeleton()->GetPathName() : FString());

		const FReferenceSkeleton& MeshReference = Mesh->GetRefSkeleton();
		const TArray<FMeshBoneInfo>& MeshInfo = MeshReference.GetRefBoneInfo();
		const TArray<FTransform>& MeshPose = MeshReference.GetRefBonePose();

		MeshRoot->SetNumberField(TEXT("numBones"), MeshInfo.Num());

		TArray<TSharedPtr<FJsonValue>> MeshBones;

		for (int32 At = 0; At < MeshInfo.Num(); ++At) {
			TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

			One->SetNumberField(TEXT("index"), At);
			One->SetStringField(TEXT("bone"), MeshInfo[At].Name.ToString());
			One->SetNumberField(TEXT("parent"), MeshInfo[At].ParentIndex);

			if (MeshPose.IsValidIndex(At)) {
				One->SetObjectField(TEXT("refPose"), TransformAsJson(MeshPose[At]));
			}

			MeshBones.Add(MakeShared<FJsonValueObject>(One));
		}

		MeshRoot->SetArrayField(TEXT("bones"), MeshBones);

		FString MeshText;
		const TSharedRef<TJsonWriter<>> MeshWriter = TJsonWriterFactory<>::Create(&MeshText);
		FJsonSerializer::Serialize(MeshRoot.ToSharedRef(), MeshWriter);

		if (!FFileHelper::SaveStringToFile(MeshText, *OutFile)) {
			UE_LOG(LogSkeletonDump, Error, TEXT("could not write \"%s\""), *OutFile);

			return 1;
		}

		UE_LOG(LogSkeletonDump, Display, TEXT("wrote \"%s\" (%d mesh bone(s))"), *OutFile, MeshInfo.Num());

		return 0;
	}

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *Path);

	if (Skeleton == nullptr) {
		UE_LOG(LogSkeletonDump, Error, TEXT("no skeleton at \"%s\""), *Path);

		return 1;
	}

	if (!FParse::Value(*Params, TEXT("out="), OutFile) || OutFile.IsEmpty()) {
		OutFile = FPaths::ProjectSavedDir() / (Skeleton->GetName() + TEXT(".json"));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("asset"), Skeleton->GetName());
	Root->SetStringField(TEXT("package"), Skeleton->GetOutermost()->GetName());
	Root->SetStringField(TEXT("virtualBoneGuid"), Skeleton->GetVirtualBoneGuid().ToString());

	BonesAsJson(Skeleton, Root);

	TArray<TSharedPtr<FJsonValue>> Virtuals;

	for (const FVirtualBone& Bone : Skeleton->GetVirtualBones()) {
		TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

		One->SetStringField(TEXT("virtualBoneName"), Bone.VirtualBoneName.ToString());
		One->SetStringField(TEXT("sourceBoneName"), Bone.SourceBoneName.ToString());
		One->SetStringField(TEXT("targetBoneName"), Bone.TargetBoneName.ToString());

		Virtuals.Add(MakeShared<FJsonValueObject>(One));
	}

	Root->SetArrayField(TEXT("virtualBones"), Virtuals);

	/* The per-bone blend scales the transitions name. An empty profile is not an inert one: a
	 * transition that asks for it blends every bone at the main weight instead. */
	TArray<TSharedPtr<FJsonValue>> Profiles;

	for (const UBlendProfile* Profile : Skeleton->BlendProfiles) {
		if (Profile == nullptr) continue;

		TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

		One->SetStringField(TEXT("name"), Profile->GetName());
		One->SetStringField(TEXT("mode"), StaticEnum<EBlendProfileMode>() != nullptr ? StaticEnum<EBlendProfileMode>()->GetNameStringByValue(static_cast<int64>(Profile->GetMode())) : FString());
		One->SetNumberField(TEXT("numEntries"), Profile->GetNumBlendEntries());

		TArray<TSharedPtr<FJsonValue>> Entries;

		for (int32 At = 0; At < Profile->GetNumBlendEntries(); ++At) {
			const FBlendProfileBoneEntry& Entry = Profile->GetEntry(At);

			TSharedPtr<FJsonObject> One_Entry = MakeShared<FJsonObject>();

			One_Entry->SetStringField(TEXT("bone"), Entry.BoneReference.BoneName.ToString());
			One_Entry->SetNumberField(TEXT("blendScale"), Entry.BlendScale);

			Entries.Add(MakeShared<FJsonValueObject>(One_Entry));
		}

		One->SetArrayField(TEXT("entries"), Entries);
		Profiles.Add(MakeShared<FJsonValueObject>(One));
	}

	Root->SetArrayField(TEXT("blendProfiles"), Profiles);

	TArray<TSharedPtr<FJsonValue>> Slots;

	for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups()) {
		TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

		One->SetStringField(TEXT("groupName"), Group.GroupName.ToString());

		TArray<TSharedPtr<FJsonValue>> Names;

		for (const FName& Name : Group.SlotNames) {
			Names.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}

		One->SetArrayField(TEXT("slotNames"), Names);
		Slots.Add(MakeShared<FJsonValueObject>(One));
	}

	Root->SetArrayField(TEXT("slotGroups"), Slots);

	TArray<TSharedPtr<FJsonValue>> Sockets;

	for (const USkeletalMeshSocket* Socket : Skeleton->Sockets) {
		if (Socket == nullptr) continue;

		TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

		One->SetStringField(TEXT("socketName"), Socket->SocketName.ToString());
		One->SetStringField(TEXT("boneName"), Socket->BoneName.ToString());
		One->SetObjectField(TEXT("relative"), TransformAsJson(FTransform(Socket->RelativeRotation, Socket->RelativeLocation, Socket->RelativeScale)));

		Sockets.Add(MakeShared<FJsonValueObject>(One));
	}

	Root->SetArrayField(TEXT("sockets"), Sockets);

	/* Each retarget source is a whole pose, and an animation names the one it was authored against */
	TSharedPtr<FJsonObject> Retargets = MakeShared<FJsonObject>();

	for (const TPair<FName, FReferencePose>& Source : Skeleton->AnimRetargetSources) {
		TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

		One->SetNumberField(TEXT("numBonePoses"), Source.Value.ReferencePose.Num());

		/* The translation length per bone is what OrientAndScale retargeting divides by, so it is the
		 * part of the pose that decides how far a bone is moved */
		TArray<TSharedPtr<FJsonValue>> Pose;

		for (const FTransform& Bone : Source.Value.ReferencePose) {
			Pose.Add(MakeShared<FJsonValueNumber>(Bone.GetTranslation().Size()));
		}

		One->SetArrayField(TEXT("translationLengths"), Pose);

		Retargets->SetObjectField(Source.Key.ToString(), One);
	}

	Root->SetObjectField(TEXT("animRetargetSources"), Retargets);

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	if (!FFileHelper::SaveStringToFile(Text, *OutFile)) {
		UE_LOG(LogSkeletonDump, Error, TEXT("could not write \"%s\""), *OutFile);

		return 1;
	}

	UE_LOG(LogSkeletonDump, Display, TEXT("wrote \"%s\" (%d bone(s), %d virtual, %d blend profile(s), %d retarget source(s))"),
		*OutFile, Skeleton->GetReferenceSkeleton().GetRefBoneInfo().Num(), Skeleton->GetVirtualBones().Num(),
		Skeleton->BlendProfiles.Num(), Skeleton->AnimRetargetSources.Num());

	return 0;
}
