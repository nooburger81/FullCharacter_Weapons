// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantCoreModule.cpp
// Runtime module implementation

#include "ClaudeAssistantCoreModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogClaudeAssistantCore, Log, All);

void FClaudeAssistantCoreModule::StartupModule()
{
	UE_LOG(LogClaudeAssistantCore, Log, TEXT("ClaudeAssistantCore module loaded - Live Coding supported"));
}

void FClaudeAssistantCoreModule::ShutdownModule()
{
	UE_LOG(LogClaudeAssistantCore, Log, TEXT("ClaudeAssistantCore module unloaded"));
}

FClaudeAssistantCoreModule& FClaudeAssistantCoreModule::Get()
{
	return FModuleManager::LoadModuleChecked<FClaudeAssistantCoreModule>("ClaudeAssistantCore");
}

bool FClaudeAssistantCoreModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("ClaudeAssistantCore");
}

IMPLEMENT_MODULE(FClaudeAssistantCoreModule, ClaudeAssistantCore)
