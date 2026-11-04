/* Copyright Reflection Contributors 2024-2026 */

/* Headless dump of an imported animation blueprint, so what the editor actually made can be put
 * beside what the class said:
 *
 *     -run=AnimBlueprintDump -path=/Game/Path/To/AnimBP -out=<file.json>
 *
 * Writes every graph and node (with its runtime node struct, function reference, interface guid,
 * pins and their links, and the bindings on its pins), the compiled class's layer functions and
 * their implemented state, the implemented interfaces, and the property access library. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimBlueprintDumpCommandlet.generated.h"

UCLASS()
class UAnimBlueprintDumpCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
