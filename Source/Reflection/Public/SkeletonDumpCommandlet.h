/* Copyright Reflection Contributors 2024-2026 */

/* Everything a skeleton holds that an animation is evaluated through, as JSON, so an imported
 * skeleton can be diffed against the Cloud's export of the shipped one:
 *
 *     -run=SkeletonDump -path=/Game/... [-out=x.json]
 *
 * The reference skeleton and the retarget sources are written by USkeleton::Serialize rather than
 * declared as properties, so AssetDump cannot see them - hence a dump of its own. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "SkeletonDumpCommandlet.generated.h"

UCLASS()
class USkeletonDumpCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
