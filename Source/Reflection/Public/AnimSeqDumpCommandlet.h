/* Copyright Reflection Contributors 2024-2026 */

/* Writes what an imported animation sequence is actually made of, in the shape the Cloud's
 * /api/export/animation answers with, so the two can be diffed field by field:
 *
 *     -run=AnimSeqDump -path=/Game/Animation/Game/MainPlayer [-asset=M_Jog] [-out=x.json] [-keys] [-limit=N]
 *
 * Without -keys it is one file holding every sequence's structure; with -keys it is one file per
 * sequence under -out, including the flattened bone keys, because the whole player set with keys
 * does not fit in one document. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimSeqDumpCommandlet.generated.h"

UCLASS()
class UAnimSeqDumpCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
