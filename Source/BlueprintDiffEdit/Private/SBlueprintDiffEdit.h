#pragma once

#include "CoreMinimal.h"
#include "BlueprintDiffGit.h"
#include "Async/Future.h"
#include "Blueprint/WidgetTree.h"
#include "BlueprintDiffModel.h"
#include "Containers/StaticArray.h"
#include "DiffResults.h"
#include "ISourceControlProvider.h"
#include "SBlueprintDiffChanges.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class IBlueprintEditor;
class ISinglePropertyView;
class ISourceControlRevision;
class SBox;
class SDockTab;
class SGraphEditor;
class SSplitter;
class UBlueprint;
class UEdGraph;
class UPackage;
struct FBlueprintDiffRevision;
struct FBlueprintDiffSnapshot;

enum class EBlueprintDiffSection : uint8
{
	Graph,
	Settings,
	Widgets
};

struct FBlueprintDiffGraph : FBlueprintGraphChange
{
	FText Label;
	EBlueprintDiffSection Section = EBlueprintDiffSection::Graph;
};

struct FBlueprintDiffChange
{
	FGuid RevisionNode;
	FGuid WorkingNode;
	FText Label;
	FSlateColor Color;
	FBlueprintGraphPair Graphs;
	FText RevisionValue;
	FText WorkingValue;
	FName WorkingWidget = NAME_None;
	FBlueprintSettingTarget Setting;
};

class SBlueprintDiffEdit final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBlueprintDiffEdit) {}
		SLATE_EVENT(FBlueprintDiffOpenAsset, OnOpenBlueprint)
		SLATE_EVENT(FBlueprintDiffBeforeReload, OnBeforeReload)
	SLATE_END_ARGS()

	void Construct(const FArguments & aArgs, const TSharedRef<IBlueprintEditor> & aEditor,
		const TSharedRef<SDockTab> & aHostTab, UBlueprint * aBlueprint);
	~SBlueprintDiffEdit();

	bool IsHostedBy(const SDockTab * aTab) const;
	bool IsEditingAnyFile(const TArray<FString> & aFiles) const;
	void Close();

	virtual void Tick(const FGeometry & aGeometry, double aTime, float aDelta) override;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FBlueprintDiffToolbarTests;
#endif

	enum EPaneSplit : uint8
	{
		OuterSplit,
		FunctionsSplit,
		RevisionSplit,
		ChangedFilesSplit,
		PaneSplitCount
	};

	using FGraphPtr = TSharedPtr<FBlueprintDiffGraph>;
	using FChangePtr = TSharedPtr<FBlueprintDiffChange>;
	using FRevisionPtr = TSharedPtr<const FBlueprintDiffRevision>;

	void DoUpdateHistory(bool aForce = false);
	void DoStartProviderHistory();
	void DoStartGitHistory();
	void DoProcessGitHistory();
	void DoCancelGitHistory();
	void DoSetHistory(TArray<FRevisionPtr> aRevisions);
	void DoReceiveHistory(const FSourceControlOperationRef & aOperation, ECommandResult::Type aResult,
		FName aProvider, uint32 aHistoryGeneration);
	void DoLoadRevision(FRevisionPtr aRevision);
	void DoStartMasterComparison();
	void DoRequestRevision(FRevisionPtr aRevision);
	void DoFinishRevision();
	void DoCancelRevision();
	void DoApplyRevision(UPackage * aRevisionPackage, const FString & aOriginalFilename,
		const FBlueprintDiffCommit & aCommit, FRevisionPtr aHistoryRevision);
	TSharedRef<SWidget> DoBuildRevisionMenu();
	void DoRebuildGraphList();
	void DoRefreshChanges();
	void DoSelectGraph(FGraphPtr aGraph, ESelectInfo::Type aSelectInfo);
	void DoFocusChange(FChangePtr aChange, ESelectInfo::Type aSelectInfo);
	void DoSelectSettingMember(FChangePtr aChange);
	void DoShowSettingChange(const FChangePtr & aChange);
	TSharedPtr<SWidget> DoBuildSettingMenu();
	void DoRevertSetting(FBlueprintSettingTarget aSetting, TWeakObjectPtr<UBlueprint> aRevision);
	bool CanEditBlueprint() const;
	void DoFocusFirstChange();
	void DoShowWidgetChange(const FBlueprintDiffChange & aChange, bool aSelectWidget);
	TSharedRef<SWidget> DoGetWidgetPreview();
	void DoApplyPendingFocus(float aDeltaTime);
	void DoCancelPendingFocus();
	void DoSavePaneSize(EPaneSplit aSplit);
	void DoApplyPaneSize(int32 aSplit, float aFraction);
	void DoBuildRevisionGraph(UEdGraph * aGraph);
	void DoShowGraphPair(const FBlueprintGraphPair & aPair, bool aOpenWorking);
	void DoSetGraphLock(const TSharedPtr<SGraphEditor> & aWorking);
	void DoSetWorkingGraphEditor(const TSharedPtr<SGraphEditor> & aWorking);
	void DoRefreshGraphHighlights();
	TSharedRef<ITableRow> DoBuildGraphRow(FGraphPtr aGraph, const TSharedRef<STableViewBase> & aOwner);
	TSharedRef<ITableRow> DoBuildChangeRow(FChangePtr aChange, const TSharedRef<STableViewBase> & aOwner);
	TSharedPtr<SWidget> DoBuildGraphMenu();
	void DoRevertFunction(FGraphPtr aGraph, TWeakObjectPtr<UBlueprint> aRevision);
	bool CanRevertFunction(const FGraphPtr & aGraph) const;
	void DoCopyRevisionNodes();
	bool CanCopyRevisionNodes() const;
	FReply DoClosePanel();
	void DoOnBlueprintChanged(UBlueprint * aBlueprint);
	void DoOnGraphChanged(const FEdGraphEditAction & aAction);
	void DoOnObjectModified(UObject * aObject);
	void DoOnObjectTransacted(UObject * aObject, const FTransactionObjectEvent & aEvent);
	void DoQueueRefresh();

	TWeakPtr<IBlueprintEditor> m_Editor;
	TWeakPtr<SDockTab> m_HostTab;
	TSharedPtr<SWidget> m_OriginalContent;
	TSharedPtr<SBox> m_WorkingEditorBox;
	TStaticArray<TSharedPtr<SSplitter>, PaneSplitCount> m_Splitters;
	FChangePtr m_PendingFocus;
	FDelegateHandle m_FocusHandle;
	double m_FocusDeadline = 0.0;
	bool m_FocusPanelsReady = false;
	TWeakObjectPtr<UBlueprint> m_WorkingBlueprint;
	TStrongObjectPtr<UBlueprint> m_RevisionBlueprint;
	TArray<FRevisionPtr> m_Revisions;
	FRevisionPtr m_Revision;
	FBlueprintDiffCommit m_Commit;
	TFuture<TSharedPtr<FBlueprintDiffSnapshot>> m_RevisionRequest;
	TSharedPtr<TAtomic<bool>> m_CancelRevision;
	TFuture<TOptional<FBlueprintDiffGitRevision>> m_LatestGitRevision;
	TFuture<FBlueprintDiffFileHistory> m_GitHistoryRequest;
	TSharedPtr<TAtomic<bool>> m_CancelGitHistory;
	FName m_HistoryProvider;
	uint32 m_HistoryGeneration = 0;
	bool m_HistoryAvailable = false;
	bool m_RevisionWasRequested = false;
	bool m_RequestingMaster = false;
	double m_NextHistoryCheck = 0.0;
	FString m_Filename;
	FText m_Status;
	FText m_HistoryStatus;
	TArray<FGraphPtr> m_Graphs;
	TArray<FChangePtr> m_Changes;
	TArray<FBlueprintSettingChange> m_Settings;
	FChangePtr m_ActiveSetting;
	TWeakObjectPtr<UObject> m_SettingObject;
	TSharedPtr<SWidget> m_SettingComparison;
	TSharedPtr<ISinglePropertyView> m_SingleSettingEditor;
	TArray<FBlueprintWidgetChange> m_Widgets;
	TStrongObjectPtr<UWidgetTree> m_RevisionWidgetTree;
	TArray<TStrongObjectPtr<UWidgetTree>> m_RevisionSlotTrees;
	TSharedPtr<SWidget> m_RevisionWidgetPreview;
	TSharedPtr<SBox> m_WidgetValuesBox;
	FBlueprintGraphPair m_DisplayedGraphs;
	FGraphPtr m_SelectedGraph;
	TSharedPtr<SListView<FGraphPtr>> m_GraphList;
	TSharedPtr<SListView<FChangePtr>> m_ChangeList;
	TSharedPtr<SBox> m_RevisionGraphBox;
	TSharedPtr<SGraphEditor> m_RevisionGraphEditor;
	TWeakPtr<SGraphEditor> m_WorkingGraphEditor;
	TWeakPtr<SGraphEditor> m_LockedWorkingEditor;
	TSharedPtr<FUICommandList> m_RevisionCommands;
	TSharedPtr<TArray<FDiffSingleResult>> m_NodeHighlights = MakeShared<TArray<FDiffSingleResult>>();
	TWeakObjectPtr<UEdGraph> m_WatchedWorkingGraph;
	TWeakObjectPtr<UEdGraph> m_LastFocusedWorkingGraph;
	FDelegateHandle m_GraphChangedHandle;
	double m_RefreshAt = 0.0;
	bool m_RebuildingList = false;
	FBlueprintGraphDiffs * m_CurrentGraphDiffs = nullptr;
};
