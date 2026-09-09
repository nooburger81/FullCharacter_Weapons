// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeSelfTest.cpp
// Console command "ClaudeAssistant.SelfTest": drives every tool through the
// registry with realistic arguments and logs PASS/FAIL/SKIP for each, so the
// whole surface can be verified headlessly without any manual testing:
//   UnrealEditor-Cmd <proj> -ExecCmds="ClaudeAssistant.SelfTest" -unattended -nullrhi

#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantFocusTracker.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogClaudeSelfTest, Log, All);

namespace
{
	int32 GPass = 0;
	int32 GFail = 0;
	int32 GSkip = 0;

	TSharedRef<FJsonObject> J(const TArray<TPair<FString, FString>>& Pairs)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& P : Pairs) { O->SetStringField(P.Key, P.Value); }
		return O;
	}

	TSharedPtr<FJsonObject> Parse(const FString& Content)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	FString Field(const FClaudeToolResult& R, const FString& Name)
	{
		if (TSharedPtr<FJsonObject> Obj = Parse(R.Content))
		{
			FString V;
			Obj->TryGetStringField(Name, V);
			return V;
		}
		return FString();
	}

	// First element's <field> from an array field of the result (e.g. nodes[0].id).
	FString ArrayFirst(const FClaudeToolResult& R, const FString& ArrayName, const FString& InnerField)
	{
		if (TSharedPtr<FJsonObject> Obj = Parse(R.Content))
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Obj->TryGetArrayField(ArrayName, Arr) && Arr && Arr->Num() > 0)
			{
				if (const TSharedPtr<FJsonObject> First = (*Arr)[0]->AsObject())
				{
					FString V;
					First->TryGetStringField(InnerField, V);
					return V;
				}
			}
		}
		return FString();
	}

	FClaudeToolResult Call(const FString& Name, const TSharedPtr<FJsonObject>& Args)
	{
		const FClaudeTool* Tool = FClaudeToolRegistry::Get().FindTool(Name);
		if (!Tool)
		{
			++GFail;
			UE_LOG(LogClaudeSelfTest, Error, TEXT("[SELFTEST] MISSING  %s"), *Name);
			return FClaudeToolResult::Error(TEXT("missing tool"));
		}
		// Destructive tools run too: their confirmation gates auto-approve in unattended mode.
		const FClaudeToolResult R = FClaudeToolRegistry::Get().ExecuteTool(Name, Args);
		if (R.bSuccess)
		{
			++GPass;
			UE_LOG(LogClaudeSelfTest, Log, TEXT("[SELFTEST] PASS     %s"), *Name);
		}
		else
		{
			++GFail;
			UE_LOG(LogClaudeSelfTest, Error, TEXT("[SELFTEST] FAIL     %s : %s"), *Name, *R.Content);
		}
		return R;
	}

	void Focus(const FString& ObjectPath)
	{
		UObject* Obj = LoadObject<UObject>(nullptr, *ObjectPath);
		FClaudeAssistantFocusTracker::Get().SetActiveAssetForTest(Obj);
		if (!Obj) { UE_LOG(LogClaudeSelfTest, Warning, TEXT("[SELFTEST] focus target not found: %s"), *ObjectPath); }
	}

	void RunSelfTest()
	{
		GPass = GFail = GSkip = 0;
		const FString P = TEXT("/Game/__ClaudeSelfTest");
		UE_LOG(LogClaudeSelfTest, Log, TEXT("[SELFTEST] ================= BEGIN (%d tools registered) ================="),
			FClaudeToolRegistry::Get().Num());

		// ---- asset creation ----
		Call(TEXT("create_struct"), J({ {TEXT("name"), TEXT("ST_SelfTest")}, {TEXT("path"), P} }));
		Call(TEXT("create_enum"), J({ {TEXT("name"), TEXT("EN_SelfTest")}, {TEXT("path"), P} }));
		Call(TEXT("create_blueprint"), J({ {TEXT("name"), TEXT("BP_SelfTest")}, {TEXT("parent_class"), TEXT("Actor")}, {TEXT("path"), P} }));
		Call(TEXT("create_widget_blueprint"), J({ {TEXT("name"), TEXT("WBP_SelfTest")}, {TEXT("path"), P} }));
		Call(TEXT("create_behavior_tree"), J({ {TEXT("name"), TEXT("BT_SelfTest")}, {TEXT("path"), P} }));
		Call(TEXT("create_blackboard"), J({ {TEXT("name"), TEXT("BB_SelfTest")}, {TEXT("path"), P} }));
		Call(TEXT("create_data_table"), J({ {TEXT("name"), TEXT("DT_SelfTest")}, {TEXT("struct_name"), TEXT("Rotator")}, {TEXT("path"), P} }));
		Call(TEXT("create_material"), J({ {TEXT("name"), TEXT("M_SelfTest")}, {TEXT("path"), P} }));

		// ---- struct / enum content ----
		Focus(P + TEXT("/ST_SelfTest.ST_SelfTest"));
		Call(TEXT("add_struct_member"), J({ {TEXT("type"), TEXT("float")} }));
		Focus(P + TEXT("/EN_SelfTest.EN_SelfTest"));
		Call(TEXT("add_enum_entry"), J({ {TEXT("display_name"), TEXT("Alpha")} }));

		// ---- Blueprint flow ----
		Focus(P + TEXT("/BP_SelfTest.BP_SelfTest"));
		Call(TEXT("get_active_context"), J({}));
		Call(TEXT("add_variable"), J({ {TEXT("name"), TEXT("Health")}, {TEXT("type"), TEXT("int")} }));
		Call(TEXT("add_variable"), J({ {TEXT("name"), TEXT("Temp")}, {TEXT("type"), TEXT("float")} }));
		Call(TEXT("rename_variable"), J({ {TEXT("old_name"), TEXT("Temp")}, {TEXT("new_name"), TEXT("Speed")} }));
		Call(TEXT("list_blueprint_variables"), J({}));
		Call(TEXT("list_blueprint_functions"), J({}));
		Call(TEXT("set_variable_config"), J({ {TEXT("variable"), TEXT("Health")}, {TEXT("category"), TEXT("Stats")} }));
		Call(TEXT("add_component"), J({ {TEXT("component_class"), TEXT("StaticMeshComponent")}, {TEXT("name"), TEXT("Mesh")} }));
		const FClaudeToolResult CommentR = Call(TEXT("add_comment"), J({ {TEXT("text"), TEXT("SelfTest")} }));
		const FString CommentId = Field(CommentR, TEXT("node_id"));
		Call(TEXT("add_function"), J({ {TEXT("name"), TEXT("MyFunc")} }));
		Call(TEXT("add_function_parameter"), J({ {TEXT("function_name"), TEXT("MyFunc")}, {TEXT("param_name"), TEXT("Delta")}, {TEXT("type"), TEXT("float")} }));

		Call(TEXT("add_event"), J({ {TEXT("name"), TEXT("MyCustomEvent")}, {TEXT("custom"), TEXT("true")} }));
		const FClaudeToolResult EventR = Call(TEXT("add_event"), J({ {TEXT("name"), TEXT("BeginPlay")} }));
		const FString EventId = Field(EventR, TEXT("node_id"));
		const FClaudeToolResult CallNodeR = Call(TEXT("add_function_call_node"), J({ {TEXT("function_name"), TEXT("PrintString")}, {TEXT("class_name"), TEXT("KismetSystemLibrary")} }));
		const FString CallNodeId = Field(CallNodeR, TEXT("node_id"));
		Call(TEXT("add_variable_node"), J({ {TEXT("variable_name"), TEXT("Health")} }));
		Call(TEXT("add_branch"), J({}));
		Call(TEXT("add_sequence"), J({}));
		Call(TEXT("add_cast_node"), J({ {TEXT("target_class"), TEXT("Pawn")} }));
		Call(TEXT("add_make_struct"), J({ {TEXT("struct_name"), TEXT("Vector")} }));
		Call(TEXT("add_break_struct"), J({ {TEXT("struct_name"), TEXT("Vector")} }));
		Call(TEXT("add_loop"), J({ {TEXT("kind"), TEXT("for")} }));

		const FClaudeToolResult NodesR = Call(TEXT("get_graph_nodes"), J({}));
		const FString FirstNodeId = ArrayFirst(NodesR, TEXT("nodes"), TEXT("id"));
		if (!FirstNodeId.IsEmpty()) { Call(TEXT("get_node_details"), J({ {TEXT("node_id"), FirstNodeId} })); }

		if (!EventId.IsEmpty() && !CallNodeId.IsEmpty())
		{
			Call(TEXT("connect_pins"), J({ {TEXT("from_node_id"), EventId}, {TEXT("from_pin"), TEXT("then")}, {TEXT("to_node_id"), CallNodeId}, {TEXT("to_pin"), TEXT("execute")} }));
		}
		if (!CallNodeId.IsEmpty())
		{
			Call(TEXT("set_pin_default"), J({ {TEXT("node_id"), CallNodeId}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("SelfTest")} }));
			Call(TEXT("disconnect_pin"), J({ {TEXT("node_id"), CallNodeId}, {TEXT("pin"), TEXT("execute")} }));
		}

		// universal node database
		const FClaudeToolResult SearchR = Call(TEXT("search_node_actions"), J({ {TEXT("query"), TEXT("delay")} }));
		const FString ActionKey = ArrayFirst(SearchR, TEXT("actions"), TEXT("key"));
		if (!ActionKey.IsEmpty()) { Call(TEXT("add_node_by_action"), J({ {TEXT("key"), ActionKey} })); }

		Call(TEXT("set_active_graph"), J({ {TEXT("graph"), TEXT("MyFunc")} }));
		Call(TEXT("set_active_graph"), J({ {TEXT("graph"), TEXT("event")} }));

		// destructive BP tools (auto-approved in unattended mode)
		if (!CommentId.IsEmpty())
		{
			Call(TEXT("set_node_property"), J({ {TEXT("node_id"), CommentId}, {TEXT("property"), TEXT("CommentColor")}, {TEXT("value"), TEXT("(R=1.0,G=0.0,B=0.0,A=1.0)")} }));
			Call(TEXT("delete_node"), J({ {TEXT("node_id"), CommentId} }));
		}
		Call(TEXT("delete_variable"), J({ {TEXT("name"), TEXT("Speed")} }));
		Call(TEXT("add_variable"), J({ {TEXT("name"), TEXT("Unused1")}, {TEXT("type"), TEXT("bool")} }));
		Call(TEXT("delete_unused_variables"), J({}));
		Call(TEXT("run_python"), J({ {TEXT("script"), TEXT("import unreal\nunreal.log('selftest python ok')")} }));

		Call(TEXT("compile_blueprint"), J({}));

		// ---- DataTable ----
		Focus(P + TEXT("/DT_SelfTest.DT_SelfTest"));
		Call(TEXT("get_data_table_struct"), J({}));
		Call(TEXT("add_data_table_row"), J({ {TEXT("row_name"), TEXT("Row1")} }));
		{
			// update_data_table_row wants an object 'values' field: build args by hand.
			TSharedRef<FJsonObject> UpdArgs = MakeShared<FJsonObject>();
			UpdArgs->SetStringField(TEXT("row_name"), TEXT("Row1"));
			UpdArgs->SetObjectField(TEXT("values"), MakeShared<FJsonObject>());
			Call(TEXT("update_data_table_row"), UpdArgs);
		}
		{
			// add_data_table_rows_batch: rows = [{row_name, values}]
			TSharedRef<FJsonObject> RowObj = MakeShared<FJsonObject>();
			RowObj->SetStringField(TEXT("row_name"), TEXT("Row2"));
			RowObj->SetObjectField(TEXT("values"), MakeShared<FJsonObject>());
			TArray<TSharedPtr<FJsonValue>> Rows;
			Rows.Add(MakeShared<FJsonValueObject>(RowObj));
			TSharedRef<FJsonObject> BatchArgs = MakeShared<FJsonObject>();
			BatchArgs->SetArrayField(TEXT("rows"), Rows);
			Call(TEXT("add_data_table_rows_batch"), BatchArgs);
		}
		Call(TEXT("delete_data_table_row"), J({ {TEXT("row_name"), TEXT("Row2")} }));

		// ---- Material ----
		Focus(P + TEXT("/M_SelfTest.M_SelfTest"));
		const FClaudeToolResult ExprR = Call(TEXT("create_material_expression"), J({ {TEXT("class"), TEXT("Constant3Vector")} }));
		const FString ExprId = Field(ExprR, TEXT("expression"));
		const FClaudeToolResult Expr2R = Call(TEXT("create_material_expression"), J({ {TEXT("class"), TEXT("Multiply")} }));
		const FString Expr2Id = Field(Expr2R, TEXT("expression"));
		if (!ExprId.IsEmpty())
		{
			Call(TEXT("set_material_expression_property"), J({ {TEXT("expression"), ExprId}, {TEXT("property"), TEXT("Constant")}, {TEXT("value"), TEXT("(R=1.0,G=0.0,B=0.0)")} }));
			Call(TEXT("connect_to_material_property"), J({ {TEXT("expression"), ExprId}, {TEXT("property"), TEXT("base_color")} }));
			if (!Expr2Id.IsEmpty())
			{
				Call(TEXT("connect_material_expressions"), J({ {TEXT("from"), ExprId}, {TEXT("to"), Expr2Id}, {TEXT("to_input"), TEXT("A")} }));
			}
		}
		Call(TEXT("recompile_material"), J({}));

		// ---- Widget ----
		Focus(P + TEXT("/WBP_SelfTest.WBP_SelfTest"));
		const FClaudeToolResult RootR = Call(TEXT("add_widget"), J({ {TEXT("class_name"), TEXT("VerticalBox")}, {TEXT("name"), TEXT("Root")} }));
		Call(TEXT("add_widget"), J({ {TEXT("class_name"), TEXT("TextBlock")}, {TEXT("name"), TEXT("Label")}, {TEXT("parent_name"), TEXT("Root")} }));
		Call(TEXT("set_widget_text"), J({ {TEXT("widget_name"), TEXT("Label")}, {TEXT("text"), TEXT("Hi")} }));
		Call(TEXT("list_widgets"), J({}));
		Call(TEXT("set_widget_property"), J({ {TEXT("widget_name"), TEXT("Label")}, {TEXT("property"), TEXT("ToolTipText")}, {TEXT("value"), TEXT("tip")} }));
		Call(TEXT("remove_widget"), J({ {TEXT("widget_name"), TEXT("Label")} }));

		// ---- Behavior Tree ----
		Focus(P + TEXT("/BT_SelfTest.BT_SelfTest"));
		const FClaudeToolResult CompR = Call(TEXT("add_bt_composite"), J({ {TEXT("composite_class"), TEXT("BTComposite_Selector")} }));
		const FString CompId = Field(CompR, TEXT("node_id"));
		if (!CompId.IsEmpty())
		{
			const FClaudeToolResult TaskR = Call(TEXT("add_bt_task"), J({ {TEXT("task_class"), TEXT("BTTask_Wait")}, {TEXT("parent_id"), CompId} }));
			const FString TaskId = Field(TaskR, TEXT("node_id"));
			if (!TaskId.IsEmpty()) { Call(TEXT("set_bt_node_property"), J({ {TEXT("node_id"), TaskId}, {TEXT("property"), TEXT("WaitTime")}, {TEXT("value"), TEXT("2.0")} })); }
			Call(TEXT("add_bt_decorator"), J({ {TEXT("decorator_class"), TEXT("BTDecorator_TimeLimit")}, {TEXT("parent_id"), CompId} }));
		}
		Call(TEXT("get_bt_nodes"), J({}));
		Focus(P + TEXT("/BB_SelfTest.BB_SelfTest"));
		Call(TEXT("add_blackboard_key"), J({ {TEXT("key_name"), TEXT("Target")}, {TEXT("type"), TEXT("object")} }));

		// ---- Level / Actor ----
		const FClaudeToolResult SpawnR = Call(TEXT("spawn_actor"), J({ {TEXT("class"), TEXT("StaticMeshActor")}, {TEXT("name"), TEXT("SelfTestActor")} }));
		const FString ActorLabel = Field(SpawnR, TEXT("actor"));
		Call(TEXT("list_actors"), J({}));
		if (!ActorLabel.IsEmpty())
		{
			Call(TEXT("set_actor_transform"), J({ {TEXT("actor"), ActorLabel}, {TEXT("location"), TEXT("100,0,0")} }));
			Call(TEXT("set_actor_property"), J({ {TEXT("actor"), ActorLabel}, {TEXT("property"), TEXT("bHidden")}, {TEXT("value"), TEXT("true")} }));
			Call(TEXT("delete_actor"), J({ {TEXT("actor"), ActorLabel} }));
		}

		// ---- Asset management ----
		Call(TEXT("find_assets"), J({ {TEXT("path"), P} }));
		Call(TEXT("duplicate_asset"), J({ {TEXT("source"), P + TEXT("/BP_SelfTest.BP_SelfTest")}, {TEXT("new_name"), TEXT("BP_SelfTest_Copy")} }));
		Call(TEXT("rename_asset"), J({ {TEXT("source"), P + TEXT("/BP_SelfTest_Copy.BP_SelfTest_Copy")}, {TEXT("new_name"), TEXT("BP_SelfTest_Copy2")} }));
		Call(TEXT("delete_asset"), J({ {TEXT("path"), P + TEXT("/BP_SelfTest_Copy2.BP_SelfTest_Copy2")} }));

		// ---- Runtime ----
		Call(TEXT("is_pie_active"), J({}));

		UE_LOG(LogClaudeSelfTest, Log, TEXT("[SELFTEST] ================= RESULT: %d PASS, %d FAIL, %d SKIP ================="), GPass, GFail, GSkip);
	}

	FAutoConsoleCommand GClaudeSelfTestCmd(
		TEXT("ClaudeAssistant.SelfTest"),
		TEXT("Drives every Claude Assistant tool through the registry and logs PASS/FAIL/SKIP."),
		FConsoleCommandDelegate::CreateStatic(&RunSelfTest));
}
