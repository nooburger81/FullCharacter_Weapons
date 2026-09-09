// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeWorldTools.cpp

#include "ClaudeWorldTools.h"
#include "ClaudeToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Modules/ModuleManager.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/App.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "ObjectTools.h"

namespace
{
	FString WorldSerJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> WorldSchema(const TArray<TPair<FString, FString>>& Props, const TArray<FString>& Required)
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

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	AActor* FindActorByLabel(UWorld* World, const FString& Label)
	{
		if (!World) { return nullptr; }
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel() == Label) { return *It; }
		}
		return nullptr;
	}

	bool ParseVec3(const FString& S, FVector& Out)
	{
		TArray<FString> Parts;
		S.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() != 3) { return false; }
		Out = FVector(FCString::Atod(*Parts[0].TrimStartAndEnd()), FCString::Atod(*Parts[1].TrimStartAndEnd()), FCString::Atod(*Parts[2].TrimStartAndEnd()));
		return true;
	}

	bool WorldConfirm(const FString& Summary)
	{
		if (FApp::IsUnattended())
		{
			// Automation context (self-test / CI): no user to ask, auto-approve.
			return true;
		}
		const FText Msg = FText::FromString(Summary + TEXT("\n\nProceed?"));
		return FMessageDialog::Open(EAppMsgType::YesNo, Msg) == EAppReturnType::Yes;
	}

	IAssetRegistry& AssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	}

	IAssetTools& AssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	}
}

namespace ClaudeWorldTools
{
	void RegisterAll(FClaudeToolRegistry& Registry)
	{
		// ================= LEVEL / ACTOR =====================================

		// spawn_actor
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("spawn_actor");
			Tool.Category = TEXT("level");
			Tool.Description = TEXT("Spawn an actor into the current editor level. class is resolved by name (e.g. 'StaticMeshActor', 'PointLight', or a Blueprint class). location is 'x,y,z' (optional). Returns the actor label. Undoable.");
			Tool.InputSchema = WorldSchema({
				{ TEXT("class"), TEXT("Actor class name.") },
				{ TEXT("location"), TEXT("Optional spawn location 'x,y,z'.") },
				{ TEXT("name"), TEXT("Optional actor label.") },
			}, { TEXT("class") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWorld* World = EditorWorld();
				if (!World) { return FClaudeToolResult::Error(TEXT("No editor world available.")); }
				FString ClassName, LocStr, Name;
				Args->TryGetStringField(TEXT("class"), ClassName);
				Args->TryGetStringField(TEXT("location"), LocStr);
				Args->TryGetStringField(TEXT("name"), Name);
				if (ClassName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'class'.")); }
				UClass* ActorClass = UClass::TryFindTypeSlow<UClass>(ClassName);
				if (!ActorClass && !ClassName.StartsWith(TEXT("A"))) { ActorClass = UClass::TryFindTypeSlow<UClass>(FString(TEXT("A")) + ClassName); }
				if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()) || ActorClass->HasAnyClassFlags(CLASS_Abstract))
				{
					return FClaudeToolResult::Error(FString::Printf(TEXT("Actor class '%s' not found."), *ClassName));
				}

				FVector Location = FVector::ZeroVector;
				if (!LocStr.IsEmpty()) { ParseVec3(LocStr, Location); }
				const FTransform SpawnTM(FRotator::ZeroRotator, Location);

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: spawn actor")));
				FActorSpawnParameters SpawnParams;
				AActor* NewActor = World->SpawnActor<AActor>(ActorClass, SpawnTM, SpawnParams);
				if (!NewActor) { return FClaudeToolResult::Error(TEXT("Spawn failed.")); }
				if (!Name.IsEmpty()) { NewActor->SetActorLabel(Name); }

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("actor"), NewActor->GetActorLabel());
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// list_actors
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("list_actors");
			Tool.Category = TEXT("level");
			Tool.Description = TEXT("List actors in the current editor level (label + class). Optional 'filter' matches the label or class (case-insensitive).");
			Tool.InputSchema = WorldSchema({
				{ TEXT("filter"), TEXT("Optional substring filter on label/class.") },
			}, {});
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWorld* World = EditorWorld();
				if (!World) { return FClaudeToolResult::Error(TEXT("No editor world available.")); }
				FString Filter;
				Args->TryGetStringField(TEXT("filter"), Filter);

				TArray<TSharedPtr<FJsonValue>> Actors;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* A = *It;
					if (!A) { continue; }
					const FString Label = A->GetActorLabel();
					const FString Cls = A->GetClass() ? A->GetClass()->GetName() : FString();
					if (!Filter.IsEmpty() && !Label.Contains(Filter, ESearchCase::IgnoreCase) && !Cls.Contains(Filter, ESearchCase::IgnoreCase)) { continue; }
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("label"), Label);
					Obj->SetStringField(TEXT("class"), Cls);
					Actors.Add(MakeShared<FJsonValueObject>(Obj));
					if (Actors.Num() >= 200) { break; }
				}

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("count"), Actors.Num());
				Out->SetArrayField(TEXT("actors"), Actors);
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// set_actor_transform
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("set_actor_transform");
			Tool.Category = TEXT("level");
			Tool.Description = TEXT("Set an actor's transform by label. Provide any of location/rotation/scale as 'x,y,z' (rotation is pitch,yaw,roll). Only provided fields change. Undoable.");
			Tool.InputSchema = WorldSchema({
				{ TEXT("actor"), TEXT("Actor label.") },
				{ TEXT("location"), TEXT("Optional 'x,y,z'.") },
				{ TEXT("rotation"), TEXT("Optional 'pitch,yaw,roll'.") },
				{ TEXT("scale"), TEXT("Optional 'x,y,z'.") },
			}, { TEXT("actor") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWorld* World = EditorWorld();
				FString ActorLabel;
				Args->TryGetStringField(TEXT("actor"), ActorLabel);
				AActor* A = FindActorByLabel(World, ActorLabel);
				if (!A) { return FClaudeToolResult::Error(FString::Printf(TEXT("Actor '%s' not found."), *ActorLabel)); }

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: set actor transform")));
				A->Modify();
				FString S;
				FVector V;
				if (Args->TryGetStringField(TEXT("location"), S) && ParseVec3(S, V)) { A->SetActorLocation(V); }
				if (Args->TryGetStringField(TEXT("rotation"), S) && ParseVec3(S, V)) { A->SetActorRotation(FRotator(V.X, V.Y, V.Z)); }
				if (Args->TryGetStringField(TEXT("scale"), S) && ParseVec3(S, V)) { A->SetActorScale3D(V); }

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// set_actor_property
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("set_actor_property");
			Tool.Category = TEXT("level");
			Tool.Description = TEXT("Set a property on an actor (by label), by property name and value as text. Undoable.");
			Tool.InputSchema = WorldSchema({
				{ TEXT("actor"), TEXT("Actor label.") },
				{ TEXT("property"), TEXT("Property name.") },
				{ TEXT("value"), TEXT("Value as text.") },
			}, { TEXT("actor"), TEXT("property"), TEXT("value") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWorld* World = EditorWorld();
				FString ActorLabel, PropName, Value;
				Args->TryGetStringField(TEXT("actor"), ActorLabel);
				Args->TryGetStringField(TEXT("property"), PropName);
				Args->TryGetStringField(TEXT("value"), Value);
				AActor* A = FindActorByLabel(World, ActorLabel);
				if (!A) { return FClaudeToolResult::Error(FString::Printf(TEXT("Actor '%s' not found."), *ActorLabel)); }
				FProperty* Prop = A->GetClass()->FindPropertyByName(FName(*PropName));
				if (!Prop) { return FClaudeToolResult::Error(FString::Printf(TEXT("Property '%s' not found on '%s'."), *PropName, *A->GetClass()->GetName())); }

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: set actor property")));
				A->Modify();
				const TCHAR* Result = Prop->ImportText_Direct(*Value, Prop->ContainerPtrToValuePtr<void>(A), A, PPF_None);
				A->PostEditChange();

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), Result != nullptr);
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// delete_actor (destructive)
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("delete_actor");
			Tool.Category = TEXT("level");
			Tool.Description = TEXT("Delete an actor from the editor level by label. Asks for confirmation. Undoable.");
			Tool.InputSchema = WorldSchema({ { TEXT("actor"), TEXT("Actor label.") } }, { TEXT("actor") });
			Tool.bIsDestructive = true;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWorld* World = EditorWorld();
				FString ActorLabel;
				Args->TryGetStringField(TEXT("actor"), ActorLabel);
				AActor* A = FindActorByLabel(World, ActorLabel);
				if (!A) { return FClaudeToolResult::Error(FString::Printf(TEXT("Actor '%s' not found."), *ActorLabel)); }
				if (!WorldConfirm(FString::Printf(TEXT("Delete actor '%s' from the level."), *ActorLabel)))
				{
					return FClaudeToolResult::Error(TEXT("User denied the deletion."));
				}
				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: delete actor")));
				const bool bOk = World->EditorDestroyActor(A, true);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), bOk);
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ================= ASSET MANAGEMENT ==================================

		// find_assets
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("find_assets");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Search the Content Browser for assets. Provide any of: 'path' (e.g. /Game/Blueprints), 'class' (e.g. 'Blueprint', 'Material'), 'query' (name substring). Returns matching asset object paths.");
			Tool.InputSchema = WorldSchema({
				{ TEXT("path"), TEXT("Optional content path to search under (recursive).") },
				{ TEXT("class"), TEXT("Optional asset class name.") },
				{ TEXT("query"), TEXT("Optional name substring.") },
			}, {});
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Path, ClassName, Query;
				Args->TryGetStringField(TEXT("path"), Path);
				Args->TryGetStringField(TEXT("class"), ClassName);
				Args->TryGetStringField(TEXT("query"), Query);
				if (Path.IsEmpty() && ClassName.IsEmpty() && Query.IsEmpty())
				{
					return FClaudeToolResult::Error(TEXT("Provide at least one of: path, class, query."));
				}

				FARFilter Filter;
				Filter.bRecursivePaths = true;
				if (!Path.IsEmpty()) { Filter.PackagePaths.Add(FName(*Path)); }
				if (!ClassName.IsEmpty())
				{
					UClass* C = UClass::TryFindTypeSlow<UClass>(ClassName);
					if (!C) { return FClaudeToolResult::Error(FString::Printf(TEXT("Class '%s' not found."), *ClassName)); }
					Filter.ClassPaths.Add(FTopLevelAssetPath(C));
					Filter.bRecursiveClasses = true;
				}
				if (Filter.PackagePaths.Num() == 0 && Filter.ClassPaths.Num() == 0)
				{
					// Only a name query given: restrict to /Game instead of scanning the entire registry.
					Filter.PackagePaths.Add(FName(TEXT("/Game")));
				}

				TArray<FAssetData> Found;
				AssetRegistry().GetAssets(Filter, Found);

				TArray<TSharedPtr<FJsonValue>> Results;
				for (const FAssetData& Data : Found)
				{
					const FString Name = Data.AssetName.ToString();
					if (!Query.IsEmpty() && !Name.Contains(Query, ESearchCase::IgnoreCase)) { continue; }
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Name);
					Obj->SetStringField(TEXT("path"), Data.GetObjectPathString());
					Obj->SetStringField(TEXT("class"), Data.AssetClassPath.GetAssetName().ToString());
					Results.Add(MakeShared<FJsonValueObject>(Obj));
					if (Results.Num() >= 100) { break; }
				}

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("count"), Results.Num());
				Out->SetArrayField(TEXT("assets"), Results);
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// duplicate_asset
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("duplicate_asset");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Duplicate an asset. source is the object path; new_name is the new asset name; dest_path is the destination folder (defaults to the source folder).");
			Tool.InputSchema = WorldSchema({
				{ TEXT("source"), TEXT("Source asset object path.") },
				{ TEXT("new_name"), TEXT("New asset name.") },
				{ TEXT("dest_path"), TEXT("Optional destination folder.") },
			}, { TEXT("source"), TEXT("new_name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Source, NewName, DestPath;
				Args->TryGetStringField(TEXT("source"), Source);
				Args->TryGetStringField(TEXT("new_name"), NewName);
				Args->TryGetStringField(TEXT("dest_path"), DestPath);
				if (Source.IsEmpty() || NewName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'source' or 'new_name'.")); }
				UObject* Original = LoadObject<UObject>(nullptr, *Source);
				if (!Original) { return FClaudeToolResult::Error(FString::Printf(TEXT("Source asset '%s' not found."), *Source)); }
				if (DestPath.IsEmpty()) { DestPath = FPackageName::GetLongPackagePath(Original->GetPackage()->GetName()); }

				UObject* NewAsset = AssetTools().DuplicateAsset(NewName, DestPath, Original);
				if (!NewAsset) { return FClaudeToolResult::Error(TEXT("Duplication failed (name may already exist).")); }

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("asset"), NewAsset->GetPathName());
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// rename_asset
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("rename_asset");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Rename an asset. source is the object path; new_name is the new asset name.");
			Tool.InputSchema = WorldSchema({
				{ TEXT("source"), TEXT("Source asset object path.") },
				{ TEXT("new_name"), TEXT("New asset name.") },
			}, { TEXT("source"), TEXT("new_name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Source, NewName;
				Args->TryGetStringField(TEXT("source"), Source);
				Args->TryGetStringField(TEXT("new_name"), NewName);
				if (Source.IsEmpty() || NewName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'source' or 'new_name'.")); }
				UObject* Asset = LoadObject<UObject>(nullptr, *Source);
				if (!Asset) { return FClaudeToolResult::Error(FString::Printf(TEXT("Asset '%s' not found."), *Source)); }
				const FString FolderPath = FPackageName::GetLongPackagePath(Asset->GetPackage()->GetName());

				TArray<FAssetRenameData> Renames;
				Renames.Add(FAssetRenameData(Asset, FolderPath, NewName));
				const bool bOk = AssetTools().RenameAssets(Renames);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), bOk);
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// delete_asset (destructive)
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("delete_asset");
			Tool.Category = TEXT("asset");
			Tool.Description = TEXT("Delete an asset by object path. Shows the editor's delete confirmation dialog. This is irreversible if confirmed.");
			Tool.InputSchema = WorldSchema({ { TEXT("path"), TEXT("Asset object path to delete.") } }, { TEXT("path") });
			Tool.bIsDestructive = true;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Path;
				Args->TryGetStringField(TEXT("path"), Path);
				if (Path.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'path'.")); }
				UObject* Asset = LoadObject<UObject>(nullptr, *Path);
				if (!Asset) { return FClaudeToolResult::Error(FString::Printf(TEXT("Asset '%s' not found."), *Path)); }

				TArray<UObject*> ToDelete = { Asset };
				const int32 NumDeleted = ObjectTools::DeleteObjects(ToDelete, /*bShowConfirmation*/ !FApp::IsUnattended());

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), NumDeleted > 0);
				Out->SetNumberField(TEXT("deleted"), NumDeleted);
				return FClaudeToolResult::Ok(WorldSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}
	}
}
