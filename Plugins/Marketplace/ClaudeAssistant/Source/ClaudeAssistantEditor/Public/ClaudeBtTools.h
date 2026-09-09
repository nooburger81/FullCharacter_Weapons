// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeBtTools.h
// Behavior Tree structure authoring tools (composites, tasks, decorators).

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeBtTools
{
	/** Registers Behavior Tree authoring tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
