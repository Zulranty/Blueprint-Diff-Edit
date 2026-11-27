#pragma once

#include "CoreMinimal.h"
#include "Templates/Atomic.h"

struct FBlueprintDiffCommit
{
	FString Hash;
	FString Subject;
	FString Author;
	FString Date;
	FString Ref;

	FText GetMenuLabel(bool aShowDate) const;
};

struct FBlueprintDiffMaster
{
	FBlueprintDiffCommit Commit;
	TArray<uint8> PackageBytes;
	FString SourceFilename;
	FText Error;
};

struct FBlueprintDiffGitRevision
{
	FString Repository;
	FString CommitId;
	FString Filename;
	FString Description;
	FBlueprintDiffCommit Commit;
};

struct FBlueprintDiffFileHistory
{
	TArray<FBlueprintDiffGitRevision> Revisions;
	FText Error;
	bool bUseProvider = false;
};

struct FBlueprintDiffChangedFile
{
	FString Path;
	FString OriginalPath;
	TCHAR Index = TEXT(' ');
	TCHAR Working = TEXT(' ');

	bool IsUntracked() const { return Index == TEXT('?'); }
	bool IsStaged() const { return Index != TEXT(' ') && !IsUntracked(); }
	bool IsWorking() const { return Working != TEXT(' '); }
	bool IsConflicted() const;
	bool operator==(const FBlueprintDiffChangedFile & aOther) const = default;
};

struct FBlueprintDiffWorkingTree
{
	FString Repository;
	TArray<FBlueprintDiffChangedFile> Files;
	FText Error;
};

struct FBlueprintDiffProjectFile : FBlueprintDiffChangedFile
{
	FString Repository;
	FString Filename;

	bool operator==(const FBlueprintDiffProjectFile & aOther) const = default;
};

struct FBlueprintDiffProjectChanges
{
	TArray<FBlueprintDiffProjectFile> Files;
	FText Error;
};

namespace BlueprintDiffGit
{
	FBlueprintDiffWorkingTree ReadWorkingTree(const FString & aExecutable, const FString & aContextFilename,
		const TAtomic<bool> * aCancelled = nullptr);
	FBlueprintDiffProjectChanges ReadProjectChanges(const FString & aExecutable, const FString & aProjectDirectory,
		const TArray<FString> & aCandidateDirectories, const TAtomic<bool> * aCancelled = nullptr);
	bool IsAssetFile(const FString & aFilename);
	FText StageFile(const FString & aExecutable, const FBlueprintDiffProjectFile & aFile);
	FText RevertFile(const FString & aExecutable, const FString & aRepository, const FBlueprintDiffChangedFile & aFile);
	FBlueprintDiffFileHistory ReadFileHistory(const FString & aExecutable, const FString & aFilename,
		TFunctionRef<void(TOptional<FBlueprintDiffGitRevision>)> aPublishLatest,
		const TAtomic<bool> * aCancelled = nullptr, bool aPreload = true);
	FBlueprintDiffMaster ReadHistoryEntry(const FString & aExecutable, const FBlueprintDiffGitRevision & aRevision,
		const TAtomic<bool> * aCancelled = nullptr, const FString * aDestination = nullptr);
	FString GetExecutable();
	FString GetSubject(const FString & aDescription);
	FBlueprintDiffMaster ReadMaster(const FString & aExecutable, const FString & aFilename,
		const TAtomic<bool> * aCancelled = nullptr, const FString * aDestination = nullptr);
	FBlueprintDiffMaster ReadHistory(const FString & aExecutable, const FString & aWorkingFilename,
		const FString & aRevisionFilename, const FString & aRevision, const TAtomic<bool> * aCancelled = nullptr, const FString * aDestination = nullptr);
}
