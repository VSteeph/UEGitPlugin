using UnrealBuildTool;

public class GitSourceControlTestHostEditorTarget : TargetRules
{
	public GitSourceControlTestHostEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.AddRange(new[] { "GitSourceControlTestHost", "GitSourceControlTests" });
	}
}
