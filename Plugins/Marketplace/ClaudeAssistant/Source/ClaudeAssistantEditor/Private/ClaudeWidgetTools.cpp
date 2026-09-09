// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeWidgetTools.cpp

#include "ClaudeWidgetTools.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeAssistantFocusTracker.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/TextBlock.h"

namespace
{
	FString WidgetSerJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> WidgetSchema(const TArray<TPair<FString, FString>>& Props, const TArray<FString>& Required)
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

	// Target Widget Blueprint: explicit asset_path (LoadObject), else the focused asset.
	UWidgetBlueprint* ResolveWidgetBP(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path;
		if (Args.IsValid()) { Args->TryGetStringField(TEXT("asset_path"), Path); }
		if (!Path.IsEmpty()) { return LoadObject<UWidgetBlueprint>(nullptr, *Path); }
		return Cast<UWidgetBlueprint>(FClaudeAssistantFocusTracker::Get().GetActiveAsset());
	}
}

namespace ClaudeWidgetTools
{
	void RegisterAll(FClaudeToolRegistry& Registry)
	{
		// ---------------------------------------------------------------------
		// add_widget
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("add_widget");
			Tool.Category = TEXT("widget");
			Tool.Description = TEXT("Add a UMG widget to a Widget Blueprint's designer tree. class_name is a widget class short name ('Button','TextBlock','VerticalBox','CanvasPanel','Border', or any UWidget). If parent_name is empty the widget becomes the root; otherwise it is added under that panel. Operates on the focused Widget Blueprint unless asset_path is given. Undoable.");
			Tool.InputSchema = WidgetSchema({
				{ TEXT("class_name"), TEXT("Widget class short name (e.g. 'Button', 'TextBlock', 'VerticalBox').") },
				{ TEXT("name"), TEXT("Optional name for the new widget.") },
				{ TEXT("parent_name"), TEXT("Optional parent panel name; empty makes this the root.") },
				{ TEXT("asset_path"), TEXT("Optional Widget Blueprint object path; defaults to the focused asset.") },
			}, { TEXT("class_name") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWidgetBlueprint* WBP = ResolveWidgetBP(Args);
				if (!WBP || !WBP->WidgetTree) { return FClaudeToolResult::Error(TEXT("No Widget Blueprint is focused (or pass asset_path).")); }
				UWidgetTree* Tree = WBP->WidgetTree;

				FString ClassName, NewNameStr, ParentStr;
				Args->TryGetStringField(TEXT("class_name"), ClassName);
				Args->TryGetStringField(TEXT("name"), NewNameStr);
				Args->TryGetStringField(TEXT("parent_name"), ParentStr);
				if (ClassName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'class_name'.")); }

				UClass* Cls = UClass::TryFindTypeSlow<UClass>(ClassName, EFindFirstObjectOptions::NativeFirst);
				if (!Cls && ClassName.StartsWith(TEXT("U")))
				{
					FString Alt = ClassName; Alt.RightChopInline(1);
					Cls = UClass::TryFindTypeSlow<UClass>(Alt, EFindFirstObjectOptions::NativeFirst);
				}
				if (!Cls || !Cls->IsChildOf(UWidget::StaticClass()) || Cls->HasAnyClassFlags(CLASS_Abstract))
				{
					return FClaudeToolResult::Error(FString::Printf(TEXT("Widget class '%s' not found."), *ClassName));
				}
				const FName NewName = NewNameStr.IsEmpty() ? NAME_None : FName(*NewNameStr);

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: add widget")));
				WBP->Modify();
				Tree->Modify();
				UWidget* NewWidget = Tree->ConstructWidget<UWidget>(Cls, NewName);
				if (!NewWidget) { return FClaudeToolResult::Error(TEXT("Failed to construct the widget.")); }

				if (ParentStr.IsEmpty())
				{
					Tree->RootWidget = NewWidget;
				}
				else
				{
					UPanelWidget* Parent = Cast<UPanelWidget>(Tree->FindWidget(FName(*ParentStr)));
					if (!Parent) { return FClaudeToolResult::Error(FString::Printf(TEXT("Parent panel '%s' not found (or is not a panel)."), *ParentStr)); }
					Parent->Modify();
					UPanelSlot* Slot = Parent->AddChild(NewWidget);
					if (!Slot) { return FClaudeToolResult::Error(TEXT("AddChild failed (the parent may be a single-child widget that is already full).")); }
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetStringField(TEXT("name"), NewWidget->GetName());
				Out->SetStringField(TEXT("class"), Cls->GetName());
				return FClaudeToolResult::Ok(WidgetSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// set_widget_text
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("set_widget_text");
			Tool.Category = TEXT("widget");
			Tool.Description = TEXT("Set the text of a TextBlock in a Widget Blueprint's tree, by widget name. Operates on the focused Widget Blueprint unless asset_path is given. Undoable.");
			Tool.InputSchema = WidgetSchema({
				{ TEXT("widget_name"), TEXT("Name of the TextBlock.") },
				{ TEXT("text"), TEXT("New text.") },
				{ TEXT("asset_path"), TEXT("Optional Widget Blueprint object path; defaults to the focused asset.") },
			}, { TEXT("widget_name"), TEXT("text") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWidgetBlueprint* WBP = ResolveWidgetBP(Args);
				if (!WBP || !WBP->WidgetTree) { return FClaudeToolResult::Error(TEXT("No Widget Blueprint is focused (or pass asset_path).")); }

				FString WName, Text;
				Args->TryGetStringField(TEXT("widget_name"), WName);
				Args->TryGetStringField(TEXT("text"), Text);
				if (WName.IsEmpty()) { return FClaudeToolResult::Error(TEXT("Missing 'widget_name'.")); }

				UTextBlock* TB = Cast<UTextBlock>(WBP->WidgetTree->FindWidget(FName(*WName)));
				if (!TB) { return FClaudeToolResult::Error(FString::Printf(TEXT("No TextBlock named '%s'."), *WName)); }

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: set widget text")));
				WBP->Modify();
				TB->Modify();
				TB->SetText(FText::FromString(Text));
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				return FClaudeToolResult::Ok(WidgetSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// list_widgets
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("list_widgets");
			Tool.Category = TEXT("widget");
			Tool.Description = TEXT("List every widget in a Widget Blueprint's tree (name, class, is_root). Operates on the focused Widget Blueprint unless asset_path is given.");
			Tool.InputSchema = WidgetSchema({
				{ TEXT("asset_path"), TEXT("Optional Widget Blueprint object path; defaults to the focused asset.") },
			}, {});
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWidgetBlueprint* WBP = ResolveWidgetBP(Args);
				if (!WBP || !WBP->WidgetTree) { return FClaudeToolResult::Error(TEXT("No Widget Blueprint is focused (or pass asset_path).")); }

				const UWidget* Root = WBP->WidgetTree->RootWidget;
				TArray<UWidget*> All;
				WBP->WidgetTree->GetAllWidgets(All);

				TArray<TSharedPtr<FJsonValue>> Widgets;
				for (const UWidget* W : All)
				{
					if (!W) { continue; }
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), W->GetName());
					Obj->SetStringField(TEXT("class"), W->GetClass() ? W->GetClass()->GetName() : FString());
					Obj->SetBoolField(TEXT("is_root"), W == Root);
					Widgets.Add(MakeShared<FJsonValueObject>(Obj));
				}

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("count"), Widgets.Num());
				Out->SetArrayField(TEXT("widgets"), Widgets);
				return FClaudeToolResult::Ok(WidgetSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// set_widget_property
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("set_widget_property");
			Tool.Category = TEXT("widget");
			Tool.Description = TEXT("Set a property on a widget in a Widget Blueprint's tree, by widget name, property name, and value as text (e.g. 'ColorAndOpacity', 'Visibility', 'ToolTipText'). Operates on the focused Widget Blueprint unless asset_path is given. Undoable.");
			Tool.InputSchema = WidgetSchema({
				{ TEXT("widget_name"), TEXT("Widget name.") },
				{ TEXT("property"), TEXT("Property name.") },
				{ TEXT("value"), TEXT("Value as text.") },
				{ TEXT("asset_path"), TEXT("Optional Widget Blueprint object path; defaults to the focused asset.") },
			}, { TEXT("widget_name"), TEXT("property"), TEXT("value") });
			Tool.bIsDestructive = false;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWidgetBlueprint* WBP = ResolveWidgetBP(Args);
				if (!WBP || !WBP->WidgetTree) { return FClaudeToolResult::Error(TEXT("No Widget Blueprint is focused (or pass asset_path).")); }
				FString WName, PropName, Value;
				Args->TryGetStringField(TEXT("widget_name"), WName);
				Args->TryGetStringField(TEXT("property"), PropName);
				Args->TryGetStringField(TEXT("value"), Value);
				UWidget* W = WBP->WidgetTree->FindWidget(FName(*WName));
				if (!W) { return FClaudeToolResult::Error(FString::Printf(TEXT("Widget '%s' not found."), *WName)); }
				FProperty* Prop = W->GetClass()->FindPropertyByName(FName(*PropName));
				if (!Prop) { return FClaudeToolResult::Error(FString::Printf(TEXT("Property '%s' not found on '%s'."), *PropName, *W->GetClass()->GetName())); }

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: set widget property")));
				WBP->Modify();
				W->Modify();
				const TCHAR* Result = Prop->ImportText_Direct(*Value, Prop->ContainerPtrToValuePtr<void>(W), W, PPF_None);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), Result != nullptr);
				return FClaudeToolResult::Ok(WidgetSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}

		// ---------------------------------------------------------------------
		// remove_widget
		// ---------------------------------------------------------------------
		{
			FClaudeTool Tool;
			Tool.Name = TEXT("remove_widget");
			Tool.Category = TEXT("widget");
			Tool.Description = TEXT("Remove a widget (and its children) from a Widget Blueprint's tree, by name. Operates on the focused Widget Blueprint unless asset_path is given. Undoable.");
			Tool.InputSchema = WidgetSchema({
				{ TEXT("widget_name"), TEXT("Widget name to remove.") },
				{ TEXT("asset_path"), TEXT("Optional Widget Blueprint object path; defaults to the focused asset.") },
			}, { TEXT("widget_name") });
			Tool.bIsDestructive = true;
			Tool.Execute.BindLambda([](const TSharedPtr<FJsonObject>& Args) -> FClaudeToolResult
			{
				UWidgetBlueprint* WBP = ResolveWidgetBP(Args);
				if (!WBP || !WBP->WidgetTree) { return FClaudeToolResult::Error(TEXT("No Widget Blueprint is focused (or pass asset_path).")); }
				FString WName;
				Args->TryGetStringField(TEXT("widget_name"), WName);
				UWidget* W = WBP->WidgetTree->FindWidget(FName(*WName));
				if (!W) { return FClaudeToolResult::Error(FString::Printf(TEXT("Widget '%s' not found."), *WName)); }

				const FScopedTransaction Transaction(FText::FromString(TEXT("Claude: remove widget")));
				WBP->Modify();
				WBP->WidgetTree->Modify();
				const bool bOk = WBP->WidgetTree->RemoveWidget(W);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), bOk);
				return FClaudeToolResult::Ok(WidgetSerJson(Out));
			});
			Registry.RegisterTool(MoveTemp(Tool));
		}
	}
}
