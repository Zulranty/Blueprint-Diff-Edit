#include "BlueprintDiffFunction.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

/**
 * Exercises function rollback on transient Blueprints using the same operation as the diff menu.
 * The isolated transaction buffer keeps undo records out of the user's editing session.
 */
BEGIN_DEFINE_SPEC(FBlueprintDiffFunctionTests, "BlueprintDiffEdit.Function",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	TStrongObjectPtr<UBlueprint> m_Working;
	TStrongObjectPtr<UBlueprint> m_Revision;
	TStrongObjectPtr<UTransactor> m_PreviousTransactor;
	TStrongObjectPtr<UTransBuffer> m_Transactor;
	UBlueprint * DoMakeBlueprint();
	UEdGraph * DoMakeFunction(UBlueprint * aBlueprint, FName aName);
END_DEFINE_SPEC(FBlueprintDiffFunctionTests)

UBlueprint * FBlueprintDiffFunctionTests::DoMakeBlueprint()
{
	const FString lPackageName = TEXT("/Temp/BlueprintDiff_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	return FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), CreatePackage(*lPackageName),
		FName(TEXT("BP_DiffTest")), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("BlueprintDiffFunction")));
}

UEdGraph * FBlueprintDiffFunctionTests::DoMakeFunction(UBlueprint * aBlueprint, FName aName)
{
	UEdGraph * const lFunction = FBlueprintEditorUtils::CreateNewGraph(aBlueprint, aName,
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(aBlueprint, lFunction, true, nullptr);
	return lFunction;
}

void FBlueprintDiffFunctionTests::Define()
{
	// Restore linked nodes and an owned nested graph while preserving the current graph and its siblings.
	It("Restores the selected function body and supports undo and redo", [this]
	{
		if (!TestNotNull(TEXT("Working Blueprint"), m_Working.Get()) || !TestNotNull(TEXT("Revision Blueprint"), m_Revision.Get()))
			return;

		const FName FUNCTION_NAME(TEXT("ReviewedFunction"));
		UEdGraph * const lOld = DoMakeFunction(m_Revision.Get(), FUNCTION_NAME);
		UEdGraph * const lWorking = DoMakeFunction(m_Working.Get(), FUNCTION_NAME);
		UEdGraph * const lOther = DoMakeFunction(m_Working.Get(), FName(TEXT("OtherFunction")));

		// Add a linked branch and collapsed graph to the old revision only.
		TArray<UK2Node_FunctionEntry *> lEntries;
		lOld->GetNodesOfClass(lEntries);
		if (!TestEqual(TEXT("Revision entry count"), lEntries.Num(), 1))
			return;
		FGraphNodeCreator<UK2Node_IfThenElse> lBranchCreator(*lOld);
		UK2Node_IfThenElse * const lOldBranch = lBranchCreator.CreateNode();
		lBranchCreator.Finalize();
		UEdGraphPin * const lThen = lEntries[0]->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin * const lExecute = lOldBranch->FindPin(UEdGraphSchema_K2::PN_Execute);
		if (!TestNotNull(TEXT("Entry execution output"), lThen) || !TestNotNull(TEXT("Branch execution input"), lExecute))
			return;
		lThen->MakeLinkTo(lExecute);
		lEntries[0]->MetaData.ToolTip = FText::FromString(TEXT("Before"));
		FGraphNodeCreator<UK2Node_Composite> lCompositeCreator(*lOld);
		UK2Node_Composite * const lComposite = lCompositeCreator.CreateNode();
		lCompositeCreator.Finalize();

		// Apply the production operation and inspect both the restored payload and ownership.
		FText lError;
		if (!TestTrue(TEXT("Revert succeeded"), BlueprintDiffFunction::Revert(m_Working.Get(), m_Revision.Get(), lWorking, lOld, lError)))
		{
			AddInfo(lError.ToString());
			return;
		}
		TestTrue(TEXT("Existing graph identity preserved"), m_Working->FunctionGraphs.Contains(lWorking));
		TestTrue(TEXT("Other graph preserved"), m_Working->FunctionGraphs.Contains(lOther));

		TArray<UK2Node_IfThenElse *> lBranches;
		TArray<UK2Node_FunctionEntry *> lRestoredEntries;
		TArray<UK2Node_Composite *> lComposites;
		lWorking->GetNodesOfClass(lBranches);
		lWorking->GetNodesOfClass(lRestoredEntries);
		lWorking->GetNodesOfClass(lComposites);
		if (TestEqual(TEXT("Restored branch count"), lBranches.Num(), 1)
			&& TestEqual(TEXT("Restored entry count"), lRestoredEntries.Num(), 1))
		{
			TestEqual(TEXT("Branch owner"), lBranches[0]->GetGraph(), lWorking);
			TestEqual(TEXT("Original entry metadata"), lRestoredEntries[0]->MetaData.ToolTip.ToString(), FString(TEXT("Before")));
			UEdGraphPin * const lRestoredThen = lRestoredEntries[0]->FindPin(UEdGraphSchema_K2::PN_Then);
			UEdGraphPin * const lRestoredExecute = lBranches[0]->FindPin(UEdGraphSchema_K2::PN_Execute);
			if (TestNotNull(TEXT("Restored entry execution output"), lRestoredThen)
				&& TestNotNull(TEXT("Restored branch execution input"), lRestoredExecute)
				&& TestEqual(TEXT("Restored outgoing link count"), lRestoredThen->LinkedTo.Num(), 1))
			{
				TestEqual(TEXT("Link points at the cloned branch"), lRestoredThen->LinkedTo[0], lRestoredExecute);
			}
		}
		if (TestEqual(TEXT("Restored composite count"), lComposites.Num(), 1)
			&& TestNotNull(TEXT("Restored bound graph"), lComposites[0]->BoundGraph.Get()))
		{
			TestEqual(TEXT("Nested graph owner"), lComposites[0]->BoundGraph->GetOuter(), static_cast<UObject *>(lComposites[0]));
			TestTrue(TEXT("Nested graph registered with the function"), lWorking->SubGraphs.Contains(lComposites[0]->BoundGraph));
		}
		TestEqual(TEXT("Revision branch stays in the old graph"), lOldBranch->GetGraph(), lOld);
		TestEqual(TEXT("Revision composite stays in the old graph"), lComposite->GetGraph(), lOld);

		// A single undo reattaches the prior body. Redo restores the cloned body.
		TestTrue(TEXT("Undo reverted function overwrite"), m_Transactor->Undo());
		lBranches.Reset();
		lWorking->GetNodesOfClass(lBranches);
		TestEqual(TEXT("No branch after undo"), lBranches.Num(), 0);
		TestTrue(TEXT("Redo restored function overwrite"), m_Transactor->Redo());
		lBranches.Reset();
		lWorking->GetNodesOfClass(lBranches);
		TestEqual(TEXT("One branch after redo"), lBranches.Num(), 1);
	});

	It("Restores self-context calls with a valid owning Blueprint", [this]
	{
		const FName HELPER_NAME(TEXT("SharedHelper"));
		const FName REVIEW_NAME(TEXT("FunctionWithSelfCall"));
		DoMakeFunction(m_Revision.Get(), HELPER_NAME);
		DoMakeFunction(m_Working.Get(), HELPER_NAME);
		UEdGraph * const lRevision = DoMakeFunction(m_Revision.Get(), REVIEW_NAME);
		UEdGraph * const lWorking = DoMakeFunction(m_Working.Get(), REVIEW_NAME);

		FGraphNodeCreator<UK2Node_CallFunction> lCreator(*lRevision);
		UK2Node_CallFunction * const lCall = lCreator.CreateNode();
		lCall->FunctionReference.SetSelfMember(HELPER_NAME);
		lCreator.Finalize();
		if (!TestNotNull(TEXT("Revision call resolves before copying"), lCall->GetTargetFunction()))
			return;

		FText lError;
		if (!TestTrue(TEXT("Revert an existing function with a self call"),
			BlueprintDiffFunction::Revert(m_Working.Get(), m_Revision.Get(), lWorking, lRevision, lError)))
		{
			AddInfo(lError.ToString());
			return;
		}

		TArray<UK2Node_CallFunction *> lCalls;
		lWorking->GetNodesOfClass(lCalls);
		if (TestEqual(TEXT("One restored call"), lCalls.Num(), 1))
		{
			TestTrue(TEXT("Restored node still calls self"), lCalls[0]->FunctionReference.IsSelfContext());
			TestTrue(TEXT("Restored self resolves to the working Blueprint"),
				lCalls[0]->GetTargetFunction()
				&& lCalls[0]->GetTargetFunction()->GetOwnerClass()->ClassGeneratedBy == m_Working.Get());
		}
		TestTrue(TEXT("Undo restores the working function before the self call"), m_Transactor->Undo());
		lCalls.Reset();
		lWorking->GetNodesOfClass(lCalls);
		TestEqual(TEXT("No call after undo"), lCalls.Num(), 0);
		TestTrue(TEXT("Redo restores the self call"), m_Transactor->Redo());
		lCalls.Reset();
		lWorking->GetNodesOfClass(lCalls);
		TestEqual(TEXT("One call after redo"), lCalls.Num(), 1);

		FBlueprintEditorUtils::RemoveGraph(m_Working.Get(), lWorking);
		if (!TestTrue(TEXT("Restore an absent function with a self call"),
			BlueprintDiffFunction::Revert(m_Working.Get(), m_Revision.Get(), nullptr, lRevision, lError)))
		{
			AddInfo(lError.ToString());
			return;
		}
		const UEdGraph * const lRestored = FindObject<UEdGraph>(m_Working.Get(), *REVIEW_NAME.ToString());
		if (TestNotNull(TEXT("Restored graph"), lRestored))
			TestFalse(TEXT("Restored graph can be saved"), lRestored->HasAnyFlags(RF_Transient));
	});

	// A missing function is restored under its original name. A local-only function is removed.
	It("Restores a removed function and removes a local-only function", [this]
	{
		if (!TestNotNull(TEXT("Working Blueprint"), m_Working.Get()) || !TestNotNull(TEXT("Revision Blueprint"), m_Revision.Get()))
			return;
		const FName REMOVED_NAME(TEXT("RemovedFunction"));
		UEdGraph * const lOld = DoMakeFunction(m_Revision.Get(), REMOVED_NAME);
		UEdGraph * const lAdded = DoMakeFunction(m_Working.Get(), FName(TEXT("AddedFunction")));

		FText lError;
		if (!TestTrue(TEXT("Restored missing function"), BlueprintDiffFunction::Revert(m_Working.Get(), m_Revision.Get(), nullptr, lOld, lError)))
		{
			AddInfo(lError.ToString());
			return;
		}
		UEdGraph * const lRestored = FindObject<UEdGraph>(m_Working.Get(), *REMOVED_NAME.ToString());
		if (TestNotNull(TEXT("Original name restored"), lRestored))
			TestTrue(TEXT("Restored graph listed in the Blueprint"), m_Working->FunctionGraphs.Contains(lRestored));
		TestTrue(TEXT("Removed local addition"), BlueprintDiffFunction::Revert(m_Working.Get(), m_Revision.Get(), lAdded, nullptr, lError));
		TestFalse(TEXT("Local addition no longer listed"), m_Working->FunctionGraphs.Contains(lAdded));
	});

	It("Refuses invalid or nonlocal reverts before changing the working graph", [this]
	{
		const FName REVIEW(TEXT("Review"));
		const FName MISSING(TEXT("MissingHelper"));
		UEdGraph * const lRevision = DoMakeFunction(m_Revision.Get(), REVIEW);
		UEdGraph * const lWorking = DoMakeFunction(m_Working.Get(), REVIEW);
		DoMakeFunction(m_Revision.Get(), MISSING);
		FGraphNodeCreator<UK2Node_CallFunction> lOldCreator(*lRevision);
		UK2Node_CallFunction * const lOldCall = lOldCreator.CreateNode();
		lOldCall->FunctionReference.SetSelfMember(MISSING);
		lOldCreator.Finalize();
		if (!TestNotNull(TEXT("Historical helper resolves in the historical Blueprint"), lOldCall->GetTargetFunction()))
			return;

		const FGuid lEntryId = lWorking->Nodes[0]->NodeGuid;
		FText lError;
		TestFalse(TEXT("Unknown working helper refuses revert"), BlueprintDiffFunction::Revert(
			m_Working.Get(), m_Revision.Get(), lWorking, lRevision, lError));
		TestTrue(TEXT("Diagnostic names the missing helper"), lError.ToString().Contains(MISSING.ToString()));
		TestEqual(TEXT("Working function remains unchanged"), lWorking->Nodes[0]->NodeGuid, lEntryId);

		UEdGraph * const lLocal = DoMakeFunction(m_Working.Get(), TEXT("LocalOnly"));
		FGraphNodeCreator<UK2Node_CallFunction> lWorkingCreator(*lWorking);
		UK2Node_CallFunction * const lWorkingCall = lWorkingCreator.CreateNode();
		UFunction * const lTarget = m_Working->SkeletonGeneratedClass->FindFunctionByName(lLocal->GetFName());
		if (!TestNotNull(TEXT("Working-only function resolves"), lTarget))
			return;
		lWorkingCall->SetFromFunction(lTarget);
		lWorkingCreator.Finalize();
		TestFalse(TEXT("A working-only function with callers cannot be removed"), BlueprintDiffFunction::Revert(
			m_Working.Get(), m_Revision.Get(), lLocal, nullptr, lError));
		TestTrue(TEXT("Referenced local graph remains in the Blueprint"), m_Working->FunctionGraphs.Contains(lLocal));

		lLocal->GraphGuid = lRevision->GraphGuid;
		TestFalse(TEXT("A function rename would exceed the selected graph"), BlueprintDiffFunction::Revert(
			m_Working.Get(), m_Revision.Get(), lLocal, lRevision, lError));
		TestEqual(TEXT("Working-only function name preserved"), lLocal->GetFName(), FName(TEXT("LocalOnly")));
		TestEqual(TEXT("Existing call unaffected"), lWorkingCall->GetFunctionName(), lLocal->GetFName());
	});

	// If the original name is occupied, the existing function graph must survive the failure.
	It("Protects the working Blueprint from a function name collision", [this]
	{
		if (!TestNotNull(TEXT("Working Blueprint"), m_Working.Get()) || !TestNotNull(TEXT("Revision Blueprint"), m_Revision.Get()))
			return;
		const FName SHARED_NAME(TEXT("OccupiedFunction"));
		UEdGraph * const lOld = DoMakeFunction(m_Revision.Get(), SHARED_NAME);
		UEdGraph * const lOccupied = DoMakeFunction(m_Working.Get(), SHARED_NAME);
		const int lCount = m_Working->FunctionGraphs.Num();

		FText lError;
		TestFalse(TEXT("Restore refused"), BlueprintDiffFunction::Revert(m_Working.Get(), m_Revision.Get(), nullptr, lOld, lError));
		TestFalse(TEXT("Refusal explains the error"), lError.IsEmpty());
		TestEqual(TEXT("Function count unchanged"), m_Working->FunctionGraphs.Num(), lCount);
		TestTrue(TEXT("Existing function retained"), m_Working->FunctionGraphs.Contains(lOccupied));
	});

	BeforeEach([this]
	{
		constexpr SIZE_T TRANSACTION_BYTES = 4 * 1024 * 1024;
		m_PreviousTransactor.Reset(GEditor->Trans.Get());
		m_Transactor.Reset(NewObject<UTransBuffer>());
		m_Transactor->Initialize(TRANSACTION_BYTES);
		GEditor->Trans = m_Transactor.Get();
		m_Working.Reset(DoMakeBlueprint());
		m_Revision.Reset(DoMakeBlueprint());
	});

	AfterEach([this]
	{
		GEditor->Trans = m_PreviousTransactor.Get();
		if (m_Working.IsValid())
			m_Working->GetOutermost()->SetDirtyFlag(false);
		if (m_Revision.IsValid())
			m_Revision->GetOutermost()->SetDirtyFlag(false);
		m_Working.Reset();
		m_Revision.Reset();
		m_Transactor.Reset();
		m_PreviousTransactor.Reset();
	});
}

#endif
