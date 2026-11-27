#include "SBlueprintDiffChanges.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformTime.h"
#include "IContentBrowserSingleton.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceCodeNavigation.h"
#include "SourceControlHelpers.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "BlueprintDiffChanges"

namespace
{
	constexpr TCHAR LAYOUT_SECTION[] = TEXT("BlueprintDiffEdit.Layout");
	constexpr TCHAR FILTER_WIDTH[] = TEXT("GitChangesFiltersWidth");
	constexpr TCHAR FILTER_VISIBLE[] = TEXT("GitChangesFiltersVisible");

	bool MapPackageDirectory(const FString & aFilename, FString aDirectory, const FString & aMount, FString & aPackage)
	{
		aDirectory = FPaths::ConvertRelativePathToFull(aDirectory);
		FPaths::NormalizeDirectoryName(aDirectory);
		if (!FPaths::IsUnderDirectory(aFilename, aDirectory))
			return false;
		const FString lRelative = aFilename.RightChop(aDirectory.Len() + 1);
		aPackage = aMount / FPaths::ChangeExtension(lRelative, FString());
		return FPackageName::IsValidLongPackageName(aPackage);
	}

	FString PackageNameForFile(const FString & aFilename)
	{
		if (!BlueprintDiffGit::IsAssetFile(aFilename))
			return {};
		FString lPackage;
		if (FPackageName::TryConvertFilenameToLongPackageName(aFilename, lPackage)
			|| MapPackageDirectory(aFilename, FPaths::ProjectContentDir(), TEXT("/Game"), lPackage))
			return lPackage;
		for (const TSharedRef<IPlugin> & lPlugin : IPluginManager::Get().GetEnabledPluginsWithContent())
		{
			if (MapPackageDirectory(aFilename, lPlugin->GetContentDir(), lPlugin->GetMountedAssetPath(), lPackage))
				return lPackage;
		}
		return {};
	}

	FAssetData ResolveAsset(const FString & aFilename)
	{
		const FString lPackage = PackageNameForFile(aFilename);
		if (lPackage.IsEmpty())
			return {};
		IAssetRegistry & lRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> lAssets;
		lRegistry.GetAssetsByPackageName(*lPackage, lAssets);
		if (lAssets.IsEmpty())
		{
			lRegistry.ScanFilesSynchronous({ aFilename });
			lRegistry.GetAssetsByPackageName(*lPackage, lAssets);
		}
		const FName lPackageAssetName(*FPackageName::GetShortName(lPackage));
		if (const FAssetData * lAsset = lAssets.FindByPredicate([lPackageAssetName](const FAssetData & aAsset)
			{ return aAsset.AssetName == lPackageAssetName; }))
			return *lAsset;
		if (lAssets.Num() == 1)
			return lAssets[0];
		if (UPackage * const lLoaded = LoadPackage(nullptr, *lPackage, LOAD_None))
		{
			if (UObject * const lAsset = lLoaded->FindAssetInPackage())
				return FAssetData(lAsset);
		}
		return {};
	}

	FText DescribeStatus(const FBlueprintDiffChangedFile & aFile)
	{
		if (aFile.IsConflicted())
			return LOCTEXT("Conflicted", "Conflict");
		if (aFile.IsUntracked())
			return LOCTEXT("Untracked", "Untracked");
		if (aFile.Index == TEXT('R') || aFile.Working == TEXT('R'))
			return LOCTEXT("Renamed", "Renamed");
		if (aFile.Index == TEXT('D') || aFile.Working == TEXT('D'))
			return LOCTEXT("Deleted", "Deleted");
		if (aFile.IsStaged() && aFile.IsWorking())
			return LOCTEXT("StagedAndWorking", "Staged + working");
		return aFile.IsStaged() ? LOCTEXT("Staged", "Staged") : LOCTEXT("Working", "Working");
	}

	class SBlueprintDiffFileRow final : public SMultiColumnTableRow<TSharedPtr<const FBlueprintDiffProjectFile>>
	{
	public:
		using FFilePtr = TSharedPtr<const FBlueprintDiffProjectFile>;
		SLATE_BEGIN_ARGS(SBlueprintDiffFileRow) {}
			SLATE_ARGUMENT(FFilePtr, File)
		SLATE_END_ARGS()

		void Construct(const FArguments & aArgs, const TSharedRef<STableViewBase> & aOwner)
		{
			m_File = aArgs._File;
			SMultiColumnTableRow<FFilePtr>::Construct(FTableRowArgs()
				.Padding(FMargin(6, 4)).ToolTipText(FText::FromString(m_File->Path)), aOwner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName & aColumn) override
		{
			const FText lText = aColumn == TEXT("State") ? DescribeStatus(*m_File)
				: FText::FromString(aColumn == TEXT("Name") ? FPaths::GetCleanFilename(m_File->Path) : m_File->Path);
			return SNew(STextBlock).Text(lText).OverflowPolicy(ETextOverflowPolicy::Ellipsis);
		}

	private:
		FFilePtr m_File;
	};
}

void SBlueprintDiffChanges::Construct(const FArguments & aArgs)
{
	m_ContextFilename = aArgs._ContextFilename;
	m_Compact = aArgs._Compact;
	m_OnOpenBlueprint = aArgs._OnOpenBlueprint;
	m_OnBeforeReload = aArgs._OnBeforeReload;

	TSharedPtr<SHeaderRow> lHeader;
	float lFilterWidth = 0.19f;
	if (!m_Compact)
	{
		GConfig->GetBool(LAYOUT_SECTION, FILTER_VISIBLE, m_ShowFilters, GEditorPerProjectIni);
		float lSavedWidth = 0.0f;
		if (GConfig->GetFloat(LAYOUT_SECTION, FILTER_WIDTH, lSavedWidth, GEditorPerProjectIni)
			&& FMath::IsFinite(lSavedWidth) && lSavedWidth > 0.0f && lSavedWidth < 1.0f)
			lFilterWidth = lSavedWidth;
		lHeader = SNew(SHeaderRow)
			+ SHeaderRow::Column("Name").DefaultLabel(LOCTEXT("FileColumn", "File")).FillWidth(0.28f)
			+ SHeaderRow::Column("Path").DefaultLabel(LOCTEXT("PathColumn", "Repository path")).FillWidth(0.57f)
			+ SHeaderRow::Column("State").DefaultLabel(LOCTEXT("StateColumn", "State")).FillWidth(0.15f);
	}
	const TSharedRef<SVerticalBox> lContent = SNew(SVerticalBox)
		.Tag("BlueprintDiffChangedFiles")
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 5)
		.VAlign(VAlign_Center)
		[
			m_Compact ? DoBuildFilters(0) : StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 0, 6, 5)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Visibility(m_Compact ? EVisibility::Collapsed : EVisibility::Visible)
				.Text_Lambda([this] { return m_ShowFilters ? LOCTEXT("HideFilters", "Hide Filters") : LOCTEXT("ShowFilters", "Show Filters"); })
				.OnClicked(this, &SBlueprintDiffChanges::DoToggleFilters)
			]
			+ SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("FindFiles", "Find changed files"))
				.OnTextChanged_Lambda([this](const FText & aSearch) { m_Search = aSearch.ToString(); DoFilter(); })
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("RefreshTip", "Refresh Git changes"))
				.IsEnabled_Lambda([this] { return !m_Request.IsValid() && !m_StageRequest.IsValid(); })
				.OnClicked_Lambda([this] { m_Error = FText(); DoRefresh(); return FReply::Handled(); })
				[
					SNew(STextBlock).Text(LOCTEXT("Refresh", "Refresh"))
				]
			]
		]
		+ SVerticalBox::Slot().FillHeight(1)
		[
			SAssignNew(m_List, SListView<FFilePtr>)
			.Tag("BlueprintDiffChangedFileList")
			.HeaderRow(lHeader)
			.ListItemsSource(&m_VisibleFiles)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SBlueprintDiffChanges::DoBuildRow)
			.OnMouseButtonDoubleClick(this, &SBlueprintDiffChanges::DoOpen)
			.OnContextMenuOpening(this, &SBlueprintDiffChanges::DoBuildMenu)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 4)
		[
			SNew(STextBlock).Text(this, &SBlueprintDiffChanges::DoGetStatus)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground()).AutoWrapText(true)
		];
	if (m_Compact)
		ChildSlot.AttachWidget(lContent);
	else
	{
		ChildSlot
		[
			SAssignNew(m_FilterSplitter, SSplitter)
			.OnSplitterFinishedResizing(this, &SBlueprintDiffChanges::DoSaveFilterWidth)
			+ SSplitter::Slot().Value(lFilterWidth).MinSize(92.0f)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this] { return m_ShowFilters ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SVerticalBox::Slot().AutoHeight().Padding(6)
				[
					DoBuildFilters(1)
				]
			]
			+ SSplitter::Slot().Value(1.0f - lFilterWidth).MinSize(180.0f)
			[
				lContent
			]
		];
	}
	DoRefresh();
}

TSharedRef<SWidget> SBlueprintDiffChanges::DoBuildFilters(int32 aSegmentsPerLine)
{
	return SNew(SSegmentedControl<EFilter>)
		.MaxSegmentsPerLine(aSegmentsPerLine)
		.Value_Lambda([this] { return m_Filter; })
		.OnValueChanged_Lambda([this](EFilter aFilter) { m_Filter = aFilter; DoFilter(); })
		+ SSegmentedControl<EFilter>::Slot(EFilter::All).Text(LOCTEXT("All", "All"))
		+ SSegmentedControl<EFilter>::Slot(EFilter::Staged).Text(LOCTEXT("FilterStaged", "Staged"))
		+ SSegmentedControl<EFilter>::Slot(EFilter::Working).Text(LOCTEXT("FilterWorking", "Working"));
}

void SBlueprintDiffChanges::DoSaveFilterWidth()
{
	if (!m_ShowFilters)
		return;
	const float lLeft = m_FilterSplitter->SlotAt(0).GetSizeValue();
	const float lRight = m_FilterSplitter->SlotAt(1).GetSizeValue();
	if (lLeft > 0 && lRight > 0)
	{
		GConfig->SetFloat(LAYOUT_SECTION, FILTER_WIDTH, lLeft / (lLeft + lRight), GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
}

FReply SBlueprintDiffChanges::DoToggleFilters()
{
	m_ShowFilters = !m_ShowFilters;
	GConfig->SetBool(LAYOUT_SECTION, FILTER_VISIBLE, m_ShowFilters, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
	return FReply::Handled();
}

SBlueprintDiffChanges::~SBlueprintDiffChanges()
{
	if (m_CancelRequest)
		m_CancelRequest->Store(true);
}

void SBlueprintDiffChanges::Tick(const FGeometry & aGeometry, double aTime, float aDelta)
{
	SCompoundWidget::Tick(aGeometry, aTime, aDelta);
	if (m_StageRequest.IsValid())
	{
		if (!m_StageRequest.IsReady())
			return;
		m_Error = m_StageRequest.Consume();
		DoRestartRefresh();
	}
	if (m_Request.IsValid())
	{
		if (!m_Request.IsReady())
			return;
		FBlueprintDiffProjectChanges lResult = m_Request.Consume();
		m_CancelRequest.Reset();
		m_NextRefresh = FPlatformTime::Seconds() + (lResult.Error.IsEmpty() ? 5.0 : 20.0);
		m_HasReadChanges = true;
		m_RefreshError = MoveTemp(lResult.Error);
		bool lChanged = m_Files.Num() != lResult.Files.Num();
		for (int32 lIndex = 0; !lChanged && lIndex < m_Files.Num(); ++lIndex)
			lChanged = !(*m_Files[lIndex] == lResult.Files[lIndex]);
		if (lChanged)
		{
			m_Files.Reset(lResult.Files.Num());
			for (FBlueprintDiffProjectFile & lFile : lResult.Files)
				m_Files.Add(MakeShared<const FBlueprintDiffProjectFile>(MoveTemp(lFile)));
			DoFilter();
		}
	}
	if (FPlatformTime::Seconds() >= m_NextRefresh)
		DoRefresh();
}

void SBlueprintDiffChanges::DoRefresh()
{
	if (m_Request.IsValid() || m_StageRequest.IsValid())
		return;
	TArray<FString> lCandidates;
	if (!m_ContextFilename.IsEmpty())
		lCandidates.Add(FPaths::GetPath(m_ContextFilename));
	for (const TSharedRef<IPlugin> & lPlugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		if (lPlugin->GetLoadedFrom() == EPluginLoadedFrom::Project)
			lCandidates.Add(lPlugin->GetBaseDir());
	}
	m_CancelRequest = MakeShared<TAtomic<bool>>(false);
	m_Request = Async(EAsyncExecution::ThreadPool, [lExecutable = BlueprintDiffGit::GetExecutable(),
		lProject = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), lCandidates = MoveTemp(lCandidates), lCancel = m_CancelRequest]
	{
		return BlueprintDiffGit::ReadProjectChanges(lExecutable, lProject, lCandidates, lCancel.Get());
	});
}

void SBlueprintDiffChanges::DoRestartRefresh()
{
	if (m_CancelRequest)
		m_CancelRequest->Store(true);
	m_Request = {};
	DoRefresh();
}

bool SBlueprintDiffChanges::IsCurrentAsset(const FBlueprintDiffProjectFile & aFile) const
{
	return m_Compact && !m_ContextFilename.IsEmpty() && FPaths::IsSamePath(m_ContextFilename, aFile.Filename);
}

void SBlueprintDiffChanges::DoFilter()
{
	const TArray<FFilePtr> lSelected = m_List->GetSelectedItems();
	const FFilePtr lSelection = lSelected.IsEmpty() ? nullptr : lSelected[0];
	m_VisibleFiles.Reset();
	for (const FFilePtr & lFile : m_Files)
	{
		if ((m_Filter == EFilter::Working && !lFile->IsWorking()) || (m_Filter == EFilter::Staged && !lFile->IsStaged()))
			continue;
		if (m_Search.IsEmpty() || lFile->Path.Contains(m_Search) || lFile->OriginalPath.Contains(m_Search))
			m_VisibleFiles.Add(lFile);
	}
	m_List->RequestListRefresh();
	if (lSelection)
	{
		if (const FFilePtr * lMatch = m_VisibleFiles.FindByPredicate([&lSelection](const FFilePtr & aFile)
			{ return FPaths::IsSamePath(aFile->Filename, lSelection->Filename); }))
		{
			m_List->SetSelection(*lMatch);
			return;
		}
	}
	if (const FFilePtr * lCurrent = m_VisibleFiles.FindByPredicate([this](const FFilePtr & aFile) { return IsCurrentAsset(*aFile); }))
	{
		m_List->SetSelection(*lCurrent);
		m_List->RequestScrollIntoView(*lCurrent);
	}
	else
		m_List->ClearSelection();
}

FText SBlueprintDiffChanges::DoGetStatus() const
{
	if (!m_Error.IsEmpty())
		return m_Error;
	if (!m_RefreshError.IsEmpty())
		return m_RefreshError;
	if (m_StageRequest.IsValid())
		return LOCTEXT("Staging", "Staging file…");
	if (m_Request.IsValid() && !m_HasReadChanges)
		return LOCTEXT("Loading", "Reading Git changes…");
	return FText::Format(LOCTEXT("FileCount", "{0} of {1} changed files"), m_VisibleFiles.Num(), m_Files.Num());
}

TSharedRef<ITableRow> SBlueprintDiffChanges::DoBuildRow(FFilePtr aFile, const TSharedRef<STableViewBase> & aOwner)
{
	if (!m_Compact)
		return SNew(SBlueprintDiffFileRow, aOwner).File(aFile);
	return SNew(STableRow<FFilePtr>, aOwner).Padding(FMargin(6, 4)).ToolTipText(FText::FromString(aFile->Path))
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock).Text(FText::FromString(FPaths::GetCleanFilename(aFile->Path)))
			.ColorAndOpacity(IsCurrentAsset(*aFile) ? FSlateColor(FStyleColors::AccentBlue) : FSlateColor::UseForeground())
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("CompactDetails", "{0} · {1}"), DescribeStatus(*aFile), FText::FromString(FPaths::GetPath(aFile->Path))))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground()).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		]
	];
}

void SBlueprintDiffChanges::DoOpen(FFilePtr aFile)
{
	m_Error = FText();
	const FString & lFilename = aFile->Filename;
	if (!FPaths::FileExists(lFilename))
	{
		m_Error = LOCTEXT("FileMissing", "The working file is missing. Revert it to restore the file from Git.");
		return;
	}
	if (!BlueprintDiffGit::IsAssetFile(lFilename))
	{
		if (!FSourceCodeNavigation::OpenSourceFile(lFilename))
			m_Error = LOCTEXT("ExternalEditorMissing", "The configured source editor could not open this file.");
		return;
	}
	const FAssetData lAsset = ResolveAsset(lFilename);
	UObject * const lObject = lAsset.IsValid() ? lAsset.GetAsset() : nullptr;
	if (!lObject)
	{
		m_Error = LOCTEXT("AssetNotFound", "The file could not be loaded as an Unreal asset.");
		return;
	}
	if (m_OnOpenBlueprint.IsBound() && lObject->IsA<UBlueprint>())
		m_OnOpenBlueprint.Execute(lAsset);
	else if (!GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(lObject))
		m_Error = LOCTEXT("EditorFailed", "The asset editor could not open this file.");
}

TSharedPtr<SWidget> SBlueprintDiffChanges::DoBuildMenu()
{
	const TArray<FFilePtr> lSelection = m_List->GetSelectedItems();
	if (lSelection.IsEmpty())
		return nullptr;
	const FFilePtr lFile = lSelection[0];
	const FString lFilename = lFile->Filename;
	FMenuBuilder lMenu(true, nullptr);
	lMenu.AddMenuEntry(LOCTEXT("OpenFile", "Open File"), LOCTEXT("OpenFileTip", "Open the working file for editing."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffChanges::DoOpen, lFile),
			FCanExecuteAction::CreateLambda([lFilename] { return FPaths::FileExists(lFilename); })));
	if (BlueprintDiffGit::IsAssetFile(lFilename))
	{
		lMenu.AddMenuEntry(LOCTEXT("BrowseToAsset", "Browse to Asset"),
			LOCTEXT("BrowseToAssetTip", "Locate and select this asset in the Content Browser."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffChanges::DoBrowse, lFile),
				FCanExecuteAction::CreateLambda([lFilename] { return FPaths::FileExists(lFilename); })));
	}
	lMenu.AddMenuEntry(LOCTEXT("StageFile", "Stage File"),
		LOCTEXT("StageFileTip", "Add the saved working changes for the selected file to the Git index."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffChanges::DoStage, lFile),
			FCanExecuteAction::CreateSP(this, &SBlueprintDiffChanges::CanStage, lFile)));
	lMenu.AddMenuEntry(lFile->IsUntracked() ? LOCTEXT("RemoveUntracked", "Remove Untracked File…") : LOCTEXT("RevertFile", "Revert File to HEAD…"),
		LOCTEXT("RevertFileTip", "Discard the selected file's Git changes."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SBlueprintDiffChanges::DoRevert, lFile),
			FCanExecuteAction::CreateLambda([lFile] { return !lFile->IsConflicted(); })));
	return lMenu.MakeWidget();
}

void SBlueprintDiffChanges::DoBrowse(FFilePtr aFile)
{
	m_Error = FText();
	const FAssetData lAsset = ResolveAsset(aFile->Filename);
	if (lAsset.IsValid())
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets({ lAsset });
	else
		m_Error = LOCTEXT("AssetNotFound", "The file could not be loaded as an Unreal asset.");
}

bool SBlueprintDiffChanges::CanStage(FFilePtr aFile) const
{
	return !m_StageRequest.IsValid() && aFile->IsWorking() && !aFile->IsConflicted();
}

void SBlueprintDiffChanges::DoStage(FFilePtr aFile)
{
	if (!CanStage(aFile))
		return;
	m_Error = FText();
	m_StageRequest = Async(EAsyncExecution::ThreadPool, [lExecutable = BlueprintDiffGit::GetExecutable(), aFile]
	{
		return BlueprintDiffGit::StageFile(lExecutable, *aFile);
	});
}

void SBlueprintDiffChanges::DoRevert(FFilePtr aFile)
{
	if (m_StageRequest.IsValid())
		return;
	const TSharedRef<SWidget> lKeepAlive = AsShared();
	const FString & lRepository = aFile->Repository;
	TArray<FString> lFiles { aFile->Filename };
	if (aFile->Index == TEXT('R') || aFile->Working == TEXT('R'))
		lFiles.Add(lRepository / aFile->OriginalPath);
	const FText lPrompt = aFile->IsUntracked()
		? FText::Format(LOCTEXT("ConfirmDelete", "Permanently remove the untracked file {0}?"), FText::FromString(aFile->Path))
		: FText::Format(LOCTEXT("ConfirmRevert", "Restore {0} to HEAD? This discards both staged and working changes for this file."),
			FText::FromString(aFile->Path));
	if (FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, lPrompt) != EAppReturnType::Yes)
		return;
	if (GEditor->PlayWorld)
	{
		m_Error = LOCTEXT("RevertDuringPlay", "Stop Play in Editor before reverting files.");
		return;
	}
	bool lHasPackages = false;
	for (const FString & lFile : lFiles)
	{
		const FString lPackageName = PackageNameForFile(lFile);
		lHasPackages |= !lPackageName.IsEmpty();
		if (const UPackage * lPackage = lPackageName.IsEmpty() ? nullptr : FindPackage(nullptr, *lPackageName))
		{
			if (lPackage->IsDirty())
			{
				m_Error = LOCTEXT("DirtyPackage", "Save or close the asset with unsaved changes before reverting it.");
				return;
			}
		}
	}
	m_Error = FText();
	const auto lRevert = [&](const TArray<FString> & aEligibleFiles)
	{
		if (aEligibleFiles.Num() != lFiles.Num())
		{
			m_Error = LOCTEXT("LoadedLevel", "Close the affected level before reverting its files.");
			return false;
		}
		m_Error = BlueprintDiffGit::RevertFile(BlueprintDiffGit::GetExecutable(), lRepository, *aFile);
		if (m_Error.IsEmpty())
			m_OnBeforeReload.ExecuteIfBound(lFiles);
		return m_Error.IsEmpty();
	};
	if (lHasPackages)
		USourceControlHelpers::ApplyOperationAndReloadPackages(lFiles, lRevert);
	else
		lRevert(lFiles);
	if (!m_Error.IsEmpty())
		FMessageDialog::Open(EAppMsgType::Ok, m_Error);
	DoRestartRefresh();
}

#undef LOCTEXT_NAMESPACE
