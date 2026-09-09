// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantFocusTracker.h
// Tracks the asset the user is currently working on in the editor so the
// assistant can answer context-aware questions ("explain this", "add a var here")
// without the user pasting the asset name. Editor-only, read-only observer.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UObject;
class IAssetEditorInstance;

/**
 * FClaudeAssistantFocusTracker
 *
 * Singleton observer that records the most recently opened/activated asset editor
 * (Blueprint, DataTable, Material, ...). Exposes a compact context string that the
 * widget injects into the system prompt each turn.
 *
 * v1 signal: the last asset opened via UAssetEditorSubsystem::OnAssetOpenedInEditor.
 * A later increment refines this to true active-tab focus via the global tab manager.
 */
class CLAUDEASSISTANTEDITOR_API FClaudeAssistantFocusTracker
{
public:
	static FClaudeAssistantFocusTracker& Get();

	/** Bind to the asset editor subsystem. Safe to call once module is up (needs GEditor). */
	void Initialize();

	/** Unbind delegates and clear state. */
	void Shutdown();

	/** The currently tracked asset, or nullptr if none/stale. */
	UObject* GetActiveAsset() const;

	/** Test/automation hook: force the tracked asset (used by the ClaudeAssistant.SelfTest command). */
	void SetActiveAssetForTest(UObject* Asset);

	/**
	 * Compact one-line context for the system prompt, e.g.
	 *   [Editor context: Blueprint 'BP_Player' at /Game/Blueprints/BP_Player.BP_Player]
	 * Returns an empty string when no asset editor is being tracked.
	 */
	FString GetActiveContextString() const;

private:
	FClaudeAssistantFocusTracker() = default;

	void OnAssetOpened(UObject* Asset, IAssetEditorInstance* Instance);

	TWeakObjectPtr<UObject> LastAsset;
	FDelegateHandle OnAssetOpenedHandle;
	bool bInitialized = false;
};
