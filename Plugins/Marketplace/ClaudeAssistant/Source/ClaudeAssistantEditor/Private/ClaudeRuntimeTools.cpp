// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeRuntimeTools.cpp

#include "ClaudeRuntimeTools.h"
#include "ClaudeToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"

namespace
{
	// Uniquely named to avoid ODR clashes with helpers in sibling tool files
	// when the module is compiled as a Unity build.
	FString RuntimeSerializeJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> RuntimeNoArgSchema()
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		return Schema;
	}

	bool RuntimeIsPieRunning()
	{
		return GEditor && GEditor->PlayWorld != nullptr;
	}
}

namespace ClaudeRuntimeTools
{
	void RegisterAll(FClaudeToolRegistry& Registry)
	{
		// ---------------------------------------------------------------------
		// is_pie_active
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("is_pie_active");
			Tool.Category = TEXT("runtime");
			Tool.Description = TEXT("Return whether a Play-In-Editor (PIE) session is currently running.");
			Tool.InputSchema = RuntimeNoArgSchema();
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>&) -> FClaudeToolResult
			{
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("pie_active"), RuntimeIsPieRunning());
				return FClaudeToolResult::Ok(RuntimeSerializeJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// start_pie
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("start_pie");
			Tool.Category = TEXT("runtime");
			Tool.Description = TEXT("Start a Play-In-Editor (PIE) session in the active level viewport. Fails if PIE is already running. The session starts on the next editor tick.");
			Tool.InputSchema = RuntimeNoArgSchema();
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>&) -> FClaudeToolResult
			{
				if (!GEditor) { return FClaudeToolResult::Error(TEXT("Editor is not available.")); }
				if (RuntimeIsPieRunning()) { return FClaudeToolResult::Error(TEXT("PIE is already running.")); }

				FRequestPlaySessionParams Params;
				GEditor->RequestPlaySession(Params);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("status"), TEXT("PIE start requested."));
				return FClaudeToolResult::Ok(RuntimeSerializeJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// stop_pie
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("stop_pie");
			Tool.Category = TEXT("runtime");
			Tool.Description = TEXT("Stop the running Play-In-Editor (PIE) session.");
			Tool.InputSchema = RuntimeNoArgSchema();
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>&) -> FClaudeToolResult
			{
				if (!GEditor) { return FClaudeToolResult::Error(TEXT("Editor is not available.")); }
				if (!RuntimeIsPieRunning()) { return FClaudeToolResult::Error(TEXT("No PIE session is running.")); }

				GEditor->RequestEndPlayMap();

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("status"), TEXT("PIE stop requested."));
				return FClaudeToolResult::Ok(RuntimeSerializeJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}
	}
}
