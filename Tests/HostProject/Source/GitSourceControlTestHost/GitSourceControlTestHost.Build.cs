using UnrealBuildTool;

public class GitSourceControlTestHost : ModuleRules
{
	public GitSourceControlTestHost(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.Add("Core");
	}
}
