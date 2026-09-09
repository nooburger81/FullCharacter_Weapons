// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantSettings.cpp

#include "ClaudeAssistantSettings.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	// LaunchURL can fail silently on machines with a broken default-browser handler,
	// so fall back to an explicit OS shell command and log what happened.
	void SettingsOpenFeedbackUrl(const TCHAR* Url)
	{
		FString Error;
		FPlatformProcess::LaunchURL(Url, nullptr, &Error);
		if (Error.IsEmpty())
		{
			return;
		}
#if PLATFORM_WINDOWS
		FPlatformProcess::CreateProc(TEXT("cmd.exe"), *FString::Printf(TEXT("/c start \"\" \"%s\""), Url), true, true, true, nullptr, 0, nullptr, nullptr);
#elif PLATFORM_MAC
		FPlatformProcess::CreateProc(TEXT("/usr/bin/open"), Url, true, true, true, nullptr, 0, nullptr, nullptr);
#else
		FPlatformProcess::CreateProc(TEXT("/usr/bin/xdg-open"), Url, true, true, true, nullptr, 0, nullptr, nullptr);
#endif
		UE_LOG(LogTemp, Warning, TEXT("ClaudeAssistant: LaunchURL failed ('%s'), used OS shell fallback for %s"), *Error, Url);
	}
}

void UClaudeAssistantSettings::OpenFeatureRequest()
{
	SettingsOpenFeedbackUrl(TEXT("https://www.pixelsdesign.it/feature-request.html"));
}

void UClaudeAssistantSettings::OpenBugReport()
{
	SettingsOpenFeedbackUrl(TEXT("https://www.pixelsdesign.it/bug-report.html"));
}

FString UClaudeAssistantSettings::GetModelId() const
{
	switch (Model)
	{
	case EClaudeModel::Opus48:
		return TEXT("claude-opus-4-8");
	case EClaudeModel::Opus47:
		return TEXT("claude-opus-4-7");
	case EClaudeModel::Sonnet46:
		return TEXT("claude-sonnet-4-6");
	case EClaudeModel::Haiku45:
		return TEXT("claude-haiku-4-5-20251001");
	case EClaudeModel::Sonnet4:
		return TEXT("claude-sonnet-4-20250514");
	case EClaudeModel::Opus4:
		return TEXT("claude-opus-4-20250514");
	case EClaudeModel::Sonnet37:
		return TEXT("claude-3-7-sonnet-20250219");
	case EClaudeModel::Haiku35:
		return TEXT("claude-3-5-haiku-20241022");
	default:
		return TEXT("claude-sonnet-4-6");
	}
}

TArray<FString> UClaudeAssistantSettings::GetClaudeCLICandidatePaths()
{
	TArray<FString> Out;

#if PLATFORM_WINDOWS
	const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));

	// 1. Current Anthropic installer (2025+): ~/.local/bin/claude.exe
	if (!UserProfile.IsEmpty())
	{
		Out.Add(FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin"), TEXT("claude.exe")));
	}
	// 2. Legacy MSI installer
	if (!LocalAppData.IsEmpty())
	{
		Out.Add(FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("claude"), TEXT("claude.exe")));
		Out.Add(FPaths::Combine(LocalAppData, TEXT("AnthropicClaude"), TEXT("claude.exe")));
	}
	// 3. npm global install
	if (!AppData.IsEmpty())
	{
		Out.Add(FPaths::Combine(AppData, TEXT("npm"), TEXT("claude.cmd")));
	}
#else
	const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!Home.IsEmpty())
	{
		Out.Add(FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), TEXT("claude")));
	}
	Out.Add(TEXT("/usr/local/bin/claude"));
	Out.Add(TEXT("/opt/homebrew/bin/claude"));
#endif

	return Out;
}

TArray<FString> UClaudeAssistantSettings::GetClaudeCLICandidatePathsForDisplay()
{
	TArray<FString> Out;

#if PLATFORM_WINDOWS
	Out.Add(TEXT("%USERPROFILE%\\.local\\bin\\claude.exe"));
	Out.Add(TEXT("%LOCALAPPDATA%\\Programs\\claude\\claude.exe"));
	Out.Add(TEXT("%LOCALAPPDATA%\\AnthropicClaude\\claude.exe"));
	Out.Add(TEXT("%APPDATA%\\npm\\claude.cmd"));
#else
	Out.Add(TEXT("$HOME/.local/bin/claude"));
	Out.Add(TEXT("/usr/local/bin/claude"));
	Out.Add(TEXT("/opt/homebrew/bin/claude"));
#endif

	return Out;
}

FString UClaudeAssistantSettings::AutoDetectClaudeCLI()
{
	IFileManager& FM = IFileManager::Get();
	for (const FString& Candidate : GetClaudeCLICandidatePaths())
	{
		if (FM.FileExists(*Candidate))
		{
			return Candidate;
		}
	}
	return FString();
}

FString UClaudeAssistantSettings::GetEffectiveCLIPath() const
{
	if (!ClaudeCLIPath.IsEmpty())
	{
		return ClaudeCLIPath;
	}

	const FString AutoDetected = AutoDetectClaudeCLI();
	if (!AutoDetected.IsEmpty())
	{
		return AutoDetected;
	}

	// Last resort: rely on PATH lookup at spawn time.
#if PLATFORM_WINDOWS
	return TEXT("claude.exe");
#else
	return TEXT("claude");
#endif
}

bool UClaudeAssistantSettings::HasAuth() const
{
	switch (AuthMode)
	{
	case EClaudeAuthMode::APIKey:
		return !APIKey.IsEmpty();
	case EClaudeAuthMode::SubscriptionCLI:
		// Trust an explicit user-supplied path (it might be on PATH only).
		// Otherwise require autodetection to find the binary on disk.
		return !ClaudeCLIPath.IsEmpty() || !AutoDetectClaudeCLI().IsEmpty();
	default:
		return false;
	}
}
