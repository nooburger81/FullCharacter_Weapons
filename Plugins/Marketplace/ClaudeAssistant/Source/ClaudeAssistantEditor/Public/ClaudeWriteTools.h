// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeWriteTools.h
// Mutating tools (bIsDestructive). Currently the run_python escape hatch, which
// covers editor actions the dedicated tools don't, behind a confirmation gate.

#pragma once

#include "CoreMinimal.h"

class FClaudeToolRegistry;

class UBlueprint;
class UEdGraph;

namespace ClaudeWriteTools
{
	/** Registers the built-in write/mutation tools into the given registry. */
	void RegisterAll(FClaudeToolRegistry& Registry);

	/** Graph that node-creation tools target (chosen via set_active_graph); defaults to the Event Graph. */
	UEdGraph* ResolveTargetGraph(UBlueprint* BP);
	void SetActiveGraphName(FName GraphName);
}
