// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeWriteTools.cpp
// Mutating tools (bIsDestructive). Dedicated Blueprint-variable tools (reliable,
// one-shot, undo-safe) plus the run_python escape hatch for the long tail.

#include "ClaudeWriteTools.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantSettings.h"
#include "ClaudeAssistantFocusTracker.h"

#include "IPythonScriptPlugin.h"
#include "Misc/MessageDialog.h"
#include "Misc/App.h"
#include "ScopedTransaction.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/Class.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "JsonObjectConverter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "UObject/UnrealType.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Runtime/Launch/Resources/Version.h"
// UUserDefinedStruct moved from Engine to CoreUObject/StructUtils in UE 5.5.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 5
#include "Engine/UserDefinedStruct.h"
#else
#include "StructUtils/UserDefinedStruct.h"
#endif
#include "Engine/UserDefinedEnum.h"
#include "UObject/TopLevelAssetPath.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "ClaudeWriteTools"

namespace
{
	FString WriteSerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	FClaudeToolResult OkJson(const TSharedRef<FJsonObject>& Obj)
	{
		return FClaudeToolResult::Ok(WriteSerializeJson(Obj));
	}

	TSharedPtr<FJsonObject> StringProp(const FString& Desc)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("string"));
		P->SetStringField(TEXT("description"), Desc);
		return P;
	}

	/** Build an object JSON schema from ordered (name, prop) pairs and a required list. */
	TSharedPtr<FJsonObject> ObjSchema(const TArray<TPair<FString, TSharedPtr<FJsonObject>>>& Props, const TArray<FString>& Required)
	{
		TSharedRef<FJsonObject> PropsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : Props)
		{
			PropsObj->SetObjectField(Pair.Key, Pair.Value.IsValid() ? Pair.Value : MakeShared<FJsonObject>());
		}
		TArray<TSharedPtr<FJsonValue>> Req;
		for (const FString& R : Required)
		{
			Req.Add(MakeShared<FJsonValueString>(R));
		}
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), PropsObj);
		Schema->SetArrayField(TEXT("required"), Req);
		return Schema;
	}

	/** Map a friendly type name to an FEdGraphPinType. Returns false for unsupported types. */
	bool BuildPinType(const FString& InType, FEdGraphPinType& Out)
	{
		Out = FEdGraphPinType();
		const FString T = InType.ToLower();

		if (T == TEXT("bool") || T == TEXT("boolean")) { Out.PinCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
		if (T == TEXT("byte")) { Out.PinCategory = UEdGraphSchema_K2::PC_Byte; return true; }
		if (T == TEXT("int") || T == TEXT("int32") || T == TEXT("integer")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int; return true; }
		if (T == TEXT("int64")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int64; return true; }
		if (T == TEXT("float") || T == TEXT("double") || T == TEXT("real"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Real;
			Out.PinSubCategory = (T == TEXT("double")) ? UEdGraphSchema_K2::PC_Double : UEdGraphSchema_K2::PC_Float;
			return true;
		}
		if (T == TEXT("string")) { Out.PinCategory = UEdGraphSchema_K2::PC_String; return true; }
		if (T == TEXT("name")) { Out.PinCategory = UEdGraphSchema_K2::PC_Name; return true; }
		if (T == TEXT("text")) { Out.PinCategory = UEdGraphSchema_K2::PC_Text; return true; }

		UScriptStruct* Struct = nullptr;
		if (T == TEXT("vector")) { Struct = TBaseStructure<FVector>::Get(); }
		else if (T == TEXT("vector2d")) { Struct = TBaseStructure<FVector2D>::Get(); }
		else if (T == TEXT("rotator")) { Struct = TBaseStructure<FRotator>::Get(); }
		else if (T == TEXT("transform")) { Struct = TBaseStructure<FTransform>::Get(); }
		else if (T == TEXT("linearcolor") || T == TEXT("color")) { Struct = TBaseStructure<FLinearColor>::Get(); }
		if (Struct)
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = Struct;
			return true;
		}
		return false;
	}

	// Node-creation tools target this graph by name; NAME_None => Event Graph.
	FName GActiveTargetGraph;

	UBlueprint* ActiveBlueprint()
	{
		return Cast<UBlueprint>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}

	UDataTable* ActiveDataTable()
	{
		return Cast<UDataTable>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}

	UUserDefinedStruct* ActiveUserStruct(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path;
		if (Args.IsValid()) { Args->TryGetStringField(TEXT("asset_path"), Path); }
		if (!Path.IsEmpty()) { return LoadObject<UUserDefinedStruct>(nullptr, *Path); }
		return Cast<UUserDefinedStruct>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}

	UUserDefinedEnum* ActiveUserEnum(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path;
		if (Args.IsValid()) { Args->TryGetStringField(TEXT("asset_path"), Path); }
		if (!Path.IsEmpty()) { return LoadObject<UUserDefinedEnum>(nullptr, *Path); }
		return Cast<UUserDefinedEnum>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}

	// Find a node by its GUID across all graphs of a Blueprint.
	UEdGraphNode* FindBpNodeByGuid(UBlueprint* BP, const FGuid& Guid)
	{
		if (!BP) { return nullptr; }
		auto Scan = [&Guid](const TArray<UEdGraph*>& Graphs) -> UEdGraphNode*
		{
			for (UEdGraph* G : Graphs)
			{
				if (!G) { continue; }
				for (UEdGraphNode* N : G->Nodes)
				{
					if (N && N->NodeGuid == Guid) { return N; }
				}
			}
			return nullptr;
		};
		if (UEdGraphNode* N = Scan(BP->UbergraphPages)) { return N; }
		if (UEdGraphNode* N = Scan(BP->FunctionGraphs)) { return N; }
		if (UEdGraphNode* N = Scan(BP->MacroGraphs)) { return N; }
		return nullptr;
	}

	TSharedPtr<FJsonObject> TypedProp(const FString& JsonType, const FString& Desc)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), JsonType);
		P->SetStringField(TEXT("description"), Desc);
		return P;
	}

	bool ConfirmDestructive(const FString& Summary)
	{
		if (FApp::IsUnattended())
		{
			// Automation context (self-test / CI): no user to ask, auto-approve.
			return true;
		}
		const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
		if (Settings && !Settings->bConfirmRunPython)
		{
			// Reuse the same "confirm before mutating" preference toggle.
			return true;
		}
		const FText Message = FText::FromString(FString::Printf(TEXT("Claude wants to perform a destructive action:\n\n%s\n\nAllow?"), *Summary));
		return FMessageDialog::Open(EAppMsgType::YesNo, Message) == EAppReturnType::Yes;
	}
}

UEdGraph* ClaudeWriteTools::ResolveTargetGraph(UBlueprint* BP)
{
	if (!BP) { return nullptr; }
	if (!GActiveTargetGraph.IsNone())
	{
		auto Find = [](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* G : Graphs) { if (G && G->GetFName() == GActiveTargetGraph) { return G; } }
			return nullptr;
		};
		if (UEdGraph* G = Find(BP->FunctionGraphs)) { return G; }
		if (UEdGraph* G = Find(BP->UbergraphPages)) { return G; }
		if (UEdGraph* G = Find(BP->MacroGraphs)) { return G; }
	}
	return BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
}

void ClaudeWriteTools::SetActiveGraphName(FName GraphName)
{
	GActiveTargetGraph = GraphName;
}

void ClaudeWriteTools::RegisterAll(FClaudeToolRegistry& Registry)
{
	// -------------------------------------------------------------------------
	// add_variable
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_variable");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a member variable to the currently focused Blueprint. Reliable one-shot (prefer this over run_python for variables). Supported types: bool, byte, int, int64, float, double, string, name, text, vector, vector2d, rotator, transform, linearcolor. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("name"), StringProp(TEXT("Variable name.")) },
			{ TEXT("type"), StringProp(TEXT("Variable type (e.g. float, bool, int, vector).")) },
			{ TEXT("category"), StringProp(TEXT("Optional variable category.")) },
			{ TEXT("default"), StringProp(TEXT("Optional default value (as text).")) },
		}, { TEXT("name"), TEXT("type") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString Name, Type, Category, DefaultValue;
			Args->TryGetStringField(TEXT("name"), Name);
			Args->TryGetStringField(TEXT("type"), Type);
			Args->TryGetStringField(TEXT("category"), Category);
			Args->TryGetStringField(TEXT("default"), DefaultValue);
			if (Name.IsEmpty() || Type.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing required 'name' or 'type'.")); }

			FEdGraphPinType PinType;
			if (!BuildPinType(Type, PinType))
			{
				return FClaudeToolResult::Error(FString::Printf(TEXT("Unsupported type '%s'."), *Type));
			}

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: add variable %s"), *Name)));
			const bool bOk = FBlueprintEditorUtils::AddMemberVariable(BP, FName(*Name), PinType, DefaultValue);
			if (!bOk) { return FClaudeToolResult::Error(FString::Printf(TEXT("Failed to add '%s' (a variable with that name may already exist)."), *Name)); }
			if (!Category.IsEmpty())
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, FName(*Name), nullptr, FText::FromString(Category));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("variable"), Name);
			Out->SetStringField(TEXT("type"), Type);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// rename_variable
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("rename_variable");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Rename a member variable of the focused Blueprint, updating all references. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("old_name"), StringProp(TEXT("Current variable name.")) },
			{ TEXT("new_name"), StringProp(TEXT("New variable name.")) },
		}, { TEXT("old_name"), TEXT("new_name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString OldName, NewName;
			Args->TryGetStringField(TEXT("old_name"), OldName);
			Args->TryGetStringField(TEXT("new_name"), NewName);
			if (OldName.IsEmpty() || NewName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'old_name' or 'new_name'.")); }

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: rename variable %s"), *OldName)));
			FBlueprintEditorUtils::RenameMemberVariable(BP, FName(*OldName), FName(*NewName));

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("old_name"), OldName);
			Out->SetStringField(TEXT("new_name"), NewName);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// delete_variable (destructive)
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("delete_variable");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Delete a member variable from the focused Blueprint (graph references become broken). Asks for confirmation. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("name"), StringProp(TEXT("Variable name to delete.")) },
		}, { TEXT("name") });
		Tool.bIsDestructive = true;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString Name;
			Args->TryGetStringField(TEXT("name"), Name);
			if (Name.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'name'.")); }

			if (!ConfirmDestructive(FString::Printf(TEXT("Delete variable '%s' from Blueprint '%s'."), *Name, *BP->GetName())))
			{
				return FClaudeToolResult::Error(TEXT("User denied the deletion."));
			}

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: delete variable %s"), *Name)));
			FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*Name));

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("deleted"), Name);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// delete_unused_variables (destructive, batch)
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("delete_unused_variables");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Delete every member variable of the focused Blueprint that is not referenced by any graph node. Shows the list and asks for confirmation. Undoable.");
		Tool.InputSchema = ObjSchema({}, {});
		Tool.bIsDestructive = true;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& /*Args*/) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			TArray<FName> Unused;
			for (const FBPVariableDescription& Var : BP->NewVariables)
			{
				if (!FBlueprintEditorUtils::IsVariableUsed(BP, Var.VarName))
				{
					Unused.Add(Var.VarName);
				}
			}

			if (Unused.Num() == 0)
			{
				TSharedRef<FJsonObject> None = MakeShared<FJsonObject>();
				None->SetBoolField(TEXT("success"), true);
				None->SetNumberField(TEXT("removed"), 0);
				return OkJson(None);
			}

			FString List;
			for (const FName& N : Unused) { List += TEXT("  - ") + N.ToString() + TEXT("\n"); }
			if (!ConfirmDestructive(FString::Printf(TEXT("Delete %d unused variable(s) from '%s':\n%s"), Unused.Num(), *BP->GetName(), *List)))
			{
				return FClaudeToolResult::Error(TEXT("User denied the batch deletion."));
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: delete unused variables")));
			TArray<TSharedPtr<FJsonValue>> Removed;
			for (const FName& N : Unused)
			{
				FBlueprintEditorUtils::RemoveMemberVariable(BP, N);
				Removed.Add(MakeShared<FJsonValueString>(N.ToString()));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetNumberField(TEXT("removed"), Unused.Num());
			Out->SetArrayField(TEXT("names"), Removed);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_data_table_row
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_data_table_row");
		Tool.Category = TEXT("datatable");
		Tool.Description = TEXT("Add a row to the focused DataTable. Call get_data_table_struct first so 'values' keys match the row struct field names. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("row_name"), StringProp(TEXT("Unique row name.")) },
			{ TEXT("values"), TypedProp(TEXT("object"), TEXT("Field name -> value map matching the row struct.")) },
		}, { TEXT("row_name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UDataTable* DT = ActiveDataTable();
			if (!DT || !DT->RowStruct) { return FClaudeToolResult::Error(TEXT("No DataTable is currently focused (or it has no row struct).")); }

			FString RowName;
			Args->TryGetStringField(TEXT("row_name"), RowName);
			if (RowName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'row_name'.")); }
			const FName Row(*RowName);
			if (DT->GetRowMap().Contains(Row)) { return FClaudeToolResult::Error(FString::Printf(TEXT("Row '%s' already exists."), *RowName)); }

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: add row %s"), *RowName)));
			DT->Modify();
			uint8* RowData = FDataTableEditorUtils::AddRow(DT, Row);
			if (!RowData) { return FClaudeToolResult::Error(TEXT("Failed to add the row.")); }

			const TSharedPtr<FJsonObject>* Values = nullptr;
			if (Args->TryGetObjectField(TEXT("values"), Values) && Values)
			{
				FJsonObjectConverter::JsonObjectToUStruct((*Values).ToSharedRef(), DT->RowStruct, RowData, 0, 0);
			}
			FDataTableEditorUtils::BroadcastPostChange(DT, FDataTableEditorUtils::EDataTableChangeInfo::RowData);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("row_name"), RowName);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_data_table_rows_batch
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_data_table_rows_batch");
		Tool.Category = TEXT("datatable");
		Tool.Description = TEXT("Add many rows to the focused DataTable in one undoable step. 'rows' is an array of objects, each with 'row_name' and 'values'. Duplicate names are skipped.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("rows"), TypedProp(TEXT("array"), TEXT("Array of { row_name, values } objects.")) },
		}, { TEXT("rows") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UDataTable* DT = ActiveDataTable();
			if (!DT || !DT->RowStruct) { return FClaudeToolResult::Error(TEXT("No DataTable is currently focused.")); }

			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!Args->TryGetArrayField(TEXT("rows"), Rows) || !Rows) { return FClaudeToolResult::Error(TEXT("Missing 'rows' array.")); }

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add DataTable rows")));
			DT->Modify();
			int32 Added = 0;
			TArray<TSharedPtr<FJsonValue>> Skipped;
			for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
			{
				const TSharedPtr<FJsonObject> RowObj = RowVal->AsObject();
				if (!RowObj.IsValid()) { continue; }
				FString RowName;
				RowObj->TryGetStringField(TEXT("row_name"), RowName);
				if (RowName.IsEmpty()) { continue; }
				const FName Row(*RowName);
				if (DT->GetRowMap().Contains(Row)) { Skipped.Add(MakeShared<FJsonValueString>(RowName)); continue; }
				uint8* RowData = FDataTableEditorUtils::AddRow(DT, Row);
				if (!RowData) { continue; }
				const TSharedPtr<FJsonObject>* Values = nullptr;
				if (RowObj->TryGetObjectField(TEXT("values"), Values) && Values)
				{
					FJsonObjectConverter::JsonObjectToUStruct((*Values).ToSharedRef(), DT->RowStruct, RowData, 0, 0);
				}
				++Added;
			}
			FDataTableEditorUtils::BroadcastPostChange(DT, FDataTableEditorUtils::EDataTableChangeInfo::RowData);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetNumberField(TEXT("added"), Added);
			Out->SetArrayField(TEXT("skipped_existing"), Skipped);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// update_data_table_row
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("update_data_table_row");
		Tool.Category = TEXT("datatable");
		Tool.Description = TEXT("Update fields of an existing DataTable row (merge: only the fields present in 'values' change). Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("row_name"), StringProp(TEXT("Existing row name.")) },
			{ TEXT("values"), TypedProp(TEXT("object"), TEXT("Field name -> new value map.")) },
		}, { TEXT("row_name"), TEXT("values") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UDataTable* DT = ActiveDataTable();
			if (!DT || !DT->RowStruct) { return FClaudeToolResult::Error(TEXT("No DataTable is currently focused.")); }

			FString RowName;
			Args->TryGetStringField(TEXT("row_name"), RowName);
			if (RowName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'row_name'.")); }
			const FName Row(*RowName);
			uint8* RowData = DT->GetRowMap().FindRef(Row);
			if (!RowData) { return FClaudeToolResult::Error(FString::Printf(TEXT("Row '%s' not found."), *RowName)); }

			const TSharedPtr<FJsonObject>* Values = nullptr;
			if (!Args->TryGetObjectField(TEXT("values"), Values) || !Values) { return FClaudeToolResult::Error(TEXT("Missing 'values'.")); }

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: update row %s"), *RowName)));
			DT->Modify();
			FJsonObjectConverter::JsonObjectToUStruct((*Values).ToSharedRef(), DT->RowStruct, RowData, 0, 0);
			FDataTableEditorUtils::BroadcastPostChange(DT, FDataTableEditorUtils::EDataTableChangeInfo::RowData);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("row_name"), RowName);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// delete_data_table_row (destructive)
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("delete_data_table_row");
		Tool.Category = TEXT("datatable");
		Tool.Description = TEXT("Delete a row from the focused DataTable by name. Asks for confirmation. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("row_name"), StringProp(TEXT("Row name to delete.")) },
		}, { TEXT("row_name") });
		Tool.bIsDestructive = true;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UDataTable* DT = ActiveDataTable();
			if (!DT) { return FClaudeToolResult::Error(TEXT("No DataTable is currently focused.")); }

			FString RowName;
			Args->TryGetStringField(TEXT("row_name"), RowName);
			if (RowName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'row_name'.")); }
			const FName Row(*RowName);
			if (!DT->GetRowMap().Contains(Row)) { return FClaudeToolResult::Error(FString::Printf(TEXT("Row '%s' not found."), *RowName)); }

			if (!ConfirmDestructive(FString::Printf(TEXT("Delete row '%s' from DataTable '%s'."), *RowName, *DT->GetName())))
			{
				return FClaudeToolResult::Error(TEXT("User denied the deletion."));
			}

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: delete row %s"), *RowName)));
			DT->Modify();
			FDataTableEditorUtils::RemoveRow(DT, Row);
			FDataTableEditorUtils::BroadcastPostChange(DT, FDataTableEditorUtils::EDataTableChangeInfo::RowList);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("deleted"), RowName);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_comment : annotation box on the Event Graph (no logic, safe).
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_comment");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a comment box to the focused Blueprint's Event Graph. Pure annotation, no logic. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("text"), StringProp(TEXT("Comment text.")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position (default 0).")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position (default 0).")) },
			{ TEXT("width"), TypedProp(TEXT("number"), TEXT("Optional width (default 400).")) },
			{ TEXT("height"), TypedProp(TEXT("number"), TEXT("Optional height (default 200).")) },
		}, { TEXT("text") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			if (BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("Blueprint has no Event Graph.")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }

			FString Text;
			Args->TryGetStringField(TEXT("text"), Text);
			if (Text.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'text'.")); }
			double X = 0.0, Y = 0.0, W = 400.0, H = 200.0;
			Args->TryGetNumberField(TEXT("x"), X);
			Args->TryGetNumberField(TEXT("y"), Y);
			Args->TryGetNumberField(TEXT("width"), W);
			Args->TryGetNumberField(TEXT("height"), H);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add comment")));
			Graph->Modify();
			UEdGraphNode_Comment* Node = NewObject<UEdGraphNode_Comment>(Graph);
			Node->SetFlags(RF_Transactional);
			Graph->AddNode(Node, true, false);
			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();
			Node->NodePosX = static_cast<int32>(X);
			Node->NodePosY = static_cast<int32>(Y);
			Node->NodeComment = Text;
			// EdGraph node geometry switched from FVector2D to FVector2f in UE 5.6.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 6
			Node->ResizeNode(FVector2D(W, H));
#else
			Node->ResizeNode(FVector2f(static_cast<float>(W), static_cast<float>(H)));
#endif
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_function : create a new (empty) user function graph.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_function");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Create a new empty user function in the focused Blueprint. Returns the function name. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("name"), StringProp(TEXT("New function name (must be unique).")) },
		}, { TEXT("name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString Name;
			Args->TryGetStringField(TEXT("name"), Name);
			if (Name.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'name'.")); }
			const FName FnName(*Name);
			for (const UEdGraph* G : BP->FunctionGraphs)
			{
				if (G && G->GetFName() == FnName) { return FClaudeToolResult::Error(FString::Printf(TEXT("Function '%s' already exists."), *Name)); }
			}

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: add function %s"), *Name)));
			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FnName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!NewGraph) { return FClaudeToolResult::Error(TEXT("Failed to create the function graph.")); }
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated*/ true, static_cast<UClass*>(nullptr));

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("function"), Name);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_function_call_node : place a Call Function node.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_function_call_node");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Place a 'Call Function' node in the focused Blueprint's Event Graph. Give function_name and, for library/engine functions, class_name (e.g. 'KismetSystemLibrary'). Omit class_name to call a function on this Blueprint. Returns the new node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("function_name"), StringProp(TEXT("Function name to call.")) },
			{ TEXT("class_name"), StringProp(TEXT("Optional owning class (e.g. 'KismetSystemLibrary'). Omit for a function on this Blueprint.")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, { TEXT("function_name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			if (BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("Blueprint has no Event Graph.")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }

			FString FunctionName, ClassName;
			Args->TryGetStringField(TEXT("function_name"), FunctionName);
			Args->TryGetStringField(TEXT("class_name"), ClassName);
			if (FunctionName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'function_name'.")); }

			UFunction* Func = nullptr;
			if (!ClassName.IsEmpty())
			{
				UClass* Cls = UClass::TryFindTypeSlow<UClass>(ClassName);
				if (!Cls) { return FClaudeToolResult::Error(FString::Printf(TEXT("Class '%s' not found."), *ClassName)); }
				Func = Cls->FindFunctionByName(FName(*FunctionName));
			}
			else
			{
				if (BP->SkeletonGeneratedClass) { Func = BP->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName)); }
				if (!Func && BP->GeneratedClass) { Func = BP->GeneratedClass->FindFunctionByName(FName(*FunctionName)); }
				if (!Func && BP->ParentClass) { Func = BP->ParentClass->FindFunctionByName(FName(*FunctionName)); }
			}
			if (!Func) { return FClaudeToolResult::Error(FString::Printf(TEXT("Function '%s' not found."), *FunctionName)); }

			double X = 0.0, Y = 0.0;
			Args->TryGetNumberField(TEXT("x"), X);
			Args->TryGetNumberField(TEXT("y"), Y);

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: add call %s"), *FunctionName)));
			Graph->Modify();
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode(false);
			Node->SetFromFunction(Func);
			Node->NodePosX = static_cast<int32>(X);
			Node->NodePosY = static_cast<int32>(Y);
			Creator.Finalize();
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_variable_node : place a variable Get or Set node.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_variable_node");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Place a variable Get or Set node for a member variable of the focused Blueprint. Set 'setter' true for a Set node (default Get). Returns the new node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("variable_name"), StringProp(TEXT("Member variable name.")) },
			{ TEXT("setter"), TypedProp(TEXT("boolean"), TEXT("True for a Set node, false/omitted for a Get node.")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, { TEXT("variable_name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			if (BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("Blueprint has no Event Graph.")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }

			FString VarName;
			Args->TryGetStringField(TEXT("variable_name"), VarName);
			if (VarName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'variable_name'.")); }
			bool bSetter = false;
			Args->TryGetBoolField(TEXT("setter"), bSetter);
			double X = 0.0, Y = 0.0;
			Args->TryGetNumberField(TEXT("x"), X);
			Args->TryGetNumberField(TEXT("y"), Y);

			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: add variable node %s"), *VarName)));
			Graph->Modify();
			FString NodeId;
			if (bSetter)
			{
				FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
				UK2Node_VariableSet* Node = Creator.CreateNode(false);
				Node->VariableReference.SetSelfMember(FName(*VarName));
				Node->NodePosX = static_cast<int32>(X);
				Node->NodePosY = static_cast<int32>(Y);
				Creator.Finalize();
				NodeId = Node->NodeGuid.ToString();
			}
			else
			{
				FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
				UK2Node_VariableGet* Node = Creator.CreateNode(false);
				Node->VariableReference.SetSelfMember(FName(*VarName));
				Node->NodePosX = static_cast<int32>(X);
				Node->NodePosY = static_cast<int32>(Y);
				Creator.Finalize();
				NodeId = Node->NodeGuid.ToString();
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), NodeId);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// connect_pins : wire two node pins together.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("connect_pins");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Connect two node pins in the focused Blueprint. Provide the node ids (from get_graph_nodes) and pin names (from get_node_details). Returns whether the connection was made. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("from_node_id"), StringProp(TEXT("Source node id.")) },
			{ TEXT("from_pin"), StringProp(TEXT("Source pin name.")) },
			{ TEXT("to_node_id"), StringProp(TEXT("Target node id.")) },
			{ TEXT("to_pin"), StringProp(TEXT("Target pin name.")) },
		}, { TEXT("from_node_id"), TEXT("from_pin"), TEXT("to_node_id"), TEXT("to_pin") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString FromId, FromPin, ToId, ToPin;
			Args->TryGetStringField(TEXT("from_node_id"), FromId);
			Args->TryGetStringField(TEXT("from_pin"), FromPin);
			Args->TryGetStringField(TEXT("to_node_id"), ToId);
			Args->TryGetStringField(TEXT("to_pin"), ToPin);
			FGuid FromGuid, ToGuid;
			if (!FGuid::Parse(FromId, FromGuid) || !FGuid::Parse(ToId, ToGuid)) { return FClaudeToolResult::Error(TEXT("Invalid node id(s).")); }

			UEdGraphNode* NodeA = FindBpNodeByGuid(BP, FromGuid);
			UEdGraphNode* NodeB = FindBpNodeByGuid(BP, ToGuid);
			if (!NodeA || !NodeB) { return FClaudeToolResult::Error(TEXT("Node(s) not found.")); }
			UEdGraphPin* PinA = NodeA->FindPin(FName(*FromPin));
			UEdGraphPin* PinB = NodeB->FindPin(FName(*ToPin));
			if (!PinA || !PinB) { return FClaudeToolResult::Error(TEXT("Pin(s) not found (check names via get_node_details).")); }
			UEdGraph* Graph = NodeA->GetGraph();
			const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
			if (!Schema) { return FClaudeToolResult::Error(TEXT("Graph schema unavailable.")); }

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: connect pins")));
			Graph->Modify();
			const bool bConnected = Schema->TryCreateConnection(PinA, PinB);
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), bConnected);
			if (!bConnected) { Out->SetStringField(TEXT("note"), TEXT("The schema rejected the connection (incompatible pin types or directions).")); }
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// delete_node (destructive)
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("delete_node");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Delete a node from the focused Blueprint by its id (from get_graph_nodes). Asks for confirmation. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("node_id"), StringProp(TEXT("Node id to delete.")) },
		}, { TEXT("node_id") });
		Tool.bIsDestructive = true;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString NodeId;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			FGuid Guid;
			if (!FGuid::Parse(NodeId, Guid)) { return FClaudeToolResult::Error(TEXT("Invalid 'node_id'.")); }
			UEdGraphNode* Node = FindBpNodeByGuid(BP, Guid);
			if (!Node) { return FClaudeToolResult::Error(TEXT("Node not found.")); }
			if (!Node->CanUserDeleteNode()) { return FClaudeToolResult::Error(TEXT("This node cannot be deleted.")); }

			if (!ConfirmDestructive(FString::Printf(TEXT("Delete node '%s' from Blueprint '%s'."),
				*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *BP->GetName())))
			{
				return FClaudeToolResult::Error(TEXT("User denied the deletion."));
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: delete node")));
			FBlueprintEditorUtils::RemoveNode(BP, Node, false);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// set_pin_default : set the literal default value of an input pin.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("set_pin_default");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Set the literal default value of an input pin (e.g. the string of a Print String node). Provide node_id (from get_graph_nodes), pin name (from get_node_details) and the value as text. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("node_id"), StringProp(TEXT("Node id.")) },
			{ TEXT("pin"), StringProp(TEXT("Input pin name.")) },
			{ TEXT("value"), StringProp(TEXT("New default value, as text.")) },
		}, { TEXT("node_id"), TEXT("pin"), TEXT("value") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FString NodeId, PinName, Value;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetStringField(TEXT("pin"), PinName);
			Args->TryGetStringField(TEXT("value"), Value);
			FGuid Guid;
			if (!FGuid::Parse(NodeId, Guid)) { return FClaudeToolResult::Error(TEXT("Invalid 'node_id'.")); }
			UEdGraphNode* Node = FindBpNodeByGuid(BP, Guid);
			if (!Node) { return FClaudeToolResult::Error(TEXT("Node not found.")); }
			UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
			if (!Pin) { return FClaudeToolResult::Error(TEXT("Pin not found (check names via get_node_details).")); }
			if (Pin->Direction != EGPD_Input) { return FClaudeToolResult::Error(TEXT("Only input pins have a default value.")); }
			UEdGraph* Graph = Node->GetGraph();
			const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
			if (!Schema) { return FClaudeToolResult::Error(TEXT("Graph schema unavailable.")); }

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: set pin default")));
			Graph->Modify();
			Schema->TrySetDefaultValue(*Pin, Value);
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("pin"), PinName);
			Out->SetStringField(TEXT("value"), Pin->DefaultValue);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_event : place an override event (BeginPlay/Tick/...) or a Custom Event.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_event");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Place an event node in the focused Blueprint's Event Graph. Set custom=true for a Custom Event with the given name; otherwise it adds an overridable engine event (e.g. 'BeginPlay', 'Tick' resolve to ReceiveBeginPlay/ReceiveTick on the parent class). Returns the new node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("name"), StringProp(TEXT("Event name. Overrides: 'BeginPlay', 'Tick', etc. Custom: any name.")) },
			{ TEXT("custom"), TypedProp(TEXT("boolean"), TEXT("True for a Custom Event, false/omitted for an engine override event.")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, { TEXT("name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			if (BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("Blueprint has no Event Graph.")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }

			FString Name;
			Args->TryGetStringField(TEXT("name"), Name);
			if (Name.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'name'.")); }
			bool bCustom = false;
			Args->TryGetBoolField(TEXT("custom"), bCustom);
			double X = 0.0, Y = 0.0;
			Args->TryGetNumberField(TEXT("x"), X);
			Args->TryGetNumberField(TEXT("y"), Y);

			FString NodeId;
			const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Claude: add event %s"), *Name)));
			Graph->Modify();

			if (bCustom)
			{
				if (FBlueprintEditorUtils::FindCustomEventNode(BP, FName(*Name)))
				{
					return FClaudeToolResult::Error(FString::Printf(TEXT("A custom event named '%s' already exists."), *Name));
				}
				FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
				UK2Node_CustomEvent* Node = Creator.CreateNode(false);
				Node->CustomFunctionName = FName(*Name);
				Node->NodePosX = static_cast<int32>(X);
				Node->NodePosY = static_cast<int32>(Y);
				Creator.Finalize();
				NodeId = Node->NodeGuid.ToString();
			}
			else
			{
				UClass* SearchClass = BP->GeneratedClass ? BP->GeneratedClass : BP->ParentClass;
				UFunction* Func = SearchClass ? SearchClass->FindFunctionByName(FName(*Name)) : nullptr;
				if (!Func)
				{
					const FString Alt = FString(TEXT("Receive")) + Name;
					Func = SearchClass ? SearchClass->FindFunctionByName(FName(*Alt)) : nullptr;
				}
				if (!Func) { return FClaudeToolResult::Error(FString::Printf(TEXT("No overridable event '%s' found on the parent class (use custom=true for a Custom Event)."), *Name)); }

				UClass* SigClass = Func->GetOwnerClass();
				if (UK2Node_Event* Existing = SigClass ? FBlueprintEditorUtils::FindOverrideForFunction(BP, SigClass, Func->GetFName()) : nullptr)
				{
					// New Blueprints ship with disabled "ghost" BeginPlay/Tick nodes. Treat those
					// (and genuinely existing events) idempotently: enable if needed, return the id.
					if (Existing->IsAutomaticallyPlacedGhostNode())
					{
						Existing->Modify();
						Existing->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ true);
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
					TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
					Out->SetBoolField(TEXT("success"), true);
					Out->SetStringField(TEXT("node_id"), Existing->NodeGuid.ToString());
					Out->SetStringField(TEXT("note"), TEXT("Event already existed; returning it."));
					return OkJson(Out);
				}
				FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
				UK2Node_Event* Node = Creator.CreateNode(false);
				Node->EventReference.SetExternalMember(Func->GetFName(), SigClass);
				Node->bOverrideFunction = true;
				Node->NodePosX = static_cast<int32>(X);
				Node->NodePosY = static_cast<int32>(Y);
				Creator.Finalize();
				NodeId = Node->NodeGuid.ToString();
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), NodeId);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_branch : Branch (if/then/else) node.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_branch");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a Branch (if) node to the Event Graph. It has an exec input, a boolean 'Condition' input, and 'True'/'False' exec outputs. Returns the node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, {});
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP || BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("No Blueprint focused (or no Event Graph).")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }
			double X = 0.0, Y = 0.0; Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add branch")));
			Graph->Modify();
			FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
			UK2Node_IfThenElse* Node = Creator.CreateNode(false);
			Node->NodePosX = static_cast<int32>(X); Node->NodePosY = static_cast<int32>(Y);
			Creator.Finalize();
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_sequence : Sequence node (fires exec outputs in order).
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_sequence");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a Sequence node to the Event Graph (runs its exec outputs Then 0, Then 1, ... in order). 'outputs' sets how many exec outputs (default 2). Returns the node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("outputs"), TypedProp(TEXT("number"), TEXT("Number of exec outputs (default 2).")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, {});
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP || BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("No Blueprint focused (or no Event Graph).")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }
			double X = 0.0, Y = 0.0, Outputs = 2.0;
			Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y);
			Args->TryGetNumberField(TEXT("outputs"), Outputs);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add sequence")));
			Graph->Modify();
			UK2Node_ExecutionSequence* Node = nullptr;
			{
				FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
				Node = Creator.CreateNode(false);
				Node->NodePosX = static_cast<int32>(X); Node->NodePosY = static_cast<int32>(Y);
				Creator.Finalize();
			}
			const int32 OutputCount = FMath::Clamp(static_cast<int32>(Outputs), 2, 64);
			for (int32 i = 2; i < OutputCount; ++i) { Node->AddInputPin(); }
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_cast_node : dynamic Cast To <class> node.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_cast_node");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a 'Cast To <class>' node to the Event Graph. target_class is resolved by name (e.g. 'Character', 'MyPawn'). Returns the node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("target_class"), StringProp(TEXT("Class to cast to (by name).")) },
			{ TEXT("pure"), TypedProp(TEXT("boolean"), TEXT("Optional 'true' for a pure cast (no exec pins).")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, { TEXT("target_class") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP || BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("No Blueprint focused (or no Event Graph).")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }
			FString ClassName; Args->TryGetStringField(TEXT("target_class"), ClassName);
			if (ClassName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'target_class'.")); }
			UClass* Target = UClass::TryFindTypeSlow<UClass>(ClassName);
			if (!Target) { return FClaudeToolResult::Error(FString::Printf(TEXT("Class '%s' not found."), *ClassName)); }
			bool bPure = false; Args->TryGetBoolField(TEXT("pure"), bPure);
			double X = 0.0, Y = 0.0; Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add cast")));
			Graph->Modify();
			FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
			UK2Node_DynamicCast* Node = Creator.CreateNode(false);
			Node->TargetType = Target;
			Node->SetPurity(bPure);
			Node->NodePosX = static_cast<int32>(X); Node->NodePosY = static_cast<int32>(Y);
			Creator.Finalize();
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_make_struct / add_break_struct
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_make_struct");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a 'Make <Struct>' node to the Event Graph (builds a struct from member inputs). struct_name resolved by name. Returns the node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("struct_name"), StringProp(TEXT("Struct type name.")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, { TEXT("struct_name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP || BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("No Blueprint focused (or no Event Graph).")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }
			FString StructName; Args->TryGetStringField(TEXT("struct_name"), StructName);
			if (StructName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'struct_name'.")); }
			UScriptStruct* Struct = UClass::TryFindTypeSlow<UScriptStruct>(StructName);
			if (!Struct) { return FClaudeToolResult::Error(FString::Printf(TEXT("Struct '%s' not found."), *StructName)); }
			double X = 0.0, Y = 0.0; Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add make struct")));
			Graph->Modify();
			FGraphNodeCreator<UK2Node_MakeStruct> Creator(*Graph);
			UK2Node_MakeStruct* Node = Creator.CreateNode(false);
			Node->StructType = Struct;
			Node->NodePosX = static_cast<int32>(X); Node->NodePosY = static_cast<int32>(Y);
			Creator.Finalize();
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_break_struct");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a 'Break <Struct>' node to the Event Graph (splits a struct into member outputs). struct_name resolved by name. Returns the node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("struct_name"), StringProp(TEXT("Struct type name.")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, { TEXT("struct_name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP || BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("No Blueprint focused (or no Event Graph).")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }
			FString StructName; Args->TryGetStringField(TEXT("struct_name"), StructName);
			if (StructName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'struct_name'.")); }
			UScriptStruct* Struct = UClass::TryFindTypeSlow<UScriptStruct>(StructName);
			if (!Struct) { return FClaudeToolResult::Error(FString::Printf(TEXT("Struct '%s' not found."), *StructName)); }
			double X = 0.0, Y = 0.0; Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add break struct")));
			Graph->Modify();
			FGraphNodeCreator<UK2Node_BreakStruct> Creator(*Graph);
			UK2Node_BreakStruct* Node = Creator.CreateNode(false);
			Node->StructType = Struct;
			Node->NodePosX = static_cast<int32>(X); Node->NodePosY = static_cast<int32>(Y);
			Creator.Finalize();
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_loop : ForLoop / ForEachLoop / WhileLoop (standard macros).
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_loop");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a loop node to the Event Graph. kind: 'for' (ForLoop), 'for_break' (ForLoopWithBreak), 'foreach' (ForEachLoop), 'foreach_break' (ForEachLoopWithBreak), 'while' (WhileLoop). Returns the node id. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("kind"), StringProp(TEXT("Loop kind: for | for_break | foreach | foreach_break | while.")) },
			{ TEXT("x"), TypedProp(TEXT("number"), TEXT("Optional X position.")) },
			{ TEXT("y"), TypedProp(TEXT("number"), TEXT("Optional Y position.")) },
		}, { TEXT("kind") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP || BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("No Blueprint focused (or no Event Graph).")); }
			UEdGraph* Graph = ResolveTargetGraph(BP);
			if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found (use set_active_graph).")); }

			FString Kind; Args->TryGetStringField(TEXT("kind"), Kind);
			FString MacroName;
			if (Kind.Equals(TEXT("for"), ESearchCase::IgnoreCase)) { MacroName = TEXT("ForLoop"); }
			else if (Kind.Equals(TEXT("for_break"), ESearchCase::IgnoreCase)) { MacroName = TEXT("ForLoopWithBreak"); }
			else if (Kind.Equals(TEXT("foreach"), ESearchCase::IgnoreCase)) { MacroName = TEXT("ForEachLoop"); }
			else if (Kind.Equals(TEXT("foreach_break"), ESearchCase::IgnoreCase)) { MacroName = TEXT("ForEachLoopWithBreak"); }
			else if (Kind.Equals(TEXT("while"), ESearchCase::IgnoreCase)) { MacroName = TEXT("WhileLoop"); }
			else { return FClaudeToolResult::Error(TEXT("Unknown 'kind' (use for | for_break | foreach | foreach_break | while).")); }

			UBlueprint* MacroBP = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
			if (!MacroBP) { MacroBP = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorKismetResources/StandardMacros.StandardMacros")); }
			if (!MacroBP) { return FClaudeToolResult::Error(TEXT("Standard macro library not found.")); }
			UEdGraph* MacroGraph = nullptr;
			for (UEdGraph* G : MacroBP->MacroGraphs) { if (G && G->GetName() == MacroName) { MacroGraph = G; break; } }
			if (!MacroGraph) { return FClaudeToolResult::Error(FString::Printf(TEXT("Macro '%s' not found."), *MacroName)); }

			double X = 0.0, Y = 0.0; Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y);
			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add loop")));
			Graph->Modify();
			FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
			UK2Node_MacroInstance* Node = Creator.CreateNode(false);
			Node->SetMacroGraph(MacroGraph);
			Node->NodePosX = static_cast<int32>(X); Node->NodePosY = static_cast<int32>(Y);
			Creator.Finalize();
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// set_active_graph : choose which graph node-creation tools target.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("set_active_graph");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Choose which graph the node-creation tools target in the focused Blueprint. Pass a function/graph name, or 'event' (or empty) for the Event Graph. Applies to all later add_* node tools until changed.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("graph"), StringProp(TEXT("Function/graph name, or 'event' for the Event Graph.")) },
		}, {});
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			FString GraphName;
			Args->TryGetStringField(TEXT("graph"), GraphName);
			if (GraphName.IsEmpty() || GraphName.Equals(TEXT("event"), ESearchCase::IgnoreCase) || GraphName.Equals(TEXT("eventgraph"), ESearchCase::IgnoreCase))
			{
				ClaudeWriteTools::SetActiveGraphName(NAME_None);
			}
			else
			{
				bool bFound = false;
				const FName Wanted(*GraphName);
				auto Check = [&bFound, &Wanted](const TArray<UEdGraph*>& Gs) { for (UEdGraph* G : Gs) { if (G && G->GetFName() == Wanted) { bFound = true; } } };
				Check(BP->FunctionGraphs); Check(BP->UbergraphPages); Check(BP->MacroGraphs);
				if (!bFound) { return FClaudeToolResult::Error(FString::Printf(TEXT("Graph '%s' not found."), *GraphName)); }
				ClaudeWriteTools::SetActiveGraphName(Wanted);
			}
			UEdGraph* Resolved = ClaudeWriteTools::ResolveTargetGraph(BP);
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("active_graph"), Resolved ? Resolved->GetName() : FString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// set_node_property : set a details-panel property on a node.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("set_node_property");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Set a property (a details-panel value, not a pin) on a node in the focused Blueprint, by node id, property name, and value as text. For values that appear as input pins, use set_pin_default instead. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("node_id"), StringProp(TEXT("Node id (from get_graph_nodes).")) },
			{ TEXT("property"), StringProp(TEXT("Property name on the node.")) },
			{ TEXT("value"), StringProp(TEXT("New value, as text.")) },
		}, { TEXT("node_id"), TEXT("property"), TEXT("value") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			FString NodeId, PropName, Value;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetStringField(TEXT("property"), PropName);
			Args->TryGetStringField(TEXT("value"), Value);
			FGuid Guid;
			if (!FGuid::Parse(NodeId, Guid)) { return FClaudeToolResult::Error(TEXT("Invalid 'node_id'.")); }
			UEdGraphNode* Node = FindBpNodeByGuid(BP, Guid);
			if (!Node) { return FClaudeToolResult::Error(TEXT("Node not found.")); }
			FProperty* Prop = Node->GetClass()->FindPropertyByName(FName(*PropName));
			if (!Prop) { return FClaudeToolResult::Error(FString::Printf(TEXT("Property '%s' not found on node '%s'."), *PropName, *Node->GetClass()->GetName())); }

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: set node property")));
			Node->Modify();
			const TCHAR* Result = Prop->ImportText_Direct(*Value, Prop->ContainerPtrToValuePtr<void>(Node), Node, PPF_None);
			Node->ReconstructNode();
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), Result != nullptr);
			if (!Result) { Out->SetStringField(TEXT("note"), TEXT("The value could not be parsed for this property type.")); }
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_component : add a component to an Actor Blueprint (construction script).
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_component");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a component to the focused Actor Blueprint. component_class by name (e.g. 'StaticMeshComponent', 'BoxComponent', 'PointLightComponent'). Optionally parent it under an existing scene component by name. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("component_class"), StringProp(TEXT("Component class name (e.g. 'StaticMeshComponent').")) },
			{ TEXT("name"), StringProp(TEXT("Optional variable name for the component.")) },
			{ TEXT("parent_component"), StringProp(TEXT("Optional existing scene component to attach under.")) },
		}, { TEXT("component_class") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			if (!BP->SimpleConstructionScript) { return FClaudeToolResult::Error(TEXT("This Blueprint has no component tree (it is not an Actor-based Blueprint).")); }

			FString ClassName, Name, ParentName;
			Args->TryGetStringField(TEXT("component_class"), ClassName);
			Args->TryGetStringField(TEXT("name"), Name);
			Args->TryGetStringField(TEXT("parent_component"), ParentName);
			if (ClassName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'component_class'.")); }
			UClass* CompClass = UClass::TryFindTypeSlow<UClass>(ClassName);
			if (!CompClass && ClassName.StartsWith(TEXT("U"))) { FString Alt = ClassName; Alt.RightChopInline(1); CompClass = UClass::TryFindTypeSlow<UClass>(Alt); }
			if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()) || CompClass->HasAnyClassFlags(CLASS_Abstract))
			{
				return FClaudeToolResult::Error(FString::Printf(TEXT("Component class '%s' not found."), *ClassName));
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add component")));
			BP->Modify();
			USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
			SCS->Modify();
			USCS_Node* NewNode = SCS->CreateNode(CompClass, Name.IsEmpty() ? NAME_None : FName(*Name));
			if (!NewNode) { return FClaudeToolResult::Error(TEXT("Failed to create the component node.")); }

			USCS_Node* Parent = nullptr;
			if (!ParentName.IsEmpty())
			{
				const FName WantedParent(*ParentName);
				for (USCS_Node* N : SCS->GetAllNodes()) { if (N && N->GetVariableName() == WantedParent) { Parent = N; break; } }
			}
			if (Parent) { Parent->Modify(); Parent->AddChildNode(NewNode); }
			else { SCS->AddNode(NewNode); }

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetStringField(TEXT("component"), NewNode->GetVariableName().ToString());
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_function_parameter : add an input/output parameter to a function.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_function_parameter");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a parameter to a Blueprint function. type is a friendly type name (bool/int/float/string/name/text/vector/rotator/transform/...). Set output=true for a return value (the function must already have a result). Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("function_name"), StringProp(TEXT("Function name.")) },
			{ TEXT("param_name"), StringProp(TEXT("New parameter name.")) },
			{ TEXT("type"), StringProp(TEXT("Parameter type (friendly name).")) },
			{ TEXT("output"), TypedProp(TEXT("boolean"), TEXT("True for a return value; default input.")) },
		}, { TEXT("function_name"), TEXT("param_name"), TEXT("type") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			FString FnName, ParamName, TypeStr;
			Args->TryGetStringField(TEXT("function_name"), FnName);
			Args->TryGetStringField(TEXT("param_name"), ParamName);
			Args->TryGetStringField(TEXT("type"), TypeStr);
			if (FnName.IsEmpty() || ParamName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'function_name' or 'param_name'.")); }
			bool bOutput = false; Args->TryGetBoolField(TEXT("output"), bOutput);

			FEdGraphPinType PinType;
			if (!BuildPinType(TypeStr, PinType)) { return FClaudeToolResult::Error(FString::Printf(TEXT("Unsupported type '%s'."), *TypeStr)); }

			UEdGraph* FnGraph = nullptr;
			for (UEdGraph* G : BP->FunctionGraphs) { if (G && G->GetName() == FnName) { FnGraph = G; break; } }
			if (!FnGraph) { return FClaudeToolResult::Error(FString::Printf(TEXT("Function '%s' not found."), *FnName)); }

			UK2Node_EditablePinBase* Target = nullptr;
			EEdGraphPinDirection Dir = EGPD_Output;
			if (bOutput)
			{
				for (UEdGraphNode* N : FnGraph->Nodes) { if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N)) { Target = R; break; } }
				if (!Target) { return FClaudeToolResult::Error(TEXT("This function has no result node (add a return value in the editor first).")); }
				Dir = EGPD_Input;
			}
			else
			{
				for (UEdGraphNode* N : FnGraph->Nodes) { if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) { Target = E; break; } }
				if (!Target) { return FClaudeToolResult::Error(TEXT("Function entry node not found.")); }
				Dir = EGPD_Output;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add function parameter")));
			FnGraph->Modify();
			Target->Modify();
			UEdGraphPin* NewPin = Target->CreateUserDefinedPin(FName(*ParamName), PinType, Dir, true);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), NewPin != nullptr);
			Out->SetStringField(TEXT("parameter"), ParamName);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// disconnect_pin : break all links on a node's pin.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("disconnect_pin");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Break all links on a pin of a node in the focused Blueprint, by node id and pin name. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("node_id"), StringProp(TEXT("Node id.")) },
			{ TEXT("pin"), StringProp(TEXT("Pin name.")) },
		}, { TEXT("node_id"), TEXT("pin") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			FString NodeId, PinName;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetStringField(TEXT("pin"), PinName);
			FGuid Guid;
			if (!FGuid::Parse(NodeId, Guid)) { return FClaudeToolResult::Error(TEXT("Invalid 'node_id'.")); }
			UEdGraphNode* Node = FindBpNodeByGuid(BP, Guid);
			if (!Node) { return FClaudeToolResult::Error(TEXT("Node not found.")); }
			UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
			if (!Pin) { return FClaudeToolResult::Error(TEXT("Pin not found.")); }

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: disconnect pin")));
			if (UEdGraph* G = Node->GetGraph()) { G->Modify(); }
			Pin->BreakAllPinLinks();
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// compile_blueprint : compile and report errors/warnings.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("compile_blueprint");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Compile the focused Blueprint and report the error and warning count plus messages. Call this after building graph logic to verify it is valid.");
		Tool.InputSchema = ObjSchema({}, {});
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }

			FCompilerResultsLog Results;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

			TArray<TSharedPtr<FJsonValue>> Msgs;
			for (const TSharedRef<FTokenizedMessage>& M : Results.Messages)
			{
				Msgs.Add(MakeShared<FJsonValueString>(M->ToText().ToString()));
				if (Msgs.Num() >= 20) { break; }
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), Results.NumErrors == 0);
			Out->SetNumberField(TEXT("errors"), Results.NumErrors);
			Out->SetNumberField(TEXT("warnings"), Results.NumWarnings);
			Out->SetArrayField(TEXT("messages"), Msgs);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// set_variable_config : expose/categorize a member variable.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("set_variable_config");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Configure a member variable of the focused Blueprint: instance_editable (exposed to the details panel), category, and tooltip. Only the provided fields change. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("variable"), StringProp(TEXT("Variable name.")) },
			{ TEXT("instance_editable"), TypedProp(TEXT("boolean"), TEXT("Expose the variable on instances (details panel).")) },
			{ TEXT("category"), StringProp(TEXT("Optional category.")) },
			{ TEXT("tooltip"), StringProp(TEXT("Optional tooltip.")) },
		}, { TEXT("variable") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			FString VarName;
			Args->TryGetStringField(TEXT("variable"), VarName);
			if (VarName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'variable'.")); }
			const FName Var(*VarName);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: configure variable")));
			bool bInstanceEditable = false;
			if (Args->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable))
			{
				// BlueprintOnlyEditable == NOT instance-editable, so invert.
				FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(BP, Var, !bInstanceEditable);
			}
			FString Category;
			if (Args->TryGetStringField(TEXT("category"), Category) && !Category.IsEmpty())
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, Var, nullptr, TEXT("Category"), Category);
			}
			FString Tooltip;
			if (Args->TryGetStringField(TEXT("tooltip"), Tooltip) && !Tooltip.IsEmpty())
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, Var, nullptr, TEXT("tooltip"), Tooltip);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// implement_interface
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("implement_interface");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Make the focused Blueprint implement a Blueprint Interface, by interface name. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("interface_name"), StringProp(TEXT("Interface class name.")) },
		}, { TEXT("interface_name") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UBlueprint* BP = ActiveBlueprint();
			if (!BP) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
			FString InterfaceName;
			Args->TryGetStringField(TEXT("interface_name"), InterfaceName);
			if (InterfaceName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'interface_name'.")); }

			UClass* IfaceClass = UClass::TryFindTypeSlow<UClass>(InterfaceName);
			if (!IfaceClass) { return FClaudeToolResult::Error(FString::Printf(TEXT("Interface '%s' not found."), *InterfaceName)); }

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: implement interface")));
			const bool bOk = FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(IfaceClass));
			if (bOk) { FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP); }

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), bOk);
			if (!bOk) { Out->SetStringField(TEXT("note"), TEXT("Interface already implemented, or not a valid interface.")); }
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_struct_member : add a typed member to a User Defined Struct.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_struct_member");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add a member of the given type to a User Defined Struct (the focused struct, or asset_path). type is a friendly type name (bool/int/float/string/vector/...). The member gets an auto name. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("type"), StringProp(TEXT("Member type (friendly name).")) },
			{ TEXT("asset_path"), StringProp(TEXT("Optional struct object path; defaults to the focused asset.")) },
		}, { TEXT("type") });
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UUserDefinedStruct* Struct = ActiveUserStruct(Args);
			if (!Struct) { return FClaudeToolResult::Error(TEXT("No User Defined Struct is focused (or pass asset_path).")); }
			FString TypeStr;
			Args->TryGetStringField(TEXT("type"), TypeStr);
			FEdGraphPinType PinType;
			if (!BuildPinType(TypeStr, PinType)) { return FClaudeToolResult::Error(FString::Printf(TEXT("Unsupported type '%s'."), *TypeStr)); }

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add struct member")));
			const bool bOk = FStructureEditorUtils::AddVariable(Struct, PinType);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), bOk);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// add_enum_entry : add an entry to a User Defined Enum.
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("add_enum_entry");
		Tool.Category = TEXT("blueprint");
		Tool.Description = TEXT("Add an entry to a User Defined Enum (the focused enum, or asset_path), optionally with a display name. Undoable.");
		Tool.InputSchema = ObjSchema({
			{ TEXT("display_name"), StringProp(TEXT("Optional display name for the new entry.")) },
			{ TEXT("asset_path"), StringProp(TEXT("Optional enum object path; defaults to the focused asset.")) },
		}, {});
		Tool.bIsDestructive = false;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			UUserDefinedEnum* Enum = ActiveUserEnum(Args);
			if (!Enum) { return FClaudeToolResult::Error(TEXT("No User Defined Enum is focused (or pass asset_path).")); }

			const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add enum entry")));
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);

			FString DisplayName;
			if (Args->TryGetStringField(TEXT("display_name"), DisplayName) && !DisplayName.IsEmpty())
			{
				// Newly added entry is the last real one (index NumEnums()-2, before the hidden _MAX).
				const int32 Index = Enum->NumEnums() - 2;
				if (Index >= 0)
				{
					FEnumEditorUtils::SetEnumeratorDisplayName(Enum, Index, FText::FromString(DisplayName));
				}
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), true);
			Out->SetNumberField(TEXT("entries"), Enum->NumEnums() - 1);
			return OkJson(Out);
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}

	// -------------------------------------------------------------------------
	// run_python : universal escape hatch (fallback for the long tail).
	// -------------------------------------------------------------------------
	{
		FClaudeTool Tool;
		Tool.Name = TEXT("run_python");
		Tool.Category = TEXT("python");
		Tool.Description = TEXT(
			"FALLBACK: run a Python script inside the Unreal editor (full 'unreal' module access) for actions "
			"that have NO dedicated tool. Prefer dedicated tools (add_variable, rename_variable, ...) whenever one "
			"exists — they are reliable and don't require guessing the Python API. Runs only after the user approves "
			"the script in a confirmation dialog. Use print(...) for output.");
		TSharedRef<FJsonObject> ScriptProp = MakeShared<FJsonObject>();
		ScriptProp->SetStringField(TEXT("type"), TEXT("string"));
		ScriptProp->SetStringField(TEXT("description"), TEXT("The Python script to run. Full access to the 'unreal' module."));
		Tool.InputSchema = ObjSchema({ { TEXT("script"), ScriptProp } }, { TEXT("script") });
		Tool.bIsDestructive = true;
		Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
		{
			FString Script;
			if (!Args.IsValid() || !Args->TryGetStringField(TEXT("script"), Script) || Script.IsEmpty())
			{
				return FClaudeToolResult::Error(TEXT("Missing required 'script' argument."));
			}

			const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
			// Automation context (self-test / CI): no user to ask, auto-approve.
			const bool bConfirm = !FApp::IsUnattended() && (!Settings || Settings->bConfirmRunPython);
			if (bConfirm)
			{
				const FString Preview = Script.Len() > 4000 ? (Script.Left(4000) + TEXT("\n... [truncated]")) : Script;
				const FText Message = FText::FromString(FString::Printf(
					TEXT("Claude wants to run this Python script in the editor:\n\n%s\n\nAllow?"), *Preview));
				if (FMessageDialog::Open(EAppMsgType::YesNo, Message) != EAppReturnType::Yes)
				{
					return FClaudeToolResult::Error(TEXT("User denied the Python execution."));
				}
			}

			IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
			if (!Python || !Python->IsPythonAvailable())
			{
				return FClaudeToolResult::Error(TEXT("Python is not available. Enable the 'Python Editor Script Plugin' for this project."));
			}

			FPythonCommandEx Cmd;
			Cmd.Command = Script;
			Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
			Cmd.FileExecutionScope = EPythonFileExecutionScope::Public;

			const bool bOk = Python->ExecPythonCommandEx(Cmd);

			FString CapturedLog;
			for (const FPythonLogOutputEntry& Entry : Cmd.LogOutput)
			{
				CapturedLog += Entry.Output;
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"), bOk);
			Out->SetStringField(TEXT("result"), Cmd.CommandResult);
			Out->SetStringField(TEXT("output"), CapturedLog);
			return bOk ? OkJson(Out) : FClaudeToolResult::Error(WriteSerializeJson(Out));
		});
		Registry.RegisterTool(MoveTemp(Tool));
	}
}

#undef LOCTEXT_NAMESPACE
