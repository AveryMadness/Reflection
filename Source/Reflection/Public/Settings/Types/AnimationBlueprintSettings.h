/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "AnimationBlueprintSettings.generated.h"

/* Settings for animation blueprints */
USTRUCT()
struct FRAnimationBlueprintSettings {
	GENERATED_BODY()
public:
	/* Saves IDs in Node's comment. */
	UPROPERTY(EditAnywhere, DisplayName = "ID in Comments", Config, AdvancedDisplay, Category = AnimationBlueprintSettings)
	bool NodeIDComments = false;

	/* Reflects the linked animation blueprints an import reaches even when they are already in the project, so a whole chain is rebuilt from one import. */
	UPROPERTY(EditAnywhere, DisplayName = "Reflect Existing Animation Blueprints", Config, Category = AnimationBlueprintSettings)
	bool ReflectExistingAnimationBlueprints = true;
};