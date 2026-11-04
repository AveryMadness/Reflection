/* Copyright Reflection Contributors 2024-2026 */

#include "AnimSeqDumpCommandlet.h"

#include "Engine/Compatibility.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif

#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

DECLARE_LOG_CATEGORY_CLASS(LogAnimSeqDump, All, All);

namespace {
	FString EnumName(const UEnum* Enum, const int64 Value) {
		return Enum != nullptr ? Enum->GetNameStringByValue(Value) : FString::FromInt(Value);
	}

	FString RateAsString(const FFrameRate& Rate) {
		return FString::Printf(TEXT("%d/%d"), Rate.Numerator, Rate.Denominator);
	}

	/* Flat float arrays rather than objects per key: the Cloud sends the same three channels the same
	 * way, and a per-key object triples the file for a set this size */
	void KeysAsJson(const TArray<FTransform>& Keys, const TSharedPtr<FJsonObject>& Out) {
		TArray<TSharedPtr<FJsonValue>> Positions, Rotations, Scales;

		for (const FTransform& Key : Keys) {
			const FVector Position = Key.GetTranslation();
			const FQuat Rotation = Key.GetRotation();
			const FVector Scale = Key.GetScale3D();

			Positions.Add(MakeShared<FJsonValueNumber>(Position.X));
			Positions.Add(MakeShared<FJsonValueNumber>(Position.Y));
			Positions.Add(MakeShared<FJsonValueNumber>(Position.Z));

			Rotations.Add(MakeShared<FJsonValueNumber>(Rotation.X));
			Rotations.Add(MakeShared<FJsonValueNumber>(Rotation.Y));
			Rotations.Add(MakeShared<FJsonValueNumber>(Rotation.Z));
			Rotations.Add(MakeShared<FJsonValueNumber>(Rotation.W));

			Scales.Add(MakeShared<FJsonValueNumber>(Scale.X));
			Scales.Add(MakeShared<FJsonValueNumber>(Scale.Y));
			Scales.Add(MakeShared<FJsonValueNumber>(Scale.Z));
		}

		Out->SetArrayField(TEXT("positions"), Positions);
		Out->SetArrayField(TEXT("rotations"), Rotations);
		Out->SetArrayField(TEXT("scales"), Scales);
	}

	TSharedPtr<FJsonObject> SequenceAsJson(const UAnimSequence* Sequence, const bool bKeys) {
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

		Out->SetStringField(TEXT("asset"), Sequence->GetName());
		Out->SetStringField(TEXT("package"), Sequence->GetOutermost()->GetName());

		Out->SetNumberField(TEXT("sequenceLength"), Sequence->GetPlayLength());
		Out->SetNumberField(TEXT("numberOfSampledKeys"), Sequence->GetNumberOfSampledKeys());
		Out->SetStringField(TEXT("samplingFrameRate"), RateAsString(Sequence->GetSamplingFrameRate()));
		Out->SetNumberField(TEXT("rateScale"), Sequence->RateScale);
		Out->SetStringField(TEXT("interpolation"), EnumName(StaticEnum<EAnimInterpolationType>(), static_cast<int64>(Sequence->Interpolation)));

		Out->SetStringField(TEXT("additiveType"), EnumName(StaticEnum<EAdditiveAnimationType>(), static_cast<int64>(Sequence->AdditiveAnimType.GetValue())));
		Out->SetStringField(TEXT("refPoseType"), EnumName(StaticEnum<EAdditiveBasePoseType>(), static_cast<int64>(Sequence->RefPoseType.GetValue())));
		Out->SetNumberField(TEXT("refFrameIndex"), Sequence->RefFrameIndex);
		Out->SetStringField(TEXT("refPoseSequence"), Sequence->RefPoseSeq != nullptr ? Sequence->RefPoseSeq->GetPathName() : FString());

		Out->SetStringField(TEXT("retargetSource"), Sequence->RetargetSource.ToString());
		Out->SetStringField(TEXT("skeleton"), Sequence->GetSkeleton() != nullptr ? Sequence->GetSkeleton()->GetPathName() : FString());

		Out->SetBoolField(TEXT("bEnableRootMotion"), Sequence->bEnableRootMotion);
		Out->SetStringField(TEXT("rootMotionRootLock"), EnumName(StaticEnum<ERootMotionRootLock::Type>(), static_cast<int64>(Sequence->RootMotionRootLock.GetValue())));
		Out->SetBoolField(TEXT("bForceRootLock"), Sequence->bForceRootLock);
		Out->SetBoolField(TEXT("bUseNormalizedRootMotionScale"), Sequence->bUseNormalizedRootMotionScale);

		Out->SetStringField(TEXT("boneCompressionSettings"), Sequence->BoneCompressionSettings != nullptr ? Sequence->BoneCompressionSettings->GetPathName() : FString());
		Out->SetStringField(TEXT("curveCompressionSettings"), Sequence->CurveCompressionSettings != nullptr ? Sequence->CurveCompressionSettings->GetPathName() : FString());

		/* Curves off the data model, not the compressed copy: the model is what the editor edits and
		 * what a re-import writes, and a compressed curve is only rebuilt on a recompress */
		const IAnimationDataModel* Model = Sequence->GetDataModel();

		if (Model != nullptr) {
			Out->SetNumberField(TEXT("modelPlayLength"), Model->GetPlayLength());
			Out->SetNumberField(TEXT("modelNumberOfFrames"), Model->GetNumberOfFrames());
			Out->SetNumberField(TEXT("modelNumberOfKeys"), Model->GetNumberOfKeys());
			Out->SetStringField(TEXT("modelFrameRate"), RateAsString(Model->GetFrameRate()));
		}

		TArray<TSharedPtr<FJsonValue>> Curves;

		for (const FFloatCurve& Curve : Sequence->GetCurveData().FloatCurves) {
			TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

			One->SetStringField(TEXT("name"), Curve.GetName().ToString());

			TArray<TSharedPtr<FJsonValue>> Keys;

			for (const FRichCurveKey& Key : Curve.FloatCurve.GetConstRefOfKeys()) {
				TSharedPtr<FJsonObject> One_Key = MakeShared<FJsonObject>();

				One_Key->SetNumberField(TEXT("t"), Key.Time);
				One_Key->SetNumberField(TEXT("v"), Key.Value);

				Keys.Add(MakeShared<FJsonValueObject>(One_Key));
			}

			One->SetArrayField(TEXT("keys"), Keys);
			Curves.Add(MakeShared<FJsonValueObject>(One));
		}

		Out->SetArrayField(TEXT("curves"), Curves);

		TArray<TSharedPtr<FJsonValue>> Markers;

		for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers) {
			TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

			One->SetStringField(TEXT("name"), Marker.MarkerName.ToString());
			One->SetNumberField(TEXT("time"), Marker.Time);

			Markers.Add(MakeShared<FJsonValueObject>(One));
		}

		Out->SetArrayField(TEXT("syncMarkers"), Markers);

		TArray<TSharedPtr<FJsonValue>> Notifies;

		for (const FAnimNotifyEvent& Notify : Sequence->Notifies) {
			TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

			One->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
			One->SetNumberField(TEXT("time"), Notify.GetTriggerTime());
			One->SetNumberField(TEXT("duration"), Notify.GetDuration());
			One->SetStringField(TEXT("notify"), Notify.Notify != nullptr ? Notify.Notify->GetClass()->GetName() : FString());
			One->SetStringField(TEXT("state"), Notify.NotifyStateClass != nullptr ? Notify.NotifyStateClass->GetClass()->GetName() : FString());
			One->SetStringField(TEXT("montageTickType"), Notify.MontageTickType == EMontageNotifyTickType::Queued ? TEXT("Queued") : TEXT("BranchingPoint"));

			Notifies.Add(MakeShared<FJsonValueObject>(One));
		}

		Out->SetArrayField(TEXT("notifies"), Notifies);

		TArray<TSharedPtr<FJsonValue>> Tracks;

		if (Model != nullptr) {
			const FReferenceSkeleton* Reference = Sequence->GetSkeleton() != nullptr ? &Sequence->GetSkeleton()->GetReferenceSkeleton() : nullptr;

			TArray<FName> Names;
			Model->GetBoneTrackNames(Names);

			for (const FName& Name : Names) {
				TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

				/* One transform a key for every channel, which is the shape the Cloud answers in once
				 * it has decompressed; the raw model stores the channels at full length anyway */
				TArray<FTransform> Keys;
				Model->GetBoneTrackTransforms(Name, Keys);

				One->SetStringField(TEXT("bone"), Name.ToString());
				One->SetNumberField(TEXT("boneIndex"), Reference != nullptr ? Reference->FindBoneIndex(Name) : INDEX_NONE);
				One->SetNumberField(TEXT("numKeys"), Keys.Num());

				/* The extent of the track's translation. A bone whose retargeting mode is Animation is
				 * placed at exactly this, so a value far from the reference pose is a bone that will
				 * leave the body wherever the sequence is played. */
				double Shortest = TNumericLimits<double>::Max();
				double Longest = 0.0;

				for (const FTransform& Key : Keys) {
					const double Length = Key.GetTranslation().Size();

					Shortest = FMath::Min(Shortest, Length);
					Longest = FMath::Max(Longest, Length);
				}

				One->SetNumberField(TEXT("minT"), Keys.Num() > 0 ? Shortest : 0.0);
				One->SetNumberField(TEXT("maxT"), Longest);

				if (bKeys) {
					KeysAsJson(Keys, One);
				}

				Tracks.Add(MakeShared<FJsonValueObject>(One));
			}
		}

		Out->SetArrayField(TEXT("tracks"), Tracks);

		return Out;
	}

	bool Write(const TSharedPtr<FJsonObject>& Object, const FString& File) {
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);

		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);

		return FFileHelper::SaveStringToFile(Text, *File);
	}
}

int32 UAnimSeqDumpCommandlet::Main(const FString& Params) {
	FString Root = TEXT("/Game/Animation/Game/MainPlayer");
	FParse::Value(*Params, TEXT("path="), Root);

	FString Single;
	const bool bSingle = FParse::Value(*Params, TEXT("asset="), Single) && !Single.IsEmpty();

	/* The bone keys as well, which only fits one sequence to a file */
	const bool bKeys = FParse::Param(*Params, TEXT("keys"));

	int32 Limit = 0;
	FParse::Value(*Params, TEXT("limit="), Limit);

	FString Out;

	if (!FParse::Value(*Params, TEXT("out="), Out) || Out.IsEmpty()) {
		Out = bKeys ? (FPaths::ProjectSavedDir() / TEXT("AnimSeqDump")) : (FPaths::ProjectSavedDir() / TEXT("AnimSeqDump.json"));
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.ScanPathsSynchronous({ Root }, true);

	FARFilter Filter;
	Filter.PackagePaths.Add(*Root);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	Assets.Sort([](const FAssetData& A, const FAssetData& B) {
		return A.PackageName.LexicalLess(B.PackageName);
	});

	TArray<TSharedPtr<FJsonValue>> All;

	int32 Done = 0;
	int32 Failed = 0;

	for (const FAssetData& Asset : Assets) {
		if (bSingle && !Asset.AssetName.ToString().Contains(Single)) {
			continue;
		}

		if (Limit > 0 && Done >= Limit) {
			break;
		}

		const UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset());

		if (Sequence == nullptr) {
			Failed++;

			UE_LOG(LogAnimSeqDump, Warning, TEXT("could not load \"%s\""), *Asset.PackageName.ToString());

			continue;
		}

		const TSharedPtr<FJsonObject> One = SequenceAsJson(Sequence, bKeys);

		if (bKeys) {
			const FString File = Out / (Sequence->GetName() + TEXT(".json"));

			if (!Write(One, File)) {
				UE_LOG(LogAnimSeqDump, Error, TEXT("could not write \"%s\""), *File);

				return 1;
			}
		}
		else {
			All.Add(MakeShared<FJsonValueObject>(One));
		}

		Done++;

		/* The player set is 1100+ sequences and every one of them stays loaded otherwise */
		if (Done % 50 == 0) {
			CollectGarbage(RF_NoFlags);

			UE_LOG(LogAnimSeqDump, Display, TEXT("%d/%d"), Done, Assets.Num());
		}
	}

	if (!bKeys) {
		TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();

		Document->SetStringField(TEXT("root"), Root);
		Document->SetArrayField(TEXT("sequences"), All);

		if (!Write(Document, Out)) {
			UE_LOG(LogAnimSeqDump, Error, TEXT("could not write \"%s\""), *Out);

			return 1;
		}
	}

	UE_LOG(LogAnimSeqDump, Display, TEXT("wrote %d sequence(s) to \"%s\" (%d failed to load)"), Done, *Out, Failed);

	return 0;
}
