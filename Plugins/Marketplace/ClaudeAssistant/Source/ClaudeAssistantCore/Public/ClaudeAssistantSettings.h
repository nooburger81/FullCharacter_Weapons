// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantSettings.h

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Engine/DeveloperSettings.h"
#include "Runtime/Launch/Resources/Version.h"
#include "ClaudeAssistantSettings.generated.h"

UENUM()
enum class EClaudeAuthMode : uint8
{
	APIKey          UMETA(DisplayName = "API Key (console.anthropic.com, pay-per-token)"),
	SubscriptionCLI UMETA(DisplayName = "Claude Pro/Max Subscription (via Claude Code CLI)"),
};

UENUM()
enum class EClaudeModel : uint8
{
	Opus48		UMETA(DisplayName = "Claude Opus 4.8 (Most Capable, Latest)"),
	Opus47		UMETA(DisplayName = "Claude Opus 4.7"),
	Sonnet46	UMETA(DisplayName = "Claude Sonnet 4.6 (Recommended, Balanced)"),
	Haiku45		UMETA(DisplayName = "Claude Haiku 4.5 (Fast, Cheapest)"),
	Sonnet4		UMETA(DisplayName = "Claude Sonnet 4 (Legacy)"),
	Opus4		UMETA(DisplayName = "Claude Opus 4 (Legacy)"),
	Sonnet37	UMETA(DisplayName = "Claude 3.7 Sonnet (Legacy)"),
	Haiku35		UMETA(DisplayName = "Claude 3.5 Haiku (Legacy)"),
};

// Stored per-user (EditorPerProjectUserSettings ini), NOT in the project's checked-in config,
// so the API key is never committed to source control.
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Claude Assistant"))
class CLAUDEASSISTANTCORE_API UClaudeAssistantSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UClaudeAssistantSettings() {}

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName()  const override { return TEXT("Plugins"); }
	virtual FName GetSectionName()   const override { return TEXT("ClaudeAssistant"); }
#if WITH_EDITOR
	virtual FText GetSectionText() const override { return NSLOCTEXT("ClaudeAssistant", "SectionText", "Claude Assistant"); }
	virtual FText GetSectionDescription() const override { return NSLOCTEXT("ClaudeAssistant", "SectionDesc", "Configure Claude AI Assistant. Two auth modes: API Key or Claude Pro/Max Subscription via the Claude Code CLI."); }
#endif

	static FName GetContainerNameStatic()  { return TEXT("Project"); }
	static FName GetCategoryNameStatic()   { return TEXT("Plugins"); }
	static FName GetSectionNameStatic()    { return TEXT("ClaudeAssistant"); }

	// Returns the configured API key. Persisted in the per-user editor ini (not source-controlled);
	// still cleartext at rest on the local machine. No encryption is performed here.
	FString GetAPIKey() const { return APIKey; }

	/** Returns true if the selected auth mode is configured and ready to use. */
	bool HasAuth() const;

	/** Returns the API model ID string for the selected model */
	FString GetModelId() const;

	/** Returns the resolved Claude CLI executable path. Order: explicit path → autodetected install → "claude" on PATH. */
	FString GetEffectiveCLIPath() const;

	/** Scans well-known install locations for claude.exe and returns the first one found, or empty string. */
	static FString AutoDetectClaudeCLI();

	/** Returns the list of paths AutoDetectClaudeCLI() probes, in order. Useful for diagnostic UI. */
	static TArray<FString> GetClaudeCLICandidatePaths();

	/** Same list as GetClaudeCLICandidatePaths() but with env vars left as placeholders (e.g. %USERPROFILE%) for safe display. */
	static TArray<FString> GetClaudeCLICandidatePathsForDisplay();

public:
	// Which auth path to use for requests.
	UPROPERTY(config, EditAnywhere, Category = "Authentication", meta = (DisplayName = "Auth Mode"))
	EClaudeAuthMode AuthMode = EClaudeAuthMode::APIKey;

	// Anthropic API Key - starts with "sk-ant-"
	// Used only when Auth Mode = API Key.
	// Requires a separate Anthropic API account at console.anthropic.com.
	// A Claude Pro subscription does NOT include API access.
	UPROPERTY(config, EditAnywhere, Category = "Authentication", meta = (DisplayName = "API Key", EditCondition = "AuthMode == EClaudeAuthMode::APIKey"))
	FString APIKey;

	// Absolute path to claude.exe (or 'claude' on macOS/Linux). Leave empty to use PATH.
	// Required when Auth Mode = Subscription. Install from https://claude.com/download and run `claude /login` once.
	UPROPERTY(config, EditAnywhere, Category = "Authentication", meta = (DisplayName = "Claude CLI Path", EditCondition = "AuthMode == EClaudeAuthMode::SubscriptionCLI"))
	FString ClaudeCLIPath;

	// Extra args appended to the Claude CLI invocation (advanced).
	UPROPERTY(config, EditAnywhere, Category = "Authentication", meta = (DisplayName = "Extra CLI Args", EditCondition = "AuthMode == EClaudeAuthMode::SubscriptionCLI"))
	FString ExtraCLIArgs;

	// Max seconds to wait for the Claude CLI subprocess to respond before aborting.
	UPROPERTY(config, EditAnywhere, Category = "Authentication", meta = (ClampMin = "10", ClampMax = "600", DisplayName = "CLI Timeout Seconds", EditCondition = "AuthMode == EClaudeAuthMode::SubscriptionCLI"))
	int32 CLITimeoutSeconds = 120;

	// Localhost port for the in-editor MCP server that exposes editor tools to the Claude Code CLI (Subscription mode, agentic).
	UPROPERTY(config, EditAnywhere, Category = "Authentication", meta = (ClampMin = "1024", ClampMax = "65535", DisplayName = "MCP Server Port", EditCondition = "AuthMode == EClaudeAuthMode::SubscriptionCLI"))
	int32 McpServerPort = 7777;

	// Auto-approve tool calls when Claude (Subscription/CLI) invokes editor tools via MCP, so the
	// agentic loop doesn't stall on permission prompts in headless mode. Read-only today; a future
	// per-tool confirmation gate will cover destructive write tools regardless of this flag.
	UPROPERTY(config, EditAnywhere, Category = "Authentication", meta = (DisplayName = "Auto-Approve Editor Tools", EditCondition = "AuthMode == EClaudeAuthMode::SubscriptionCLI"))
	bool bAutoApproveMcpTools = true;

	// Show a confirmation dialog (with the script) before running a Python script Claude supplies
	// via the run_python tool. run_python can execute arbitrary editor code — keep ON unless you
	// fully trust the flow.
	UPROPERTY(config, EditAnywhere, Category = "Behavior", meta = (DisplayName = "Confirm Before Running Python"))
	bool bConfirmRunPython = true;

	// Curation: tool categories to disable. Disabled categories are hidden from Claude's tool list
	// AND refused at execution. One category per entry. Known categories: "context", "blueprint",
	// "datatable", "runtime", "python". Leave empty to expose everything. Example: add "python" to
	// lock down the run_python escape hatch, or "runtime" to forbid starting/stopping PIE.
	UPROPERTY(config, EditAnywhere, Category = "Tools", meta = (DisplayName = "Disabled Tool Categories"))
	TArray<FString> DisabledToolCategories;

	// Claude model to use for requests (API Key mode only; Subscription mode uses the CLI default)
	UPROPERTY(config, EditAnywhere, Category = "API Configuration", meta = (DisplayName = "Model", EditCondition = "AuthMode == EClaudeAuthMode::APIKey"))
	EClaudeModel Model = EClaudeModel::Sonnet46;

	// Maximum number of tokens in Claude's response (API Key mode only)
	UPROPERTY(config, EditAnywhere, Category = "API Configuration", meta = (ClampMin = "256", ClampMax = "32768", DisplayName = "Max Response Tokens", EditCondition = "AuthMode == EClaudeAuthMode::APIKey"))
	int32 MaxTokens = 4096;

	UPROPERTY(config, EditAnywhere, Category = "Behavior", meta = (ClampMin = "2", ClampMax = "50", DisplayName = "Max Conversation History"))
	int32 MaxConversationHistory = 20;

	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (DisplayName = "Show Timestamps"))
	bool bShowTimestamps = false;

	UPROPERTY(config, EditAnywhere, Category = "Behavior", meta = (DisplayName = "Auto Copy Code to Clipboard"))
	bool bAutoCopyCode = false;

	// --- Feedback (links to pixelsdesign.it) ----------------------------------
	// Buttons are rendered by FClaudeSettingsDetails (editor detail customization);
	// UFUNCTION(CallInEditor) rows are not reliably shown by the settings details view.
	// Opens the "Suggest a Feature" page on pixelsdesign.it in the default browser.
	static void OpenFeatureRequest();

	// Opens the "Report a Bug" page on pixelsdesign.it in the default browser.
	static void OpenBugReport();
};
