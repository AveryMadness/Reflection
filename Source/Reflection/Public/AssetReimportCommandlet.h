/* Copyright Reflection Contributors 2024-2026 */

/* Re-imports one asset from the Cloud and saves it, with no editor around it:
 *
 *     -run=AssetReimport -path=/Game/... [-json=<where to keep what the Cloud gave back>] [-dryrun] [-norefs] [-nographs]
 *
 * This is the whole importer, not a surgical edit - it rebuilds the asset. Back the package up
 * first, and check what changed afterwards. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AssetReimportCommandlet.generated.h"

UCLASS()
class UAssetReimportCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
