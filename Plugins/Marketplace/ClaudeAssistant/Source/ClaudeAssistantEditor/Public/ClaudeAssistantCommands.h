// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantCommands.h
// Editor command definitions for the plugin

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "ClaudeAssistantStyle.h"

/**
 * FClaudeAssistantCommands
 *
 * Defines the user interface commands for the plugin:
 * - Command to open the plugin window
 */
class FClaudeAssistantCommands : public TCommands<FClaudeAssistantCommands>
{
public:
	FClaudeAssistantCommands()
		: TCommands<FClaudeAssistantCommands>(
			TEXT("ClaudeAssistant"),
			NSLOCTEXT("Contexts", "ClaudeAssistant", "Claude Assistant Plugin"),
			NAME_None,
			FClaudeAssistantStyle::GetStyleSetName())
	{
	}

	/** Register all commands */
	virtual void RegisterCommands() override;

public:
	/** Command to open the plugin window */
	TSharedPtr<FUICommandInfo> OpenPluginWindow;
};
