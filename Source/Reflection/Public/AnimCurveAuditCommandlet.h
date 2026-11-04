/* Copyright Reflection Contributors 2024-2026 */

/* Lists the float curves an animation actually carries, and which of the curves the player graph reads
 * are missing across a folder:
 *
 *     -run=AnimCurveAudit -path=/Game/Animation/Game/MainPlayer [-asset=M_Jog_N_Start_Core]
 *
 * Reading this off the loaded asset rather than grepping the .uasset, because a curve name that is only
 * held in native (untagged) serialization never lands in the package's name table - the same trap that
 * made the skeleton's AnimRetargetSources look absent when the map was full. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimCurveAuditCommandlet.generated.h"

UCLASS()
class UAnimCurveAuditCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
