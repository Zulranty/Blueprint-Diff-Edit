using UnrealBuildTool;

public class BlueprintDiffEdit : ModuleRules
{
	public BlueprintDiffEdit(ReadOnlyTargetRules aTarget) : base(aTarget)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Projects",
			"Engine",
			"EditorFramework",
			"UnrealEd",
			"Slate",
			"SlateCore",
			"InputCore",
			"ApplicationCore",
			"ToolMenus",
			"ContentBrowser",
			"AssetRegistry",
			"SourceControl",
			"Kismet",
			"BlueprintGraph",
			"GraphEditor",
			"PropertyEditor",
			"MaterialEditor",
			"UMG",
			"UMGEditor"
		});
	}
}
