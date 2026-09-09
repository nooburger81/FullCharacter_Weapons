// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantCommands.cpp
// Editor commands implementation

#include "ClaudeAssistantCommands.h"

#define LOCTEXT_NAMESPACE "FClaudeAssistantModule"

void FClaudeAssistantCommands::RegisterCommands()
{
	UI_COMMAND(
		OpenPluginWindow,
		"Claude Assistant",
		"Open the Claude AI Assistant window for code generation",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Control | EModifierKey::Shift, EKeys::C)
	);
}

#undef LOCTEXT_NAMESPACE
