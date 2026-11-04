/* Copyright Reflection Contributors 2024-2026 */

#include "AnimCurveAuditCommandlet.h"

#include "Engine/Compatibility.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"

DECLARE_LOG_CATEGORY_CLASS(LogAnimCurveAudit, All, All);

int32 UAnimCurveAuditCommandlet::Main(const FString& Params) {
	FString Root = TEXT("/Game/Animation/Game/MainPlayer");
	FParse::Value(*Params, TEXT("path="), Root);

	/* One asset by name, printed curve by curve, instead of the folder tally */
	FString Single;
	const bool bSingle = FParse::Value(*Params, TEXT("asset="), Single) && !Single.IsEmpty();

	/* Bone track names, to tell a re-import that added tracks from one that moved them */
	const bool bBones = FParse::Param(*Params, TEXT("bones"));

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.ScanPathsSynchronous({ Root }, true);

	FARFilter Filter;
	Filter.PackagePaths.Add(*Root);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	/* What the ported player graph reads by name; a zero here is a missing animation curve, not a code bug */
	const TArray<FName> Wanted = {
		TEXT("StartAnim"), TEXT("StopAnim"), TEXT("PivotAnim"),
		TEXT("TurnInPlaceAnim"), TEXT("TurnRotationAmount"), TEXT("AllowTurnInPlace"),
		TEXT("MeleeTwist"), TEXT("MeleeAnim"), TEXT("AdditiveLayer"), TEXT("MaskRightArm"),
		TEXT("DisableSpeedWarping"), TEXT("DisableFullBodySprint")
	};

	TMap<FName, int32> Found;

	for (const FName& Name : Wanted) {
		Found.Add(Name, 0);
	}

	/* An additive whose base pose is ABPT_AnimFrame / ABPT_AnimScaled needs RefPoseSeq; a null one means
	 * the additive is being applied against the wrong base, which contorts bones wherever it is used */
	int32 AdditiveTotal = 0;
	int32 AdditiveNeedsSeq = 0;
	int32 AdditiveMissingSeq = 0;

	int32 Loaded = 0;
	int32 WithAnyCurve = 0;
	int32 TotalCurves = 0;

	for (const FAssetData& Asset : Assets) {
		if (bSingle && !Asset.AssetName.ToString().Contains(Single)) {
			continue;
		}

		const UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset());
		if (!Sequence) {
			continue;
		}

		Loaded++;

		const FRawCurveTracks& Curves = Sequence->GetCurveData();
		if (Curves.FloatCurves.Num() > 0) {
			WithAnyCurve++;
		}

		TotalCurves += Curves.FloatCurves.Num();

		if (Sequence->AdditiveAnimType != AAT_None) {
			AdditiveTotal++;

			if (Sequence->RefPoseType == ABPT_AnimFrame || Sequence->RefPoseType == ABPT_AnimScaled || Sequence->RefPoseType == ABPT_LocalAnimFrame) {
				AdditiveNeedsSeq++;

				if (Sequence->RefPoseSeq == nullptr) {
					AdditiveMissingSeq++;
					UE_LOG(LogAnimCurveAudit, Warning, TEXT("NO ADDITIVE BASE: %s (additive=%d refPose=%d)"),
						*Sequence->GetName(), (int32)Sequence->AdditiveAnimType, (int32)Sequence->RefPoseType);
				}
			}
		}

		if (bSingle) {
			/* What the sequence is made of, not just its curves: a re-import that lands the curves but
			 * loses the bone tracks or the length reads as "the animation is broken" everywhere it is used */
			const IAnimationDataModel* Model = Sequence->GetDataModel();

			UE_LOG(LogAnimCurveAudit, Display, TEXT("%s: %d float curve(s), RetargetSource=%s"),
				*Sequence->GetName(), Curves.FloatCurves.Num(), *Sequence->RetargetSource.ToString());
			UE_LOG(LogAnimCurveAudit, Display, TEXT("   length=%.3f keys=%d frameRate=%s rateScale=%.2f additive=%d refPose=%d boneTracks=%d markers=%d notifies=%d"),
				Sequence->GetPlayLength(), Sequence->GetNumberOfSampledKeys(),
				*Sequence->GetSamplingFrameRate().ToPrettyText().ToString(), Sequence->RateScale,
				(int32)Sequence->AdditiveAnimType, (int32)Sequence->RefPoseType,
				Model ? Model->GetNumBoneTracks() : -1,
				Sequence->AuthoredSyncMarkers.Num(), Sequence->Notifies.Num());

			if (Sequence->AdditiveAnimType != AAT_None) {
				UE_LOG(LogAnimCurveAudit, Display, TEXT("   additive base: refPose=%d refPoseSeq=%s refFrame=%d"),
					(int32)Sequence->RefPoseType,
					Sequence->RefPoseSeq ? *Sequence->RefPoseSeq->GetName() : TEXT("NULL"),
					Sequence->RefFrameIndex);
			}

			if (bBones && Model) {
				TArray<FName> BoneNames;
				Model->GetBoneTrackNames(BoneNames);

				for (const FName& BoneName : BoneNames) {
					UE_LOG(LogAnimCurveAudit, Display, TEXT("   bone %s"), *BoneName.ToString());
				}
			}
		}

		for (const FFloatCurve& Curve : Curves.FloatCurves) {
			const FName Name = Curve.GetName();

			if (bSingle) {
				UE_LOG(LogAnimCurveAudit, Display, TEXT("   %s (%d key(s))"), *Name.ToString(), Curve.FloatCurve.GetNumKeys());
			}

			if (int32* Count = Found.Find(Name)) {
				++*Count;
			}
		}
	}

	UE_LOG(LogAnimCurveAudit, Display, TEXT("%d sequence(s) under %s, %d loaded, %d carry any float curve, %d curve(s) total"),
		Assets.Num(), *Root, Loaded, WithAnyCurve, TotalCurves);

	UE_LOG(LogAnimCurveAudit, Display, TEXT("%d additive sequence(s), %d need a RefPoseSeq, %d have none"),
		AdditiveTotal, AdditiveNeedsSeq, AdditiveMissingSeq);

	for (const FName& Name : Wanted) {
		UE_LOG(LogAnimCurveAudit, Display, TEXT("   %-24s in %d sequence(s)"), *Name.ToString(), Found[Name]);
	}

	return 0;
}
