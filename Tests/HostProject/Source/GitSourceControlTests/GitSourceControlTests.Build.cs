using UnrealBuildTool;

public class GitSourceControlTests : ModuleRules
{
	public GitSourceControlTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"ContentBrowser",
			"ContentBrowserData",
			"Core",
			"CoreUObject",
			"Engine",
			"GitSourceControl",
			"InputCore",
			"Projects",
			"Slate",
			"SlateCore",
			"SourceControl",
			"ToolMenus",
			"UnrealEd"
		});
	}
}
