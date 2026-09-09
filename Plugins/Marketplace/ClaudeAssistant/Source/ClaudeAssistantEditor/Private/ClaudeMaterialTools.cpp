// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeMaterialTools.cpp

#include "ClaudeMaterialTools.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantFocusTracker.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "SceneTypes.h"

namespace
{
	FString MatSerJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> MatSchema(const TArray<TPair<FString, FString>>& Props, const TArray<FString>& Required)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Props)
		{
			TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
			Prop->SetStringField(TEXT("type"), (Pair.Key == TEXT("x") || Pair.Key == TEXT("y")) ? TEXT("number") : TEXT("string"));
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

	UMaterial* ResolveMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path;
		if (Args.IsValid()) { Args->TryGetStringField(TEXT("asset_path"), Path); }
		if (!Path.IsEmpty()) { return LoadObject<UMaterial>(nullptr, *Path); }
		return Cast<UMaterial>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}

	// Session cache of created expressions (by name) so connect/set can reference them.
	TMap<FString, TWeakObjectPtr<UMaterialExpression>>& MatExprCache()
	{
		static TMap<FString, TWeakObjectPtr<UMaterialExpression>> Cache;
		return Cache;
	}

	UMaterialExpression* FindExpr(const FString& Id)
	{
		if (const TWeakObjectPtr<UMaterialExpression>* Found = MatExprCache().Find(Id))
		{
			return Found->Get();
		}
		return nullptr;
	}

	bool ResolveMaterialProperty(const FString& In, EMaterialProperty& Out)
	{
		const FString T = In.ToLower().Replace(TEXT("_"), TEXT(""));
		if (T == TEXT("basecolor") || T == TEXT("color")) { Out = MP_BaseColor; return true; }
		if (T == TEXT("metallic")) { Out = MP_Metallic; return true; }
		if (T == TEXT("specular")) { Out = MP_Specular; return true; }
		if (T == TEXT("roughness")) { Out = MP_Roughness; return true; }
		if (T == TEXT("emissive") || T == TEXT("emissivecolor")) { Out = MP_EmissiveColor; return true; }
		if (T == TEXT("normal")) { Out = MP_Normal; return true; }
		if (T == TEXT("opacity")) { Out = MP_Opacity; return true; }
		if (T == TEXT("opacitymask")) { Out = MP_OpacityMask; return true; }
		if (T == TEXT("ao") || T == TEXT("ambientocclusion")) { Out = MP_AmbientOcclusion; return true; }
		if (T == TEXT("wpo") || T == TEXT("worldpositionoffset")) { Out = MP_WorldPositionOffset; return true; }
		return false;
	}
}

namespace ClaudeMaterialTools
{
	void RegisterAll(FClaudeToolRegistry& Registry)
	{
		// create_material_expression
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("create_material_expression");
			Tool.Category = TEXT("material");
			Tool.Description = TEXT("Add an expression node to the focused Material. class is a material expression class name (e.g. 'Constant3Vector', 'ScalarParameter', 'TextureSample', 'Multiply'); the 'MaterialExpression' prefix is optional. Returns the expression id for connect/set tools.");
			Tool.InputSchema = MatSchema({
				{ TEXT("class"), TEXT("Material expression class (e.g. 'Constant3Vector', 'Multiply').") },
				{ TEXT("x"), TEXT("Optional X position.") },
				{ TEXT("y"), TEXT("Optional Y position.") },
				{ TEXT("asset_path"), TEXT("Optional Material object path; defaults to the focused asset.") },
			}, { TEXT("class") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UMaterial* Material = ResolveMaterial(Args);
				if (!Material) { return FClaudeToolResult::Error(TEXT("No Material is focused (or pass asset_path).")); }
				FString ClassName;
				Args->TryGetStringField(TEXT("class"), ClassName);
				if (ClassName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'class'.")); }
				UClass* ExprClass = UClass::TryFindTypeSlow<UClass>(ClassName);
				if (!ExprClass && !ClassName.StartsWith(TEXT("MaterialExpression")))
				{
					ExprClass = UClass::TryFindTypeSlow<UClass>(FString(TEXT("MaterialExpression")) + ClassName);
				}
				if (!ExprClass || !ExprClass->IsChildOf(UMaterialExpression::StaticClass()) || ExprClass->HasAnyClassFlags(CLASS_Abstract))
				{
					return FClaudeToolResult::Error(FString::Printf(TEXT("Material expression class '%s' not found."), *ClassName));
				}
				double X = 0.0, Y = 0.0;
				Args->TryGetNumberField(TEXT("x"), X);
				Args->TryGetNumberField(TEXT("y"), Y);

				UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExprClass, static_cast<int32>(X), static_cast<int32>(Y));
				if (!Expr) { return FClaudeToolResult::Error(TEXT("Failed to create the expression.")); }
				// Key by full path (globally unique) so expressions in different materials never collide.
				const FString Id = Expr->GetPathName();
				MatExprCache().Add(Id, Expr);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("expression"), Id);
				return FClaudeToolResult::Ok(MatSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// set_material_expression_property
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("set_material_expression_property");
			Tool.Category = TEXT("material");
			Tool.Description = TEXT("Set a property on a material expression (from create_material_expression), by expression id, property name, and value as text (e.g. a Constant3Vector's 'Constant', a ScalarParameter's 'DefaultValue' or 'ParameterName').");
			Tool.InputSchema = MatSchema({
				{ TEXT("expression"), TEXT("Expression id.") },
				{ TEXT("property"), TEXT("Property name.") },
				{ TEXT("value"), TEXT("Value as text.") },
			}, { TEXT("expression"), TEXT("property"), TEXT("value") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString Id, PropName, Value;
				Args->TryGetStringField(TEXT("expression"), Id);
				Args->TryGetStringField(TEXT("property"), PropName);
				Args->TryGetStringField(TEXT("value"), Value);
				UMaterialExpression* Expr = FindExpr(Id);
				if (!Expr) { return FClaudeToolResult::Error(TEXT("Expression not found (re-create it).")); }
				FProperty* Prop = Expr->GetClass()->FindPropertyByName(FName(*PropName));
				if (!Prop) { return FClaudeToolResult::Error(FString::Printf(TEXT("Property '%s' not found on '%s'."), *PropName, *Expr->GetClass()->GetName())); }

				Expr->Modify();
				const TCHAR* Result = Prop->ImportText_Direct(*Value, Prop->ContainerPtrToValuePtr<void>(Expr), Expr, PPF_None);
				Expr->PostEditChange();

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), Result != nullptr);
				return FClaudeToolResult::Ok(MatSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// connect_to_material_property
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("connect_to_material_property");
			Tool.Category = TEXT("material");
			Tool.Description = TEXT("Connect a material expression output to a material output property. property: base_color/metallic/roughness/specular/emissive/normal/opacity/ao/wpo. output is the expression's output name (empty for the default).");
			Tool.InputSchema = MatSchema({
				{ TEXT("expression"), TEXT("Expression id.") },
				{ TEXT("property"), TEXT("Material property (e.g. 'base_color').") },
				{ TEXT("output"), TEXT("Optional output pin name; empty for default.") },
				{ TEXT("asset_path"), TEXT("Optional Material object path; defaults to the focused asset.") },
			}, { TEXT("expression"), TEXT("property") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UMaterial* Material = ResolveMaterial(Args);
				if (!Material) { return FClaudeToolResult::Error(TEXT("No Material is focused (or pass asset_path).")); }
				FString Id, PropStr, Output;
				Args->TryGetStringField(TEXT("expression"), Id);
				Args->TryGetStringField(TEXT("property"), PropStr);
				Args->TryGetStringField(TEXT("output"), Output);
				UMaterialExpression* Expr = FindExpr(Id);
				if (!Expr) { return FClaudeToolResult::Error(TEXT("Expression not found (re-create it).")); }
				EMaterialProperty Property;
				if (!ResolveMaterialProperty(PropStr, Property)) { return FClaudeToolResult::Error(FString::Printf(TEXT("Unknown material property '%s'."), *PropStr)); }

				const bool bOk = UMaterialEditingLibrary::ConnectMaterialProperty(Expr, Output, Property);
				UMaterialEditingLibrary::RecompileMaterial(Material);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), bOk);
				return FClaudeToolResult::Ok(MatSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// connect_material_expressions
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("connect_material_expressions");
			Tool.Category = TEXT("material");
			Tool.Description = TEXT("Connect one material expression's output to another's input. Provide the two expression ids, the source output name (empty for default), and the target input name.");
			Tool.InputSchema = MatSchema({
				{ TEXT("from"), TEXT("Source expression id.") },
				{ TEXT("from_output"), TEXT("Source output name (empty for default).") },
				{ TEXT("to"), TEXT("Target expression id.") },
				{ TEXT("to_input"), TEXT("Target input name.") },
			}, { TEXT("from"), TEXT("to"), TEXT("to_input") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				FString FromId, FromOut, ToId, ToIn;
				Args->TryGetStringField(TEXT("from"), FromId);
				Args->TryGetStringField(TEXT("from_output"), FromOut);
				Args->TryGetStringField(TEXT("to"), ToId);
				Args->TryGetStringField(TEXT("to_input"), ToIn);
				UMaterialExpression* From = FindExpr(FromId);
				UMaterialExpression* To = FindExpr(ToId);
				if (!From || !To) { return FClaudeToolResult::Error(TEXT("Expression(s) not found (re-create them).")); }

				const bool bOk = UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOut, To, ToIn);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), bOk);
				return FClaudeToolResult::Ok(MatSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// recompile_material
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("recompile_material");
			Tool.Category = TEXT("material");
			Tool.Description = TEXT("Recompile the focused Material to apply pending changes.");
			Tool.InputSchema = MatSchema({
				{ TEXT("asset_path"), TEXT("Optional Material object path; defaults to the focused asset.") },
			}, {});
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UMaterial* Material = ResolveMaterial(Args);
				if (!Material) { return FClaudeToolResult::Error(TEXT("No Material is focused (or pass asset_path).")); }
				UMaterialEditingLibrary::RecompileMaterial(Material);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				return FClaudeToolResult::Ok(MatSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}
	}
}
