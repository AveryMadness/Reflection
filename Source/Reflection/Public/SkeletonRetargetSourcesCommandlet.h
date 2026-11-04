/* Copyright Reflection Contributors 2024-2026 */

/* Restores a skeleton's AnimRetargetSources from a Cloud export, in place:
 *
 *     -run=SkeletonRetargetSources -skeleton=/Game/Path/To/Skeleton -json=<export json on disk> [-dryrun]
 *
 * The importer used to read AnimRetargetSources out of the export's Properties, where it never is
 * (it is serialized beside ReferenceSkeleton at the export's top level), so skeletons imported before
 * that fix carry an empty map. Every animation then has a RetargetSource naming a pose the skeleton
 * does not know, USkeleton::GetRefLocalPoses silently answers with the skeleton's own reference pose,
 * and animations authored against a different body get no translation retargeting at all - which reads
 * as limbs glitching when two such animations blend.
 *
 * This patches the map onto the existing asset rather than re-importing the skeleton, because the
 * meshes and the 1000+ animations already bound to it do not need rebuilding. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "SkeletonRetargetSourcesCommandlet.generated.h"

UCLASS()
class USkeletonRetargetSourcesCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
