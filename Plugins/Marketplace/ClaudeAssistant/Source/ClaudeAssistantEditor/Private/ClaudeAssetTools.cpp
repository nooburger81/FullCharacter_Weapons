// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeAssetTools.cpp

#include "ClaudeAssetTools.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantFocusTracker.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/BlueprintFactory.h"
#include "Factories/DataTableFactory.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/EnumFactory.h"
#include "Factories/StructureFactory.h"
#include "Factories/MaterialFactoryNew.h"
#include "WidgetBlueprintFactory.h"
#include "BehaviorTreeFactory.h"
#include "BlackboardDataFactory.h"
#include "ClaudeAssistantCompat.h"
#include "Animation/Skeleton.h"
#include "Blueprint/UserWidget.h"

namespace
{
	// Uniquely named to avoid ODR clashes with helpers in sibling tool files (Unity build).
	FString AssetSerializeJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> AssetSchema(const TArray<TPair<FString, FString>>& Props, const TArray<FString>& Required)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Props)
		{
			TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
			Prop->SetStringField(TEXT("type"), TEXT("string"));
			Prop->SetStringField(TEXT("description"), Pair.Value);
			P->SetObjectField(Pair.Key, Prop);
		}
		TArray<TSharedPtr<FJsonValue>> Req;
		for (const FString& R : Required) { Req.Add(MakeShared<FJsonValueString>(R)); }

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), P);
		Schema->SetArrayField(TEXT("required"), Req);
		return Schema;
	}

	// Target content folder: explicit request, else the open asset's folder, else /Game.
	FString ResolveAssetPath(const FString& Requested)
	{
		if (!Requested.IsEmpty()) { return Requested; }
		if (UObject* Active = FClaudeAssistantFocusTracker::Get().GetActiveAsset())
		{
			if (UPackage* Pkg = Active->GetPackage())
			{
				return FPackageName::GetLongPackagePath(Pkg->GetName());
			}
		}
		return TEXT("/Game");
	}

	// Create the asset via IAssetTools using the factory's own supported class.
	FClaudeToolResult CreateWithFactory(const FString& Name, const FString& Path, UFactory* Factory)
	{
		if (Name.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'name'.")); }
		if (!Factory) { return FClaudeToolResult::Error(TEXT("Failed to create the factory.")); }
		UClass* AssetClass = Factory->GetSupportedClass();
		if (!AssetClass) { return FClaudeToolResult::Error(TEXT("Factory has no supported class.")); }

		FAssetToolsModule& Module = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		UObject* NewAsset = Module.Get().CreateAsset(Name, Path, AssetClass, Factory);
		if (!NewAsset) { return FClaudeToolResult::Error(TEXT("Asset creation failed (the name may already exist, or the path is invalid).")); }

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("asset"), NewAsset->GetPathName());
		return FClaudeToolResult::Ok(AssetSerializeJson(Out));
	}
}

namespace ClaudeAssetTools
{
	void RegisterAll(FClaudeToolRegistry& Registry)
	{
		// ---------------------------------------------------------------------
		// create_blueprint
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_blueprint");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new Blueprint asset. parent_class is resolved by name (e.g. 'Actor', 'Pawn', 'Character', 'ActorComponent'); defaults to Actor. path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("parent_class"), TEXT("Parent class name (default 'Actor').") },
				{ TEXT("path"), TEXT("Optional content path, e.g. /Game/Blueprints.") },
			}, { TEXT("name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, ParentName, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("parent_class"), ParentName);
				Args->TryGetStringField(TEXT("path"), Path);
				if (ParentName.IsEmpty()) { ParentName = TEXT("Actor"); }

				UClass* Parent = UClass::TryFindTypeSlow<UClass>(ParentName);
				if (!Parent) { return FClaudeToolResult::Error(FString::Printf(TEXT("Parent class '%s' not found."), *ParentName)); }

				UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
				Factory->ParentClass = Parent;
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// create_widget_blueprint
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_widget_blueprint");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new UMG Widget Blueprint (UUserWidget-based). path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("path"), TEXT("Optional content path.") },
			}, { TEXT("name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("path"), Path);

				UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
				Factory->ParentClass = UUserWidget::StaticClass();
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// create_animation_blueprint
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_animation_blueprint");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new Animation Blueprint bound to an existing Skeleton. 'skeleton' is the full object path to a USkeleton asset (e.g. /Game/Chars/Hero_Skeleton.Hero_Skeleton). path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("skeleton"), TEXT("Full object path to the target USkeleton asset.") },
				{ TEXT("path"), TEXT("Optional content path.") },
			}, { TEXT("name"), TEXT("skeleton") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, SkeletonPath, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("skeleton"), SkeletonPath);
				Args->TryGetStringField(TEXT("path"), Path);
				if (SkeletonPath.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'skeleton' (path to a USkeleton asset).")); }

				USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
				if (!Skeleton) { return FClaudeToolResult::Error(FString::Printf(TEXT("Skeleton '%s' not found."), *SkeletonPath)); }

				UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
				Factory->TargetSkeleton = Skeleton;
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// create_behavior_tree
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_behavior_tree");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new empty Behavior Tree asset. path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("path"), TEXT("Optional content path.") },
			}, { TEXT("name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("path"), Path);
				UBehaviorTreeFactory* Factory = NewObject<UBehaviorTreeFactory>();
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// create_blackboard
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_blackboard");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new empty Blackboard Data asset (for Behavior Trees). path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("path"), TEXT("Optional content path.") },
			}, { TEXT("name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("path"), Path);
#if CLAUDE_UE_BEFORE(5, 6)
				// UBlackboardDataFactory has no export/MinimalAPI before 5.6: resolve via reflection.
				UClass* FactoryCls = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BlackboardDataFactory"));
				if (!FactoryCls) { return FClaudeToolResult::Error(TEXT("BlackboardDataFactory class not found.")); }
				UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryCls);
#else
				UBlackboardDataFactory* Factory = NewObject<UBlackboardDataFactory>();
#endif
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// create_data_table
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_data_table");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new DataTable using an existing row struct. struct_name is resolved by name (C++ or user-defined struct). path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("struct_name"), TEXT("Row struct name.") },
				{ TEXT("path"), TEXT("Optional content path.") },
			}, { TEXT("name"), TEXT("struct_name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, StructName, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("struct_name"), StructName);
				Args->TryGetStringField(TEXT("path"), Path);
				if (StructName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'struct_name'.")); }

				UScriptStruct* Struct = UClass::TryFindTypeSlow<UScriptStruct>(StructName);
				if (!Struct) { return FClaudeToolResult::Error(FString::Printf(TEXT("Struct '%s' not found."), *StructName)); }

				UDataTableFactory* Factory = NewObject<UDataTableFactory>();
				Factory->Struct = Struct;
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// create_material
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_material");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new (empty) Material asset. Use the material tools (create_material_expression, connect_to_material_property) to author its graph. path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("path"), TEXT("Optional content path.") },
			}, { TEXT("name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("path"), Path);
				UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// create_struct
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_struct");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new (empty) user-defined Struct asset. path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("path"), TEXT("Optional content path.") },
			}, { TEXT("name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("path"), Path);
				UStructureFactory* Factory = NewObject<UStructureFactory>();
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// create_enum
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_enum");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Create a new (empty) user-defined Enum asset. path defaults to the open asset's folder or /Game.");
			Tool.InputSchema = AssetSchema({
				{ TEXT("name"), TEXT("New asset name.") },
				{ TEXT("path"), TEXT("Optional content path.") },
			}, { TEXT("name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Name, Path;
				Args->TryGetStringField(TEXT("name"), Name);
				Args->TryGetStringField(TEXT("path"), Path);
				UEnumFactory* Factory = NewObject<UEnumFactory>();
				return CreateWithFactory(Name, ResolveAssetPath(Path), Factory);
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}
	}
}
