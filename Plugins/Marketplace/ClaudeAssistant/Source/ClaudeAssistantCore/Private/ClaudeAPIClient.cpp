// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAPIClient.cpp
// Dual backend: Anthropic HTTP API (API key) OR local Claude Code CLI subprocess (subscription).

#include "ClaudeAPIClient.h"
#include "ClaudeAssistantSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

// API Configuration
const FString FClaudeAPIClient::APIEndpoint = TEXT("https://api.anthropic.com/v1/messages");
const FString FClaudeAPIClient::APIVersion = TEXT("2023-06-01");

DEFINE_LOG_CATEGORY_STATIC(LogClaudeAPI, Log, All);

FClaudeAPIClient& FClaudeAPIClient::Get()
{
	static FClaudeAPIClient Instance;
	return Instance;
}

FClaudeAPIClient::FClaudeAPIClient()
	: bRequestInProgress(false)
	, bUseCustomPrompt(false)
	, bIsTestRequest(false)
{
}

void FClaudeAPIClient::SendMessage(const FString& UserMessage, FOnClaudeResponseReceived OnResponseReceived)
{
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();

	if (Settings->AuthMode == EClaudeAuthMode::SubscriptionCLI)
	{
		if (bRequestInProgress)
		{
			// Reset test flag to avoid leaking it into the next real request.
			bIsTestRequest = false;
			OnResponseReceived.ExecuteIfBound(false, TEXT("A request is already in progress. Please wait."));
			return;
		}

		ResponseCallback = OnResponseReceived;
		bRequestInProgress = true;

		if (!bIsTestRequest)
		{
			ConversationHistory.Add(FClaudeMessage(TEXT("user"), UserMessage));
		}

		SendViaCLI(UserMessage);
		return;
	}

	// API Key path
	FString APIKey = Settings->GetAPIKey();
	if (APIKey.IsEmpty())
	{
		bIsTestRequest = false;
		OnResponseReceived.ExecuteIfBound(false, TEXT("API Key not configured.\n\nPlease set your API key in:\nProject Settings > Plugins > Claude Assistant\n\nYou need an Anthropic API key from https://console.anthropic.com/\nNote: A Claude Pro subscription does NOT include API access.\n\nAlternative: switch Auth Mode to 'Claude Pro/Max Subscription' to use the Claude Code CLI instead."));
		return;
	}

	SendMessageWithKey(UserMessage, APIKey, OnResponseReceived);
}

void FClaudeAPIClient::SendMessageWithKey(const FString& UserMessage, const FString& APIKey, FOnClaudeResponseReceived OnResponseReceived)
{
	if (bRequestInProgress)
	{
		bIsTestRequest = false;
		OnResponseReceived.ExecuteIfBound(false, TEXT("A request is already in progress. Please wait."));
		return;
	}

	if (APIKey.IsEmpty())
	{
		bIsTestRequest = false;
		OnResponseReceived.ExecuteIfBound(false, TEXT("API Key is empty."));
		return;
	}

	ResponseCallback = OnResponseReceived;
	bRequestInProgress = true;

	// Add user message to history (skip for test requests)
	if (!bIsTestRequest)
	{
		ConversationHistory.Add(FClaudeMessage(TEXT("user"), UserMessage));
	}

	// Read settings
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
	FString ModelId = Settings->GetModelId();
	int32 MaxTokens = bIsTestRequest ? 16 : Settings->MaxTokens;

	// Create HTTP request
	CurrentRequest = FHttpModule::Get().CreateRequest();
	CurrentRequest->SetURL(APIEndpoint);
	CurrentRequest->SetVerb(TEXT("POST"));

	// Headers required by Anthropic
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	CurrentRequest->SetHeader(TEXT("x-api-key"), APIKey);
	CurrentRequest->SetHeader(TEXT("anthropic-version"), APIVersion);

	// Build and set the body
	FString RequestBody = BuildRequestBody(UserMessage, APIKey);
	CurrentRequest->SetContentAsString(RequestBody);

	// Callback for response
	CurrentRequest->OnProcessRequestComplete().BindRaw(this, &FClaudeAPIClient::OnHttpResponseReceived);

	UE_LOG(LogClaudeAPI, Log, TEXT("Sending request to Claude API..."));
	UE_LOG(LogClaudeAPI, Log, TEXT("  Endpoint: %s"), *APIEndpoint);
	UE_LOG(LogClaudeAPI, Log, TEXT("  Model: %s"), *ModelId);
	UE_LOG(LogClaudeAPI, Log, TEXT("  Max Tokens: %d"), MaxTokens);
	UE_LOG(LogClaudeAPI, Log, TEXT("  API Version: %s"), *APIVersion);
	UE_LOG(LogClaudeAPI, Log, TEXT("  API Key configured: %s"), APIKey.IsEmpty() ? TEXT("NO") : TEXT("YES (hidden)"));

	CurrentRequest->ProcessRequest();
}

FString FClaudeAPIClient::BuildRequestBody(const FString& UserMessage, const FString& APIKey)
{
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();

	TSharedPtr<FJsonObject> RootObject = MakeShareable(new FJsonObject);

	RootObject->SetStringField(TEXT("model"), Settings->GetModelId());
	RootObject->SetNumberField(TEXT("max_tokens"), bIsTestRequest ? 16 : Settings->MaxTokens);
	RootObject->SetStringField(TEXT("system"), GetCurrentSystemPrompt());

	// Build the messages array
	TArray<TSharedPtr<FJsonValue>> MessagesArray;

	if (bIsTestRequest)
	{
		// Test request: single minimal message
		TSharedPtr<FJsonObject> MessageObject = MakeShareable(new FJsonObject);
		MessageObject->SetStringField(TEXT("role"), TEXT("user"));
		MessageObject->SetStringField(TEXT("content"), UserMessage);
		MessagesArray.Add(MakeShareable(new FJsonValueObject(MessageObject)));
	}
	else
	{
		for (const FClaudeMessage& Message : ConversationHistory)
		{
			TSharedPtr<FJsonObject> MessageObject = MakeShareable(new FJsonObject);
			MessageObject->SetStringField(TEXT("role"), Message.Role);
			MessageObject->SetStringField(TEXT("content"), Message.Content);
			MessagesArray.Add(MakeShareable(new FJsonValueObject(MessageObject)));
		}
	}

	RootObject->SetArrayField(TEXT("messages"), MessagesArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);

	return OutputString;
}

void FClaudeAPIClient::OnHttpResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bRequestInProgress = false;
	CurrentRequest.Reset();

	bool bWasTest = bIsTestRequest;
	bIsTestRequest = false;

	if (!bWasSuccessful || !Response.IsValid())
	{
		FString ErrorDetails;
		if (!bWasSuccessful)
		{
			// Get detailed status information
			FString StatusInfo = TEXT("Unknown");
			if (Request.IsValid())
			{
				EHttpRequestStatus::Type Status = Request->GetStatus();
				if (Status == EHttpRequestStatus::Failed) StatusInfo = TEXT("Failed - SSL/TLS or network error");
				else if (Status == EHttpRequestStatus::Processing) StatusInfo = TEXT("Timeout");
				UE_LOG(LogClaudeAPI, Error, TEXT("Request Status: %s"), *StatusInfo);
			}
			ErrorDetails = FString::Printf(TEXT("HTTP request failed: %s"), *StatusInfo);
		}
		else if (!Response.IsValid())
		{
			ErrorDetails = TEXT("Response object is invalid");
		}

		UE_LOG(LogClaudeAPI, Error, TEXT("HTTP request failed: %s"), *ErrorDetails);
		UE_LOG(LogClaudeAPI, Error, TEXT("Endpoint: %s"), *APIEndpoint);

		if (!bWasTest && ConversationHistory.Num() > 0)
		{
			ConversationHistory.RemoveAt(ConversationHistory.Num() - 1);
		}

		ResponseCallback.ExecuteIfBound(false, FString::Printf(TEXT("Connection error: %s\n\nPlease check:\n1. Internet connection\n2. Firewall settings\n3. API endpoint accessibility"), *ErrorDetails));
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	FString ResponseBody = Response->GetContentAsString();

	UE_LOG(LogClaudeAPI, Log, TEXT("Response code: %d"), ResponseCode);

	if (ResponseCode != 200)
	{
		// Parse the actual error from the API response body
		FString ErrorMessage = ParseAPIError(ResponseBody, ResponseCode);

		UE_LOG(LogClaudeAPI, Error, TEXT("API Error Response: %s"), *ResponseBody);

		if (!bWasTest && ConversationHistory.Num() > 0)
		{
			ConversationHistory.RemoveAt(ConversationHistory.Num() - 1);
		}

		ResponseCallback.ExecuteIfBound(false, ErrorMessage);
		return;
	}

	FString Content;
	FString ParseError;

	if (!ParseResponse(ResponseBody, Content, ParseError))
	{
		UE_LOG(LogClaudeAPI, Error, TEXT("Failed to parse response: %s"), *ParseError);

		if (!bWasTest && ConversationHistory.Num() > 0)
		{
			ConversationHistory.RemoveAt(ConversationHistory.Num() - 1);
		}

		ResponseCallback.ExecuteIfBound(false, FString::Printf(TEXT("Failed to parse response: %s"), *ParseError));
		return;
	}

	// Add response to history (skip for test requests)
	if (!bWasTest)
	{
		ConversationHistory.Add(FClaudeMessage(TEXT("assistant"), Content));
		TrimConversationHistory();
	}

	UE_LOG(LogClaudeAPI, Log, TEXT("Successfully received response from Claude (%d chars)"), Content.Len());

	ResponseCallback.ExecuteIfBound(true, Content);
}

bool FClaudeAPIClient::ParseResponse(const FString& ResponseBody, FString& OutContent, FString& OutError)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		OutError = TEXT("Invalid JSON response");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ContentArray;
	if (!JsonObject->TryGetArrayField(TEXT("content"), ContentArray))
	{
		OutError = TEXT("Missing 'content' field in response");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
	{
		const TSharedPtr<FJsonObject>* ContentObject;
		if (ContentValue->TryGetObject(ContentObject))
		{
			FString Type;
			if ((*ContentObject)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("text"))
			{
				FString Text;
				if ((*ContentObject)->TryGetStringField(TEXT("text"), Text))
				{
					OutContent = Text;
					return true;
				}
			}
		}
	}

	OutError = TEXT("No text content found in response");
	return false;
}

FString FClaudeAPIClient::ParseAPIError(const FString& ResponseBody, int32 ResponseCode) const
{
	// Try to extract the actual error message from the API response JSON
	FString APIErrorMessage;
	FString APIErrorType;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);

	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		const TSharedPtr<FJsonObject>* ErrorObject;
		if (JsonObject->TryGetObjectField(TEXT("error"), ErrorObject))
		{
			(*ErrorObject)->TryGetStringField(TEXT("message"), APIErrorMessage);
			(*ErrorObject)->TryGetStringField(TEXT("type"), APIErrorType);
		}
	}

	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
	FString ModelId = Settings->GetModelId();

	FString Result;

	switch (ResponseCode)
	{
	case 400:
		if (!APIErrorMessage.IsEmpty())
		{
			Result = FString::Printf(TEXT("Bad Request (400): %s"), *APIErrorMessage);
		}
		else
		{
			Result = TEXT("Bad Request (400). The request could not be processed.");
		}
		Result += FString::Printf(TEXT("\n\nCurrent model: %s\nMax tokens: %d\n\nTry changing the model in Project Settings > Plugins > Claude Assistant."), *ModelId, Settings->MaxTokens);
		break;
	case 401:
		Result = TEXT("Authentication Error (401): Invalid API key.\n\nPlease check your API key in Project Settings > Plugins > Claude Assistant.\n\nIMPORTANT: You need an Anthropic API key from https://console.anthropic.com/\nA Claude Pro subscription does NOT include API access - they are separate services.\n\nAlternative: switch Auth Mode to 'Claude Pro/Max Subscription' to use your subscription via the Claude Code CLI.");
		if (!APIErrorMessage.IsEmpty())
		{
			Result += FString::Printf(TEXT("\n\nDetails: %s"), *APIErrorMessage);
		}
		break;
	case 403:
		Result = TEXT("Forbidden (403): Your API key does not have access to this resource.");
		if (!APIErrorMessage.IsEmpty())
		{
			Result += FString::Printf(TEXT("\n\nDetails: %s"), *APIErrorMessage);
		}
		Result += FString::Printf(TEXT("\n\nCurrent model: %s\nTry a different model in Project Settings > Plugins > Claude Assistant."), *ModelId);
		break;
	case 404:
		Result = FString::Printf(TEXT("Not Found (404): The model \"%s\" may not be available on your API plan.\n\nTry changing the model in Project Settings > Plugins > Claude Assistant."), *ModelId);
		if (!APIErrorMessage.IsEmpty())
		{
			Result += FString::Printf(TEXT("\n\nDetails: %s"), *APIErrorMessage);
		}
		break;
	case 429:
		Result = TEXT("Rate Limit Exceeded (429): Too many requests. Please wait a moment and try again.");
		if (!APIErrorMessage.IsEmpty())
		{
			Result += FString::Printf(TEXT("\n\nDetails: %s"), *APIErrorMessage);
		}
		break;
	case 500:
	case 502:
	case 503:
		Result = FString::Printf(TEXT("Server Error (%d): Claude API is temporarily unavailable. Please try again later."), ResponseCode);
		if (!APIErrorMessage.IsEmpty())
		{
			Result += FString::Printf(TEXT("\n\nDetails: %s"), *APIErrorMessage);
		}
		break;
	case 529:
		Result = TEXT("API Overloaded (529): Claude API is currently overloaded. Please try again later.");
		break;
	default:
		if (!APIErrorMessage.IsEmpty())
		{
			Result = FString::Printf(TEXT("API Error (%d): %s"), ResponseCode, *APIErrorMessage);
		}
		else
		{
			Result = FString::Printf(TEXT("API Error (code %d). Check the Output Log for details."), ResponseCode);
		}
		break;
	}

	return Result;
}

FString FClaudeAPIClient::GetDefaultSystemPrompt() const
{
	// LIVE CODING: You can modify this prompt and apply with Hot Reload!
	return TEXT(
		"You are an AI coding assistant specialized in Unreal Engine 5 development. "
		"You help developers write C++ code for Unreal Engine 5.3+.\n\n"
		"Guidelines:\n"
		"- Always provide code that follows Unreal Engine coding standards\n"
		"- Use UPROPERTY, UFUNCTION, UCLASS macros appropriately\n"
		"- Include necessary #include directives\n"
		"- Add helpful comments in the code\n"
		"- When showing code, use markdown code blocks with cpp language identifier\n"
		"- Explain your code briefly after providing it\n"
		"- If the request is unclear, ask for clarification\n"
		"- Prefer Unreal Engine's built-in types (FString, TArray, TMap, etc.)\n"
		"- Consider performance implications in your suggestions\n"
		"- Mention any Blueprint exposure when relevant (BlueprintCallable, BlueprintReadWrite, etc.)\n\n"
		"The user is working in the Unreal Engine 5 Editor and needs practical, copy-paste ready code."
	);
}

FString FClaudeAPIClient::GetCurrentSystemPrompt() const
{
	return bUseCustomPrompt ? CustomSystemPrompt : GetDefaultSystemPrompt();
}

void FClaudeAPIClient::SetCustomSystemPrompt(const FString& InPrompt)
{
	CustomSystemPrompt = InPrompt;
	bUseCustomPrompt = !InPrompt.IsEmpty();
}

void FClaudeAPIClient::ResetSystemPrompt()
{
	CustomSystemPrompt.Empty();
	bUseCustomPrompt = false;
}

void FClaudeAPIClient::ClearConversationHistory()
{
	ConversationHistory.Empty();
	UE_LOG(LogClaudeAPI, Log, TEXT("Conversation history cleared"));
}

void FClaudeAPIClient::CancelCurrentRequest()
{
	if (bRequestInProgress)
	{
		if (CurrentRequest.IsValid())
		{
			CurrentRequest->CancelRequest();
			CurrentRequest.Reset();
		}

		if (CLIProc.IsValid() && FPlatformProcess::IsProcRunning(CLIProc))
		{
			FPlatformProcess::TerminateProc(CLIProc, /*KillTree*/ true);
		}
		CLICleanup();

		bRequestInProgress = false;
		bIsTestRequest = false;

		if (ConversationHistory.Num() > 0 && ConversationHistory.Last().Role == TEXT("user"))
		{
			ConversationHistory.RemoveAt(ConversationHistory.Num() - 1);
		}

		// Unbind so the late HTTP cancellation callback can't deliver a zombie
		// response/error to the UI after the user explicitly stopped.
		ResponseCallback.Unbind();

		UE_LOG(LogClaudeAPI, Log, TEXT("Request cancelled"));
	}
}

void FClaudeAPIClient::SendMessageWithSystem(const FString& UserMessage, const FString& SystemPrompt, FOnClaudeResponseReceived OnResponseReceived)
{
	SetCustomSystemPrompt(SystemPrompt);
	SendMessage(UserMessage, OnResponseReceived);
}

void FClaudeAPIClient::SetConversationHistory(const TArray<FClaudeMessage>& NewHistory)
{
	ConversationHistory = NewHistory;
}

void FClaudeAPIClient::TestConnection(FOnClaudeResponseReceived OnResponseReceived)
{
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();

	bIsTestRequest = true;

	if (Settings->AuthMode == EClaudeAuthMode::SubscriptionCLI)
	{
		SendMessage(TEXT("Reply with exactly: OK"), OnResponseReceived);
		return;
	}

	FString APIKey = Settings->GetAPIKey();
	if (APIKey.IsEmpty())
	{
		bIsTestRequest = false;
		OnResponseReceived.ExecuteIfBound(false, TEXT("API Key not configured.\n\nPlease set your API key in:\nProject Settings > Plugins > Claude Assistant"));
		return;
	}

	SendMessageWithKey(TEXT("Hi"), APIKey, OnResponseReceived);
}

// =============================================================================
// CLI (Subscription) backend
// =============================================================================

FString FClaudeAPIClient::BuildCLIPrompt() const
{
	// Concatenate system prompt + conversation into a single prompt fed via stdin.
	// CLI session_id tracking is avoided on purpose so tab-switching / multi-conversation
	// support keeps working with a single FClaudeAPIClient instance (same pattern as the HTTP path).
	FString Out;
	Out.Reserve(4096);

	Out += TEXT("[System]\n");
	Out += GetCurrentSystemPrompt();
	Out += TEXT("\n\n");

	if (bIsTestRequest)
	{
		// Test requests don't mutate ConversationHistory; the last user message is passed directly.
		// Use a minimal prompt to keep the round-trip cheap.
		Out += TEXT("[User]\nReply with exactly: OK\n");
		return Out;
	}

	for (int32 i = 0; i < ConversationHistory.Num(); ++i)
	{
		const FClaudeMessage& Msg = ConversationHistory[i];
		const bool bIsUser = Msg.Role == TEXT("user");
		Out += bIsUser ? TEXT("[User]\n") : TEXT("[Assistant]\n");
		Out += Msg.Content;
		Out += TEXT("\n\n");
	}

	return Out;
}

void FClaudeAPIClient::SendViaCLI(const FString& UserMessage)
{
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
	const FString ExePath = Settings->GetEffectiveCLIPath();

	// Build args. -p with no positional + stdin = non-interactive read from stdin.
	// --output-format json = single JSON object on stdout we can parse at EOF.
	// --permission-mode dontAsk + empty --allowedTools = chat-only, no tool invocation.
	FString Params = TEXT("-p --input-format text --output-format json --permission-mode dontAsk --allowedTools \"\"");
	if (!Settings->ExtraCLIArgs.IsEmpty())
	{
		Params += TEXT(" ");
		Params += Settings->ExtraCLIArgs;
	}

	// Create pipes: stdout (child writes, parent reads) and stdin (parent writes, child reads).
	if (!FPlatformProcess::CreatePipe(CLIStdOutRead, CLIStdOutWrite))
	{
		CLIFinish(false, TEXT("Failed to create stdout pipe for Claude CLI."));
		return;
	}
	// bWritePipeLocal = true marks the parent-side write handle as non-inheritable
	// so the child process doesn't inherit the parent's write end of its own stdin pipe.
	if (!FPlatformProcess::CreatePipe(CLIStdInRead, CLIStdInWrite, /*bWritePipeLocal=*/ true))
	{
		CLIFinish(false, TEXT("Failed to create stdin pipe for Claude CLI."));
		return;
	}

	// Scrub ANTHROPIC_API_KEY from child env so CLI uses OAuth (subscription billing), not API key billing.
	// The child process inherits env at CreateProc time; we restore immediately after spawn.
	const FString SavedAPIKey = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_API_KEY"));
	if (!SavedAPIKey.IsEmpty())
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("ANTHROPIC_API_KEY"), TEXT(""));
	}

	UE_LOG(LogClaudeAPI, Log, TEXT("Spawning Claude CLI: %s %s"), *ExePath, *Params);

	CLIProc = FPlatformProcess::CreateProc(
		*ExePath,
		*Params,
		/*bLaunchDetached*/ false,
		/*bLaunchHidden*/ true,
		/*bLaunchReallyHidden*/ true,
		/*OutProcessID*/ nullptr,
		/*PriorityModifier*/ 0,
		/*OptionalWorkingDirectory*/ nullptr,
		/*PipeWriteChild (stdout)*/ CLIStdOutWrite,
		/*PipeReadChild (stdin)*/ CLIStdInRead
	);

	// Restore env var regardless of success.
	if (!SavedAPIKey.IsEmpty())
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("ANTHROPIC_API_KEY"), *SavedAPIKey);
	}

	if (!CLIProc.IsValid())
	{
		CLIFinish(false, FString::Printf(TEXT("Failed to launch Claude CLI at '%s'.\n\nInstall from https://claude.com/download and make sure 'claude' is on PATH or configure the Claude CLI Path in Project Settings."), *ExePath));
		return;
	}

	// Parent no longer needs its copy of the child-side handles. Closing them lets EOF
	// propagate to the child once we close our write end of stdin below.
	FPlatformProcess::ClosePipe(nullptr, CLIStdOutWrite);
	CLIStdOutWrite = nullptr;
	FPlatformProcess::ClosePipe(CLIStdInRead, nullptr);
	CLIStdInRead = nullptr;

	// Write prompt to stdin, then close stdin to signal EOF.
	// FPlatformProcess::WritePipe(FString) internally converts to UTF-8 on Windows.
	const FString Prompt = BuildCLIPrompt();
	if (!Prompt.IsEmpty())
	{
		FPlatformProcess::WritePipe(CLIStdInWrite, Prompt);
	}
	FPlatformProcess::ClosePipe(nullptr, CLIStdInWrite);
	CLIStdInWrite = nullptr;

	// Start polling stdout on the game thread (ticker). Non-blocking.
	CLIStartTime = FDateTime::UtcNow();
	CLITickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FClaudeAPIClient::CLITick),
		/*Delay=*/ 0.05f
	);
}

bool FClaudeAPIClient::CLITick(float /*DeltaTime*/)
{
	// Drain stdout.
	if (CLIStdOutRead)
	{
		FString Chunk = FPlatformProcess::ReadPipe(CLIStdOutRead);
		if (!Chunk.IsEmpty())
		{
			CLIStdOutBuffer += Chunk;
		}
	}

	if (CLIProc.IsValid() && FPlatformProcess::IsProcRunning(CLIProc))
	{
		// Timeout guard: kill the subprocess if it hangs past the configured threshold.
		const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
		const int32 TimeoutSec = Settings ? FMath::Max(10, Settings->CLITimeoutSeconds) : 120;
		const FTimespan Elapsed = FDateTime::UtcNow() - CLIStartTime;
		if (Elapsed.GetTotalSeconds() > (double)TimeoutSec)
		{
			UE_LOG(LogClaudeAPI, Error, TEXT("Claude CLI timed out after %d seconds, terminating subprocess."), TimeoutSec);
			FPlatformProcess::TerminateProc(CLIProc, /*KillTree*/ true);
			CLIFinish(false, FString::Printf(TEXT("Claude CLI timed out after %d seconds.\n\nThe subprocess was terminated. If your network or model is slow, raise 'CLI Timeout Seconds' in Project Settings > Plugins > Claude Assistant."), TimeoutSec));
			return false; // stop ticking
		}
		return true; // keep ticking
	}

	// Process exited: drain any remaining buffered output.
	if (CLIStdOutRead)
	{
		for (int32 i = 0; i < 8; ++i)
		{
			FString Chunk = FPlatformProcess::ReadPipe(CLIStdOutRead);
			if (Chunk.IsEmpty())
			{
				break;
			}
			CLIStdOutBuffer += Chunk;
		}
	}

	int32 ReturnCode = 0;
	bool bHaveReturnCode = false;
	if (CLIProc.IsValid())
	{
		bHaveReturnCode = FPlatformProcess::GetProcReturnCode(CLIProc, &ReturnCode);
	}

	// Parse JSON result from stdout.
	FString ExtractedText;
	FString ErrorText;

	// stdout with --output-format json should be a single top-level object. Trim whitespace and parse.
	FString Trimmed = CLIStdOutBuffer;
	Trimmed.TrimStartAndEndInline();

	if (Trimmed.IsEmpty())
	{
		ErrorText = FString::Printf(TEXT("Claude CLI returned no output (exit code %d).\n\nMake sure you ran 'claude /login' once in a terminal to authenticate your Claude Pro/Max subscription."),
			bHaveReturnCode ? ReturnCode : -1);
	}
	else
	{
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
		{
			bool bIsError = false;
			JsonObject->TryGetBoolField(TEXT("is_error"), bIsError);

			FString Subtype;
			JsonObject->TryGetStringField(TEXT("subtype"), Subtype);

			FString Result;
			JsonObject->TryGetStringField(TEXT("result"), Result);

			if (!bIsError && !Result.IsEmpty())
			{
				ExtractedText = Result;
			}
			else
			{
				FString ErrMsg = Result.IsEmpty() ? Trimmed : Result;
				ErrorText = FString::Printf(TEXT("Claude CLI error (subtype=%s): %s"), *Subtype, *ErrMsg);
			}
		}
		else
		{
			// Not a JSON object: treat first 512 chars as diagnostic.
			ErrorText = FString::Printf(TEXT("Claude CLI returned non-JSON output (exit=%d):\n%s"),
				bHaveReturnCode ? ReturnCode : -1,
				*Trimmed.Left(512));
		}
	}

	if (!ExtractedText.IsEmpty())
	{
		CLIFinish(true, ExtractedText);
	}
	else
	{
		CLIFinish(false, ErrorText);
	}

	return false; // stop ticking
}

void FClaudeAPIClient::CLIFinish(bool bSuccess, const FString& Message)
{
	const bool bWasTest = bIsTestRequest;
	bIsTestRequest = false;

	if (bSuccess && !bWasTest)
	{
		ConversationHistory.Add(FClaudeMessage(TEXT("assistant"), Message));
		TrimConversationHistory();
		UE_LOG(LogClaudeAPI, Log, TEXT("Claude CLI response (%d chars)"), Message.Len());
	}
	else if (!bSuccess)
	{
		UE_LOG(LogClaudeAPI, Error, TEXT("Claude CLI failure: %s"), *Message);
		if (!bWasTest && ConversationHistory.Num() > 0 && ConversationHistory.Last().Role == TEXT("user"))
		{
			ConversationHistory.RemoveAt(ConversationHistory.Num() - 1);
		}
	}

	CLICleanup();
	bRequestInProgress = false;

	ResponseCallback.ExecuteIfBound(bSuccess, Message);
}

void FClaudeAPIClient::TrimConversationHistory()
{
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
	int32 MaxCount = Settings ? Settings->MaxConversationHistory : 20;
	if (MaxCount < 2)
	{
		MaxCount = 2;
	}

	while (ConversationHistory.Num() > MaxCount)
	{
		ConversationHistory.RemoveAt(0);
	}

	// Anthropic API requires the first message to have role "user".
	// After trimming we may end up with a leading "assistant" turn — drop it.
	if (ConversationHistory.Num() > 0 && ConversationHistory[0].Role != TEXT("user"))
	{
		ConversationHistory.RemoveAt(0);
	}
}

void FClaudeAPIClient::CLICleanup()
{
	if (CLITickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(CLITickerHandle);
		CLITickerHandle.Reset();
	}

	if (CLIStdOutRead || CLIStdOutWrite)
	{
		FPlatformProcess::ClosePipe(CLIStdOutRead, CLIStdOutWrite);
		CLIStdOutRead = nullptr;
		CLIStdOutWrite = nullptr;
	}
	if (CLIStdInRead || CLIStdInWrite)
	{
		FPlatformProcess::ClosePipe(CLIStdInRead, CLIStdInWrite);
		CLIStdInRead = nullptr;
		CLIStdInWrite = nullptr;
	}

	if (CLIProc.IsValid())
	{
		FPlatformProcess::CloseProc(CLIProc);
		CLIProc.Reset();
	}

	CLIStdOutBuffer.Empty();
	CLIStdErrBuffer.Empty();
}
