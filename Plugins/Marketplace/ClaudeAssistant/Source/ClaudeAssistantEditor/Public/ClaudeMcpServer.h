// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeMcpServer.h
// In-editor MCP server (JSON-RPC over HTTP on localhost). The Claude Code CLI
// connects to this to invoke editor tools from the user's Pro/Max subscription.
// This is the primary (subscription-first) agentic transport; the Direct API
// tool-use loop reuses the same FClaudeToolRegistry as a secondary path.

#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
struct FHttpServerRequest;

class CLAUDEASSISTANTEDITOR_API FClaudeMcpServer
{
public:
	static FClaudeMcpServer& Get();

	/** Bind POST /mcp on 127.0.0.1:InPort and start listening. Idempotent. */
	bool Start(int32 InPort);

	/** Unbind the route and stop. Safe to call when not running. */
	void Stop();

	bool IsRunning() const { return bRunning; }
	int32 GetPort() const { return Port; }

	/** Per-session auth token required on every MCP request (embedded in the CLI's mcp-config headers). */
	const FString& GetAuthToken() const { return AuthToken; }

private:
	FClaudeMcpServer() = default;

	/** JSON-RPC request handler: initialize / notifications/initialized / tools/list / tools/call. */
	bool HandleMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle Route;
	FString AuthToken;
	bool bRunning = false;
	int32 Port = 0;
};
