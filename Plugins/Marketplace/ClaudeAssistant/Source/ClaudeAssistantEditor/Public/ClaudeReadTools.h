// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeReadTools.h
// Non-destructive introspection tools (no confirmation required).

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeReadTools
{
	/** Registers all built-in read-only tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
