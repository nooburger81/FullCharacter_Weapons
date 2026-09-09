// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAnimTools.h
// Animation Blueprint AnimGraph authoring tools (v1: sequence player -> output pose).

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeAnimTools
{
	/** Registers AnimGraph authoring tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
