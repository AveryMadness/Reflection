/* Copyright Reflection Contributors 2024-2026 */

/* Rebuilds the runtime data of imported blend spaces, which the importer never wrote:
 *
 *     -run=BlendSpaceResave [-path=/Game/Some/Folder] [-dryrun]
 *
 * A blend space keeps its samples in SampleData, but what it is sampled through at runtime is the
 * triangulation in BlendSpaceData (plus GridSamples). Only ResampleData() builds those, and PostLoad
 * calls ValidateSampleData() alone, so an imported blend space loads with an empty triangulation,
 * returns no samples from GetSamplesFromBlendInput and holds a single frame forever. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlendSpaceResaveCommandlet.generated.h"

UCLASS()
class UBlendSpaceResaveCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
