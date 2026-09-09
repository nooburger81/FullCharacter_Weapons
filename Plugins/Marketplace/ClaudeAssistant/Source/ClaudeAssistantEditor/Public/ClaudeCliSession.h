// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeCliSession.h
// Persistent Claude Code CLI session for the SUBSCRIPTION (primary) agentic path.
// Spawns `claude` in stream-json mode, points it at our in-editor MCP server via
// --mcp-config, and streams assistant text + tool activity back to the UI.
// The CLI drives the agentic loop natively; our MCP server executes the tools.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"

class FRunnableThread;
class FClaudeCliStdoutReader;
class FJsonObject;

DECLARE_DELEGATE_OneParam(FClaudeCliOnText,       const FString& /*FullText*/);
DECLARE_DELEGATE_OneParam(FClaudeCliOnTextDelta,  const FString& /*Chunk*/);
DECLARE_DELEGATE(FClaudeCliOnMessageEnd);
DECLARE_DELEGATE_OneParam(FClaudeCliOnToolCall,   const FString& /*ToolName*/);
DECLARE_DELEGATE(FClaudeCliOnTurnComplete);
DECLARE_DELEGATE_OneParam(FClaudeCliOnError,      const FString& /*Message*/);
DECLARE_DELEGATE(FClaudeCliOnExited);

/**
 * FClaudeCliSession
 *
 * Must be owned by a TSharedPtr (create via MakeShared) because the background
 * stdout reader holds a TWeakPtr back to it.
 */
class CLAUDEASSISTANTEDITOR_API FClaudeCliSession : public TSharedFromThis<FClaudeCliSession>
{
public:
	FClaudeCliSession() = default;
	~FClaudeCliSession();

	bool IsRunning() const;

	/** Spawn the CLI (idempotent). OptionalSystemPrompt is appended via --append-system-prompt. */
	bool Start(const FString& OptionalSystemPrompt);

	/** Terminate the CLI (kill tree) and join the reader thread. */
	void Stop();

	/** Send a user turn to the running CLI (JSONL on stdin). */
	void SendUserMessage(const FString& Text);

	// Invoked on the game thread by the reader thread.
	void HandleStreamMessage(const TSharedPtr<FJsonObject>& Obj);
	void HandleReaderExit(const FString& Reason);

	// Delegates (fired on the game thread).
	FClaudeCliOnText         OnAssistantText;
	FClaudeCliOnTextDelta    OnAssistantTextDelta;
	FClaudeCliOnMessageEnd   OnAssistantMessageEnd;
	FClaudeCliOnToolCall     OnToolCall;
	FClaudeCliOnTurnComplete OnTurnComplete;
	FClaudeCliOnError        OnError;
	FClaudeCliOnExited       OnExited;

private:
	FString BuildArgs(const FString& OptionalSystemPrompt) const;
	void ShutdownPipes();

	FProcHandle ProcessHandle;
	void* StdInReadPipe = nullptr;
	void* StdInWritePipe = nullptr;
	void* StdOutReadPipe = nullptr;
	void* StdOutWritePipe = nullptr;

	FClaudeCliStdoutReader* Reader = nullptr;
	FRunnableThread* ReaderThread = nullptr;
};
