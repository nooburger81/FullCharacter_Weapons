// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAPIClient.h
// Client for communicating with Claude. Supports two backends:
//   1) Anthropic HTTP API (pay-per-token, requires API key)
//   2) Local Claude Code CLI subprocess (uses user's Pro/Max subscription via OAuth)

#pragma once

#include "CoreMinimal.h"
#include "Http.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"  // FProcHandle (transitive in 5.6, explicit needed for 5.4/5.5/5.7)

/**
 * Structure for a single message in the conversation
 */
struct CLAUDEASSISTANTCORE_API FClaudeMessage
{
	/** Role: "user" or "assistant" */
	FString Role;

	/** Message content */
	FString Content;

	FClaudeMessage() = default;

	FClaudeMessage(const FString& InRole, const FString& InContent)
		: Role(InRole), Content(InContent)
	{
	}
};

/**
 * Delegate called when the response is ready
 * @param bSuccess true if the request was successful
 * @param Response Response text or error message
 */
DECLARE_DELEGATE_TwoParams(FOnClaudeResponseReceived, bool /*bSuccess*/, const FString& /*Response*/);

/**
 * FClaudeAPIClient
 *
 * Singleton client. Routes to HTTP API or Claude CLI subprocess based on settings.
 */
class CLAUDEASSISTANTCORE_API FClaudeAPIClient
{
public:
	static FClaudeAPIClient& Get();

	/** Send a message using the currently configured auth mode */
	void SendMessage(const FString& UserMessage, FOnClaudeResponseReceived OnResponseReceived);

	/** Send message with explicit API key (forces HTTP path, for testing) */
	void SendMessageWithKey(const FString& UserMessage, const FString& APIKey, FOnClaudeResponseReceived OnResponseReceived);

	/** Send message with custom system prompt for this request */
	void SendMessageWithSystem(const FString& UserMessage, const FString& SystemPrompt, FOnClaudeResponseReceived OnResponseReceived);

	/** Test the connection with a minimal request (uses current auth mode) */
	void TestConnection(FOnClaudeResponseReceived OnResponseReceived);

	/** Clear the conversation history */
	void ClearConversationHistory();

	/** Set the conversation history (for multi-tab support) */
	void SetConversationHistory(const TArray<FClaudeMessage>& NewHistory);

	/** Returns the current conversation history */
	const TArray<FClaudeMessage>& GetConversationHistory() const { return ConversationHistory; }

	/** Check if a request is currently in progress */
	bool IsRequestInProgress() const { return bRequestInProgress; }

	/** Cancel the current request if present */
	void CancelCurrentRequest();

	/** Set a custom system prompt */
	void SetCustomSystemPrompt(const FString& InPrompt);

	/** Reset to the default system prompt */
	void ResetSystemPrompt();

	/** Get the current system prompt */
	FString GetCurrentSystemPrompt() const;

private:
	FClaudeAPIClient();

	// HTTP (API Key) backend
	FString BuildRequestBody(const FString& UserMessage, const FString& APIKey);
	void OnHttpResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	bool ParseResponse(const FString& ResponseBody, FString& OutContent, FString& OutError);
	FString ParseAPIError(const FString& ResponseBody, int32 ResponseCode) const;
	FString GetDefaultSystemPrompt() const;

	// CLI (Subscription) backend
	void SendViaCLI(const FString& UserMessage);
	FString BuildCLIPrompt() const;
	bool CLITick(float DeltaTime);
	void CLIFinish(bool bSuccess, const FString& Message);
	void CLICleanup();

	// Keeps ConversationHistory bounded by Settings->MaxConversationHistory and
	// guarantees it always starts with a "user"-role message (Anthropic requirement).
	void TrimConversationHistory();

private:
	TArray<FClaudeMessage> ConversationHistory;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> CurrentRequest;
	bool bRequestInProgress;
	FOnClaudeResponseReceived ResponseCallback;
	FString CustomSystemPrompt;
	bool bUseCustomPrompt;
	bool bIsTestRequest;

	// CLI subprocess state
	FProcHandle CLIProc;
	void* CLIStdOutRead = nullptr;
	void* CLIStdOutWrite = nullptr;
	void* CLIStdInRead = nullptr;
	void* CLIStdInWrite = nullptr;
	FString CLIStdOutBuffer;
	FString CLIStdErrBuffer;
	FTSTicker::FDelegateHandle CLITickerHandle;
	FDateTime CLIStartTime;

	static const FString APIEndpoint;
	static const FString APIVersion;
};
