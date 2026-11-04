/* Copyright Reflection Contributors 2024-2026 */

/* Puts the shipped values of a blueprint's class defaults and of the component templates it inherits back onto the
 * project's copy, without re-importing the blueprint:
 *
 *     -run=BlueprintDefaults -path=/Game/A;/Game/B;... [-subobjects=CharMoveComp,CollisionCylinder] [-reset=CharMoveComp]
 *                            [-skip=AttachParent,...] [-dryrun]
 *
 * Blueprints are handled in the order given, so list a parent before its children. "-reset" first copies each named
 * template back from its archetype (the parent's), which removes overrides the shipped blueprint does not have; the
 * shipped values are then written on top. A property is left alone when the project class has no such property, or
 * when its value names an asset the project does not have, so nothing is cleared for want of a reference. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintDefaultsCommandlet.generated.h"

UCLASS()
class UBlueprintDefaultsCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
