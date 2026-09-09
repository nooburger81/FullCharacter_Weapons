// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeMaterialTools.h
// Material graph authoring via UMaterialEditingLibrary.

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

namespace ClaudeMaterialTools
{
	/** Registers material-graph tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);
}
