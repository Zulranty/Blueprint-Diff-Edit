#include "SMaterialDiffEdit.h"

#include "Async/Async.h"
#include "BlueprintDiffGraphPanel.h"
#include "DiffUtils.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GraphDiffControl.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "IMaterialEditor.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/ToolkitManager.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "MaterialDiffEdit"

struct FMaterialDiffSnapshot
{
	FString Filename;
	FBlueprintDiffMaster Revision;

	~FMaterialDiffSnapshot()
	{
		if (!Filename.IsEmpty())
			IFileManager::Get().Delete(*Filename);
	}
};

namespace
{
	UMaterialGraph * BuildGraph(UMaterial & aMaterial, const FString & aAssetName)
	{
		UMaterialGraph * const lGraph = NewObject<UMaterialGraph>(&aMaterial, NAME_None, RF_Transient);
		aMaterial.MaterialGraph = lGraph;
		lGraph->Material = &aMaterial;
		lGraph->Schema = UMaterialGraphSchema::StaticClass();
		lGraph->OriginalMaterialFullName = aAssetName;
		lGraph->RebuildGraph();
		lGraph->bEditable = false;
		if (lGraph->RootNode)
			lGraph->RootNode->NodeGuid = FGuid(0, 0, 0, 1);
		for (UMaterialExpression * const lExpression : aMaterial.GetExpressions())
		{
			const FGuid lId = lExpression->GetMaterialExpressionId();
			if (lId.IsValid() && lExpression->GraphNode)
				lExpression->GraphNode->NodeGuid = lId;
		}
		return lGraph;
	}

	FText ExportProperty(const FResolvedProperty & aResolved)
	{
		if (!aResolved.Property || !aResolved.Object)
			return LOCTEXT("Missing", "(not present)");

		FString lValue;
		aResolved.Property->ExportTextItem_Direct(lValue,
			aResolved.Property->ContainerPtrToValuePtr<void>(aResolved.Object), nullptr, nullptr, PPF_None);
		return FText::FromString(MoveTemp(lValue));
	}

	void AddPropertyDiffs(const UObject * aBefore, const UObject * aAfter, const FText & aSection,
		TArray<FDiffSingleResult> & aChanges, UEdGraphNode * aOldNode = nullptr, UEdGraphNode * aNewNode = nullptr)
	{
		TArray<FSingleObjectDiffEntry> lProperties;
		DiffUtils::CompareUnrelatedObjects(aBefore, aAfter, lProperties);
		for (const FSingleObjectDiffEntry & lProperty : lProperties)
		{
			const FResolvedProperty lBefore = lProperty.DiffType == EPropertyDiffType::PropertyAddedToB
				? FResolvedProperty() : lProperty.Identifier.Resolve(aBefore);
			const FResolvedProperty lAfter = lProperty.DiffType == EPropertyDiffType::PropertyAddedToA
				? FResolvedProperty() : lProperty.Identifier.Resolve(aAfter);
			FDiffSingleResult & lChange = aChanges.AddDefaulted_GetRef();
			lChange.Node1 = aOldNode;
			lChange.Node2 = aNewNode;
			lChange.Diff = aOldNode ? EDiffType::NODE_PROPERTY : EDiffType::OBJECT_PROPERTY;
			lChange.Category = EDiffType::MODIFICATION;
			lChange.DisplayString = FText::Format(LOCTEXT("Property", "{0} · {1}"), aSection,
				FText::FromString(lProperty.Identifier.ToDisplayName()));
			lChange.ToolTip = FText::Format(LOCTEXT("PropertyValues", "Revision\n{0}\n\nWorking material\n{1}"),
				ExportProperty(lBefore), ExportProperty(lAfter));
		}
	}

	void DiffGraphs(UMaterialGraph & aBefore, UMaterialGraph & aAfter, TMap<UEdGraph *, UEdGraph *> & aPairs,
		TArray<FDiffSingleResult> & aChanges)
	{
		aPairs.Add(&aBefore, &aAfter);
		if (aBefore.RootNode && aAfter.RootNode)
			aBefore.RootNode->NodeGuid = aAfter.RootNode->NodeGuid;
		TMap<FGuid, UMaterialGraphNode *> lBeforeNodes;
		for (UEdGraphNode * const lNode : aBefore.Nodes)
		{
			if (UMaterialGraphNode * const lMaterialNode = Cast<UMaterialGraphNode>(lNode);
				lMaterialNode && lMaterialNode->MaterialExpression)
			{
				const FGuid lId = lMaterialNode->MaterialExpression->GetMaterialExpressionId();
				if (lId.IsValid())
				{
					lNode->NodeGuid = lId;
					lBeforeNodes.Add(lId, lMaterialNode);
				}
			}
		}
		for (UEdGraphNode * const lNode : aAfter.Nodes)
		{
			if (UMaterialGraphNode * const lMaterialNode = Cast<UMaterialGraphNode>(lNode);
				lMaterialNode && lMaterialNode->MaterialExpression)
			{
				if (UMaterialGraphNode * const lBefore = lBeforeNodes.FindRef(lMaterialNode->MaterialExpression->GetMaterialExpressionId()))
				{
					// Editor graph GUIDs are local to each editor session. Change only the historical graph.
					lBefore->NodeGuid = lNode->NodeGuid;
					AddPropertyDiffs(lBefore->MaterialExpression, lMaterialNode->MaterialExpression,
						lNode->GetNodeTitle(ENodeTitleType::ListView), aChanges, lBefore, lNode);
				}
			}
		}
		FGraphDiffControl::DiffGraphs(&aBefore, &aAfter, aChanges);
		for (UEdGraph * const lGraph : aBefore.SubGraphs)
		{
			UMaterialGraph * const lBefore = Cast<UMaterialGraph>(lGraph);
			if (!lBefore || !lBefore->SubgraphExpression)
				continue;
			for (UEdGraph * const lCandidate : aAfter.SubGraphs)
			{
				UMaterialGraph * const lAfter = Cast<UMaterialGraph>(lCandidate);
				if (lAfter && lAfter->SubgraphExpression
					&& lBefore->SubgraphExpression->GetMaterialExpressionId() == lAfter->SubgraphExpression->GetMaterialExpressionId())
				{
					DiffGraphs(*lBefore, *lAfter, aPairs, aChanges);
					break;
				}
			}
		}
	}
}

bool SMaterialDiffEdit::Supports(const UObject * aObject)
{
	return aObject && (aObject->IsA<UMaterial>() || aObject->IsA<UMaterialInstanceConstant>());
}

TSharedPtr<SMaterialDiffEdit> SMaterialDiffEdit::Open(UMaterialInterface * aMaterial)
{
	if (!Supports(aMaterial) || !GEditor || !GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(aMaterial))
		return nullptr;

	const TSharedPtr<IToolkit> lToolkit = FToolkitManager::Get().FindEditorForAsset(aMaterial);
	const FName lExpected = aMaterial->IsA<UMaterial>() ? FName(TEXT("MaterialEditor")) : FName(TEXT("MaterialInstanceEditor"));
	if (!lToolkit || lToolkit->GetToolkitFName() != lExpected)
		return nullptr;

	const TSharedPtr<IMaterialEditor> lEditor = StaticCastSharedPtr<IMaterialEditor>(lToolkit);
	const TSharedPtr<FTabManager> lTabManager = lEditor->GetTabManager();
	const TSharedPtr<SDockTab> lTab = lTabManager ? lTabManager->GetOwnerTab() : nullptr;
	if (!lTab)
		return nullptr;

	const TSharedRef<SMaterialDiffEdit> lPanel = SNew(SMaterialDiffEdit, aMaterial, lEditor.ToSharedRef(), lTab.ToSharedRef());
	lTab->SetContent(lPanel);
	return lPanel;
}

bool SMaterialDiffEdit::TryFocus(UMaterialInterface * aMaterial) const
{
	const TSharedPtr<SDockTab> lTab = m_HostTab.Pin();
	if (m_Source.Get() != aMaterial || !lTab || &lTab->GetContent().Get() != this)
		return false;
	GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(aMaterial, true);
	return true;
}

bool SMaterialDiffEdit::IsEditingAnyFile(const TArray<FString> & aFiles) const
{
	return aFiles.ContainsByPredicate([this](const FString & aFile) { return FPaths::IsSamePath(m_Filename, aFile); });
}

void SMaterialDiffEdit::Close()
{
	const TSharedRef<SWidget> lKeepAlive = AsShared();
	DoCancel();
	DoUpdateGraphLock(nullptr);
	if (const TSharedPtr<SDockTab> lTab = m_HostTab.Pin(); lTab && &lTab->GetContent().Get() == this)
	{
		m_NewBox->SetContent(SNullWidget::NullWidget);
		lTab->SetContent(m_OriginalContent.ToSharedRef());
	}
	m_HostTab.Reset();
	m_Editor.Reset();
	m_OriginalContent.Reset();
}

void SMaterialDiffEdit::Construct(const FArguments & aArgs, UMaterialInterface * aMaterial,
	const TSharedRef<IMaterialEditor> & aEditor, const TSharedRef<SDockTab> & aHostTab)
{
	m_Source = aMaterial;
	m_Editor = aEditor;
	m_HostTab = aHostTab;
	m_OriginalContent = aHostTab->GetContent();
	if (!FPackageName::TryConvertLongPackageNameToFilename(aMaterial->GetOutermost()->GetName(),
		m_Filename, FPackageName::GetAssetPackageExtension()))
		m_Status = LOCTEXT("InvalidAsset", "This material does not have a source-controlled package path.");
	else
		m_Filename = FPaths::ConvertRelativePathToFull(m_Filename);

	const TSharedRef<SWidget> lComparison =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 6)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1)
			[
				SNew(SComboButton)
				.IsEnabled_Lambda([this] { return !m_Filename.IsEmpty(); })
				.OnGetMenuContent(this, &SMaterialDiffEdit::DoBuildRevisionMenu)
				.ButtonContent()
				[
					SNew(STextBlock).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text_Lambda([this] { return m_RevisionMaterial ? m_Commit.GetMenuLabel(true)
						: LOCTEXT("SelectRevision", "Select a Git revision"); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
			[
				SNew(SButton).Text(LOCTEXT("Refresh", "Refresh"))
				.IsEnabled_Lambda([this] { return m_RevisionMaterial.IsValid() && m_Source.IsValid(); })
				.OnClicked_Lambda([this] { DoRefreshComparison(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
			[
				SNew(SButton).Text(LOCTEXT("CloseDiff", "Close Diff"))
				.OnClicked_Lambda([this] { Close(); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 5)
		[
			SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return m_Status.IsEmpty() ? m_ComparisonStatus : m_Status; })
			.Visibility_Lambda([this] { return m_Status.IsEmpty() && m_ComparisonStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		]
		+ SVerticalBox::Slot().FillHeight(1)
		[
			SNew(SSplitter).Orientation(Orient_Vertical)
			+ SSplitter::Slot().Value(0.35f).MinSize(80)
			[
				SAssignNew(m_List, SListView<FChange>).ListItemsSource(&m_Changes)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SMaterialDiffEdit::DoBuildRow)
				.OnSelectionChanged(this, &SMaterialDiffEdit::DoSelect)
			]
			+ SSplitter::Slot().Value(0.65f).MinSize(100)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 4)
				[
					SNew(STextBlock).Text(LOCTEXT("OldGraph", "Revision (read-only)"))
					.Visibility_Lambda([this] { return m_OldGraph ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot().FillHeight(1)
				[
					SAssignNew(m_OldBox, SBox)
					.Visibility_Lambda([this] { return m_OldGraph ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 4)
				[
					SNew(SBox).MaxDesiredHeight(160)
					.Visibility_Lambda([this] { return m_Detail.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return m_Detail; })
						]
					]
				]
			]
		];
	ChildSlot
	[
		SNew(SSplitter)
		+ SSplitter::Slot().Value(0.36f).MinSize(240)
		[
			lComparison
		]
		+ SSplitter::Slot().Value(0.64f).MinSize(350)
		[
			SAssignNew(m_NewBox, SBox)
			[
				m_OriginalContent.ToSharedRef()
			]
		]
	];
	FCoreUObjectDelegates::OnObjectModified.AddSP(this, &SMaterialDiffEdit::DoScheduleRefresh);
	FCoreUObjectDelegates::OnObjectPropertyChanged.AddSPLambda(this, [this](UObject * aObject, FPropertyChangedEvent &)
	{
		DoScheduleRefresh(aObject);
	});
	FCoreUObjectDelegates::OnObjectTransacted.AddSPLambda(this, [this](UObject * aObject, const FTransactionObjectEvent &)
	{
		DoScheduleRefresh(aObject);
	});
	if (!m_Filename.IsEmpty())
	{
		m_Status = LOCTEXT("ReadingHistory", "Reading Git history…");
		m_CancelHistory = MakeShared<TAtomic<bool>>(false);
		m_History = Async(EAsyncExecution::ThreadPool, [lExecutable = BlueprintDiffGit::GetExecutable(),
			lFilename = m_Filename, lCancel = m_CancelHistory]
		{
			return BlueprintDiffGit::ReadFileHistory(lExecutable, lFilename,
				[](TOptional<FBlueprintDiffGitRevision>) {}, lCancel.Get(), false);
		});
	}
}

SMaterialDiffEdit::~SMaterialDiffEdit()
{
	DoCancel();
	DoUpdateGraphLock(nullptr);
}

void SMaterialDiffEdit::DoCancel()
{
	FCoreUObjectDelegates::OnObjectModified.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectTransacted.RemoveAll(this);
	if (m_CancelHistory)
		m_CancelHistory->Store(true);
	if (m_CancelRevision)
		m_CancelRevision->Store(true);
	m_History = {};
	m_Request = {};
	m_RefreshAt = 0;
}

void SMaterialDiffEdit::DoScheduleRefresh(UObject * aObject)
{
	if (!m_RevisionMaterial || !aObject)
		return;
	const TSharedPtr<IMaterialEditor> lEditor = m_Editor.Pin();
	const UMaterialInterface * const lLive = lEditor ? lEditor->GetMaterialInterface() : nullptr;
	if (!lLive)
		return;

	const UMaterialEditorInstanceConstant * lInstanceEditor = Cast<UMaterialEditorInstanceConstant>(aObject);
	if (!lInstanceEditor)
		lInstanceEditor = aObject->GetTypedOuter<UMaterialEditorInstanceConstant>();
	if (aObject == m_Source.Get() || aObject == lLive || aObject->IsIn(lLive)
		|| (lInstanceEditor && lInstanceEditor->SourceInstance == lLive))
		m_RefreshAt = FPlatformTime::Seconds() + 0.15;
}

void SMaterialDiffEdit::Tick(const FGeometry & aGeometry, double aTime, float aDelta)
{
	SCompoundWidget::Tick(aGeometry, aTime, aDelta);
	if (m_History.IsValid() && m_History.IsReady())
	{
		FBlueprintDiffFileHistory lHistory = m_History.Consume();
		m_CancelHistory.Reset();
		m_HistoryError = MoveTemp(lHistory.Error);
		m_Revisions = MoveTemp(lHistory.Revisions);
		if (!m_RevisionRequested)
		{
			if (!m_Revisions.IsEmpty())
				DoRequest(m_Revisions[0]);
			else
				m_Status = m_HistoryError.IsEmpty() ? LOCTEXT("NoHistory", "No Git revisions are available for this material.") : m_HistoryError;
		}
	}
	if (m_Request.IsValid() && m_Request.IsReady())
	{
		const TSharedPtr<FMaterialDiffSnapshot> lSnapshot = m_Request.Consume();
		m_CancelRevision.Reset();
		DoApply(*lSnapshot);
	}
	if (m_RefreshAt > 0 && FPlatformTime::Seconds() >= m_RefreshAt)
		DoRefreshComparison();
	if (m_Editor.IsValid() && m_OldGraph && !m_LockedGraph.IsValid())
		DoUpdateGraphLock(m_GraphPairs.FindRef(m_OldGraph->GetCurrentGraph()));
}

TSharedRef<SWidget> SMaterialDiffEdit::DoBuildRevisionMenu()
{
	FMenuBuilder lMenu(true, nullptr);
	lMenu.AddMenuEntry(LOCTEXT("DefaultBranch", "Compare with default branch"), FText(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SMaterialDiffEdit::DoRequest, TOptional<FBlueprintDiffGitRevision>())));
	for (const FBlueprintDiffGitRevision & lRevision : m_Revisions)
		lMenu.AddMenuEntry(lRevision.Commit.GetMenuLabel(true), FText::FromString(lRevision.Description), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SMaterialDiffEdit::DoRequest, TOptional<FBlueprintDiffGitRevision>(lRevision))));
	return lMenu.MakeWidget();
}

void SMaterialDiffEdit::DoRequest(TOptional<FBlueprintDiffGitRevision> aRevision)
{
	if (m_CancelRevision)
		m_CancelRevision->Store(true);
	m_RevisionRequested = true;
	m_Status = LOCTEXT("LoadingMaterial", "Reading the material revision…");
	m_CancelRevision = MakeShared<TAtomic<bool>>(false);
	const FString lDirectory = FPaths::ConvertRelativePathToFull(FPaths::DiffDir());
	m_Request = Async(EAsyncExecution::ThreadPool, [lExecutable = BlueprintDiffGit::GetExecutable(), lFilename = m_Filename,
		lDirectory, lCancel = m_CancelRevision, lRevision = MoveTemp(aRevision)]
	{
		const TSharedPtr<FMaterialDiffSnapshot> lSnapshot = MakeShared<FMaterialDiffSnapshot>();
		IFileManager::Get().MakeDirectory(*lDirectory, true);
		lSnapshot->Filename = FPaths::CreateTempFilename(*lDirectory, TEXT("temp-material-diff-revision-"), TEXT(".uasset"));
		lSnapshot->Revision = lRevision
			? BlueprintDiffGit::ReadHistoryEntry(lExecutable, *lRevision, lCancel.Get(), &lSnapshot->Filename)
			: BlueprintDiffGit::ReadMaster(lExecutable, lFilename, lCancel.Get(), &lSnapshot->Filename);
		return lSnapshot;
	});
}

void SMaterialDiffEdit::DoApply(const FMaterialDiffSnapshot & aSnapshot)
{
	if (!m_Source.IsValid())
		return;
	if (!aSnapshot.Revision.Error.IsEmpty())
	{
		m_Status = aSnapshot.Revision.Error;
		return;
	}
	const FPackagePath lTemp = FPackagePath::FromLocalPath(aSnapshot.Filename);
	UPackage * const lPackage = FPackageName::IsTempPackage(lTemp.GetPackageName()) ? DiffUtils::LoadPackageForDiff(lTemp,
		FPackagePath::FromLocalPath(aSnapshot.Revision.SourceFilename)) : nullptr;
	UMaterialInterface * lMaterial = nullptr;
	if (lPackage && lPackage != m_Source->GetOutermost())
	{
		lPackage->FullyLoad();
		ResetLoaders(lPackage);
		lMaterial = FindObject<UMaterialInterface>(lPackage, *FPaths::GetBaseFilename(aSnapshot.Revision.SourceFilename));
		if (!lMaterial)
			lMaterial = FindObject<UMaterialInterface>(lPackage, *m_Source->GetName());
	}
	if (!Supports(lMaterial) || lMaterial->GetClass() != m_Source->GetClass())
	{
		m_Status = LOCTEXT("InvalidMaterial", "The revision could not be loaded as a material of the same type.");
		return;
	}

	m_Commit = aSnapshot.Revision.Commit;
	m_Status = FText();
	m_RevisionMaterial.Reset(lMaterial);
	DoRefreshComparison(true);
}

void SMaterialDiffEdit::DoRefreshComparison(bool aFocusFirstChange)
{
	m_RefreshAt = 0;
	UMaterialInterface * const lSource = m_Source.Get();
	if (!lSource || !m_RevisionMaterial)
		return;
	const TSharedPtr<IMaterialEditor> lEditor = m_Editor.Pin();
	UMaterialInterface * const lLive = lEditor ? lEditor->GetMaterialInterface() : nullptr;
	if (!lLive)
		return;

	const TWeakObjectPtr<UEdGraph> lPreviousGraph = m_OldGraph ? m_OldGraph->GetCurrentGraph() : nullptr;
	if (aFocusFirstChange)
		DoShowGraphs(nullptr, nullptr);
	m_List->ClearSelection();
	m_Detail = FText();
	m_Changes.Reset();
	m_Highlights->Reset();
	m_WorkingHighlights->Reset();
	m_GraphPairs.Reset();

	TArray<FDiffSingleResult> lDiffs;
	AddPropertyDiffs(m_RevisionMaterial.Get(), lLive, LOCTEXT("MaterialSettings", "Material Settings"), lDiffs);
	if (UMaterial * const lMaterial = Cast<UMaterial>(lLive))
	{
		UMaterial * const lRevision = CastChecked<UMaterial>(m_RevisionMaterial.Get());
		UMaterialGraph * const lBefore = lRevision->MaterialGraph ? lRevision->MaterialGraph.Get() : BuildGraph(*lRevision, lSource->GetName());
		UMaterialGraph * const lAfter = lMaterial->MaterialGraph;
		if (lAfter)
		{
			DiffGraphs(*lBefore, *lAfter, m_GraphPairs, lDiffs);
			UEdGraph * const lDisplayed = !aFocusFirstChange && m_GraphPairs.Contains(lPreviousGraph.Get())
				? lPreviousGraph.Get() : lBefore;
			DoShowGraphs(lDisplayed, m_GraphPairs.FindRef(lDisplayed));
		}
	}
	for (const FDiffSingleResult & lDiff : lDiffs)
	{
		if (!lDiff.IsRealDifference())
			continue;

		const FChange lChange = MakeShared<FChangeEntry>();
		lChange->Display = lDiff;
		lChange->Display.Node1 = lChange->Display.Node2 = nullptr;
		lChange->Display.Pin1 = lChange->Display.Pin2 = nullptr;
		const auto lClassify = [&](UEdGraphNode * aNode)
		{
			if (aNode)
			{
				if (m_GraphPairs.Contains(aNode->GetGraph()))
					lChange->OldNode = aNode;
				else
					lChange->NewNode = aNode;
			}
		};
		lClassify(lDiff.Node1 ? lDiff.Node1 : lDiff.Pin1 ? lDiff.Pin1->GetOwningNode() : nullptr);
		lClassify(lDiff.Node2 ? lDiff.Node2 : lDiff.Pin2 ? lDiff.Pin2->GetOwningNode() : nullptr);
		m_Changes.Add(lChange);

		// Slate's diff painter dereferences pins, so keep pin pointers only for the historical graph.
		FDiffSingleResult & lHighlight = m_Highlights->Add_GetRef(lChange->Display);
		lHighlight.Node1 = lChange->OldNode.Get();
		if (lDiff.Pin1 && m_GraphPairs.Contains(lDiff.Pin1->GetOwningNode()->GetGraph()))
			lHighlight.Pin1 = lDiff.Pin1;
		else if (lDiff.Pin2 && m_GraphPairs.Contains(lDiff.Pin2->GetOwningNode()->GetGraph()))
			lHighlight.Pin1 = lDiff.Pin2;
		if (UEdGraphNode * const lNode = lChange->NewNode.Get())
			m_WorkingHighlights->Add_GetRef(lChange->Display).Node1 = lNode;
	}
	if (SGraphPanel * const lPanel = m_OldGraph ? m_OldGraph->GetGraphPanel() : nullptr)
		lPanel->Invalidate(EInvalidateWidgetReason::Paint);
	if (const TSharedPtr<SGraphEditor> lWorking = m_LockedGraph.Pin())
	{
		if (SGraphPanel * const lPanel = lWorking->GetGraphPanel())
			lPanel->Invalidate(EInvalidateWidgetReason::Paint);
	}
	m_List->RequestListRefresh();
	m_ComparisonStatus = m_Changes.IsEmpty() ? LOCTEXT("NoChanges", "No material differences found.") : FText();
	if (aFocusFirstChange && !m_Changes.IsEmpty())
		m_List->SetSelection(m_Changes[0]);
}

void SMaterialDiffEdit::DoShowGraphs(UEdGraph * aRevision, UEdGraph * aWorking)
{
	if (!m_OldGraph || m_OldGraph->GetCurrentGraph() != aRevision)
	{
		DoUpdateGraphLock(nullptr);
		m_OldGraph.Reset();
		m_OldBox->SetContent(SNullWidget::NullWidget);
		if (aRevision)
		{
			m_OldBox->SetContent(SAssignNew(m_OldGraph, SGraphEditor).GraphToEdit(aRevision)
				.IsEditable(false).DisplayAsReadOnly(true).DiffResults(m_Highlights));
		}
	}
	DoUpdateGraphLock(aWorking);
}

void SMaterialDiffEdit::DoUpdateGraphLock(UEdGraph * aWorking)
{
	const TSharedPtr<SGraphEditor> lWorking = aWorking && m_OldGraph ? SGraphEditor::FindGraphEditorForGraph(aWorking) : nullptr;
	const TSharedPtr<SGraphEditor> lPrevious = m_LockedGraph.Pin();
	if (lPrevious == lWorking)
		return;

	if (lPrevious && m_OldGraph)
	{
		m_OldGraph->UnlockFromGraphEditor(lPrevious);
		lPrevious->UnlockFromGraphEditor(m_OldGraph);
		if (SGraphPanel * const lPanel = lPrevious->GetGraphPanel())
		{
			auto & lResults = BlueprintDiffGraphPanel::GetResults(*lPanel);
			if (lResults == m_WorkingHighlights)
			{
				lResults.Reset();
				lPanel->Invalidate(EInvalidateWidgetReason::Paint);
			}
		}
	}
	if (lWorking)
	{
		FVector2f lPosition = FVector2f::ZeroVector;
		float lZoom = 1.0f;
		lWorking->GetViewLocation(lPosition, lZoom);
		m_OldGraph->SetViewLocation(lPosition, lZoom);
		m_OldGraph->LockToGraphEditor(lWorking);
		lWorking->LockToGraphEditor(m_OldGraph);
		if (SGraphPanel * const lPanel = lWorking->GetGraphPanel())
		{
			auto & lResults = BlueprintDiffGraphPanel::GetResults(*lPanel);
			if (!lResults)
			{
				lResults = m_WorkingHighlights;
				lPanel->Invalidate(EInvalidateWidgetReason::Paint);
			}
		}
	}
	m_LockedGraph = lWorking;
}

void SMaterialDiffEdit::DoSelect(FChange aChange, ESelectInfo::Type aSelectInfo)
{
	if (!aChange || m_RefreshAt > 0)
		return;
	m_Detail = aChange->Display.ToolTip;
	UEdGraphNode * const lOld = aChange->OldNode.Get();
	UEdGraphNode * const lNew = aChange->NewNode.Get();
	UEdGraph * const * lPair = lNew ? m_GraphPairs.FindKey(lNew->GetGraph()) : nullptr;
	UEdGraph * const lBefore = lOld ? lOld->GetGraph() : lPair ? *lPair : nullptr;
	UEdGraph * const lAfter = m_GraphPairs.FindRef(lBefore);
	if (!lBefore || !lAfter)
		return;
	if (!m_OldGraph || m_OldGraph->GetCurrentGraph() != lBefore)
		DoShowGraphs(lBefore, lAfter);
	if (lOld)
		m_OldGraph->JumpToNode(lOld);
	if (const TSharedPtr<IMaterialEditor> lEditor = m_Editor.Pin())
	{
		if (lNew && lAfter->Nodes.Contains(lNew))
			lEditor->JumpToHyperlink(lNew);
		else if (!m_LockedGraph.IsValid())
			lEditor->JumpToHyperlink(lAfter);
	}
}

TSharedRef<ITableRow> SMaterialDiffEdit::DoBuildRow(FChange aChange, const TSharedRef<STableViewBase> & aOwner)
{
	return SNew(STableRow<FChange>, aOwner)
	[
		SNew(STextBlock).Text(aChange->Display.DisplayString).ToolTipText(aChange->Display.ToolTip)
		.ColorAndOpacity(aChange->Display.GetDisplayColor()).AutoWrapText(true)
	];
}

#undef LOCTEXT_NAMESPACE
