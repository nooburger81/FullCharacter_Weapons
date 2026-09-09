// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantModule.cpp
// Editor UI module implementation

#include "ClaudeAssistantModule.h"
#include "ClaudeAssistantCompat.h"
#include "ClaudeAssistantStyle.h"
#include "ClaudeAssistantCommands.h"
#include "ClaudeAssistantSettings.h"
#include "SClaudeAssistantWidget.h"
#include "ClaudeAssistantFocusTracker.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeMcpServer.h"
#include "ClaudeSettingsDetails.h"
#include "PropertyEditorModule.h"

#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogClaudeAssistantEditor, Log, All);

const FName FClaudeAssistantEditorModule::ClaudeAssistantTabName("ClaudeAssistantTab");

#define LOCTEXT_NAMESPACE "FClaudeAssistantEditorModule"

void FClaudeAssistantEditorModule::StartupModule()
{
	FClaudeAssistantStyle::Initialize();
	FClaudeAssistantStyle::ReloadTextures();

	FClaudeAssistantCommands::Register();

	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FClaudeAssistantCommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FClaudeAssistantEditorModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FClaudeAssistantEditorModule::RegisterMenus));

	RegisterSettings();

	// Feedback buttons in Project Settings: explicit detail customization,
	// because CallInEditor rows are not reliably rendered by the settings view.
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.RegisterCustomClassLayout(
			UClaudeAssistantSettings::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(&FClaudeSettingsDetails::MakeInstance));
		PropertyModule.NotifyCustomizationModuleChanged();
	}

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(ClaudeAssistantTabName, FOnSpawnTab::CreateRaw(this, &FClaudeAssistantEditorModule::OnSpawnPluginTab))
		.SetDisplayName(LOCTEXT("ClaudeAssistantTabTitle", "Claude Assistant"))
		.SetTooltipText(LOCTEXT("ClaudeAssistantTooltip", "Open Claude AI Assistant"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FClaudeAssistantStyle::GetStyleSetName(), "ClaudeAssistant.TabIcon"));

	// Start observing which asset editor the user is working in, for context-aware answers.
	FClaudeAssistantFocusTracker::Get().Initialize();

	// Register the built-in editor tools the assistant can invoke (agentic backbone).
	FClaudeToolRegistry::Get().RegisterBuiltinTools();

	// Apply per-project tool curation: disable any categories the user has opted out of.
	if (const UClaudeAssistantSettings* CurationSettings = GetDefault<UClaudeAssistantSettings>())
	{
		for (const FString& Category : CurationSettings->DisabledToolCategories)
		{
			const FString Trimmed = Category.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				FClaudeToolRegistry::Get().SetCategoryEnabled(Trimmed, false);
				UE_LOG(LogClaudeAssistantEditor, Log, TEXT("Tool category '%s' disabled by project curation"), *Trimmed);
			}
		}
	}

	UE_LOG(LogClaudeAssistantEditor, Log, TEXT("ClaudeAssistantEditor: %d tool(s) registered"),
		FClaudeToolRegistry::Get().Num());

	// Subscription (Claude Code CLI) is the primary agentic path: expose our tools to it
	// via a localhost MCP server. Only bound when the user is in Subscription mode.
	{
		const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
		if (Settings && Settings->AuthMode == EClaudeAuthMode::SubscriptionCLI)
		{
			FClaudeMcpServer::Get().Start(Settings->McpServerPort);
		}
	}

	UE_LOG(LogClaudeAssistantEditor, Log, TEXT("ClaudeAssistantEditor module loaded"));
}

void FClaudeAssistantEditorModule::ShutdownModule()
{
	FClaudeMcpServer::Get().Stop();
	FClaudeAssistantFocusTracker::Get().Shutdown();

	UnregisterSettings();

	if (FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
	{
		PropertyModule->UnregisterCustomClassLayout(UClaudeAssistantSettings::StaticClass()->GetFName());
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	FClaudeAssistantCommands::Unregister();
	FClaudeAssistantStyle::Shutdown();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ClaudeAssistantTabName);

	UE_LOG(LogClaudeAssistantEditor, Log, TEXT("ClaudeAssistantEditor module unloaded"));
}

void FClaudeAssistantEditorModule::RegisterSettings()
{
	ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>("Settings");
	SettingsModule.RegisterSettings(
		UClaudeAssistantSettings::GetContainerNameStatic(),
		UClaudeAssistantSettings::GetCategoryNameStatic(),
		UClaudeAssistantSettings::GetSectionNameStatic(),
		LOCTEXT("ClaudeAssistantSettingsName", "Claude Assistant"),
		LOCTEXT("ClaudeAssistantSettingsDescription", "Configure Claude AI Assistant. Two auth modes: (1) API Key from console.anthropic.com (pay-per-token), or (2) Claude Pro/Max subscription via the Claude Code CLI (requires local 'claude' install + 'claude /login')."),
		GetMutableDefault<UClaudeAssistantSettings>()
	);
	UE_LOG(LogClaudeAssistantEditor, Log, TEXT("Project Settings > Plugins > Claude Assistant registered"));
}

void FClaudeAssistantEditorModule::UnregisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings(
			UClaudeAssistantSettings::GetContainerNameStatic(),
			UClaudeAssistantSettings::GetCategoryNameStatic(),
			UClaudeAssistantSettings::GetSectionNameStatic()
		);
	}
}

FClaudeAssistantEditorModule& FClaudeAssistantEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FClaudeAssistantEditorModule>("ClaudeAssistantEditor");
}

bool FClaudeAssistantEditorModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("ClaudeAssistantEditor");
}

void FClaudeAssistantEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");

		Section.AddMenuEntryWithCommandList(
			FClaudeAssistantCommands::Get().OpenPluginWindow,
			PluginCommands,
			LOCTEXT("ClaudeAssistantMenuEntry", "Claude Assistant"),
			LOCTEXT("ClaudeAssistantMenuEntryTooltip", "Open Claude AI Assistant window"),
			FSlateIcon(FClaudeAssistantStyle::GetStyleSetName(), "ClaudeAssistant.MenuIcon")
		);
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");

		FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FClaudeAssistantCommands::Get().OpenPluginWindow));
		Entry.SetCommandList(PluginCommands);
	}
}

TSharedRef<SDockTab> FClaudeAssistantEditorModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SClaudeAssistantWidget)
		];
}

void FClaudeAssistantEditorModule::PluginButtonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(ClaudeAssistantTabName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FClaudeAssistantEditorModule, ClaudeAssistantEditor)
