/* Copyright Reflection Contributors 2024-2026 */

/* Every reflected property of one asset, as JSON, so an imported asset can be diffed against the
 * Cloud's export of the shipped one:
 *
 *     -run=AssetDump -path=/Game/...[;/Game/...] [-out=x.json | -outdir=<dir>]   (DataTables also get their rows)
 *
 * For the asset kinds that have a dump of their own (an animation sequence, an animation blueprint)
 * use that instead - this is for the plain data assets that neither covers. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AssetDumpCommandlet.generated.h"

UCLASS()
class UAssetDumpCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
