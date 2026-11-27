#include "BlueprintDiffGit.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/** Verifies the same revision formatting and Git/LFS reader consumed by the diff panel. */
BEGIN_DEFINE_SPEC(FBlueprintDiffGitTests, "BlueprintDiffEdit.Git",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	FString m_Fixture;
	bool RunFixtureGit(const FString & aArguments, FString * aOutput = nullptr);
END_DEFINE_SPEC(FBlueprintDiffGitTests)

bool FBlueprintDiffGitTests::RunFixtureGit(const FString & aArguments, FString * aOutput)
{
	const FString lGit = BlueprintDiffGit::GetExecutable();
	const FString lCommand = FString::Printf(TEXT("-C \"%s\" -c core.hooksPath=.git/no-hooks -c user.name=\"Ирина Test\" -c user.email=diff@example.invalid -c commit.gpgsign=false %s"),
		*m_Fixture, *aArguments);
	FString lOutput;
	FString lError;
	int lCode = INDEX_NONE;
	const bool lStarted = FPlatformProcess::ExecProcess(*lGit, *lCommand, &lCode, &lOutput, &lError);
	if (aOutput)
		*aOutput = lOutput.TrimStartAndEnd();
	if (!lStarted || lCode != 0)
	{
		AddError(FString::Printf(TEXT("Fixture command '%s' failed: %s"), *aArguments, *lError));
		return false;
	}
	return true;
}

void FBlueprintDiffGitTests::Define()
{
	It("Uses the first line and keeps Unicode in revision labels", [this]
	{
		const FString lSubject = BlueprintDiffGit::GetSubject(TEXT("   Откат функции\r\n\r\nLong commit body"));
		TestEqual(TEXT("Commit subject only"), lSubject, FString(TEXT("Откат функции")));
		TestEqual(TEXT("Only whitespace gives an empty label segment"), BlueprintDiffGit::GetSubject(TEXT("\r\n ")), FString());
		const FBlueprintDiffCommit lCommit{ TEXT("abcd1234"), lSubject, TEXT("Ирина Test"), TEXT("2026-11-23 15:00"), TEXT("origin/master") };
		TestEqual(TEXT("Commit item includes ref, hash, subject, author and date"), lCommit.GetMenuLabel(true).ToString(),
			FString(TEXT("origin/master  ·  abcd1234  ·  Откат функции  ·  Ирина Test  ·  2026-11-23 15:00")));
	});

	It("Reads the tracked master commit with historical LFS attributes and preserves the working file", [this]
	{
		const FString lFilename = m_Fixture / TEXT("Nested Project/Asset Snapshot.uasset");
		const FString lAttrs = m_Fixture / TEXT(".gitattributes");
		const TArray<uint8> REMOTE_BYTES{ 0xc1, 0x83, 0x2a, 0x9e, 0x00, 0x0d, 0x0a, 0xff };
		const TArray<uint8> LOCAL_BYTES{ 0xc1, 0x83, 0x2a, 0x9e, 0x88, 0x00 };
		const TArray<uint8> WORKING_BYTES{ 0xc1, 0x83, 0x2a, 0x9e, 0x11, 0x00 };

		// Store the remote fixture through LFS, including NUL bytes and non-ASCII bytes.
		if (!RunFixtureGit(TEXT("init --initial-branch=master")) || !RunFixtureGit(TEXT("lfs install --local --skip-repo")))
			return;
		if (!TestTrue(TEXT("Fixture LFS rules saved"), FFileHelper::SaveStringToFile(
			TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text\n"), *lAttrs, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			|| !TestTrue(TEXT("Remote Blueprint bytes saved"), FFileHelper::SaveArrayToFile(REMOTE_BYTES, *lFilename))
			|| !RunFixtureGit(TEXT("add -- .gitattributes \"Nested Project/Asset Snapshot.uasset\""))
			|| !RunFixtureGit(TEXT("commit -m \"Первый коммит\" -m \"Details below the subject\"")))
		{
			return;
		}

		FString lCommitHash;
		FString lPointer;
		if (!RunFixtureGit(TEXT("rev-parse HEAD"), &lCommitHash)
			|| !RunFixtureGit(TEXT("cat-file -p \"HEAD:Nested Project/Asset Snapshot.uasset\""), &lPointer)
			|| !TestTrue(TEXT("The committed blob is an LFS pointer"), lPointer.StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")))
			|| !RunFixtureGit(TEXT("update-ref refs/remotes/origin/master ") + lCommitHash))
		{
			return;
		}

		// Advance local master separately and remove the LFS rule in the workspace.
		if (!TestTrue(TEXT("Working LFS rules removed"), FFileHelper::SaveStringToFile(TEXT("# no workspace filter\n"),
			*lAttrs, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			|| !TestTrue(TEXT("Local master bytes saved"), FFileHelper::SaveArrayToFile(LOCAL_BYTES, *lFilename))
			|| !RunFixtureGit(TEXT("add -- .gitattributes \"Nested Project/Asset Snapshot.uasset\""))
			|| !RunFixtureGit(TEXT("commit -m \"Different local master\""))
			|| !TestTrue(TEXT("Uncommitted workspace bytes saved"), FFileHelper::SaveArrayToFile(WORKING_BYTES, *lFilename)))
		{
			return;
		}

		const FBlueprintDiffMaster lRemote = BlueprintDiffGit::ReadMaster(BlueprintDiffGit::GetExecutable(), lFilename);
		if (!TestTrue(TEXT("The tracked master revision loaded"), lRemote.Error.IsEmpty()))
		{
			AddInfo(lRemote.Error.ToString());
			return;
		}
		TestEqual(TEXT("Tracking ref wins"), lRemote.Commit.Ref, FString(TEXT("origin/master")));
		TestTrue(TEXT("Commit hash is a nonempty hexadecimal abbreviation"),
			lRemote.Commit.Hash.Len() >= 4 && lCommitHash.StartsWith(lRemote.Commit.Hash, ESearchCase::CaseSensitive));
		TestEqual(TEXT("Unicode subject"), lRemote.Commit.Subject, FString(TEXT("Первый коммит")));
		TestEqual(TEXT("Unicode author"), lRemote.Commit.Author, FString(TEXT("Ирина Test")));
		TestTrue(TEXT("Exact remote LFS payload returned"), lRemote.PackageBytes == REMOTE_BYTES);
		const FBlueprintDiffMaster lHistory = BlueprintDiffGit::ReadHistory(BlueprintDiffGit::GetExecutable(),
			lFilename, TEXT("Nested Project/Asset Snapshot.uasset"), lCommitHash.Left(12));
		TestTrue(TEXT("History applies the attributes from the old revision"), lHistory.Error.IsEmpty()
			&& lHistory.PackageBytes == REMOTE_BYTES);

		TArray<uint8> lUnchanged;
		TestTrue(TEXT("Workspace file remains readable"), FFileHelper::LoadFileToArray(lUnchanged, *lFilename));
		TestTrue(TEXT("Workspace file is unchanged"), lUnchanged == WORKING_BYTES);

		// When the remote-tracking ref is absent, the local master commit is the baseline.
		if (!RunFixtureGit(TEXT("update-ref -d refs/remotes/origin/master")))
			return;
		const FBlueprintDiffMaster lLocal = BlueprintDiffGit::ReadMaster(BlueprintDiffGit::GetExecutable(), lFilename);
		TestTrue(TEXT("Local master revision loaded"), lLocal.Error.IsEmpty());
		TestEqual(TEXT("Falls back to local master"), lLocal.Commit.Ref, FString(TEXT("master")));
		TestTrue(TEXT("Exact local master payload returned"), lLocal.PackageBytes == LOCAL_BYTES);

		const FBlueprintDiffMaster lMissing = BlueprintDiffGit::ReadMaster(BlueprintDiffGit::GetExecutable(),
			m_Fixture / TEXT("Nested Project/Missing.uasset"));
		TestTrue(TEXT("A missing asset reports an error"), !lMissing.Error.IsEmpty());
		TestTrue(TEXT("A missing asset has no payload"), lMissing.PackageBytes.IsEmpty());
	});

	It("Publishes the latest Git entry and follows renamed historical asset paths", [this]
	{
		const FString lOldPath = TEXT("Nested Project/Previous Name.uasset");
		const FString lCurrentPath = TEXT("Nested Project/Current Name.uasset");
		const FString lCurrentFile = m_Fixture / lCurrentPath;
		const TArray<uint8> ORIGINAL{ 41, 52, 63 };
		if (!RunFixtureGit(TEXT("init --initial-branch=master"))
			|| !FFileHelper::SaveArrayToFile(ORIGINAL, *(m_Fixture / lOldPath))
			|| !RunFixtureGit(TEXT("add -- ."))
			|| !RunFixtureGit(TEXT("commit -m Original"))
			|| !RunFixtureGit(TEXT("mv \"Nested Project/Previous Name.uasset\" \"Nested Project/Current Name.uasset\""))
			|| !FFileHelper::SaveStringToFile(TEXT("Переименование\nImmediate detail\n"), *(m_Fixture / TEXT(".git/rename-message")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			|| !RunFixtureGit(TEXT("commit -F .git/rename-message")))
			return;

		TOptional<FBlueprintDiffGitRevision> lLatest;
		int32 lPublications = 0;
		const FString lGit = BlueprintDiffGit::GetExecutable();
		const auto lPublishLatest = [&lLatest, &lPublications](TOptional<FBlueprintDiffGitRevision> aLatest)
		{
			++lPublications;
			lLatest = MoveTemp(aLatest);
		};
		const auto lHistory = BlueprintDiffGit::ReadFileHistory(lGit, lCurrentFile, lPublishLatest);
		TestFalse(TEXT("No slow provider fallback for a regular Git rename"), lHistory.bUseProvider);
		TestEqual(TEXT("Latest published exactly once"), lPublications, 1);
		if (!TestTrue(TEXT("Latest entry was published"), lLatest.IsSet())
			|| !TestEqual(TEXT("History covers both revisions"), lHistory.Revisions.Num(), 2))
			return;

		TestEqual(TEXT("Latest revision uses the new path"), lLatest->Filename, lCurrentPath);
		TestEqual(TEXT("Latest revision uses only the first line of the Unicode commit"), lLatest->Commit.Subject, FString(TEXT("Переименование")));
		TestTrue(TEXT("The entry retains the full commit description"), lLatest->Description.Contains(TEXT("Immediate detail")));
		TestEqual(TEXT("Latest and full history use the same pinned identity"), lLatest->CommitId, lHistory.Revisions[0].CommitId);
		TestEqual(TEXT("Previous revision uses its original Git path"), lHistory.Revisions[1].Filename, lOldPath);
		const auto lBefore = BlueprintDiffGit::ReadHistoryEntry(lGit, lHistory.Revisions[1]);
		TestTrue(TEXT("Historical blob resolves in the old path"), lBefore.Error.IsEmpty() && lBefore.PackageBytes == ORIGINAL);

		if (!RunFixtureGit(TEXT("rm -- \"Nested Project/Current Name.uasset\""))
			|| !RunFixtureGit(TEXT("commit -m Removed")))
			return;
		const auto lRemovedHistory = BlueprintDiffGit::ReadFileHistory(lGit, lCurrentFile, lPublishLatest);
		TestFalse(TEXT("Deleted revision has no snapshot to publish"), lLatest.IsSet());
		TestEqual(TEXT("Full list retains the two usable entries"), lRemovedHistory.Revisions.Num(), 2);
	});

	It("Uses the exact Git path for assets sharing a name and commit", [this]
	{
		const FString lFirst = m_Fixture / TEXT("Nested Project/Same.uasset");
		const FString lSecond = m_Fixture / TEXT("Second/Same.uasset");
		const TArray<uint8> FIRST_BYTES { 12, 23, 34 };
		const TArray<uint8> SECOND_BYTES { 56, 67, 78 };
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(lSecond), true);
		FString lCommit;
		if (!TestTrue(TEXT("First fixture written"), FFileHelper::SaveArrayToFile(FIRST_BYTES, *lFirst))
			|| !TestTrue(TEXT("Second fixture written"), FFileHelper::SaveArrayToFile(SECOND_BYTES, *lSecond))
			|| !RunFixtureGit(TEXT("init --initial-branch=master"))
			|| !RunFixtureGit(TEXT("add -- ."))
			|| !RunFixtureGit(TEXT("commit -m Both"))
			|| !RunFixtureGit(TEXT("rev-parse HEAD"), &lCommit))
		{
			return;
		}
		const FString lGit = BlueprintDiffGit::GetExecutable();
		auto lBefore = BlueprintDiffGit::ReadHistory(lGit, lFirst, TEXT("Second/Same.uasset"), lCommit);
		TestTrue(TEXT("Explicit historical path selects the second asset"), lBefore.Error.IsEmpty() && lBefore.PackageBytes == SECOND_BYTES);
		lBefore = BlueprintDiffGit::ReadHistory(lGit, lFirst, TEXT("Nested Project/Same.uasset"), lCommit);
		TestTrue(TEXT("A same-named asset cannot reuse the wrong revision"), lBefore.Error.IsEmpty() && lBefore.PackageBytes == FIRST_BYTES);
		lBefore = BlueprintDiffGit::ReadHistory(lGit, lSecond, TEXT("Second/Same.uasset"), lCommit);
		TestTrue(TEXT("Selecting the second asset again still returns its own bytes"), lBefore.Error.IsEmpty() && lBefore.PackageBytes == SECOND_BYTES);

		TAtomic<bool> lCancelled(true);
		TestFalse(TEXT("A cancelled request publishes no payload"),
			BlueprintDiffGit::ReadHistory(lGit, lFirst, lFirst, lCommit, &lCancelled).Error.IsEmpty());
	});

	It("Lists working and staged files and reverts only the selected file or rename", [this]
	{
		const FString lAsset = TEXT("Nested Project/[Asset] & data.uasset");
		const FString lOther = TEXT("Nested Project/Other.txt");
		const FString lFrom = TEXT("Nested Project/Before.txt");
		const FString lTo = TEXT("Nested Project/After.txt");
		const FString lNew = TEXT("Nested Project/New.txt");
		const auto lWrite = [&](const FString & aPath, const TCHAR * aText)
		{
			return FFileHelper::SaveStringToFile(aText, *(m_Fixture / aPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		};
		if (!RunFixtureGit(TEXT("init --initial-branch=main")) || !lWrite(lAsset, TEXT("baseline"))
			|| !lWrite(lOther, TEXT("other baseline")) || !lWrite(lFrom, TEXT("renamed content"))
			|| !RunFixtureGit(TEXT("add -- .")) || !RunFixtureGit(TEXT("commit -m Initial"))
			|| !RunFixtureGit(TEXT("update-ref refs/remotes/origin/main HEAD"))
			|| !RunFixtureGit(TEXT("symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main"))
			|| !lWrite(lAsset, TEXT("staged"))
			|| !RunFixtureGit(TEXT("--literal-pathspecs add -- \"Nested Project/[Asset] & data.uasset\""))
			|| !lWrite(lAsset, TEXT("working")) || !lWrite(lOther, TEXT("other edited")) || !lWrite(lNew, TEXT("new"))
			|| !RunFixtureGit(TEXT("mv \"Nested Project/Before.txt\" \"Nested Project/After.txt\"")))
			return;

		const FString lGit = BlueprintDiffGit::GetExecutable();
		const FString lContext = m_Fixture / lAsset;
		const FBlueprintDiffMaster lDefault = BlueprintDiffGit::ReadMaster(lGit, lContext);
		TestTrue(TEXT("GitHub-style origin/main resolves"), lDefault.Error.IsEmpty() && lDefault.Commit.Ref == TEXT("origin/main"));
		const FBlueprintDiffWorkingTree lTree = BlueprintDiffGit::ReadWorkingTree(lGit, lContext);
		if (!TestTrue(TEXT("Status read succeeds"), lTree.Error.IsEmpty())
			|| !TestEqual(TEXT("All four rows are present"), lTree.Files.Num(), 4))
			return;
		const auto lFind = [&](const FString & aPath)
		{
			return lTree.Files.FindByPredicate([&aPath](const FBlueprintDiffChangedFile & aFile) { return aFile.Path == aPath; });
		};
		const auto * lEdited = lFind(lAsset);
		const auto * lRename = lFind(lTo);
		const auto * lUntracked = lFind(lNew);
		if (!TestNotNull(TEXT("Literal bracketed path"), lEdited) || !TestNotNull(TEXT("Renamed path"), lRename)
			|| !TestNotNull(TEXT("Untracked path"), lUntracked))
			return;
		TestTrue(TEXT("Both staged and working"), lEdited->IsStaged() && lEdited->IsWorking());
		TestTrue(TEXT("Original path is associated with rename"), lRename->IsStaged() && lRename->OriginalPath == lFrom);
		TestTrue(TEXT("Untracked appears in Working"), lUntracked->IsUntracked() && lUntracked->IsWorking() && !lUntracked->IsStaged());
		TestTrue(TEXT("Reverts selected literal path"), BlueprintDiffGit::RevertFile(lGit, lTree.Repository, *lEdited).IsEmpty());
		TestFalse(TEXT("Stale status cannot revert twice"), BlueprintDiffGit::RevertFile(lGit, lTree.Repository, *lEdited).IsEmpty());
		if (!TestTrue(TEXT("Independently recreated the original path"), lWrite(lFrom, TEXT("recreated by the user"))))
			return;
		TestFalse(TEXT("Revert refuses to replace the recreated file"), BlueprintDiffGit::RevertFile(lGit, lTree.Repository, *lRename).IsEmpty());
		FString lContent;
		TestTrue(TEXT("The recreated file is preserved"),
			FFileHelper::LoadFileToString(lContent, *(m_Fixture / lFrom)) && lContent == TEXT("recreated by the user"));
		TestTrue(TEXT("The rename remains intact"), FPaths::FileExists(m_Fixture / lTo));
		if (!TestTrue(TEXT("Removed the fixture collision"), IFileManager::Get().Delete(*(m_Fixture / lFrom), true)))
			return;
		TestTrue(TEXT("Reverts rename"), BlueprintDiffGit::RevertFile(lGit, lTree.Repository, *lRename).IsEmpty());
		TestTrue(TEXT("Old filename restored"), FPaths::FileExists(m_Fixture / lFrom));
		TestFalse(TEXT("Renamed destination removed"), FPaths::FileExists(m_Fixture / lTo));
		TestTrue(TEXT("Untracked can be removed explicitly"), BlueprintDiffGit::RevertFile(lGit, lTree.Repository, *lUntracked).IsEmpty());
		TestTrue(TEXT("Baseline bytes restored"), FFileHelper::LoadFileToString(lContent, *lContext) && lContent == TEXT("baseline"));
		TestTrue(TEXT("Unrelated local edits preserved"),
			FFileHelper::LoadFileToString(lContent, *(m_Fixture / lOther)) && lContent == TEXT("other edited"));
	});

	It("Lists project assets and C++ only and stages inside the owning repository", [this]
	{
		const FString lProject = m_Fixture / TEXT("Nested Project");
		const FString lSource = lProject / TEXT("Source");
		const FString lPlugin = lProject / TEXT("Plugins/Fixture");
		const auto lWrite = [&](const FString & aFile)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(aFile), true);
			return FFileHelper::SaveStringToFile(TEXT("fixture"), *aFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		};
		for (const FString & lFile : {
			lProject / TEXT("Content/[Test].UASSET"), lProject / TEXT("Content/Unwanted.umap"),
			lProject / TEXT("Binaries/Unwanted.dll"), lProject / TEXT("Plugins/Fixture/Source/Plugin.hpp"),
			lSource / TEXT("Nested.cpp"), lSource / TEXT("Script.py"), m_Fixture / TEXT("Sibling/Other.uasset") })
		{
			if (!TestTrue(TEXT("Fixture file written"), lWrite(lFile)))
				return;
		}
		if (!RunFixtureGit(TEXT("init --initial-branch=main"))
			|| !RunFixtureGit(TEXT("-C \"Nested Project/Source\" init --initial-branch=main"))
			|| !RunFixtureGit(TEXT("-C \"Nested Project/Plugins/Fixture\" init --initial-branch=main")))
			return;

		const FString lExecutable = BlueprintDiffGit::GetExecutable();
		const auto lChanges = BlueprintDiffGit::ReadProjectChanges(lExecutable, lProject, { lPlugin });
		if (!TestTrue(TEXT("Project status read"), lChanges.Error.IsEmpty())
			|| !TestEqual(TEXT("Only the asset and two C++ files are returned"), lChanges.Files.Num(), 3))
			return;
		const FBlueprintDiffProjectFile * lAsset = lChanges.Files.FindByPredicate([](const auto & aFile)
			{ return BlueprintDiffGit::IsAssetFile(aFile.Path); });
		const FBlueprintDiffProjectFile * lCode = lChanges.Files.FindByPredicate([](const auto & aFile)
			{ return aFile.Path == TEXT("Nested.cpp"); });
		if (!TestNotNull(TEXT("Uppercase asset suffix recognised"), lAsset) || !TestNotNull(TEXT("Nested Source row"), lCode))
			return;
		TestTrue(TEXT("Source row owns the nested Git root"), FPaths::IsSamePath(lCode->Repository, lSource));
		TestTrue(TEXT("Stages the literal asset path"), BlueprintDiffGit::StageFile(lExecutable, *lAsset).IsEmpty());
		TestTrue(TEXT("Stages in the nested Source repository"), BlueprintDiffGit::StageFile(lExecutable, *lCode).IsEmpty());
		const auto lAfter = BlueprintDiffGit::ReadProjectChanges(lExecutable, lProject, { lPlugin });
		TestEqual(TEXT("Exactly the two selected rows were staged"), lAfter.Files.FilterByPredicate([](const auto & aFile)
			{ return aFile.IsStaged() && !aFile.IsWorking(); }).Num(), 2);
		TestTrue(TEXT("The other repository entry is still untracked"), lAfter.Files.ContainsByPredicate([](const auto & aFile)
			{ return aFile.Path == TEXT("Source/Plugin.hpp") && aFile.IsUntracked(); }));
	});

	BeforeEach([this]
	{
		m_Fixture = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / (TEXT("Blueprint Diff Git ") + FGuid::NewGuid().ToString()));
		IFileManager::Get().MakeDirectory(*(m_Fixture / TEXT("Nested Project")), true);
	});

	AfterEach([this]
	{
		IFileManager::Get().DeleteDirectory(*m_Fixture, false, true);
	});
}

#endif
