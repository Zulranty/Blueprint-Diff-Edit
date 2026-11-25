#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;

namespace BlueprintDiffFunction
{
	bool IsFunction(const UBlueprint * aBlueprint, const UEdGraph * aGraph);

	bool Revert(
		UBlueprint * aWorkingBlueprint,
		const UBlueprint * aRevisionBlueprint,
		UEdGraph * aWorkingGraph,
		UEdGraph * aRevisionGraph,
		FText & aError);
}
