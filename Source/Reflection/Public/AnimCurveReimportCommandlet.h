/* Copyright Reflection Contributors 2024-2026 */

/* Writes the float curves from the Cloud onto animations that are missing them, and nothing else:
 *
 *     -run=AnimCurveReimport [-path=/Game/...] [-limit=N] [-skipwithcurves] [-dryrun]
 *
 * Animations imported before the importer learned to read CompressedCurveData off the export root came in
 * with no float curves at all. The player graph drives its whole transition system off those curves by
 * name (StartAnim, StopAnim, PivotAnim, TurnInPlaceAnim, TurnRotationAmount, AllowTurnInPlace, ...), and
 * a missing curve reads back as 0, which silently disables jog starts, stops, pivots and turn-in-place.
 *
 * This goes through ReadAnimationCurves, which edits RCT_Float through the animation data controller and
 * leaves every bone track, the length, the sync markers and the notifies alone. The earlier version ran
 * the whole animation importer instead: that rebuilt each asset from the export and added camera,
 * camera_root, warp_point_01..03, attach_* and hand_attach_* bone tracks to 134 sequences - mostly
 * additive aim-offset poses - which fed the warping nodes and the camera garbage and had to be reverted.
 *
 * Curve-only is idempotent, so every sequence is processed by default; -skipwithcurves leaves the ones
 * that already carry curves alone, which makes an interrupted run resumable. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimCurveReimportCommandlet.generated.h"

UCLASS()
class UAnimCurveReimportCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
