// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeNodeActionTools.cpp
// Exposes Unreal's Blueprint Action Database (the right-click palette) so the
// assistant can search for and spawn ANY node the editor can create, across the
// whole engine and any installed plugin. search_node_actions -> add_node_by_action.

#include "ClaudeNodeActionTools.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantFocusTracker.h"
#include "ClaudeWriteTools.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintNodeSignature.h"
#include "BlueprintNodeBinder.h"

namespace
{
	FString NodeActSerJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> NodeActSchema(const TArray<TPair<FString, FString>>& Props, const TArray<FString>& Required)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Props)
		{
			TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
			Prop->SetStringField(TEXT("type"), Pair.Key == TEXT("x") || Pair.Key == TEXT("y") || Pair.Key == TEXT("limit") ? TEXT("number") : TEXT("string"));
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

	UBlueprint* NodeActActiveBP()
	{
		return Cast<UBlueprint>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}

	// Session cache: signature-string -> spawner, filled during search to make spawn fast.
	TMap<FString, TWeakObjectPtr<UBlueprintNodeSpawner>>& NodeActionCache()
	{
		static TMap<FString, TWeakObjectPtr<UBlueprintNodeSpawner>> Cache;
		return Cache;
	}

	// Cheap title: read the cached DefaultMenuSignature only. Do NOT PrimeDefaultUiSpec()
	// during a scan -- priming spawns template nodes and badly hitches the game thread
	// across the whole action database.
	FString SpawnerTitle(UBlueprintNodeSpawner* Spawner)
	{
		const FText Name = Spawner->DefaultMenuSignature.MenuName;
		if (!Name.IsEmpty()) { return Name.ToString(); }
		return Spawner->NodeClass ? Spawner->NodeClass->GetName() : FString();
	}

	bool MatchesAllTokens(const FString& Haystack, const TArray<FString>& Tokens)
	{
		for (const FString& Tok : Tokens)
		{
			if (!Haystack.Contains(Tok, ESearchCase::IgnoreCase)) { return false; }
		}
		return Tokens.Num() > 0;
	}
}

namespace ClaudeNodeActionTools
{
	void RegisterAll(FClaudeToolRegistry& Registry)
	{
		// ---------------------------------------------------------------------
		// search_node_actions : search the entire Blueprint palette.
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("search_node_actions");
			Tool.Category = TEXT("blueprint");
			Tool.Description = TEXT("Search the entire Blueprint node palette (every node the editor can add, across the whole engine and installed plugins) by keyword. Returns matching actions with a 'key' to pass to add_node_by_action. Use specific queries (e.g. 'delay', 'get player controller', 'random integer in range'). Needs a focused Blueprint for context.");
			Tool.InputSchema = NodeActSchema({
				{ TEXT("query"), TEXT("Keywords to match against node titles/keywords/category.") },
				{ TEXT("limit"), TEXT("Max results (default 25).") },
			}, { TEXT("query") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UBlueprint* BP = NodeActActiveBP();
				if (!BP || BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused (needed for node context).")); }

				FString Query;
				Args->TryGetStringField(TEXT("query"), Query);
				if (Query.TrimStartAndEnd().IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'query'.")); }
				TArray<FString> Tokens;
				Query.ParseIntoArrayWS(Tokens);

				double LimitD = 25.0;
				Args->TryGetNumberField(TEXT("limit"), LimitD);
				const int32 Limit = FMath::Clamp(static_cast<int32>(LimitD), 1, 100);

				const FBlueprintActionDatabase::FActionRegistry& Registry2 = FBlueprintActionDatabase::Get().GetAllActions();

				TArray<TSharedPtr<FJsonValue>> Results;
				int32 Inspected = 0;
				bool bTruncated = false;
				const int32 MaxInspect = 80000; // hard bound on the game-thread scan
				for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : Registry2)
				{
					for (UBlueprintNodeSpawner* Spawner : Pair.Value)
					{
						if (!Spawner) { continue; }
						if (++Inspected > MaxInspect) { bTruncated = true; break; }
						const FString Title = SpawnerTitle(Spawner);
						if (Title.IsEmpty()) { continue; }
						const FString Category = Spawner->DefaultMenuSignature.Category.ToString();
						const FString Keywords = Spawner->DefaultMenuSignature.Keywords.ToString();
						const FString Haystack = Title + TEXT("|") + Category + TEXT("|") + Keywords;
						if (!MatchesAllTokens(Haystack, Tokens)) { continue; }

						// Stop as soon as we would exceed the limit, so 'truncated' only means
						// "more matches exist beyond what we returned".
						if (Results.Num() >= Limit) { bTruncated = true; break; }

						const FString Key = Spawner->GetSpawnerSignature().ToString();
						NodeActionCache().Add(Key, Spawner);

						TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("title"), Title);
						Obj->SetStringField(TEXT("category"), Category);
						Obj->SetStringField(TEXT("key"), Key);
						Results.Add(MakeShared<FJsonValueObject>(Obj));
					}
					if (bTruncated) { break; }
				}

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("count"), Results.Num());
				Out->SetBoolField(TEXT("truncated"), bTruncated);
				Out->SetArrayField(TEXT("actions"), Results);
				return FClaudeToolResult::Ok(NodeActSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// add_node_by_action : spawn any node found via search_node_actions.
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("add_node_by_action");
			Tool.Category = TEXT("blueprint");
			Tool.Description = TEXT("Spawn a node into the focused Blueprint's Event Graph, given a 'key' from search_node_actions. This can create ANY node the editor palette offers. Returns the new node id (use get_node_details for its pins, then connect_pins/set_pin_default). Undoable.");
			Tool.InputSchema = NodeActSchema({
				{ TEXT("key"), TEXT("Action key from search_node_actions.") },
				{ TEXT("x"), TEXT("Optional X position.") },
				{ TEXT("y"), TEXT("Optional Y position.") },
			}, { TEXT("key") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UBlueprint* BP = NodeActActiveBP();
				if (!BP || BP->UbergraphPages.Num() == 0) { return FClaudeToolResult::Error(TEXT("No Blueprint is currently focused.")); }
				UEdGraph* Graph = ClaudeWriteTools::ResolveTargetGraph(BP);
				if (!Graph) { return FClaudeToolResult::Error(TEXT("No target graph found.")); }

				FString Key;
				Args->TryGetStringField(TEXT("key"), Key);
				if (Key.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'key' (from search_node_actions).")); }
				double X = 0.0, Y = 0.0;
				Args->TryGetNumberField(TEXT("x"), X);
				Args->TryGetNumberField(TEXT("y"), Y);

				UBlueprintNodeSpawner* Spawner = nullptr;
				if (const TWeakObjectPtr<UBlueprintNodeSpawner>* Cached = NodeActionCache().Find(Key))
				{
					Spawner = Cached->Get();
				}
				if (!Spawner)
				{
					// Cache miss / stale: re-resolve by scanning for the matching signature.
					const FBlueprintActionDatabase::FActionRegistry& Registry2 = FBlueprintActionDatabase::Get().GetAllActions();
					for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : Registry2)
					{
						for (UBlueprintNodeSpawner* S : Pair.Value)
						{
							if (S && S->GetSpawnerSignature().ToString() == Key) { Spawner = S; break; }
						}
						if (Spawner) { break; }
					}
				}
				if (!Spawner) { return FClaudeToolResult::Error(TEXT("Action not found (re-run search_node_actions to refresh keys).")); }

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add node")));
				Graph->Modify();
				IBlueprintNodeBinder::FBindingSet Bindings;
				UEdGraphNode* NewNode = Spawner->Invoke(Graph, Bindings, FVector2D(static_cast<float>(X), static_cast<float>(Y)));
				if (!NewNode) { return FClaudeToolResult::Error(TEXT("The spawner did not produce a node.")); }
				FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
				Out->SetStringField(TEXT("title"), NewNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				return FClaudeToolResult::Ok(NodeActSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}
	}
}
