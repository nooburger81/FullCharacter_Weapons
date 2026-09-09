// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeToolRegistry.h
// Central registry of editor tools the assistant can invoke. This is the single
// backbone for the agentic path: the MCP server (subscription / Claude Code CLI)
// and the Direct API tool-use loop both serve tools from here.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Result of a tool invocation. Content is a JSON (or plain text) payload for the model. */
struct FClaudeToolResult
{
	bool bSuccess = true;
	FString Content;

	static FClaudeToolResult Ok(const FString& InContent)
	{
		FClaudeToolResult R;
		R.bSuccess = true;
		R.Content = InContent;
		return R;
	}

	static FClaudeToolResult Error(const FString& InMessage)
	{
		FClaudeToolResult R;
		R.bSuccess = false;
		R.Content = InMessage;
		return R;
	}
};

/** Tool handler. Receives the parsed JSON arguments (may be null) and returns a result. */
DECLARE_DELEGATE_RetVal_OneParam(FClaudeToolResult, FClaudeToolExecuteDelegate, const TSharedPtr<FJsonObject>& /*Args*/);

/** A single registered tool: MCP-compatible schema + a handler. */
struct FClaudeTool
{
	FString Name;
	FString Description;
	/** Category for curation/grouping, e.g. "blueprint", "level", "datatable". */
	FString Category;
	/** JSON Schema object describing the tool's input (MCP "inputSchema"). */
	TSharedPtr<FJsonObject> InputSchema;
	/** Destructive tools mutate assets and require confirmation before running. */
	bool bIsDestructive = false;
	FClaudeToolExecuteDelegate Execute;
};

/**
 * FClaudeToolRegistry
 *
 * Singleton hub. Register tools once at module startup, then serve/dispatch them
 * to whichever transport is active (MCP server or Direct API tool-use loop).
 */
class CLAUDEASSISTANTEDITOR_API FClaudeToolRegistry
{
public:
	static FClaudeToolRegistry& Get();

	void RegisterTool(FClaudeTool Tool);
	bool HasTool(const FString& Name) const;
	const FClaudeTool* FindTool(const FString& Name) const;
	int32 Num() const { return Tools.Num(); }
	TArray<FString> GetToolNames() const;

	/** MCP `tools/list` payload: array of { name, description, inputSchema } objects. */
	TArray<TSharedPtr<FJsonValue>> GetToolSchemasJson() const;

	/** Dispatch a tool call by name. Returns an error result if the tool is unknown/unbound. */
	FClaudeToolResult ExecuteTool(const FString& Name, const TSharedPtr<FJsonObject>& Args) const;

	/** Register the built-in tools (idempotent). Read-only tools first; write tools added later. */
	void RegisterBuiltinTools();

	// --- Curation: keep the MCP tool list lean as the tool count grows toward parity ---
	/** Enable/disable a single tool by name. Disabled tools are hidden from tools/list and refuse execution. */
	void SetToolEnabled(const FString& Name, bool bEnabled);
	/** Enable/disable an entire category at once (e.g. "sequencer"). */
	void SetCategoryEnabled(const FString& Category, bool bEnabled);
	/** True if the tool exists and is not disabled directly or via its category. */
	bool IsToolEnabled(const FString& Name) const;

private:
	FClaudeToolRegistry() = default;

	TMap<FString, FClaudeTool> Tools;
	TSet<FString> DisabledTools;
	TSet<FString> DisabledCategories;
	bool bBuiltinsRegistered = false;
};
