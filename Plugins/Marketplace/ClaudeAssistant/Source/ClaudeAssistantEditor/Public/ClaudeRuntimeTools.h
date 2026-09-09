// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeRuntimeTools.h
// Play-In-Editor (PIE) runtime control tools.

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeRuntimeTools
{
	/** Registers PIE runtime-control tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
