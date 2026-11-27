#pragma once

#include "Async/Future.h"
#include "BlueprintDiffGit.h"
#include "DiffResults.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class IMaterialEditor;
class SBox;
class SDockTab;
class SGraphEditor;
class UMaterialInterface;
struct FMaterialDiffSnapshot;

class SMaterialDiffEdit final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMaterialDiffEdit) {}
	SLATE_END_ARGS()

	void Construct(const FArguments & aArgs, UMaterialInterface * aMaterial,
		const TSharedRef<IMaterialEditor> & aEditor, const TSharedRef<SDockTab> & aHostTab);
	virtual ~SMaterialDiffEdit() override;
	virtual void Tick(const FGeometry & aGeometry, double aTime, float aDelta) override;

	static bool Supports(const UObject * aObject);
	static TSharedPtr<SMaterialDiffEdit> Open(UMaterialInterface * aMaterial);
	bool TryFocus(UMaterialInterface * aMaterial) const;
	bool IsEditingAnyFile(const TArray<FString> & aFiles) const;
	void Close();

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FMaterialDiffTests;
#endif
	struct FChangeEntry
	{
		FDiffSingleResult Display;
		TWeakObjectPtr<UEdGraphNode> OldNode;
		TWeakObjectPtr<UEdGraphNode> NewNode;
	};
	using FChange = TSharedPtr<FChangeEntry>;

	void DoRequest(TOptional<FBlueprintDiffGitRevision> aRevision);
	void DoApply(const FMaterialDiffSnapshot & aSnapshot);
	void DoRefreshComparison(bool aFocusFirstChange = false);
	void DoShowGraphs(UEdGraph * aRevision, UEdGraph * aWorking);
	void DoUpdateGraphLock(UEdGraph * aWorking);
	void DoCancel();
	void DoScheduleRefresh(UObject * aObject);
	void DoSelect(FChange aChange, ESelectInfo::Type aSelectInfo);
	TSharedRef<SWidget> DoBuildRevisionMenu();
	TSharedRef<ITableRow> DoBuildRow(FChange aChange, const TSharedRef<STableViewBase> & aOwner);

	TWeakObjectPtr<UMaterialInterface> m_Source;
	TWeakPtr<IMaterialEditor> m_Editor;
	TWeakPtr<SDockTab> m_HostTab;
	TSharedPtr<SWidget> m_OriginalContent;
	TWeakPtr<SGraphEditor> m_LockedGraph;
	double m_RefreshAt = 0;
	TStrongObjectPtr<UMaterialInterface> m_RevisionMaterial;
	FString m_Filename;
	FBlueprintDiffCommit m_Commit;
	FText m_Status;
	FText m_ComparisonStatus;
	FText m_HistoryError;
	FText m_Detail;
	bool m_RevisionRequested = false;
	TFuture<FBlueprintDiffFileHistory> m_History;
	TFuture<TSharedPtr<FMaterialDiffSnapshot>> m_Request;
	TSharedPtr<TAtomic<bool>> m_CancelHistory;
	TSharedPtr<TAtomic<bool>> m_CancelRevision;
	TArray<FBlueprintDiffGitRevision> m_Revisions;
	TArray<FChange> m_Changes;
	TSharedPtr<TArray<FDiffSingleResult>> m_Highlights = MakeShared<TArray<FDiffSingleResult>>();
	TSharedPtr<TArray<FDiffSingleResult>> m_WorkingHighlights = MakeShared<TArray<FDiffSingleResult>>();
	TSharedPtr<SListView<FChange>> m_List;
	TSharedPtr<SBox> m_OldBox;
	TSharedPtr<SBox> m_NewBox;
	TSharedPtr<SGraphEditor> m_OldGraph;
	TMap<UEdGraph *, UEdGraph *> m_GraphPairs;
};
