// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// SClaudeAssistantWidget.cpp
// Main widget implementation with multi-tab, file management, and auto-compilation



#include "SClaudeAssistantWidget.h"

#include "ClaudeAPIClient.h"

#include "ClaudeAssistantSettings.h"

#include "ClaudeAssistantStyle.h"
#include "ClaudeAssistantCompat.h"
#include "ClaudeAssistantFocusTracker.h"
#include "ClaudeCliSession.h"
#include "ClaudeMcpServer.h"



#include "Widgets/Input/SMultiLineEditableTextBox.h"

#include "Widgets/Input/SButton.h"

#include "Widgets/Text/STextBlock.h"

#include "Widgets/Layout/SScrollBox.h"

#include "Widgets/Layout/SBox.h"

#include "Widgets/Layout/SSeparator.h"

#include "Widgets/Layout/SBorder.h"

#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SCheckBox.h"

#include "HAL/PlatformApplicationMisc.h"

#include "HAL/PlatformProcess.h"

#include "Misc/MessageDialog.h"

#include "Misc/FileHelper.h"

#include "Misc/Paths.h"

#include "HAL/PlatformFileManager.h"

#include "ISettingsModule.h"

#include "Async/Async.h"
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"



#define LOCTEXT_NAMESPACE "ClaudeAssistantWidget"



void SClaudeAssistantWidget::Construct(const FArguments& InArgs)

{

	bIsWaitingForResponse = false;

	bIsCompiling = false;

	bAutoFixErrors = true;
	bBlueprintMode = false;

	CompileRetryCount = 0;
	MaxCompileRetries = 3;

	ActiveTabIndex = 0;



	ChatTabs.Add(MakeShared<FChatTab>(TEXT("Chat 1")));



	ChildSlot

	[

		SNew(SVerticalBox)



		+ SVerticalBox::Slot()

		.AutoHeight()

		.Padding(4.0f)

		[

			BuildToolbar()

		]



		+ SVerticalBox::Slot()

		.AutoHeight()

		.Padding(4.0f, 0.0f)

		[

			BuildTabBar()

		]



		+ SVerticalBox::Slot()

		.AutoHeight()

		[

			SNew(SSeparator)

		]



		+ SVerticalBox::Slot()

		.FillHeight(1.0f)

		.Padding(4.0f)

		[

			BuildChatArea()

		]



		+ SVerticalBox::Slot()

		.AutoHeight()

		[

			SNew(SSeparator)

		]



		+ SVerticalBox::Slot()

		.AutoHeight()

		.Padding(4.0f)

		[

			BuildInputArea()

		]

	];



	RefreshTabBar();



	if (!IsAPIKeyConfigured())

	{

		ShowAPIKeyWarning();

	}

}



SClaudeAssistantWidget::~SClaudeAssistantWidget()

{

	FClaudeAPIClient::Get().CancelCurrentRequest();

	if (CliSession.IsValid())
	{
		CliSession->Stop();
		CliSession.Reset();
	}

}



TSharedRef<SWidget> SClaudeAssistantWidget::BuildToolbar()

{

	return SNew(SHorizontalBox)



		+ SHorizontalBox::Slot()

		.AutoWidth()

		.VAlign(VAlign_Center)

		.Padding(4.0f, 0.0f)

		[

			SNew(STextBlock)

			.Text(LOCTEXT("Title", "Claude Assistant"))

			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))

		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(12.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SAssignNew(BlueprintModeCheckbox, SCheckBox)
				.IsChecked_Lambda([this]() { return bBlueprintMode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) {
					bBlueprintMode = (NewState == ECheckBoxState::Checked);
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BlueprintMode", "Blueprint Assistant"))
				.ToolTipText(LOCTEXT("BlueprintModeTooltip", "When enabled, Claude will provide Blueprint instructions instead of C++ code"))
			]
		]

		+ SHorizontalBox::Slot()

		.FillWidth(1.0f)

		[

			SNullWidget::NullWidget

		]



		+ SHorizontalBox::Slot()

		.AutoWidth()

		.VAlign(VAlign_Center)

		.Padding(4.0f, 0.0f)

		[

			SAssignNew(LoadingThrobber, SThrobber)

			.Visibility(EVisibility::Collapsed)

		]



		+ SHorizontalBox::Slot()

		.AutoWidth()

		.VAlign(VAlign_Center)

		.Padding(4.0f, 0.0f)

		[

			SAssignNew(StatusText, STextBlock)

			.Text(FText::GetEmpty())

			.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))

		]



		+ SHorizontalBox::Slot()

		.AutoWidth()

		.Padding(2.0f, 0.0f)

		[

			SAssignNew(ApplyCodeButton, SButton)

			.Text(LOCTEXT("ApplyCode", "Apply Code"))

			.ToolTipText(LOCTEXT("ApplyCodeTooltip", "Create/update files from Claude response"))

			.OnClicked(this, &SClaudeAssistantWidget::OnApplyCodeClicked)

			.IsEnabled_Lambda([this]() { return LastExtractedFiles.Num() > 0 && !bIsCompiling; })

		]



		+ SHorizontalBox::Slot()

		.AutoWidth()

		.Padding(2.0f, 0.0f)

		[

			SAssignNew(CompileButton, SButton)

			.Text(LOCTEXT("Compile", "Compile"))

			.ToolTipText(LOCTEXT("CompileTooltip", "Compile the project"))

			.OnClicked(this, &SClaudeAssistantWidget::OnCompileClicked)

			.IsEnabled_Lambda([this]() { return !bIsCompiling && !bIsWaitingForResponse; })

		]



		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SAssignNew(StopButton, SButton)
			.Text(LOCTEXT("Stop", "Stop"))
			.ToolTipText(LOCTEXT("StopTooltip", "Stop current operation"))
			.OnClicked(this, &SClaudeAssistantWidget::OnStopClicked)
			.IsEnabled_Lambda([this]() { return bIsWaitingForResponse || bIsCompiling; })
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("CopyCode", "Copy Code"))

			.ToolTipText(LOCTEXT("CopyCodeTooltip", "Copy the last code block to clipboard"))

			.OnClicked(this, &SClaudeAssistantWidget::OnCopyLastCodeClicked)

		]



		+ SHorizontalBox::Slot()

		.AutoWidth()

		.Padding(2.0f, 0.0f)

		[

			SNew(SButton)

			.Text(LOCTEXT("Clear", "Clear"))

			.ToolTipText(LOCTEXT("ClearTooltip", "Clear conversation history"))

			.OnClicked(this, &SClaudeAssistantWidget::OnClearButtonClicked)

		]



		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SAssignNew(AnalyzeProjectButton, SButton)
			.Text(LOCTEXT("AnalyzeProject", "Analyze Project"))
			.ToolTipText(LOCTEXT("AnalyzeProjectTooltip", "Analyze entire project structure. WARNING: This may consume significant API credits!"))
			.OnClicked(this, &SClaudeAssistantWidget::OnAnalyzeProjectClicked)
			.IsEnabled_Lambda([this]() { return !bIsWaitingForResponse && !bIsCompiling && IsAPIKeyConfigured(); })
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SAssignNew(AttachFileButton, SButton)
			.Text(LOCTEXT("AttachFile", "Attach File"))
			.ToolTipText(LOCTEXT("AttachFileTooltip", "Attach existing source file for context"))
			.OnClicked(this, &SClaudeAssistantWidget::OnAttachFileClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SAssignNew(SaveStateButton, SButton)
			.Text(LOCTEXT("SaveState", "Save Memory"))
			.ToolTipText(LOCTEXT("SaveStateTooltip", "Save project memory to CLAUDE_PROJECT_MEMORY.md - Claude will read this at start of each session"))
			.OnClicked(this, &SClaudeAssistantWidget::OnSaveProjectStateClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SAssignNew(LoadStateButton, SButton)
			.Text(LOCTEXT("LoadState", "Load Memory"))
			.ToolTipText(LOCTEXT("LoadStateTooltip", "Load previous project state to resume work"))
			.OnClicked(this, &SClaudeAssistantWidget::OnLoadProjectStateClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("TestAPI", "Test API"))
			.ToolTipText(LOCTEXT("TestAPITooltip", "Test API connection with a minimal request to verify your API key and model settings"))
			.OnClicked(this, &SClaudeAssistantWidget::OnTestAPIClicked)
			.IsEnabled_Lambda([this]() { return !bIsWaitingForResponse && !bIsCompiling && IsAPIKeyConfigured(); })
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Settings", "Settings"))
			.ToolTipText(LOCTEXT("SettingsTooltip", "Open plugin settings"))
			.OnClicked(this, &SClaudeAssistantWidget::OnSettingsButtonClicked)
		];

}



TSharedRef<SWidget> SClaudeAssistantWidget::BuildTabBar()

{

	return SNew(SHorizontalBox)



		+ SHorizontalBox::Slot()

		.FillWidth(1.0f)

		[

			SAssignNew(TabBarContainer, SHorizontalBox)

		]



		+ SHorizontalBox::Slot()

		.AutoWidth()

		.Padding(4.0f, 0.0f)

		[

			SNew(SButton)

			.Text(LOCTEXT("NewTab", "+"))

			.ToolTipText(LOCTEXT("NewTabTooltip", "Create new chat tab"))

			.OnClicked(this, &SClaudeAssistantWidget::OnNewTabClicked)

		];

}



FString SClaudeAssistantWidget::GetWelcomeText() const

{

	FString Text;

	Text += TEXT("Welcome to Claude Assistant!\n\n");

	Text += TEXT("I am here to help you write C++ code for Unreal Engine 5.\n");

	Text += TEXT("Just describe what you need and I will generate the code for you.\n\n");

	Text += TEXT("Controls:\n");

	Text += TEXT("- Enter: Send message\n");

	Text += TEXT("- Shift+Enter: New line\n");

	Text += TEXT("- Arrow Up/Down: Browse message history\n\n");

	Text += TEXT("Features:\n");

	Text += TEXT("- Auto-compile after file creation\n");

	Text += TEXT("- Automatic error fixing (up to 3 retries)\n");

	Text += TEXT("- Select and copy any text from chat\n\n");

	Text += TEXT("Examples:\n");

	Text += TEXT("- Create an Actor that rotates continuously\n");

	Text += TEXT("- Write a function to find the nearest player");

	return Text;

}



TSharedRef<SWidget> SClaudeAssistantWidget::BuildChatArea()

{

	return SNew(SBorder)

		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))

		.Padding(4.0f)

		[

			SAssignNew(ChatScrollBox, SScrollBox)

			+ SScrollBox::Slot()

			[

				SAssignNew(ChatMessagesContainer, SVerticalBox)



				+ SVerticalBox::Slot()

				.AutoHeight()

				.Padding(8.0f)

				[

					SNew(SMultiLineEditableTextBox)

					.Text(FText::FromString(GetWelcomeText()))

					.AutoWrapText(true)

					.IsReadOnly(true)

					.BackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f))

					.ForegroundColor(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))

				]

			]

		];

}



TSharedRef<SWidget> SClaudeAssistantWidget::BuildInputArea()

{

	return SNew(SVerticalBox)

		// Attached files row
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SAssignNew(AttachedFilesContainer, SHorizontalBox)
		]

		+ SVerticalBox::Slot()

		.AutoHeight()

		[

			SNew(SBox)

			.MinDesiredHeight(80.0f)

			.MaxDesiredHeight(200.0f)

			[

				SAssignNew(InputTextBox, SMultiLineEditableTextBox)

				.HintText(LOCTEXT("InputHint", "Describe the code you need... (Enter to send, Up/Down for history)"))

				.OnKeyDownHandler(this, &SClaudeAssistantWidget::OnInputKeyDown)

				.AutoWrapText(true)

			]

		]



		+ SVerticalBox::Slot()

		.AutoHeight()

		.HAlign(HAlign_Right)

		.Padding(0.0f, 4.0f, 0.0f, 0.0f)

		[

			SAssignNew(SendButton, SButton)

			.Text(LOCTEXT("Send", "Send"))

			.OnClicked(this, &SClaudeAssistantWidget::OnSendButtonClicked)

			.IsEnabled_Lambda([this]() { return !bIsWaitingForResponse && !bIsCompiling && IsAPIKeyConfigured(); })

		];

}



FReply SClaudeAssistantWidget::OnNewTabClicked()

{

	int32 NewTabIndex = ChatTabs.Num() + 1;

	ChatTabs.Add(MakeShared<FChatTab>(FString::Printf(TEXT("Chat %d"), NewTabIndex)));

	RefreshTabBar();

	SwitchToTab(ChatTabs.Num() - 1);

	return FReply::Handled();

}



FReply SClaudeAssistantWidget::OnCloseTabClicked(int32 TabIndex)

{

	if (ChatTabs.Num() <= 1)

	{

		return FReply::Handled();

	}



	ChatTabs.RemoveAt(TabIndex);



	if (ActiveTabIndex >= ChatTabs.Num())

	{

		ActiveTabIndex = ChatTabs.Num() - 1;

	}



	RefreshTabBar();

	RefreshChatDisplay();



	return FReply::Handled();

}



void SClaudeAssistantWidget::SwitchToTab(int32 TabIndex)

{

	if (TabIndex >= 0 && TabIndex < ChatTabs.Num())

	{

		ActiveTabIndex = TabIndex;

		RefreshTabBar();

		RefreshChatDisplay();

		FClaudeAPIClient::Get().SetConversationHistory(ChatTabs[ActiveTabIndex]->APIHistory);

	}

}



void SClaudeAssistantWidget::RefreshTabBar()

{

	if (!TabBarContainer.IsValid())

	{

		return;

	}



	TabBarContainer->ClearChildren();



	for (int32 i = 0; i < ChatTabs.Num(); ++i)

	{

		const int32 TabIndex = i;

		const bool bIsActive = (i == ActiveTabIndex);



		TabBarContainer->AddSlot()

		.AutoWidth()

		.Padding(2.0f, 0.0f)

		[

			SNew(SHorizontalBox)



			+ SHorizontalBox::Slot()

			.AutoWidth()

			[

				SNew(SButton)

				.Text(FText::FromString(ChatTabs[i]->TabName))

				.ButtonColorAndOpacity(bIsActive ? FLinearColor(0.2f, 0.4f, 0.8f) : FLinearColor(0.3f, 0.3f, 0.3f))

				.OnClicked_Lambda([this, TabIndex]() {

					SwitchToTab(TabIndex);

					return FReply::Handled();

				})

			]



			+ SHorizontalBox::Slot()

			.AutoWidth()

			[

				SNew(SButton)

				.Text(LOCTEXT("CloseTab", "x"))

				.Visibility(ChatTabs.Num() > 1 ? EVisibility::Visible : EVisibility::Collapsed)

				.OnClicked_Lambda([this, TabIndex]() {

					return OnCloseTabClicked(TabIndex);

				})

			]

		];

	}

}



void SClaudeAssistantWidget::RefreshChatDisplay()

{

	if (!ChatMessagesContainer.IsValid() || !ChatTabs.IsValidIndex(ActiveTabIndex))

	{

		return;

	}



	ChatMessagesContainer->ClearChildren();



	FChatTab& CurrentTab = *ChatTabs[ActiveTabIndex];



	if (CurrentTab.Messages.Num() == 0)

	{

		ChatMessagesContainer->AddSlot()

		.AutoHeight()

		.Padding(8.0f)

		[

			SNew(SMultiLineEditableTextBox)

			.Text(FText::FromString(GetWelcomeText()))

			.AutoWrapText(true)

			.IsReadOnly(true)

			.BackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f))

			.ForegroundColor(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))

		];

	}

	else

	{

		for (const FChatMessage& Message : CurrentTab.Messages)

		{

			ChatMessagesContainer->AddSlot()

			.AutoHeight()

			.Padding(4.0f, 2.0f)

			[

				CreateMessageWidget(Message)

			];

		}

	}



	ChatScrollBox->ScrollToEnd();

}



FReply SClaudeAssistantWidget::OnInputKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)

{

	// Enter to send (without Shift)

	if (InKeyEvent.GetKey() == EKeys::Enter && !InKeyEvent.IsShiftDown())

	{

		SendCurrentMessage();

		return FReply::Handled();

	}



	// Arrow Up - go back in history

	if (InKeyEvent.GetKey() == EKeys::Up)

	{

		NavigateHistory(true);

		return FReply::Handled();

	}



	// Arrow Down - go forward in history

	if (InKeyEvent.GetKey() == EKeys::Down)

	{

		NavigateHistory(false);

		return FReply::Handled();

	}



	return FReply::Unhandled();

}



void SClaudeAssistantWidget::NavigateHistory(bool bGoUp)

{

	if (!ChatTabs.IsValidIndex(ActiveTabIndex))

	{

		return;

	}



	FChatTab& CurrentTab = *ChatTabs[ActiveTabIndex];



	if (CurrentTab.UserMessageHistory.Num() == 0)

	{

		return;

	}



	// Save current input if we're starting to navigate

	if (CurrentTab.HistoryIndex == -1)

	{

		CurrentTab.CurrentInputBuffer = InputTextBox->GetText().ToString();

	}



	if (bGoUp)

	{

		// Go back in history

		if (CurrentTab.HistoryIndex == -1)

		{

			// Start from the most recent

			CurrentTab.HistoryIndex = CurrentTab.UserMessageHistory.Num() - 1;

		}

		else if (CurrentTab.HistoryIndex > 0)

		{

			CurrentTab.HistoryIndex--;

		}



		if (CurrentTab.HistoryIndex >= 0 && CurrentTab.HistoryIndex < CurrentTab.UserMessageHistory.Num())

		{

			InputTextBox->SetText(FText::FromString(CurrentTab.UserMessageHistory[CurrentTab.HistoryIndex]));

		}

	}

	else

	{

		// Go forward in history

		if (CurrentTab.HistoryIndex >= 0)

		{

			CurrentTab.HistoryIndex++;



			if (CurrentTab.HistoryIndex >= CurrentTab.UserMessageHistory.Num())

			{

				// Back to current input

				CurrentTab.HistoryIndex = -1;

				InputTextBox->SetText(FText::FromString(CurrentTab.CurrentInputBuffer));

			}

			else

			{

				InputTextBox->SetText(FText::FromString(CurrentTab.UserMessageHistory[CurrentTab.HistoryIndex]));

			}

		}

	}

}



FReply SClaudeAssistantWidget::OnSendButtonClicked()

{

	SendCurrentMessage();

	return FReply::Handled();

}



void SClaudeAssistantWidget::SendCurrentMessage()

{

	if (bIsWaitingForResponse || bIsCompiling)

	{

		return;

	}



	if (!IsAPIKeyConfigured())

	{

		ShowAPIKeyWarning();

		return;

	}



	FString UserMessage = InputTextBox->GetText().ToString().TrimStartAndEnd();



	if (UserMessage.IsEmpty())

	{

		return;

	}



	// Add to message history for arrow navigation

	if (ChatTabs.IsValidIndex(ActiveTabIndex))

	{

		FChatTab& CurrentTab = *ChatTabs[ActiveTabIndex];

		CurrentTab.UserMessageHistory.Add(UserMessage);

		CurrentTab.HistoryIndex = -1;  // Reset history navigation

		CurrentTab.CurrentInputBuffer.Empty();

	}



	AddMessageToChat(FChatMessage(true, UserMessage));

	InputTextBox->SetText(FText::GetEmpty());

	SetLoadingState(true);



	FString SystemPrompt = GetSystemPromptWithContext();

	// Build message with any attached files
	FString MessageWithAttachments = BuildMessageWithAttachments(UserMessage);

	// Subscription (Claude Code CLI) = primary agentic path: route to the persistent session
	// that reaches our editor tools through the local MCP server. API-key mode falls through.
	{
		const UClaudeAssistantSettings* AuthSettings = GetDefault<UClaudeAssistantSettings>();
		if (AuthSettings && AuthSettings->AuthMode == EClaudeAuthMode::SubscriptionCLI)
		{
			FClaudeMcpServer::Get().Start(AuthSettings->McpServerPort); // idempotent
			EnsureCliSession();
			// Start() surfaces its own error via OnCliError (which also clears loading);
			// only send if the session is actually up, to avoid a second error bubble.
			const bool bReady = CliSession->IsRunning() || CliSession->Start(GetAgenticSystemPrompt());
			if (bReady)
			{
				// Refresh the editor context every turn: the persistent session's system prompt
				// is fixed at start, so prepend the live focused-asset line (the model also has
				// the get_active_context tool for the authoritative current state).
				const FString Ctx = FClaudeAssistantFocusTracker::Get().GetActiveContextString();
				const FString AgenticMessage = Ctx.IsEmpty()
					? MessageWithAttachments
					: (Ctx + TEXT("\n\n") + MessageWithAttachments);
				CliSession->SendUserMessage(AgenticMessage);
			}

			AttachedFiles.Empty();
			RefreshAttachedFilesDisplay();
			return;
		}
	}

	FClaudeAPIClient::Get().SendMessageWithSystem(

		MessageWithAttachments,

		SystemPrompt,

		FOnClaudeResponseReceived::CreateSP(this, &SClaudeAssistantWidget::OnClaudeResponseReceived)

	);

	// Clear attachments after sending
	AttachedFiles.Empty();
	RefreshAttachedFilesDisplay();

}



FString SClaudeAssistantWidget::GetSystemPromptWithContext() const
{
	FString ProjectPath = GetProjectSourcePath();

	FString Prompt;

	if (bBlueprintMode)
	{
		// Blueprint Assistant mode
		Prompt += TEXT("You are an expert Unreal Engine 5 Blueprint assistant integrated directly into the editor. ");
		Prompt += TEXT("You help users create Blueprints by providing DETAILED STEP-BY-STEP INSTRUCTIONS.\n\n");
		Prompt += TEXT("IMPORTANT RULES FOR BLUEPRINT MODE:\n");
		Prompt += TEXT("1. NEVER provide C++ code. Always explain how to do things with Blueprints.\n");
		Prompt += TEXT("2. Provide clear, numbered step-by-step instructions.\n");
		Prompt += TEXT("3. Mention exact node names as they appear in the Blueprint editor.\n");
		Prompt += TEXT("4. Describe pin connections clearly (e.g., 'Connect the Return Value pin to the Target pin').\n");
		Prompt += TEXT("5. Include screenshots descriptions when helpful (e.g., 'You should see a purple node labeled...').\n");
		Prompt += TEXT("6. Mention which Blueprint type to use (Actor, Character, Widget, etc.).\n");
		Prompt += TEXT("7. Explain variable types and how to create them.\n");
		Prompt += TEXT("8. When relevant, mention the category in the right-click menu to find nodes.\n\n");
		Prompt += TEXT("FORMAT YOUR RESPONSE LIKE THIS:\n");
		Prompt += TEXT("## Blueprint: [Name]\n");
		Prompt += TEXT("**Type:** [Actor/Character/Widget/etc.]\n\n");
		Prompt += TEXT("### Steps:\n");
		Prompt += TEXT("1. First step...\n");
		Prompt += TEXT("2. Second step...\n\n");
		Prompt += TEXT("### Variables Needed:\n");
		Prompt += TEXT("- VariableName (Type): Description\n\n");
		Prompt += TEXT("### Event Graph:\n");
		Prompt += TEXT("Detailed node-by-node instructions...\n\n");
		Prompt += TEXT("The user is working in Unreal Engine 5 and needs practical Blueprint instructions they can follow immediately.");
	}
	else
	{
		// C++ mode (default)
		Prompt += TEXT("You are an expert Unreal Engine 5 C++ developer assistant integrated directly into the editor. ");
		Prompt += TEXT("You help users write, modify, and understand UE5 C++ code.\n\n");
		Prompt += TEXT("IMPORTANT: When generating code files, use this EXACT format so the plugin can extract and create files:\n");
		Prompt += TEXT("```cpp:FileName.h\n// file content here\n```\n\n");
		Prompt += TEXT("```cpp:FileName.cpp\n// file content here\n```\n\n");
		Prompt += FString::Printf(TEXT("The project source folder is: %s\n\n"), *ProjectPath);
		Prompt += TEXT("Always include both .h and .cpp files when creating new classes. ");
		Prompt += TEXT("Follow Unreal Engine coding standards and best practices. ");
		Prompt += TEXT("Include necessary #include directives and forward declarations. ");
		Prompt += TEXT("Use UCLASS, UPROPERTY, UFUNCTION macros appropriately.\n\n");
		Prompt += TEXT("CRITICAL: Use ONLY ASCII characters. NO accented characters. Write comments in English.\n\n");
		Prompt += TEXT("The code will be auto-compiled after creation. If there are compile errors, you will receive them and must fix the code.");
	}

	// Editor context awareness: tell Claude which asset the user currently has open/focused,
	// so "this blueprint / this table / this material" resolves without the user naming it.
	const FString EditorContext = FClaudeAssistantFocusTracker::Get().GetActiveContextString();
	if (!EditorContext.IsEmpty())
	{
		Prompt += TEXT("\n\n");
		Prompt += EditorContext;
		Prompt += TEXT("\nWhen the user says \"this\" asset, blueprint, table, or material, assume they mean the one above unless they clearly refer to something else.");
	}

	return Prompt;
}

void SClaudeAssistantWidget::OnClaudeResponseReceived(bool bSuccess, const FString& Response)

{

	if (!IsInGameThread())

	{

		AsyncTask(ENamedThreads::GameThread, [this, bSuccess, Response]()

		{

			OnClaudeResponseReceived(bSuccess, Response);

		});

		return;

	}



	SetLoadingState(false);



	if (bSuccess)

	{

		AddMessageToChat(FChatMessage(false, Response));



		TArray<FString> CodeBlocks = ExtractCodeBlocks(Response);

		if (CodeBlocks.Num() > 0)

		{

			LastCodeBlock = CodeBlocks.Last();



			const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();

			if (Settings->bAutoCopyCode)

			{

				FPlatformApplicationMisc::ClipboardCopy(*LastCodeBlock);

			}

		}



		LastExtractedFiles = ExtractCodeFiles(Response);

		if (LastExtractedFiles.Num() > 0)

		{

			StatusText->SetText(FText::FromString(FString::Printf(TEXT("Found %d file(s). Click 'Apply Code' to create files."), LastExtractedFiles.Num())));

		}



		if (ChatTabs.IsValidIndex(ActiveTabIndex))

		{

			ChatTabs[ActiveTabIndex]->APIHistory = FClaudeAPIClient::Get().GetConversationHistory();

		}

	}

	else

	{

		AddMessageToChat(FChatMessage(false, FString::Printf(TEXT("Error: %s"), *Response)));

	}



	ChatScrollBox->ScrollToEnd();

}



FString SClaudeAssistantWidget::GetAgenticSystemPrompt() const
{
	FString P;
	P += TEXT("You are Claude Assistant, embedded inside the Unreal Engine 5 editor. ");
	P += TEXT("You act on the asset the user currently has open by calling the provided MCP editor tools.\n\n");
	P += TEXT("Guidelines:\n");
	P += TEXT("- ALWAYS prefer a dedicated tool over run_python. Dedicated tools (get_active_context, ");
	P += TEXT("list_blueprint_variables, list_blueprint_functions, get_data_table_struct, add_variable, ");
	P += TEXT("rename_variable, delete_variable, delete_unused_variables) are reliable and need no Python API knowledge.\n");
	P += TEXT("- Use run_python ONLY for actions that have no dedicated tool. Never guess the Unreal Python API by trial and error.\n");
	P += TEXT("- If unsure which asset is focused, call get_active_context first, and operate on the focused asset.\n");
	P += TEXT("- After a mutation you may verify with the matching read tool. Keep answers concise.\n");
	P += TEXT("- Do NOT generate C++ code files or cpp: code blocks in this mode; act through the tools.");
	return P;
}

void SClaudeAssistantWidget::EnsureCliSession()
{
	if (CliSession.IsValid())
	{
		return;
	}
	CliSession = MakeShared<FClaudeCliSession>();
	CliSession->OnAssistantText.BindSP(this, &SClaudeAssistantWidget::OnCliAssistantText);
	CliSession->OnToolCall.BindSP(this, &SClaudeAssistantWidget::OnCliToolCall);
	CliSession->OnTurnComplete.BindSP(this, &SClaudeAssistantWidget::OnCliTurnComplete);
	CliSession->OnError.BindSP(this, &SClaudeAssistantWidget::OnCliError);
	CliSession->OnExited.BindSP(this, &SClaudeAssistantWidget::OnCliTurnComplete);
}

void SClaudeAssistantWidget::OnCliAssistantText(const FString& Text)
{
	AddMessageToChat(FChatMessage(false, Text));

	// Keep the Apply Code / Compile buttons working with subscription responses too.
	TArray<FString> CodeBlocks = ExtractCodeBlocks(Text);
	if (CodeBlocks.Num() > 0)
	{
		LastCodeBlock = CodeBlocks.Last();
	}
	LastExtractedFiles = ExtractCodeFiles(Text);
	if (LastExtractedFiles.Num() > 0 && StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("Found %d file(s). Click 'Apply Code' to create files."), LastExtractedFiles.Num())));
	}
}

void SClaudeAssistantWidget::OnCliToolCall(const FString& ToolName)
{
	// Only surface our own editor tools; hide Claude Code's internal tools (e.g. ToolSearch).
	const FString Prefix = TEXT("mcp__unreal-editor__");
	if (!ToolName.StartsWith(Prefix))
	{
		return;
	}
	AddMessageToChat(FChatMessage(false, FString::Printf(TEXT("[tool] %s"), *ToolName.RightChop(Prefix.Len()))));
}

void SClaudeAssistantWidget::OnCliTurnComplete()
{
	SetLoadingState(false);
}

void SClaudeAssistantWidget::OnCliError(const FString& Message)
{
	AddMessageToChat(FChatMessage(false, FString::Printf(TEXT("Error: %s"), *Message)));
	SetLoadingState(false);
}

void SClaudeAssistantWidget::AddMessageToChat(const FChatMessage& Message)

{

	if (!ChatTabs.IsValidIndex(ActiveTabIndex))

	{

		return;

	}



	ChatTabs[ActiveTabIndex]->Messages.Add(Message);



	ChatMessagesContainer->AddSlot()

		.AutoHeight()

		.Padding(4.0f, 2.0f)

		[

			CreateMessageWidget(Message)

		];



	ChatScrollBox->ScrollToEnd();

}



TSharedRef<SWidget> SClaudeAssistantWidget::CreateMessageWidget(const FChatMessage& Message)
{
	FLinearColor BackgroundColor = Message.bIsUserMessage
		? FLinearColor(0.15f, 0.15f, 0.25f, 1.0f)
		: FLinearColor(0.1f, 0.2f, 0.1f, 1.0f);

	FString RoleLabel = Message.bIsUserMessage ? TEXT("You") : TEXT("Claude");

	FString TimestampStr;
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
	if (Settings->bShowTimestamps)
	{
		TimestampStr = Message.Timestamp.ToString(TEXT(" [%H:%M]"));
	}

	// Create the content container
	TSharedRef<SVerticalBox> ContentBox = SNew(SVerticalBox);

	// Parse content for code blocks and regular text
	FString RemainingContent = Message.Content;
	int32 SearchStart = 0;

	while (true)
	{
		int32 CodeStart = RemainingContent.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);

		if (CodeStart == INDEX_NONE)
		{
			// No more code blocks, add remaining text
			FString TextPart = RemainingContent.Mid(SearchStart);
			if (!TextPart.IsEmpty())
			{
				ContentBox->AddSlot()
					.AutoHeight()
					[
						SNew(SMultiLineEditableTextBox)
						.Text(FText::FromString(TextPart))
						.AutoWrapText(true)
						.IsReadOnly(true)
						.BackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f))
						.ForegroundColor(FSlateColor(FLinearColor::White))
					];
			}
			break;
		}

		// Add text before code block
		if (CodeStart > SearchStart)
		{
			FString TextBefore = RemainingContent.Mid(SearchStart, CodeStart - SearchStart);
			if (!TextBefore.IsEmpty())
			{
				ContentBox->AddSlot()
					.AutoHeight()
					[
						SNew(SMultiLineEditableTextBox)
						.Text(FText::FromString(TextBefore))
						.AutoWrapText(true)
						.IsReadOnly(true)
						.BackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f))
						.ForegroundColor(FSlateColor(FLinearColor::White))
					];
			}
		}

		// Find end of language specifier (after ```)
		int32 LanguageEnd = RemainingContent.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, CodeStart + 3);
		if (LanguageEnd == INDEX_NONE) break;

		// Find closing ```
		int32 CodeEnd = RemainingContent.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, LanguageEnd);
		if (CodeEnd == INDEX_NONE) break;

		// Extract language and code
		FString Language = RemainingContent.Mid(CodeStart + 3, LanguageEnd - CodeStart - 3).TrimStartAndEnd();
		FString CodeContent = RemainingContent.Mid(LanguageEnd + 1, CodeEnd - LanguageEnd - 1);

		// Add code block with special styling
		ContentBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 4.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.08f, 1.0f))
				.Padding(8.0f)
				[
					SNew(SVerticalBox)
					// Language label
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Language.IsEmpty() ? TEXT("code") : Language))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.7f, 0.9f)))
					]
					// Code content with monospace font
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SMultiLineEditableTextBox)
						.Text(FText::FromString(CodeContent))
						.AutoWrapText(false)
						.IsReadOnly(true)
						.BackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f))
						.ForegroundColor(FSlateColor(FLinearColor(0.7f, 0.9f, 0.7f)))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					]
				]
			];

		SearchStart = CodeEnd + 3;
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(BackgroundColor)
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(RoleLabel + TimestampStr))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				ContentBox
			]
		];
}



FReply SClaudeAssistantWidget::OnClearButtonClicked()

{

	if (!ChatTabs.IsValidIndex(ActiveTabIndex))

	{

		return FReply::Handled();

	}



	FChatTab& CurrentTab = *ChatTabs[ActiveTabIndex];

	CurrentTab.Messages.Empty();

	CurrentTab.APIHistory.Empty();

	CurrentTab.UserMessageHistory.Empty();

	CurrentTab.HistoryIndex = -1;

	CurrentTab.CurrentInputBuffer.Empty();



	FClaudeAPIClient::Get().ClearConversationHistory();

	RefreshChatDisplay();



	LastCodeBlock.Empty();

	LastExtractedFiles.Empty();

	CompileRetryCount = 0;

	StatusText->SetText(FText::GetEmpty());



	return FReply::Handled();

}



FReply SClaudeAssistantWidget::OnCopyLastCodeClicked()

{

	if (LastCodeBlock.IsEmpty())

	{

		StatusText->SetText(LOCTEXT("NoCodeToCopy", "No code to copy"));

		return FReply::Handled();

	}



	FPlatformApplicationMisc::ClipboardCopy(*LastCodeBlock);

	StatusText->SetText(LOCTEXT("CodeCopiedStatus", "Code copied to clipboard!"));



	return FReply::Handled();

}



FReply SClaudeAssistantWidget::OnSettingsButtonClicked()
{
	// Create a popup window for quick settings
	TSharedRef<SWindow> SettingsWindow = SNew(SWindow)
		.Title(LOCTEXT("SettingsWindowTitle", "Claude Assistant Settings"))
		.ClientSize(FVector2D(350, 150))
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	TSharedRef<SVerticalBox> SettingsContent = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MaxRetries", "Max auto-fix retries:"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(10.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SSpinBox<int32>)
				.MinValue(0)
				.MaxValue(10)
				.Value(this->MaxCompileRetries)
				.OnValueChanged_Lambda([this](int32 NewValue) {
					this->MaxCompileRetries = NewValue;
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RetriesHelp", "Set to 0 to disable auto-fix"))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f)
		.HAlign(HAlign_Right)
		[
			SNew(SButton)
			.Text(LOCTEXT("OpenFullSettings", "Open Full Settings..."))
			.OnClicked_Lambda([this, &SettingsWindow]() {
				FModuleManager::LoadModuleChecked<ISettingsModule>("Settings")
					.ShowViewer("Project", "Plugins", "Claude Assistant");
				return FReply::Handled();
			})
		];

	SettingsWindow->SetContent(SettingsContent);
	FSlateApplication::Get().AddWindow(SettingsWindow);

	return FReply::Handled();
}



TArray<FCodeFile> SClaudeAssistantWidget::ExtractCodeFiles(const FString& Response)

{

	TArray<FCodeFile> Files;

	int32 SearchStart = 0;

	FString StartMarker = TEXT("```cpp:");



	while (true)

	{

		int32 BlockStart = Response.Find(StartMarker, ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchStart);

		if (BlockStart == INDEX_NONE)

		{

			break;

		}



		int32 FileNameStart = BlockStart + StartMarker.Len();

		int32 FileNameEnd = Response.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FileNameStart);

		if (FileNameEnd == INDEX_NONE)

		{

			break;

		}



		FString FileName = Response.Mid(FileNameStart, FileNameEnd - FileNameStart).TrimStartAndEnd();



		int32 ContentStart = FileNameEnd + 1;

		int32 BlockEnd = Response.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ContentStart);

		if (BlockEnd == INDEX_NONE)

		{

			break;

		}



		FString Content = Response.Mid(ContentStart, BlockEnd - ContentStart);



		FCodeFile CodeFile;

		CodeFile.FileName = FileName;

		CodeFile.Content = Content;

		CodeFile.bIsHeader = FileName.EndsWith(TEXT(".h"));



		Files.Add(CodeFile);

		SearchStart = BlockEnd + 3;

	}



	return Files;

}



FString SClaudeAssistantWidget::GetProjectSourcePath() const

{

	FString ProjectDir = FPaths::ProjectDir();

	FString SourcePath = FPaths::Combine(ProjectDir, TEXT("Source"));

	FString ProjectName = FApp::GetProjectName();

	FString ModulePath = FPaths::Combine(SourcePath, ProjectName);



	if (FPaths::DirectoryExists(ModulePath))

	{

		return ModulePath;

	}



	return SourcePath;

}



FString SClaudeAssistantWidget::GetProjectPath() const

{

	return FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());

}



FString SClaudeAssistantWidget::GetUnrealBuildToolPath() const

{

	FString EnginePath = FPaths::EngineDir();

	return FPaths::Combine(EnginePath, TEXT("Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe"));

}



bool SClaudeAssistantWidget::WriteFileToProject(const FString& FileName, const FString& Content)

{

	FString BasePath = GetProjectSourcePath();

	FString FullPath;



	if (FileName.EndsWith(TEXT(".h")))

	{

		FullPath = FPaths::Combine(BasePath, TEXT("Public"), FileName);

	}

	else if (FileName.EndsWith(TEXT(".cpp")))

	{

		FullPath = FPaths::Combine(BasePath, TEXT("Private"), FileName);

	}

	else

	{

		FullPath = FPaths::Combine(BasePath, FileName);

	}



	FString Directory = FPaths::GetPath(FullPath);

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*Directory))

	{

		PlatformFile.CreateDirectoryTree(*Directory);

	}



	bool bSuccess = FFileHelper::SaveStringToFile(Content, *FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);



	if (bSuccess)

	{

		UE_LOG(LogTemp, Log, TEXT("ClaudeAssistant: Created file %s"), *FullPath);

	}

	else

	{

		UE_LOG(LogTemp, Error, TEXT("ClaudeAssistant: Failed to create file %s"), *FullPath);

	}



	return bSuccess;

}



void SClaudeAssistantWidget::CreateOrUpdateFiles(const TArray<FCodeFile>& Files)
{
	int32 SuccessCount = 0;
	TArray<FString> CreatedFiles;
	TArray<FString> ModifiedFiles;

	FString BasePath = GetProjectSourcePath();

	for (const FCodeFile& File : Files)
	{
		// Check if file already exists
		FString FullPath;
		if (File.FileName.EndsWith(TEXT(".h")))
		{
			FullPath = FPaths::Combine(BasePath, TEXT("Public"), File.FileName);
		}
		else if (File.FileName.EndsWith(TEXT(".cpp")))
		{
			FullPath = FPaths::Combine(BasePath, TEXT("Private"), File.FileName);
		}
		else
		{
			FullPath = FPaths::Combine(BasePath, File.FileName);
		}

		bool bFileExists = FPaths::FileExists(FullPath);

		if (WriteFileToProject(File.FileName, File.Content))
		{
			SuccessCount++;
			if (bFileExists)
			{
				ModifiedFiles.Add(File.FileName);
			}
			else
			{
				CreatedFiles.Add(File.FileName);
			}
		}
	}

	// Build status message
	TArray<FString> Messages;
	if (CreatedFiles.Num() > 0)
	{
		FString FileList = FString::Join(CreatedFiles, TEXT(", "));
		Messages.Add(FString::Printf(TEXT("Created %d file(s): %s"), CreatedFiles.Num(), *FileList));
	}
	if (ModifiedFiles.Num() > 0)
	{
		FString FileList = FString::Join(ModifiedFiles, TEXT(", "));
		Messages.Add(FString::Printf(TEXT("Modified %d file(s): %s"), ModifiedFiles.Num(), *FileList));
	}

	if (SuccessCount == Files.Num())
	{
		FString StatusMsg = FString::Join(Messages, TEXT(" | "));
		AddMessageToChat(FChatMessage(false, FString::Printf(TEXT("[System] %s"), *StatusMsg)));
	}
	else
	{
		AddMessageToChat(FChatMessage(false, FString::Printf(TEXT("[System] Warning: Processed only %d of %d files"), SuccessCount, Files.Num())));
	}

}



FReply SClaudeAssistantWidget::OnApplyCodeClicked()

{

	if (LastExtractedFiles.Num() == 0 || bIsCompiling)

	{

		return FReply::Handled();

	}



	// Create files only (no compilation)

	CreateOrUpdateFiles(LastExtractedFiles);

	

	StatusText->SetText(LOCTEXT("FilesCreated", "Files created. Click 'Compile' to build."));

	LastExtractedFiles.Empty();



	return FReply::Handled();

}



FReply SClaudeAssistantWidget::OnCompileClicked()
{
	if (bIsCompiling)
	{
		return FReply::Handled();
	}

	// Reset retry count
	CompileRetryCount = 0;

	// Start compilation
	CompileProjectAfterFileCreation();

	return FReply::Handled();
}

FReply SClaudeAssistantWidget::OnStopClicked()
{
	StopAllOperations();
	return FReply::Handled();
}

void SClaudeAssistantWidget::StopAllOperations()
{
	// Actually abort the in-flight HTTP request / CLI subprocess. Without this,
	// the network call or child process keeps running, tokens get burned, and a
	// zombie response would still be appended to chat once it completes.
	FClaudeAPIClient::Get().CancelCurrentRequest();

	if (bIsWaitingForResponse)
	{
		bIsWaitingForResponse = false;
		StatusText->SetText(LOCTEXT("Stopped", "Operation stopped by user"));
		LoadingThrobber->SetVisibility(EVisibility::Collapsed);

		// Add message to chat
		AddMessageToChat(FChatMessage(false, TEXT("[Operation cancelled by user]")));
	}

	if (bIsCompiling)
	{
		bIsCompiling = false;
		CompileRetryCount = MaxCompileRetries + 1; // Prevent retries
		StatusText->SetText(LOCTEXT("CompileStopped", "Compilation stopped by user"));
		LoadingThrobber->SetVisibility(EVisibility::Collapsed);
	}
}



void SClaudeAssistantWidget::CompileProjectAfterFileCreation()
{
	bIsCompiling = true;
	LoadingThrobber->SetVisibility(EVisibility::Visible);
	StatusText->SetText(LOCTEXT("Compiling", "Starting Live Coding..."));

#if WITH_LIVE_CODING
	AddMessageToChat(FChatMessage(false, TEXT("[System] Triggering Live Coding (Ctrl+Alt+F11)...")));

	// Use Live Coding module
	ILiveCodingModule& LiveCoding = FModuleManager::LoadModuleChecked<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);

	if (!LiveCoding.IsEnabledForSession())
	{
		// Try to enable it
		LiveCoding.EnableForSession(true);
	}

	if (LiveCoding.IsEnabledForSession())
	{
		// Trigger Live Coding compile
		LiveCoding.Compile();
		StatusText->SetText(LOCTEXT("LiveCodingRunning", "Live Coding compiling..."));
		AddMessageToChat(FChatMessage(false, TEXT("[System] Live Coding started. Check the bottom-right corner for progress.")));
	}
	else
	{
		AddMessageToChat(FChatMessage(false, TEXT("[System] Could not enable Live Coding. Press Ctrl+Alt+F11 manually.")));
	}

	// Reset state - Live Coding handles its own status
	bIsCompiling = false;
	LoadingThrobber->SetVisibility(EVisibility::Collapsed);
	StatusText->SetText(LOCTEXT("LiveCodingTriggered", "Live Coding triggered - check console"));
#else
	// Live Coding is Windows-only. On Mac/Linux ask the user to compile from their IDE.
	AddMessageToChat(FChatMessage(false, TEXT("[System] Live Coding is not available on this platform. Compile the project from your IDE (e.g. Xcode), then restart the editor to load the new code.")));
	bIsCompiling = false;
	LoadingThrobber->SetVisibility(EVisibility::Collapsed);
	StatusText->SetText(LOCTEXT("LiveCodingUnavailable", "Live Coding not available on this platform - compile from IDE"));
#endif
}

void SClaudeAssistantWidget::OnCompilationFinished(bool bSuccess, const FString& Output)

{

	bIsCompiling = false;

	LoadingThrobber->SetVisibility(EVisibility::Collapsed);



	if (bSuccess)

	{

		StatusText->SetText(LOCTEXT("CompileSuccess", "Compilation successful!"));

		AddMessageToChat(FChatMessage(false, TEXT("[System] Compilation successful! You may need to restart the editor to see the new classes.")));

		CompileRetryCount = 0;

		LastExtractedFiles.Empty();

	}

	else

	{

		// Extract error messages

		FString Errors;

		TArray<FString> Lines;

		Output.ParseIntoArrayLines(Lines);



		for (const FString& Line : Lines)

		{

			if (Line.Contains(TEXT("error")) || Line.Contains(TEXT("Error")))

			{

				Errors += Line + TEXT("\n");

			}

		}



		if (Errors.IsEmpty())

		{

			Errors = TEXT("Unknown compilation error. Check the output log.");

		}



		AddMessageToChat(FChatMessage(false, FString::Printf(TEXT("[System] Compilation failed!\n\nErrors:\n%s"), *Errors)));



		CompileRetryCount++;



		if (bAutoFixErrors && CompileRetryCount <= MaxCompileRetries)

		{

			StatusText->SetText(FText::FromString(FString::Printf(TEXT("Fixing errors (attempt %d/%d)..."), CompileRetryCount, MaxCompileRetries)));

			RequestErrorFix(Errors);

		}

		else

		{

			StatusText->SetText(LOCTEXT("CompileFailed", "Compilation failed. Check errors above."));

			if (CompileRetryCount > MaxCompileRetries)

			{

				AddMessageToChat(FChatMessage(false, TEXT("[System] Max retry attempts reached. Please fix errors manually or try a different approach.")));

			}

		}

	}



	ChatScrollBox->ScrollToEnd();

}



void SClaudeAssistantWidget::RequestErrorFix(const FString& CompileErrors)

{

	if (bIsWaitingForResponse)

	{

		return;

	}



	FString FixRequest = FString::Printf(

		TEXT("The code you generated has compilation errors. Please fix them:\n\n%s\n\nProvide the corrected complete files using the same ```cpp:FileName.h format."),

		*CompileErrors

	);



	AddMessageToChat(FChatMessage(true, FixRequest));

	SetLoadingState(true);



	FString SystemPrompt = GetSystemPromptWithContext();

	SystemPrompt += TEXT("\n\nIMPORTANT: The previous code had compile errors. Fix ALL errors and provide the COMPLETE corrected files.");



	FClaudeAPIClient::Get().SendMessageWithSystem(

		FixRequest,

		SystemPrompt,

		FOnClaudeResponseReceived::CreateSP(this, &SClaudeAssistantWidget::OnErrorFixResponseReceived)

	);

}



void SClaudeAssistantWidget::OnErrorFixResponseReceived(bool bSuccess, const FString& Response)

{

	if (!IsInGameThread())

	{

		AsyncTask(ENamedThreads::GameThread, [this, bSuccess, Response]()

		{

			OnErrorFixResponseReceived(bSuccess, Response);

		});

		return;

	}



	SetLoadingState(false);



	if (bSuccess)

	{

		AddMessageToChat(FChatMessage(false, Response));



		// Extract fixed files

		TArray<FCodeFile> FixedFiles = ExtractCodeFiles(Response);



		if (FixedFiles.Num() > 0)

		{

			LastExtractedFiles = FixedFiles;



			// Save API history

			if (ChatTabs.IsValidIndex(ActiveTabIndex))

			{

				ChatTabs[ActiveTabIndex]->APIHistory = FClaudeAPIClient::Get().GetConversationHistory();

			}



			// Apply fixes and recompile

			CreateOrUpdateFiles(LastExtractedFiles);

			CompileProjectAfterFileCreation();

		}

		else

		{

			AddMessageToChat(FChatMessage(false, TEXT("[System] Could not extract fixed files from response. Please try again.")));

			StatusText->SetText(LOCTEXT("NoFixFound", "No fixed code found in response."));

		}

	}

	else

	{

		AddMessageToChat(FChatMessage(false, FString::Printf(TEXT("[System] Error getting fix: %s"), *Response)));

		StatusText->SetText(LOCTEXT("FixRequestFailed", "Failed to get error fix."));

	}



	ChatScrollBox->ScrollToEnd();

}



void SClaudeAssistantWidget::SetLoadingState(bool bIsLoading)

{

	bIsWaitingForResponse = bIsLoading;



	if (bIsLoading)

	{

		LoadingThrobber->SetVisibility(EVisibility::Visible);

		StatusText->SetText(LOCTEXT("WaitingForResponse", "Waiting for Claude..."));

		SendButton->SetEnabled(false);

	}

	else

	{

		if (!bIsCompiling)

		{

			LoadingThrobber->SetVisibility(EVisibility::Collapsed);

		}

		StatusText->SetText(FText::GetEmpty());

		SendButton->SetEnabled(true);

	}

}



TArray<FString> SClaudeAssistantWidget::ExtractCodeBlocks(const FString& Text)

{

	TArray<FString> CodeBlocks;

	int32 StartIndex = 0;

	FString Marker = TEXT("```");



	while (true)

	{

		int32 BlockStart = Text.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, StartIndex);

		if (BlockStart == INDEX_NONE)

		{

			break;

		}



		int32 ContentStart = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BlockStart + 3);

		if (ContentStart == INDEX_NONE)

		{

			break;

		}

		ContentStart++;



		int32 BlockEnd = Text.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, ContentStart);

		if (BlockEnd == INDEX_NONE)

		{

			break;

		}



		FString CodeBlock = Text.Mid(ContentStart, BlockEnd - ContentStart).TrimStartAndEnd();

		if (!CodeBlock.IsEmpty())

		{

			CodeBlocks.Add(CodeBlock);

		}



		StartIndex = BlockEnd + 3;

	}



	return CodeBlocks;

}



bool SClaudeAssistantWidget::IsAPIKeyConfigured() const

{

	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();

	return Settings && Settings->HasAuth();

}



void SClaudeAssistantWidget::ShowAPIKeyWarning()
{
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
	FString Msg;

	if (Settings && Settings->AuthMode == EClaudeAuthMode::SubscriptionCLI)
	{
		Msg += TEXT("Claude CLI not found.\n\n");
		Msg += TEXT("Auth Mode is set to 'Claude Pro/Max Subscription' but the 'claude' executable could not be located.\n\n");

		Msg += TEXT("Searched these locations:\n");
		const TArray<FString> Candidates = UClaudeAssistantSettings::GetClaudeCLICandidatePathsForDisplay();
		for (const FString& Candidate : Candidates)
		{
			Msg += TEXT("  - ");
			Msg += Candidate;
			Msg += TEXT("\n");
		}
		Msg += TEXT("  - (system PATH lookup for 'claude.exe')\n\n");

		Msg += TEXT("To fix:\n");
		Msg += TEXT("1. Install Claude Code from https://claude.com/download\n");
		Msg += TEXT("   (default install: %USERPROFILE%\\.local\\bin\\claude.exe)\n");
		Msg += TEXT("2. Open a terminal and run:  claude /login\n");
		Msg += TEXT("   (one-time OAuth, uses your Pro/Max subscription)\n");
		Msg += TEXT("3. Restart the editor\n\n");

		Msg += TEXT("If installed in a custom location, set the path manually in:\n");
		Msg += TEXT("Project Settings > Plugins > Claude Assistant > Claude CLI Path\n\n");

		Msg += TEXT("Alternative: switch Auth Mode back to 'API Key' to use console.anthropic.com.");
	}
	else
	{
		Msg += TEXT("API Key not configured!\n\n");
		Msg += TEXT("Please set your Anthropic API key in:\n");
		Msg += TEXT("Project Settings > Plugins > Claude Assistant\n\n");
		Msg += TEXT("Get an API key from:\n");
		Msg += TEXT("https://console.anthropic.com/\n\n");
		Msg += TEXT("Alternative: switch Auth Mode to 'Claude Pro/Max Subscription' to use the Claude Code CLI with your existing subscription instead of paying per token.");
	}

	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Msg));
}




// File Attachment System
FReply SClaudeAssistantWidget::OnAttachFileClicked()
{
	ShowFilePickerDialog();
	return FReply::Handled();
}

void SClaudeAssistantWidget::ShowFilePickerDialog()
{
	TArray<FString> OutFiles;
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

	if (DesktopPlatform)
	{
		FString DefaultPath = GetProjectSourcePath();
		const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);

		bool bOpened = DesktopPlatform->OpenFileDialog(
			ParentWindowHandle,
			TEXT("Select Source File to Attach"),
			DefaultPath,
			TEXT(""),
			TEXT("C++ Files (*.h;*.cpp)|*.h;*.cpp|All Files (*.*)|*.*"),
			EFileDialogFlags::Multiple,
			OutFiles
		);

		if (bOpened && OutFiles.Num() > 0)
		{
			for (const FString& FilePath : OutFiles)
			{
				AttachFile(FilePath);
			}
		}
	}
}

void SClaudeAssistantWidget::AttachFile(const FString& FilePath)
{
	// Check if already attached
	for (const FAttachedFile& Existing : AttachedFiles)
	{
		if (Existing.FilePath == FilePath)
		{
			StatusText->SetText(FText::FromString(TEXT("File already attached")));
			return;
		}
	}

	// Read file content
	FString FileContent;
	if (FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		FAttachedFile NewFile;
		NewFile.FilePath = FilePath;
		NewFile.FileName = FPaths::GetCleanFilename(FilePath);
		NewFile.Content = FileContent;
		AttachedFiles.Add(NewFile);

		RefreshAttachedFilesDisplay();
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("Attached: %s"), *NewFile.FileName)));
	}
	else
	{
		StatusText->SetText(FText::FromString(TEXT("Failed to read file")));
	}
}

void SClaudeAssistantWidget::RemoveAttachedFile(int32 Index)
{
	if (AttachedFiles.IsValidIndex(Index))
	{
		AttachedFiles.RemoveAt(Index);
		RefreshAttachedFilesDisplay();
	}
}

void SClaudeAssistantWidget::RefreshAttachedFilesDisplay()
{
	if (!AttachedFilesContainer.IsValid())
	{
		return;
	}

	AttachedFilesContainer->ClearChildren();

	if (AttachedFiles.Num() == 0)
	{
		return;
	}

	// Label
	AttachedFilesContainer->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("AttachedLabel", "Attached:"))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
		];

	for (int32 i = 0; i < AttachedFiles.Num(); ++i)
	{
		const int32 FileIndex = i;

		AttachedFilesContainer->AddSlot()
			.AutoWidth()
			.Padding(2.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.BorderBackgroundColor(FLinearColor(0.2f, 0.3f, 0.4f))
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(AttachedFiles[i].FileName))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("RemoveAttachment", "x"))
						.ButtonStyle(FAppStyle::Get(), "NoBorder")
						.OnClicked_Lambda([this, FileIndex]() {
							RemoveAttachedFile(FileIndex);
							return FReply::Handled();
						})
					]
				]
			];
	}
}

FString SClaudeAssistantWidget::BuildMessageWithAttachments(const FString& UserMessage) const
{
	if (AttachedFiles.Num() == 0)
	{
		return UserMessage;
	}

	FString Result;

	// Add attached files context
	Result += TEXT("I am providing you with the following existing source files for context:\n\n");

	for (const FAttachedFile& File : AttachedFiles)
	{
		Result += FString::Printf(TEXT("--- %s ---\n"), *File.FileName);
		Result += File.Content;
		Result += TEXT("\n--- End of ") + File.FileName + TEXT(" ---\n\n");
	}

	Result += TEXT("Based on the above files, here is my request:\n\n");
	Result += UserMessage;

	return Result;
}

TArray<FString> SClaudeAssistantWidget::GetProjectSourceFiles() const
{
	TArray<FString> SourceFiles;
	FString SourcePath = GetProjectSourcePath();

	// Find all .h and .cpp files
	IFileManager& FileManager = IFileManager::Get();
	FileManager.FindFilesRecursive(SourceFiles, *SourcePath, TEXT("*.h"), true, false, false);

	TArray<FString> CppFiles;
	FileManager.FindFilesRecursive(CppFiles, *SourcePath, TEXT("*.cpp"), true, false, false);
	SourceFiles.Append(CppFiles);

	return SourceFiles;
}


// Project Analysis
FReply SClaudeAssistantWidget::OnAnalyzeProjectClicked()
{
	// Show confirmation dialog
	FText Title = LOCTEXT("AnalyzeProjectTitle", "Analyze Project");
	FText Message = LOCTEXT("AnalyzeProjectConfirm",
		"This will send all source files in your project to Claude for analysis.\n\n"
		"WARNING: This may consume significant API credits depending on your project size!\n\n"
		"Do you want to continue?");

	EAppReturnType::Type Result = FMessageDialog::Open(EAppMsgType::YesNo, Message, Title);

	if (Result == EAppReturnType::Yes)
	{
		AnalyzeProject();
	}

	return FReply::Handled();
}

void SClaudeAssistantWidget::AnalyzeProject()
{
	if (bIsWaitingForResponse || !IsAPIKeyConfigured())
	{
		return;
	}

	// Collect all source files
	TArray<FString> SourceFiles = GetProjectSourceFiles();

	if (SourceFiles.Num() == 0)
	{
		StatusText->SetText(LOCTEXT("NoSourceFiles", "No source files found in project"));
		return;
	}

	// Build the analysis request
	FString AnalysisRequest;
	AnalysisRequest += TEXT("Please analyze my Unreal Engine project. Here are all the source files:\n\n");

	int32 TotalChars = 0;
	const int32 MaxChars = 100000; // Limit to avoid token overflow

	for (const FString& FilePath : SourceFiles)
	{
		FString FileContent;
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			FString FileName = FPaths::GetCleanFilename(FilePath);

			// Check if we exceed limit
			if (TotalChars + FileContent.Len() > MaxChars)
			{
				AnalysisRequest += TEXT("\n[Additional files truncated due to size limit]\n");
				break;
			}

			AnalysisRequest += FString::Printf(TEXT("--- %s ---\n"), *FileName);
			AnalysisRequest += FileContent;
			AnalysisRequest += TEXT("\n--- End of ") + FileName + TEXT(" ---\n\n");

			TotalChars += FileContent.Len();
		}
	}

	AnalysisRequest += TEXT("\nPlease provide:\n");
	AnalysisRequest += TEXT("1. An overview of the project architecture\n");
	AnalysisRequest += TEXT("2. Key classes and their responsibilities\n");
	AnalysisRequest += TEXT("3. Any potential issues or improvements\n");
	AnalysisRequest += TEXT("4. Suggestions for best practices\n");

	// Add to chat
	AddMessageToChat(FChatMessage(true, FString::Printf(TEXT("[Analyzing %d source files...]"), SourceFiles.Num())));
	SetLoadingState(true);
	StatusText->SetText(FText::FromString(FString::Printf(TEXT("Analyzing %d files..."), SourceFiles.Num())));

	// Send to Claude
	FString SystemPrompt = GetSystemPromptWithContext();
	SystemPrompt += TEXT("\n\nYou are analyzing an entire Unreal Engine project. Provide a comprehensive analysis.");

	FClaudeAPIClient::Get().SendMessageWithSystem(
		AnalysisRequest,
		SystemPrompt,
		FOnClaudeResponseReceived::CreateSP(this, &SClaudeAssistantWidget::OnClaudeResponseReceived)
	);

	// Save API history
	if (ChatTabs.IsValidIndex(ActiveTabIndex))
	{
		ChatTabs[ActiveTabIndex]->APIHistory = FClaudeAPIClient::Get().GetConversationHistory();
	}
}


// Project State Management Implementation
FString SClaudeAssistantWidget::GetProjectStateFilePath() const
{
	return FPaths::Combine(FPaths::ProjectDir(), FString::Printf(TEXT("%s_Doc.md"), FApp::GetProjectName()));
}

FReply SClaudeAssistantWidget::OnSaveProjectStateClicked()
{
	ShowProjectStateEditor();
	return FReply::Handled();
}

FReply SClaudeAssistantWidget::OnLoadProjectStateClicked()
{
	if (LoadProjectState())
	{
		// Build a summary message to show in chat
		FString Summary;
		Summary += TEXT("[Project State Loaded]\n\n");
		Summary += FString::Printf(TEXT("Project: %s\n"), *CurrentProjectState.ProjectName);
		Summary += FString::Printf(TEXT("Last Update: %s\n\n"), *CurrentProjectState.LastUpdateDate);

		if (!CurrentProjectState.ProjectDescription.IsEmpty())
		{
			Summary += FString::Printf(TEXT("Description:\n%s\n\n"), *CurrentProjectState.ProjectDescription);
		}

		if (CurrentProjectState.CompletedTasks.Num() > 0)
		{
			Summary += TEXT("COMPLETED TASKS:\n");
			for (const FProjectTask& Task : CurrentProjectState.CompletedTasks)
			{
				Summary += FString::Printf(TEXT("  [X] %s\n"), *Task.Description);
			}
			Summary += TEXT("\n");
		}

		if (CurrentProjectState.PendingTasks.Num() > 0)
		{
			Summary += TEXT("PENDING TASKS:\n");
			for (const FProjectTask& Task : CurrentProjectState.PendingTasks)
			{
				Summary += FString::Printf(TEXT("  [ ] %s\n"), *Task.Description);
			}
			Summary += TEXT("\n");
		}

		if (!CurrentProjectState.Notes.IsEmpty())
		{
			Summary += FString::Printf(TEXT("Notes:\n%s\n"), *CurrentProjectState.Notes);
		}

		AddMessageToChat(FChatMessage(false, Summary));
		StatusText->SetText(LOCTEXT("StateLoaded", "Project state loaded successfully"));
	}
	else
	{
		FString FilePath = GetProjectStateFilePath();
		AddMessageToChat(FChatMessage(false, FString::Printf(
			TEXT("[No project state found]\n\nNo saved state file exists at:\n%s\n\nClick 'Save State' to create one."),
			*FilePath)));
		StatusText->SetText(LOCTEXT("NoStateFound", "No saved state found"));
	}
	return FReply::Handled();
}

void SClaudeAssistantWidget::ShowProjectStateEditor()
{
	// Create a popup window for editing project state
	TSharedRef<SWindow> StateWindow = SNew(SWindow)
		.Title(LOCTEXT("ProjectStateTitle", "Save Project State"))
		.ClientSize(FVector2D(600, 500))
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	// Initialize with current state or defaults
	if (CurrentProjectState.ProjectName.IsEmpty())
	{
		CurrentProjectState.ProjectName = FApp::GetProjectName();
	}
	CurrentProjectState.LastUpdateDate = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M"));

	TSharedPtr<SMultiLineEditableTextBox> DescriptionBox;
	TSharedPtr<SMultiLineEditableTextBox> CompletedTasksBox;
	TSharedPtr<SMultiLineEditableTextBox> PendingTasksBox;
	TSharedPtr<SMultiLineEditableTextBox> NotesBox;

	// Build existing tasks strings
	FString CompletedStr;
	for (const FProjectTask& Task : CurrentProjectState.CompletedTasks)
	{
		CompletedStr += Task.Description + TEXT("\n");
	}

	FString PendingStr;
	for (const FProjectTask& Task : CurrentProjectState.PendingTasks)
	{
		PendingStr += Task.Description + TEXT("\n");
	}

	TSharedRef<SVerticalBox> Content = SNew(SVerticalBox)
		// Project Description
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 10.0f, 10.0f, 5.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DescLabel", "Project Description:"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f, 10.0f, 10.0f)
		[
			SNew(SBox)
			.HeightOverride(60.0f)
			[
				SAssignNew(DescriptionBox, SMultiLineEditableTextBox)
				.Text(FText::FromString(CurrentProjectState.ProjectDescription))
				.HintText(LOCTEXT("DescHint", "Brief description of what the project does..."))
			]
		]

		// Completed Tasks
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 5.0f, 10.0f, 5.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("CompletedLabel", "Completed Tasks (one per line):"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f, 10.0f, 10.0f)
		[
			SNew(SBox)
			.HeightOverride(100.0f)
			[
				SAssignNew(CompletedTasksBox, SMultiLineEditableTextBox)
				.Text(FText::FromString(CompletedStr))
				.HintText(LOCTEXT("CompletedHint", "Task 1\nTask 2\n..."))
			]
		]

		// Pending Tasks
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 5.0f, 10.0f, 5.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PendingLabel", "Pending Tasks (one per line):"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f, 10.0f, 10.0f)
		[
			SNew(SBox)
			.HeightOverride(100.0f)
			[
				SAssignNew(PendingTasksBox, SMultiLineEditableTextBox)
				.Text(FText::FromString(PendingStr))
				.HintText(LOCTEXT("PendingHint", "Task to do 1\nTask to do 2\n..."))
			]
		]

		// Notes
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 5.0f, 10.0f, 5.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NotesLabel", "Additional Notes:"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f, 10.0f, 10.0f)
		[
			SNew(SBox)
			.HeightOverride(60.0f)
			[
				SAssignNew(NotesBox, SMultiLineEditableTextBox)
				.Text(FText::FromString(CurrentProjectState.Notes))
				.HintText(LOCTEXT("NotesHint", "Any additional notes for resuming work..."))
			]
		]

		// Buttons
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		.Padding(10.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(5.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("SaveBtn", "Save"))
				.OnClicked_Lambda([this, DescriptionBox, CompletedTasksBox, PendingTasksBox, NotesBox, StateWindow]() {
					// Parse completed tasks
					CurrentProjectState.CompletedTasks.Empty();
					FString CompletedText = CompletedTasksBox->GetText().ToString();
					TArray<FString> CompletedLines;
					CompletedText.ParseIntoArrayLines(CompletedLines);
					for (const FString& Line : CompletedLines)
					{
						FString Trimmed = Line.TrimStartAndEnd();
						if (!Trimmed.IsEmpty())
						{
							CurrentProjectState.CompletedTasks.Add(FProjectTask(Trimmed, true));
						}
					}

					// Parse pending tasks
					CurrentProjectState.PendingTasks.Empty();
					FString PendingText = PendingTasksBox->GetText().ToString();
					TArray<FString> PendingLines;
					PendingText.ParseIntoArrayLines(PendingLines);
					for (const FString& Line : PendingLines)
					{
						FString Trimmed = Line.TrimStartAndEnd();
						if (!Trimmed.IsEmpty())
						{
							CurrentProjectState.PendingTasks.Add(FProjectTask(Trimmed, false));
						}
					}

					CurrentProjectState.ProjectDescription = DescriptionBox->GetText().ToString();
					CurrentProjectState.Notes = NotesBox->GetText().ToString();

					SaveProjectState();
					StateWindow->RequestDestroyWindow();

					StatusText->SetText(LOCTEXT("StateSaved", "Project state saved successfully"));
					AddMessageToChat(FChatMessage(false, TEXT("[Project state saved successfully]")));

					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(5.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("CancelBtn", "Cancel"))
				.OnClicked_Lambda([StateWindow]() {
					StateWindow->RequestDestroyWindow();
					return FReply::Handled();
				})
			]
		];

	StateWindow->SetContent(Content);
	FSlateApplication::Get().AddWindow(StateWindow);
}

void SClaudeAssistantWidget::SaveProjectState()
{
	FString FilePath = GetProjectStateFilePath();
	FString Content;
	FString ProjectName = FApp::GetProjectName();

	// Generate Markdown documentation - English content
	Content += FString::Printf(TEXT("# %s - Project Documentation\n\n"), *ProjectName);
	Content += TEXT("© 2025 PixelsDesign. All Rights Reserved.\n\n");
	Content += TEXT("> **IMPORTANT**: This file serves as persistent project memory. Claude should read this at the start of each session.\n\n");
	Content += TEXT("---\n\n");

	// Project Overview
	Content += TEXT("## Project Overview\n\n");
	Content += FString::Printf(TEXT("**Project Name**: %s\n"), *ProjectName);
	Content += FString::Printf(TEXT("**Last Updated**: %s\n"), *CurrentProjectState.LastUpdateDate);
	Content += FString::Printf(TEXT("**Project Path**: `%s`\n\n"), *FPaths::ProjectDir());

	if (!CurrentProjectState.ProjectDescription.IsEmpty())
	{
		Content += TEXT("### Description\n\n");
		Content += CurrentProjectState.ProjectDescription + TEXT("\n\n");
	}

	Content += TEXT("---\n\n");

	// Completed Tasks
	Content += TEXT("## Completed Tasks\n\n");
	if (CurrentProjectState.CompletedTasks.Num() > 0)
	{
		for (const FProjectTask& Task : CurrentProjectState.CompletedTasks)
		{
			Content += FString::Printf(TEXT("- [x] %s\n"), *Task.Description);
		}
	}
	else
	{
		Content += TEXT("*No completed tasks*\n");
	}
	Content += TEXT("\n---\n\n");

	// Pending Tasks
	Content += TEXT("## Pending Tasks / TODO\n\n");
	if (CurrentProjectState.PendingTasks.Num() > 0)
	{
		for (const FProjectTask& Task : CurrentProjectState.PendingTasks)
		{
			Content += FString::Printf(TEXT("- [ ] %s\n"), *Task.Description);
		}
	}
	else
	{
		Content += TEXT("*No pending tasks*\n");
	}
	Content += TEXT("\n---\n\n");

	// Technical Notes
	if (!CurrentProjectState.Notes.IsEmpty())
	{
		Content += TEXT("## Technical Notes\n\n");
		Content += CurrentProjectState.Notes + TEXT("\n\n");
		Content += TEXT("---\n\n");
	}

	// Plugin structure info
	Content += TEXT("## Plugin Structure\n\n");
	Content += TEXT("```\n");
	Content += TEXT("Plugins/ClaudeAssistant/\n");
	Content += TEXT("|- Source/ClaudeAssistantCore/     # API Client, Settings\n");
	Content += TEXT("|  |- Private/ClaudeAPIClient.cpp  # HTTP requests\n");
	Content += TEXT("|- Source/ClaudeAssistantEditor/   # Slate UI\n");
	Content += TEXT("   |- Private/SClaudeAssistantWidget.cpp # Main widget\n");
	Content += TEXT("```\n\n");

	// Build commands section
	Content += TEXT("## Build Commands\n\n");
	Content += TEXT("```bash\n");
	Content += TEXT("# Compilation\n");
	Content += FString::Printf(TEXT("\"%sBinaries/DotNET/UnrealBuildTool/UnrealBuildTool\" "), *FPaths::EngineDir());
#if PLATFORM_MAC
	const TCHAR* TargetPlatform = TEXT("Mac");
#elif PLATFORM_LINUX
	const TCHAR* TargetPlatform = TEXT("Linux");
#else
	const TCHAR* TargetPlatform = TEXT("Win64");
#endif
	Content += FString::Printf(TEXT("ClaudeAssistantEditor %s Development -project=\"%s%s.uproject\" -progress\n"), TargetPlatform, *FPaths::ProjectDir(), *ProjectName);
	Content += TEXT("```\n\n");

	// Recent files section
	if (CurrentProjectState.RecentFiles.Num() > 0)
	{
		Content += TEXT("## Recently Modified Files\n\n");
		for (const FString& File : CurrentProjectState.RecentFiles)
		{
			Content += FString::Printf(TEXT("- `%s`\n"), *File);
		}
		Content += TEXT("\n");
	}

	// Debug info
	Content += TEXT("## Debug\n\n");
	Content += TEXT("- **Log File**: `Saved/Logs/ClaudeAssistant.log`\n");
	Content += TEXT("- **Log Filter**: `LogClaudeAPI`\n\n");

	Content += TEXT("---\n\n");
	Content += TEXT("*Auto-generated by Claude Assistant Plugin - © 2025 PixelsDesign*\n");

	FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Log, TEXT("ClaudeAssistant: Project documentation saved to %s"), *FilePath);
}

bool SClaudeAssistantWidget::LoadProjectState()
{
	FString FilePath = GetProjectStateFilePath();
	FString Content;

	if (!FFileHelper::LoadFileToString(Content, *FilePath))
	{
		return false;
	}

	// Parse the Markdown file
	CurrentProjectState = FProjectState();
	CurrentProjectState.ProjectName = FApp::GetProjectName();

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines);

	enum class EParseSection { None, Description, Completed, Pending, Notes };
	EParseSection CurrentSection = EParseSection::None;

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();

		// Check for Markdown section headers (English)
		if (Trimmed.Contains(TEXT("## Project Overview")) || Trimmed.Contains(TEXT("### Description")))
		{
			CurrentSection = EParseSection::Description;
			continue;
		}
		else if (Trimmed.Contains(TEXT("## Completed Tasks")))
		{
			CurrentSection = EParseSection::Completed;
			continue;
		}
		else if (Trimmed.Contains(TEXT("## Pending Tasks")))
		{
			CurrentSection = EParseSection::Pending;
			continue;
		}
		else if (Trimmed.Contains(TEXT("## Technical Notes")))
		{
			CurrentSection = EParseSection::Notes;
			continue;
		}
		else if (Trimmed.StartsWith(TEXT("## ")) || Trimmed.StartsWith(TEXT("---")) || Trimmed.StartsWith(TEXT("```")) || Trimmed.StartsWith(TEXT(">")))
		{
			// Skip other sections and formatting
			if (Trimmed.StartsWith(TEXT("## Plugin")) || Trimmed.StartsWith(TEXT("## Build")) || Trimmed.StartsWith(TEXT("## Debug")) || Trimmed.StartsWith(TEXT("## Recently")))
			{
				CurrentSection = EParseSection::None;
			}
			continue;
		}

		// Parse Last Update from Markdown format
		if (Trimmed.StartsWith(TEXT("**Last Updated**:")))
		{
			CurrentProjectState.LastUpdateDate = Trimmed.Mid(17).TrimStartAndEnd();
			continue;
		}

		// Parse content based on section
		switch (CurrentSection)
		{
		case EParseSection::Description:
			if (!Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("**")) && !Trimmed.StartsWith(TEXT("*No ")))
			{
				if (!CurrentProjectState.ProjectDescription.IsEmpty())
				{
					CurrentProjectState.ProjectDescription += TEXT("\n");
				}
				CurrentProjectState.ProjectDescription += Trimmed;
			}
			break;

		case EParseSection::Completed:
			// Parse Markdown checkbox format: - [x] task
			if (Trimmed.StartsWith(TEXT("- [x]")) || Trimmed.StartsWith(TEXT("- [X]")))
			{
				FString TaskDesc = Trimmed.Mid(5).TrimStartAndEnd();
				if (!TaskDesc.IsEmpty())
				{
					CurrentProjectState.CompletedTasks.Add(FProjectTask(TaskDesc, true));
				}
			}
			break;

		case EParseSection::Pending:
			// Parse Markdown checkbox format: - [ ] task
			if (Trimmed.StartsWith(TEXT("- [ ]")))
			{
				FString TaskDesc = Trimmed.Mid(5).TrimStartAndEnd();
				if (!TaskDesc.IsEmpty())
				{
					CurrentProjectState.PendingTasks.Add(FProjectTask(TaskDesc, false));
				}
			}
			break;

		case EParseSection::Notes:
			if (!Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("*No ")))
			{
				if (!CurrentProjectState.Notes.IsEmpty())
				{
					CurrentProjectState.Notes += TEXT("\n");
				}
				CurrentProjectState.Notes += Trimmed;
			}
			break;

		default:
			break;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ClaudeAssistant: Project documentation loaded from %s"), *FilePath);
	return true;
}

FReply SClaudeAssistantWidget::OnTestAPIClicked()
{
	SetLoadingState(true);
	StatusText->SetText(LOCTEXT("TestingAPI", "Testing API connection..."));

	FClaudeAPIClient::Get().TestConnection(
		FOnClaudeResponseReceived::CreateSP(this, &SClaudeAssistantWidget::OnTestAPIResponseReceived)
	);

	return FReply::Handled();
}

void SClaudeAssistantWidget::OnTestAPIResponseReceived(bool bSuccess, const FString& Response)
{
	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this, bSuccess, Response]()
		{
			OnTestAPIResponseReceived(bSuccess, Response);
		});
		return;
	}

	SetLoadingState(false);

	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();

	if (bSuccess)
	{
		FString SuccessMessage;
		if (Settings && Settings->AuthMode == EClaudeAuthMode::SubscriptionCLI)
		{
			SuccessMessage = TEXT("Claude CLI connection successful!\n\nUsing your Claude Pro/Max subscription via the Claude Code CLI. The model and token budget are picked by the CLI based on your plan; per-request Model and Max Tokens settings are ignored in this mode.");
		}
		else if (Settings)
		{
			SuccessMessage = FString::Printf(TEXT("API connection successful!\n\nModel: %s\nMax tokens: %d\n\nYour API key is valid and the selected model is accessible."),
				*Settings->GetModelId(), Settings->MaxTokens);
		}
		else
		{
			SuccessMessage = TEXT("Connection successful.");
		}
		AddMessageToChat(FChatMessage(false, SuccessMessage));
	}
	else
	{
		AddMessageToChat(FChatMessage(false, FString::Printf(TEXT("API Connection Test Failed\n\n%s"), *Response)));
	}
}

#undef LOCTEXT_NAMESPACE

