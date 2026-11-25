#pragma once

#include "CoreMinimal.h"
#include "DiffResults.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UWidgetBlueprint;

struct FBlueprintGraphPair
{
	TWeakObjectPtr<UEdGraph> Revision;
	TWeakObjectPtr<UEdGraph> Working;

	bool operator==(const FBlueprintGraphPair & aOther) const = default;

	friend uint32 GetTypeHash(const FBlueprintGraphPair & aPair)
	{
		return HashCombine(GetTypeHash(aPair.Revision), GetTypeHash(aPair.Working));
	}
};

struct FBlueprintGraphChange
{
	FBlueprintGraphPair Root;
	TArray<FBlueprintGraphPair> ChangedChildren;
	bool HasRootChanges = false;
};

struct FBlueprintSettingChange
{
	FText Label;
	FText RevisionValue;
	FText WorkingValue;
};

struct FBlueprintWidgetChange
{
	FBlueprintSettingChange Values;
	FName WorkingWidget;
};

struct FBlueprintNodeDifference
{
	TWeakObjectPtr<UEdGraphNode> RevisionNode;
	TWeakObjectPtr<UEdGraphNode> WorkingNode;
	FDiffSingleResult Display;
};

using FBlueprintGraphDiffs = TMap<FBlueprintGraphPair, TArray<FBlueprintNodeDifference>>;

namespace BlueprintDiffModel
{
	const TArray<FBlueprintNodeDifference> & GetGraphDifferences(const FBlueprintGraphPair & aPair, FBlueprintGraphDiffs & aDiffs);
	TArray<FBlueprintGraphChange> FindChangedGraphs(const UBlueprint * aRevision, const UBlueprint * aWorking,
		FBlueprintGraphDiffs * aCachedDiffs = nullptr);
	TArray<FBlueprintWidgetChange> FindChangedWidgets(const UWidgetBlueprint * aRevision, const UWidgetBlueprint * aWorking);
	TArray<FBlueprintSettingChange> FindChangedSettings(const UBlueprint * aRevision, const UBlueprint * aWorking);
}
