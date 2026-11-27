#include "SBlueprintDiffEdit.h"
#include "SMaterialDiffEdit.h"

#include "AssetRegistry/AssetData.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorContext.h"
#include "BlueprintEditorModule.h"
#include "ContentBrowserMenuContexts.h"
#include "Engine/Blueprint.h"
#include "Framework/Docking/TabManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SNullWidget.h"

#define LOCTEXT_NAMESPACE "BlueprintDiffEditModule"

namespace
{
	const FName CHANGES_TAB("BlueprintDiffEdit.GitChanges");
}

class FBlueprintDiffEditModule final : public IModuleInterface
{
public:
	// A Git read can finish on the worker pool after its diff tab is closed.
	virtual bool SupportsDynamicReloading() override { return false; }

	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(CHANGES_TAB,
			FOnSpawnTab::CreateRaw(this, &FBlueprintDiffEditModule::DoSpawnChangesTab))
			.SetDisplayName(LOCTEXT("GitChanges", "Git Changes"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);
		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlueprintDiffEditModule::DoRegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CHANGES_TAB);
		if (const TSharedPtr<SDockTab> lChanges = m_ChangesTab.Pin())
		{
			lChanges->SetContent(SNullWidget::NullWidget);
			lChanges->RequestCloseTab();
		}
		for (const TWeakPtr<SBlueprintDiffEdit> & lWeakPanel : m_Panels)
		{
			if (const TSharedPtr<SBlueprintDiffEdit> lPanel = lWeakPanel.Pin())
				lPanel->Close();
		}
		for (const TWeakPtr<SMaterialDiffEdit> & lWeakPanel : m_MaterialPanels)
		{
			if (const TSharedPtr<SMaterialDiffEdit> lPanel = lWeakPanel.Pin())
				lPanel->Close();
		}
	}

private:
	void DoRegisterMenus()
	{
		const FToolMenuOwnerScoped lOwner(this);
		UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools")->FindOrAddSection("Source Control")
			.AddMenuEntry("BlueprintDiffEditGitChanges", LOCTEXT("GitChanges", "Git Changes"),
				LOCTEXT("GitChangesTip", "Open the changed files in the local Git working tree."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(CHANGES_TAB); })));
		UToolMenu * const lMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UBlueprint::StaticClass());
		lMenu->FindOrAddSection("GetAssetActions").AddDynamicEntry("BlueprintDiffEdit",
			FNewToolMenuSectionDelegate::CreateRaw(this, &FBlueprintDiffEditModule::DoPopulateMenu));

		for (UClass * const lClass : { UMaterial::StaticClass(), UMaterialInstanceConstant::StaticClass() })
		{
			UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(lClass)->FindOrAddSection("GetAssetActions")
				.AddDynamicEntry("MaterialDiffEdit",
					FNewToolMenuSectionDelegate::CreateRaw(this, &FBlueprintDiffEditModule::DoPopulateMaterialMenu));
		}
		for (const TCHAR * lToolbar : { TEXT("AssetEditor.MaterialEditor.ToolBar"), TEXT("AssetEditor.MaterialInstanceEditor.ToolBar") })
		{
			UToolMenus::Get()->ExtendMenu(lToolbar)->FindOrAddSection("MaterialDiffEdit")
				.AddDynamicEntry("MaterialDiffEditToolbar",
					FNewToolMenuSectionDelegate::CreateRaw(this, &FBlueprintDiffEditModule::DoPopulateMaterialToolbar));
		}

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

	void DoPopulateMaterialMenu(FToolMenuSection & aSection)
	{
		const UContentBrowserAssetContextMenuContext * const lContext =
			UContentBrowserAssetContextMenuContext::FindContextWithAssets(aSection);
		if (!lContext || lContext->SelectedAssets.Num() != 1)
			return;
		aSection.AddMenuEntry("OpenMaterialDiffEdit", LOCTEXT("MaterialDiff", "Material Diff…"),
			LOCTEXT("MaterialDiffTip", "Edit the working material alongside a historical revision."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintDiffEditModule::DoOpenMaterialAsset, lContext->SelectedAssets[0])));
	}

	void DoPopulateMaterialToolbar(FToolMenuSection & aSection)
	{
		const UAssetEditorToolkitMenuContext * const lContext = aSection.FindContext<UAssetEditorToolkitMenuContext>();
		if (!lContext)
			return;
		const TArray<UObject *> lObjects = lContext->GetEditingObjects();
		const auto * lAsset = lObjects.FindByPredicate(&SMaterialDiffEdit::Supports);
		if (!lAsset)
			return;
		aSection.AddEntry(FToolMenuEntry::InitToolBarButton("MaterialDiffEditToolbarButton",
			FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintDiffEditModule::DoOpenMaterial,
				TWeakObjectPtr<UMaterialInterface>(CastChecked<UMaterialInterface>(*lAsset)))),
			LOCTEXT("MaterialDiffButton", "Material Diff"),
			LOCTEXT("MaterialDiffTip", "Edit the working material alongside a historical revision."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "BlueprintDiff.ToolbarIcon")));
	}

	void DoOpenMaterialAsset(FAssetData aAsset)
	{
		DoOpenMaterial(Cast<UMaterialInterface>(aAsset.GetAsset()));
	}

	void DoOpenMaterial(TWeakObjectPtr<UMaterialInterface> aAsset)
	{
		UMaterialInterface * const lAsset = aAsset.Get();
		if (!SMaterialDiffEdit::Supports(lAsset))
			return;
		m_MaterialPanels.RemoveAll([](const TWeakPtr<SMaterialDiffEdit> & aPanel) { return !aPanel.IsValid(); });
		for (const TWeakPtr<SMaterialDiffEdit> & lWeakPanel : m_MaterialPanels)
		{
			if (const TSharedPtr<SMaterialDiffEdit> lPanel = lWeakPanel.Pin(); lPanel && lPanel->TryFocus(lAsset))
				return;
		}
		if (const TSharedPtr<SMaterialDiffEdit> lPanel = SMaterialDiffEdit::Open(lAsset))
			m_MaterialPanels.Add(lPanel);
	}

	TSharedRef<SDockTab> DoSpawnChangesTab(const FSpawnTabArgs & aArgs)
	{
		const TSharedRef<SDockTab> lTab = SNew(SDockTab).TabRole(ETabRole::NomadTab)
		[
			SNew(SBlueprintDiffChanges)
			.ContextFilename(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()))
			.OnBeforeReload(FBlueprintDiffBeforeReload::CreateRaw(this, &FBlueprintDiffEditModule::DoCloseAffectedPanels))
		];
		m_ChangesTab = lTab;
		return lTab;
	}

	void DoCloseAffectedPanels(const TArray<FString> & aFiles)
	{
		for (const TWeakPtr<SBlueprintDiffEdit> & lWeakPanel : m_Panels)
		{
			if (const TSharedPtr<SBlueprintDiffEdit> lPanel = lWeakPanel.Pin(); lPanel && lPanel->IsEditingAnyFile(aFiles))
				lPanel->Close();
		}
		for (const TWeakPtr<SMaterialDiffEdit> & lWeakPanel : m_MaterialPanels)
		{
			if (const TSharedPtr<SMaterialDiffEdit> lPanel = lWeakPanel.Pin(); lPanel && lPanel->IsEditingAnyFile(aFiles))
				lPanel->Close();
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
			aEditor.ToSharedRef(), lHostTab.ToSharedRef(), aBlueprint)
			.OnOpenBlueprint(FBlueprintDiffOpenAsset::CreateLambda([this](const FAssetData & aAsset) { DoOpen(aAsset); }))
			.OnBeforeReload(FBlueprintDiffBeforeReload::CreateRaw(this, &FBlueprintDiffEditModule::DoCloseAffectedPanels));
		lHostTab->SetContent(lPanel);
		m_Panels.Add(lPanel);
	}

	TArray<TWeakPtr<SBlueprintDiffEdit>> m_Panels;
	TArray<TWeakPtr<SMaterialDiffEdit>> m_MaterialPanels;
	TWeakPtr<SDockTab> m_ChangesTab;
};

IMPLEMENT_MODULE(FBlueprintDiffEditModule, BlueprintDiffEdit)

#undef LOCTEXT_NAMESPACE
