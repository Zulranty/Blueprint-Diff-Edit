#include "SMaterialDiffEdit.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BlueprintDiffGraphPanel.h"
#include "Editor.h"
#include "Framework/Commands/UICommandList.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IMaterialEditor.h"
#include "MaterialEditorActions.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "SGraphPanel.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"

BEGIN_DEFINE_SPEC(FMaterialDiffTests, "BlueprintDiffEdit.Material",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FMaterialDiffTests)

namespace
{
	void CloseMaterialFixture(const TSharedPtr<SMaterialDiffEdit> & aDiff, UMaterialInterface * aMaterial)
	{
		if (aDiff)
			aDiff->Close();
		if (IAssetEditorInstance * const lInstance = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(aMaterial, false))
			lInstance->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
	}
}

void FMaterialDiffTests::Define()
{
	It("Loads a real Git material revision and compares graphs without editing the working asset", [this]
	{
		const FString lFolder = TEXT("MaterialDiffFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString lRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / lFolder);
		IFileManager::Get().MakeDirectory(*lRoot, true);
		ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*lRoot, false, true); };
		const FString lFile = lRoot / TEXT("M_Fixture.uasset");
		UPackage * const lPackage = CreatePackage(*(TEXT("/Game/") + lFolder + TEXT("/M_Fixture")));
		const TStrongObjectPtr<UMaterial> lMaterial(NewObject<UMaterial>(lPackage, TEXT("M_Fixture"), RF_Public | RF_Standalone));
		ON_SCOPE_EXIT { lPackage->SetDirtyFlag(false); };

		UMaterialExpressionConstant * const lExpression = NewObject<UMaterialExpressionConstant>(lMaterial.Get());
		lExpression->Material = lMaterial.Get();
		lExpression->UpdateMaterialExpressionGuid(true, false);
		lExpression->R = 0.25f;
		lMaterial->GetExpressionCollection().AddExpression(lExpression);
		lMaterial->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, lExpression);

		FSavePackageArgs lSaveArgs;
		lSaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!TestTrue(TEXT("Baseline material saved"), UPackage::SavePackage(lPackage, lMaterial.Get(), *lFile, lSaveArgs)))
			return;
		const auto lGit = [&](const TCHAR * aCommand)
		{
			int32 lExit = INDEX_NONE;
			FString lOutput, lError;
			const FString lArguments = FString::Printf(TEXT("-C \"%s\" -c core.hooksPath=.git/no-hooks -c user.name=Fixture ")
				TEXT("-c user.email=diff@example.invalid -c commit.gpgsign=false %s"), *lRoot, aCommand);
			return TestTrue(aCommand, FPlatformProcess::ExecProcess(*BlueprintDiffGit::GetExecutable(),
				*lArguments, &lExit, &lOutput, &lError) && lExit == 0);
		};
		if (!lGit(TEXT("init --initial-branch=main")) || !lGit(TEXT("add -- M_Fixture.uasset")) || !lGit(TEXT("commit -m Baseline")))
			return;

		lExpression->R = 0.75f;
		lExpression->MaterialExpressionEditorX = 256;
		const auto * lOriginalNode = lExpression->GraphNode.Get();
		const auto * lOriginalGraph = lMaterial->MaterialGraph.Get();
		const TSharedPtr<SMaterialDiffEdit> lDiff = SMaterialDiffEdit::Open(lMaterial.Get());
		ON_SCOPE_EXIT { CloseMaterialFixture(lDiff, lMaterial.Get()); };
		if (!TestTrue(TEXT("Material Diff opens in the original editor tab"), lDiff.IsValid()))
			return;
		if (!TestTrue(TEXT("Git history completed"), lDiff->m_History.WaitFor(FTimespan::FromSeconds(12))))
			return;
		lDiff->Tick(FGeometry(), 0.0, 0.0f);
		if (!TestTrue(TEXT("Git snapshot completed"), lDiff->m_Request.IsValid() && lDiff->m_Request.WaitFor(FTimespan::FromSeconds(12))))
			return;
		lDiff->Tick(FGeometry(), 0.0, 0.0f);
		lDiff->SlatePrepass(1.0f);
		TestTrue(TEXT("Both material graphs are shown"), lDiff->m_OldGraph.IsValid() && lDiff->m_LockedGraph.IsValid());
		TestTrue(TEXT("Material node value changed"), lDiff->m_Changes.ContainsByPredicate([](const auto & aChange)
			{ return aChange->Display.Diff == EDiffType::NODE_PROPERTY && aChange->Display.ToolTip.ToString().Contains(TEXT("0.25"))
				&& aChange->Display.ToolTip.ToString().Contains(TEXT("0.75")); }));
		TestTrue(TEXT("Native graph reports node movement"), lDiff->m_Changes.ContainsByPredicate([](const auto & aChange)
			{ return aChange->Display.Diff == EDiffType::NODE_MOVED; }));
		TestTrue(TEXT("Working expression and graph stay untouched"), lExpression->GraphNode.Get() == lOriginalNode
			&& lMaterial->MaterialGraph.Get() == lOriginalGraph && lExpression->R == 0.75f);
	});

	It("Edits the native working Material graph alongside a revision", [this]
	{
		const TStrongObjectPtr<UMaterial> lRevision(NewObject<UMaterial>());
		UPackage * const lPackage = CreatePackage(*FString::Printf(TEXT("/Game/MaterialEditorDiff_%s"), *FGuid::NewGuid().ToString()));
		// Apply reconstructs the original UObject at the same address. The native editor keeps it alive.
		UMaterial * const lMaterial = NewObject<UMaterial>(lPackage, *FPackageName::GetLongPackageAssetName(lPackage->GetName()),
			RF_Public | RF_Standalone);
		ON_SCOPE_EXIT { lPackage->SetDirtyFlag(false); };

		const TSharedPtr<SMaterialDiffEdit> lDiff = SMaterialDiffEdit::Open(lMaterial);
		ON_SCOPE_EXIT { CloseMaterialFixture(lDiff, lMaterial); };
		if (!TestTrue(TEXT("Material Diff hosted in the native material editor"), lDiff.IsValid()))
			return;
		const TSharedPtr<IMaterialEditor> lEditor = lDiff->m_Editor.Pin();
		const TSharedPtr<SDockTab> lTab = lDiff->m_HostTab.Pin();
		const TSharedPtr<SWidget> lNativeContent = lDiff->m_OriginalContent;

		lDiff->m_RevisionMaterial.Reset(lRevision.Get());
		lDiff->DoRefreshComparison();
		UMaterial * const lPreview = Cast<UMaterial>(lEditor->GetMaterialInterface());
		const TSharedPtr<SGraphEditor> lWorking = lDiff->m_LockedGraph.Pin();
		if (!TestTrue(TEXT("Working pane is the native preview graph"), lPreview && lPreview != lMaterial
			&& lWorking && lWorking->GetCurrentGraph() == lPreview->MaterialGraph.Get() && lWorking->GetGraphPanel()))
			return;
		TestTrue(TEXT("Historical pane is read-only"), lDiff->m_OldGraph && !lDiff->m_OldGraph->GetGraphPanel()->IsGraphEditable());
		TestTrue(TEXT("The native right panel remains editable"), lWorking->GetGraphPanel()->IsGraphEditable());

		UMaterialExpression * const lNode = lEditor->CreateNewMaterialExpression(UMaterialExpressionConstant::StaticClass(),
			FVector2D(200, 150), true, false, lPreview->MaterialGraph);
		TestNotNull(TEXT("Native editor created a working expression"), lNode);
		TestEqual(TEXT("Original asset unchanged before Apply"), lMaterial->GetExpressions().Num(), 0);
		TestEqual(TEXT("Revision remains unchanged"), lRevision->GetExpressions().Num(), 0);
		TestTrue(TEXT("Editing scheduled a comparison refresh"), lDiff->m_RefreshAt > 0);
		lDiff->m_RefreshAt = 1;
		lDiff->Tick(FGeometry(), 0.0, 0.0f);
		TestTrue(TEXT("Unapplied expression appears in the comparison"), lDiff->m_Changes.ContainsByPredicate([lNode](const auto & aChange)
			{ return lNode && aChange->Display.Diff == EDiffType::NODE_ADDED && aChange->NewNode.Get() == lNode->GraphNode.Get(); }));
		TestTrue(TEXT("The live graph displays the diff outlines"),
			BlueprintDiffGraphPanel::GetResults(*lWorking->GetGraphPanel()) == lDiff->m_WorkingHighlights);
		TestTrue(TEXT("The native Apply command succeeds"), lEditor->GetToolkitCommands()->TryExecuteAction(FMaterialEditorCommands::Get().Apply.ToSharedRef()));
		TestEqual(TEXT("Apply updates the actual material"), lMaterial->GetExpressions().Num(), 1);
		TestTrue(TEXT("The diff remains attached to the material after Apply"), lDiff->m_Source.Get() == lMaterial);

		lDiff->Close();
		TestTrue(TEXT("Close Diff restores the original editor widget"), lTab && lTab->GetContent() == lNativeContent.ToSharedRef());
		TestFalse(TEXT("Close Diff removes the outlines from the normal editor"),
			BlueprintDiffGraphPanel::GetResults(*lWorking->GetGraphPanel()).IsValid());
	});

	It("Refreshes overrides edited by the native Material Instance editor", [this]
	{
		const TStrongObjectPtr<UMaterialInstanceConstant> lRevision(NewObject<UMaterialInstanceConstant>());
		UPackage * const lPackage = CreatePackage(*FString::Printf(TEXT("/Game/MaterialInstanceDiff_%s"), *FGuid::NewGuid().ToString()));
		const TStrongObjectPtr<UMaterialInstanceConstant> lMaterial(NewObject<UMaterialInstanceConstant>(lPackage,
			*FPackageName::GetLongPackageAssetName(lPackage->GetName()), RF_Public | RF_Standalone));
		ON_SCOPE_EXIT { lPackage->SetDirtyFlag(false); };
		lMaterial->SetParentEditorOnly(UMaterial::GetDefaultMaterial(MD_Surface));
		lRevision->SetParentEditorOnly(lMaterial->Parent);

		const TSharedPtr<SMaterialDiffEdit> lDiff = SMaterialDiffEdit::Open(lMaterial.Get());
		ON_SCOPE_EXIT { CloseMaterialFixture(lDiff, lMaterial.Get()); };
		if (!TestTrue(TEXT("Diff hosted in the native Material Instance editor"), lDiff.IsValid()))
			return;
		lDiff->m_RevisionMaterial.Reset(lRevision.Get());
		lDiff->DoRefreshComparison();
		TestFalse(TEXT("Instances have no local graph"), lDiff->m_OldGraph.IsValid() || lDiff->m_LockedGraph.IsValid());

		const TSharedPtr<IMaterialEditor> lEditor = lDiff->m_Editor.Pin();
		const auto * lObjects = lEditor->GetObjectsCurrentlyBeingEdited();
		UObject * const * lProxy = lObjects->FindByPredicate([](const UObject * aObject) { return aObject->IsA<UMaterialEditorInstanceConstant>(); });
		if (!TestNotNull(TEXT("Native editor has an instance parameter proxy"), lProxy))
			return;

		UMaterialEditorInstanceConstant * const lSettings = CastChecked<UMaterialEditorInstanceConstant>(*lProxy);
		FProperty * const lProperty = FindFProperty<FProperty>(UMaterialEditorInstanceConstant::StaticClass(),
			GET_MEMBER_NAME_CHECKED(UMaterialEditorInstanceConstant, BasePropertyOverrides));
		lSettings->PreEditChange(lProperty);
		lSettings->BasePropertyOverrides.bOverride_OpacityMaskClipValue = true;
		lSettings->BasePropertyOverrides.OpacityMaskClipValue = 0.875f;
		FPropertyChangedEvent lEvent(lProperty);
		lSettings->PostEditChangeProperty(lEvent);
		TestEqual(TEXT("Native edit updates the working instance"), lMaterial->BasePropertyOverrides.OpacityMaskClipValue, 0.875f);
		TestFalse(TEXT("Revision has no override"), lRevision->BasePropertyOverrides.bOverride_OpacityMaskClipValue);
		TestTrue(TEXT("Instance edit scheduled a comparison refresh"), lDiff->m_RefreshAt > 0);
		lDiff->m_RefreshAt = 1;
		lDiff->Tick(FGeometry(), 0.0, 0.0f);
		TestTrue(TEXT("New instance override appears in the diff"), lDiff->m_Changes.ContainsByPredicate([](const auto & aChange)
			{ return aChange->Display.Diff == EDiffType::OBJECT_PROPERTY && aChange->Display.ToolTip.ToString().Contains(TEXT("0.875")); }));
	});
}

#endif
