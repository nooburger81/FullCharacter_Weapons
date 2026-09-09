// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantCore.Build.cs
// Runtime Core Module - Compatible with UE 5.0+

using UnrealBuildTool;

public class ClaudeAssistantCore : ModuleRules
{
	public ClaudeAssistantCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"HTTP",
				"SSL",
				"Json",
				"JsonUtilities",
			}
		);
	}
}

