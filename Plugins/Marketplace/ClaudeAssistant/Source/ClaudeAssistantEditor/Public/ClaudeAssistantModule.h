// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantModule.h
// Editor UI module for the plugin

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FToolBarBuilder;
class FMenuBuilder;
class SDockTab;

class FClaudeAssistantEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FClaudeAssistantEditorModule& Get();
	static bool IsAvailable();

private:
	void RegisterMenus();
	void RegisterSettings();
	void UnregisterSettings();
	TSharedRef<SDockTab> OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs);
	void PluginButtonClicked();

private:
	TSharedPtr<FUICommandList> PluginCommands;
	static const FName ClaudeAssistantTabName;
};
