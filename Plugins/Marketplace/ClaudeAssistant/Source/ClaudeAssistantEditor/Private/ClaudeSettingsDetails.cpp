// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeSettingsDetails.cpp

#include "ClaudeSettingsDetails.h"
#include "ClaudeAssistantSettings.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"

#define LOCTEXT_NAMESPACE "ClaudeSettingsDetails"

TSharedRef<IDetailCustomization> FClaudeSettingsDetails::MakeInstance()
{
	return MakeShareable(new FClaudeSettingsDetails);
}

void FClaudeSettingsDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	IDetailCategoryBuilder& Feedback = DetailBuilder.EditCategory(
		TEXT("Feedback"), LOCTEXT("FeedbackCategory", "Feedback"), ECategoryPriority::Uncommon);

	Feedback.AddCustomRow(LOCTEXT("FeedbackFilter", "Feedback Suggest a Feature Report a Bug"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.f, 4.f, 8.f, 4.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("SuggestFeature", "Suggest a Feature"))
			.ToolTipText(LOCTEXT("SuggestFeatureTip", "Open pixelsdesign.it/feature-request.html in your default browser"))
			.OnClicked_Lambda([]()
			{
				UClaudeAssistantSettings::OpenFeatureRequest();
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.f, 4.f, 8.f, 4.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("ReportBug", "Report a Bug"))
			.ToolTipText(LOCTEXT("ReportBugTip", "Open pixelsdesign.it/bug-report.html in your default browser"))
			.OnClicked_Lambda([]()
			{
				UClaudeAssistantSettings::OpenBugReport();
				return FReply::Handled();
			})
		]
	];
}

#undef LOCTEXT_NAMESPACE
