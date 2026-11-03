using UnrealBuildTool;

public class GitSourceControlTests : ModuleRules
{
	public GitSourceControlTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GitSourceControl",
			"Projects",
			"SourceControl",
			"UnrealEd"
		});
	}
}
