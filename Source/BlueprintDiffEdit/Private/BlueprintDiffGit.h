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

namespace BlueprintDiffGit
{
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
