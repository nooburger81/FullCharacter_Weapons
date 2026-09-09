// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeWidgetTools.h
// UMG Widget Blueprint UI-tree authoring tools.

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeWidgetTools
{
	/** Registers Widget Blueprint UI-tree tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
