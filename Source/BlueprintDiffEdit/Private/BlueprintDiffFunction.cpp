#include "BlueprintDiffFunction.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"

#define LOCTEXT_NAMESPACE "BlueprintDiffFunction"

namespace
{
	class FRevisionReferenceRemap final : public FArchiveReplaceObjectRef<UObject>
	{
	public:
		FRevisionReferenceRemap(
			UEdGraph * aGraph,
			const TMap<UObject *, UObject *> & aReplacements,
			const UPackage * aRevisionPackage)
			: FArchiveReplaceObjectRef(aGraph, aReplacements,
				EArchiveReplaceObjectFlags::DelayStart | EArchiveReplaceObjectFlags::IgnoreOuterRef
				| EArchiveReplaceObjectFlags::IgnoreArchetypeRef)
			, m_RevisionPackage(aRevisionPackage)
		{
			SerializeSearchObject();
		}

		bool HasUnresolvedReference() const
		{
			return m_HasUnresolvedReference;
		}

		using FArchiveReplaceObjectRef::operator<<;

		FArchive & operator<<(UObject *& aObject) override
		{
			FArchiveReplaceObjectRef::operator<<(aObject);
			if (aObject && aObject->GetOutermost() == m_RevisionPackage)
				m_HasUnresolvedReference = true;
			return *this;
		}

		FArchive & operator<<(FSoftObjectPath & aPath) override
		{
			const bool lWasRevisionPath = aPath.GetLongPackageFName() == m_RevisionPackage->GetFName();
			FArchiveReplaceObjectRef::operator<<(aPath);
			if ((lWasRevisionPath && aPath.GetLongPackageFName() == m_RevisionPackage->GetFName())
				|| aPath.GetLongPackageFName() == GetTransientPackage()->GetFName())
			{
				m_HasUnresolvedReference = true;
			}
			return *this;
		}

	private:
		const UPackage * m_RevisionPackage;
		bool m_HasUnresolvedReference = false;
	};

	UEdGraph * StageFunction(
		UBlueprint * aWorkingBlueprint,
		const UBlueprint * aRevisionBlueprint,
		UEdGraph * aWorkingGraph,
		UEdGraph * aRevisionGraph,
		FText & aError)
	{
		// A revision is loaded under a temporary package name. References to other objects from that
		// package (such as the generated class or a macro graph) must point back into the workspace.
		UPackage * const lRevisionPackage = aRevisionBlueprint->GetOutermost();
		UPackage * const lWorkingPackage = aWorkingBlueprint->GetOutermost();
		TArray<UObject *> lRevisionObjects;
		GetObjectsWithOuter(lRevisionPackage, lRevisionObjects);
		TMap<UObject *, UObject *> lReplacements;
		lReplacements.Add(lRevisionPackage, lWorkingPackage);
		lReplacements.Add(const_cast<UBlueprint *>(aRevisionBlueprint), aWorkingBlueprint);
		for (UObject * const lObject : lRevisionObjects)
		{
			if (lObject == aRevisionBlueprint || lObject == aRevisionGraph || lObject->IsIn(aRevisionGraph))
				continue;

			const FString lPath = lObject->GetPathName(lRevisionPackage);
			if (UObject * const lWorkspaceObject = StaticFindObject(lObject->GetClass(), lWorkingPackage, *lPath, true))
				lReplacements.Add(lObject, lWorkspaceObject);
		}

		// Blueprint nodes resolve self references in PostDuplicate, before archive remapping runs.
		FObjectDuplicationParameters lParameters(aRevisionGraph, aWorkingBlueprint);
		lParameters.DestName = MakeUniqueObjectName(aWorkingBlueprint, UEdGraph::StaticClass(), TEXT("DiffRevertStaging"));
		lParameters.FlagMask &= ~(RF_Transient | RF_Public | RF_Standalone);
		lParameters.ApplyFlags |= RF_Transactional;
		lParameters.DuplicationSeed = lReplacements;
		UEdGraph * const lStagedGraph = Cast<UEdGraph>(StaticDuplicateObjectEx(lParameters));
		if (!lStagedGraph)
		{
			aError = LOCTEXT("CannotDuplicate", "Unreal could not copy the function from the selected revision.");
			return nullptr;
		}

		lStagedGraph->SetFlags(RF_Transient);
		if (aWorkingGraph)
			lReplacements.Add(lStagedGraph, aWorkingGraph);
		const FRevisionReferenceRemap lRemap(lStagedGraph, lReplacements, lRevisionPackage);
		if (lRemap.HasUnresolvedReference())
		{
			aError = LOCTEXT("MissingDependency",
				"The function references an object from the old Blueprint that could not be mapped to the workspace.");
			return nullptr;
		}

		TArray<UEdGraph *> lGraphs { lStagedGraph };
		lStagedGraph->GetAllChildrenGraphs(lGraphs);
		for (const UEdGraph * lGraph : lGraphs)
		{
			for (const UEdGraphNode * lNode : lGraph->Nodes)
			{
				const UK2Node_CallFunction * const lCall = Cast<UK2Node_CallFunction>(lNode);
				if (!lCall || lCall->GetTargetFunction())
					continue;

				const FMemberReference & lReference = lCall->FunctionReference;
				if (lReference.IsSelfContext() && lReference.GetMemberName() == aRevisionGraph->GetFName())
					continue; // A restored function can call itself before it is registered in the working Blueprint.

				aError = FText::Format(LOCTEXT("MissingFunctionDependency",
					"Cannot revert this function. The working Blueprint cannot resolve the call to {0}."),
					FText::FromName(lReference.GetMemberName()));
				return nullptr;
			}
		}
		return lStagedGraph;
	}

	bool MoveFunctionContents(UEdGraph * aStagedGraph, UEdGraph * aWorkingGraph)
	{
		TArray<UObject *> lChildren;
		GetObjectsWithOuter(aStagedGraph, lChildren, false);

		TArray<UObject *> lMovedChildren;
		for (UObject * const lChild : lChildren)
		{
			lChild->Modify(false);
			if (!lChild->Rename(nullptr, aWorkingGraph, REN_DontCreateRedirectors | REN_DoNotDirty))
			{
				for (UObject * const lMovedChild : lMovedChildren)
					lMovedChild->Rename(nullptr, aStagedGraph, REN_NonTransactional | REN_DontCreateRedirectors | REN_DoNotDirty);
				return false;
			}
			lMovedChildren.Add(lChild);
		}

		aWorkingGraph->Modify();
		aWorkingGraph->Nodes = MoveTemp(aStagedGraph->Nodes);
		aWorkingGraph->SubGraphs = MoveTemp(aStagedGraph->SubGraphs);
		aWorkingGraph->GraphGuid = aStagedGraph->GraphGuid;
		aWorkingGraph->InterfaceGuid = aStagedGraph->InterfaceGuid;
		aWorkingGraph->bEditable = aStagedGraph->bEditable;
		aWorkingGraph->bAllowDeletion = aStagedGraph->bAllowDeletion;
		aWorkingGraph->bAllowRenaming = aStagedGraph->bAllowRenaming;
		return true;
	}
}

bool BlueprintDiffFunction::IsFunction(const UBlueprint * aBlueprint, const UEdGraph * aGraph)
{
	return aBlueprint && aGraph && aGraph->GetOuter() == aBlueprint && aBlueprint->FunctionGraphs.Contains(aGraph)
		&& aGraph->GetClass() == UEdGraph::StaticClass()
		&& aGraph->Schema && aGraph->Schema->IsChildOf(UEdGraphSchema_K2::StaticClass())
		&& aGraph->GetFName() != UEdGraphSchema_K2::FN_UserConstructionScript;
}

bool BlueprintDiffFunction::Revert(
	UBlueprint * aWorkingBlueprint,
	const UBlueprint * aRevisionBlueprint,
	UEdGraph * aWorkingGraph,
	UEdGraph * aRevisionGraph,
	FText & aError)
{
	aError = FText::GetEmpty();
	if (!aWorkingBlueprint || !aRevisionBlueprint || aWorkingBlueprint->GetOutermost() == aRevisionBlueprint->GetOutermost()
		|| aWorkingBlueprint->GetClass() != aRevisionBlueprint->GetClass() || (!aWorkingGraph && !aRevisionGraph)
		|| (aWorkingGraph && !IsFunction(aWorkingBlueprint, aWorkingGraph))
		|| (aRevisionGraph && !IsFunction(aRevisionBlueprint, aRevisionGraph))
		|| (aWorkingGraph && aRevisionGraph && aWorkingGraph->Schema != aRevisionGraph->Schema))
	{
		aError = LOCTEXT("FunctionOnly", "Revert is available for top-level Blueprint function graphs from this asset.");
		return false;
	}

	if (!aRevisionGraph)
	{
		if (!aWorkingGraph->bAllowDeletion)
		{
			aError = LOCTEXT("CannotDelete", "This function cannot be deleted by the Blueprint editor.");
			return false;
		}

		if (FBlueprintEditorUtils::IsFunctionUsed(aWorkingBlueprint, aWorkingGraph->GetFName()))
		{
			aError = LOCTEXT("FunctionInUse", "Cannot remove this function because the working Blueprint still calls it.");
			return false;
		}
		const FScopedTransaction lTransaction(LOCTEXT("RemoveAddedFunction", "Revert added Blueprint function"));
		aWorkingBlueprint->Modify();
		FBlueprintEditorUtils::RemoveGraph(aWorkingBlueprint, aWorkingGraph);
		return true;
	}

	const FName lRevisionName = aRevisionGraph->GetFName();
	if (aWorkingGraph && aWorkingGraph->GetFName() != lRevisionName)
	{
		aError = LOCTEXT("CannotRevertRename",
			"Cannot revert a renamed function in isolation. Unreal can update callers and overrides when changing the function name.");
		return false;
	}
	if (!aWorkingGraph && !FBlueprintEditorUtils::IsGraphNameUnique(aWorkingBlueprint, lRevisionName))
	{
		aError = LOCTEXT("NameInUse", "The function name from the selected revision is already in use.");
		return false;
	}

	UEdGraph * const lStagedGraph = StageFunction(aWorkingBlueprint, aRevisionBlueprint, aWorkingGraph, aRevisionGraph, aError);
	if (!lStagedGraph)
		return false;

	FScopedTransaction lTransaction(LOCTEXT("RevertFunction", "Revert Blueprint function to compared revision"));
	if (aWorkingGraph)
	{
		if (!MoveFunctionContents(lStagedGraph, aWorkingGraph))
		{
			lTransaction.Cancel();
			aError = LOCTEXT("CannotMoveBody", "Unreal could not install the copied function body.");
			return false;
		}
		aWorkingBlueprint->Modify();
		aWorkingGraph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(aWorkingBlueprint);
	}
	else
	{
		lStagedGraph->Modify(false);
		if (!lStagedGraph->Rename(*lRevisionName.ToString(), aWorkingBlueprint, REN_DontCreateRedirectors | REN_DoNotDirty))
		{
			lTransaction.Cancel();
			aError = LOCTEXT("CannotMoveFunction", "Unreal could not restore the function into the working Blueprint.");
			return false;
		}
		lStagedGraph->ClearFlags(RF_Transient);
		aWorkingBlueprint->Modify();
		aWorkingBlueprint->FunctionGraphs.Add(lStagedGraph);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(aWorkingBlueprint);
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
