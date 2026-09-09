// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeWorldTools.h
// Level/Actor tools and Content-Browser asset management.

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeWorldTools
{
	/** Registers level/actor and asset-management tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
