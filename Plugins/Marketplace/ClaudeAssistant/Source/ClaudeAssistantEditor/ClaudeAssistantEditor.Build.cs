// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssistantEditor.Build.cs
// Editor UI Module - Compatible with UE 5.0+

using UnrealBuildTool;

public class ClaudeAssistantEditor : ModuleRules
{
	public ClaudeAssistantEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"ClaudeAssistantCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"UnrealEd",
				"EditorStyle",
				"ToolMenus",
				"WorkspaceMenuStructure",
				"Settings",
				"Projects",
				"ApplicationCore",
				"DesktopPlatform",
				"Json",
				"BlueprintGraph",
				"Kismet",
				"HTTPServer",
				"PythonScriptPlugin",
				"DataTableEditor",
				"JsonUtilities",
				"AssetTools",
				"UMG",
				"UMGEditor",
				"BehaviorTreeEditor",
				"AIModule",
				"AnimGraph",
				"AIGraph",
				"AssetRegistry",
				"MaterialEditor",
				"PropertyEditor",
			}
		);

		// Live Coding (Live++) is Windows-only; the module does not exist on Mac/Linux.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
