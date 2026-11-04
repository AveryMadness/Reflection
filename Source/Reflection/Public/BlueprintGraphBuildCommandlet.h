/* Copyright Reflection Contributors 2024-2026 */

/* Builds a blueprint's graphs from a hand-written description - every node, wire and pin default is spelled out in the
 * spec; nothing is inferred from the cooked bytecode:
 *
 *     -run=BlueprintGraphBuild -path=/Game/A -spec=<file.json> [-only=GraphA,GraphB] [-nosave]
 *
 * Spec: { "graphs": [ { "name": "...", "kind": "function" | "override" | "event",
 *                       "flags": ["pure", "const", "protected", "private"],
 *                       "inputs": [{"name", "type"}], "outputs": [{"name", "type"}], "locals": [{"name", "type"}],
 *                       "nodes": [ {"id", "k", ...} ], "links": [["node.pin", "node.pin"]], "defaults": {"node.pin": "value"} } ] }
 *
 * A function / override graph is made (or emptied) and gets "entry" and "result" nodes; an event spec adds its nodes to
 * the EventGraph after removing the nodes an earlier run of the same spec put there.
 * Types: bool int int64 byte float double name string text, object:<path> class:<path> softobject:<path>
 * softclass:<path> struct:<path> enum:<path>, wrapped as array:<type> / set:<type>; a "ref:" prefix passes by reference.
 * Node kinds: call get set self branch cast event customevent macro createdelegate adddelegate removedelegate makearray
 * getarrayitem async subsystem context switchenum enumliteral select knot sequence return. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintGraphBuildCommandlet.generated.h"

UCLASS()
class UBlueprintGraphBuildCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
