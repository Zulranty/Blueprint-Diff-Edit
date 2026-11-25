#include "BlueprintDiffModel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BlueprintEditor.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/NamedSlot.h"
#include "Components/TextBlock.h"
#include "SBlueprintDiffEdit.h"
#include "BlueprintDiffGraphPanel.h"
#include "BlueprintEditorContext.h"
#include "BlueprintEditorModule.h"
#include "Containers/Ticker.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_Composite.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "DiffUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "Input/Events.h"
#include "Layout/ArrangedChildren.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SSplitter.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "WidgetReference.h"

/** Exercises the same graph and Blueprint settings filters as the diff list. */
BEGIN_DEFINE_SPEC(FBlueprintDiffModelTests, "BlueprintDiffEdit.Model",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	TStrongObjectPtr<UBlueprint> m_Revision;
	TStrongObjectPtr<UBlueprint> m_Working;
	UEdGraph * DoMakeFunction(UBlueprint * aBlueprint, FName aName);
	UEdGraph * DoAddNestedGraph(UEdGraph * aFunction);
END_DEFINE_SPEC(FBlueprintDiffModelTests)

static UBlueprint * CreateFixtureBlueprint()
{
	const FString lName = TEXT("/Temp/BlueprintDiffEditModel_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	return FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*lName),
		FName(TEXT("BP_DiffModel")), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("BlueprintDiffModel")));
}

static UWidgetBlueprint * CreateWidgetFixtureBlueprint(const FText & aText, UClass * aParent = UUserWidget::StaticClass())
{
	const FString lName = TEXT("/Temp/BlueprintDiffEditWidget_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UWidgetBlueprint * const lBlueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		aParent, CreatePackage(*lName), FName(TEXT("WBP_DiffFixture")), BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), FName(TEXT("BlueprintDiffWidget"))));
	UWidgetTree * const lTree = lBlueprint->WidgetTree;
	UCanvasPanel * const lCanvas = lTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	lTree->RootWidget = lCanvas;
	UTextBlock * const lText = lTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Caption"));
	lText->SetText(aText);
	lCanvas->AddChildToCanvas(lText);
	return lBlueprint;
}

UEdGraph * FBlueprintDiffModelTests::DoMakeFunction(UBlueprint * aBlueprint, FName aName)
{
	UEdGraph * const lGraph = FBlueprintEditorUtils::CreateNewGraph(aBlueprint, aName,
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(aBlueprint, lGraph, true, nullptr);
	return lGraph;
}

UEdGraph * FBlueprintDiffModelTests::DoAddNestedGraph(UEdGraph * aFunction)
{
	FGraphNodeCreator<UK2Node_Composite> lCreator(*aFunction);
	UK2Node_Composite * const lNode = lCreator.CreateNode();
	lCreator.Finalize();
	return lNode->BoundGraph;
}

void FBlueprintDiffModelTests::Define()
{
	It("Keeps only changed functions and groups nested differences with their function", [this]
	{
		if (!TestNotNull(TEXT("Revision Blueprint"), m_Revision.Get()) || !TestNotNull(TEXT("Working Blueprint"), m_Working.Get()))
			return;

		const FName UNCHANGED(TEXT("Unchanged"));
		const FName NESTED(TEXT("Nested"));
		DoMakeFunction(m_Revision.Get(), UNCHANGED);
		DoMakeFunction(m_Working.Get(), UNCHANGED);
		UEdGraph * const lOldNestedFunction = DoMakeFunction(m_Revision.Get(), NESTED);
		UEdGraph * const lNewNestedFunction = DoMakeFunction(m_Working.Get(), NESTED);
		UEdGraph * const lOldNestedBody = DoAddNestedGraph(lOldNestedFunction);
		UEdGraph * const lNewNestedBody = DoAddNestedGraph(lNewNestedFunction);
		lNewNestedBody->GraphGuid = lOldNestedBody->GraphGuid;
		FGraphNodeCreator<UK2Node_IfThenElse> lBranch(*lNewNestedBody);
		lBranch.CreateNode();
		lBranch.Finalize();

		UEdGraph * const lRemoved = DoMakeFunction(m_Revision.Get(), FName(TEXT("Removed")));
		UEdGraph * const lAdded = DoMakeFunction(m_Working.Get(), FName(TEXT("Added")));
		UEdGraph * const lOldName = DoMakeFunction(m_Revision.Get(), FName(TEXT("OldName")));
		UEdGraph * const lNewName = DoMakeFunction(m_Working.Get(), FName(TEXT("NewName")));
		lNewName->GraphGuid = lOldName->GraphGuid;

		const TArray<FBlueprintGraphChange> lChanges = BlueprintDiffModel::FindChangedGraphs(m_Revision.Get(), m_Working.Get());
		TestEqual(TEXT("Only changed function rows"), lChanges.Num(), 4);
		TestTrue(TEXT("Addition stays one-sided"), lChanges.ContainsByPredicate([lAdded](const FBlueprintGraphChange & aChange)
		{
			return aChange.Root.Working.Get() == lAdded && !aChange.Root.Revision.IsValid();
		}));
		TestTrue(TEXT("Removal stays one-sided"), lChanges.ContainsByPredicate([lRemoved](const FBlueprintGraphChange & aChange)
		{
			return aChange.Root.Revision.Get() == lRemoved && !aChange.Root.Working.IsValid();
		}));
		TestTrue(TEXT("A rename stays paired"), lChanges.ContainsByPredicate([lOldName, lNewName](const FBlueprintGraphChange & aChange)
		{
			return aChange.Root.Revision.Get() == lOldName && aChange.Root.Working.Get() == lNewName;
		}));
		const FBlueprintGraphChange * const lNested = lChanges.FindByPredicate([lNewNestedFunction](const FBlueprintGraphChange & aChange)
		{
			return aChange.Root.Working.Get() == lNewNestedFunction;
		});
		if (TestNotNull(TEXT("Function with changed nested graph"), lNested)
			&& TestEqual(TEXT("One paired nested change"), lNested->ChangedChildren.Num(), 1))
		{
			TestEqual(TEXT("Revision nested graph"), lNested->ChangedChildren[0].Revision.Get(), lOldNestedBody);
			TestEqual(TEXT("Working nested graph"), lNested->ChangedChildren[0].Working.Get(), lNewNestedBody);
		}
	});

	It("Includes changed Event Graphs and groups collapsed graphs under them", [this]
	{
		if (!TestEqual(TEXT("Revision Event Graph count"), m_Revision->UbergraphPages.Num(), 1)
			|| !TestEqual(TEXT("Working Event Graph count"), m_Working->UbergraphPages.Num(), 1))
		{
			return;
		}

		UEdGraph * const lOldEvent = m_Revision->UbergraphPages[0];
		UEdGraph * const lWorkingEvent = m_Working->UbergraphPages[0];
		UEdGraph * const lOldChild = DoAddNestedGraph(lOldEvent);
		UEdGraph * const lWorkingChild = DoAddNestedGraph(lWorkingEvent);
		lWorkingChild->GraphGuid = lOldChild->GraphGuid;
		lWorkingEvent->Nodes.Last()->NodeGuid = lOldEvent->Nodes.Last()->NodeGuid;

		TestEqual(TEXT("Unchanged Event Graph is absent"),
			BlueprintDiffModel::FindChangedGraphs(m_Revision.Get(), m_Working.Get()).Num(), 0);

		FGraphNodeCreator<UK2Node_IfThenElse> lNestedBranch(*lWorkingChild);
		lNestedBranch.CreateNode();
		lNestedBranch.Finalize();
		const auto lNestedChanges = BlueprintDiffModel::FindChangedGraphs(m_Revision.Get(), m_Working.Get());
		if (TestEqual(TEXT("A child change produces one Event Graph row"), lNestedChanges.Num(), 1))
		{
			TestTrue(TEXT("The same Event Graph is paired across revisions"),
				lNestedChanges[0].Root.Revision.Get() == lOldEvent && lNestedChanges[0].Root.Working.Get() == lWorkingEvent);
			if (TestEqual(TEXT("One changed collapsed graph"), lNestedChanges[0].ChangedChildren.Num(), 1))
				TestEqual(TEXT("Working collapsed graph"), lNestedChanges[0].ChangedChildren[0].Working.Get(), lWorkingChild);
		}

		FGraphNodeCreator<UK2Node_IfThenElse> lEventBranch(*lWorkingEvent);
		lEventBranch.CreateNode();
		lEventBranch.Finalize();
		const auto lRootChanges = BlueprintDiffModel::FindChangedGraphs(m_Revision.Get(), m_Working.Get());
		if (TestEqual(TEXT("Root and child changes share one Event Graph row"), lRootChanges.Num(), 1))
			TestTrue(TEXT("The Event Graph has a root difference"), lRootChanges[0].HasRootChanges);
	});

	It("Reports widget property, layout, addition and removal changes", [this]
	{
		const TStrongObjectPtr<UWidgetBlueprint> lRevision(CreateWidgetFixtureBlueprint(FText::FromString(TEXT("Before"))));
		const TStrongObjectPtr<UWidgetBlueprint> lWorking(CreateWidgetFixtureBlueprint(FText::FromString(TEXT("Before"))));
		ON_SCOPE_EXIT
		{
			lRevision->GetOutermost()->SetDirtyFlag(false);
			lWorking->GetOutermost()->SetDirtyFlag(false);
		};

		TestEqual(TEXT("Identical layouts have no changes"), BlueprintDiffModel::FindChangedWidgets(lRevision.Get(), lWorking.Get()).Num(), 0);
		UTextBlock * const lWorkingText = lWorking->WidgetTree->FindWidget<UTextBlock>(TEXT("Caption"));
		lWorkingText->SetText(FText::FromString(TEXT("After")));
		CastChecked<UCanvasPanelSlot>(lWorkingText->Slot)->SetPosition(FVector2D(82.0, 24.0));
		UCanvasPanel * const lWorkingRoot = CastChecked<UCanvasPanel>(lWorking->WidgetTree->RootWidget);
		UCanvasPanel * const lRevisionRoot = CastChecked<UCanvasPanel>(lRevision->WidgetTree->RootWidget);
		lWorkingRoot->AddChildToCanvas(lWorking->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NewLabel")));
		lRevisionRoot->AddChildToCanvas(lRevision->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("OldLabel")));

		const auto lChanges = BlueprintDiffModel::FindChangedWidgets(lRevision.Get(), lWorking.Get());
		TestTrue(TEXT("Text comparison includes both values"), lChanges.ContainsByPredicate([](const FBlueprintWidgetChange & aChange)
		{
			return aChange.WorkingWidget == TEXT("Caption") && aChange.Values.RevisionValue.ToString().Contains(TEXT("Before"))
				&& aChange.Values.WorkingValue.ToString().Contains(TEXT("After"));
		}));
		TestTrue(TEXT("Slot position change is reported"), lChanges.ContainsByPredicate([](const FBlueprintWidgetChange & aChange)
		{
			return aChange.WorkingWidget == TEXT("Caption") && aChange.Values.Label.ToString().Contains(TEXT("slot"))
				&& aChange.Values.WorkingValue.ToString().Contains(TEXT("82"));
		}));
		TestTrue(TEXT("Added widget selects the workspace widget"), lChanges.ContainsByPredicate([](const FBlueprintWidgetChange & aChange)
		{
			return aChange.WorkingWidget == TEXT("NewLabel") && aChange.Values.Label.ToString().Contains(TEXT("(added)"));
		}));
		TestTrue(TEXT("Removed widget has no workspace target"), lChanges.ContainsByPredicate([](const FBlueprintWidgetChange & aChange)
		{
			return aChange.WorkingWidget.IsNone() && aChange.Values.Label.ToString().Contains(TEXT("OldLabel (removed)"));
		}));
	});

	It("Shows keys when a map key changes without changing its value", [this]
	{
		const FName VARIABLE(TEXT("ReviewMap"));
		FEdGraphPinType lType;
		lType.PinCategory = UEdGraphSchema_K2::PC_Name;
		lType.ContainerType = EPinContainerType::Map;
		lType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
		for (UBlueprint * lBlueprint : { m_Revision.Get(), m_Working.Get() })
		{
			if (!TestTrue(TEXT("Map variable added"), FBlueprintEditorUtils::AddMemberVariable(lBlueprint, VARIABLE, lType)))
				return;
			FKismetEditorUtilities::CompileBlueprint(lBlueprint);
			const FMapProperty * const lMap = FindFProperty<FMapProperty>(lBlueprint->GeneratedClass, VARIABLE);
			if (!TestNotNull(TEXT("Generated map property"), lMap))
				return;
			FScriptMapHelper lHelper(lMap, lMap->ContainerPtrToValuePtr<void>(lBlueprint->GeneratedClass->GetDefaultObject()));
			const FName lKey = lBlueprint == m_Revision.Get() ? TEXT("OldKey") : TEXT("NewKey");
			const int32 lValue = 7;
			lHelper.AddPair(&lKey, &lValue);
		}
		const auto lChanges = BlueprintDiffModel::FindChangedSettings(m_Revision.Get(), m_Working.Get());
		TestTrue(TEXT("The class-default row shows both changed keys"), lChanges.ContainsByPredicate([](const auto & aChange)
		{
			return aChange.Label.ToString().Contains(TEXT("Class Defaults"))
				&& aChange.RevisionValue.ToString().Contains(TEXT("OldKey"))
				&& aChange.WorkingValue.ToString().Contains(TEXT("NewKey"));
		}));
	});

	It("Reports class defaults and member variables without adding graph rows", [this]
	{
		if (!TestNotNull(TEXT("Revision Blueprint"), m_Revision.Get()) || !TestNotNull(TEXT("Working Blueprint"), m_Working.Get()))
			return;
		const TArray<FBlueprintSettingChange> lIdentical = BlueprintDiffModel::FindChangedSettings(m_Revision.Get(), m_Working.Get());
		TestEqual(TEXT("Identical settings are omitted"), lIdentical.Num(), 0);

		FEdGraphPinType lType;
		lType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		const FName ADDED_VARIABLE(TEXT("ReviewFlag"));
		TestTrue(TEXT("Fixture variable added"), FBlueprintEditorUtils::AddMemberVariable(m_Working.Get(), ADDED_VARIABLE, lType));
		AActor * const lDefaults = m_Working->GeneratedClass->GetDefaultObject<AActor>();
		constexpr float CHANGED_LIFESPAN = 8.0f;
		const float lOldLifespan = m_Revision->GeneratedClass->GetDefaultObject<AActor>()->InitialLifeSpan;
		lDefaults->InitialLifeSpan = CHANGED_LIFESPAN;

		const TArray<FBlueprintSettingChange> lSettings = BlueprintDiffModel::FindChangedSettings(m_Revision.Get(), m_Working.Get());
		TestTrue(TEXT("An added variable is in the settings diff"), lSettings.ContainsByPredicate([](const FBlueprintSettingChange & aChange)
		{
			return aChange.Label.ToString().Contains(TEXT("ReviewFlag"));
		}));
		if (!TestTrue(TEXT("Class default has old and working values"), lSettings.ContainsByPredicate(
			[lOldLifespan](const FBlueprintSettingChange & aChange)
			{
				return aChange.Label.ToString().Contains(TEXT("InitialLifeSpan"))
					&& aChange.RevisionValue.ToString().IsNumeric() && FCString::Atof(*aChange.RevisionValue.ToString()) == lOldLifespan
					&& FCString::Atof(*aChange.WorkingValue.ToString()) == CHANGED_LIFESPAN;
			})))
		{
			for (const FBlueprintSettingChange & lSetting : lSettings)
				AddInfo(FString::Printf(TEXT("%s: '%s' to '%s'"), *lSetting.Label.ToString(),
					*lSetting.RevisionValue.ToString(), *lSetting.WorkingValue.ToString()));
		}
		TestEqual(TEXT("No graph differences"), BlueprintDiffModel::FindChangedGraphs(m_Revision.Get(), m_Working.Get()).Num(), 0);
	});

	BeforeEach([this]
	{
		m_Revision.Reset(CreateFixtureBlueprint());
		m_Working.Reset(CreateFixtureBlueprint());
	});

	AfterEach([this]
	{
		if (m_Revision.IsValid())
			m_Revision->GetOutermost()->SetDirtyFlag(false);
		if (m_Working.IsValid())
			m_Working->GetOutermost()->SetDirtyFlag(false);
		m_Revision.Reset();
		m_Working.Reset();
	});
}

/** Checks the generated toolbar in a native Blueprint editor window. */
BEGIN_DEFINE_SPEC(FBlueprintDiffToolbarTests, "BlueprintDiffEdit.Toolbar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
	void DoCheckDivider(SSplitter & aSplitter, const FVector2f & aSpace, bool aHorizontal);
END_DEFINE_SPEC(FBlueprintDiffToolbarTests)

void FBlueprintDiffToolbarTests::DoCheckDivider(SSplitter & aSplitter, const FVector2f & aSpace, bool aHorizontal)
{
	const FGeometry lGeometry = FGeometry::MakeRoot(aSpace, FSlateLayoutTransform());
	FArrangedChildren lBefore(EVisibility::All);
	aSplitter.OnArrangeChildren(lGeometry, lBefore);
	if (!TestEqual(TEXT("The divider separates two panes"), lBefore.Num(), 2))
		return;

	const FVector2f lFirstSize = lBefore[0].Geometry.GetLocalSize();
	const FVector2f lSecondSize = lBefore[1].Geometry.GetLocalSize();
	const FVector2f lFirstOrigin = lBefore[0].Geometry.GetAbsolutePosition();
	const FVector2f lSecondOrigin = lBefore[1].Geometry.GetAbsolutePosition();
	const FVector2f lHandle = aHorizontal
		? FVector2f((lFirstOrigin.X + lFirstSize.X + lSecondOrigin.X) * 0.5f, aSpace.Y * 0.5f)
		: FVector2f(aSpace.X * 0.5f, (lFirstOrigin.Y + lFirstSize.Y + lSecondOrigin.Y) * 0.5f);
	const FVector2f lDestination = lHandle + (aHorizontal ? FVector2f(64.0f, 0.0f) : FVector2f(0.0f, 64.0f));
	const TSet<FKey> lPressed{ EKeys::LeftMouseButton };
	const FPointerEvent lDown(0, lHandle, lHandle, lPressed, EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
	const FPointerEvent lDrag(0, lDestination, lHandle, lPressed, EKeys::Invalid, 0.0f, FModifierKeysState());
	const FPointerEvent lUp(0, lDestination, lDestination, lPressed, EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
	aSplitter.OnMouseMove(lGeometry, lDown);
	if (!TestTrue(TEXT("Mouse down on the divider captures a resize"), aSplitter.OnMouseButtonDown(lGeometry, lDown).IsEventHandled()))
		return;
	TestTrue(TEXT("The divider accepts the drag"), aSplitter.OnMouseMove(lGeometry, lDrag).IsEventHandled());
	TestTrue(TEXT("Releasing the divider ends the resize"), aSplitter.OnMouseButtonUp(lGeometry, lUp).IsEventHandled());

	FArrangedChildren lAfter(EVisibility::All);
	aSplitter.OnArrangeChildren(lGeometry, lAfter);
	if (TestEqual(TEXT("Both panes remain after resizing"), lAfter.Num(), 2))
	{
		const int lAxis = aHorizontal ? 0 : 1;
		TestTrue(TEXT("The first pane grows along the selected axis"), lAfter[0].Geometry.GetLocalSize()[lAxis] > lFirstSize[lAxis]);
		TestTrue(TEXT("The other pane shrinks along the selected axis"), lAfter[1].Geometry.GetLocalSize()[lAxis] < lSecondSize[lAxis]);
	}
}

void FBlueprintDiffToolbarTests::Define()
{
	It("Compares a Widget Blueprint beside its native Designer", [this]
	{
		const TStrongObjectPtr<UWidgetBlueprint> lRevision(CreateWidgetFixtureBlueprint(FText::FromString(TEXT("Before"))));
		const TStrongObjectPtr<UWidgetBlueprint> lWorking(CreateWidgetFixtureBlueprint(FText::FromString(TEXT("After"))));
		ON_SCOPE_EXIT
		{
			lRevision->GetOutermost()->SetDirtyFlag(false);
			lWorking->GetOutermost()->SetDirtyFlag(false);
		};
		FKismetEditorUtilities::CompileBlueprint(lWorking.Get());
		const TSharedRef<FWidgetBlueprintEditor> lEditor = MakeShared<FWidgetBlueprintEditor>();
		lEditor->InitWidgetBlueprintEditor(EToolkitMode::Standalone, nullptr, { lWorking.Get() }, false);
		ON_SCOPE_EXIT { lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); };

		UBlueprintEditorToolMenuContext * const lContext = NewObject<UBlueprintEditorToolMenuContext>();
		lContext->BlueprintEditor = lEditor;
		FToolMenuContext lMenuContext;
		lMenuContext.AddObject(lContext);
		for (const FName lMode : { FName(TEXT("DesignerName")), FName(TEXT("GraphName")) })
		{
			FName lParent;
			const UToolMenu * const lMenu = UToolMenus::Get()->GenerateMenu(lEditor->GetToolMenuToolbarNameForMode(lMode, lParent), lMenuContext);
			TestTrue(TEXT("Widget editor mode has a Diff & Edit toolbar button"), lMenu && lMenu->Sections.ContainsByPredicate(
				[](const FToolMenuSection & aSection) { return aSection.FindEntry("BlueprintDiffEditToolbarButton") != nullptr; }));
		}

		const TSharedPtr<SDockTab> lHost = lEditor->GetTabManager()->GetOwnerTab();
		if (!TestNotNull(TEXT("Native Widget Blueprint editor host"), lHost.Get()))
			return;
		const TSharedRef<SBlueprintDiffEdit> lDiff = SNew(SBlueprintDiffEdit, lEditor, lHost.ToSharedRef(), lWorking.Get());
		lHost->SetContent(lDiff);
		ON_SCOPE_EXIT { lDiff->Close(); };
		lDiff->m_RevisionBlueprint.Reset(lRevision.Get());
		lDiff->DoRebuildGraphList();
		if (!TestTrue(TEXT("Widgets section selected"), lDiff->m_SelectedGraph
			&& lDiff->m_SelectedGraph->Section == EBlueprintDiffSection::Widgets)
			|| !TestTrue(TEXT("Historical preview has a separate widget tree"),
				lDiff->m_RevisionWidgetTree.IsValid() && lDiff->m_RevisionWidgetTree.Get() != lRevision->WidgetTree))
		{
			return;
		}
		TestTrue(TEXT("The old widget root is rendered in Slate"), lDiff->m_RevisionWidgetPreview.IsValid()
			&& lDiff->m_RevisionWidgetTree->RootWidget->GetCachedWidget().IsValid());
		TestEqual(TEXT("The right-hand editor is in Designer"), lEditor->GetCurrentMode(), FName(TEXT("DesignerName")));

		UTextBlock * const lText = lWorking->WidgetTree->FindWidget<UTextBlock>(TEXT("Caption"));
		TestTrue(TEXT("Selecting the widget diff selects the working widget"), lEditor->GetSelectedWidgets().Contains(
			lEditor->GetReferenceFromTemplate(lText)));

		lText->SetText(FText::FromString(TEXT("Before")));
		lDiff->DoRebuildGraphList();
		TestTrue(TEXT("Changing the working widget removes the resolved differences"), lDiff->m_Widgets.IsEmpty());
	});

	It("Renders inherited Widget Blueprint slots with historical content", [this]
	{
		const TStrongObjectPtr<UWidgetBlueprint> lParent(CreateWidgetFixtureBlueprint(FText::FromString(TEXT("Parent"))));
		const FName SLOT_NAME(TEXT("InheritedSlot"));
		UNamedSlot * const lSlot = lParent->WidgetTree->ConstructWidget<UNamedSlot>(UNamedSlot::StaticClass(), SLOT_NAME);
		CastChecked<UCanvasPanel>(lParent->WidgetTree->RootWidget)->AddChildToCanvas(lSlot);
		FKismetEditorUtilities::CompileBlueprint(lParent.Get());

		const FText OLD_TEXT = FText::FromString(TEXT("Historical slot content"));
		const TStrongObjectPtr<UWidgetBlueprint> lRevision(CreateWidgetFixtureBlueprint(OLD_TEXT, lParent->GeneratedClass));
		const TStrongObjectPtr<UWidgetBlueprint> lWorking(CreateWidgetFixtureBlueprint(
			FText::FromString(TEXT("Working slot content")), lParent->GeneratedClass));
		ON_SCOPE_EXIT
		{
			for (UWidgetBlueprint * lBlueprint : { lParent.Get(), lRevision.Get(), lWorking.Get() })
				lBlueprint->GetOutermost()->SetDirtyFlag(false);
		};
		for (UWidgetBlueprint * lBlueprint : { lRevision.Get(), lWorking.Get() })
		{
			UTextBlock * const lCaption = lBlueprint->WidgetTree->FindWidget<UTextBlock>(TEXT("Caption"));
			lCaption->RemoveFromParent();
			lCaption->Rename(TEXT("SlotCaption"), lBlueprint->WidgetTree);
			lBlueprint->WidgetTree->RootWidget = nullptr;
			lBlueprint->WidgetTree->SetContentForSlot(SLOT_NAME, lCaption);
			FKismetEditorUtilities::CompileBlueprint(lBlueprint);
		}
		const TSharedRef<FWidgetBlueprintEditor> lEditor = MakeShared<FWidgetBlueprintEditor>();
		lEditor->InitWidgetBlueprintEditor(EToolkitMode::Standalone, nullptr, { lWorking.Get() }, false);
		ON_SCOPE_EXIT { lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); };
		const TSharedPtr<SDockTab> lHost = lEditor->GetTabManager()->GetOwnerTab();
		if (!TestNotNull(TEXT("Native Widget Blueprint editor host"), lHost.Get()))
			return;

		const TSharedRef<SBlueprintDiffEdit> lDiff = SNew(SBlueprintDiffEdit, lEditor, lHost.ToSharedRef(), lWorking.Get());
		lHost->SetContent(lDiff);
		ON_SCOPE_EXIT { lDiff->Close(); };
		lDiff->m_RevisionBlueprint.Reset(lRevision.Get());
		lDiff->DoRebuildGraphList();
		if (!TestTrue(TEXT("Historical Designer renders a copied inherited layout"), lDiff->m_RevisionWidgetTree.IsValid()
			&& lDiff->m_RevisionWidgetPreview.IsValid() && lDiff->m_RevisionWidgetTree->RootWidget != lParent->WidgetTree->RootWidget))
			return;

		const UNamedSlot * const lPreviewSlot = lDiff->m_RevisionWidgetTree->FindWidget<UNamedSlot>(SLOT_NAME);
		const UTextBlock * const lPreviewText = lPreviewSlot ? Cast<UTextBlock>(lPreviewSlot->GetContent()) : nullptr;
		if (TestNotNull(TEXT("Historical override is inside the inherited slot"), lPreviewText))
			TestTrue(TEXT("Preview uses the historical override value"), lPreviewText->GetText().EqualTo(OLD_TEXT));
		TestNull(TEXT("Preview did not alter the parent template"), lSlot->GetContent());
	});

	It("Loads a serialized Git revision before full history and saves a reverted function", [this]
	{
		const FString lFixture = TEXT("BlueprintDiffEditAutomation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString lRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / lFixture);
		IFileManager::Get().MakeDirectory(*lRoot, true);
		ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*lRoot, false, true); };

		const FString lPackageName = TEXT("/Game/") + lFixture + TEXT("/BP_RealRevision");
		const FString lFile = lRoot / TEXT("BP_RealRevision.uasset");
		UPackage * const lPackage = CreatePackage(*lPackageName);
		const TStrongObjectPtr<UBlueprint> lWorking(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), lPackage,
			TEXT("BP_RealRevision"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None));
		if (!TestNotNull(TEXT("Serializable Blueprint"), lWorking.Get()))
			return;
		ON_SCOPE_EXIT { lPackage->SetDirtyFlag(false); };

		const FName FUNCTION_NAME(TEXT("RevertMe"));
		UEdGraph * const lGraph = FBlueprintEditorUtils::CreateNewGraph(lWorking.Get(), FUNCTION_NAME,
			UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(lWorking.Get(), lGraph, true, nullptr);
		FGraphNodeCreator<UK2Node_IfThenElse> lCreator(*lGraph);
		UK2Node_IfThenElse * const lOldBranch = lCreator.CreateNode();
		lCreator.Finalize();
		lOldBranch->NodePosX = 1234;
		FKismetEditorUtilities::CompileBlueprint(lWorking.Get());
		FSavePackageArgs lSaveArgs;
		lSaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!TestTrue(TEXT("Historical Blueprint package written"), UPackage::SavePackage(lPackage, lWorking.Get(), *lFile, lSaveArgs)))
			return;

		const auto lGit = [this, &lRoot](const TCHAR * aArgs)
		{
			const FString lArgs = FString::Printf(TEXT("-C \"%s\" -c core.hooksPath=.git/no-hooks -c user.name=Automation -c user.email=diff@example.invalid -c commit.gpgsign=false %s"), *lRoot, aArgs);
			int32 lExitCode = INDEX_NONE;
			FString lOut, lErr;
			const bool lOk = FPlatformProcess::ExecProcess(*BlueprintDiffGit::GetExecutable(), *lArgs, &lExitCode, &lOut, &lErr)
				&& lExitCode == 0;
			if (!lOk)
				AddError(lErr);
			return lOk;
		};
		if (!lGit(TEXT("init --initial-branch=master")) || !lGit(TEXT("add -- BP_RealRevision.uasset"))
			|| !lGit(TEXT("commit -m Before")))
			return;

		lGraph->RemoveNode(lOldBranch);
		FKismetEditorUtilities::CompileBlueprint(lWorking.Get());
		FBlueprintEditorModule & lModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");
		const TSharedRef<IBlueprintEditor> lEditor = lModule.CreateBlueprintEditor(EToolkitMode::Standalone, nullptr, lWorking.Get(), false);
		ON_SCOPE_EXIT { lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); };
		const TSharedPtr<SDockTab> lHost = lEditor->GetTabManager()->GetOwnerTab();
		if (!TestNotNull(TEXT("Native Blueprint toolkit host"), lHost.Get()))
			return;

		const TSharedRef<SBlueprintDiffEdit> lDiff = SNew(SBlueprintDiffEdit, lEditor, lHost.ToSharedRef(), lWorking.Get());
		lHost->SetContent(lDiff);
		ON_SCOPE_EXIT { lDiff->Close(); };
		TArray<FString> lBefore, lAfter;
		const FString lSnapshots = FPaths::DiffDir() / TEXT("temp-blueprint-diff-edit-revision-*.uasset");
		IFileManager::Get().FindFiles(lBefore, *lSnapshots, true, false);
		lDiff->DoStartGitHistory();
		if (!TestTrue(TEXT("Latest history entry is published by the background worker"),
			lDiff->m_LatestGitRevision.WaitFor(FTimespan::FromSeconds(12))))
			return;
		auto lFullHistory = MoveTemp(lDiff->m_GitHistoryRequest);
		lDiff->DoProcessGitHistory();
		if (!TestTrue(TEXT("The snapshot request starts while full history still awaits delivery"),
			lDiff->m_RevisionRequest.IsValid() && lDiff->m_RevisionRequest.WaitFor(FTimespan::FromSeconds(12))))
			return;
		lDiff->DoFinishRevision();
		if (!TestTrue(TEXT("An isolated serialized revision was applied"), lDiff->m_RevisionBlueprint.IsValid()
			&& lDiff->m_RevisionBlueprint.Get() != lWorking.Get() && lDiff->m_Commit.Subject == TEXT("Before")))
		{
			AddInfo(lDiff->m_Status.ToString());
			return;
		}
		IFileManager::Get().FindFiles(lAfter, *lSnapshots, true, false);
		TestEqual(TEXT("Loaded snapshots are removed after detaching the linker"), lAfter.Num(), lBefore.Num());
		const TWeakObjectPtr<UBlueprint> lActiveRevision(lDiff->m_RevisionBlueprint.Get());
		lDiff->m_GitHistoryRequest = MoveTemp(lFullHistory);
		if (!TestTrue(TEXT("Full history completes separately"), lDiff->m_GitHistoryRequest.WaitFor(FTimespan::FromSeconds(12))))
			return;
		lDiff->DoProcessGitHistory();
		TestTrue(TEXT("History publication retains the already loaded revision"),
			lActiveRevision.Get() == lDiff->m_RevisionBlueprint.Get() && !lDiff->m_RevisionRequest.IsValid());
		if (!TestEqual(TEXT("Full history contains the committed file"), lDiff->m_Revisions.Num(), 1))
			return;
		lDiff->DoLoadRevision(lDiff->m_Revisions[0]);
		TestFalse(TEXT("Selecting the same revision from the full list skips package loading"), lDiff->m_RevisionRequest.IsValid());
		const auto * lRow = lDiff->m_Graphs.FindByPredicate([lGraph](const auto & aRow) { return aRow->Root.Working.Get() == lGraph; });
		if (!TestNotNull(TEXT("Changed function row"), lRow))
			return;

		lDiff->m_GraphList->SetSelection(*lRow);
		TestTrue(TEXT("The function has a native context action"), lDiff->DoBuildGraphMenu().IsValid());
		lDiff->DoRevertFunction(*lRow, TWeakObjectPtr<UBlueprint>(lDiff->m_RevisionBlueprint.Get()));
		FKismetEditorUtilities::CompileBlueprint(lWorking.Get());
		TestTrue(TEXT("Reverted Blueprint compiles"), lWorking->Status != BS_Error);
		if (!TestTrue(TEXT("Reverted package can be saved"), UPackage::SavePackage(lPackage, lWorking.Get(), *lFile, lSaveArgs)))
			return;

		const FString lCopy = FPaths::CreateTempFilename(*FPaths::DiffDir(), TEXT("BlueprintDiffEditVerifySave-"), TEXT(".uasset"));
		if (!TestEqual(TEXT("Saved asset copied for independent reload"), IFileManager::Get().Copy(*lCopy, *lFile), static_cast<uint32>(COPY_OK)))
			return;
		ON_SCOPE_EXIT { IFileManager::Get().Delete(*lCopy, false, true, true); };
		UPackage * const lReloaded = DiffUtils::LoadPackageForDiff(FPackagePath::FromLocalPath(lCopy), FPackagePath::FromLocalPath(lFile));
		ON_SCOPE_EXIT { if (lReloaded) ResetLoaders(lReloaded); };
		UBlueprint * const lSaved = lReloaded ? FindObject<UBlueprint>(lReloaded, TEXT("BP_RealRevision")) : nullptr;
		const UEdGraph * const lSavedGraph = lSaved ? FindObject<UEdGraph>(lSaved, *FUNCTION_NAME.ToString()) : nullptr;
		TArray<UK2Node_IfThenElse *> lBranches;
		if (TestNotNull(TEXT("Saved function reopened from disk"), lSavedGraph))
			lSavedGraph->GetNodesOfClass(lBranches);
		if (TestEqual(TEXT("Restored node persists on disk"), lBranches.Num(), 1))
			TestEqual(TEXT("Restored node position persists"), lBranches[0]->NodePosX, 1234);
	});

	It("Formats only the working graph in the real Diff panel", [this]
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("GraphFormatter")))
		{
			AddInfo(TEXT("GraphFormatter is disabled. The joint command requires both plugins."));
			return;
		}
		const TStrongObjectPtr<UBlueprint> lRevision(CreateFixtureBlueprint());
		const TStrongObjectPtr<UBlueprint> lWorking(CreateFixtureBlueprint());
		ON_SCOPE_EXIT
		{
			lWorking->GetOutermost()->SetDirtyFlag(false);
			lRevision->GetOutermost()->SetDirtyFlag(false);
		};
		const auto lAddChain = [](UEdGraph & aGraph, int32 aY)
		{
			FGraphNodeCreator<UK2Node_IfThenElse> lFirstCreator(aGraph);
			UK2Node_IfThenElse * const lFirst = lFirstCreator.CreateNode();
			lFirstCreator.Finalize();
			FGraphNodeCreator<UK2Node_IfThenElse> lSecondCreator(aGraph);
			UK2Node_IfThenElse * const lSecond = lSecondCreator.CreateNode();
			lSecondCreator.Finalize();
			lSecond->NodePosX = 2400;
			lSecond->NodePosY = aY;
			lFirst->FindPinChecked(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(lSecond->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
			return lSecond;
		};
		UEdGraph * const lWorkingGraph = lWorking->UbergraphPages[0];
		UK2Node_IfThenElse * const lWorkingTarget = lAddChain(*lWorkingGraph, 1700);
		UK2Node_IfThenElse * const lRevisionTarget = lAddChain(*lRevision->UbergraphPages[0], 1900);

		FBlueprintEditorModule & lModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");
		const TSharedRef<IBlueprintEditor> lEditor = lModule.CreateBlueprintEditor(EToolkitMode::Standalone, nullptr, lWorking.Get(), false);
		ON_SCOPE_EXIT { lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); };
		const TSharedPtr<SDockTab> lHost = lEditor->GetTabManager()->GetOwnerTab();
		const TSharedPtr<SGraphEditor> lGraph = lEditor->OpenGraphAndBringToFront(lWorkingGraph, false);
		const auto lCommand = FInputBindingManager::Get().FindCommandInContext(TEXT("GraphFormatterEditor"), TEXT("FormatGraph"));
		if (!TestTrue(TEXT("The native editor, graph and formatter command are available"), lHost && lGraph && lCommand))
			return;

		const TSharedRef<SBlueprintDiffEdit> lDiff = SNew(SBlueprintDiffEdit, lEditor, lHost.ToSharedRef(), lWorking.Get());
		lHost->SetContent(lDiff);
		ON_SCOPE_EXIT { lDiff->Close(); };
		lDiff->m_RevisionBlueprint.Reset(lRevision.Get());
		lDiff->DoRebuildGraphList();
		if (!TestTrue(TEXT("The real panel shows the working graph and historical comparison"), lDiff->m_RevisionGraphEditor.IsValid()
			&& lEditor->GetFocusedGraph() == lWorkingGraph))
			return;
		lGraph->ClearSelectionSet();
		lGraph->GetGraphPanel()->Update();
		lHost->SlatePrepass(1.0f);
		TestTrue(TEXT("Working toolkit executes its Format Graph action"), lEditor->GetToolkitCommands()->TryExecuteAction(lCommand.ToSharedRef()));
		TestTrue(TEXT("Format Graph changed the live graph"), lWorkingTarget->NodePosY != 1700 || lWorkingTarget->NodePosX != 2400);
		TestEqual(TEXT("Historical graph Y remains unchanged"), lRevisionTarget->NodePosY, 1900);
		TestEqual(TEXT("Historical graph X remains unchanged"), lRevisionTarget->NodePosX, 2400);
	});

	It("Registers Diff and Edit after Debugging in a Blueprint editor toolbar", [this]
	{
		const TStrongObjectPtr<UBlueprint> lBlueprint(CreateFixtureBlueprint());
		if (!TestNotNull(TEXT("Working Blueprint"), lBlueprint.Get()))
			return;

		ON_SCOPE_EXIT { lBlueprint->GetOutermost()->SetDirtyFlag(false); };
		FBlueprintEditorModule & lModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");
		const TArray<UBlueprint *> lBlueprints{ lBlueprint.Get() };
		const TSharedRef<IBlueprintEditor> lEditor = lModule.CreateBlueprintEditor(EToolkitMode::Standalone, nullptr, lBlueprints, false);
		ON_SCOPE_EXIT { lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); };

		UBlueprintEditorToolMenuContext * const lBlueprintContext = NewObject<UBlueprintEditorToolMenuContext>();
		lBlueprintContext->BlueprintEditor = StaticCastSharedRef<FBlueprintEditor>(lEditor);
		FToolMenuContext lMenuContext;
		lMenuContext.AddObject(lBlueprintContext);
		FName lParentMenu;
		const UToolMenu * const lToolbar = UToolMenus::Get()->GenerateMenu(lEditor->GetToolMenuToolbarName(lParentMenu), lMenuContext);
		if (!TestNotNull(TEXT("Generated Blueprint toolbar"), lToolbar))
			return;

		const int lButtonSection = lToolbar->Sections.IndexOfByPredicate([](const FToolMenuSection & aSection)
		{
			return aSection.FindEntry("BlueprintDiffEditToolbarButton") != nullptr;
		});
		if (!TestTrue(TEXT("Diff and Edit toolbar button"), lButtonSection != INDEX_NONE))
			return;

		const int lDebuggingIndex = lToolbar->Sections.IndexOfByPredicate([](const FToolMenuSection & aSection)
		{
			return aSection.Name == FName(TEXT("Debugging"));
		});
		TestTrue(TEXT("Diff and Edit follows Debugging"), lDebuggingIndex != INDEX_NONE && lButtonSection > lDebuggingIndex);
	});

	It("Applies native diff outlines to the editable graph and clears them when the graph changes or Diff closes", [this]
	{
		const TStrongObjectPtr<UBlueprint> lRevision(CreateFixtureBlueprint());
		const TStrongObjectPtr<UBlueprint> lWorking(CreateFixtureBlueprint());
		ON_SCOPE_EXIT
		{
			lRevision->GetOutermost()->SetDirtyFlag(false);
			lWorking->GetOutermost()->SetDirtyFlag(false);
		};

		const auto lAddFunction = [](UBlueprint * aBlueprint, FName aName)
		{
			UEdGraph * const lGraph = FBlueprintEditorUtils::CreateNewGraph(aBlueprint, aName,
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(aBlueprint, lGraph, true, nullptr);
			return lGraph;
		};
		const auto lAddBranch = [](UEdGraph * aGraph)
		{
			FGraphNodeCreator<UK2Node_IfThenElse> lCreator(*aGraph);
			UK2Node_IfThenElse * const lNode = lCreator.CreateNode();
			lCreator.Finalize();
			return lNode;
		};

		UEdGraph * const lOldGraph = lAddFunction(lRevision.Get(), TEXT("ReviewFunction"));
		UEdGraph * const lWorkingGraph = lAddFunction(lWorking.Get(), TEXT("ReviewFunction"));
		UEdGraph * const lAddedGraph = lAddFunction(lWorking.Get(), TEXT("NewFunction"));
		UK2Node_IfThenElse * const lOld = lAddBranch(lOldGraph);
		UK2Node_IfThenElse * const lMoved = lAddBranch(lWorkingGraph);
		lMoved->NodeGuid = lOld->NodeGuid;
		lMoved->NodePosX = lOld->NodePosX + 320;
		UK2Node_IfThenElse * const lAdded = lAddBranch(lWorkingGraph);
		UK2Node_IfThenElse * const lNewFunctionNode = lAddBranch(lAddedGraph);

		FBlueprintEditorModule & lModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");
		const TSharedRef<IBlueprintEditor> lEditor = lModule.CreateBlueprintEditor(EToolkitMode::Standalone, nullptr, lWorking.Get(), false);
		ON_SCOPE_EXIT { lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); };
		const TSharedPtr<SDockTab> lHost = lEditor->GetTabManager()->GetOwnerTab();
		const TSharedPtr<SGraphEditor> lWorkingEditor = lEditor->OpenGraphAndBringToFront(lWorkingGraph, false);
		if (!TestNotNull(TEXT("Native Blueprint editor host"), lHost.Get())
			|| !TestTrue(TEXT("Native graph panel exists"), lWorkingEditor && lWorkingEditor->GetGraphPanel()))
		{
			return;
		}

		SGraphPanel & lWorkingPanel = *lWorkingEditor->GetGraphPanel();
		auto & lWorkingResults = BlueprintDiffGraphPanel::GetResults(lWorkingPanel);
		TestFalse(TEXT("A regular graph opens without diff results"), lWorkingResults.IsValid());
		const TSharedRef<SBlueprintDiffEdit> lDiff = SNew(SBlueprintDiffEdit, lEditor, lHost.ToSharedRef(), lWorking.Get());
		lHost->SetContent(lDiff);
		ON_SCOPE_EXIT { lDiff->Close(); };

		lDiff->m_RevisionBlueprint.Reset(lRevision.Get());
		lDiff->DoRebuildGraphList();
		if (!TestTrue(TEXT("Diff results are installed on the original editable panel"), lWorkingResults.IsValid()))
			return;

		const auto lIsAdded = [](const FDiffSingleResult & aResult, const UEdGraphNode * aNode)
		{
			return aResult.Node1 == aNode && aResult.Diff == EDiffType::NODE_ADDED && aResult.Category == EDiffType::ADDITION;
		};
		TestTrue(TEXT("Moved node has the native minor diff color and references the old node"), lWorkingResults->ContainsByPredicate(
			[lMoved, lOld](const FDiffSingleResult & aResult)
			{
				return aResult.Node1 == lMoved && aResult.Node2 == lOld && aResult.Diff == EDiffType::NODE_MOVED
					&& aResult.Category == EDiffType::MINOR;
			}));
		TestTrue(TEXT("Added node has the native addition color"), lWorkingResults->ContainsByPredicate(
			[lAdded, &lIsAdded](const FDiffSingleResult & aResult) { return lIsAdded(aResult, lAdded); }));

		lMoved->NodePosX = lOld->NodePosX;
		lWorkingGraph->NotifyGraphChanged();
		TestTrue(TEXT("Editing the live graph schedules a refresh"), lDiff->m_RefreshAt > 0.0);
		lDiff->m_RefreshAt = FPlatformTime::Seconds();
		lDiff->Tick(FGeometry(), 0.0, 0.0f);
		TestTrue(TEXT("Reverting a node's position clears its outline"), lWorkingResults && !lWorkingResults->ContainsByPredicate(
			[lMoved](const FDiffSingleResult & aResult) { return aResult.Node1 == lMoved; }));

		const SBlueprintDiffEdit::FGraphPtr * const lAddedRow = lDiff->m_Graphs.FindByPredicate(
			[lAddedGraph](const SBlueprintDiffEdit::FGraphPtr & aRow) { return aRow->Root.Working.Get() == lAddedGraph; });
		if (!TestNotNull(TEXT("Added function row"), lAddedRow))
			return;
		lDiff->m_GraphList->SetSelection(*lAddedRow, ESelectInfo::OnMouseClick);
		const TSharedPtr<SGraphEditor> lAddedEditor = lDiff->m_WorkingGraphEditor.Pin();
		TestFalse(TEXT("Switching functions clears the old working panel"), lWorkingResults.IsValid());
		if (!TestTrue(TEXT("The added function uses a native graph panel"), lAddedEditor && lAddedEditor->GetGraphPanel()))
			return;
		const auto & lAddedResults = BlueprintDiffGraphPanel::GetResults(*lAddedEditor->GetGraphPanel());
		TestTrue(TEXT("Nodes of a wholly added function get the native addition color"), lAddedResults
			&& lAddedResults->ContainsByPredicate([lNewFunctionNode, &lIsAdded](const FDiffSingleResult & aResult)
			{
				return lIsAdded(aResult, lNewFunctionNode);
			}));

		lDiff->Close();
		TestFalse(TEXT("Close Diff clears outlines from the editable graph"), lAddedResults.IsValid());
	});

	LatentIt("Frames moved Event Graph nodes in both graph panes and keeps function rollback scoped", FTimespan::FromSeconds(12.0), [this](const FDoneDelegate & aDone)
	{
		const TSharedRef<TStrongObjectPtr<UBlueprint>> lRevision = MakeShared<TStrongObjectPtr<UBlueprint>>(CreateFixtureBlueprint());
		const TSharedRef<TStrongObjectPtr<UBlueprint>> lWorking = MakeShared<TStrongObjectPtr<UBlueprint>>(CreateFixtureBlueprint());
		const auto lAddBranch = [](UBlueprint * aBlueprint, int32 aX, int32 aY)
		{
			FGraphNodeCreator<UK2Node_IfThenElse> lCreator(*aBlueprint->UbergraphPages[0]);
			UK2Node_IfThenElse * const lNode = lCreator.CreateNode();
			lCreator.Finalize();
			lNode->NodePosX = aX;
			lNode->NodePosY = aY;
			return lNode;
		};

		UK2Node_IfThenElse * const lOldNode = lAddBranch(lRevision->Get(), 720, 140);
		UK2Node_IfThenElse * const lWorkingNode = lAddBranch(lWorking->Get(), 1410, -280);
		lWorkingNode->NodeGuid = lOldNode->NodeGuid;
		FBlueprintEditorModule & lModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");
		const TSharedRef<IBlueprintEditor> lEditor = lModule.CreateBlueprintEditor(EToolkitMode::Standalone, nullptr, lWorking->Get(), false);
		bool lFixtureReady = false;
		ON_SCOPE_EXIT
		{
			if (!lFixtureReady)
			{
				lRevision->Get()->GetOutermost()->SetDirtyFlag(false);
				lWorking->Get()->GetOutermost()->SetDirtyFlag(false);
				lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
				aDone.Execute();
			}
		};
		const TSharedPtr<SDockTab> lHost = lEditor->GetTabManager()->GetOwnerTab();
		const TSharedPtr<SGraphEditor> lGraph = lEditor->OpenGraphAndBringToFront(lWorkingNode->GetGraph(), false);
		if (!TestNotNull(TEXT("Blueprint editor host"), lHost.Get())
			|| !TestTrue(TEXT("Working graph is available"), lGraph && lGraph->GetGraphPanel()))
		{
			return;
		}

		const TSharedRef<SBlueprintDiffEdit> lDiff = SNew(SBlueprintDiffEdit, lEditor, lHost.ToSharedRef(), lWorking->Get());
		lHost->SetContent(lDiff);
		lDiff->m_RevisionBlueprint.Reset(lRevision->Get());
		lDiff->DoRebuildGraphList();
		TestTrue(TEXT("The changed Event Graph is selected"),
			lDiff->m_SelectedGraph && lDiff->m_SelectedGraph->Root.Working.Get() == lWorkingNode->GetGraph());
		TestFalse(TEXT("Event Graph cannot use function rollback"), lDiff->CanRevertFunction(lDiff->m_SelectedGraph));
		const auto & lHighlights = BlueprintDiffGraphPanel::GetResults(*lGraph->GetGraphPanel());
		TestTrue(TEXT("The changed Event Graph node has a native diff outline"), lHighlights
			&& lHighlights->ContainsByPredicate([lWorkingNode](const FDiffSingleResult & aResult) { return aResult.Node1 == lWorkingNode; }));
		if (!TestTrue(TEXT("Opening the Event Graph queues its first changed node"), lDiff->m_PendingFocus
			&& lDiff->m_PendingFocus->WorkingNode == lWorkingNode->NodeGuid))
		{
			lDiff->Close();
			return;
		}

		lFixtureReady = true;
		const double lDeadline = FPlatformTime::Seconds() + 5.0;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[this, lEditor, lRevision, lWorking, lDiff, lGraph, lOldNode, lWorkingNode, lDeadline, aDone](float)
			{
				if (lDiff->m_PendingFocus && FPlatformTime::Seconds() < lDeadline)
					return true;

				ON_SCOPE_EXIT
				{
					lDiff->Close();
					lRevision->Get()->GetOutermost()->SetDirtyFlag(false);
					lWorking->Get()->GetOutermost()->SetDirtyFlag(false);
					lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
					aDone.Execute();
				};

				const auto lCheckFrame = [this](const TCHAR * aLabel, const SGraphPanel & aPanel, UEdGraphNode * aNode)
				{
					FVector2f lMin, lMax;
					if (!TestTrue(aLabel, aPanel.GetBoundsForNode(aNode, lMin, lMax)))
						return;

					const FVector2f lDimensions(aPanel.GetTickSpaceGeometry().GetLocalSize());
					const FVector2f lAnchor(lMin.X, (lMin.Y + lMax.Y) * 0.5f);
					const FVector2f lOnScreen = (lAnchor - FVector2f(aPanel.GetViewOffset())) * aPanel.GetZoomAmount();
					TestTrue(FString::Printf(TEXT("%s anchors the node at 20%% across and 50%% down (actual %.1f, %.1f in %.1f x %.1f)"),
						aLabel, lOnScreen.X, lOnScreen.Y, lDimensions.X, lDimensions.Y),
						lDimensions.X > 0.0f && lDimensions.Y > 0.0f
						&& lOnScreen.Equals(lDimensions * FVector2f(0.20f, 0.50f), 2.0f));
				};

				if (TestTrue(TEXT("Revision graph panel is available"),
					lDiff->m_RevisionGraphEditor && lDiff->m_RevisionGraphEditor->GetGraphPanel()))
				{
					lCheckFrame(TEXT("Revision pane"), *lDiff->m_RevisionGraphEditor->GetGraphPanel(), lOldNode);
				}
				lCheckFrame(TEXT("Working pane"), *lGraph->GetGraphPanel(), lWorkingNode);
				TestTrue(TEXT("Diff focus preserves an empty working selection for Format Graph"), lGraph->GetSelectedNodes().IsEmpty());
				return false;
			}));
	});

	It("Keeps the changed list beside both revision panes and lets each divider resize", [this]
	{
		const TStrongObjectPtr<UBlueprint> lBlueprint(CreateFixtureBlueprint());
		if (!TestNotNull(TEXT("Working Blueprint"), lBlueprint.Get()))
			return;

		ON_SCOPE_EXIT { lBlueprint->GetOutermost()->SetDirtyFlag(false); };
		FBlueprintEditorModule & lModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");
		const TSharedRef<IBlueprintEditor> lEditor = lModule.CreateBlueprintEditor(EToolkitMode::Standalone, nullptr, lBlueprint.Get(), false);
		ON_SCOPE_EXIT { lEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); };
		const TSharedPtr<SDockTab> lHost = lEditor->GetTabManager()->GetOwnerTab();
		if (!TestNotNull(TEXT("Native Blueprint editor host"), lHost.Get()))
			return;

		const TSharedRef<SBlueprintDiffEdit> lDiff = SNew(SBlueprintDiffEdit, lEditor, lHost.ToSharedRef(), lBlueprint.Get());
		lHost->SetContent(lDiff);
		ON_SCOPE_EXIT { lDiff->Close(); };

		FChildren * const lRootChildren = lDiff->GetChildren();
		if (!TestEqual(TEXT("One layout root"), lRootChildren->Num(), 1))
			return;
		const TSharedRef<SWidget> lRoot = lRootChildren->GetChildAt(0);
		if (!TestEqual(TEXT("Root is the diff/editor splitter"), lRoot->GetType(), FName(TEXT("SSplitter")))
			|| !TestEqual(TEXT("Diff and working editor slots"), lRoot->GetChildren()->Num(), 2))
		{
			return;
		}

		FChildren * const lBorderChildren = lRoot->GetChildren()->GetChildAt(0)->GetChildren();
		if (!TestEqual(TEXT("One column splitter in the diff border"), lBorderChildren->Num(), 1))
			return;
		const TSharedRef<SWidget> lColumns = lBorderChildren->GetChildAt(0);
		if (!TestEqual(TEXT("Functions and revision column splitter"), lColumns->GetType(), FName(TEXT("SSplitter")))
			|| !TestEqual(TEXT("Two diff columns"), lColumns->GetChildren()->Num(), 2))
		{
			return;
		}
		TestEqual(TEXT("Changed function list occupies the left column"),
			lColumns->GetChildren()->GetChildAt(0)->GetTag(), FName(TEXT("BlueprintDiffEditFunctions")));

		const TSharedRef<SWidget> lRevision = lColumns->GetChildren()->GetChildAt(1);
		if (!TestEqual(TEXT("Revision header and graph splitter"), lRevision->GetType(), FName(TEXT("SSplitter")))
			|| !TestEqual(TEXT("Two revision panes"), lRevision->GetChildren()->Num(), 2))
		{
			return;
		}
		TestEqual(TEXT("Revision controls occupy the upper right pane"),
			lRevision->GetChildren()->GetChildAt(0)->GetTag(), FName(TEXT("BlueprintDiffEditRevisionControls")));
		TestEqual(TEXT("Old graph occupies the lower right pane"),
			lRevision->GetChildren()->GetChildAt(1)->GetTag(), FName(TEXT("BlueprintDiffEditBaselinePreview")));

		constexpr TCHAR SECTION[] = TEXT("BlueprintDiffEdit.Layout");
		const TCHAR * const lKeys[] = { TEXT("DiffWidth"), TEXT("FunctionsWidth"), TEXT("RevisionControlsHeight") };
		FString lOriginalValues[UE_ARRAY_COUNT(lKeys)];
		bool lHadValues[UE_ARRAY_COUNT(lKeys)];
		for (int32 lIndex = 0; lIndex < UE_ARRAY_COUNT(lKeys); ++lIndex)
			lHadValues[lIndex] = GConfig->GetString(SECTION, lKeys[lIndex], lOriginalValues[lIndex], GEditorPerProjectIni);
		ON_SCOPE_EXIT
		{
			for (int32 lIndex = 0; lIndex < UE_ARRAY_COUNT(lKeys); ++lIndex)
			{
				if (lHadValues[lIndex])
					GConfig->SetString(SECTION, lKeys[lIndex], *lOriginalValues[lIndex], GEditorPerProjectIni);
				else
					GConfig->RemoveKey(SECTION, lKeys[lIndex], GEditorPerProjectIni);
			}
			GConfig->Flush(false, GEditorPerProjectIni);
		};

		const TStrongObjectPtr<UBlueprint> lOtherBlueprint(CreateFixtureBlueprint());
		ON_SCOPE_EXIT { lOtherBlueprint->GetOutermost()->SetDirtyFlag(false); };
		const TSharedRef<IBlueprintEditor> lOtherEditor = lModule.CreateBlueprintEditor(EToolkitMode::Standalone, nullptr, lOtherBlueprint.Get(), false);
		ON_SCOPE_EXIT { lOtherEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); };
		const TSharedPtr<SDockTab> lOtherHost = lOtherEditor->GetTabManager()->GetOwnerTab();
		if (!TestNotNull(TEXT("Second Blueprint editor host"), lOtherHost.Get()))
			return;

		const TSharedRef<SBlueprintDiffEdit> lOtherDiff = SNew(SBlueprintDiffEdit, lOtherEditor, lOtherHost.ToSharedRef(), lOtherBlueprint.Get());
		lOtherHost->SetContent(lOtherDiff);
		ON_SCOPE_EXIT { lOtherDiff->Close(); };

		lDiff->SlatePrepass(1.0f);
		DoCheckDivider(StaticCastSharedRef<SSplitter>(lRoot).Get(), FVector2f(2400.0f, 850.0f), true);
		DoCheckDivider(StaticCastSharedRef<SSplitter>(lColumns).Get(), FVector2f(1100.0f, 850.0f), true);
		DoCheckDivider(StaticCastSharedRef<SSplitter>(lRevision).Get(), FVector2f(470.0f, 850.0f), false);

		for (int32 lIndex = 0; lIndex < UE_ARRAY_COUNT(lKeys); ++lIndex)
		{
			const float lSize = lDiff->m_Splitters[lIndex]->SlotAt(0).GetSizeValue();
			float lSaved = 0.0f;
			TestTrue(lKeys[lIndex], GConfig->GetFloat(SECTION, lKeys[lIndex], lSaved, GEditorPerProjectIni)
				&& FMath::IsNearlyEqual(lSize, lSaved, 0.0001f));
			TestEqual(TEXT("The Blueprint already open uses the new size"), lOtherDiff->m_Splitters[lIndex]->SlotAt(0).GetSizeValue(), lSize);
		}

		lDiff->Close();
		lOtherDiff->Close();
		const TSharedRef<SBlueprintDiffEdit> lReopened = SNew(SBlueprintDiffEdit, lOtherEditor, lOtherHost.ToSharedRef(), lOtherBlueprint.Get());
		lOtherHost->SetContent(lReopened);
		ON_SCOPE_EXIT { lReopened->Close(); };
		for (int32 lIndex = 0; lIndex < UE_ARRAY_COUNT(lKeys); ++lIndex)
		{
			TestTrue(TEXT("Reopening a different Blueprint restores its saved divider"),
				FMath::IsNearlyEqual(lReopened->m_Splitters[lIndex]->SlotAt(0).GetSizeValue(),
					lDiff->m_Splitters[lIndex]->SlotAt(0).GetSizeValue(), 0.0001f));
		}
	});
}

#endif
