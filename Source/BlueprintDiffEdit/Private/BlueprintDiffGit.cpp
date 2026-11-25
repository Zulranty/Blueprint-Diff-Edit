#include "BlueprintDiffGit.h"

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeExit.h"
#include "Serialization/Archive.h"
#include "SourceControlHelpers.h"

#define LOCTEXT_NAMESPACE "BlueprintDiffGit"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintDiffGit, Log, All);

namespace
{
	constexpr ANSICHAR LFS_POINTER_PREFIX[] = "version https://git-lfs.github.com/spec/v1";
	constexpr int32 PROBE_SIZE = sizeof(LFS_POINTER_PREFIX) - 1;

	struct FGitRequest
	{
		const FString & Executable;
		const TAtomic<bool> * Cancelled;
		const FString * Destination;
		const double Deadline = FPlatformTime::Seconds() + 60.0;
	};

	struct FGitResult
	{
		int ExitCode = INDEX_NONE;
		FString Failure;

		bool Succeeded() const { return ExitCode == 0 && Failure.IsEmpty(); }
	};

	FGitResult RunGit(const FGitRequest & aRequest, const FString & aDirectory, const FString & aArguments,
		TArray<uint8> & aOutput, FArchive * aFile = nullptr)
	{
		aOutput.Reset();
		if (aRequest.Cancelled && aRequest.Cancelled->Load())
			return { INDEX_NONE, TEXT("Cancelled") };
		if (FPlatformTime::Seconds() >= aRequest.Deadline)
			return { INDEX_NONE, TEXT("Timed out") };

		void * lOutputRead = nullptr;
		void * lOutputWrite = nullptr;
		void * lErrorRead = nullptr;
		void * lErrorWrite = nullptr;
		ON_SCOPE_EXIT
		{
			FPlatformProcess::ClosePipe(lOutputRead, lOutputWrite);
			FPlatformProcess::ClosePipe(lErrorRead, lErrorWrite);
		};

		if (!FPlatformProcess::CreatePipe(lOutputRead, lOutputWrite) || !FPlatformProcess::CreatePipe(lErrorRead, lErrorWrite))
			return { INDEX_NONE, TEXT("Could not create Git process pipes") };

		FProcHandle lProcess = FPlatformProcess::CreateProc(*aRequest.Executable, *aArguments, true, true, true,
			nullptr, 0, *aDirectory, lOutputWrite, nullptr, lErrorWrite);
		if (!lProcess.IsValid())
			return { INDEX_NONE, TEXT("Could not start Git") };
		ON_SCOPE_EXIT { FPlatformProcess::CloseProc(lProcess); };

		TArray<uint8> lChunk;
		TArray<uint8> lDiagnostics;
		while (true)
		{
			if ((aRequest.Cancelled && aRequest.Cancelled->Load()) || FPlatformTime::Seconds() >= aRequest.Deadline)
			{
				FPlatformProcess::TerminateProc(lProcess, true);
				return { INDEX_NONE, aRequest.Cancelled && aRequest.Cancelled->Load() ? TEXT("Cancelled") : TEXT("Timed out") };
			}

			const bool lRunning = FPlatformProcess::IsProcRunning(lProcess);
			const bool lReadOutput = FPlatformProcess::ReadPipeToArray(lOutputRead, lChunk);
			if (lReadOutput)
			{
				if (aFile)
				{
					aOutput.Append(lChunk.GetData(), FMath::Min(lChunk.Num(), PROBE_SIZE - aOutput.Num()));
					aFile->Serialize(lChunk.GetData(), lChunk.Num());
					if (aFile->IsError())
					{
						FPlatformProcess::TerminateProc(lProcess, true);
						return { INDEX_NONE, TEXT("Could not write the revision snapshot") };
					}
				}
				else
				{
					if (lChunk.Num() > MAX_int32 - aOutput.Num())
					{
						FPlatformProcess::TerminateProc(lProcess, true);
						return { INDEX_NONE, TEXT("The revision exceeds the supported in-memory buffer size") };
					}
					aOutput.Append(lChunk);
				}
			}
			const bool lReadError = FPlatformProcess::ReadPipeToArray(lErrorRead, lChunk);
			if (lReadError)
				lDiagnostics.Append(lChunk.GetData(), FMath::Min(lChunk.Num(), 4096 - lDiagnostics.Num()));
			if (!lRunning && !lReadOutput && !lReadError)
				break;
			if (!lReadOutput && !lReadError)
				FPlatformProcess::SleepNoStats(0.005f);
		}

		FGitResult lResult;
		if (!FPlatformProcess::GetProcReturnCode(lProcess, &lResult.ExitCode))
			lResult.Failure = TEXT("Could not determine the Git exit code");
		else if (lResult.ExitCode != 0 && !lDiagnostics.IsEmpty())
		{
			const FUTF8ToTCHAR lError(reinterpret_cast<const UTF8CHAR *>(lDiagnostics.GetData()), lDiagnostics.Num());
			FString lMessage(lError.Length(), lError.Get());
			lMessage.ReplaceInline(TEXT("\r"), TEXT(" "));
			lMessage.ReplaceInline(TEXT("\n"), TEXT(" "));
			// Git errors may include credential-helper or remote output. Keep that text out of editor status and logs.
			UE_LOG(LogBlueprintDiffGit, Verbose, TEXT("A Git command failed with code %d and %d diagnostic bytes."),
				lResult.ExitCode, lDiagnostics.Num());
			if (lMessage.Contains(TEXT("git-lfs")) || lMessage.Contains(TEXT("smudge filter lfs")))
				lResult.Failure = TEXT("Git LFS filter failed");
		}
		return lResult;
	}

	FGitResult RunGitText(const FGitRequest & aRequest, const FString & aDirectory, const FString & aArguments, FString & aOutput)
	{
		TArray<uint8> lOutput;
		FGitResult lResult = RunGit(aRequest, aDirectory, aArguments, lOutput);
		aOutput.Reset();
		if (lResult.Succeeded() && !lOutput.IsEmpty())
		{
			const FUTF8ToTCHAR lText(reinterpret_cast<const UTF8CHAR *>(lOutput.GetData()), lOutput.Num());
			aOutput = FString(lText.Length(), lText.Get());
		}
		return lResult;
	}

	FText DescribeFailure(const FText & aStage, const FGitResult & aResult)
	{
		const FText lDetail = aResult.Failure.IsEmpty()
			? FText::Format(LOCTEXT("GitExitCode", "Git exit code {0}"), aResult.ExitCode)
			: FText::FromString(aResult.Failure);
		return FText::Format(LOCTEXT("GitFailure", "{0} ({1})."), aStage, lDetail);
	}

	bool IsCommitHash(const FString & aValue, bool aAllowAbbreviated = false)
	{
		if (aAllowAbbreviated ? aValue.Len() < 4 || aValue.Len() > 64 : aValue.Len() != 40 && aValue.Len() != 64)
			return false;
		for (const TCHAR lChar : aValue)
		{
			if (!FChar::IsHexDigit(lChar))
				return false;
		}
		return true;
	}

	bool LocateRepository(const FGitRequest & aRequest, const FString & aFilename, FString & aRoot, FText & aError)
	{
		FString lDirectory = FPaths::GetPath(aFilename);
		// An unsaved or deleted asset can have a path below a directory absent from the working tree.
		while (!lDirectory.IsEmpty() && !IFileManager::Get().DirectoryExists(*lDirectory))
		{
			FString lParent = FPaths::GetPath(lDirectory);
			if (lParent == lDirectory)
				break;
			lDirectory = MoveTemp(lParent);
		}
		if (lDirectory.IsEmpty())
		{
			aError = LOCTEXT("MissingRepositoryDirectory", "Could not locate the working directory for this Blueprint.");
			return false;
		}
		const FGitResult lResult = RunGitText(aRequest, lDirectory, TEXT("rev-parse --show-toplevel"), aRoot);
		if (!lResult.Succeeded() || aRoot.IsEmpty())
		{
			aError = DescribeFailure(LOCTEXT("GitRepositoryUnavailable", "Could not find the Git repository for this Blueprint"), lResult);
			return false;
		}
		aRoot.TrimStartAndEndInline();
		FPaths::NormalizeDirectoryName(aRoot);
		return true;
	}

	bool ResolveCommit(const FGitRequest & aRequest, const FString & aRoot, const FString & aRef, FString & aCommit, FText & aError)
	{
		const FGitResult lResult = RunGitText(aRequest, aRoot, TEXT("rev-parse --verify --quiet ") + aRef + TEXT("^{commit}"), aCommit);
		aCommit.TrimStartAndEndInline();
		if (lResult.Succeeded() && IsCommitHash(aCommit))
			return true;
		aError = DescribeFailure(LOCTEXT("GitCommitUnavailable", "Could not resolve the selected Git commit"), lResult);
		return false;
	}

	bool MakeRepositoryRelative(const FString & aRoot, FString & aFilename)
	{
		FPaths::NormalizeFilename(aFilename);
		if (!FPaths::IsRelative(aFilename) && !FPaths::MakePathRelativeTo(aFilename, *(aRoot / TEXT(".git"))))
			return false;
		return !aFilename.IsEmpty() && FPaths::IsRelative(aFilename)
			&& !aFilename.StartsWith(TEXT("../")) && !aFilename.Contains(TEXT("\""));
	}

	void ReadPackage(const FGitRequest & aRequest, const FString & aRoot, const FString & aCommit,
		FString aFilename, FBlueprintDiffMaster & aResult)
	{
		if (!MakeRepositoryRelative(aRoot, aFilename))
		{
			aResult.Error = LOCTEXT("OutsideRepository", "The revision path is invalid or outside the Git working tree.");
			return;
		}

		aResult.SourceFilename = aRoot / aFilename;
		const FString lBlob = FString::Printf(TEXT("\"%s:%s\""), *aCommit, *aFilename);
		const FString lRead = FString::Printf(TEXT("--attr-source=%s cat-file --filters %s"), *aCommit, *lBlob);
		TUniquePtr<FArchive> lWriter;
		if (aRequest.Destination)
		{
			lWriter.Reset(IFileManager::Get().CreateFileWriter(**aRequest.Destination));
			if (!lWriter)
			{
				aResult.Error = LOCTEXT("OpenSnapshotFailed", "Could not create the temporary Blueprint snapshot.");
				return;
			}
		}
		const FGitResult lResult = RunGit(aRequest, aRoot, lRead, aResult.PackageBytes, lWriter.Get());
		const bool lWritten = !lWriter || lWriter->Close();
		if (!lResult.Succeeded())
		{
			aResult.PackageBytes.Reset();
			aResult.Error = DescribeFailure(LOCTEXT("GitReadFailed", "Could not read the Blueprint at the selected Git revision"), lResult);
			return;
		}

		if (!lWritten)
		{
			aResult.PackageBytes.Reset();
			aResult.Error = LOCTEXT("CloseSnapshotFailed", "Could not finish writing the temporary Blueprint snapshot.");
			return;
		}
		if (aResult.PackageBytes.IsEmpty())
		{
			aResult.Error = LOCTEXT("EmptyGitRevision", "Git returned an empty Blueprint package.");
			return;
		}
		if (aResult.PackageBytes.Num() >= PROBE_SIZE
			&& FMemory::Memcmp(aResult.PackageBytes.GetData(), LFS_POINTER_PREFIX, PROBE_SIZE) == 0)
		{
			aResult.PackageBytes.Reset();
			aResult.Error = LOCTEXT("LfsPointerOnly", "Git returned an LFS pointer. Enable Git LFS smudging to load the Blueprint data.");
		}
		if (aRequest.Destination)
			aResult.PackageBytes.Reset();
	}
	bool ParseFileHistory(const TArray<uint8> & aOutput, const FString & aRepository,
		FString aTrackedPath, TArray<FBlueprintDiffGitRevision> & aRevisions)
	{
		// The leading NUL in the format marks each commit. Git adds one newline before the name-status record.
		TArray<FString> lFields;
		int32 lStart = 0;
		for (int32 lIndex = 0; lIndex < aOutput.Num(); ++lIndex)
		{
			if (aOutput[lIndex] != 0)
				continue;

			const FUTF8ToTCHAR lField(reinterpret_cast<const UTF8CHAR *>(aOutput.GetData() + lStart), lIndex - lStart);
			lFields.Emplace(lField.Length(), lField.Get());
			lStart = lIndex + 1;
		}
		if (lStart != aOutput.Num())
			return false;

		for (int32 lCursor = 0; lCursor < lFields.Num(); )
		{
			if (!lFields[lCursor].IsEmpty() || lFields.Num() - lCursor < 6
				|| !IsCommitHash(lFields[lCursor + 1]) || !IsCommitHash(lFields[lCursor + 2], true)
				|| !lFields[lCursor + 1].StartsWith(lFields[lCursor + 2], ESearchCase::CaseSensitive))
				return false;

			const int32 lCommit = ++lCursor;
			lCursor += 5;
			bool lFoundFile = false;
			// --follow emits only the tracked path (possibly the old and new names for a rename).
			while (lCursor < lFields.Num() && !lFields[lCursor].IsEmpty())
			{
				FString lAction = lFields[lCursor++].TrimStartAndEnd();
				if (lAction.IsEmpty() || !lFields.IsValidIndex(lCursor))
					return false;
				const FString & lOldPath = lFields[lCursor++];
				const bool lRename = lAction[0] == TEXT('R') || lAction[0] == TEXT('C');
				if (lRename && !lFields.IsValidIndex(lCursor))
					return false;
				const FString & lPath = lRename ? lFields[lCursor++] : lOldPath;
				if (!lPath.Equals(aTrackedPath, ESearchCase::CaseSensitive))
					continue;
				if (lFoundFile)
					return false;
				lFoundFile = true;
				if (lAction[0] != TEXT('D'))
				{
					aRevisions.Add({ aRepository, lFields[lCommit], lPath, lFields[lCommit + 2], {
						lFields[lCommit + 1].Left(12), BlueprintDiffGit::GetSubject(lFields[lCommit + 2]), lFields[lCommit + 3],
						lFields[lCommit + 4], {} } });
				}
				if (lRename)
					aTrackedPath = lOldPath;
			}
		}
		return true;
	}

	FBlueprintDiffFileHistory ReadFileHistoryBatch(const FGitRequest & aRequest, const FString & aRepository,
		const FString & aHead, const FString & aPath, int32 aLimit)
	{
		FBlueprintDiffFileHistory lHistory;
		const FString lArgs = FString::Printf(
			TEXT("--literal-pathspecs log --no-color --no-show-signature --no-notes --encoding=UTF-8 --follow --name-status -z ")
			TEXT("--format=tformat:%%x00%%H%%x00%%h%%x00%%B%%x00%%an%%x00%%ai --max-count=%d %s -- \"%s\""),
			aLimit, *aHead, *aPath);
		TArray<uint8> lOutput;
		const FGitResult lResult = RunGit(aRequest, aRepository, lArgs, lOutput);
		if (!lResult.Succeeded())
		{
			lHistory.Error = DescribeFailure(LOCTEXT("GitHistoryFailed", "Could not read Git revision history"), lResult);
			lHistory.bUseProvider = true;
		}
		else if (!ParseFileHistory(lOutput, aRepository, aPath, lHistory.Revisions))
		{
			lHistory.Revisions.Reset();
			lHistory.Error = LOCTEXT("InvalidGitHistory", "The Git history response could not be read safely.");
			lHistory.bUseProvider = true;
		}
		return lHistory;
	}
}

FText FBlueprintDiffCommit::GetMenuLabel(bool aShowDate) const
{
	TArray<FString> lSegments;
	for (const FString * const lValue : { &Ref, &Hash, &Subject, &Author, aShowDate ? &Date : nullptr })
	{
		if (lValue && !lValue->IsEmpty())
			lSegments.Add(*lValue);
	}
	return FText::FromString(FString::Join(lSegments, TEXT("  ·  ")));
}

FString BlueprintDiffGit::GetExecutable()
{
	FString lConfiguredGit;
	GConfig->GetString(TEXT("GitSourceControl.GitSourceControlSettings"), TEXT("BinaryPath"),
		lConfiguredGit, USourceControlHelpers::GetSettingsIni());
	if (!lConfiguredGit.IsEmpty() && IFileManager::Get().FileExists(*lConfiguredGit))
		return lConfiguredGit;

	return PLATFORM_WINDOWS ? TEXT("git.exe") : TEXT("git");
}

FString BlueprintDiffGit::GetSubject(const FString & aDescription)
{
	const FString lDescription = aDescription.TrimStartAndEnd();
	const int lEnd = lDescription.Len();
	for (int lIndex = 0; lIndex < lEnd; ++lIndex)
	{
		if (lDescription[lIndex] == TCHAR('\r') || lDescription[lIndex] == TCHAR('\n'))
			return lDescription.Left(lIndex).TrimEnd();
	}
	return lDescription;
}

FBlueprintDiffMaster BlueprintDiffGit::ReadMaster(const FString & aExecutable, const FString & aFilename,
	const TAtomic<bool> * aCancelled, const FString * aDestination)
{
	FBlueprintDiffMaster lResult;
	const FGitRequest lRequest { aExecutable, aCancelled, aDestination };
	FString lRoot;
	if (!LocateRepository(lRequest, aFilename, lRoot, lResult.Error))
		return lResult;

	FString lUnused;
	FString lRef = TEXT("refs/remotes/origin/master");
	const FGitResult lRemote = RunGitText(lRequest, lRoot, TEXT("show-ref --verify --quiet ") + lRef, lUnused);
	if (lRemote.ExitCode == 1 && lRemote.Failure.IsEmpty())
	{
		lRef = TEXT("refs/heads/master");
		lResult.Commit.Ref = TEXT("master");
	}
	else if (lRemote.Succeeded())
		lResult.Commit.Ref = TEXT("origin/master");
	else
	{
		lResult.Error = DescribeFailure(LOCTEXT("MasterRefUnavailable", "Could not check origin/master"), lRemote);
		return lResult;
	}

	FString lCommit;
	if (!ResolveCommit(lRequest, lRoot, lRef, lCommit, lResult.Error))
		return lResult;

	FString lMetadata;
	const FString lShow = FString::Printf(TEXT("-c i18n.logOutputEncoding=UTF-8 show -s --no-show-signature --format=%%h%%n%%s%%n%%an%%n%%ai %s --"), *lCommit);
	const FGitResult lMetadataResult = RunGitText(lRequest, lRoot, lShow, lMetadata);
	TArray<FString> lLines;
	if (lMetadataResult.Succeeded())
	{
		lMetadata.ReplaceInline(TEXT("\r"), TEXT(""));
		lMetadata.ParseIntoArray(lLines, TEXT("\n"), false);
	}
	if (lLines.Num() >= 4)
	{
		lResult.Commit.Hash = MoveTemp(lLines[0]);
		lResult.Commit.Subject = MoveTemp(lLines[1]);
		lResult.Commit.Author = MoveTemp(lLines[2]);
		lResult.Commit.Date = MoveTemp(lLines[3]);
	}
	else
	{
		lResult.Commit.Hash = lCommit.Left(12);
		lResult.Commit.Subject = LOCTEXT("MetadataUnavailable", "Commit metadata unavailable").ToString();
	}
	ReadPackage(lRequest, lRoot, lCommit, aFilename, lResult);
	return lResult;
}

FBlueprintDiffMaster BlueprintDiffGit::ReadHistory(const FString & aExecutable, const FString & aWorkingFilename,
	const FString & aRevisionFilename, const FString & aRevision, const TAtomic<bool> * aCancelled, const FString * aDestination)
{
	FBlueprintDiffMaster lResult;
	if (!IsCommitHash(aRevision, true))
	{
		lResult.Error = LOCTEXT("InvalidHistoryCommit", "Git returned an invalid revision identifier.");
		return lResult;
	}

	const FGitRequest lRequest { aExecutable, aCancelled, aDestination };
	FString lRoot;
	FString lCommit;
	if (LocateRepository(lRequest, aWorkingFilename, lRoot, lResult.Error)
		&& ResolveCommit(lRequest, lRoot, aRevision, lCommit, lResult.Error))
	{
		ReadPackage(lRequest, lRoot, lCommit, aRevisionFilename, lResult);
	}
	return lResult;
}

FBlueprintDiffFileHistory BlueprintDiffGit::ReadFileHistory(const FString & aExecutable, const FString & aFilename,
	TFunctionRef<void(TOptional<FBlueprintDiffGitRevision>)> aPublishLatest, const TAtomic<bool> * aCancelled, bool aPreload)
{
	const FGitRequest lRequest { aExecutable, aCancelled, nullptr };
	FBlueprintDiffFileHistory lHistory;
	FString lRoot;
	FString lPath = aFilename;
	if (!LocateRepository(lRequest, aFilename, lRoot, lHistory.Error) || !MakeRepositoryRelative(lRoot, lPath))
	{
		aPublishLatest({});
		lHistory.bUseProvider = true;
		return lHistory;
	}

	// Keep the provider's conflict/MERGE_HEAD history for unmerged files.
	TArray<uint8> lUnmerged;
	const FGitResult lIndex = RunGit(lRequest, lRoot, FString::Printf(TEXT("--literal-pathspecs ls-files --unmerged -z -- \"%s\""), *lPath), lUnmerged);
	if (!lIndex.Succeeded() || !lUnmerged.IsEmpty())
	{
		aPublishLatest({});
		lHistory.bUseProvider = true;
		return lHistory;
	}

	FString lHead;
	const FGitResult lHeadResult = RunGitText(lRequest, lRoot, TEXT("rev-parse --verify --quiet HEAD"), lHead);
	lHead.TrimStartAndEndInline();
	if (!lHeadResult.Succeeded() || !IsCommitHash(lHead))
	{
		aPublishLatest({});
		// An empty repository has no committed revision to load.
		lHistory.bUseProvider = lHeadResult.ExitCode != 1 || !lHeadResult.Failure.IsEmpty();
		return lHistory;
	}

	if (aPreload)
	{
		FBlueprintDiffFileHistory lLatest = ReadFileHistoryBatch(lRequest, lRoot, lHead, lPath, 1);
		if (lLatest.Revisions.IsEmpty())
			aPublishLatest({});
		else
			aPublishLatest(MoveTemp(lLatest.Revisions[0]));
		if (lLatest.bUseProvider)
			return lLatest;
	}
	else
		aPublishLatest({});

	constexpr int32 HISTORY_LIMIT = 250; // Match the installed Git provider's history menu limit.
	return ReadFileHistoryBatch(lRequest, lRoot, lHead, lPath, HISTORY_LIMIT);
}

FBlueprintDiffMaster BlueprintDiffGit::ReadHistoryEntry(const FString & aExecutable,
	const FBlueprintDiffGitRevision & aRevision, const TAtomic<bool> * aCancelled, const FString * aDestination)
{
	FBlueprintDiffMaster lResult;
	lResult.Commit = aRevision.Commit;
	if (!IsCommitHash(aRevision.CommitId))
	{
		lResult.Error = LOCTEXT("InvalidHistoryCommit", "Git returned an invalid revision identifier.");
		return lResult;
	}
	const FGitRequest lRequest { aExecutable, aCancelled, aDestination };
	ReadPackage(lRequest, aRevision.Repository, aRevision.CommitId, aRevision.Filename, lResult);
	return lResult;
}

#undef LOCTEXT_NAMESPACE
