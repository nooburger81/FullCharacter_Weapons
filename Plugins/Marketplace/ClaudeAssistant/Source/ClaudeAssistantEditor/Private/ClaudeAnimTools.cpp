// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAnimTools.cpp
// AnimGraph authoring, v1 scope: play one sequence wired to the output pose.
// (Full state machines are an intentionally deferred, separate phase.)

#include "ClaudeAnimTools.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantFocusTracker.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Root.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"

namespace
{
	FString AnimSerJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> AnimSchema(const TArray<TPair<FString, FString>>& Props, const TArray<FString>& Required)
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

	UAnimBlueprint* ResolveAnimBP(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path;
		if (Args.IsValid()) { Args->TryGetStringField(TEXT("asset_path"), Path); }
		if (!Path.IsEmpty()) { return LoadObject<UAnimBlueprint>(nullptr, *Path); }
		return Cast<UAnimBlueprint>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}

	UEdGraph* FindAnimGraph(UAnimBlueprint* AnimBP)
	{
		// The AnimGraph lives in the inherited UBlueprint::FunctionGraphs, named GN_AnimGraph.
		return FindObject<UEdGraph>(AnimBP, *UEdGraphSchema_K2::GN_AnimGraph.ToString());
	}
}

namespace ClaudeAnimTools
{
	void RegisterAll(FClaudeToolRegistry& Registry)
	{
		// ---------------------------------------------------------------------
		// add_anim_sequence_player : spawn a Sequence Player wired to output pose.
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("add_anim_sequence_player");
			Tool.Category = TEXT("anim");
			Tool.Description = TEXT("In an Animation Blueprint's AnimGraph, add a Sequence Player playing the given anim sequence and wire its Pose output to the Output Pose (Result). 'sequence' is the object path to a UAnimSequence. Operates on the focused Animation Blueprint unless asset_path is given. (State machines are not covered by this tool.) Undoable.");
			Tool.InputSchema = AnimSchema({
				{ TEXT("sequence"), TEXT("Object path to a UAnimSequence asset.") },
				{ TEXT("connect_to_output"), TEXT("Optional 'false' to skip wiring to the output pose; defaults to true.") },
				{ TEXT("asset_path"), TEXT("Optional Animation Blueprint object path; defaults to the focused asset.") },
			}, { TEXT("sequence") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UAnimBlueprint* AnimBP = ResolveAnimBP(Args);
				if (!AnimBP) { return FClaudeToolResult::Error(TEXT("No Animation Blueprint is focused (or pass asset_path).")); }

				FString SeqPath;
				Args->TryGetStringField(TEXT("sequence"), SeqPath);
				if (SeqPath.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'sequence' (object path to a UAnimSequence).")); }
				UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *SeqPath);
				if (!Seq) { return FClaudeToolResult::Error(FString::Printf(TEXT("Anim sequence '%s' not found."), *SeqPath)); }

				UEdGraph* AnimGraph = FindAnimGraph(AnimBP);
				if (!AnimGraph) { return FClaudeToolResult::Error(TEXT("This Animation Blueprint has no local AnimGraph (template/child/interface BPs may inherit it).")); }

				UAnimGraphNode_Root* RootNode = nullptr;
				for (UEdGraphNode* N : AnimGraph->Nodes)
				{
					if (UAnimGraphNode_Root* R = Cast<UAnimGraphNode_Root>(N)) { RootNode = R; break; }
				}
				if (!RootNode) { return FClaudeToolResult::Error(TEXT("Output pose node not found in the AnimGraph.")); }

				FString ConnectStr;
				Args->TryGetStringField(TEXT("connect_to_output"), ConnectStr);
				const bool bConnect = !ConnectStr.Equals(TEXT("false"), ESearchCase::IgnoreCase);

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add sequence player")));
				AnimGraph->Modify();
				RootNode->Modify();

				UAnimGraphNode_SequencePlayer* PlayerNode = nullptr;
				{
					FGraphNodeCreator<UAnimGraphNode_SequencePlayer> Creator(*AnimGraph);
					PlayerNode = Creator.CreateNode(false);
					PlayerNode->NodePosX = RootNode->NodePosX - 300;
					PlayerNode->NodePosY = RootNode->NodePosY;
					PlayerNode->SetAnimationAsset(Seq);
					Creator.Finalize();
				}

				bool bConnected = false;
				if (bConnect)
				{
					UEdGraphPin* PosePin = PlayerNode->FindPin(TEXT("Pose"), EGPD_Output);
					UEdGraphPin* ResultPin = RootNode->FindPin(TEXT("Result"), EGPD_Input);
					if (PosePin && ResultPin)
					{
						const UAnimationGraphSchema* Schema = GetDefault<UAnimationGraphSchema>();
						bConnected = Schema->TryCreateConnection(PosePin, ResultPin);
					}
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("node_id"), PlayerNode->NodeGuid.ToString());
				Out->SetBoolField(TEXT("connected_to_output"), bConnected);
				return FClaudeToolResult::Ok(AnimSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// get_anim_graph_nodes : list the AnimGraph nodes.
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("get_anim_graph_nodes");
			Tool.Category = TEXT("anim");
			Tool.Description = TEXT("List the nodes of an Animation Blueprint's AnimGraph (id, class, title). Operates on the focused Animation Blueprint unless asset_path is given.");
			Tool.InputSchema = AnimSchema({
				{ TEXT("asset_path"), TEXT("Optional Animation Blueprint object path; defaults to the focused asset.") },
			}, {});
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UAnimBlueprint* AnimBP = ResolveAnimBP(Args);
				if (!AnimBP) { return FClaudeToolResult::Error(TEXT("No Animation Blueprint is focused (or pass asset_path).")); }
				UEdGraph* AnimGraph = FindAnimGraph(AnimBP);
				if (!AnimGraph) { return FClaudeToolResult::Error(TEXT("This Animation Blueprint has no local AnimGraph.")); }

				TArray<TSharedPtr<FJsonValue>> Nodes;
				for (const UEdGraphNode* N : AnimGraph->Nodes)
				{
					if (!N) { continue; }
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("id"), N->NodeGuid.ToString());
					Obj->SetStringField(TEXT("class"), N->GetClass() ? N->GetClass()->GetName() : FString());
					Obj->SetStringField(TEXT("title"), N->GetNodeTitle(ENodeTitleType::ListView).ToString());
					Nodes.Add(MakeShared<FJsonValueObject>(Obj));
				}

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("count"), Nodes.Num());
				Out->SetArrayField(TEXT("nodes"), Nodes);
				return FClaudeToolResult::Ok(AnimSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}
	}
}
