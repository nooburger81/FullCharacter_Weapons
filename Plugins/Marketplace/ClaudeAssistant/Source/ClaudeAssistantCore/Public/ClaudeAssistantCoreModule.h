// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantCoreModule.h
// Runtime core module - supports Live Coding

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * FClaudeAssistantCoreModule
 *
 * Runtime module containing the core logic:
 * - API client for communication with Claude
 * - Plugin settings
 *
 * This module supports Live Coding for logic modifications.
 */
class FClaudeAssistantCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FClaudeAssistantCoreModule& Get();
	static bool IsAvailable();
};
