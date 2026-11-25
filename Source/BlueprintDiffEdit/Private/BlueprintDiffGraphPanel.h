#pragma once

#include "SGraphPanel.h"

namespace BlueprintDiffGraphPanel
{
	// UE 5.6 takes diff results at construction and exposes the storage to derived panels.
	// Use a base-class member pointer so the existing Blueprint panel keeps its native editor behavior.
	class FDiffAccess : public SGraphPanel
	{
	public:
		static auto GetMember() { return &FDiffAccess::DiffResults; }
	};

	inline TSharedPtr<TArray<FDiffSingleResult>> & GetResults(SGraphPanel & aPanel)
	{
		return aPanel.*FDiffAccess::GetMember();
	}
}
