// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeSettingsDetails.h
// Detail customization for UClaudeAssistantSettings: renders the Feedback
// buttons (Suggest a Feature / Report a Bug) as explicit Slate buttons,
// since UFUNCTION(CallInEditor) rows are not reliably shown by the
// Project Settings details view.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class FClaudeSettingsDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
