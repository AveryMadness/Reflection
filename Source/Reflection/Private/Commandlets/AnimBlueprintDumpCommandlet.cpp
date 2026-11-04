/* Copyright Reflection Contributors 2024-2026 */

#include "AnimBlueprintDumpCommandlet.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimSubsystem_PropertyAccess.h"
#include "Animation/AnimSubsystem_Tag.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_LinkedAnimGraphBase.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/MemberReference.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "PropertyAccess.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DECLARE_LOG_CATEGORY_CLASS(LogAnimBlueprintDump, All, All);

namespace {
	TSharedPtr<FJsonObject> StructAsJson(const UStruct* Type, const void* Value) {
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

		if (Type != nullptr && Value != nullptr) {
			FJsonObjectConverter::UStructToJsonObject(Type, Value, Out.ToSharedRef(), 0, 0);
		}

		return Out;
	}

	TSharedPtr<FJsonObject> MemberReferenceAsJson(const FMemberReference& Reference) {
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

		Out->SetStringField(TEXT("MemberName"), Reference.GetMemberName().ToString());
		Out->SetStringField(TEXT("MemberParent"), Reference.GetMemberParentClass() != nullptr ? Reference.GetMemberParentClass()->GetPathName() : FString());
		Out->SetBoolField(TEXT("bSelfContext"), Reference.IsSelfContext());
		Out->SetStringField(TEXT("MemberGuid"), Reference.GetMemberGuid().ToString());

		return Out;
	}

	TSharedPtr<FJsonObject> NodeAsJson(const UEdGraphNode* Node) {
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

		Out->SetStringField(TEXT("Name"), Node->GetName());
		Out->SetStringField(TEXT("Class"), Node->GetClass()->GetName());
		Out->SetStringField(TEXT("Title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Out->SetStringField(TEXT("Comment"), Node->NodeComment);

		const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);

		if (AnimNode != nullptr) {
			/* The runtime node struct as the editor node holds it */
			if (const FStructProperty* NodeProperty = AnimNode->GetFNodeProperty()) {
				Out->SetObjectField(TEXT("Node"), StructAsJson(NodeProperty->Struct, NodeProperty->ContainerPtrToValuePtr<void>(AnimNode)));
			}

			/* And the bindings on its pins, which live on a private object of its own */
			if (const FObjectProperty* Holds = FindFProperty<FObjectProperty>(UAnimGraphNode_Base::StaticClass(), TEXT("Binding"))) {
				if (const UObject* Binding = Holds->GetObjectPropertyValue_InContainer(AnimNode)) {
					Out->SetObjectField(TEXT("Binding"), StructAsJson(Binding->GetClass(), Binding));
				}
			}

			if (const FNameProperty* Tag = FindFProperty<FNameProperty>(UAnimGraphNode_Base::StaticClass(), TEXT("Tag"))) {
				Out->SetStringField(TEXT("Tag"), Tag->GetPropertyValue_InContainer(AnimNode).ToString());
			}
		}

		if (const UAnimGraphNode_LinkedAnimGraphBase* Linked = Cast<UAnimGraphNode_LinkedAnimGraphBase>(Node)) {
			if (const FStructProperty* Reference = FindFProperty<FStructProperty>(UAnimGraphNode_LinkedAnimGraphBase::StaticClass(), TEXT("FunctionReference"))) {
				Out->SetObjectField(TEXT("FunctionReference"), MemberReferenceAsJson(*Reference->ContainerPtrToValuePtr<FMemberReference>(Linked)));
			}

			Out->SetStringField(TEXT("TargetClass"), Linked->GetTargetClass() != nullptr ? Linked->GetTargetClass()->GetPathName() : FString());
		}

		if (const UAnimGraphNode_LinkedAnimLayer* Layer = Cast<UAnimGraphNode_LinkedAnimLayer>(Node)) {
			if (const FStructProperty* Guid = FindFProperty<FStructProperty>(UAnimGraphNode_LinkedAnimLayer::StaticClass(), TEXT("InterfaceGuid"))) {
				Out->SetStringField(TEXT("InterfaceGuid"), Guid->ContainerPtrToValuePtr<FGuid>(Layer)->ToString());
			}
		}

		TArray<TSharedPtr<FJsonValue>> Pins;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr) continue;

			TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

			One->SetStringField(TEXT("Name"), Pin->PinName.ToString());
			One->SetStringField(TEXT("Direction"), Pin->Direction == EGPD_Input ? TEXT("In") : TEXT("Out"));
			One->SetStringField(TEXT("Category"), Pin->PinType.PinCategory.ToString());
			One->SetBoolField(TEXT("bPose"), UAnimationGraphSchema::IsPosePin(Pin->PinType));
			One->SetStringField(TEXT("Default"), Pin->DefaultValue);
			One->SetStringField(TEXT("DefaultObject"), Pin->DefaultObject != nullptr ? Pin->DefaultObject->GetPathName() : FString());
			One->SetBoolField(TEXT("bHidden"), Pin->bHidden);

			TArray<TSharedPtr<FJsonValue>> Links;

			for (const UEdGraphPin* Other : Pin->LinkedTo) {
				if (Other == nullptr || Other->GetOwningNodeUnchecked() == nullptr) continue;

				Links.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *Other->GetOwningNodeUnchecked()->GetName(), *Other->PinName.ToString())));
			}

			One->SetArrayField(TEXT("LinkedTo"), Links);
			Pins.Add(MakeShared<FJsonValueObject>(One));
		}

		Out->SetArrayField(TEXT("Pins"), Pins);

		return Out;
	}

	TSharedPtr<FJsonObject> GraphAsJson(const UEdGraph* Graph) {
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

		Out->SetStringField(TEXT("Name"), Graph->GetName());
		Out->SetStringField(TEXT("Class"), Graph->GetClass()->GetName());
		Out->SetStringField(TEXT("InterfaceGuid"), Graph->InterfaceGuid.ToString());
		Out->SetStringField(TEXT("GraphGuid"), Graph->GraphGuid.ToString());

		TArray<TSharedPtr<FJsonValue>> Nodes;

		for (const UEdGraphNode* Node : Graph->Nodes) {
			if (Node != nullptr) Nodes.Add(MakeShared<FJsonValueObject>(NodeAsJson(Node)));
		}

		Out->SetArrayField(TEXT("Nodes"), Nodes);

		TArray<TSharedPtr<FJsonValue>> Subs;

		for (const UEdGraph* Sub : Graph->SubGraphs) {
			if (Sub != nullptr) Subs.Add(MakeShared<FJsonValueObject>(GraphAsJson(Sub)));
		}

		Out->SetArrayField(TEXT("SubGraphs"), Subs);

		return Out;
	}
}

int32 UAnimBlueprintDumpCommandlet::Main(const FString& Params) {
	FString Path, OutFile;

	if (!FParse::Value(*Params, TEXT("path="), Path) || Path.IsEmpty()) {
		UE_LOG(LogAnimBlueprintDump, Error, TEXT("-path=/Game/... is required"));

		return 1;
	}

	if (!FParse::Value(*Params, TEXT("out="), OutFile) || OutFile.IsEmpty()) {
		OutFile = FPaths::ProjectSavedDir() / TEXT("AnimBlueprintDump.json");
	}

	UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, *Path);

	if (Blueprint == nullptr) {
		UE_LOG(LogAnimBlueprintDump, Error, TEXT("no animation blueprint at \"%s\""), *Path);

		return 1;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("Blueprint"), Blueprint->GetPathName());
	Root->SetStringField(TEXT("ParentClass"), Blueprint->ParentClass != nullptr ? Blueprint->ParentClass->GetPathName() : FString());
	Root->SetStringField(TEXT("GeneratedClass"), Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass->GetPathName() : FString());
	Root->SetStringField(TEXT("Status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));

	/* Interfaces and the graphs they brought */
	TArray<TSharedPtr<FJsonValue>> Interfaces;

	for (const FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces) {
		TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

		One->SetStringField(TEXT("Interface"), Implemented.Interface != nullptr ? Implemented.Interface->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> Graphs;

		for (const UEdGraph* Graph : Implemented.Graphs) {
			if (Graph != nullptr) Graphs.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s (%s)"), *Graph->GetName(), *Graph->InterfaceGuid.ToString())));
		}

		One->SetArrayField(TEXT("Graphs"), Graphs);
		Interfaces.Add(MakeShared<FJsonValueObject>(One));
	}

	Root->SetArrayField(TEXT("ImplementedInterfaces"), Interfaces);

	/* Every graph */
	TArray<TSharedPtr<FJsonValue>> Graphs;
	TArray<UEdGraph*> All;
	Blueprint->GetAllGraphs(All);

	TSet<const UEdGraph*> Nested;

	for (const UEdGraph* Graph : All) {
		if (Graph == nullptr) continue;

		for (const UEdGraph* Sub : Graph->SubGraphs) Nested.Add(Sub);
	}

	for (const UEdGraph* Graph : All) {
		if (Graph == nullptr || Nested.Contains(Graph)) continue;

		Graphs.Add(MakeShared<FJsonValueObject>(GraphAsJson(Graph)));
	}

	Root->SetArrayField(TEXT("Graphs"), Graphs);

	/* And what compiled out of them */
	if (const IAnimClassInterface* AnimClass = IAnimClassInterface::GetFromClass(Blueprint->GeneratedClass)) {
		TArray<TSharedPtr<FJsonValue>> Functions;

		for (const FAnimBlueprintFunction& Function : AnimClass->GetAnimBlueprintFunctions()) {
			TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();

			One->SetStringField(TEXT("Name"), Function.Name.ToString());
			One->SetStringField(TEXT("Group"), Function.Group.ToString());
			One->SetBoolField(TEXT("bImplemented"), Function.bImplemented);
			One->SetNumberField(TEXT("OutputPoseNodeIndex"), Function.OutputPoseNodeIndex);
			One->SetStringField(TEXT("OutputPoseNodeProperty"), Function.OutputPoseNodeProperty != nullptr ? Function.OutputPoseNodeProperty->GetName() : FString());

			TArray<TSharedPtr<FJsonValue>> Inputs;

			for (int32 At = 0; At < Function.InputPoseNames.Num(); ++At) {
				Inputs.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s -> %d"), *Function.InputPoseNames[At].ToString(), Function.InputPoseNodeIndices.IsValidIndex(At) ? Function.InputPoseNodeIndices[At] : INDEX_NONE)));
			}

			One->SetArrayField(TEXT("Inputs"), Inputs);
			Functions.Add(MakeShared<FJsonValueObject>(One));
		}

		Root->SetArrayField(TEXT("AnimBlueprintFunctions"), Functions);

		TArray<TSharedPtr<FJsonValue>> NodeProperties;

		for (const FStructProperty* Property : AnimClass->GetAnimNodeProperties()) {
			NodeProperties.Add(MakeShared<FJsonValueString>(Property != nullptr ? FString::Printf(TEXT("%s : %s"), *Property->GetName(), *Property->Struct->GetName()) : FString()));
		}

		Root->SetArrayField(TEXT("AnimNodeProperties"), NodeProperties);

		/* The same nodes as the generated class holds them, with their values. This is the side that can
		 * be compared against a cooked class's ClassDefaultObject: the editor graph above is keyed by
		 * editor node name, which a cook does not keep, while these are keyed by the property name the
		 * compiler gave the node and a cooked CDO carries the identical keys. */
		if (const UObject* Defaults = Blueprint->GeneratedClass->GetDefaultObject(false)) {
			TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();

			int32 At = 0;

			for (const FStructProperty* Property : AnimClass->GetAnimNodeProperties()) {
				if (Property == nullptr) continue;

				TSharedPtr<FJsonObject> One = StructAsJson(Property->Struct, Property->ContainerPtrToValuePtr<void>(Defaults));

				One->SetStringField(TEXT("__struct"), Property->Struct->GetName());
				One->SetNumberField(TEXT("__index"), At++);

				Values->SetObjectField(Property->GetName(), One);
			}

			Root->SetObjectField(TEXT("AnimNodeValues"), Values);
		}

		/* The state machines as they were baked, which is the locomotion logic itself: states, the
		 * transition graph, each transition's crossfade and blend, and the delegate index that
		 * decides it. A cooked class carries the identical structure under BakedStateMachines. */
		TArray<TSharedPtr<FJsonValue>> Machines;

		for (const FBakedAnimationStateMachine& Machine : AnimClass->GetBakedStateMachines()) {
			Machines.Add(MakeShared<FJsonValueObject>(StructAsJson(FBakedAnimationStateMachine::StaticStruct(), &Machine)));
		}

		Root->SetArrayField(TEXT("BakedStateMachines"), Machines);

		TArray<TSharedPtr<FJsonValue>> SyncGroups;

		for (const FName& Group : AnimClass->GetSyncGroupNames()) {
			SyncGroups.Add(MakeShared<FJsonValueString>(Group.ToString()));
		}

		Root->SetArrayField(TEXT("SyncGroupNames"), SyncGroups);

		TSharedPtr<FJsonObject> SavedPoses = MakeShared<FJsonObject>();

		for (const TPair<FName, FCachedPoseIndices>& Saved : AnimClass->GetOrderedSavedPoseNodeIndicesMap()) {
			SavedPoses->SetObjectField(Saved.Key.ToString(), StructAsJson(FCachedPoseIndices::StaticStruct(), &Saved.Value));
		}

		Root->SetObjectField(TEXT("OrderedSavedPoseIndicesMap"), SavedPoses);

		if (const FAnimSubsystem_PropertyAccess* Access = AnimClass->FindSubsystem<FAnimSubsystem_PropertyAccess>()) {
			Root->SetObjectField(TEXT("PropertyAccess"), StructAsJson(FPropertyAccessLibrary::StaticStruct(), &Access->GetLibrary()));
		}

		if (const FAnimSubsystem_Tag* Tags = AnimClass->FindSubsystem<FAnimSubsystem_Tag>()) {
			Root->SetObjectField(TEXT("Tags"), StructAsJson(FAnimSubsystem_Tag::StaticStruct(), Tags));
		}
	}

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	if (!FFileHelper::SaveStringToFile(Text, *OutFile)) {
		UE_LOG(LogAnimBlueprintDump, Error, TEXT("could not write \"%s\""), *OutFile);

		return 1;
	}

	UE_LOG(LogAnimBlueprintDump, Display, TEXT("wrote \"%s\" (%d graph(s))"), *OutFile, Graphs.Num());

	return 0;
}
