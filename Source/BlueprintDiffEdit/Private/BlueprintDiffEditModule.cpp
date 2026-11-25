#include "SBlueprintDiffEdit.h"

#include "AssetRegistry/AssetData.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorContext.h"
#include "BlueprintEditorModule.h"
#include "ContentBrowserMenuContexts.h"
#include "Engine/Blueprint.h"
#include "Framework/Docking/TabManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "BlueprintDiffEditModule"

class FBlueprintDiffEditModule final : public IModuleInterface
{
public:
	// A Git read can finish on the worker pool after its diff tab is closed.
	virtual bool SupportsDynamicReloading() override { return false; }

	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlueprintDiffEditModule::DoRegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		for (const TWeakPtr<SBlueprintDiffEdit> & lWeakPanel : m_Panels)
		{
			if (const TSharedPtr<SBlueprintDiffEdit> lPanel = lWeakPanel.Pin())
				lPanel->Close();
		}
	}

private:
	void DoRegisterMenus()
	{
		const FToolMenuOwnerScoped lOwner(this);
		UToolMenu * const lMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UBlueprint::StaticClass());
		lMenu->FindOrAddSection("GetAssetActions").AddDynamicEntry("BlueprintDiffEdit",
			FNewToolMenuSectionDelegate::CreateRaw(this, &FBlueprintDiffEditModule::DoPopulateMenu));

		const FName lToolbars[] = {
			TEXT("AssetEditor.BlueprintEditor.ToolBar.GraphName"),
			TEXT("AssetEditor.WidgetBlueprintEditor.ToolBar.DesignerName"),
			TEXT("AssetEditor.WidgetBlueprintEditor.ToolBar.GraphName")
		};
		for (const FName lName : lToolbars)
		{
			UToolMenu * const lToolbar = UToolMenus::Get()->ExtendMenu(lName);
			FToolMenuSection & lSection = lToolbar->FindOrAddSection("BlueprintDiffEdit");
			lSection.InsertPosition = FToolMenuInsert("Debugging", EToolMenuInsertType::After);
			lSection.AddDynamicEntry("BlueprintDiffEditToolbar",
				FNewToolMenuSectionDelegate::CreateRaw(this, &FBlueprintDiffEditModule::DoPopulateToolbar));
		}
	}

	void DoPopulateMenu(FToolMenuSection & aSection)
	{
		const UContentBrowserAssetContextMenuContext * const lContext =
			UContentBrowserAssetContextMenuContext::FindContextWithAssets(aSection);
		if (!lContext || lContext->SelectedAssets.Num() != 1)
			return;

		const FAssetData lAsset = lContext->SelectedAssets[0];
		aSection.AddMenuEntry("OpenBlueprintDiffEdit", LOCTEXT("DiffAndEdit", "Diff & Edit…"),
			LOCTEXT("DiffAndEditTip", "Review a source-control revision beside the editable working Blueprint."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintDiffEditModule::DoOpen, lAsset)));
	}

	void DoPopulateToolbar(FToolMenuSection & aSection)
	{
		const UBlueprintEditorToolMenuContext * const lContext = aSection.FindContext<UBlueprintEditorToolMenuContext>();
		if (!lContext || !lContext->GetBlueprintObj() || !lContext->BlueprintEditor.IsValid())
			return;

		FToolMenuEntry & lEntry = aSection.AddEntry(FToolMenuEntry::InitToolBarButton("BlueprintDiffEditToolbarButton",
			FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintDiffEditModule::DoOpenFromToolbar, lContext->BlueprintEditor)),
			LOCTEXT("DiffAndEditToolbar", "Diff & Edit"),
			LOCTEXT("DiffAndEditTip", "Review a source-control revision beside the editable working Blueprint."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "BlueprintDiff.ToolbarIcon")));
		lEntry.StyleNameOverride = "CalloutToolbar";
	}

	void DoOpenFromToolbar(TWeakPtr<FBlueprintEditor> aWeakEditor)
	{
		if (const TSharedPtr<FBlueprintEditor> lEditor = aWeakEditor.Pin())
		{
			if (UBlueprint * const lBlueprint = lEditor->GetBlueprintObj())
				DoOpenPanel(lEditor, lBlueprint);
		}
	}

	void DoOpen(FAssetData aAsset)
	{
		if (UBlueprint * const lBlueprint = Cast<UBlueprint>(aAsset.GetAsset()))
			DoOpenPanel(FKismetEditorUtilities::GetIBlueprintEditorForObject(lBlueprint, true), lBlueprint);
	}

	void DoOpenPanel(const TSharedPtr<IBlueprintEditor> & aEditor, UBlueprint * aBlueprint)
	{
		const TSharedPtr<FTabManager> lTabManager = aEditor ? aEditor->GetTabManager() : nullptr;
		const TSharedPtr<SDockTab> lHostTab = lTabManager ? lTabManager->GetOwnerTab() : nullptr;
		if (!lHostTab)
		{
			FMessageDialog::Open(EAppMsgType::Ok,
				LOCTEXT("StandaloneRequired", "Diff & Edit needs a standalone Blueprint editor tab."));
			return;
		}

		m_Panels.RemoveAll([](const TWeakPtr<SBlueprintDiffEdit> & aPanel) { return !aPanel.IsValid(); });
		for (const TWeakPtr<SBlueprintDiffEdit> & lWeakPanel : m_Panels)
		{
			const TSharedPtr<SBlueprintDiffEdit> lPanel = lWeakPanel.Pin();
			if (lPanel && lPanel->IsHostedBy(lHostTab.Get()))
				return;
		}

		const TSharedRef<SBlueprintDiffEdit> lPanel = SNew(SBlueprintDiffEdit,
			aEditor.ToSharedRef(), lHostTab.ToSharedRef(), aBlueprint);
		lHostTab->SetContent(lPanel);
		m_Panels.Add(lPanel);
	}

	TArray<TWeakPtr<SBlueprintDiffEdit>> m_Panels;
};

IMPLEMENT_MODULE(FBlueprintDiffEditModule, BlueprintDiffEdit)

#undef LOCTEXT_NAMESPACE
