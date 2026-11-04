/* Copyright Reflection Contributors 2024-2026 */

#include "BlueprintGraphBuildCommandlet.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "WidgetBlueprint.h"

#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScriptDisassembler.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

DECLARE_LOG_CATEGORY_CLASS(LogBlueprintGraphBuild, All, All);

namespace {
	UObject* FindPath(const FString& Path) {
		if (UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Path)) return Found;

		return StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
	}

	/* "/Script/Module.Class:Member" -> (class, member) */
	bool SplitMember(const FString& Spec, UClass*& OutClass, FName& OutMember) {
		FString ClassPath, Member;

		if (!Spec.Split(TEXT(":"), &ClassPath, &Member, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) return false;

		OutClass = Cast<UClass>(FindPath(ClassPath));
		OutMember = FName(*Member);

		return OutClass != nullptr;
	}

	bool ParseType(FString Type, FEdGraphPinType& Out, FString& Error) {
		Out = FEdGraphPinType();

		if (Type.RemoveFromStart(TEXT("ref:"))) Out.bIsReference = true;
		if (Type.RemoveFromStart(TEXT("const:"))) Out.bIsConst = true;
		if (Type.RemoveFromStart(TEXT("array:"))) Out.ContainerType = EPinContainerType::Array;
		else if (Type.RemoveFromStart(TEXT("set:"))) Out.ContainerType = EPinContainerType::Set;

		FString Kind = Type, Argument;
		int32 Colon;

		if (Type.FindChar(TEXT(':'), Colon)) {
			Kind = Type.Left(Colon);
			Argument = Type.Mid(Colon + 1);
		}

		UObject* Object = Argument.IsEmpty() ? nullptr : FindPath(Argument);

		if (!Argument.IsEmpty() && Object == nullptr) {
			Error = FString::Printf(TEXT("no type at \"%s\""), *Argument);

			return false;
		}

		if (Kind == TEXT("bool")) Out.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		else if (Kind == TEXT("int")) Out.PinCategory = UEdGraphSchema_K2::PC_Int;
		else if (Kind == TEXT("int64")) Out.PinCategory = UEdGraphSchema_K2::PC_Int64;
		else if (Kind == TEXT("byte")) Out.PinCategory = UEdGraphSchema_K2::PC_Byte;
		else if (Kind == TEXT("float")) { Out.PinCategory = UEdGraphSchema_K2::PC_Real; Out.PinSubCategory = UEdGraphSchema_K2::PC_Float; }
		else if (Kind == TEXT("double")) { Out.PinCategory = UEdGraphSchema_K2::PC_Real; Out.PinSubCategory = UEdGraphSchema_K2::PC_Double; }
		else if (Kind == TEXT("name")) Out.PinCategory = UEdGraphSchema_K2::PC_Name;
		else if (Kind == TEXT("string")) Out.PinCategory = UEdGraphSchema_K2::PC_String;
		else if (Kind == TEXT("text")) Out.PinCategory = UEdGraphSchema_K2::PC_Text;
		else if (Kind == TEXT("object")) { Out.PinCategory = UEdGraphSchema_K2::PC_Object; Out.PinSubCategoryObject = Object; }
		else if (Kind == TEXT("class")) { Out.PinCategory = UEdGraphSchema_K2::PC_Class; Out.PinSubCategoryObject = Object; }
		else if (Kind == TEXT("softobject")) { Out.PinCategory = UEdGraphSchema_K2::PC_SoftObject; Out.PinSubCategoryObject = Object; }
		else if (Kind == TEXT("softclass")) { Out.PinCategory = UEdGraphSchema_K2::PC_SoftClass; Out.PinSubCategoryObject = Object; }
		else if (Kind == TEXT("interface")) { Out.PinCategory = UEdGraphSchema_K2::PC_Interface; Out.PinSubCategoryObject = Object; }
		else if (Kind == TEXT("struct")) { Out.PinCategory = UEdGraphSchema_K2::PC_Struct; Out.PinSubCategoryObject = Object; }
		else if (Kind == TEXT("enum")) { Out.PinCategory = UEdGraphSchema_K2::PC_Byte; Out.PinSubCategoryObject = Object; }
		else {
			Error = FString::Printf(TEXT("unknown type \"%s\""), *Kind);

			return false;
		}

		return true;
	}

	template <typename T>
	T* SetProperty(UObject* Object, const TCHAR* Name, const T& Value) {
		FProperty* Property = Object->GetClass()->FindPropertyByName(Name);
		check(Property);
		T* Slot = Property->ContainerPtrToValuePtr<T>(Object);
		*Slot = Value;

		return Slot;
	}

	UEdGraph* FindGraph(UBlueprint* Blueprint, const FName Name) {
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);

		for (UEdGraph* Graph : Graphs) {
			if (Graph->GetFName() == Name) return Graph;
		}

		return nullptr;
	}

	UEdGraphPin* FindNodePin(UEdGraphNode* Node, const FString& Name) {
		FString Wanted = Name;

		if (Wanted.Equals(TEXT("exec"), ESearchCase::CaseSensitive)) Wanted = UEdGraphSchema_K2::PN_Execute.ToString();

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin->PinName.ToString().Equals(Wanted, ESearchCase::IgnoreCase)) return Pin;
		}

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin->GetDisplayName().ToString().Equals(Wanted, ESearchCase::IgnoreCase)) return Pin;
		}

		return nullptr;
	}

	FString PinList(const UEdGraphNode* Node) {
		TArray<FString> Names;

		for (const UEdGraphPin* Pin : Node->Pins) {
			Names.Add(FString::Printf(TEXT("%s%s"), Pin->Direction == EGPD_Input ? TEXT(">") : TEXT("<"), *Pin->PinName.ToString()));
		}

		return FString::Join(Names, TEXT(" "));
	}

	struct FBuilder {
		UBlueprint* Blueprint = nullptr;
		int32 Errors = 0;

		void Fail(const FString& Message) {
			UE_LOG(LogBlueprintGraphBuild, Error, TEXT("%s"), *Message);
			Errors++;
		}

		template <typename T>
		T* Spawn(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Spec, int32 Index, TFunctionRef<void(T*)> Configure) {
			FGraphNodeCreator<T> Creator(*Graph);
			T* Node = Creator.CreateNode(false);
			Configure(Node);

			double X = Index * 320.0, Y = 0.0;
			Spec->TryGetNumberField(TEXT("x"), X);
			Spec->TryGetNumberField(TEXT("y"), Y);
			Node->NodePosX = static_cast<int32>(X);
			Node->NodePosY = static_cast<int32>(Y);

			Creator.Finalize();

			return Node;
		}

		/* Phase one: the graph and its signature, so calls to it resolve before any node is laid */
		void Declare(const TSharedPtr<FJsonObject>& Spec) {
			const FName Name(*Spec->GetStringField(TEXT("name")));
			const FString Kind = Spec->GetStringField(TEXT("kind"));

			if (Kind == TEXT("event")) return;

			if (UEdGraph* Existing = FindGraph(Blueprint, Name)) {
				FBlueprintEditorUtils::RemoveGraph(Blueprint, Existing, EGraphRemoveFlags::None);
			}

			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

			if (Kind == TEXT("override")) {
				const UFunction* Parent = Blueprint->ParentClass ? Blueprint->ParentClass->FindFunctionByName(Name) : nullptr;

				if (Parent == nullptr) {
					Fail(FString::Printf(TEXT("%s: the parent class has no function to override"), *Name.ToString()));

					return;
				}

				FBlueprintEditorUtils::AddFunctionGraph(Blueprint, Graph, false, Parent->GetOwnerClass());

				return;
			}

			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);

			TArray<UK2Node_FunctionEntry*> Entries;
			Graph->GetNodesOfClass(Entries);
			UK2Node_FunctionEntry* Entry = Entries.Num() > 0 ? Entries[0] : nullptr;

			if (Entry == nullptr) {
				Fail(FString::Printf(TEXT("%s: no entry node was made"), *Name.ToString()));

				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* Flags = nullptr;
			int32 Extra = Entry->GetExtraFlags();

			if (Spec->TryGetArrayField(TEXT("flags"), Flags)) {
				for (const TSharedPtr<FJsonValue>& Flag : *Flags) {
					const FString F = Flag->AsString();

					if (F == TEXT("pure")) Extra |= FUNC_BlueprintPure;
					else if (F == TEXT("const")) Extra |= FUNC_Const;
					else if (F == TEXT("protected")) Extra = (Extra & ~FUNC_AccessSpecifiers) | FUNC_Protected;
					else if (F == TEXT("private")) Extra = (Extra & ~FUNC_AccessSpecifiers) | FUNC_Private;
				}
			}

			Entry->SetExtraFlags(Extra);

			for (const TSharedPtr<FJsonValue>& Value : Spec->GetArrayField(TEXT("inputs"))) {
				const TSharedPtr<FJsonObject> Pin = Value->AsObject();
				FEdGraphPinType Type;
				FString Error;

				if (!ParseType(Pin->GetStringField(TEXT("type")), Type, Error)) {
					Fail(FString::Printf(TEXT("%s input %s: %s"), *Name.ToString(), *Pin->GetStringField(TEXT("name")), *Error));
					continue;
				}

				Entry->CreateUserDefinedPin(FName(*Pin->GetStringField(TEXT("name"))), Type, EGPD_Output, false);
			}

			const TArray<TSharedPtr<FJsonValue>>& Outputs = Spec->GetArrayField(TEXT("outputs"));

			if (Outputs.Num() > 0) {
				UK2Node_FunctionResult* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);

				for (const TSharedPtr<FJsonValue>& Value : Outputs) {
					const TSharedPtr<FJsonObject> Pin = Value->AsObject();
					FEdGraphPinType Type;
					FString Error;

					if (!ParseType(Pin->GetStringField(TEXT("type")), Type, Error)) {
						Fail(FString::Printf(TEXT("%s output %s: %s"), *Name.ToString(), *Pin->GetStringField(TEXT("name")), *Error));
						continue;
					}

					Result->CreateUserDefinedPin(FName(*Pin->GetStringField(TEXT("name"))), Type, EGPD_Input, false);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Locals = nullptr;

			if (Spec->TryGetArrayField(TEXT("locals"), Locals)) {
				for (const TSharedPtr<FJsonValue>& Value : *Locals) {
					const TSharedPtr<FJsonObject> Local = Value->AsObject();
					FEdGraphPinType Type;
					FString Error;

					if (!ParseType(Local->GetStringField(TEXT("type")), Type, Error)) {
						Fail(FString::Printf(TEXT("%s local %s: %s"), *Name.ToString(), *Local->GetStringField(TEXT("name")), *Error));
						continue;
					}

					FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, FName(*Local->GetStringField(TEXT("name"))), Type);
				}
			}
		}

		UEdGraphNode* MakeNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Spec, int32 Index) {
			const FString Kind = Spec->GetStringField(TEXT("k"));
			const FString Id = Spec->GetStringField(TEXT("id"));
			UClass* Class = nullptr;
			FName Member;

			auto Str = [&Spec](const TCHAR* Field) { FString V; Spec->TryGetStringField(Field, V); return V; };

			if (Kind == TEXT("call")) {
				const FString Fn = Str(TEXT("fn"));

				if (Fn.StartsWith(TEXT("self:"))) {
					return Spawn<UK2Node_CallFunction>(Graph, Spec, Index, [&](UK2Node_CallFunction* N) { N->FunctionReference.SetSelfMember(FName(*Fn.Mid(5))); });
				}

				UFunction* Function = SplitMember(Fn, Class, Member) ? Class->FindFunctionByName(Member) : nullptr;

				if (Function == nullptr) {
					Fail(FString::Printf(TEXT("%s: no function \"%s\""), *Id, *Fn));

					return nullptr;
				}

				return Spawn<UK2Node_CallFunction>(Graph, Spec, Index, [&](UK2Node_CallFunction* N) { N->SetFromFunction(Function); });
			}

			if (Kind == TEXT("get") || Kind == TEXT("set")) {
				const FName Var(*Str(TEXT("var")));
				const FString Owner = Str(TEXT("owner"));
				FMemberReference Reference;

				if (Owner.IsEmpty() || Owner == TEXT("self")) {
					Reference.SetSelfMember(Var);
				} else if (Owner == TEXT("local")) {
					FGuid Guid;

					TArray<UK2Node_FunctionEntry*> Entries;
					Graph->GetNodesOfClass(Entries);

					for (const UK2Node_FunctionEntry* Entry : Entries) {
						for (const FBPVariableDescription& Local : Entry->LocalVariables) {
							if (Local.VarName == Var) Guid = Local.VarGuid;
						}
					}

					Reference.SetLocalMember(Var, Graph->GetName(), Guid);
				} else {
					UClass* OwnerClass = Cast<UClass>(FindPath(Owner));

					if (OwnerClass == nullptr) {
						Fail(FString::Printf(TEXT("%s: no class \"%s\""), *Id, *Owner));

						return nullptr;
					}

					Reference.SetExternalMember(Var, OwnerClass);
				}

				if (Kind == TEXT("get")) {
					return Spawn<UK2Node_VariableGet>(Graph, Spec, Index, [&](UK2Node_VariableGet* N) { N->VariableReference = Reference; });
				}

				return Spawn<UK2Node_VariableSet>(Graph, Spec, Index, [&](UK2Node_VariableSet* N) { N->VariableReference = Reference; });
			}

			if (Kind == TEXT("self")) return Spawn<UK2Node_Self>(Graph, Spec, Index, [](UK2Node_Self*) {});
			if (Kind == TEXT("branch")) return Spawn<UK2Node_IfThenElse>(Graph, Spec, Index, [](UK2Node_IfThenElse*) {});
			if (Kind == TEXT("knot")) return Spawn<UK2Node_Knot>(Graph, Spec, Index, [](UK2Node_Knot*) {});
			if (Kind == TEXT("getarrayitem")) return Spawn<UK2Node_GetArrayItem>(Graph, Spec, Index, [](UK2Node_GetArrayItem*) {});
			if (Kind == TEXT("select")) return Spawn<UK2Node_Select>(Graph, Spec, Index, [](UK2Node_Select*) {});

			if (Kind == TEXT("return")) {
				return Spawn<UK2Node_FunctionResult>(Graph, Spec, Index, [](UK2Node_FunctionResult*) {});
			}

			if (Kind == TEXT("sequence")) {
				double Outputs = 2;
				Spec->TryGetNumberField(TEXT("outputs"), Outputs);
				UK2Node_ExecutionSequence* N = Spawn<UK2Node_ExecutionSequence>(Graph, Spec, Index, [](UK2Node_ExecutionSequence*) {});

				for (int32 i = 2; i < static_cast<int32>(Outputs); i++) N->AddInputPin();

				return N;
			}

			if (Kind == TEXT("cast")) {
				UClass* Target = Cast<UClass>(FindPath(Str(TEXT("class"))));

				if (Target == nullptr) {
					Fail(FString::Printf(TEXT("%s: no class \"%s\""), *Id, *Str(TEXT("class"))));

					return nullptr;
				}

				bool bPure = false;
				Spec->TryGetBoolField(TEXT("pure"), bPure);
				UK2Node_DynamicCast* N = Spawn<UK2Node_DynamicCast>(Graph, Spec, Index, [&](UK2Node_DynamicCast* C) { C->TargetType = Target; });

				if (bPure) N->SetPurity(true);

				return N;
			}

			if (Kind == TEXT("event")) {
				if (!SplitMember(Str(TEXT("fn")), Class, Member)) {
					Fail(FString::Printf(TEXT("%s: no event \"%s\""), *Id, *Str(TEXT("fn"))));

					return nullptr;
				}

				return Spawn<UK2Node_Event>(Graph, Spec, Index, [&](UK2Node_Event* N) {
					N->EventReference.SetExternalMember(Member, Class);
					N->bOverrideFunction = true;
				});
			}

			if (Kind == TEXT("customevent")) {
				UK2Node_CustomEvent* N = Spawn<UK2Node_CustomEvent>(Graph, Spec, Index, [&](UK2Node_CustomEvent* E) { E->CustomFunctionName = FName(*Str(TEXT("name"))); });
				const TArray<TSharedPtr<FJsonValue>>* Flags = nullptr;

				/* The flags exactly as cooked, where they differ from what a new custom event is given */
				if (Spec->TryGetArrayField(TEXT("functionflags"), Flags)) {
					N->FunctionFlags = 0;

					for (const TSharedPtr<FJsonValue>& Flag : *Flags) {
						const FString Name = Flag->AsString();

						if (Name == TEXT("BlueprintCallable")) N->FunctionFlags |= FUNC_BlueprintCallable;
						else if (Name == TEXT("BlueprintEvent")) N->FunctionFlags |= FUNC_BlueprintEvent;
						else if (Name == TEXT("Public")) N->FunctionFlags |= FUNC_Public;
						else Fail(FString::Printf(TEXT("%s: unknown function flag \"%s\""), *Id, *Name));
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;

				if (Spec->TryGetArrayField(TEXT("inputs"), Inputs)) {
					for (const TSharedPtr<FJsonValue>& Value : *Inputs) {
						FEdGraphPinType Type;
						FString Error;

						if (ParseType(Value->AsObject()->GetStringField(TEXT("type")), Type, Error)) {
							N->CreateUserDefinedPin(FName(*Value->AsObject()->GetStringField(TEXT("name"))), Type, EGPD_Output, false);
						} else {
							Fail(FString::Printf(TEXT("%s: %s"), *Id, *Error));
						}
					}
				}

				return N;
			}

			if (Kind == TEXT("macro")) {
				FString LibraryPath, MacroName;
				Str(TEXT("macro")).Split(TEXT(":"), &LibraryPath, &MacroName);
				UBlueprint* Library = Cast<UBlueprint>(FindPath(LibraryPath));
				UEdGraph* Macro = nullptr;

				if (Library) {
					for (UEdGraph* Candidate : Library->MacroGraphs) {
						if (Candidate->GetName() == MacroName) Macro = Candidate;
					}
				}

				if (Macro == nullptr) {
					Fail(FString::Printf(TEXT("%s: no macro \"%s\""), *Id, *Str(TEXT("macro"))));

					return nullptr;
				}

				return Spawn<UK2Node_MacroInstance>(Graph, Spec, Index, [&](UK2Node_MacroInstance* N) { N->SetMacroGraph(Macro); });
			}

			if (Kind == TEXT("createdelegate")) {
				UK2Node_CreateDelegate* N = Spawn<UK2Node_CreateDelegate>(Graph, Spec, Index, [](UK2Node_CreateDelegate*) {});
				N->SetFunction(FName(*Str(TEXT("fn"))));

				return N;
			}

			if (Kind == TEXT("adddelegate") || Kind == TEXT("removedelegate") || Kind == TEXT("cleardelegate")) {
				const FProperty* Property = SplitMember(Str(TEXT("prop")), Class, Member) ? Class->FindPropertyByName(Member) : nullptr;

				if (Property == nullptr) {
					Fail(FString::Printf(TEXT("%s: no delegate property \"%s\""), *Id, *Str(TEXT("prop"))));

					return nullptr;
				}

				bool bSelf = false;
				Spec->TryGetBoolField(TEXT("self"), bSelf);

				if (Kind == TEXT("adddelegate")) {
					return Spawn<UK2Node_AddDelegate>(Graph, Spec, Index, [&](UK2Node_AddDelegate* N) { N->SetFromProperty(Property, bSelf, Class); });
				}

				if (Kind == TEXT("cleardelegate")) {
					return Spawn<UK2Node_ClearDelegate>(Graph, Spec, Index, [&](UK2Node_ClearDelegate* N) { N->SetFromProperty(Property, bSelf, Class); });
				}

				return Spawn<UK2Node_RemoveDelegate>(Graph, Spec, Index, [&](UK2Node_RemoveDelegate* N) { N->SetFromProperty(Property, bSelf, Class); });
			}

			if (Kind == TEXT("makestruct")) {
				UScriptStruct* Struct = Cast<UScriptStruct>(FindPath(Str(TEXT("struct"))));

				if (Struct == nullptr) {
					Fail(FString::Printf(TEXT("%s: no struct \"%s\""), *Id, *Str(TEXT("struct"))));

					return nullptr;
				}

				return Spawn<UK2Node_MakeStruct>(Graph, Spec, Index, [&](UK2Node_MakeStruct* N) { N->StructType = Struct; });
			}

			if (Kind == TEXT("makearray")) {
				double Inputs = 1;
				Spec->TryGetNumberField(TEXT("inputs"), Inputs);

				return Spawn<UK2Node_MakeArray>(Graph, Spec, Index, [&](UK2Node_MakeArray* N) { SetProperty<int32>(N, TEXT("NumInputs"), static_cast<int32>(Inputs)); });
			}

			if (Kind == TEXT("async")) {
				UFunction* Factory = SplitMember(Str(TEXT("fn")), Class, Member) ? Class->FindFunctionByName(Member) : nullptr;
				const FObjectProperty* Return = Factory ? CastField<FObjectProperty>(Factory->GetReturnProperty()) : nullptr;

				if (Return == nullptr) {
					Fail(FString::Printf(TEXT("%s: no async factory \"%s\""), *Id, *Str(TEXT("fn"))));

					return nullptr;
				}

				return Spawn<UK2Node_AsyncAction>(Graph, Spec, Index, [&](UK2Node_AsyncAction* N) {
					SetProperty<FName>(N, TEXT("ProxyFactoryFunctionName"), Factory->GetFName());
					SetProperty<TObjectPtr<UClass>>(N, TEXT("ProxyFactoryClass"), Factory->GetOuterUClass());
					SetProperty<TObjectPtr<UClass>>(N, TEXT("ProxyClass"), Return->PropertyClass);
				});
			}

			if (Kind == TEXT("subsystem")) {
				UClass* Subsystem = Cast<UClass>(FindPath(Str(TEXT("class"))));

				if (Subsystem == nullptr) {
					Fail(FString::Printf(TEXT("%s: no class \"%s\""), *Id, *Str(TEXT("class"))));

					return nullptr;
				}

				return Spawn<UK2Node_GetSubsystem>(Graph, Spec, Index, [&](UK2Node_GetSubsystem* N) { N->Initialize(Subsystem); });
			}

			if (Kind == TEXT("context")) {
				/* UK2Node_GetBlueprintContextEx lives in a private editor module - reach it by path and fill CustomClass before the pins exist */
				UClass* NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintContextEditor.K2Node_GetBlueprintContextEx"));
				UClass* Context = Cast<UClass>(FindPath(Str(TEXT("class"))));

				if (NodeClass == nullptr || Context == nullptr) {
					Fail(FString::Printf(TEXT("%s: no context node class or no class \"%s\""), *Id, *Str(TEXT("class"))));

					return nullptr;
				}

				UK2Node* Node = NewObject<UK2Node>(Graph, NodeClass, NAME_None, RF_Transactional);
				Graph->AddNode(Node, false, false);
				Node->CreateNewGuid();
				Node->PostPlacedNewNode();
				SetProperty<TObjectPtr<UClass>>(Node, TEXT("CustomClass"), Context);

				double X = Index * 320.0, Y = 0.0;
				Spec->TryGetNumberField(TEXT("x"), X);
				Spec->TryGetNumberField(TEXT("y"), Y);
				Node->NodePosX = static_cast<int32>(X);
				Node->NodePosY = static_cast<int32>(Y);
				Node->AllocateDefaultPins();

				return Node;
			}

			if (Kind == TEXT("switchenum") || Kind == TEXT("enumliteral")) {
				UEnum* Enum = Cast<UEnum>(FindPath(Str(TEXT("enum"))));

				if (Enum == nullptr) {
					Fail(FString::Printf(TEXT("%s: no enum \"%s\""), *Id, *Str(TEXT("enum"))));

					return nullptr;
				}

				if (Kind == TEXT("switchenum")) {
					return Spawn<UK2Node_SwitchEnum>(Graph, Spec, Index, [&](UK2Node_SwitchEnum* N) { N->Enum = Enum; });
				}

				return Spawn<UK2Node_EnumLiteral>(Graph, Spec, Index, [&](UK2Node_EnumLiteral* N) { N->Enum = Enum; });
			}

			Fail(FString::Printf(TEXT("%s: unknown node kind \"%s\""), *Id, *Kind));

			return nullptr;
		}

		/* Phase two: the nodes, then the wires, then the pin defaults */
		void Build(const TSharedPtr<FJsonObject>& Spec) {
			const FString Name = Spec->GetStringField(TEXT("name"));
			const FString Kind = Spec->GetStringField(TEXT("kind"));
			UEdGraph* Graph = nullptr;
			TMap<FString, UEdGraphNode*> Nodes;

			if (Kind == TEXT("event")) {
				Graph = FindGraph(Blueprint, FName(*Name));

				if (Graph == nullptr) Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);

				if (Graph == nullptr) {
					Fail(FString::Printf(TEXT("%s: no event graph"), *Name));

					return;
				}

				TArray<UEdGraphNode*> Old = Graph->Nodes;

				for (UEdGraphNode* Node : Old) {
					FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
				}
			} else {
				Graph = FindGraph(Blueprint, FName(*Name));

				if (Graph == nullptr) {
					Fail(FString::Printf(TEXT("%s: graph was not declared"), *Name));

					return;
				}

				TArray<UK2Node_FunctionEntry*> Entries;
				Graph->GetNodesOfClass(Entries);
				TArray<UK2Node_FunctionResult*> Results;
				Graph->GetNodesOfClass(Results);

				if (Entries.Num() > 0) Nodes.Add(TEXT("entry"), Entries[0]);
				if (Results.Num() > 0) Nodes.Add(TEXT("result"), Results[0]);
			}

			const TArray<TSharedPtr<FJsonValue>>* NodeSpecs = nullptr;

			if (Spec->TryGetArrayField(TEXT("nodes"), NodeSpecs)) {
				int32 Index = 1;

				for (const TSharedPtr<FJsonValue>& Value : *NodeSpecs) {
					const TSharedPtr<FJsonObject> NodeSpec = Value->AsObject();
					const FString Id = NodeSpec->GetStringField(TEXT("id"));

					if (UEdGraphNode* Node = MakeNode(Graph, NodeSpec, Index++)) {
						Nodes.Add(Id, Node);
					}
				}
			}

			/* entry / result positions, if given */
			for (const TCHAR* Fixed : {TEXT("entry"), TEXT("result")}) {
				const TSharedPtr<FJsonObject>* Position = nullptr;

				if (Nodes.Contains(Fixed) && Spec->TryGetObjectField(Fixed, Position)) {
					double X = 0, Y = 0;
					(*Position)->TryGetNumberField(TEXT("x"), X);
					(*Position)->TryGetNumberField(TEXT("y"), Y);
					Nodes[Fixed]->NodePosX = static_cast<int32>(X);
					Nodes[Fixed]->NodePosY = static_cast<int32>(Y);
				}
			}

			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

			auto Resolve = [&](const FString& Ref) -> UEdGraphPin* {
				FString NodeId, PinName;

				if (!Ref.Split(TEXT("."), &NodeId, &PinName)) {
					Fail(FString::Printf(TEXT("%s: \"%s\" is not node.pin"), *Name, *Ref));

					return nullptr;
				}

				UEdGraphNode** Node = Nodes.Find(NodeId);

				if (Node == nullptr) {
					Fail(FString::Printf(TEXT("%s: no node \"%s\""), *Name, *NodeId));

					return nullptr;
				}

				UEdGraphPin* Pin = FindNodePin(*Node, PinName);

				if (Pin == nullptr) {
					Fail(FString::Printf(TEXT("%s: %s has no pin \"%s\" (pins: %s)"), *Name, *NodeId, *PinName, *PinList(*Node)));
				}

				return Pin;
			};

			const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;

			if (Spec->TryGetArrayField(TEXT("links"), Links)) {
				for (const TSharedPtr<FJsonValue>& Value : *Links) {
					const TArray<TSharedPtr<FJsonValue>>& Pair = Value->AsArray();
					UEdGraphPin* A = Resolve(Pair[0]->AsString());
					UEdGraphPin* B = Resolve(Pair[1]->AsString());

					if (A && B && !Schema->TryCreateConnection(A, B)) {
						const FPinConnectionResponse Why = Schema->CanCreateConnection(A, B);
						Fail(FString::Printf(TEXT("%s: cannot wire %s -> %s: %s"), *Name, *Pair[0]->AsString(), *Pair[1]->AsString(), *Why.Message.ToString()));
					}
				}
			}

			/* A create-delegate node only knows its function once its delegate pin is wired */
			for (const TPair<FString, UEdGraphNode*>& Pair : Nodes) {
				if (UK2Node_CreateDelegate* Delegate = Cast<UK2Node_CreateDelegate>(Pair.Value)) {
					Delegate->HandleAnyChange(true);
				}
			}

			const TSharedPtr<FJsonObject>* Defaults = nullptr;

			if (Spec->TryGetObjectField(TEXT("defaults"), Defaults)) {
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Defaults)->Values) {
					UEdGraphPin* Pin = Resolve(Pair.Key);

					if (Pin == nullptr) continue;

					const FString Value = Pair.Value->AsString();
					const FName Category = Pin->PinType.PinCategory;

					if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
						|| Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass) {
						UObject* Object = Value.IsEmpty() || Value == TEXT("None") ? nullptr : FindPath(Value);

						if (!Value.IsEmpty() && Value != TEXT("None") && Object == nullptr) {
							UE_LOG(LogBlueprintGraphBuild, Warning, TEXT("%s: %s - \"%s\" is not in the project, left empty"), *Name, *Pair.Key, *Value);
						}

						Schema->TrySetDefaultObject(*Pin, Object);
					} else if (Category == UEdGraphSchema_K2::PC_Text) {
						Schema->TrySetDefaultText(*Pin, FText::FromString(Value));
					} else {
						Schema->TrySetDefaultValue(*Pin, Value);
					}
				}
			}

			UE_LOG(LogBlueprintGraphBuild, Display, TEXT("%s: %d node(s) laid"), *Name, Nodes.Num());
		}
	};
}

int32 UBlueprintGraphBuildCommandlet::Main(const FString& Params) {
	FString Path, SpecFile, OnlyList;

	if (!FParse::Value(*Params, TEXT("path="), Path, false) || !FParse::Value(*Params, TEXT("spec="), SpecFile, false)) {
		UE_LOG(LogBlueprintGraphBuild, Error, TEXT("-path=/Game/... and -spec=<file.json> are required"));

		return 1;
	}

	FParse::Value(*Params, TEXT("only="), OnlyList, false);
	TArray<FString> Only;
	OnlyList.ParseIntoArray(Only, TEXT(","));

	FString Text;

	if (!FFileHelper::LoadFileToString(Text, *SpecFile)) {
		UE_LOG(LogBlueprintGraphBuild, Error, TEXT("cannot read \"%s\""), *SpecFile);

		return 1;
	}

	TSharedPtr<FJsonObject> Root;

	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) {
		UE_LOG(LogBlueprintGraphBuild, Error, TEXT("\"%s\" is not valid json"), *SpecFile);

		return 1;
	}

	FBuilder Builder;
	Builder.Blueprint = LoadObject<UBlueprint>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));

	if (Builder.Blueprint == nullptr) {
		UE_LOG(LogBlueprintGraphBuild, Error, TEXT("no blueprint at \"%s\""), *Path);

		return 1;
	}

	if (FParse::Param(*Params, TEXT("widgets"))) {
		if (const UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Builder.Blueprint); WidgetBlueprint && WidgetBlueprint->WidgetTree) {
			WidgetBlueprint->WidgetTree->ForEachWidget([](UWidget* Widget) {
				UE_LOG(LogBlueprintGraphBuild, Display, TEXT("widget %s : %s variable=%d parent=%s"), *Widget->GetName(), *Widget->GetClass()->GetPathName(), Widget->bIsVariable ? 1 : 0, Widget->GetParent() ? *Widget->GetParent()->GetName() : TEXT("-"));
			});
		}
	}

	TArray<TSharedPtr<FJsonObject>> Graphs;

	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("graphs"))) {
		const TSharedPtr<FJsonObject> Graph = Value->AsObject();

		if (Only.Num() == 0 || Only.Contains(Graph->GetStringField(TEXT("name")))) {
			Graphs.Add(Graph);
		}
	}

	for (const TSharedPtr<FJsonObject>& Graph : Graphs) Builder.Declare(Graph);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Builder.Blueprint);

	for (const TSharedPtr<FJsonObject>& Graph : Graphs) Builder.Build(Graph);

	FCompilerResultsLog Results;
	Results.bSilentMode = false;
	FKismetEditorUtilities::CompileBlueprint(Builder.Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages) {
		UE_LOG(LogBlueprintGraphBuild, Display, TEXT("[compile] %s"), *Message->ToText().ToString());
	}

	UE_LOG(LogBlueprintGraphBuild, Display, TEXT("%d graph(s), %d build error(s), %d compile error(s), %d compile warning(s)"),
		Graphs.Num(), Builder.Errors, Results.NumErrors, Results.NumWarnings);

	FString DisasmFile;

	/* The compiled bytecode, to hold against what was cooked */
	if (FParse::Value(*Params, TEXT("disasm="), DisasmFile, false) && Builder.Blueprint->GeneratedClass) {
		FStringOutputDevice Listing;
		FKismetBytecodeDisassembler Disasm(Listing);

		for (TFieldIterator<UFunction> It(Builder.Blueprint->GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It) {
			Listing.Logf(TEXT("[function %s]"), *It->GetName());
			Disasm.DisassembleStructure(*It);
		}

		FFileHelper::SaveStringToFile(Listing, *DisasmFile);
	}

	if (!FParse::Param(*Params, TEXT("nosave"))) {
		UPackage* Package = Builder.Blueprint->GetOutermost();
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
			UE_LOG(LogBlueprintGraphBuild, Display, TEXT("saved %s"), *FileName);
		} else {
			UE_LOG(LogBlueprintGraphBuild, Error, TEXT("could not save %s"), *FileName);

			return 1;
		}
	}

	return Builder.Errors == 0 && Results.NumErrors == 0 ? 0 : 1;
}
