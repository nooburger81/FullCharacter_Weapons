// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeToolRegistry.cpp

#include "ClaudeToolRegistry.h"
#include "ClaudeReadTools.h"
#include "ClaudeWriteTools.h"
#include "ClaudeRuntimeTools.h"
#include "ClaudeAssetTools.h"
#include "ClaudeWidgetTools.h"
#include "ClaudeAnimTools.h"
#include "ClaudeBtTools.h"
#include "ClaudeNodeActionTools.h"
#include "ClaudeWorldTools.h"
#include "ClaudeMaterialTools.h"

FClaudeToolRegistry& FClaudeToolRegistry::Get()
{
	static FClaudeToolRegistry Instance;
	return Instance;
}

void FClaudeToolRegistry::RegisterTool(FClaudeTool Tool)
{
	if (!Tool.Name.IsEmpty())
	{
		Tools.Add(Tool.Name, MoveTemp(Tool));
	}
}

bool FClaudeToolRegistry::HasTool(const FString& Name) const
{
	return Tools.Contains(Name);
}

const FClaudeTool* FClaudeToolRegistry::FindTool(const FString& Name) const
{
	return Tools.Find(Name);
}

TArray<FString> FClaudeToolRegistry::GetToolNames() const
{
	TArray<FString> Out;
	Tools.GetKeys(Out);
	return Out;
}

TArray<TSharedPtr<FJsonValue>> FClaudeToolRegistry::GetToolSchemasJson() const
{
	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(Tools.Num());

	for (const TPair<FString, FClaudeTool>& Pair : Tools)
	{
		// Curation: only advertise enabled tools so the model's tool list stays lean.
		if (!IsToolEnabled(Pair.Key))
		{
			continue;
		}

		const FClaudeTool& Tool = Pair.Value;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Tool.Name);
		Obj->SetStringField(TEXT("description"), Tool.Description);
		Obj->SetObjectField(TEXT("inputSchema"),
			Tool.InputSchema.IsValid() ? Tool.InputSchema : MakeShared<FJsonObject>());

		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return Out;
}

FClaudeToolResult FClaudeToolRegistry::ExecuteTool(const FString& Name, const TSharedPtr<FJsonObject>& Args) const
{
	const FClaudeTool* Tool = Tools.Find(Name);
	if (!Tool)
	{
		return FClaudeToolResult::Error(FString::Printf(TEXT("Unknown tool: %s"), *Name));
	}
	if (!IsToolEnabled(Name))
	{
		return FClaudeToolResult::Error(FString::Printf(TEXT("Tool '%s' is disabled by project curation."), *Name));
	}
	if (!Tool->Execute.IsBound())
	{
		return FClaudeToolResult::Error(FString::Printf(TEXT("Tool '%s' has no handler bound."), *Name));
	}
	return Tool->Execute.Execute(Args);
}

void FClaudeToolRegistry::RegisterBuiltinTools()
{
	if (bBuiltinsRegistered)
	{
		return;
	}
	ClaudeReadTools::RegisterAll(*this);
	ClaudeWriteTools::RegisterAll(*this);
	ClaudeRuntimeTools::RegisterAll(*this);
	ClaudeAssetTools::RegisterAll(*this);
	ClaudeWidgetTools::RegisterAll(*this);
	ClaudeAnimTools::RegisterAll(*this);
	ClaudeBtTools::RegisterAll(*this);
	ClaudeNodeActionTools::RegisterAll(*this);
	ClaudeWorldTools::RegisterAll(*this);
	ClaudeMaterialTools::RegisterAll(*this);
	bBuiltinsRegistered = true;
}

void FClaudeToolRegistry::SetToolEnabled(const FString& Name, bool bEnabled)
{
	if (bEnabled)
	{
		DisabledTools.Remove(Name);
	}
	else
	{
		DisabledTools.Add(Name);
	}
}

void FClaudeToolRegistry::SetCategoryEnabled(const FString& Category, bool bEnabled)
{
	if (bEnabled)
	{
		DisabledCategories.Remove(Category);
	}
	else
	{
		DisabledCategories.Add(Category);
	}
}

bool FClaudeToolRegistry::IsToolEnabled(const FString& Name) const
{
	const FClaudeTool* Tool = Tools.Find(Name);
	if (!Tool)
	{
		return false;
	}
	if (DisabledTools.Contains(Name))
	{
		return false;
	}
	if (!Tool->Category.IsEmpty() && DisabledCategories.Contains(Tool->Category))
	{
		return false;
	}
	return true;
}
