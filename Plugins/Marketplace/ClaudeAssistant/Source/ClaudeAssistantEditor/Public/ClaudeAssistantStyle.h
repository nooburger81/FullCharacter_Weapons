// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantStyle.h
// Custom Slate style management for the plugin

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/**
 * FClaudeAssistantStyle
 *
 * Manages custom styles for the plugin interface:
 * - Icons for menu and toolbar
 * - Custom colors
 * - Brushes for widgets
 */
class FClaudeAssistantStyle
{
public:
	/** Initialize styles - call in StartupModule */
	static void Initialize();

	/** Cleanup styles - call in ShutdownModule */
	static void Shutdown();

	/** Reload textures (for hot-reload) */
	static void ReloadTextures();

	/** Returns the StyleSet name */
	static FName GetStyleSetName();

	/** Returns the StyleSet */
	static const ISlateStyle& Get();

private:
	/** Create and register the StyleSet */
	static TSharedRef<FSlateStyleSet> Create();

private:
	/** StyleSet instance */
	static TSharedPtr<FSlateStyleSet> StyleInstance;
};
