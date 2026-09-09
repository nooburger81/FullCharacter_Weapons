// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantCompat.h
// Compatibility layer for UE 5.0+

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

// FAppStyle was introduced in UE 5.1, before that it was FEditorStyle
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 1
    #include "EditorStyleSet.h"
    #define FAppStyle FEditorStyle
#else
    #include "Styling/AppStyle.h"
#endif

// Version helper: true when compiling against an engine OLDER than Major.Minor.
// Used to keep the 5.6+ code paths (the reference, runtime-verified ones) untouched
// while older engines get their own variants.
#define CLAUDE_UE_BEFORE(MajorVer, MinorVer) \
    (ENGINE_MAJOR_VERSION < (MajorVer) || (ENGINE_MAJOR_VERSION == (MajorVer) && ENGINE_MINOR_VERSION < (MinorVer)))
