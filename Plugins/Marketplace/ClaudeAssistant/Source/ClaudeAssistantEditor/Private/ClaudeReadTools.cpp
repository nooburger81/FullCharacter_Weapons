// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeReadTools.cpp

#include "ClaudeReadTools.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantFocusTracker.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Object.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

namespace
{
	/** A minimal JSON Schema for a tool that takes no arguments. */
	TSharedPtr<FJsonObject> MakeNoArgSchema()
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		return Schema;
	}

	FString SerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}
}

void ClaudeReadTools::RegisterAll(FClaudeToolRegistry& Registry)
{
	// -------------------------------------------------------------------------
	// get_active_context : which asset the user currently has open/focused.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("get_active_context");
		Tool.Category = TEXT("context");
		Tool.Description = TEXT(
			"Returns the type, name and path of the asset the user currently has open or focused "
			"in the Unreal editor (Blueprint, DataTable, Material, or other). Call this first to "
			"know what the user is working on before answering \"this\" or \"here\" style requests.");
		Tool.InputSchema = MakeNoArgSchema();
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& /*Args*/) -> FClaudeToolResult
		{
			UObject* Asset = FClaudeAssistantFocusTracker::Get().GetActiveAsset();

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			if (!Asset)
			{
				Out->SetBoolField(TEXT("active"), false);
				Out->SetStringField(TEXT("message"), TEXT("No asset editor is currently focused."));
				return FClaudeToolResult::Ok(SerializeJson(Out));
			}

			Out->SetBoolField(TEXT("active"), true);
			Out->SetStringField(TEXT("name"), Asset->GetName());
			Out->SetStringField(TEXT("path"), Asset->GetPathName());
			Out->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
			Out->SetStringField(TEXT("summary"), FClaudeAssistantFocusTracker::Get().GetActiveContextString());
			return FClaudeToolResult::Ok(SerializeJson(Out));
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// list_blueprint_variables : member variables of the focused Blueprint.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("list_blueprint_variables");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT(
			"Lists the member variables of the Blueprint the user currently has open: "
			"name, type, category and default value. Requires a Blueprint asset to be focused.");
		Tool.InputSchema = MakeNoArgSchema();
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& /*Args*/) -> FClaudeToolResult
		{
			UBlueprint* BP = Cast<UBlueprint>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
			if (!BP)
			{
				return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused. Open a Blueprint asset first."));
			}

			TArray<TSharedPtr<FJsonValue>> Vars;
			for (const FBPVariableDescription& Var : BP->NewVariables)
			{
				TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
				V->SetStringField(TEXT("name"), Var.VarName.ToString());
				V->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(Var.VarType).ToString());
				V->SetStringField(TEXT("category"), Var.Category.ToString());
				V->SetStringField(TEXT("default"), Var.DefaultValue);
				Vars.Add(MakeShared<FJsonValueObject>(V));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("blueprint"), BP->GetName());
			Out->SetNumberField(TEXT("count"), Vars.Num());
			Out->SetArrayField(TEXT("variables"), Vars);
			return FClaudeToolResult::Ok(SerializeJson(Out));
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// list_blueprint_functions : function / macro / event graphs of the Blueprint.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("list_blueprint_functions");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT(
			"Lists the function graphs, macro graphs and event graphs of the focused Blueprint, "
			"each with its name and node count. Requires a Blueprint asset to be focused.");
		Tool.InputSchema = MakeNoArgSchema();
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& /*Args*/) -> FClaudeToolResult
		{
			UBlueprint* BP = Cast<UBlueprint>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
			if (!BP)
			{
				return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused. Open a Blueprint asset first."));
			}

			TArray<TSharedPtr<FJsonValue>> Graphs;
			auto AddGraphs = [&Graphs](const TArray<UEdGraph*>& InGraphs, const TCHAR* Kind)
			{
				for (const UEdGraph* Graph : InGraphs)
				{
					if (!Graph)
					{
						continue;
					}
					TSharedRef<FJsonObject> G = MakeShared<FJsonObject>();
					G->SetStringField(TEXT("name"), Graph->GetName());
					G->SetStringField(TEXT("kind"), Kind);
					G->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
					Graphs.Add(MakeShared<FJsonValueObject>(G));
				}
			};
			AddGraphs(BP->FunctionGraphs, TEXT("function"));
			AddGraphs(BP->MacroGraphs, TEXT("macro"));
			AddGraphs(BP->UbergraphPages, TEXT("event"));

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("blueprint"), BP->GetName());
			Out->SetNumberField(TEXT("count"), Graphs.Num());
			Out->SetArrayField(TEXT("graphs"), Graphs);
			return FClaudeToolResult::Ok(SerializeJson(Out));
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// get_data_table_struct : row-struct schema of the focused DataTable.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("get_data_table_struct");
		Tool.Category = TEXT("datatable");
		Tool.Description = TEXT(
			"Returns the row-struct schema of the DataTable the user currently has open: each "
			"field's name and C++ type. Call this before generating or editing rows so types match. "
			"Requires a DataTable asset to be focused.");
		Tool.InputSchema = MakeNoArgSchema();
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& /*Args*/) -> FClaudeToolResult
		{
			UDataTable* DataTable = Cast<UDataTable>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
			if (!DataTable)
			{
				return FClaudeToolResult::Error(TEXT("No DataTable is currently focused. Open a DataTable asset first."));
			}
			const UScriptStruct* RowStruct = DataTable->RowStruct;
			if (!RowStruct)
			{
				return FClaudeToolResult::Error(TEXT("The focused DataTable has no row struct assigned."));
			}

			TArray<TSharedPtr<FJsonValue>> Fields;
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				const FProperty* Property = *It;
				TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("name"), Property->GetName());
				// Include the extended type text so container element types survive
				// (e.g. TArray<FVector> instead of a bare "TArray").
				FString ExtendedType;
				const FString BaseType = Property->GetCPPType(&ExtendedType);
				F->SetStringField(TEXT("type"), BaseType + ExtendedType);
				Fields.Add(MakeShared<FJsonValueObject>(F));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("data_table"), DataTable->GetName());
			Out->SetStringField(TEXT("row_struct"), RowStruct->GetName());
			Out->SetNumberField(TEXT("field_count"), Fields.Num());
			Out->SetArrayField(TEXT("fields"), Fields);
			return FClaudeToolResult::Ok(SerializeJson(Out));
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// get_graph_nodes : nodes of a graph in the focused Blueprint.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("get_graph_nodes");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("List the nodes of a graph in the focused Blueprint (id, class, title, position, pin count). Defaults to the main Event Graph; pass graph_name to target a specific function/graph.");
		{
			TSharedRef<FJsonObject> GraphNameProp = MakeShared<FJsonObject>();
			GraphNameProp->SetStringField(TEXT("type"), TEXT("string"));
			GraphNameProp->SetStringField(TEXT("description"), TEXT("Optional graph name; defaults to the Event Graph."));
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			Props->SetObjectField(TEXT("graph_name"), GraphNameProp);
			TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), Props);
			Tool.InputSchema = Schema;
		}
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = Cast<UBlueprint>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString GraphName;
			if (Args.IsValid()) { Args->TryGetStringField(TEXT("graph_name"), GraphName); }

			UEdGraph* Target = nullptr;
			auto Search = [&Target, &GraphName](const TArray<UEdGraph*>& Graphs)
			{
				if (Target) { return; }
				for (UEdGraph* G : Graphs)
				{
					if (G && (GraphName.IsEmpty() || G->GetName() == GraphName)) { Target = G; return; }
				}
			};
			if (!GraphName.IsEmpty())
			{
				Search(BP->FunctionGraphs); Search(BP->MacroGraphs); Search(BP->UbergraphPages);
			}
			else
			{
				Search(BP->UbergraphPages); Search(BP->FunctionGraphs);
			}
			if (!Target) { return FClaudeToolResult::Error(TEXT("Graph not found.")); }

			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const UEdGraphNode* Node : Target->Nodes)
			{
				if (!Node) { continue; }
				TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
				N->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
				N->SetStringField(TEXT("class"), Node->GetClass() ? Node->GetClass()->GetName() : FString());
				N->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				N->SetNumberField(TEXT("x"), Node->NodePosX);
				N->SetNumberField(TEXT("y"), Node->NodePosY);
				N->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
				Nodes.Add(MakeShared<FJsonValueObject>(N));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("graph"), Target->GetName());
			Out->SetNumberField(TEXT("count"), Nodes.Num());
			Out->SetArrayField(TEXT("nodes"), Nodes);
			return FClaudeToolResult::Ok(SerializeJson(Out));
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// get_node_details : pins and connections of one node (by id).
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("get_node_details");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Return the pins and connections of a single node in the focused Blueprint, identified by its node id (from get_graph_nodes).");
		{
			TSharedRef<FJsonObject> IdProp = MakeShared<FJsonObject>();
			IdProp->SetStringField(TEXT("type"), TEXT("string"));
			IdProp->SetStringField(TEXT("description"), TEXT("Node id (GUID) from get_graph_nodes."));
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			Props->SetObjectField(TEXT("node_id"), IdProp);
			TArray<TSharedPtr<FJsonValue>> Req = { MakeShared<FJsonValueString>(TEXT("node_id")) };
			TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), Props);
			Schema->SetArrayField(TEXT("required"), Req);
			Tool.InputSchema = Schema;
		}
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = Cast<UBlueprint>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString NodeId;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			FGuid TargetGuid;
			if (NodeId.IsEmpty() || !FGuid::Parse(NodeId, TargetGuid)) { return FClaudeToolResult::Error(TEXT("Missing or invalid 'node_id'.")); }

			const UEdGraphNode* Found = nullptr;
			auto Scan = [&Found, &TargetGuid](const TArray<UEdGraph*>& Graphs)
			{
				if (Found) { return; }
				for (const UEdGraph* G : Graphs)
				{
					if (!G) { continue; }
					for (const UEdGraphNode* Nd : G->Nodes)
					{
						if (Nd && Nd->NodeGuid == TargetGuid) { Found = Nd; return; }
					}
				}
			};
			Scan(BP->UbergraphPages); Scan(BP->FunctionGraphs); Scan(BP->MacroGraphs);
			if (!Found) { return FClaudeToolResult::Error(TEXT("Node not found.")); }

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Found->Pins)
			{
				if (!Pin) { continue; }
				TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"), Pin->PinName.ToString());
				P->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
				P->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString());
				P->SetStringField(TEXT("default"), Pin->DefaultValue);

				TArray<TSharedPtr<FJsonValue>> Links;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNodeUnchecked()) { continue; }
					TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
					L->SetStringField(TEXT("node_id"), Linked->GetOwningNodeUnchecked()->NodeGuid.ToString());
					L->SetStringField(TEXT("pin"), Linked->PinName.ToString());
					Links.Add(MakeShared<FJsonValueObject>(L));
				}
				P->SetArrayField(TEXT("links"), Links);
				Pins.Add(MakeShared<FJsonValueObject>(P));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("id"), NodeId);
			Out->SetStringField(TEXT("title"), Found->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Out->SetStringField(TEXT("class"), Found->GetClass() ? Found->GetClass()->GetName() : FString());
			Out->SetArrayField(TEXT("pins"), Pins);
			return FClaudeToolResult::Ok(SerializeJson(Out));
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}
}
