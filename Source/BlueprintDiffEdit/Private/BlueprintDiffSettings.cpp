#include "BlueprintDiffModel.h"

#include "BlueprintDiffFunction.h"
#include "DiffUtils.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "BlueprintDiffSettings"

namespace
{
	FString GetRollbackValue(const FProperty & aProperty, const void * aContainer)
	{
		FString lValue;
		aProperty.ExportTextItem_Direct(lValue, aProperty.ContainerPtrToValuePtr<void>(aContainer),
			nullptr, nullptr, PPF_None);
		return lValue;
	}

	bool CanCopyProperty(const FProperty * aOld, const FProperty * aWorking,
		const void * aBefore, const UPackage & aRevisionPackage)
	{
		return aOld && aWorking && aOld->SameType(aWorking) && aOld->ArrayDim == aWorking->ArrayDim
			&& !aOld->ContainsInstancedObjectProperty() && !aOld->IsA<FDelegateProperty>()
			&& !aOld->IsA<FMulticastDelegateProperty>()
			&& !GetRollbackValue(*aOld, aBefore).Contains(aRevisionPackage.GetName(), ESearchCase::CaseSensitive);
	}

	bool RevertObjectProperty(UBlueprint & aBlueprint, const UObject & aOld, UObject & aWorking,
		FName aProperty, const UPackage & aRevisionPackage, FText & aError)
	{
		const FProperty * const lOld = FindFProperty<FProperty>(aOld.GetClass(), aProperty);
		FProperty * const lWorking = FindFProperty<FProperty>(aWorking.GetClass(), aProperty);
		if (!CanCopyProperty(lOld, lWorking, &aOld, aRevisionPackage) || !lWorking->HasAnyPropertyFlags(CPF_Edit)
			|| lWorking->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnTemplate))
		{
			aError = LOCTEXT("IncompatibleProperty", "This value cannot be restored in isolation. It may be read-only, have changed type, or depend on the historical asset.");
			return false;
		}

		const FScopedTransaction lTransaction(LOCTEXT("RevertProperty", "Revert Blueprint setting to compared revision"));
		aBlueprint.Modify();
		aWorking.SetFlags(RF_Transactional);
		aWorking.Modify();
		aWorking.PreEditChange(lWorking);
		lWorking->CopyCompleteValue(lWorking->ContainerPtrToValuePtr<void>(&aWorking), lOld->ContainerPtrToValuePtr<void>(&aOld));
		const UObject * lObjects[] = { &aWorking };
		FPropertyChangedEvent lChange(lWorking, EPropertyChangeType::ValueSet, lObjects);
		aWorking.PostEditChangeProperty(lChange);
		FBlueprintEditorUtils::MarkBlueprintAsModified(&aBlueprint, lChange);
		FBlueprintEditorUtils::PostEditChangeBlueprintActors(&aBlueprint);
		return true;
	}

	bool RestoreVariable(UBlueprint & aWorking, const UBlueprint & aRevision,
		const FBPVariableDescription & aVariable, FText & aError)
	{
		const FName lName = aVariable.VarName;
		if (!FBlueprintEditorUtils::IsGraphNameUnique(&aWorking, lName)
			|| (aWorking.SkeletonGeneratedClass && FindFProperty<FProperty>(aWorking.SkeletonGeneratedClass, lName))
			|| aWorking.NewVariables.ContainsByPredicate([&aVariable](const FBPVariableDescription & aOther)
				{ return aOther.VarGuid == aVariable.VarGuid; }))
		{
			aError = LOCTEXT("MemberCollision", "The member from the selected revision conflicts with an existing Blueprint member.");
			return false;
		}

		FBPVariableDescription lRestored = aVariable;
		if (const UObject * const lDefaults = DiffUtils::GetCDO(&aRevision))
		{
			if (const FProperty * const lDefault = FindFProperty<FProperty>(lDefaults->GetClass(), lName))
				lRestored.DefaultValue = GetRollbackValue(*lDefault, lDefaults);
		}
		const UPackage * const lRevisionPackage = aRevision.GetOutermost();
		const FProperty * const lType = FindFProperty<FProperty>(FBPVariableDescription::StaticStruct(), GET_MEMBER_NAME_CHECKED(FBPVariableDescription, VarType));
		if (!CanCopyProperty(lType, lType, &aVariable, *lRevisionPackage)
			|| lRestored.DefaultValue.Contains(lRevisionPackage->GetName(), ESearchCase::CaseSensitive)
			|| (!lRestored.RepNotifyFunc.IsNone()
				&& (!aWorking.SkeletonGeneratedClass || !aWorking.SkeletonGeneratedClass->FindFunctionByName(lRestored.RepNotifyFunc))))
		{
			aError = LOCTEXT("MemberDependency", "The old variable depends on a type, value, or function that could not be restored from this revision.");
			return false;
		}

		const FScopedTransaction lTransaction(LOCTEXT("RestoreVariable", "Restore Blueprint variable from compared revision"));
		aWorking.Modify();
		aWorking.NewVariables.Add(MoveTemp(lRestored));
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&aWorking);
		FKismetEditorUtilities::CompileBlueprint(&aWorking);
		return true;
	}
}

bool BlueprintDiffModel::RevertSetting(UBlueprint * aWorking, const UBlueprint * aRevision,
	const FBlueprintSettingTarget & aTarget, FText & aError)
{
	aError = FText::GetEmpty();
	if (!aWorking || !aRevision || aWorking->GetClass() != aRevision->GetClass()
		|| aWorking->GetOutermost() == aRevision->GetOutermost() || aTarget.Kind == EBlueprintSettingKind::None)
		return false;

	const FName lName = aTarget.Member;
	const int32 lOldIndex = FBlueprintEditorUtils::FindNewVariableIndex(aRevision, lName);
	const int32 lNewIndex = FBlueprintEditorUtils::FindNewVariableIndex(aWorking, lName);
	const FBPVariableDescription * const lOld = lOldIndex != INDEX_NONE ? &aRevision->NewVariables[lOldIndex] : nullptr;
	FBPVariableDescription * const lNew = lNewIndex != INDEX_NONE ? &aWorking->NewVariables[lNewIndex] : nullptr;
	const bool lMemberOnlyOnOneSide = (lOld == nullptr) != (lNew == nullptr);
	const bool lRevertMember = aTarget.Kind == EBlueprintSettingKind::Member
		|| (aTarget.Kind == EBlueprintSettingKind::ClassDefault && lMemberOnlyOnOneSide);

	if (aTarget.Kind == EBlueprintSettingKind::DispatcherSignature || (aTarget.IsDispatcher && lRevertMember))
	{
		return BlueprintDiffFunction::RevertDispatcher(aWorking, aRevision,
			FindDispatcherSignature(aWorking, lName), FindDispatcherSignature(aRevision, lName), aError);
	}
	if (lRevertMember)
	{
		if (!lMemberOnlyOnOneSide)
			return false;
		if (lOld)
			return RestoreVariable(*aWorking, *aRevision, *lOld, aError);
		if (FBlueprintEditorUtils::IsVariableUsed(aWorking, lName))
		{
			aError = LOCTEXT("VariableInUse", "This added variable is still used by a Blueprint. Remove its references before reverting it.");
			return false;
		}
		const FScopedTransaction lTransaction(LOCTEXT("RemoveAddedVariable", "Revert added Blueprint variable"));
		aWorking->Modify();
		FBlueprintEditorUtils::RemoveMemberVariable(aWorking, lName);
		FKismetEditorUtilities::CompileBlueprint(aWorking);
		return true;
	}

	const UPackage & lRevisionPackage = *aRevision->GetOutermost();
	if (aTarget.Kind == EBlueprintSettingKind::ClassDefault)
	{
		const UObject * const lOldDefaults = DiffUtils::GetCDO(aRevision);
		UObject * const lNewDefaults = const_cast<UObject *>(DiffUtils::GetCDO(aWorking));
		return lOldDefaults && lNewDefaults
			&& RevertObjectProperty(*aWorking, *lOldDefaults, *lNewDefaults, aTarget.Property, lRevisionPackage, aError);
	}
	if (aTarget.Kind == EBlueprintSettingKind::ClassSetting)
		return RevertObjectProperty(*aWorking, *aRevision, *aWorking, aTarget.Property, lRevisionPackage, aError);
	if (!lOld || !lNew)
		return false;

	const FProperty * const lField = FindFProperty<FProperty>(FBPVariableDescription::StaticStruct(), aTarget.Property);
	if (!CanCopyProperty(lField, lField, lOld, lRevisionPackage))
	{
		aError = LOCTEXT("IncompatibleMemberProperty", "This variable setting refers to a type or value in the historical package.");
		return false;
	}
	if (aTarget.Property == GET_MEMBER_NAME_CHECKED(FBPVariableDescription, VarType))
	{
		if (aTarget.IsDispatcher || lOld->VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
		{
			aError = LOCTEXT("DispatcherType", "Revert the dispatcher signature to restore its parameter types.");
			return false;
		}
		const FEdGraphPinType lType = lOld->VarType;
		FBlueprintEditorUtils::ChangeMemberVariableType(aWorking, lName, lType);
		const int32 lIndex = FBlueprintEditorUtils::FindNewVariableIndex(aWorking, lName);
		return lIndex != INDEX_NONE && aWorking->NewVariables[lIndex].VarType == lType;
	}
	if (aTarget.Property == GET_MEMBER_NAME_CHECKED(FBPVariableDescription, DefaultValue))
	{
		const UObject * const lOldDefaults = DiffUtils::GetCDO(aRevision);
		UObject * const lNewDefaults = const_cast<UObject *>(DiffUtils::GetCDO(aWorking));
		return lOldDefaults && lNewDefaults
			&& RevertObjectProperty(*aWorking, *lOldDefaults, *lNewDefaults, lName, lRevisionPackage, aError);
	}
	if (aTarget.Property == GET_MEMBER_NAME_CHECKED(FBPVariableDescription, RepNotifyFunc) && !lOld->RepNotifyFunc.IsNone()
		&& (!aWorking->SkeletonGeneratedClass || !aWorking->SkeletonGeneratedClass->FindFunctionByName(lOld->RepNotifyFunc)))
	{
		aError = LOCTEXT("MissingRepNotify", "Restore the replication callback in the working Blueprint before reverting this setting.");
		return false;
	}

	const FScopedTransaction lTransaction(LOCTEXT("RevertVariableProperty", "Revert Blueprint variable setting to compared revision"));
	aWorking->Modify();
	lField->CopyCompleteValue_InContainer(lNew, lOld);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(aWorking);
	return true;
}

#undef LOCTEXT_NAMESPACE
