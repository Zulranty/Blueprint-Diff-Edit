#include "SBlueprintDiffEdit.h"

#include "BlueprintDiffGraphPanel.h"

#include "BlueprintDiffFunction.h"
#include "Async/Async.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorModule.h"
#include "BlueprintEditorTabs.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlot.h"
#include "Components/Widget.h"
#include "DiffUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GraphDiffControl.h"
#include "GraphEditor.h"
#include "IDetailsView.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ISourceControlModule.h"
#include "ISourceControlRevision.h"
#include "ISourceControlState.h"
#include "ISinglePropertyView.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "SMyBlueprint.h"
#include "SourceControlOperations.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"

#define LOCTEXT_NAMESPACE "BlueprintDiffEdit"

struct FBlueprintDiffRevision
{
	TOptional<FBlueprintDiffGitRevision> Git;
	TSharedPtr<ISourceControlRevision> Provider;
};

struct FBlueprintDiffSnapshot
{
	~FBlueprintDiffSnapshot()
	{
		if (!Filename.IsEmpty())
			IFileManager::Get().Delete(*Filename, false, true, true);
	}

	FString Filename;
	FString SourceFilename;
	FBlueprintDiffCommit Commit;
	TSharedPtr<const FBlueprintDiffRevision> Revision;
	FText Error;
};

namespace
{
	const FName DESIGNER_MODE(TEXT("DesignerName"));
	const FName GRAPH_MODE(TEXT("GraphName"));

	TSharedRef<SWidget> BuildValueComparison(const FBlueprintDiffChange & aChange)
	{
		return SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(8.0f)
			[
				SNew(STextBlock).Text(aChange.Label).AutoWrapText(true)
			]
			+ SScrollBox::Slot().Padding(8.0f)
			[
				SNew(STextBlock).Text(FText::Format(LOCTEXT("RevisionSetting", "Revision\n{0}"), aChange.RevisionValue)).AutoWrapText(true)
			]
			+ SScrollBox::Slot().Padding(8.0f)
			[
				SNew(STextBlock).Text(FText::Format(LOCTEXT("WorkingSetting", "Working Blueprint\n{0}"), aChange.WorkingValue)).AutoWrapText(true)
			];
	}

	constexpr TCHAR LAYOUT_SECTION[] = TEXT("BlueprintDiffEdit.Layout");
	constexpr const TCHAR * PANE_SIZE_KEYS[] = {
		TEXT("DiffWidth"), TEXT("FunctionsWidth"), TEXT("RevisionControlsHeight"), TEXT("FunctionsHeight")
	};
	constexpr float DEFAULT_PANE_SIZES[] = { 0.43f, 0.40f, 0.18f, 0.54f };
	TMulticastDelegate<void(int32, float)> OnPaneSizeChanged;

	float GetSavedPaneSize(int32 aSplit)
	{
		float lSaved = 0.0f;
		if (GConfig->GetFloat(LAYOUT_SECTION, PANE_SIZE_KEYS[aSplit], lSaved, GEditorPerProjectIni)
			&& FMath::IsFinite(lSaved) && lSaved > 0.0f && lSaved < 1.0f)
		{
			return lSaved;
		}
		return DEFAULT_PANE_SIZES[aSplit];
	}

	bool IsReadyForDiffFocus(const SGraphPanel & aPanel)
	{
		FVector2f lTopLeft, lBottomRight;
		const FVector2f lSize(aPanel.GetTickSpaceGeometry().GetLocalSize());
		return lSize.X > 0.0f && lSize.Y > 0.0f && !aPanel.HasDeferredZoomDestination()
			&& !aPanel.GetZoomTargetRect(lTopLeft, lBottomRight);
	}

	bool GetDiffFocusAnchor(const SGraphPanel * aPanel, const UEdGraphNode * aNode, FVector2f & aAnchor)
	{
		FVector2f lMin, lMax;
		if (!aPanel || !aNode || !aPanel->GetBoundsForNode(aNode, lMin, lMax) || lMax.Y <= lMin.Y)
			return false;

		aAnchor = FVector2f(lMin.X, (lMin.Y + lMax.Y) * 0.5f);
		return true;
	}

	void FocusDiffPanel(SGraphPanel & aPanel, const FVector2f & aAnchor, float aZoom)
	{
		const FVector2f lSize(aPanel.GetTickSpaceGeometry().GetLocalSize());
		const FVector2f lScreenAnchor = lSize * FVector2f(0.20f, 0.50f);
		aPanel.RestoreViewSettings(aAnchor - lScreenAnchor / aZoom, aZoom);
		aPanel.Invalidate(EInvalidateWidgetReason::Paint);
	}

	bool IsGitProvider(FName aProvider)
	{
		return aProvider == TEXT("Git") || aProvider == TEXT("Git LFS 2");
	}

	FBlueprintDiffCommit DescribeHistoryRevision(const FBlueprintDiffRevision & aRevision)
	{
		if (aRevision.Git.IsSet())
			return aRevision.Git->Commit;

		const ISourceControlRevision & lProvider = *aRevision.Provider;
		return { lProvider.GetRevision().Left(12), BlueprintDiffGit::GetSubject(lProvider.GetDescription()),
			lProvider.GetUserName(), lProvider.GetDate().ToString(TEXT("%Y-%m-%d %H:%M")), {} };
	}

	bool IsSameGitRevision(const FBlueprintDiffGitRevision & aFirst, const FBlueprintDiffGitRevision & aSecond)
	{
		return aFirst.CommitId.Equals(aSecond.CommitId, ESearchCase::CaseSensitive)
			&& aFirst.Filename.Equals(aSecond.Filename, ESearchCase::CaseSensitive)
			&& aFirst.Repository.Equals(aSecond.Repository, ESearchCase::CaseSensitive);
	}

	FGuid GetNodeGuid(const UEdGraph * aGraph, const UEdGraphNode * aNode)
	{
		return aNode && aNode->GetGraph() == aGraph ? aNode->NodeGuid : FGuid();
	}

	UEdGraphNode * FindNode(const UEdGraph * aGraph, const FGuid & aGuid)
	{
		if (aGraph && aGuid.IsValid())
		{
			for (UEdGraphNode * const lNode : aGraph->Nodes)
			{
				if (lNode && lNode->NodeGuid == aGuid)
					return lNode;
			}
		}
		return nullptr;
	}

	bool IsSameRootGraph(const FBlueprintGraphPair & aLeft, const FBlueprintGraphPair & aRight)
	{
		return (aLeft.Revision.IsValid() && aLeft.Revision == aRight.Revision)
			|| (aLeft.Working.IsValid() && aLeft.Working == aRight.Working);
	}

	const FBlueprintGraphPair * FindWorkingGraph(const FBlueprintGraphChange & aChange, const UEdGraph * aGraph)
	{
		if (!aGraph)
			return nullptr;
		if (aChange.Root.Working.Get() == aGraph)
			return &aChange.Root;
		return aChange.ChangedChildren.FindByPredicate([aGraph](const FBlueprintGraphPair & aPair)
		{
			return aPair.Working.Get() == aGraph;
		});
	}
}

void SBlueprintDiffEdit::Construct(const FArguments & aArgs, const TSharedRef<IBlueprintEditor> & aEditor,
	const TSharedRef<SDockTab> & aHostTab, UBlueprint * aBlueprint)
{
	m_Editor = aEditor;
	m_LastFocusedWorkingGraph = aEditor->GetFocusedGraph();
	m_HostTab = aHostTab;
	m_WorkingBlueprint = aBlueprint;
	if (FPackageName::TryConvertLongPackageNameToFilename(aBlueprint->GetOutermost()->GetName(),
		m_Filename, FPackageName::GetAssetPackageExtension()))
		m_Filename = FPaths::ConvertRelativePathToFull(m_Filename);
	else
		m_Filename.Reset();
	m_OriginalContent = aHostTab->GetContent();
	aHostTab->SetContent(SNullWidget::NullWidget);
	m_RevisionCommands = MakeShared<FUICommandList>();
	m_RevisionCommands->MapAction(FGenericCommands::Get().Copy,
		FExecuteAction::CreateSP(this, &SBlueprintDiffEdit::DoCopyRevisionNodes),
		FCanExecuteAction::CreateSP(this, &SBlueprintDiffEdit::CanCopyRevisionNodes));

	const TSharedRef<SWidget> lFunctions =
		SAssignNew(m_GraphList, SListView<FGraphPtr>)
		.Tag("BlueprintDiffEditFunctions")
		.ListItemsSource(&m_Graphs)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SBlueprintDiffEdit::DoBuildGraphRow)
		.OnSelectionChanged(this, &SBlueprintDiffEdit::DoSelectGraph)
		.OnContextMenuOpening(this, &SBlueprintDiffEdit::DoBuildGraphMenu);

	const TSharedRef<SWidget> lRevisionControls =
		SNew(SVerticalBox)
		.Tag("BlueprintDiffEditRevisionControls")
		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &SBlueprintDiffEdit::DoBuildRevisionMenu)
				.IsEnabled_Lambda([this] { return !m_Filename.IsEmpty(); })
				.ToolTipText_Lambda([this] { return m_Commit.GetMenuLabel(true); })
				.ButtonContent()
				[
					SNew(STextBlock)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text_Lambda([this]
					{
						return m_RevisionBlueprint.IsValid() ? m_Commit.GetMenuLabel(false)
							: LOCTEXT("SelectRevision", "Select a revision");
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("CloseDiff", "Close Diff"))
				.OnClicked(this, &SBlueprintDiffEdit::DoClosePanel)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
		[
			SNew(STextBlock)
			.Text_Lambda([this] { return m_Status.IsEmpty() ? m_HistoryStatus : m_Status; })
			.Visibility_Lambda([this] { return m_Status.IsEmpty() && m_HistoryStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SAssignNew(m_ChangeList, SListView<FChangePtr>)
			.ListItemsSource(&m_Changes)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SBlueprintDiffEdit::DoBuildChangeRow)
			.OnSelectionChanged(this, &SBlueprintDiffEdit::DoFocusChange)
			.OnMouseButtonClick(this, &SBlueprintDiffEdit::DoSelectSettingMember)
			.OnContextMenuOpening(this, &SBlueprintDiffEdit::DoBuildSettingMenu)
		];

	constexpr float HANDLE_SIZE = 5.0f;
	constexpr float HANDLE_HIT_SIZE = 10.0f;
	constexpr float MIN_FUNCTIONS_WIDTH = 176.0f;
	constexpr float MIN_REVISION_WIDTH = 224.0f;
	static_assert(UE_ARRAY_COUNT(PANE_SIZE_KEYS) == PaneSplitCount && UE_ARRAY_COUNT(DEFAULT_PANE_SIZES) == PaneSplitCount);
	const float lOuterSize = GetSavedPaneSize(OuterSplit);
	const float lFunctionsSize = GetSavedPaneSize(FunctionsSplit);
	const float lRevisionSize = GetSavedPaneSize(RevisionSplit);
	const float lChangedFilesSize = GetSavedPaneSize(ChangedFilesSplit);

	ChildSlot
	[
		SAssignNew(m_Splitters[OuterSplit], SSplitter)
		.Orientation(Orient_Horizontal)
		.OnSplitterFinishedResizing(this, &SBlueprintDiffEdit::DoSavePaneSize, OuterSplit)
		.PhysicalSplitterHandleSize(HANDLE_SIZE)
		.HitDetectionSplitterHandleSize(HANDLE_HIT_SIZE)
		+ SSplitter::Slot().Value(lOuterSize).MinSize(MIN_FUNCTIONS_WIDTH + MIN_REVISION_WIDTH + HANDLE_SIZE + 8.0f)
		[
			SNew(SBorder)
			.Padding(4.0f)
			[
				SAssignNew(m_Splitters[FunctionsSplit], SSplitter)
				.Orientation(Orient_Horizontal)
				.OnSplitterFinishedResizing(this, &SBlueprintDiffEdit::DoSavePaneSize, FunctionsSplit)
				.PhysicalSplitterHandleSize(HANDLE_SIZE)
				.HitDetectionSplitterHandleSize(HANDLE_HIT_SIZE)
				+ SSplitter::Slot().Value(lFunctionsSize).MinSize(MIN_FUNCTIONS_WIDTH)
				[
					SAssignNew(m_Splitters[ChangedFilesSplit], SSplitter)
					.Orientation(Orient_Vertical)
					.OnSplitterFinishedResizing(this, &SBlueprintDiffEdit::DoSavePaneSize, ChangedFilesSplit)
					.PhysicalSplitterHandleSize(HANDLE_SIZE)
					.HitDetectionSplitterHandleSize(HANDLE_HIT_SIZE)
					+ SSplitter::Slot().Value(lChangedFilesSize).MinSize(64.0f)
					[
						lFunctions
					]
					+ SSplitter::Slot().Value(1.0f - lChangedFilesSize).MinSize(128.0f)
					[
						SNew(SBlueprintDiffChanges)
						.Compact(true)
						.ContextFilename(m_Filename.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()) : m_Filename)
						.OnOpenBlueprint(aArgs._OnOpenBlueprint)
						.OnBeforeReload(aArgs._OnBeforeReload)
					]
				]
				+ SSplitter::Slot().Value(1.0f - lFunctionsSize).MinSize(MIN_REVISION_WIDTH)
				[
					SAssignNew(m_Splitters[RevisionSplit], SSplitter)
					.Orientation(Orient_Vertical)
					.OnSplitterFinishedResizing(this, &SBlueprintDiffEdit::DoSavePaneSize, RevisionSplit)
					.PhysicalSplitterHandleSize(HANDLE_SIZE)
					.HitDetectionSplitterHandleSize(HANDLE_HIT_SIZE)
					+ SSplitter::Slot().Value(lRevisionSize).MinSize(72.0f)
					[
						lRevisionControls
					]
					+ SSplitter::Slot().Value(1.0f - lRevisionSize).MinSize(110.0f)
					[
						SAssignNew(m_RevisionGraphBox, SBox)
						.Tag("BlueprintDiffEditBaselinePreview")
					]
				]
			]
		]
		+ SSplitter::Slot().Value(1.0f - lOuterSize).MinSize(320.0f)
		[
			SAssignNew(m_WorkingEditorBox, SBox)
			[
				m_OriginalContent.ToSharedRef()
			]
		]
	];

	OnPaneSizeChanged.AddSP(this, &SBlueprintDiffEdit::DoApplyPaneSize);
	aBlueprint->OnChanged().AddSP(this, &SBlueprintDiffEdit::DoOnBlueprintChanged);
	aBlueprint->OnCompiled().AddSP(this, &SBlueprintDiffEdit::DoOnBlueprintChanged);
	FCoreUObjectDelegates::OnObjectModified.AddSP(this, &SBlueprintDiffEdit::DoOnObjectModified);
	FCoreUObjectDelegates::OnObjectTransacted.AddSP(this, &SBlueprintDiffEdit::DoOnObjectTransacted);
	DoBuildRevisionGraph(nullptr);
	if (!m_Filename.IsEmpty())
		DoUpdateHistory();
	else
		m_Status = LOCTEXT("NoPackageFile", "The working Blueprint does not have a source-controlled package path.");
}

SBlueprintDiffEdit::~SBlueprintDiffEdit()
{
	DoCancelGitHistory();
	DoCancelRevision();
	DoCancelPendingFocus();
	OnPaneSizeChanged.RemoveAll(this);
	DoSetGraphLock(nullptr);
	DoSetWorkingGraphEditor(nullptr);
	FCoreUObjectDelegates::OnObjectModified.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectTransacted.RemoveAll(this);
	if (UBlueprint * const lBlueprint = m_WorkingBlueprint.Get())
	{
		lBlueprint->OnChanged().RemoveAll(this);
		lBlueprint->OnCompiled().RemoveAll(this);
	}
	if (UEdGraph * const lGraph = m_WatchedWorkingGraph.Get())
		lGraph->RemoveOnGraphChangedHandler(m_GraphChangedHandle);
}

bool SBlueprintDiffEdit::IsHostedBy(const SDockTab * aTab) const
{
	return m_HostTab.Pin().Get() == aTab;
}

bool SBlueprintDiffEdit::IsEditingAnyFile(const TArray<FString> & aFiles) const
{
	return aFiles.ContainsByPredicate([this](const FString & aFile)
	{
		return FPaths::IsSamePath(m_Filename, aFile);
	});
}

void SBlueprintDiffEdit::Close()
{
	const TSharedRef<SWidget> lKeepAlive = AsShared();
	++m_HistoryGeneration;
	DoCancelGitHistory();
	DoCancelRevision();
	DoCancelPendingFocus();
	DoSetGraphLock(nullptr);
	DoSetWorkingGraphEditor(nullptr);
	const TSharedPtr<SDockTab> lTab = m_HostTab.Pin();
	if (lTab && &lTab->GetContent().Get() == this)
	{
		m_WorkingEditorBox->SetContent(SNullWidget::NullWidget);
		lTab->SetContent(m_OriginalContent.ToSharedRef());
	}
	m_HostTab.Reset();
}

FReply SBlueprintDiffEdit::DoClosePanel()
{
	Close();
	return FReply::Handled();
}

void SBlueprintDiffEdit::DoUpdateHistory(bool aForce)
{
	if (m_Filename.IsEmpty())
		return;

	ISourceControlModule & lSourceControl = ISourceControlModule::Get();
	ISourceControlProvider & lProvider = lSourceControl.GetProvider();
	const FName lName = lProvider.GetName();
	const bool lAvailable = lSourceControl.IsEnabled() && lProvider.IsAvailable();
	if (!aForce && lName == m_HistoryProvider && lAvailable == m_HistoryAvailable && m_HistoryGeneration != 0)
		return;

	++m_HistoryGeneration;
	DoCancelGitHistory();
	m_HistoryProvider = lName;
	m_HistoryAvailable = lAvailable;
	m_Revisions.Reset();
	if (!aForce && m_RevisionRequest.IsValid() && !m_RequestingMaster)
	{
		DoCancelRevision();
		m_Status = FText::GetEmpty();
		m_RevisionWasRequested = m_RevisionBlueprint.IsValid();
	}
	if (!lAvailable)
	{
		m_HistoryStatus = LOCTEXT("SourceControlUnavailable", "Connect Revision Control to load commit history. Git master is available from the revision menu.");
		return;
	}

	m_HistoryStatus = LOCTEXT("LoadingHistory", "Loading revision history…");
	if (IsGitProvider(lName))
		DoStartGitHistory();
	else
		DoStartProviderHistory();
}

void SBlueprintDiffEdit::DoStartGitHistory()
{
	TPromise<TOptional<FBlueprintDiffGitRevision>> lLatest;
	m_LatestGitRevision = lLatest.GetFuture();
	m_CancelGitHistory = MakeShared<TAtomic<bool>>(false);
	m_GitHistoryRequest = Async(EAsyncExecution::ThreadPool,
		[lExecutable = BlueprintDiffGit::GetExecutable(), lFilename = m_Filename,
			lCancellation = m_CancelGitHistory, lLatest = MoveTemp(lLatest), lPreload = !m_RevisionWasRequested]() mutable
		{
			return BlueprintDiffGit::ReadFileHistory(lExecutable, lFilename,
				[&lLatest](TOptional<FBlueprintDiffGitRevision> aRevision) { lLatest.SetValue(MoveTemp(aRevision)); },
				lCancellation.Get(), lPreload);
		});
}

void SBlueprintDiffEdit::DoCancelGitHistory()
{
	if (m_CancelGitHistory)
		m_CancelGitHistory->Store(true);
	m_CancelGitHistory.Reset();
	m_LatestGitRevision.Reset();
	m_GitHistoryRequest.Reset();
}

void SBlueprintDiffEdit::DoProcessGitHistory()
{
	if (m_LatestGitRevision.IsReady())
	{
		TOptional<FBlueprintDiffGitRevision> lLatest = m_LatestGitRevision.Consume();
		if (lLatest.IsSet())
		{
			const FRevisionPtr lRevision = MakeShared<FBlueprintDiffRevision>(
				FBlueprintDiffRevision{ MoveTemp(lLatest), nullptr });
			m_Revisions.Add(lRevision);
			if (!m_RevisionWasRequested)
				DoRequestRevision(lRevision);
		}
	}

	if (!m_GitHistoryRequest.IsReady())
		return;

	FBlueprintDiffFileHistory lHistory = m_GitHistoryRequest.Consume();
	m_CancelGitHistory.Reset();
	if (lHistory.bUseProvider)
	{
		if (!lHistory.Error.IsEmpty())
			UE_LOG(LogTemp, Verbose, TEXT("Blueprint Diff is using provider history: %s"), *lHistory.Error.ToString());
		DoStartProviderHistory();
		return;
	}

	TArray<FRevisionPtr> lRevisions;
	lRevisions.Reserve(lHistory.Revisions.Num());
	for (FBlueprintDiffGitRevision & lGit : lHistory.Revisions)
	{
		lRevisions.Add(MakeShared<FBlueprintDiffRevision>(
			FBlueprintDiffRevision{ MoveTemp(lGit), nullptr }));
	}
	DoSetHistory(MoveTemp(lRevisions));
}

void SBlueprintDiffEdit::DoSetHistory(TArray<FRevisionPtr> aRevisions)
{
	m_Revisions = MoveTemp(aRevisions);
	m_HistoryStatus = m_Revisions.IsEmpty() ? LOCTEXT("NoHistory", "This Blueprint has no revisions in the connected source-control provider.")
		: FText::GetEmpty();
	if (!m_RevisionWasRequested && !m_Revisions.IsEmpty())
		DoRequestRevision(m_Revisions[0]);
}

void SBlueprintDiffEdit::DoStartProviderHistory()
{
	ISourceControlProvider & lProvider = ISourceControlModule::Get().GetProvider();
	const TSharedRef<FUpdateStatus> lOperation = ISourceControlOperation::Create<FUpdateStatus>();
	lOperation->SetUpdateHistory(true);
	if (lProvider.Execute(lOperation, m_Filename, EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateSP(this, &SBlueprintDiffEdit::DoReceiveHistory, lProvider.GetName(), m_HistoryGeneration))
		!= ECommandResult::Succeeded)
	{
		m_HistoryStatus = LOCTEXT("HistoryFailed", "Unable to load revision history for this Blueprint.");
	}
}

void SBlueprintDiffEdit::DoReceiveHistory(const FSourceControlOperationRef &, ECommandResult::Type aResult,
	FName aProvider, uint32 aHistoryGeneration)
{
	ISourceControlProvider & lProvider = ISourceControlModule::Get().GetProvider();
	if (aHistoryGeneration != m_HistoryGeneration || lProvider.GetName() != aProvider || !lProvider.IsAvailable())
		return;
	if (aResult != ECommandResult::Succeeded)
	{
		m_HistoryStatus = LOCTEXT("HistoryFailed", "Unable to load revision history for this Blueprint.");
		return;
	}

	TArray<FRevisionPtr> lRevisions;
	const FSourceControlStatePtr lState = lProvider.GetState(m_Filename, EStateCacheUsage::Use);
	if (lState)
	{
		for (int lIndex = 0; lIndex < lState->GetHistorySize(); ++lIndex)
		{
			if (TSharedPtr<ISourceControlRevision> lRevision = lState->GetHistoryItem(lIndex))
			{
				lRevisions.Add(MakeShared<FBlueprintDiffRevision>(
					FBlueprintDiffRevision{ {}, MoveTemp(lRevision) }));
			}
		}
	}
	DoSetHistory(MoveTemp(lRevisions));
}

TSharedRef<SWidget> SBlueprintDiffEdit::DoBuildRevisionMenu()
{
	FMenuBuilder lMenu(true, nullptr);
	lMenu.AddMenuEntry(LOCTEXT("CompareMaster", "Compare with default branch"),
		LOCTEXT("CompareMasterTip", "Compare the working Blueprint with origin/master, or local master if origin/master is absent."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffEdit::DoStartMasterComparison)));
	if (m_HistoryAvailable)
	{
		lMenu.AddMenuEntry(LOCTEXT("RefreshHistory", "Refresh Revision History"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffEdit::DoUpdateHistory, true)));
	}
	if (!m_Revisions.IsEmpty())
		lMenu.AddMenuSeparator();

	for (const FRevisionPtr & lRevision : m_Revisions)
	{
		const FBlueprintDiffCommit lCommit = DescribeHistoryRevision(*lRevision);
		lMenu.AddMenuEntry(lCommit.GetMenuLabel(true),
			FText::FromString(lRevision->Git ? lRevision->Git->Description : lRevision->Provider->GetDescription()), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffEdit::DoLoadRevision, lRevision)));
	}
	return lMenu.MakeWidget();
}

void SBlueprintDiffEdit::DoLoadRevision(FRevisionPtr aRevision)
{
	DoUpdateHistory();
	const FRevisionPtr * const lRevision = m_Revisions.FindByPredicate([&aRevision](const FRevisionPtr & aEntry)
	{
		return aEntry == aRevision || (aEntry->Git && aRevision->Git && IsSameGitRevision(*aEntry->Git, *aRevision->Git));
	});
	if (lRevision)
		DoRequestRevision(*lRevision);
}

void SBlueprintDiffEdit::DoStartMasterComparison()
{
	DoRequestRevision(nullptr);
}

void SBlueprintDiffEdit::DoCancelRevision()
{
	if (m_CancelRevision)
		m_CancelRevision->Store(true);
	m_CancelRevision.Reset();
	m_RevisionRequest.Reset();
}

void SBlueprintDiffEdit::DoRequestRevision(FRevisionPtr aRevision)
{
	if (!m_WorkingBlueprint.IsValid())
		return;
	DoCancelRevision();
	m_RevisionWasRequested = true;
	m_RequestingMaster = !aRevision.IsValid();
	if (aRevision && (m_Revision == aRevision || (m_Revision && aRevision->Git && m_Revision->Git
		&& IsSameGitRevision(*aRevision->Git, *m_Revision->Git))))
	{
		m_Status = FText::GetEmpty();
		return;
	}

	m_Status = m_RequestingMaster ? LOCTEXT("LoadingMaster", "Reading the Blueprint from master…")
		: LOCTEXT("LoadingRevision", "Reading the selected Blueprint revision…");
	m_CancelRevision = MakeShared<TAtomic<bool>>(false);
	const bool lUseGit = m_RequestingMaster || IsGitProvider(m_HistoryProvider) || (aRevision && aRevision->Git.IsSet());
	const TSharedPtr<ISourceControlRevision> lProviderRevision = aRevision ? aRevision->Provider : nullptr;
	const FString lRevisionFilename = lProviderRevision ? lProviderRevision->GetFilename() : FString();
	const FString lRevisionId = lProviderRevision ? lProviderRevision->GetRevision() : FString();
	const FBlueprintDiffCommit lCommit = aRevision ? DescribeHistoryRevision(*aRevision) : FBlueprintDiffCommit();
	const FString lDirectory = FPaths::ConvertRelativePathToFull(FPaths::DiffDir());
	m_RevisionRequest = Async(EAsyncExecution::ThreadPool,
		[lExecutable = lUseGit ? BlueprintDiffGit::GetExecutable() : FString(), lWorking = m_Filename,
			lRevisionFilename, lRevisionId, lCommit, lDirectory, lUseGit, lProviderRevision,
			lCancelled = m_CancelRevision, lRevision = MoveTemp(aRevision)]
		{
			const TSharedPtr<FBlueprintDiffSnapshot> lSnapshot = MakeShared<FBlueprintDiffSnapshot>();
			if (lCancelled->Load())
				return lSnapshot;

			lSnapshot->Commit = lCommit;
			lSnapshot->Revision = lRevision;
			lSnapshot->SourceFilename = lWorking;
			IFileManager::Get().MakeDirectory(*lDirectory, true);
			lSnapshot->Filename = FPaths::CreateTempFilename(*lDirectory, TEXT("temp-blueprint-diff-edit-revision-"), TEXT(".uasset"));
			if (lUseGit)
			{
				FBlueprintDiffMaster lResult;
				if (!lRevision)
					lResult = BlueprintDiffGit::ReadMaster(lExecutable, lWorking, lCancelled.Get(), &lSnapshot->Filename);
				else if (lRevision->Git)
					lResult = BlueprintDiffGit::ReadHistoryEntry(lExecutable, *lRevision->Git, lCancelled.Get(), &lSnapshot->Filename);
				else
					lResult = BlueprintDiffGit::ReadHistory(lExecutable, lWorking, lRevisionFilename, lRevisionId, lCancelled.Get(), &lSnapshot->Filename);
				if (!lResult.Error.IsEmpty())
					lSnapshot->Error = MoveTemp(lResult.Error);
				else if (!lCancelled->Load())
				{
					if (!lRevision)
						lSnapshot->Commit = MoveTemp(lResult.Commit);
					lSnapshot->SourceFilename = MoveTemp(lResult.SourceFilename);
				}
			}
			else
			{
				FString lOutput = lSnapshot->Filename;
				if (!lProviderRevision->Get(lOutput, EConcurrency::Asynchronous) || lOutput != lSnapshot->Filename)
					lSnapshot->Error = LOCTEXT("ProviderRevisionFailed", "Revision Control could not write the requested Blueprint revision to its temporary file.");
				if (!FPaths::IsRelative(lRevisionFilename))
					lSnapshot->SourceFilename = lRevisionFilename;
			}
			return lSnapshot;
		});
}

void SBlueprintDiffEdit::DoFinishRevision()
{
	const TSharedPtr<FBlueprintDiffSnapshot> lSnapshot = m_RevisionRequest.Consume();
	m_CancelRevision.Reset();
	if (!m_WorkingBlueprint.IsValid())
		return;
	if (!lSnapshot->Error.IsEmpty())
	{
		m_Status = lSnapshot->Error;
		return;
	}

	const FPackagePath lTempPath = FPackagePath::FromLocalPath(lSnapshot->Filename);
	if (!FPackageName::IsTempPackage(lTempPath.GetPackageName()))
	{
		m_Status = LOCTEXT("InvalidSnapshotPath", "The revision could not be opened as a temporary Blueprint package.");
		return;
	}
	UPackage * const lPackage = DiffUtils::LoadPackageForDiff(lTempPath,
		FPackagePath::FromLocalPath(lSnapshot->SourceFilename));
	if (lPackage)
	{
		lPackage->FullyLoad();
		ResetLoaders(lPackage);
	}
	DoApplyRevision(lPackage, lSnapshot->SourceFilename, lSnapshot->Commit, lSnapshot->Revision);
}

void SBlueprintDiffEdit::DoApplyRevision(UPackage * aRevisionPackage, const FString & aOriginalFilename,
	const FBlueprintDiffCommit & aCommit, FRevisionPtr aHistoryRevision)
{
	UBlueprint * const lWorkingBlueprint = m_WorkingBlueprint.Get();
	UBlueprint * lRevisionBlueprint = nullptr;
	if (aRevisionPackage)
	{
		lRevisionBlueprint = FindObject<UBlueprint>(aRevisionPackage, *lWorkingBlueprint->GetName());
		if (!lRevisionBlueprint)
			lRevisionBlueprint = FindObject<UBlueprint>(aRevisionPackage, *FPaths::GetBaseFilename(aOriginalFilename));
	}
	if (!lRevisionBlueprint || aRevisionPackage == lWorkingBlueprint->GetOutermost()
		|| lRevisionBlueprint->GetClass() != lWorkingBlueprint->GetClass())
	{
		m_Status = LOCTEXT("InvalidRevision", "This revision could not be loaded as a Blueprint of the same type.");
		return;
	}

	DoCancelPendingFocus();
	DoSetWorkingGraphEditor(nullptr);
	DoBuildRevisionGraph(nullptr);
	m_NodeHighlights->Reset();
	m_RevisionWidgetPreview.Reset();
	m_RevisionWidgetTree.Reset();
	m_RevisionSlotTrees.Reset();
	m_Revision = MoveTemp(aHistoryRevision);
	m_Commit = aCommit;
	m_RevisionBlueprint.Reset(lRevisionBlueprint);
	if (!lRevisionBlueprint->SkeletonGeneratedClass)
		FKismetEditorUtilities::GenerateBlueprintSkeleton(lRevisionBlueprint);

	m_Status = FText::GetEmpty();
	DoRebuildGraphList();
}

void SBlueprintDiffEdit::DoRebuildGraphList()
{
	const UBlueprint * const lWorkingBlueprint = m_WorkingBlueprint.Get();
	const UBlueprint * const lRevisionBlueprint = m_RevisionBlueprint.Get();
	if (!lWorkingBlueprint || !lRevisionBlueprint)
		return;

	const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin();
	const UEdGraph * const lFocusedGraph = lEditor && lEditor->GetCurrentMode() != DESIGNER_MODE ? lEditor->GetFocusedGraph() : nullptr;
	// Reuse the comparison across the graph list, change rows and outlines during this refresh.
	FBlueprintGraphDiffs lDiffs;
	const TGuardValue<FBlueprintGraphDiffs *> lCache(m_CurrentGraphDiffs, &lDiffs);
	TArray<FGraphPtr> lNewList;
	for (FBlueprintGraphChange & lChange : BlueprintDiffModel::FindChangedGraphs(lRevisionBlueprint, lWorkingBlueprint, &lDiffs))
	{
		const UEdGraph * const lOld = lChange.Root.Revision.Get();
		const UEdGraph * const lNew = lChange.Root.Working.Get();
		FGraphPtr lRow = MakeShared<FBlueprintDiffGraph>();
		if (!lNew)
			lRow->Label = FText::Format(LOCTEXT("RemovedGraph", "{0} (removed)"), FText::FromName(lOld->GetFName()));
		else if (!lOld)
			lRow->Label = FText::Format(LOCTEXT("AddedGraphRow", "{0} (added)"), FText::FromName(lNew->GetFName()));
		else if (lOld->GetFName() != lNew->GetFName())
			lRow->Label = FText::Format(LOCTEXT("RenamedGraph", "{0} → {1}"),
				FText::FromName(lOld->GetFName()), FText::FromName(lNew->GetFName()));
		else
			lRow->Label = FText::FromName(lNew->GetFName());

		lRow->Root = lChange.Root;
		lRow->HasRootChanges = lChange.HasRootChanges;
		lRow->ChangedChildren = MoveTemp(lChange.ChangedChildren);
		lNewList.Add(MoveTemp(lRow));
	}

	lNewList.Sort([](const FGraphPtr & aLeft, const FGraphPtr & aRight)
	{
		return aLeft->Label.CompareTo(aRight->Label) < 0;
	});

	m_Settings = BlueprintDiffModel::FindChangedSettings(lRevisionBlueprint, lWorkingBlueprint);
	if (!m_Settings.IsEmpty())
	{
		FGraphPtr lSettings = MakeShared<FBlueprintDiffGraph>();
		lSettings->Section = EBlueprintDiffSection::Settings;
		lSettings->Label = LOCTEXT("BlueprintSettings", "Blueprint Settings");
		lNewList.Insert(MoveTemp(lSettings), 0);
	}

	m_Widgets.Reset();
	const UWidgetBlueprint * const lRevisionWidget = Cast<UWidgetBlueprint>(lRevisionBlueprint);
	const UWidgetBlueprint * const lWorkingWidget = Cast<UWidgetBlueprint>(lWorkingBlueprint);
	if (lRevisionWidget && lWorkingWidget)
	{
		m_Widgets = BlueprintDiffModel::FindChangedWidgets(lRevisionWidget, lWorkingWidget);
		if (!m_Widgets.IsEmpty())
		{
			FGraphPtr lWidgets = MakeShared<FBlueprintDiffGraph>();
			lWidgets->Section = EBlueprintDiffSection::Widgets;
			lWidgets->Label = LOCTEXT("WidgetChanges", "Widgets");
			lNewList.Insert(MoveTemp(lWidgets), 0);
		}
	}

	FGraphPtr lSelection;
	for (const FGraphPtr & lRow : lNewList)
	{
		if (m_SelectedGraph && ((lRow->Section != EBlueprintDiffSection::Graph && lRow->Section == m_SelectedGraph->Section)
			|| IsSameRootGraph(lRow->Root, m_SelectedGraph->Root)))
		{
			lSelection = lRow;
			break;
		}
	}
	if (!lSelection && lFocusedGraph)
	{
		for (const FGraphPtr & lRow : lNewList)
		{
			if (FindWorkingGraph(*lRow, lFocusedGraph))
			{
				lSelection = lRow;
				break;
			}
		}
	}

	const bool lShouldOpenFirst = !lSelection && !m_SelectedGraph && m_Graphs.IsEmpty() && !lNewList.IsEmpty();
	if (lShouldOpenFirst)
		lSelection = lNewList[0];

	m_Graphs = MoveTemp(lNewList);
	{
		const TGuardValue<bool> lGuard(m_RebuildingList, true);
		if (lSelection)
			m_GraphList->SetSelection(lSelection, ESelectInfo::Direct);
		else
			m_GraphList->ClearSelection();
		m_GraphList->RequestListRefresh();
	}
	DoSelectGraph(lSelection, lShouldOpenFirst ? ESelectInfo::OnMouseClick : ESelectInfo::Direct);
}

void SBlueprintDiffEdit::DoBuildRevisionGraph(UEdGraph * aGraph)
{
	DoSetGraphLock(nullptr);
	m_RevisionGraphEditor.Reset();
	m_WidgetValuesBox.Reset();
	if (!aGraph)
	{
		const FText lText = !m_RevisionBlueprint.IsValid() ? LOCTEXT("SelectRevisionPlaceholder", "Select a revision to compare.")
			: m_Graphs.IsEmpty() ? LOCTEXT("NoDifferences", "No graph or Blueprint settings differences.")
			: m_SelectedGraph ? LOCTEXT("RevisionGraphMissing", "No graph in the selected revision.")
			: LOCTEXT("SelectDifference", "Select a changed graph or Blueprint Settings.");
		m_RevisionGraphBox->SetContent(SNew(STextBlock)
			.Text(lText)
			.Justification(ETextJustify::Center));
		return;
	}

	SGraphEditor::FGraphEditorEvents lEvents;
	lEvents.OnCreateNodeOrPinMenu = SGraphEditor::FOnCreateNodeOrPinMenu::CreateLambda(
		[](UEdGraph * aGraph, const UEdGraphNode * aNode, const UEdGraphPin * aPin, FMenuBuilder * aMenu, bool aIsDebugging)
		{
			aMenu->AddMenuEntry(FGenericCommands::Get().Copy);
			return FActionMenuContent(aMenu->MakeWidget());
		});

	m_RevisionGraphEditor = SNew(SGraphEditor)
		.GraphToEdit(aGraph)
		.DiffResults(m_NodeHighlights)
		.IsEditable(false)
		.GraphEvents(lEvents)
		.AdditionalCommands(m_RevisionCommands);
	m_RevisionGraphBox->SetContent(m_RevisionGraphEditor.ToSharedRef());
}

void SBlueprintDiffEdit::DoSelectGraph(FGraphPtr aGraph, ESelectInfo::Type aSelectInfo)
{
	if (m_RebuildingList)
		return;

	const bool lSameRootGraph = aGraph && m_SelectedGraph && IsSameRootGraph(aGraph->Root, m_SelectedGraph->Root);
	m_SelectedGraph = aGraph;
	if (!aGraph || aGraph->Section != EBlueprintDiffSection::Settings)
	{
		m_ActiveSetting.Reset();
		m_SettingComparison.Reset();
		m_SingleSettingEditor.Reset();
		m_SettingObject.Reset();
	}
	FBlueprintGraphPair lGraphs;
	if (aGraph && aGraph->Section == EBlueprintDiffSection::Graph)
	{
		lGraphs = aGraph->Root;
		if (lSameRootGraph && aGraph->ChangedChildren.Contains(m_DisplayedGraphs))
			lGraphs = m_DisplayedGraphs;
		else if (!aGraph->HasRootChanges && !aGraph->ChangedChildren.IsEmpty())
			lGraphs = aGraph->ChangedChildren[0];

		if (aSelectInfo == ESelectInfo::Direct)
		{
			if (const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin())
			{
				if (const FBlueprintGraphPair * const lFocusedPair = FindWorkingGraph(*aGraph, lEditor->GetFocusedGraph()))
					lGraphs = *lFocusedPair;
			}
		}
	}

	FBlueprintGraphDiffs lDiffs;
	const TGuardValue<FBlueprintGraphDiffs *> lCache(m_CurrentGraphDiffs, m_CurrentGraphDiffs ? m_CurrentGraphDiffs : &lDiffs);
	const bool lChangedPair = lGraphs != m_DisplayedGraphs;
	DoShowGraphPair(lGraphs, aSelectInfo != ESelectInfo::Direct);
	DoRefreshChanges();
	if (aGraph && aGraph->Section == EBlueprintDiffSection::Widgets && aSelectInfo != ESelectInfo::Direct
		&& !m_Changes.IsEmpty())
	{
		DoShowWidgetChange(*m_Changes[0], true);
	}
	if (lChangedPair)
		DoFocusFirstChange();
}

void SBlueprintDiffEdit::DoShowGraphPair(const FBlueprintGraphPair & aPair, bool aOpenWorking)
{
	const bool lChangedPair = aPair != m_DisplayedGraphs;
	if (lChangedPair)
		DoCancelPendingFocus();
	m_DisplayedGraphs = aPair;
	UEdGraph * const lOldGraph = aPair.Revision.Get();
	UEdGraph * const lWorkingGraph = aPair.Working.Get();
	const bool lShowingWidgetPreview = m_SelectedGraph
		&& m_SelectedGraph->Section == EBlueprintDiffSection::Widgets && m_WidgetValuesBox.IsValid();
	const bool lShowingSettings = m_SelectedGraph
		&& m_SelectedGraph->Section == EBlueprintDiffSection::Settings && m_SettingComparison.IsValid();
	if (!lShowingSettings && !lShowingWidgetPreview && (!lOldGraph || !m_RevisionGraphEditor || m_RevisionGraphEditor->GetCurrentGraph() != lOldGraph))
	{
		m_NodeHighlights->Reset();
		DoBuildRevisionGraph(lOldGraph);
	}

	if (m_WatchedWorkingGraph.Get() != lWorkingGraph)
	{
		if (UEdGraph * const lPrevious = m_WatchedWorkingGraph.Get())
			lPrevious->RemoveOnGraphChangedHandler(m_GraphChangedHandle);
		m_WatchedWorkingGraph = lWorkingGraph;
		if (lWorkingGraph)
			m_GraphChangedHandle = lWorkingGraph->AddOnGraphChangedHandler(
				FOnGraphChanged::FDelegate::CreateSP(this, &SBlueprintDiffEdit::DoOnGraphChanged));
	}

	TSharedPtr<SGraphEditor> lWorkingEditor;
	if (lWorkingGraph)
	{
		if (const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin())
		{
			UEdGraph * const lFocused = lEditor->GetFocusedGraph();
			if (aOpenWorking || lFocused == lWorkingGraph)
			{
				lWorkingEditor = m_WorkingGraphEditor.Pin();
				if (!lWorkingEditor || lWorkingEditor->GetCurrentGraph() != lWorkingGraph || lFocused != lWorkingGraph)
				{
					lWorkingEditor = lEditor->OpenGraphAndBringToFront(lWorkingGraph, false);
					m_LastFocusedWorkingGraph = lEditor->GetFocusedGraph();
				}
			}
		}
	}
	const bool lChangedEditor = m_WorkingGraphEditor.Pin() != lWorkingEditor;
	DoSetWorkingGraphEditor(lWorkingEditor);
	DoSetGraphLock(lWorkingEditor);
	if ((lChangedPair || lChangedEditor) && !m_CurrentGraphDiffs)
		DoRefreshGraphHighlights();
}

void SBlueprintDiffEdit::DoSetGraphLock(const TSharedPtr<SGraphEditor> & aWorking)
{
	const TSharedPtr<SGraphEditor> lPrevious = m_LockedWorkingEditor.Pin();
	if (lPrevious == aWorking)
		return;

	if (lPrevious && m_RevisionGraphEditor)
	{
		m_RevisionGraphEditor->UnlockFromGraphEditor(lPrevious);
		lPrevious->UnlockFromGraphEditor(m_RevisionGraphEditor);
	}

	m_LockedWorkingEditor.Reset();
	if (aWorking && m_RevisionGraphEditor)
	{
		FVector2f lPosition = FVector2f::ZeroVector;
		float lZoom = 1.0f;
		aWorking->GetViewLocation(lPosition, lZoom);
		m_RevisionGraphEditor->SetViewLocation(lPosition, lZoom);
		m_RevisionGraphEditor->LockToGraphEditor(aWorking);
		aWorking->LockToGraphEditor(m_RevisionGraphEditor);
		m_LockedWorkingEditor = aWorking;
	}
}

void SBlueprintDiffEdit::DoSetWorkingGraphEditor(const TSharedPtr<SGraphEditor> & aWorking)
{
	const TSharedPtr<SGraphEditor> lPrevious = m_WorkingGraphEditor.Pin();
	if (lPrevious == aWorking)
		return;

	if (SGraphPanel * const lPanel = lPrevious ? lPrevious->GetGraphPanel() : nullptr)
	{
		TSharedPtr<TArray<FDiffSingleResult>> & lResults = BlueprintDiffGraphPanel::GetResults(*lPanel);
		if (lResults == m_NodeHighlights)
		{
			lResults.Reset();
			lPanel->Invalidate(EInvalidateWidgetReason::Paint);
		}
	}
	m_WorkingGraphEditor = aWorking;
}

void SBlueprintDiffEdit::DoRefreshGraphHighlights()
{
	m_NodeHighlights->Reset();
	UEdGraph * const lOldGraph = m_DisplayedGraphs.Revision.Get();
	UEdGraph * const lWorkingGraph = m_DisplayedGraphs.Working.Get();
	if (lWorkingGraph)
	{
		if (lOldGraph)
		{
			FBlueprintGraphDiffs lDiffs;
			const auto & lResults = BlueprintDiffModel::GetGraphDifferences(
				m_DisplayedGraphs, m_CurrentGraphDiffs ? *m_CurrentGraphDiffs : lDiffs);
			for (const FBlueprintNodeDifference & lDifference : lResults)
			{
				UEdGraphNode * const lWorkingNode = lDifference.WorkingNode.Get();
				UEdGraphNode * const lOldNode = lDifference.RevisionNode.Get();
				if (!lWorkingNode && !lOldNode)
					continue;

				FDiffSingleResult & lOutline = m_NodeHighlights->Add_GetRef(lDifference.Display);
				lOutline.Node1 = lWorkingNode ? lWorkingNode : lOldNode;
				lOutline.Node2 = lWorkingNode ? lOldNode : nullptr;
			}
		}
		else
		{
			FGraphDiffControl::FNodeDiffContext lContext;
			lContext.NodeTypeDisplayName = LOCTEXT("AddedGraphNodeType", "Node");
			for (UEdGraphNode * const lNode : lWorkingGraph->Nodes)
			{
				if (!lNode || !lNode->NodeGuid.IsValid())
					continue;
				FGraphDiffControl::FNodeMatch lMatch;
				lMatch.NewNode = lNode;
				lMatch.Diff(lContext, m_NodeHighlights.Get());
			}
		}
	}

	if (SGraphPanel * const lPanel = m_RevisionGraphEditor ? m_RevisionGraphEditor->GetGraphPanel() : nullptr)
		lPanel->Invalidate(EInvalidateWidgetReason::Paint);

	const TSharedPtr<SGraphEditor> lEditor = m_WorkingGraphEditor.Pin();
	if (SGraphPanel * const lPanel = lEditor ? lEditor->GetGraphPanel() : nullptr)
	{
		TSharedPtr<TArray<FDiffSingleResult>> & lResults = BlueprintDiffGraphPanel::GetResults(*lPanel);
		if (!lResults || lResults == m_NodeHighlights)
		{
			lResults = m_NodeHighlights->IsEmpty() ? nullptr : m_NodeHighlights;
			lPanel->Invalidate(EInvalidateWidgetReason::Paint);
		}
	}
}

void SBlueprintDiffEdit::DoRefreshChanges()
{
	FChangePtr lPrevious;
	if (m_SelectedGraph && m_SelectedGraph->Section != EBlueprintDiffSection::Graph)
	{
		const TArray<FChangePtr> lSelected = m_ChangeList->GetSelectedItems();
		if (!lSelected.IsEmpty())
			lPrevious = lSelected[0];
	}
	m_Changes.Reset();
	if (m_SelectedGraph && m_SelectedGraph->Section == EBlueprintDiffSection::Settings)
	{
		for (const FBlueprintSettingChange & lChange : m_Settings)
		{
			m_Changes.Add(MakeShared<FBlueprintDiffChange>(FBlueprintDiffChange{
				{}, {}, lChange.Label, FSlateColor::UseForeground(), {}, lChange.RevisionValue, lChange.WorkingValue,
				NAME_None, lChange.Target }));
		}
	}
	else if (m_SelectedGraph && m_SelectedGraph->Section == EBlueprintDiffSection::Widgets)
	{
		for (const FBlueprintWidgetChange & lChange : m_Widgets)
		{
			m_Changes.Add(MakeShared<FBlueprintDiffChange>(FBlueprintDiffChange{
				{}, {}, lChange.Values.Label, FSlateColor::UseForeground(), {}, lChange.Values.RevisionValue,
				lChange.Values.WorkingValue, lChange.WorkingWidget }));
		}
	}
	else if (m_SelectedGraph)
	{
		TArray<FBlueprintGraphPair> lChangedGraphs = m_SelectedGraph->ChangedChildren;
		if (m_SelectedGraph->HasRootChanges)
			lChangedGraphs.Insert(m_SelectedGraph->Root, 0);

		for (const FBlueprintGraphPair & lPair : lChangedGraphs)
		{
			UEdGraph * const lOldGraph = lPair.Revision.Get();
			UEdGraph * const lWorkingGraph = lPair.Working.Get();
			const UEdGraph * const lNamedGraph = lWorkingGraph ? lWorkingGraph : lOldGraph;
			const bool lIsNested = lPair != m_SelectedGraph->Root;
			if (lOldGraph && lWorkingGraph)
			{
				FBlueprintGraphDiffs lDiffs;
				const auto & lResults = BlueprintDiffModel::GetGraphDifferences(
					lPair, m_CurrentGraphDiffs ? *m_CurrentGraphDiffs : lDiffs);
				for (const FBlueprintNodeDifference & lDifference : lResults)
				{
					const FDiffSingleResult & lResult = lDifference.Display;
					const FText lLabel = lIsNested ? FText::Format(LOCTEXT("NestedChange", "{0} · {1}"),
						FText::FromName(lNamedGraph->GetFName()), lResult.DisplayString) : lResult.DisplayString;
					m_Changes.Add(MakeShared<FBlueprintDiffChange>(FBlueprintDiffChange{
						GetNodeGuid(lOldGraph, lDifference.RevisionNode.Get()), GetNodeGuid(lWorkingGraph, lDifference.WorkingNode.Get()),
						lLabel, lResult.GetDisplayColor(), lPair }));
				}
			}

			if (!lOldGraph || !lWorkingGraph || lOldGraph->GetFName() != lWorkingGraph->GetFName())
			{
				const FText lName = FText::FromName(lNamedGraph->GetFName());
				const FText lLabel = !lOldGraph ? FText::Format(LOCTEXT("GraphAdded", "{0} added"), lName)
					: !lWorkingGraph ? FText::Format(LOCTEXT("GraphRemoved", "{0} removed"), lName)
					: FText::Format(LOCTEXT("GraphRenamed", "{0} renamed to {1}"), FText::FromName(lOldGraph->GetFName()), lName);
				FGuid lNode;
				if (!lOldGraph || !lWorkingGraph)
				{
					for (const UEdGraphNode * const lGraphNode : lNamedGraph->Nodes)
					{
						if (lGraphNode && lGraphNode->NodeGuid.IsValid())
						{
							lNode = lGraphNode->NodeGuid;
							break;
						}
					}
				}
				m_Changes.Add(MakeShared<FBlueprintDiffChange>(FBlueprintDiffChange{
					lOldGraph ? lNode : FGuid(), lWorkingGraph ? lNode : FGuid(), lLabel, FSlateColor::UseForeground(), lPair }));
			}
		}
	}

	m_ChangeList->ClearSelection();
	m_ChangeList->RequestListRefresh();
	if (m_SelectedGraph && m_SelectedGraph->Section != EBlueprintDiffSection::Graph && !m_Changes.IsEmpty())
	{
		const FChangePtr * const lSelected = m_Changes.FindByPredicate([&lPrevious](const FChangePtr & aChange)
		{
			return lPrevious && (aChange->Setting.Kind != EBlueprintSettingKind::None
				? aChange->Setting == lPrevious->Setting : aChange->Label.EqualTo(lPrevious->Label));
		});
		m_ChangeList->SetSelection(lSelected ? *lSelected : m_Changes[0], ESelectInfo::Direct);
	}

	DoRefreshGraphHighlights();
}

void SBlueprintDiffEdit::DoFocusChange(FChangePtr aChange, ESelectInfo::Type aSelectInfo)
{
	if (!aChange || !m_SelectedGraph)
		return;

	if (m_SelectedGraph->Section == EBlueprintDiffSection::Settings)
	{
		DoShowSettingChange(aChange);
		if (aSelectInfo == ESelectInfo::OnNavigation || aSelectInfo == ESelectInfo::OnKeyPress)
			DoSelectSettingMember(aChange);
		return;
	}
	if (m_SelectedGraph->Section == EBlueprintDiffSection::Widgets)
	{
		DoShowWidgetChange(*aChange, aSelectInfo != ESelectInfo::Direct);
		return;
	}

	if (aChange->Graphs != m_DisplayedGraphs || (aChange->Graphs.Working.IsValid() && !m_WorkingGraphEditor.IsValid()))
		DoShowGraphPair(aChange->Graphs, true);
	DoCancelPendingFocus();
	if (aChange->RevisionNode.IsValid() || aChange->WorkingNode.IsValid())
	{
		m_PendingFocus = aChange;
		m_FocusDeadline = FPlatformTime::Seconds() + 2.0;
		m_FocusHandle = FSlateApplication::Get().OnPostTick().AddSP(this, &SBlueprintDiffEdit::DoApplyPendingFocus);
	}
}

void SBlueprintDiffEdit::DoSelectSettingMember(FChangePtr aChange)
{
	if (!aChange || aChange->Setting.Member.IsNone())
		return;
	const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin();
	const UBlueprint * const lBlueprint = m_WorkingBlueprint.Get();
	if (!lEditor || !lBlueprint)
		return;

	const FBlueprintSettingTarget & lTarget = aChange->Setting;
	const UObject * const lDefaults = DiffUtils::GetCDO(lBlueprint);
	const FProperty * const lProperty = lDefaults ? FindFProperty<FProperty>(lDefaults->GetClass(), lTarget.Member) : nullptr;
	if (FBlueprintEditorUtils::FindNewVariableIndex(lBlueprint, lTarget.Member) == INDEX_NONE
		&& !(lTarget.Kind == EBlueprintSettingKind::ClassDefault && lProperty))
		return;

	FBlueprintEditor & lNative = static_cast<FBlueprintEditor &>(*lEditor);
	if (Cast<UWidgetBlueprint>(lBlueprint) && lEditor->GetToolkitFName() == TEXT("WidgetBlueprintEditor")
		&& lNative.GetCurrentMode() == DESIGNER_MODE)
	{
		lNative.SetCurrentMode(GRAPH_MODE);
		m_LastFocusedWorkingGraph = lEditor->GetFocusedGraph();
	}
	const TSharedPtr<FTabManager> lTabs = lNative.GetTabManager();
	if (!lTabs->FindExistingLiveTab(FBlueprintEditorTabs::MyBlueprintID))
		lTabs->TryInvokeTab(FBlueprintEditorTabs::MyBlueprintID);
	if (!lTabs->FindExistingLiveTab(FBlueprintEditorTabs::DetailsID))
		lTabs->TryInvokeTab(FBlueprintEditorTabs::DetailsID);
	if (const TSharedPtr<SMyBlueprint> lMyBlueprint = lNative.GetMyBlueprintWidget())
	{
		const bool lDispatcher = lTarget.IsDispatcher || (lProperty && lProperty->IsA<FMulticastDelegateProperty>());
		lMyBlueprint->SelectItemByName(lTarget.Member, ESelectInfo::OnMouseClick,
			lDispatcher ? NodeSectionID::DELEGATE : NodeSectionID::VARIABLE);
	}
}

void SBlueprintDiffEdit::DoShowSettingChange(const FChangePtr & aChange)
{
	const FBlueprintSettingTarget & lTarget = aChange->Setting;
	const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin();
	UBlueprint * const lBlueprint = m_WorkingBlueprint.Get();
	UObject * lObject = nullptr;
	FName lPropertyName;
	if (lBlueprint && lEditor)
	{
		if (lTarget.Kind == EBlueprintSettingKind::ClassSetting)
		{
			lObject = lBlueprint;
			lPropertyName = lTarget.Property;
		}
		else if (lTarget.Kind == EBlueprintSettingKind::ClassDefault || (lTarget.Kind == EBlueprintSettingKind::Member && !lTarget.IsDispatcher))
		{
			lObject = const_cast<UObject *>(DiffUtils::GetCDO(lBlueprint));
			lPropertyName = lTarget.Member;
		}
	}
	const bool lSameSetting = m_ActiveSetting && lTarget.Kind != EBlueprintSettingKind::None
		&& lTarget == m_ActiveSetting->Setting && m_SettingObject.Get() == lObject;
	m_ActiveSetting = aChange;
	if (lSameSetting && m_SettingComparison)
		return;

	m_SettingObject = lObject;
	m_SingleSettingEditor.Reset();
	TSharedPtr<SWidget> lValueEditor;
	const FProperty * const lProperty = lObject ? FindFProperty<FProperty>(lObject->GetClass(), lPropertyName) : nullptr;
	if (lProperty && lProperty->HasAnyPropertyFlags(CPF_Edit) && !lProperty->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnTemplate)
		&& !lProperty->IsA<FMulticastDelegateProperty>() && !lProperty->IsA<FDelegateProperty>())
	{
		FPropertyEditorModule & lModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		FBlueprintEditor & lNative = static_cast<FBlueprintEditor &>(*lEditor);
		FSinglePropertyParams lParams;
		lParams.NamePlacement = EPropertyNamePlacement::Hidden;
		lParams.NotifyHook = &lNative;
		lParams.bHideResetToDefault = true;
		m_SingleSettingEditor = lModule.CreateSingleProperty(lObject, lPropertyName, lParams);
		if (m_SingleSettingEditor && m_SingleSettingEditor->HasValidProperty())
			lValueEditor = m_SingleSettingEditor;
		else
		{
			FDetailsViewArgs lArgs;
			lArgs.bAllowSearch = false;
			lArgs.bShowOptions = false;
			lArgs.bShowObjectLabel = false;
			lArgs.bHideSelectionTip = true;
			lArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
			lArgs.NotifyHook = &lNative;
			const TSharedRef<IDetailsView> lDetails = lModule.CreateDetailView(lArgs);
			lDetails->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda([lPropertyName](const FPropertyAndParent & aProperty)
			{
				const FProperty * const lRoot = aProperty.ParentProperties.IsEmpty() ? &aProperty.Property : aProperty.ParentProperties.Last();
				return lRoot->GetFName() == lPropertyName;
			}));
			lDetails->SetObject(lObject);
			lValueEditor = lDetails;
		}
	}
	m_SettingComparison = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
		[
			SNew(STextBlock).Text_Lambda([this] { return m_ActiveSetting ? m_ActiveSetting->Label : FText(); }).AutoWrapText(true)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
		[
			SNew(SBox).MaxDesiredHeight(110.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(STextBlock).Text_Lambda([this]
					{
						return m_ActiveSetting ? FText::Format(LOCTEXT("RevisionSetting", "Revision\n{0}"), m_ActiveSetting->RevisionValue) : FText();
					}).AutoWrapText(true)
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f)
		[
			SNew(STextBlock).Text(LOCTEXT("WorkingSettingLabel", "Working Blueprint"))
		]
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8.0f, 0.0f)
		[
			lValueEditor ? StaticCastSharedRef<SWidget>(SNew(SBox).VAlign(VAlign_Top)
				.IsEnabled(this, &SBlueprintDiffEdit::CanEditBlueprint)[lValueEditor.ToSharedRef()])
				: StaticCastSharedRef<SWidget>(SNew(SScrollBox) + SScrollBox::Slot()
				[
					SNew(STextBlock).Text_Lambda([this] { return m_ActiveSetting ? m_ActiveSetting->WorkingValue : FText(); }).AutoWrapText(true)
				])
		];
	m_RevisionGraphBox->SetContent(m_SettingComparison.ToSharedRef());
}

TSharedRef<SWidget> SBlueprintDiffEdit::DoGetWidgetPreview()
{
	if (m_RevisionWidgetPreview)
		return m_RevisionWidgetPreview.ToSharedRef();

	const UWidgetBlueprint * const lRevision = Cast<UWidgetBlueprint>(m_RevisionBlueprint.Get());
	TArray<const UWidgetTree *> lWidgetTrees;
	for (const UWidgetBlueprint * lBlueprint = lRevision; lBlueprint; )
	{
		if (lBlueprint->WidgetTree)
			lWidgetTrees.Add(lBlueprint->WidgetTree);
		const UClass * const lParent = lBlueprint->ParentClass;
		lBlueprint = lParent ? Cast<UWidgetBlueprint>(lParent->ClassGeneratedBy) : nullptr;
	}
	const UWidgetTree * lLayout = nullptr;
	for (const UWidgetTree * lTree : lWidgetTrees)
	{
		if (lTree->RootWidget)
		{
			lLayout = lTree;
			break;
		}
	}
	if (!lLayout)
		return SNew(STextBlock).Text(LOCTEXT("NoRevisionWidget", "No widget layout is available for this revision."));

	m_RevisionWidgetTree.Reset(DuplicateObject<UWidgetTree>(lLayout, GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UWidgetTree::StaticClass(), TEXT("BlueprintDiffEditWidgetPreview"))));
	UWidget * const lRoot = m_RevisionWidgetTree->RootWidget;
	// Apply parent bindings first so a nearer Blueprint can override them.
	for (int32 lIndex = lWidgetTrees.Num() - 1; lIndex >= 0; --lIndex)
	{
		TArray<FName> lSlots;
		lWidgetTrees[lIndex]->GetSlotNames(lSlots);
		if (lSlots.IsEmpty())
			continue;

		UWidgetTree * const lOverlay = DuplicateObject<UWidgetTree>(lWidgetTrees[lIndex], GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UWidgetTree::StaticClass(), TEXT("BlueprintDiffEditWidgetSlots")));
		m_RevisionSlotTrees.Emplace(lOverlay);
		for (const FName lName : lSlots)
		{
			UWidget * const lContent = lOverlay->GetContentForSlot(lName);
			if (UNamedSlot * const lSlot = Cast<UNamedSlot>(m_RevisionWidgetTree->FindWidget(lName)); lSlot && lContent)
				lSlot->SetContent(lContent);
		}
	}
	m_RevisionWidgetTree->ForEachWidget([](UWidget * aWidget) { aWidget->SetDesignerFlags(EWidgetDesignFlags::Designing); });

	const UUserWidget * const lDefaults = lRevision->GeneratedClass
		? lRevision->GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	const bool lDesired = lDefaults && (lDefaults->DesignSizeMode == EDesignPreviewSizeMode::Desired
		|| lDefaults->DesignSizeMode == EDesignPreviewSizeMode::DesiredOnScreen);
	const bool lCustom = lDefaults && (lDefaults->DesignSizeMode == EDesignPreviewSizeMode::Custom
		|| lDefaults->DesignSizeMode == EDesignPreviewSizeMode::CustomOnScreen);
	const FVector2D lSize = lCustom ? lDefaults->DesignTimeSize : FVector2D(1280.0, 720.0);
	m_RevisionWidgetPreview = SNew(SScaleBox)
		.Stretch(EStretch::ScaleToFit)
		.Visibility(EVisibility::HitTestInvisible)
		[
			SNew(SBox)
			.WidthOverride(lDesired ? FOptionalSize() : FOptionalSize(FMath::Max(1.0, lSize.X)))
			.HeightOverride(lDesired ? FOptionalSize() : FOptionalSize(FMath::Max(1.0, lSize.Y)))
			[
				lRoot->TakeWidget()
			]
		];
	return m_RevisionWidgetPreview.ToSharedRef();
}

void SBlueprintDiffEdit::DoShowWidgetChange(const FBlueprintDiffChange & aChange, bool aSelectWidget)
{
	if (!m_WidgetValuesBox)
	{
		m_RevisionGraphBox->SetContent(SNew(SSplitter)
			.Orientation(Orient_Vertical)
			+ SSplitter::Slot().Value(0.70f).MinSize(80.0f)
			[
				SNew(SBorder).Padding(8.0f)[DoGetWidgetPreview()]
			]
			+ SSplitter::Slot().Value(0.30f).MinSize(64.0f)
			[
				SAssignNew(m_WidgetValuesBox, SBox)
			]);
	}
	m_WidgetValuesBox->SetContent(BuildValueComparison(aChange));

	if (aSelectWidget)
	{
		const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin();
		UWidgetBlueprint * const lBlueprint = Cast<UWidgetBlueprint>(m_WorkingBlueprint.Get());
		if (lEditor && lBlueprint && lEditor->GetToolkitFName() == TEXT("WidgetBlueprintEditor"))
		{
			FWidgetBlueprintEditor & lWidgetEditor = static_cast<FWidgetBlueprintEditor &>(*lEditor);
			lWidgetEditor.SetCurrentMode(DESIGNER_MODE);
			if (UWidget * const lWidget = lBlueprint->WidgetTree ? lBlueprint->WidgetTree->FindWidget(aChange.WorkingWidget) : nullptr)
				lWidgetEditor.SelectWidgets({ lWidgetEditor.GetReferenceFromTemplate(lWidget) }, false);
			else
				lWidgetEditor.SelectWidgets({}, false);
		}
	}
}

void SBlueprintDiffEdit::DoFocusFirstChange()
{
	const FChangePtr * const lFirst = m_Changes.FindByPredicate([this](const FChangePtr & aChange)
	{
		return aChange->Graphs == m_DisplayedGraphs && (aChange->RevisionNode.IsValid() || aChange->WorkingNode.IsValid());
	});
	if (lFirst && !m_ChangeList->IsItemSelected(*lFirst))
		m_ChangeList->SetSelection(*lFirst, ESelectInfo::Direct);
}

void SBlueprintDiffEdit::DoApplyPendingFocus(float aDeltaTime)
{
	if (!m_PendingFocus || m_PendingFocus->Graphs != m_DisplayedGraphs || FPlatformTime::Seconds() >= m_FocusDeadline)
	{
		DoCancelPendingFocus();
		return;
	}

	const TSharedPtr<SGraphEditor> lWorkingEditor = m_WorkingGraphEditor.Pin();
	SGraphPanel * const lRevisionPanel = m_RevisionGraphEditor ? m_RevisionGraphEditor->GetGraphPanel() : nullptr;
	SGraphPanel * const lWorkingPanel = lWorkingEditor ? lWorkingEditor->GetGraphPanel() : nullptr;
	if ((lRevisionPanel && !IsReadyForDiffFocus(*lRevisionPanel)) || (lWorkingPanel && !IsReadyForDiffFocus(*lWorkingPanel)))
	{
		m_FocusPanelsReady = false;
		return;
	}

	UEdGraphNode * const lOldNode = FindNode(m_DisplayedGraphs.Revision.Get(), m_PendingFocus->RevisionNode);
	UEdGraphNode * const lWorkingNode = FindNode(m_DisplayedGraphs.Working.Get(), m_PendingFocus->WorkingNode);
	FVector2f lOldAnchor, lWorkingAnchor;
	if ((lOldNode && lRevisionPanel && !GetDiffFocusAnchor(lRevisionPanel, lOldNode, lOldAnchor))
		|| (lWorkingNode && lWorkingPanel && !GetDiffFocusAnchor(lWorkingPanel, lWorkingNode, lWorkingAnchor)))
	{
		m_FocusPanelsReady = false;
		return;
	}

	const bool lHasOldAnchor = lOldNode && lRevisionPanel;
	const bool lHasWorkingAnchor = lWorkingNode && lWorkingPanel;
	if (!lHasOldAnchor && !lHasWorkingAnchor)
	{
		DoCancelPendingFocus();
		return;
	}

	// Give the graph editors' deferred view lock a paint pass after both panes finish laying out.
	if (!m_FocusPanelsReady)
	{
		m_FocusPanelsReady = true;
		return;
	}

	const float lZoom = lWorkingPanel ? lWorkingPanel->GetZoomAmount() : lRevisionPanel->GetZoomAmount();
	if (lRevisionPanel)
		FocusDiffPanel(*lRevisionPanel, lHasOldAnchor ? lOldAnchor : lWorkingAnchor, lZoom);
	if (lWorkingPanel)
		FocusDiffPanel(*lWorkingPanel, lHasWorkingAnchor ? lWorkingAnchor : lOldAnchor, lZoom);
	DoCancelPendingFocus();
}

void SBlueprintDiffEdit::DoCancelPendingFocus()
{
	if (m_FocusHandle.IsValid())
	{
		if (FSlateApplication::IsInitialized())
			FSlateApplication::Get().OnPostTick().Remove(m_FocusHandle);
		m_FocusHandle.Reset();
	}
	m_PendingFocus.Reset();
	m_FocusPanelsReady = false;
}

void SBlueprintDiffEdit::DoSavePaneSize(EPaneSplit aSplit)
{
	SSplitter & lSplitter = *m_Splitters[aSplit];
	const float lFirst = lSplitter.SlotAt(0).GetSizeValue();
	const float lTotal = lFirst + lSplitter.SlotAt(1).GetSizeValue();
	if (lFirst <= 0.0f || lTotal <= lFirst)
		return;

	const float lFraction = lFirst / lTotal;
	if (!FMath::IsNearlyEqual(lFraction, GetSavedPaneSize(aSplit)))
	{
		GConfig->SetFloat(LAYOUT_SECTION, PANE_SIZE_KEYS[aSplit], lFraction, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		OnPaneSizeChanged.Broadcast(aSplit, lFraction);
	}
}

void SBlueprintDiffEdit::DoApplyPaneSize(int32 aSplit, float aFraction)
{
	SSplitter & lSplitter = *m_Splitters[aSplit];
	lSplitter.SlotAt(0).SetSizeValue(aFraction);
	lSplitter.SlotAt(1).SetSizeValue(1.0f - aFraction);
	lSplitter.Invalidate(EInvalidateWidgetReason::Layout);
}

TSharedRef<ITableRow> SBlueprintDiffEdit::DoBuildGraphRow(FGraphPtr aGraph, const TSharedRef<STableViewBase> & aOwner)
{
	return SNew(STableRow<FGraphPtr>, aOwner)
	[
		SNew(STextBlock).Text(aGraph->Label).ToolTipText(aGraph->Label)
	];
}

TSharedRef<ITableRow> SBlueprintDiffEdit::DoBuildChangeRow(FChangePtr aChange, const TSharedRef<STableViewBase> & aOwner)
{
	return SNew(STableRow<FChangePtr>, aOwner)
	[
		SNew(STextBlock).Text(aChange->Label).ToolTipText(aChange->Label).ColorAndOpacity(aChange->Color)
	];
}

bool SBlueprintDiffEdit::CanEditBlueprint() const
{
	const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin();
	return lEditor && lEditor->GetOpenMethod() == EAssetOpenMethod::Edit && !GEditor->PlayWorld
		&& !lEditor->GetTabManager()->IsReadOnly() && m_WorkingBlueprint.IsValid();
}

TSharedPtr<SWidget> SBlueprintDiffEdit::DoBuildSettingMenu()
{
	const TArray<FChangePtr> lSelected = m_ChangeList->GetSelectedItems();
	if (lSelected.IsEmpty() || lSelected[0]->Setting.Kind == EBlueprintSettingKind::None
		|| !m_RevisionBlueprint || m_RevisionRequest.IsValid() || !CanEditBlueprint())
		return nullptr;

	FMenuBuilder lMenu(true, nullptr);
	lMenu.AddMenuEntry(LOCTEXT("RevertSettingMenu", "Revert to Compared Revision"),
		LOCTEXT("RevertSettingTip", "Restore this value or member from the displayed revision. You can undo this action."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffEdit::DoRevertSetting,
			lSelected[0]->Setting, TWeakObjectPtr<UBlueprint>(m_RevisionBlueprint.Get()))));
	return lMenu.MakeWidget();
}

void SBlueprintDiffEdit::DoRevertSetting(FBlueprintSettingTarget aSetting, TWeakObjectPtr<UBlueprint> aRevision)
{
	if (!aRevision.IsValid() || m_RevisionBlueprint.Get() != aRevision.Get() || m_RevisionRequest.IsValid() || !CanEditBlueprint())
		return;
	FText lError;
	if (!BlueprintDiffModel::RevertSetting(m_WorkingBlueprint.Get(), aRevision.Get(), aSetting, lError))
	{
		if (!lError.IsEmpty())
			FMessageDialog::Open(EAppMsgType::Ok, lError);
		return;
	}

	m_RefreshAt = 0.0;
	DoRebuildGraphList();
}

bool SBlueprintDiffEdit::CanRevertFunction(const FGraphPtr & aGraph) const
{
	if (m_RevisionRequest.IsValid() || !aGraph || aGraph->Section != EBlueprintDiffSection::Graph
		|| !m_RevisionBlueprint.IsValid() || !m_WorkingBlueprint.IsValid())
		return false;

	if (!CanEditBlueprint())
		return false;

	const UEdGraph * const lOldGraph = aGraph->Root.Revision.Get();
	const UEdGraph * const lWorkingGraph = aGraph->Root.Working.Get();
	return (lOldGraph || lWorkingGraph)
		&& (!lOldGraph || BlueprintDiffFunction::IsFunction(m_RevisionBlueprint.Get(), lOldGraph))
		&& (!lWorkingGraph || BlueprintDiffFunction::IsFunction(m_WorkingBlueprint.Get(), lWorkingGraph));
}

TSharedPtr<SWidget> SBlueprintDiffEdit::DoBuildGraphMenu()
{
	const TArray<FGraphPtr> lSelected = m_GraphList->GetSelectedItems();
	if (lSelected.IsEmpty() || !CanRevertFunction(lSelected[0]))
		return nullptr;

	const FGraphPtr lGraph = lSelected[0];
	FMenuBuilder lMenu(true, nullptr);
	lMenu.AddMenuEntry(LOCTEXT("RevertMenu", "Revert Function to Compared Revision"),
		LOCTEXT("RevertTip", "Restore this function from the displayed revision. A function added in the workspace will be removed. Undo is available."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffEdit::DoRevertFunction, lGraph, TWeakObjectPtr<UBlueprint>(m_RevisionBlueprint.Get()))));
	return lMenu.MakeWidget();
}

void SBlueprintDiffEdit::DoRevertFunction(FGraphPtr aGraph, TWeakObjectPtr<UBlueprint> aRevision)
{
	if (m_RevisionBlueprint.Get() != aRevision.Get() || !CanRevertFunction(aGraph))
		return;

	UEdGraph * const lWorkingGraph = aGraph->Root.Working.Get();
	const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin();
	if (lWorkingGraph && lEditor->GetFocusedGraph() == lWorkingGraph)
	{
		if (const TSharedPtr<SGraphEditor> lGraphEditor = lEditor->OpenGraphAndBringToFront(lWorkingGraph, false))
			lGraphEditor->ClearSelectionSet();
	}

	FText lError;
	if (!BlueprintDiffFunction::Revert(m_WorkingBlueprint.Get(), m_RevisionBlueprint.Get(),
		lWorkingGraph, aGraph->Root.Revision.Get(), lError))
	{
		FMessageDialog::Open(EAppMsgType::Ok, lError);
		return;
	}
	m_RefreshAt = 0.0;
	DoRebuildGraphList();
}

bool SBlueprintDiffEdit::CanCopyRevisionNodes() const
{
	if (m_RevisionGraphEditor)
	{
		for (UObject * const lItem : m_RevisionGraphEditor->GetSelectedNodes())
		{
			const UEdGraphNode * const lNode = Cast<UEdGraphNode>(lItem);
			if (lNode && lNode->CanDuplicateNode())
				return true;
		}
	}
	return false;
}

void SBlueprintDiffEdit::DoCopyRevisionNodes()
{
	TSet<UObject *> lCopy;
	for (UObject * const lItem : m_RevisionGraphEditor->GetSelectedNodes())
	{
		UEdGraphNode * const lNode = Cast<UEdGraphNode>(lItem);
		if (lNode && lNode->CanDuplicateNode())
			lCopy.Add(lNode);
	}
	FString lText;
	FEdGraphUtilities::ExportNodesToText(lCopy, lText);
	FPlatformApplicationMisc::ClipboardCopy(*lText);
}

void SBlueprintDiffEdit::DoOnBlueprintChanged(UBlueprint * aBlueprint)
{
	DoQueueRefresh();
}

void SBlueprintDiffEdit::DoOnGraphChanged(const FEdGraphEditAction & aAction)
{
	DoQueueRefresh();
}

void SBlueprintDiffEdit::DoOnObjectModified(UObject * aObject)
{
	if (const UBlueprint * const lBlueprint = m_WorkingBlueprint.Get())
	{
		const UObject * const lDefaults = DiffUtils::GetCDO(lBlueprint);
		if (aObject == lBlueprint || aObject->IsIn(lBlueprint) || (lDefaults && (aObject == lDefaults || aObject->IsIn(lDefaults))))
			DoQueueRefresh();
	}
}

void SBlueprintDiffEdit::DoOnObjectTransacted(UObject * aObject, const FTransactionObjectEvent & aEvent)
{
	DoOnObjectModified(aObject);
}

void SBlueprintDiffEdit::DoQueueRefresh()
{
	constexpr double REFRESH_DELAY = 0.12;
	m_RefreshAt = FPlatformTime::Seconds() + REFRESH_DELAY;
}

void SBlueprintDiffEdit::Tick(const FGeometry & aGeometry, double aTime, float aDelta)
{
	SCompoundWidget::Tick(aGeometry, aTime, aDelta);
	if (aTime >= m_NextHistoryCheck || m_RevisionRequest.IsReady() || m_LatestGitRevision.IsReady() || m_GitHistoryRequest.IsReady())
	{
		m_NextHistoryCheck = aTime + 0.5;
		DoUpdateHistory();
	}
	DoProcessGitHistory();
	if (m_RevisionRequest.IsReady())
		DoFinishRevision();

	if (m_RefreshAt > 0.0 && FPlatformTime::Seconds() >= m_RefreshAt)
	{
		m_RefreshAt = 0.0;
		DoRebuildGraphList();
	}

	const TSharedPtr<IBlueprintEditor> lEditor = m_Editor.Pin();
	UEdGraph * const lFocusedGraph = lEditor ? lEditor->GetFocusedGraph() : nullptr;
	if (m_LastFocusedWorkingGraph.Get() == lFocusedGraph)
		return;

	m_LastFocusedWorkingGraph = lFocusedGraph;
	for (const FGraphPtr & lRow : m_Graphs)
	{
		if (const FBlueprintGraphPair * const lPair = FindWorkingGraph(*lRow, lFocusedGraph))
		{
			if (m_SelectedGraph != lRow)
			{
				m_GraphList->SetSelection(lRow, ESelectInfo::Direct);
				m_GraphList->RequestScrollIntoView(lRow);
			}
			else if (*lPair != m_DisplayedGraphs)
			{
				DoShowGraphPair(*lPair, false);
				DoFocusFirstChange();
			}
			return;
		}
	}

	DoCancelPendingFocus();
	DoSetGraphLock(nullptr);
	DoSetWorkingGraphEditor(nullptr);
	if (lFocusedGraph && m_SelectedGraph && m_SelectedGraph->Section == EBlueprintDiffSection::Graph)
		m_GraphList->ClearSelection();
}

#undef LOCTEXT_NAMESPACE
