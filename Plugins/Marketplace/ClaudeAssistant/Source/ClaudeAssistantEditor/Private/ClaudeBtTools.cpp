// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeBtTools.cpp
// Behavior Tree structure authoring. The editor graph (UBehaviorTreeGraph) is the
// source of truth; Graph->UpdateAsset() regenerates the runtime tree (BTAsset->RootNode).

#include "ClaudeBtTools.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantFocusTracker.h"
#include "ClaudeAssistantCompat.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "UObject/UnrealType.h"

#include "AIGraphTypes.h"
#include "AIGraphNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "EdGraphSchema_BehaviorTree.h"

namespace
{
#if CLAUDE_UE_BEFORE(5, 6)
	// Pre-5.6 engines don't export several BehaviorTreeEditor graph classes
	// (no MinimalAPI / pre-DECLARE_CLASS2), so StaticClass()/Cast<>/NewObject<>
	// on them fail to link from plugin code. Resolve the UClass via reflection
	// and static_cast after an IsA check instead. The 5.6+ path is unchanged.
	UClass* BtEditorNodeClass(const TCHAR* ClassName)
	{
		return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/BehaviorTreeEditor.%s"), ClassName));
	}
#endif
	FString BtSerJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> BtSchema(const TArray<TPair<FString, FString>>& Props, const TArray<FString>& Required)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Props)
		{
			TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
			Prop->SetStringField(TEXT("type"), TEXT("string"));
			Prop->SetStringField(TEXT("description"), Pair.Value);
			P->SetObjectField(Pair.Key, Prop);
		}
		TArray<TSharedPtr<FJsonValue>> Req;
		for (const FString& R : Required) { Req.Add(MakeShared<FJsonValueString>(R)); }
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), P);
		Schema->SetArrayField(TEXT("required"), Req);
		return Schema;
	}

	UBehaviorTree* ResolveBT(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path;
		if (Args.IsValid()) { Args->TryGetStringField(TEXT("asset_path"), Path); }
		if (!Path.IsEmpty()) { return LoadObject<UBehaviorTree>(nullptr, *Path); }
		return Cast<UBehaviorTree>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}

	// Resolve a Blackboard: explicit asset_path, else focused blackboard, else the focused BT's blackboard.
	UBlackboardData* ResolveBlackboard(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path;
		if (Args.IsValid()) { Args->TryGetStringField(TEXT("asset_path"), Path); }
		if (!Path.IsEmpty()) { return LoadObject<UBlackboardData>(nullptr, *Path); }
		UObject* Active = FClaudeAssistantFocusTracker::Get().GetActiveAsset();
		if (UBlackboardData* BB = Cast<UBlackboardData>(Active)) { return BB; }
		if (UBehaviorTree* BT = Cast<UBehaviorTree>(Active)) { return BT->BlackboardAsset; }
		return nullptr;
	}

	UClass* ResolveBlackboardKeyType(const FString& InType)
	{
		const FString T = InType.ToLower();
		if (T == TEXT("bool") || T == TEXT("boolean")) { return UBlackboardKeyType_Bool::StaticClass(); }
		if (T == TEXT("int") || T == TEXT("int32") || T == TEXT("integer")) { return UBlackboardKeyType_Int::StaticClass(); }
		if (T == TEXT("float") || T == TEXT("double") || T == TEXT("real")) { return UBlackboardKeyType_Float::StaticClass(); }
		if (T == TEXT("vector")) { return UBlackboardKeyType_Vector::StaticClass(); }
		if (T == TEXT("object") || T == TEXT("actor")) { return UBlackboardKeyType_Object::StaticClass(); }
		if (T == TEXT("name")) { return UBlackboardKeyType_Name::StaticClass(); }
		if (T == TEXT("string")) { return UBlackboardKeyType_String::StaticClass(); }
		if (T == TEXT("rotator")) { return UBlackboardKeyType_Rotator::StaticClass(); }
		return nullptr;
	}

	// Ensure the BT has an editor graph with a Root ed-node; return the graph + root.
	UBehaviorTreeGraph* EnsureBtGraph(UBehaviorTree* BTAsset, UBehaviorTreeGraphNode_Root*& OutRoot)
	{
		OutRoot = nullptr;
		if (!BTAsset) { return nullptr; }

		UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BTAsset->BTGraph);
		if (!Graph)
		{
			BTAsset->BTGraph = FBlueprintEditorUtils::CreateNewGraph(
				BTAsset, TEXT("Behavior Tree"),
				UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass());
			Graph = Cast<UBehaviorTreeGraph>(BTAsset->BTGraph);
			if (Graph)
			{
				Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
				Graph->OnCreated();
			}
		}
		if (!Graph) { return nullptr; }
		Graph->Initialize();

		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UBehaviorTreeGraphNode_Root* R = Cast<UBehaviorTreeGraphNode_Root>(N)) { OutRoot = R; break; }
		}
		return Graph;
	}

	UEdGraphNode* FindBtNode(UEdGraph* Graph, const FGuid& Guid)
	{
		if (!Graph) { return nullptr; }
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == Guid) { return N; }
		}
		return nullptr;
	}

	UClass* ResolveNodeClass(const FString& Name, const UClass* Base)
	{
		UClass* Cls = UClass::TryFindTypeSlow<UClass>(Name);
		if ((!Cls || !Cls->IsChildOf(Base)) && Name.StartsWith(TEXT("U")))
		{
			FString Alt = Name; Alt.RightChopInline(1);
			Cls = UClass::TryFindTypeSlow<UClass>(Alt);
		}
		if (Cls && Cls->IsChildOf(Base) && !Cls->HasAnyClassFlags(CLASS_Abstract)) { return Cls; }
		return nullptr;
	}

	void PersistBt(UBehaviorTreeGraph* Graph, UBehaviorTree* BTAsset)
	{
		Graph->UpdateAsset();
		Graph->NotifyGraphChanged();
		BTAsset->MarkPackageDirty();
	}
}

namespace ClaudeBtTools
{
	void RegisterAll(FClaudeToolRegistry& Registry)
	{
		// ---------------------------------------------------------------------
		// add_bt_composite : Selector/Sequence under a parent (Root or composite).
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("add_bt_composite");
			Tool.Category = TEXT("bt");
			Tool.Description = TEXT("Add a composite node (e.g. 'BTComposite_Selector', 'BTComposite_Sequence') to a Behavior Tree. Without parent_id it attaches under the Root; otherwise under the given composite node id. child_index orders siblings left-to-right. Operates on the focused Behavior Tree unless asset_path is given. Undoable.");
			Tool.InputSchema = BtSchema({
				{ TEXT("composite_class"), TEXT("Composite class name (default 'BTComposite_Selector').") },
				{ TEXT("parent_id"), TEXT("Optional parent node id; empty attaches under the Root.") },
				{ TEXT("child_index"), TEXT("Optional sibling order index (0-based).") },
				{ TEXT("asset_path"), TEXT("Optional Behavior Tree object path; defaults to the focused asset.") },
			}, {});
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UBehaviorTree* BTAsset = ResolveBT(Args);
				if (!BTAsset) { return FClaudeToolResult::Error(TEXT("No Behavior Tree is focused (or pass asset_path).")); }

				FString ClassName;
				Args->TryGetStringField(TEXT("composite_class"), ClassName);
				if (ClassName.IsEmpty()) { ClassName = TEXT("BTComposite_Selector"); }
				UClass* CompClass = ResolveNodeClass(ClassName, UBTCompositeNode::StaticClass());
				if (!CompClass) { return FClaudeToolResult::Error(FString::Printf(TEXT("Composite class '%s' not found."), *ClassName)); }

				UBehaviorTreeGraphNode_Root* RootEd = nullptr;
				UBehaviorTreeGraph* Graph = EnsureBtGraph(BTAsset, RootEd);
				if (!Graph || !RootEd) { return FClaudeToolResult::Error(TEXT("Failed to prepare the Behavior Tree graph.")); }

				FString ParentId;
				Args->TryGetStringField(TEXT("parent_id"), ParentId);
				double ChildIndexD = 0.0;
				Args->TryGetNumberField(TEXT("child_index"), ChildIndexD);
				const int32 ChildIndex = static_cast<int32>(ChildIndexD);

				UBehaviorTreeGraphNode* ParentEd = RootEd;
				if (!ParentId.IsEmpty())
				{
					FGuid G;
					if (!FGuid::Parse(ParentId, G)) { return FClaudeToolResult::Error(TEXT("Invalid 'parent_id'.")); }
#if CLAUDE_UE_BEFORE(5, 6)
					UEdGraphNode* FoundEd = FindBtNode(Graph, G);
					UClass* BtNodeCls = BtEditorNodeClass(TEXT("BehaviorTreeGraphNode"));
					ParentEd = (FoundEd && BtNodeCls && FoundEd->IsA(BtNodeCls)) ? static_cast<UBehaviorTreeGraphNode*>(FoundEd) : nullptr;
#else
					ParentEd = Cast<UBehaviorTreeGraphNode>(FindBtNode(Graph, G));
#endif
					if (!ParentEd) { return FClaudeToolResult::Error(TEXT("Parent node not found.")); }
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add BT composite")));
				Graph->Modify();
				UBehaviorTreeGraphNode_Composite* Node = nullptr;
				{
					FGraphNodeCreator<UBehaviorTreeGraphNode_Composite> Creator(*Graph);
					Node = Creator.CreateNode(false);
					Node->ClassData = FGraphNodeClassData(CompClass, FString());
					Node->NodePosX = ParentEd->NodePosX + ChildIndex * 300;
					Node->NodePosY = ParentEd->NodePosY + 150;
					Creator.Finalize();
				}

				UEdGraphPin* ParentOut = ParentEd->GetOutputPin(0);
				UEdGraphPin* ChildIn = Node->GetInputPin(0);
				if (!ParentOut || !ChildIn) { return FClaudeToolResult::Error(TEXT("Could not wire the composite (missing pins).")); }
				ParentOut->MakeLinkTo(ChildIn);

				PersistBt(Graph, BTAsset);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
				return FClaudeToolResult::Ok(BtSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// add_bt_task : leaf task under a composite.
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("add_bt_task");
			Tool.Category = TEXT("bt");
			Tool.Description = TEXT("Add a leaf Task node (e.g. 'BTTask_Wait', 'BTTask_MoveTo') under a composite node in a Behavior Tree. parent_id must be a composite node id (from add_bt_composite). Operates on the focused Behavior Tree unless asset_path is given. Undoable.");
			Tool.InputSchema = BtSchema({
				{ TEXT("task_class"), TEXT("Task class name (e.g. 'BTTask_Wait').") },
				{ TEXT("parent_id"), TEXT("Parent composite node id.") },
				{ TEXT("child_index"), TEXT("Optional sibling order index (0-based).") },
				{ TEXT("asset_path"), TEXT("Optional Behavior Tree object path; defaults to the focused asset.") },
			}, { TEXT("task_class"), TEXT("parent_id") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UBehaviorTree* BTAsset = ResolveBT(Args);
				if (!BTAsset) { return FClaudeToolResult::Error(TEXT("No Behavior Tree is focused (or pass asset_path).")); }

				FString ClassName, ParentId;
				Args->TryGetStringField(TEXT("task_class"), ClassName);
				Args->TryGetStringField(TEXT("parent_id"), ParentId);
				if (ClassName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'task_class'.")); }
				if (ParentId.IsEmpty()) { return FClaudeToolResult::Error(TEXT("A task needs a composite parent; pass 'parent_id'.")); }
				UClass* TaskClass = ResolveNodeClass(ClassName, UBTTaskNode::StaticClass());
				if (!TaskClass) { return FClaudeToolResult::Error(FString::Printf(TEXT("Task class '%s' not found."), *ClassName)); }

				UBehaviorTreeGraphNode_Root* RootEd = nullptr;
				UBehaviorTreeGraph* Graph = EnsureBtGraph(BTAsset, RootEd);
				if (!Graph) { return FClaudeToolResult::Error(TEXT("Failed to prepare the Behavior Tree graph.")); }

				FGuid G;
				if (!FGuid::Parse(ParentId, G)) { return FClaudeToolResult::Error(TEXT("Invalid 'parent_id'.")); }
				UBehaviorTreeGraphNode_Composite* ParentEd = Cast<UBehaviorTreeGraphNode_Composite>(FindBtNode(Graph, G));
				if (!ParentEd) { return FClaudeToolResult::Error(TEXT("Parent composite not found (tasks must attach under a composite).")); }

				double ChildIndexD = 0.0;
				Args->TryGetNumberField(TEXT("child_index"), ChildIndexD);
				const int32 ChildIndex = static_cast<int32>(ChildIndexD);

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add BT task")));
				Graph->Modify();
				UBehaviorTreeGraphNode_Task* Node = nullptr;
#if CLAUDE_UE_BEFORE(5, 6)
				{
					UClass* TaskNodeCls = BtEditorNodeClass(TEXT("BehaviorTreeGraphNode_Task"));
					if (!TaskNodeCls) { return FClaudeToolResult::Error(TEXT("BehaviorTreeGraphNode_Task class not found.")); }
					// Mirrors FGraphNodeCreator + UEdGraph::CreateNode (protected pre-5.6)
					// without the unexported StaticClass(): NewObject + AddNode + guid/placed/pins.
					Node = static_cast<UBehaviorTreeGraphNode_Task*>(NewObject<UEdGraphNode>(Graph, TaskNodeCls, NAME_None, RF_Transactional));
					Graph->AddNode(Node, /*bFromUI*/false, /*bSelectNewNode*/false);
					Node->ClassData = FGraphNodeClassData(TaskClass, FString());
					Node->NodePosX = ParentEd->NodePosX + ChildIndex * 300;
					Node->NodePosY = ParentEd->NodePosY + 150;
					Node->CreateNewGuid();
					Node->PostPlacedNewNode();
					if (Node->Pins.Num() == 0) { Node->AllocateDefaultPins(); }
				}
#else
				{
					FGraphNodeCreator<UBehaviorTreeGraphNode_Task> Creator(*Graph);
					Node = Creator.CreateNode(false);
					Node->ClassData = FGraphNodeClassData(TaskClass, FString());
					Node->NodePosX = ParentEd->NodePosX + ChildIndex * 300;
					Node->NodePosY = ParentEd->NodePosY + 150;
					Creator.Finalize();
				}
#endif

				UEdGraphPin* ParentOut = ParentEd->GetOutputPin(0);
				UEdGraphPin* ChildIn = Node->GetInputPin(0);
				if (!ParentOut || !ChildIn) { return FClaudeToolResult::Error(TEXT("Could not wire the task (missing pins).")); }
				ParentOut->MakeLinkTo(ChildIn);

				PersistBt(Graph, BTAsset);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
				return FClaudeToolResult::Ok(BtSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// add_bt_decorator : attach a decorator (or service) sub-node to a node.
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("add_bt_decorator");
			Tool.Category = TEXT("bt");
			Tool.Description = TEXT("Attach a Decorator sub-node (e.g. 'BTDecorator_Blackboard') to a composite or task node in a Behavior Tree, given the parent node id. Operates on the focused Behavior Tree unless asset_path is given. Undoable.");
			Tool.InputSchema = BtSchema({
				{ TEXT("decorator_class"), TEXT("Decorator class name (e.g. 'BTDecorator_Blackboard').") },
				{ TEXT("parent_id"), TEXT("Node id to attach the decorator to.") },
				{ TEXT("asset_path"), TEXT("Optional Behavior Tree object path; defaults to the focused asset.") },
			}, { TEXT("decorator_class"), TEXT("parent_id") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UBehaviorTree* BTAsset = ResolveBT(Args);
				if (!BTAsset) { return FClaudeToolResult::Error(TEXT("No Behavior Tree is focused (or pass asset_path).")); }

				FString ClassName, ParentId;
				Args->TryGetStringField(TEXT("decorator_class"), ClassName);
				Args->TryGetStringField(TEXT("parent_id"), ParentId);
				if (ClassName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'decorator_class'.")); }
				if (ParentId.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'parent_id'.")); }
				UClass* DecClass = ResolveNodeClass(ClassName, UBTDecorator::StaticClass());
				if (!DecClass) { return FClaudeToolResult::Error(FString::Printf(TEXT("Decorator class '%s' not found."), *ClassName)); }

				UBehaviorTreeGraphNode_Root* RootEd = nullptr;
				UBehaviorTreeGraph* Graph = EnsureBtGraph(BTAsset, RootEd);
				if (!Graph) { return FClaudeToolResult::Error(TEXT("Failed to prepare the Behavior Tree graph.")); }

				FGuid G;
				if (!FGuid::Parse(ParentId, G)) { return FClaudeToolResult::Error(TEXT("Invalid 'parent_id'.")); }
#if CLAUDE_UE_BEFORE(5, 6)
				UEdGraphNode* FoundEd = FindBtNode(Graph, G);
				UClass* BtNodeCls = BtEditorNodeClass(TEXT("BehaviorTreeGraphNode"));
				UBehaviorTreeGraphNode* ParentEd = (FoundEd && BtNodeCls && FoundEd->IsA(BtNodeCls)) ? static_cast<UBehaviorTreeGraphNode*>(FoundEd) : nullptr;
#else
				UBehaviorTreeGraphNode* ParentEd = Cast<UBehaviorTreeGraphNode>(FindBtNode(Graph, G));
#endif
				if (!ParentEd) { return FClaudeToolResult::Error(TEXT("Parent node not found.")); }

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add BT decorator")));
				Graph->Modify();
#if CLAUDE_UE_BEFORE(5, 6)
				UClass* DecNodeCls = BtEditorNodeClass(TEXT("BehaviorTreeGraphNode_Decorator"));
				if (!DecNodeCls) { return FClaudeToolResult::Error(TEXT("BehaviorTreeGraphNode_Decorator class not found.")); }
				UBehaviorTreeGraphNode_Decorator* Dec = static_cast<UBehaviorTreeGraphNode_Decorator*>(NewObject<UObject>(Graph, DecNodeCls));
#else
				UBehaviorTreeGraphNode_Decorator* Dec = NewObject<UBehaviorTreeGraphNode_Decorator>(Graph);
#endif
				Dec->ClassData = FGraphNodeClassData(DecClass, FString());
				Dec->CreateNewGuid();
				ParentEd->AddSubNode(Dec, Graph);

				PersistBt(Graph, BTAsset);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("node_id"), Dec->NodeGuid.ToString());
				return FClaudeToolResult::Ok(BtSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// get_bt_nodes : list the Behavior Tree editor-graph nodes.
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("get_bt_nodes");
			Tool.Category = TEXT("bt");
			Tool.Description = TEXT("List the editor-graph nodes of a Behavior Tree (id, class, runtime class, title). Use the ids as parent_id for add_bt_task / add_bt_composite. Operates on the focused Behavior Tree unless asset_path is given.");
			Tool.InputSchema = BtSchema({
				{ TEXT("asset_path"), TEXT("Optional Behavior Tree object path; defaults to the focused asset.") },
			}, {});
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UBehaviorTree* BTAsset = ResolveBT(Args);
				if (!BTAsset) { return FClaudeToolResult::Error(TEXT("No Behavior Tree is focused (or pass asset_path).")); }
				UBehaviorTreeGraphNode_Root* RootEd = nullptr;
				UBehaviorTreeGraph* Graph = EnsureBtGraph(BTAsset, RootEd);
				if (!Graph) { return FClaudeToolResult::Error(TEXT("Failed to prepare the Behavior Tree graph.")); }

				TArray<TSharedPtr<FJsonValue>> Nodes;
				for (const UEdGraphNode* N : Graph->Nodes)
				{
					if (!N) { continue; }
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("id"), N->NodeGuid.ToString());
					Obj->SetStringField(TEXT("class"), N->GetClass() ? N->GetClass()->GetName() : FString());
					Obj->SetStringField(TEXT("title"), N->GetNodeTitle(ENodeTitleType::ListView).ToString());
					if (const UAIGraphNode* AiNode = Cast<UAIGraphNode>(N))
					{
						Obj->SetStringField(TEXT("runtime_class"), AiNode->NodeInstance && AiNode->NodeInstance->GetClass()
							? AiNode->NodeInstance->GetClass()->GetName() : FString());
					}
					Nodes.Add(MakeShared<FJsonValueObject>(Obj));
				}

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("count"), Nodes.Num());
				Out->SetArrayField(TEXT("nodes"), Nodes);
				return FClaudeToolResult::Ok(BtSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// set_bt_node_property : configure a BT node's runtime instance.
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("set_bt_node_property");
			Tool.Category = TEXT("bt");
			Tool.Description = TEXT("Set a property on a Behavior Tree node's runtime instance, by node id (from get_bt_nodes), property name, and value as text (e.g. a Wait task's 'WaitTime'). Undoable.");
			Tool.InputSchema = BtSchema({
				{ TEXT("node_id"), TEXT("BT node id (from get_bt_nodes).") },
				{ TEXT("property"), TEXT("Property name on the runtime node.") },
				{ TEXT("value"), TEXT("Value as text.") },
				{ TEXT("asset_path"), TEXT("Optional Behavior Tree object path; defaults to the focused asset.") },
			}, { TEXT("node_id"), TEXT("property"), TEXT("value") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UBehaviorTree* BTAsset = ResolveBT(Args);
				if (!BTAsset) { return FClaudeToolResult::Error(TEXT("No Behavior Tree is focused (or pass asset_path).")); }
				UBehaviorTreeGraphNode_Root* RootEd = nullptr;
				UBehaviorTreeGraph* Graph = EnsureBtGraph(BTAsset, RootEd);
				if (!Graph) { return FClaudeToolResult::Error(TEXT("Failed to prepare the Behavior Tree graph.")); }

				FString NodeId, PropName, Value;
				Args->TryGetStringField(TEXT("node_id"), NodeId);
				Args->TryGetStringField(TEXT("property"), PropName);
				Args->TryGetStringField(TEXT("value"), Value);
				FGuid G;
				if (!FGuid::Parse(NodeId, G)) { return FClaudeToolResult::Error(TEXT("Invalid 'node_id'.")); }
				UAIGraphNode* AiNode = Cast<UAIGraphNode>(FindBtNode(Graph, G));
				if (!AiNode || !AiNode->NodeInstance) { return FClaudeToolResult::Error(TEXT("Node has no runtime instance.")); }
				UObject* Inst = AiNode->NodeInstance;
				FProperty* Prop = Inst->GetClass()->FindPropertyByName(FName(*PropName));
				if (!Prop) { return FClaudeToolResult::Error(FString::Printf(TEXT("Property '%s' not found on '%s'."), *PropName, *Inst->GetClass()->GetName())); }

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: set BT node property")));
				Inst->Modify();
				const TCHAR* Result = Prop->ImportText_Direct(*Value, Prop->ContainerPtrToValuePtr<void>(Inst), Inst, PPF_None);
				PersistBt(Graph, BTAsset);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), Result != nullptr);
				return FClaudeToolResult::Ok(BtSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// add_blackboard_key
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("add_blackboard_key");
			Tool.Category = TEXT("bt");
			Tool.Description = TEXT("Add a key to a Blackboard (the focused Blackboard, the focused Behavior Tree's Blackboard, or asset_path). type: bool/int/float/vector/object/name/string/rotator. Undoable.");
			Tool.InputSchema = BtSchema({
				{ TEXT("key_name"), TEXT("New key name.") },
				{ TEXT("type"), TEXT("Key type: bool/int/float/vector/object/name/string/rotator.") },
				{ TEXT("asset_path"), TEXT("Optional Blackboard object path.") },
			}, { TEXT("key_name"), TEXT("type") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UBlackboardData* BB = ResolveBlackboard(Args);
				if (!BB) { return FClaudeToolResult::Error(TEXT("No Blackboard found (focus a Blackboard/Behavior Tree, or pass asset_path).")); }
				FString KeyName, TypeStr;
				Args->TryGetStringField(TEXT("key_name"), KeyName);
				Args->TryGetStringField(TEXT("type"), TypeStr);
				if (KeyName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'key_name'.")); }
				UClass* KeyTypeClass = ResolveBlackboardKeyType(TypeStr);
				if (!KeyTypeClass) { return FClaudeToolResult::Error(FString::Printf(TEXT("Unsupported key type '%s'."), *TypeStr)); }
				for (const FBlackboardEntry& Existing : BB->Keys)
				{
					if (Existing.EntryName == FName(*KeyName)) { return FClaudeToolResult::Error(FString::Printf(TEXT("Blackboard key '%s' already exists."), *KeyName)); }
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add blackboard key")));
				BB->Modify();
				FBlackboardEntry Entry;
				Entry.EntryName = FName(*KeyName);
				Entry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyTypeClass);
				BB->Keys.Add(Entry);
				BB->MarkPackageDirty();

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetNumberField(TEXT("keys"), BB->Keys.Num());
				return FClaudeToolResult::Ok(BtSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}
	}
}
