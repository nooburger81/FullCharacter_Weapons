// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssetTools.h
// Asset-creation tools (Blueprint, Widget, AnimBP, Behavior Tree, DataTable, Struct, Enum...).

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeAssetTools
{
	/** Registers all asset-creation tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
