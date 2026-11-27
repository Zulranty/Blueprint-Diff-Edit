#include "BlueprintDiffModel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "DiffResults.h"
#include "DiffUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GraphDiffControl.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "BlueprintDiffModel"

namespace
{
	TArray<UEdGraph *> GetComparedGraphs(const UBlueprint * aBlueprint)
	{
		TArray<UEdGraph *> lGraphs;
		aBlueprint->GetAllGraphs(lGraphs);
		lGraphs.RemoveAll([aBlueprint](const UEdGraph * aGraph)
		{
			if (aGraph->GetOuter() != aBlueprint || aBlueprint->DelegateSignatureGraphs.Contains(aGraph))
				return true;

			const UEdGraphSchema_K2 * const lSchema = Cast<UEdGraphSchema_K2>(aGraph->GetSchema());
			if (!lSchema)
				return true;

			const EGraphType lType = lSchema->GetGraphType(aGraph);
			return lType != GT_Function && lType != GT_Ubergraph && lType != GT_Macro;
		});
		return lGraphs;
	}

	TArray<FBlueprintGraphPair> PairGraphs(const TArray<UEdGraph *> & aRevision, const TArray<UEdGraph *> & aWorking,
		const UObject * aRevisionRoot, const UObject * aWorkingRoot)
	{
		TMap<FGuid, UEdGraph *> lWorkingByGuid;
		TMap<FString, UEdGraph *> lWorkingByPath;
		for (UEdGraph * const lGraph : aWorking)
		{
			if (lGraph->GraphGuid.IsValid())
				lWorkingByGuid.Add(lGraph->GraphGuid, lGraph);
			lWorkingByPath.Add(lGraph->GetPathName(aWorkingRoot), lGraph);
		}

		TArray<FBlueprintGraphPair> lPairs;
		TSet<UEdGraph *> lMatched;
		for (UEdGraph * const lGraph : aRevision)
		{
			UEdGraph * const lMatch = lWorkingByGuid.FindRef(lGraph->GraphGuid);
			if (lMatch && lMatch->Schema == lGraph->Schema && !lMatched.Contains(lMatch))
			{
				lPairs.Add({ lGraph, lMatch });
				lMatched.Add(lMatch);
			}
			else
				lPairs.Add({ lGraph, nullptr });
		}
		for (FBlueprintGraphPair & lPair : lPairs)
		{
			if (lPair.Working.IsValid())
				continue;

			const UEdGraph * const lGraph = lPair.Revision.Get();
			UEdGraph * const lMatch = lWorkingByPath.FindRef(lGraph->GetPathName(aRevisionRoot));
			if (lMatch && lMatch->Schema == lGraph->Schema && !lMatched.Contains(lMatch))
			{
				lPair.Working = lMatch;
				lMatched.Add(lMatch);
			}
		}
		for (UEdGraph * const lGraph : aWorking)
		{
			if (!lMatched.Contains(lGraph))
				lPairs.Add({ nullptr, lGraph });
		}
		return lPairs;
	}

	bool HasGraphChanges(const FBlueprintGraphPair & aPair, FBlueprintGraphDiffs * aCachedDiffs)
	{
		UEdGraph * const lRevision = aPair.Revision.Get();
		UEdGraph * const lWorking = aPair.Working.Get();
		if (!lRevision || !lWorking || lRevision->GetFName() != lWorking->GetFName())
			return true;

		if (aCachedDiffs)
			return BlueprintDiffModel::GetGraphDifferences(aPair, *aCachedDiffs).ContainsByPredicate(
				[](const FBlueprintNodeDifference & aResult) { return aResult.Display.IsRealDifference(); });

		TArray<FDiffSingleResult> lDifferences;
		FGraphDiffControl::DiffGraphs(lRevision, lWorking, lDifferences);
		return lDifferences.ContainsByPredicate([](const FDiffSingleResult & aResult) { return aResult.IsRealDifference(); });
	}

	FText ExportValue(const FResolvedProperty & aValue)
	{
		if (!aValue.Property || !aValue.Object)
			return LOCTEXT("MissingValue", "Not present");

		FString lOutput;
		const void * const lData = aValue.Property->ContainerPtrToValuePtr<void>(aValue.Object);
		aValue.Property->ExportTextItem_Direct(lOutput, lData, nullptr, nullptr, PPF_None);
		return lOutput.IsEmpty() ? LOCTEXT("EmptyValue", "(empty)") : FText::FromString(lOutput);
	}

	void AddPropertyChanges(TArray<FBlueprintSettingChange> & aChanges, const FText & aSection,
		const UStruct * aRevisionType, const void * aRevision, const UObject * aRevisionOuter,
		const UStruct * aWorkingType, const void * aWorking, const UObject * aWorkingOuter,
		FBlueprintSettingTarget aTarget = {})
	{
		TArray<FSingleObjectDiffEntry> lProperties;
		DiffUtils::CompareUnrelatedStructs(aRevisionType, aRevision, aRevisionOuter,
			aWorkingType, aWorking, aWorkingOuter, lProperties);
		TArray<FPropertySoftPath> lReportedMaps;
		TSet<FName> lReportedRoots;
		for (const FSingleObjectDiffEntry & lProperty : lProperties)
		{
			if (aTarget.Kind != EBlueprintSettingKind::None)
			{
				const FPropertySoftPath lRoot = lProperty.Identifier.GetRootProperty(1);
				const FResolvedProperty lOld = lRoot.Resolve(aRevisionType, aRevision);
				const FResolvedProperty lNew = lRoot.Resolve(aWorkingType, aWorking);
				const FProperty * const lPropertyRoot = lNew.Property ? lNew.Property : lOld.Property;
				if (!lPropertyRoot || lReportedRoots.Contains(lPropertyRoot->GetFName()))
					continue;
				lReportedRoots.Add(lPropertyRoot->GetFName());
				aTarget.Property = lPropertyRoot->GetFName();
				aChanges.Add({ FText::Format(LOCTEXT("SectionProperty", "{0} · {1}"), aSection,
					FText::FromString(lRoot.ToDisplayName())), ExportValue(lOld), ExportValue(lNew), aTarget });
				continue;
			}

			FPropertySoftPath lDisplayPath = lProperty.Identifier;
			bool lWholeMap = false;
			for (int32 lDepth = 1; ; ++lDepth)
			{
				const FPropertySoftPath lPrefix = lProperty.Identifier.GetRootProperty(lDepth);
				if (!lProperty.Identifier.IsSubPropertyMatch(lPrefix))
					break;
				if (CastField<FMapProperty>(lPrefix.Resolve(aRevisionType, aRevision).Property)
					|| CastField<FMapProperty>(lPrefix.Resolve(aWorkingType, aWorking).Property))
				{
					lDisplayPath = lPrefix;
					lWholeMap = true;
					break;
				}
			}
			// UE resolves indexed map leaves as values, even when the changed leaf is a key.
			if (lWholeMap && lReportedMaps.Contains(lDisplayPath))
				continue;
			if (lWholeMap)
				lReportedMaps.Add(lDisplayPath);
			aChanges.Add({
				FText::Format(LOCTEXT("SectionProperty", "{0} · {1}"), aSection,
					FText::FromString(lDisplayPath.ToDisplayName())),
				ExportValue(!lWholeMap && lProperty.DiffType == EPropertyDiffType::PropertyAddedToB
					? FResolvedProperty() : lDisplayPath.Resolve(aRevisionType, aRevision)),
				ExportValue(!lWholeMap && lProperty.DiffType == EPropertyDiffType::PropertyAddedToA
					? FResolvedProperty() : lDisplayPath.Resolve(aWorkingType, aWorking))
			});
		}
	}

	TConstArrayView<TSharedPtr<FUserPinInfo>> GetDispatcherParameters(const UEdGraph & aGraph)
	{
		for (const UEdGraphNode * const lNode : aGraph.Nodes)
		{
			if (const UK2Node_FunctionEntry * const lEntry = Cast<UK2Node_FunctionEntry>(lNode))
				return lEntry->UserDefinedPins;
		}
		return {};
	}

	bool HasDispatcherChanges(const UEdGraph & aBefore, const UEdGraph & aWorking)
	{
		const auto lBefore = GetDispatcherParameters(aBefore);
		const auto lWorking = GetDispatcherParameters(aWorking);
		if (lBefore.Num() != lWorking.Num())
			return true;

		for (int32 lIndex = 0; lIndex < lBefore.Num(); ++lIndex)
		{
			const FUserPinInfo * const lOld = lBefore[lIndex].Get();
			const FUserPinInfo * const lNew = lWorking[lIndex].Get();
			if (bool(lOld) != bool(lNew) || (lOld && (lOld->PinName != lNew->PinName
				|| lOld->PinType != lNew->PinType || lOld->DesiredPinDirection != lNew->DesiredPinDirection
				|| lOld->PinDefaultValue != lNew->PinDefaultValue)))
				return true;
		}
		return false;
	}

	FText DescribeDispatcher(const UEdGraph * aGraph)
	{
		if (!aGraph)
			return LOCTEXT("MissingValue", "Not present");
		TArray<FString> lParameters;
		for (const TSharedPtr<FUserPinInfo> & lPin : GetDispatcherParameters(*aGraph))
		{
			if (lPin)
			{
				FString & lDescription = lParameters.Add_GetRef(FString::Printf(TEXT("%s: %s"),
					*lPin->PinName.ToString(), *UEdGraphSchema_K2::TypeToText(lPin->PinType).ToString()));
				if (!lPin->PinDefaultValue.IsEmpty())
					lDescription += TEXT(" = ") + lPin->PinDefaultValue;
			}
		}
		return lParameters.IsEmpty() ? LOCTEXT("NoDispatcherParameters", "No parameters")
			: FText::FromString(FString::Join(lParameters, TEXT("\n")));
	}

	TMap<const UWidget *, FText> DescribeNamedSlots(const UWidgetTree & aTree, const TArray<UWidget *> & aWidgets)
	{
		TMap<const UWidget *, FText> lSlots;
		const auto lAddSlots = [&lSlots](const INamedSlotInterface & aHost, const FText & aHostName)
		{
			TArray<FName> lNames;
			aHost.GetSlotNames(lNames);
			for (const FName lSlotName : lNames)
			{
				if (const UWidget * const lContent = aHost.GetContentForSlot(lSlotName))
				{
					lSlots.Add(lContent, FText::Format(LOCTEXT("NamedSlotPath", "{0} / {1}"),
						aHostName, FText::FromName(lSlotName)));
				}
			}
		};
		lAddSlots(aTree, LOCTEXT("InheritedSlots", "Inherited slots"));
		for (const UWidget * const lWidget : aWidgets)
		{
			if (const INamedSlotInterface * const lHost = Cast<INamedSlotInterface>(lWidget))
				lAddSlots(*lHost, FText::FromName(lWidget->GetFName()));
		}
		return lSlots;
	}

	TMap<const UWidget *, int32> IndexPanelChildren(const TArray<UWidget *> & aWidgets)
	{
		TMap<const UWidget *, int32> lIndices;
		for (const UWidget * lWidget : aWidgets)
		{
			if (const UPanelWidget * const lPanel = Cast<UPanelWidget>(lWidget))
			{
				for (int32 lIndex = 0; lIndex < lPanel->GetChildrenCount(); ++lIndex)
					lIndices.Add(lPanel->GetChildAt(lIndex), lIndex + 1);
			}
		}
		return lIndices;
	}

	FText DescribeWidgetPosition(const UWidget & aWidget, const TMap<const UWidget *, FText> & aNamedSlots,
		const TMap<const UWidget *, int32> & aIndices)
	{
		if (const FText * const lSlot = aNamedSlots.Find(&aWidget))
			return *lSlot;
		if (UPanelWidget * const lParent = aWidget.GetParent())
		{
			return FText::Format(LOCTEXT("PanelWidgetPosition", "{0} · child {1}"),
				FText::FromName(lParent->GetFName()), FText::AsNumber(aIndices.FindRef(&aWidget)));
		}
		return LOCTEXT("RootWidget", "Root");
	}

	FText GetClassLabel(const UClass * aClass)
	{
		return aClass ? aClass->GetDisplayNameText() : LOCTEXT("NoClass", "None");
	}

	FString DescribeInterfaces(const UBlueprint * aBlueprint)
	{
		TArray<FString> lInterfaces;
		for (const FBPInterfaceDescription & lInterface : aBlueprint->ImplementedInterfaces)
		{
			if (lInterface.Interface)
				lInterfaces.Add(lInterface.Interface->GetPathName());
		}
		lInterfaces.Sort();
		return FString::Join(lInterfaces, TEXT("\n"));
	}
}

const TArray<FBlueprintNodeDifference> & BlueprintDiffModel::GetGraphDifferences(
	const FBlueprintGraphPair & aPair, FBlueprintGraphDiffs & aDiffs)
{
	if (const TArray<FBlueprintNodeDifference> * const lCached = aDiffs.Find(aPair))
		return *lCached;

	TArray<FBlueprintNodeDifference> & lDifferences = aDiffs.Add(aPair);
	TArray<FDiffSingleResult> lResults;
	FGraphDiffControl::DiffGraphs(aPair.Revision.Get(), aPair.Working.Get(), lResults);
	for (FDiffSingleResult & lResult : lResults)
	{
		if (!lResult.IsRealDifference())
			continue;

		UEdGraphNode * const lFirst = lResult.Pin1 ? lResult.Pin1->GetOwningNode() : lResult.Node1;
		UEdGraphNode * const lSecond = lResult.Pin2 ? lResult.Pin2->GetOwningNode() : lResult.Node2;
		const auto lFindOwner = [lFirst, lSecond](const UEdGraph * aGraph) -> UEdGraphNode *
		{
			for (UEdGraphNode * lNode : { lFirst, lSecond })
			{
				if (lNode && lNode->GetGraph() == aGraph && lNode->NodeGuid.IsValid())
					return lNode;
			}
			return nullptr;
		};
		lResult.Node1 = nullptr;
		lResult.Node2 = nullptr;
		lResult.Pin1 = nullptr;
		lResult.Pin2 = nullptr;
		lResult.Object1 = nullptr;
		lResult.Object2 = nullptr;
		lDifferences.Add({ lFindOwner(aPair.Revision.Get()), lFindOwner(aPair.Working.Get()), MoveTemp(lResult) });
	}
	return lDifferences;
}

TArray<FBlueprintGraphChange> BlueprintDiffModel::FindChangedGraphs(const UBlueprint * aRevision,
	const UBlueprint * aWorking, FBlueprintGraphDiffs * aCachedDiffs)
{
	TArray<FBlueprintGraphChange> lChanges;
	for (const FBlueprintGraphPair & lPair : PairGraphs(GetComparedGraphs(aRevision), GetComparedGraphs(aWorking), aRevision, aWorking))
	{
		FBlueprintGraphChange lChange;
		lChange.Root = lPair;
		lChange.HasRootChanges = HasGraphChanges(lPair, aCachedDiffs);
		const UEdGraph * const lRevisionGraph = lPair.Revision.Get();
		const UEdGraph * const lWorkingGraph = lPair.Working.Get();
		if (lRevisionGraph && lWorkingGraph)
		{
			TArray<UEdGraph *> lRevisionChildren;
			TArray<UEdGraph *> lWorkingChildren;
			lRevisionGraph->GetAllChildrenGraphs(lRevisionChildren);
			lWorkingGraph->GetAllChildrenGraphs(lWorkingChildren);
			for (const FBlueprintGraphPair & lChild : PairGraphs(lRevisionChildren, lWorkingChildren, lRevisionGraph, lWorkingGraph))
			{
				if (HasGraphChanges(lChild, aCachedDiffs))
					lChange.ChangedChildren.Add(lChild);
			}
		}
		if (lChange.HasRootChanges || !lChange.ChangedChildren.IsEmpty())
			lChanges.Add(MoveTemp(lChange));
	}
	return lChanges;
}

TArray<FBlueprintWidgetChange> BlueprintDiffModel::FindChangedWidgets(
	const UWidgetBlueprint * aRevision, const UWidgetBlueprint * aWorking)
{
	TArray<FBlueprintWidgetChange> lChanges;
	const UWidgetTree * const lOldTree = aRevision->WidgetTree;
	const UWidgetTree * const lWorkingTree = aWorking->WidgetTree;
	if (!lOldTree || !lWorkingTree)
		return lChanges;

	TArray<UWidget *> lOldWidgets;
	TArray<UWidget *> lWorkingWidgets;
	lOldTree->GetAllWidgets(lOldWidgets);
	lWorkingTree->GetAllWidgets(lWorkingWidgets);
	const auto lOldNamedSlots = DescribeNamedSlots(*lOldTree, lOldWidgets);
	const auto lWorkingNamedSlots = DescribeNamedSlots(*lWorkingTree, lWorkingWidgets);
	const auto lOldIndices = IndexPanelChildren(lOldWidgets);
	const auto lWorkingIndices = IndexPanelChildren(lWorkingWidgets);

	TMap<FName, UWidget *> lWorkingByName;
	TMap<FGuid, UWidget *> lWorkingByGuid;
	for (UWidget * const lWidget : lWorkingWidgets)
	{
		const FName lName = lWidget->GetFName();
		lWorkingByName.Add(lName, lWidget);
		if (const FGuid * const lGuid = aWorking->WidgetVariableNameToGuidMap.Find(lName); lGuid && lGuid->IsValid())
			lWorkingByGuid.Add(*lGuid, lWidget);
	}

	// Reserve GUID matches before using names, so a new widget cannot steal a renamed widget's match.
	TMap<const UWidget *, UWidget *> lMatches;
	TSet<const UWidget *> lMatchedWorking;
	for (const UWidget * const lWidget : lOldWidgets)
	{
		if (const FGuid * const lGuid = aRevision->WidgetVariableNameToGuidMap.Find(lWidget->GetFName()))
		{
			if (UWidget * const lMatch = lWorkingByGuid.FindRef(*lGuid); lMatch && !lMatchedWorking.Contains(lMatch))
			{
				lMatches.Add(lWidget, lMatch);
				lMatchedWorking.Add(lMatch);
			}
		}
	}
	for (const UWidget * const lWidget : lOldWidgets)
	{
		if (!lMatches.Contains(lWidget))
		{
			if (UWidget * const lMatch = lWorkingByName.FindRef(lWidget->GetFName()); lMatch && !lMatchedWorking.Contains(lMatch))
			{
				lMatches.Add(lWidget, lMatch);
				lMatchedWorking.Add(lMatch);
			}
		}
	}

	const FText lMissing = LOCTEXT("WidgetAbsent", "Not present");
	for (const UWidget * const lOld : lOldWidgets)
	{
		const UWidget * const lWorking = lMatches.FindRef(lOld);
		const FText lOldName = FText::FromName(lOld->GetFName());
		if (!lWorking)
		{
			lChanges.Add({ { FText::Format(LOCTEXT("RemovedWidget", "{0} (removed)"), lOldName),
				DescribeWidgetPosition(*lOld, lOldNamedSlots, lOldIndices), lMissing }, NAME_None });
			continue;
		}

		const FName lWorkingName = lWorking->GetFName();
		const FText lWidgetLabel = FText::FromName(lWorkingName);
		if (lOld->GetFName() != lWorkingName)
		{
			lChanges.Add({ { FText::Format(LOCTEXT("RenamedWidget", "{0} → {1}"), lOldName, lWidgetLabel),
				lOldName, lWidgetLabel }, lWorkingName });
		}
		const FText lOldPosition = DescribeWidgetPosition(*lOld, lOldNamedSlots, lOldIndices);
		const FText lWorkingPosition = DescribeWidgetPosition(*lWorking, lWorkingNamedSlots, lWorkingIndices);
		if (!lOldPosition.EqualTo(lWorkingPosition))
		{
			lChanges.Add({ { FText::Format(LOCTEXT("MovedWidget", "{0} · hierarchy"), lWidgetLabel),
				lOldPosition, lWorkingPosition }, lWorkingName });
		}

		TArray<FBlueprintSettingChange> lProperties;
		if (lOld->GetClass() == lWorking->GetClass())
		{
			AddPropertyChanges(lProperties, lWidgetLabel, lOld->GetClass(), lOld, lOldTree,
				lWorking->GetClass(), lWorking, lWorkingTree);
		}
		else
		{
			lChanges.Add({ { FText::Format(LOCTEXT("WidgetType", "{0} · widget type"), lWidgetLabel),
				GetClassLabel(lOld->GetClass()), GetClassLabel(lWorking->GetClass()) }, lWorkingName });
		}

		const UPanelSlot * const lOldSlot = lOld->Slot;
		const UPanelSlot * const lWorkingSlot = lWorking->Slot;
		if (lOldSlot && lWorkingSlot)
		{
			AddPropertyChanges(lProperties, FText::Format(LOCTEXT("WidgetSlot", "{0} · slot"), lWidgetLabel),
				lOldSlot->GetClass(), lOldSlot, lOldTree, lWorkingSlot->GetClass(), lWorkingSlot, lWorkingTree);
		}
		for (FBlueprintSettingChange & lProperty : lProperties)
			lChanges.Add({ MoveTemp(lProperty), lWorkingName });
	}
	for (const UWidget * const lWorking : lWorkingWidgets)
	{
		if (!lMatchedWorking.Contains(lWorking))
		{
			lChanges.Add({ { FText::Format(LOCTEXT("AddedWidget", "{0} (added)"), FText::FromName(lWorking->GetFName())),
				lMissing, DescribeWidgetPosition(*lWorking, lWorkingNamedSlots, lWorkingIndices) }, lWorking->GetFName() });
		}
	}

	lChanges.Sort([](const FBlueprintWidgetChange & aLeft, const FBlueprintWidgetChange & aRight)
	{
		return aLeft.Values.Label.CompareTo(aRight.Values.Label) < 0;
	});
	return lChanges;
}

TArray<FBlueprintSettingChange> BlueprintDiffModel::FindChangedSettings(const UBlueprint * aRevision, const UBlueprint * aWorking)
{
	TArray<FBlueprintSettingChange> lChanges;
	if (aRevision->ParentClass != aWorking->ParentClass)
		lChanges.Add({ LOCTEXT("ParentClass", "Parent class"), GetClassLabel(aRevision->ParentClass), GetClassLabel(aWorking->ParentClass) });

	const FString lRevisionInterfaces = DescribeInterfaces(aRevision);
	const FString lWorkingInterfaces = DescribeInterfaces(aWorking);
	if (lRevisionInterfaces != lWorkingInterfaces)
	{
		lChanges.Add({ LOCTEXT("Interfaces", "Implemented interfaces"),
			lRevisionInterfaces.IsEmpty() ? LOCTEXT("NoInterfaces", "None") : FText::FromString(lRevisionInterfaces),
			lWorkingInterfaces.IsEmpty() ? LOCTEXT("NoInterfaces", "None") : FText::FromString(lWorkingInterfaces) });
	}

	AddPropertyChanges(lChanges, LOCTEXT("ClassSettings", "Class Settings"), aRevision->GetClass(), aRevision,
		aRevision->GetPackage(), aWorking->GetClass(), aWorking, aWorking->GetPackage(), { EBlueprintSettingKind::ClassSetting });

	const UObject * const lRevisionDefaults = DiffUtils::GetCDO(aRevision);
	const UObject * const lWorkingDefaults = DiffUtils::GetCDO(aWorking);
	if (lRevisionDefaults && lWorkingDefaults)
	{
		AddPropertyChanges(lChanges, LOCTEXT("ClassDefaults", "Class Defaults"), lRevisionDefaults->GetClass(), lRevisionDefaults,
			aRevision->GetPackage(), lWorkingDefaults->GetClass(), lWorkingDefaults, aWorking->GetPackage(), { EBlueprintSettingKind::ClassDefault });
		for (FBlueprintSettingChange & lChange : lChanges)
		{
			if (lChange.Target.Kind == EBlueprintSettingKind::ClassDefault)
			{
				lChange.Target.Member = lChange.Target.Property;
				lChange.Target.IsDispatcher = FindDispatcherSignature(aWorking, lChange.Target.Member)
					|| FindDispatcherSignature(aRevision, lChange.Target.Member);
			}
		}
	}

	TSet<FName> lOldVariableNames;
	TMap<FName, const FBPVariableDescription *> lWorkingVariables;
	for (const FBPVariableDescription & lVariable : aWorking->NewVariables)
	{
		if (!lWorkingVariables.Contains(lVariable.VarName))
			lWorkingVariables.Add(lVariable.VarName, &lVariable);
	}
	const UStruct * const lVariableType = FBPVariableDescription::StaticStruct();
	for (const FBPVariableDescription & lVariable : aRevision->NewVariables)
	{
		lOldVariableNames.Add(lVariable.VarName);
		const FBPVariableDescription * const lWorkingVariable = lWorkingVariables.FindRef(lVariable.VarName);
		UEdGraph * const lOldSignature = FindDispatcherSignature(aRevision, lVariable.VarName);
		UEdGraph * const lNewSignature = FindDispatcherSignature(aWorking, lVariable.VarName);
		if (!lWorkingVariable)
		{
			const FText lLabel = lOldSignature ? LOCTEXT("RemovedDispatcher", "Removed event dispatcher {0}") : LOCTEXT("RemovedVariable", "Removed variable {0}");
			lChanges.Add({ FText::Format(lLabel, FText::FromName(lVariable.VarName)),
				lOldSignature ? DescribeDispatcher(lOldSignature) : FText::FromName(lVariable.VarName), LOCTEXT("MissingValue", "Not present"),
				{ EBlueprintSettingKind::Member, lVariable.VarName, NAME_None, lOldSignature != nullptr } });
		}
		else
		{
			AddPropertyChanges(lChanges, FText::FromName(lVariable.VarName), lVariableType, &lVariable, aRevision->GetPackage(),
				lVariableType, lWorkingVariable, aWorking->GetPackage(),
				{ EBlueprintSettingKind::MemberProperty, lVariable.VarName, NAME_None, lNewSignature != nullptr });
			if (lOldSignature && lNewSignature && HasDispatcherChanges(*lOldSignature, *lNewSignature))
			{
				lChanges.Add({ FText::Format(LOCTEXT("DispatcherSignature", "Event dispatcher · {0}"), FText::FromName(lVariable.VarName)),
					DescribeDispatcher(lOldSignature), DescribeDispatcher(lNewSignature),
					{ EBlueprintSettingKind::DispatcherSignature, lVariable.VarName, NAME_None, true } });
			}
		}
	}
	for (const FBPVariableDescription & lVariable : aWorking->NewVariables)
	{
		if (!lOldVariableNames.Contains(lVariable.VarName))
		{
			const UEdGraph * const lSignature = FindDispatcherSignature(aWorking, lVariable.VarName);
			const FText lLabel = lSignature ? LOCTEXT("AddedDispatcher", "Added event dispatcher {0}") : LOCTEXT("AddedVariable", "Added variable {0}");
			lChanges.Add({ FText::Format(lLabel, FText::FromName(lVariable.VarName)),
				LOCTEXT("MissingValue", "Not present"), lSignature ? DescribeDispatcher(lSignature) : FText::FromName(lVariable.VarName),
				{ EBlueprintSettingKind::Member, lVariable.VarName, NAME_None, lSignature != nullptr } });
		}
	}
	return lChanges;
}

UEdGraph * BlueprintDiffModel::FindDispatcherSignature(const UBlueprint * aBlueprint, FName aMember)
{
	const auto * lGraph = aBlueprint->DelegateSignatureGraphs.FindByPredicate([aMember](const UEdGraph * aGraph)
	{
		return aGraph && aGraph->GetFName() == aMember;
	});
	return lGraph ? lGraph->Get() : nullptr;
}

#undef LOCTEXT_NAMESPACE
