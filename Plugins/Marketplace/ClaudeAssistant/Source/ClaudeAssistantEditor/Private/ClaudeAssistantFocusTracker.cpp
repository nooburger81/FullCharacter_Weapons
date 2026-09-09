// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantFocusTracker.cpp

#include "ClaudeAssistantFocusTracker.h"

#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Materials/MaterialInterface.h"

FClaudeAssistantFocusTracker& FClaudeAssistantFocusTracker::Get()
{
	static FClaudeAssistantFocusTracker Instance;
	return Instance;
}

void FClaudeAssistantFocusTracker::Initialize()
{
	if (bInitialized || !GEditor)
	{
		return;
	}

	if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
	{
		OnAssetOpenedHandle = Subsystem->OnAssetOpenedInEditor().AddRaw(
			this, &FClaudeAssistantFocusTracker::OnAssetOpened);
		bInitialized = true;
	}
}

void FClaudeAssistantFocusTracker::Shutdown()
{
	if (GEditor)
	{
		if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			if (OnAssetOpenedHandle.IsValid())
			{
				Subsystem->OnAssetOpenedInEditor().Remove(OnAssetOpenedHandle);
			}
		}
	}

	OnAssetOpenedHandle.Reset();
	LastAsset.Reset();
	bInitialized = false;
}

void FClaudeAssistantFocusTracker::OnAssetOpened(UObject* Asset, IAssetEditorInstance* /*Instance*/)
{
	// Ignore transient/preview objects; keep the last real asset the user opened.
	if (Asset && Asset->IsAsset())
	{
		LastAsset = Asset;
	}
}

UObject* FClaudeAssistantFocusTracker::GetActiveAsset() const
{
	return LastAsset.Get();
}

void FClaudeAssistantFocusTracker::SetActiveAssetForTest(UObject* Asset)
{
	LastAsset = Asset;
}

FString FClaudeAssistantFocusTracker::GetActiveContextString() const
{
	UObject* Asset = LastAsset.Get();
	if (!Asset)
	{
		return FString();
	}

	FString TypeStr;
	if (Asset->IsA<UBlueprint>())
	{
		TypeStr = TEXT("Blueprint");
	}
	else if (Asset->IsA<UDataTable>())
	{
		TypeStr = TEXT("DataTable");
	}
	else if (Asset->IsA<UMaterialInterface>())
	{
		TypeStr = TEXT("Material");
	}
	else
	{
		TypeStr = Asset->GetClass() ? Asset->GetClass()->GetName() : TEXT("Asset");
	}

	return FString::Printf(TEXT("[Editor context: %s '%s' at %s]"),
		*TypeStr, *Asset->GetName(), *Asset->GetPathName());
}
