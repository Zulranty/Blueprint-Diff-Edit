#pragma once

#include "Async/Future.h"
#include "BlueprintDiffGit.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

struct FAssetData;
class SSplitter;

DECLARE_DELEGATE_OneParam(FBlueprintDiffOpenAsset, const FAssetData &);
DECLARE_DELEGATE_OneParam(FBlueprintDiffBeforeReload, const TArray<FString> &);

class SBlueprintDiffChanges final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBlueprintDiffChanges) : _Compact(false) {}
		SLATE_ARGUMENT(FString, ContextFilename)
		SLATE_ARGUMENT(bool, Compact)
		SLATE_EVENT(FBlueprintDiffOpenAsset, OnOpenBlueprint)
		SLATE_EVENT(FBlueprintDiffBeforeReload, OnBeforeReload)
	SLATE_END_ARGS()

	void Construct(const FArguments & aArgs);
	~SBlueprintDiffChanges();
	virtual void Tick(const FGeometry & aGeometry, double aTime, float aDelta) override;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FBlueprintDiffToolbarTests;
#endif
	using FFilePtr = TSharedPtr<const FBlueprintDiffProjectFile>;

	enum class EFilter : uint8 { All, Working, Staged };

	void DoRefresh();
	void DoRestartRefresh();
	void DoFilter();
	void DoOpen(FFilePtr aFile);
	void DoBrowse(FFilePtr aFile);
	void DoStage(FFilePtr aFile);
	bool CanStage(FFilePtr aFile) const;
	void DoRevert(FFilePtr aFile);
	void DoSaveFilterWidth();
	FReply DoToggleFilters();
	TSharedRef<SWidget> DoBuildFilters(int32 aSegmentsPerLine);
	bool IsCurrentAsset(const FBlueprintDiffProjectFile & aFile) const;
	TSharedPtr<SWidget> DoBuildMenu();
	TSharedRef<ITableRow> DoBuildRow(FFilePtr aFile, const TSharedRef<STableViewBase> & aOwner);
	FText DoGetStatus() const;

	FString m_ContextFilename;
	FString m_Search;
	EFilter m_Filter = EFilter::All;
	bool m_Compact = false;
	bool m_ShowFilters = true;
	bool m_HasReadChanges = false;
	double m_NextRefresh = 0.0;
	FText m_Error;
	FText m_RefreshError;
	TFuture<FBlueprintDiffProjectChanges> m_Request;
	TFuture<FText> m_StageRequest;
	TSharedPtr<TAtomic<bool>> m_CancelRequest;
	TArray<FFilePtr> m_Files;
	TArray<FFilePtr> m_VisibleFiles;
	TSharedPtr<SListView<FFilePtr>> m_List;
	TSharedPtr<SSplitter> m_FilterSplitter;
	FBlueprintDiffOpenAsset m_OnOpenBlueprint;
	FBlueprintDiffBeforeReload m_OnBeforeReload;
};
