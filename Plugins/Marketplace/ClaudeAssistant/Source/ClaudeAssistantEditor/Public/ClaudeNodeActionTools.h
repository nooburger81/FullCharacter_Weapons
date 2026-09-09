// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeNodeActionTools.h
// Universal node authoring via the Blueprint Action Database (the whole palette).

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeNodeActionTools
{
	/** Registers the search/spawn-any-node tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
