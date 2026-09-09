// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantStyle.cpp
// Custom style implementation

#include "ClaudeAssistantStyle.h"
#include "ClaudeAssistantCompat.h"
#include "Styling/SlateStyleRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#define RootToContentDir Style->RootToContentDir

TSharedPtr<FSlateStyleSet> FClaudeAssistantStyle::StyleInstance = nullptr;

void FClaudeAssistantStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FClaudeAssistantStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FClaudeAssistantStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("ClaudeAssistantStyle"));
	return StyleSetName;
}

const ISlateStyle& FClaudeAssistantStyle::Get()
{
	return *StyleInstance;
}

void FClaudeAssistantStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

TSharedRef<FSlateStyleSet> FClaudeAssistantStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(GetStyleSetName()));

	// Set content path to plugin's Resources folder
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin("ClaudeAssistant");
	FString ResourcesPath = Plugin->GetBaseDir() / TEXT("Resources");
	Style->SetContentRoot(ResourcesPath);

	// Define icon sizes
	const FVector2D Icon16x16(16.0f, 16.0f);
	const FVector2D Icon20x20(20.0f, 20.0f);
	const FVector2D Icon40x40(40.0f, 40.0f);

	// Custom Claude icons
	Style->Set("ClaudeAssistant.TabIcon", new FSlateImageBrush(
		ResourcesPath / TEXT("Icon16.png"),
		Icon16x16
	));

	Style->Set("ClaudeAssistant.MenuIcon", new FSlateImageBrush(
		ResourcesPath / TEXT("Icon16.png"),
		Icon16x16
	));

	Style->Set("ClaudeAssistant.PluginAction", new FSlateImageBrush(
		ResourcesPath / TEXT("Icon40.png"),
		Icon40x40
	));

	Style->Set("ClaudeAssistant.PluginAction.Small", new FSlateImageBrush(
		ResourcesPath / TEXT("Icon20.png"),
		Icon20x20
	));

	// User message style
	Style->Set("ClaudeAssistant.UserMessage", FTextBlockStyle()
		.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 10))
		.SetColorAndOpacity(FSlateColor(FLinearColor::White))
	);

	// Claude message style
	Style->Set("ClaudeAssistant.AssistantMessage", FTextBlockStyle()
		.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 10))
		.SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.9f, 1.0f)))
	);

	// Code block style
	Style->Set("ClaudeAssistant.CodeBlock", FTextBlockStyle()
		.SetFont(FCoreStyle::GetDefaultFontStyle("Mono", 9))
		.SetColorAndOpacity(FSlateColor(FLinearColor(0.8f, 1.0f, 0.8f)))
	);

	return Style;
}

#undef RootToContentDir
