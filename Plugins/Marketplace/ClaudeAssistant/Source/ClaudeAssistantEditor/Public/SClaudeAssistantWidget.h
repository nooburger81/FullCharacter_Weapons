// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// SClaudeAssistantWidget.h
// Main widget with multi-tab support and file management

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "ClaudeAPIClient.h"

class SMultiLineEditableTextBox;
class SEditableTextBox;
class SScrollBox;
class SButton;
class STextBlock;
class SThrobber;
class SCheckBox;
class FClaudeCliSession;

struct FChatMessage
{
	bool bIsUserMessage;
	FString Content;
	FDateTime Timestamp;

	FChatMessage() : bIsUserMessage(true), Timestamp(FDateTime::Now()) {}
	FChatMessage(bool bInIsUser, const FString& InContent)
		: bIsUserMessage(bInIsUser), Content(InContent), Timestamp(FDateTime::Now()) {}
};

struct FChatTab
{
	FString TabName;
	TArray<FChatMessage> Messages;
	TArray<struct FClaudeMessage> APIHistory;
	TArray<FString> UserMessageHistory;  // History of sent messages for arrow navigation
	int32 HistoryIndex;  // Current position in history (-1 = new message)
	FString CurrentInputBuffer;  // Buffer for current input when navigating history

	FChatTab() : TabName(TEXT("Chat 1")), HistoryIndex(-1) {}
	FChatTab(const FString& Name) : TabName(Name), HistoryIndex(-1) {}
};

// Structure for files extracted from responses
struct FCodeFile
{
	FString FileName;
	FString Content;
	bool bIsHeader;
};

// Structure for attached files
struct FAttachedFile
{
	FString FilePath;
	FString FileName;
	FString Content;
};

// Structure for project tasks
struct FProjectTask
{
	FString Description;
	bool bIsCompleted;
	FDateTime CreatedAt;
	FDateTime CompletedAt;

	FProjectTask() : bIsCompleted(false), CreatedAt(FDateTime::Now()) {}
	FProjectTask(const FString& InDesc, bool bCompleted = false)
		: Description(InDesc), bIsCompleted(bCompleted), CreatedAt(FDateTime::Now()) {}
};

// Structure for project state documentation
struct FProjectState
{
	FString ProjectName;
	FString LastUpdateDate;
	FString ProjectDescription;
	TArray<FProjectTask> CompletedTasks;
	TArray<FProjectTask> PendingTasks;
	TArray<FString> RecentFiles;
	FString Notes;
};

class CLAUDEASSISTANTEDITOR_API SClaudeAssistantWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClaudeAssistantWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SClaudeAssistantWidget();

private:
	// UI Building
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildTabBar();
	TSharedRef<SWidget> BuildChatArea();
	TSharedRef<SWidget> BuildInputArea();
	TSharedRef<SWidget> CreateMessageWidget(const FChatMessage& Message);

	// Tab Management
	FReply OnNewTabClicked();
	FReply OnCloseTabClicked(int32 TabIndex);
	void SwitchToTab(int32 TabIndex);
	void RefreshTabBar();
	void RefreshChatDisplay();

	// Message Handling
	void AddMessageToChat(const FChatMessage& Message);
	FReply OnSendButtonClicked();
	FReply OnClearButtonClicked();
	FReply OnCopyLastCodeClicked();
	FReply OnSettingsButtonClicked();
	void OnClaudeResponseReceived(bool bSuccess, const FString& Response);
	void OnErrorFixResponseReceived(bool bSuccess, const FString& Response);
	void SendCurrentMessage();

	// Subscription (Claude Code CLI) persistent agentic session
	void EnsureCliSession();
	FString GetAgenticSystemPrompt() const;
	void OnCliAssistantText(const FString& Text);
	void OnCliToolCall(const FString& ToolName);
	void OnCliTurnComplete();
	void OnCliError(const FString& Message);

	// Input Handling - Enter to send, Shift+Enter for newline, Arrow keys for history
	FReply OnInputKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
	void NavigateHistory(bool bGoUp);

	// File Operations
	TArray<FCodeFile> ExtractCodeFiles(const FString& Response);

	// File Attachment System
	FReply OnAttachFileClicked();
	void ShowFilePickerDialog();
	void AttachFile(const FString& FilePath);
	void RemoveAttachedFile(int32 Index);
	void RefreshAttachedFilesDisplay();
	FString BuildMessageWithAttachments(const FString& UserMessage) const;
	TArray<FString> GetProjectSourceFiles() const;

	// Project Analysis
	FReply OnAnalyzeProjectClicked();
	void AnalyzeProject();

	// Auto-Compile System
	void CompileProjectAfterFileCreation();
	void OnCompilationFinished(bool bSuccess, const FString& Output);
	void RequestErrorFix(const FString& CompileErrors);
	FString GetUnrealBuildToolPath() const;
	FString GetProjectPath() const;
	void CreateOrUpdateFiles(const TArray<FCodeFile>& Files);
	FString GetProjectSourcePath() const;
	bool WriteFileToProject(const FString& FileName, const FString& Content);
	FReply OnApplyCodeClicked();
	FReply OnCompileClicked();
	FReply OnStopClicked();
	void StopAllOperations();

	// Utilities
	TArray<FString> ExtractCodeBlocks(const FString& Text);
	FString GetWelcomeText() const;
	void SetLoadingState(bool bIsLoading);
	bool IsAPIKeyConfigured() const;
	void ShowAPIKeyWarning();
	FString GetSystemPromptWithContext() const;

	// API Test
	FReply OnTestAPIClicked();
	void OnTestAPIResponseReceived(bool bSuccess, const FString& Response);

	// Project State Management
	FReply OnSaveProjectStateClicked();
	FReply OnLoadProjectStateClicked();
	void SaveProjectState();
	bool LoadProjectState();
	FString GetProjectStateFilePath() const;
	void ShowProjectStateEditor();

private:
	// Tabs
	TArray<TSharedPtr<FChatTab>> ChatTabs;
	int32 ActiveTabIndex;
	TSharedPtr<SHorizontalBox> TabBarContainer;

	// UI Elements
	TSharedPtr<SMultiLineEditableTextBox> InputTextBox;
	TSharedPtr<SScrollBox> ChatScrollBox;
	TSharedPtr<SButton> SendButton;
	TSharedPtr<SThrobber> LoadingThrobber;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SVerticalBox> ChatMessagesContainer;
	TSharedPtr<SButton> ApplyCodeButton;
	TSharedPtr<SButton> CompileButton;
	TSharedPtr<SButton> StopButton;
	TSharedPtr<SButton> AttachFileButton;
	TSharedPtr<SButton> AnalyzeProjectButton;
	TSharedPtr<SHorizontalBox> AttachedFilesContainer;
	TSharedPtr<SCheckBox> BlueprintModeCheckbox;

	// State
	FString LastCodeBlock;
	TArray<FCodeFile> LastExtractedFiles;
	TArray<FAttachedFile> AttachedFiles;
	bool bIsWaitingForResponse;
	bool bIsCompiling;
	bool bAutoFixErrors;
	bool bBlueprintMode;
	int32 CompileRetryCount;
	int32 MaxCompileRetries;

	// Subscription (Claude Code CLI) persistent agentic session (created lazily).
	TSharedPtr<FClaudeCliSession> CliSession;

	// Project State
	FProjectState CurrentProjectState;
	TSharedPtr<SButton> SaveStateButton;
	TSharedPtr<SButton> LoadStateButton;
};
