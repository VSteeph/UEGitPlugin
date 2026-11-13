#include "GitLockScenario.h"

#include "GitSourceControlSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_UseServerLockOwnerName_When_DesignerLocksWithoutSavedLfsUserName,
	"UEGitPlugin.Integration.Should_UseServerLockOwnerName_When_DesignerLocksWithoutSavedLfsUserName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_UseServerLockOwnerName_When_DesignerLocksWithoutSavedLfsUserName::RunTest(const FString& Parameters)
{
	// Arrange: no saved LFS username, and a Git user.name that differs from the LFS server login.
	FGitSourceControlModule& Module = FGitSourceControlModule::Get();
	FGitSourceControlSettings& Settings = Module.AccessSettings();
	const FString SavedLfsUserName = Settings.GetLfsUserName();
	Settings.SetLfsUserName(FString());
	FGitLockScenario Scenario(*this);
	FGitSourceControlProvider& Provider = Module.GetProvider();
	if (!Scenario.Prepare()
		|| !TestNotEqual(TEXT("Git user.name differs from the LFS login"), Provider.GetUserName(), FString(TEXT("Designer"))))
	{
		Settings.SetLfsUserName(SavedLfsUserName);
		return false;
	}

	// Act: the first lock reveals the server name, applied on the next editor tick.
	const TArray<FString> Errors = Scenario.CheckOutAsset();
	Provider.Tick();

	// Assert
	TestTrue(TEXT("The lock succeeds"), Errors.IsEmpty());
	TestEqual(TEXT("The plugin now uses the server name"), Provider.GetLockUser(), FString(TEXT("Designer")));
	TestTrue(TEXT("The saved LFS username is left to the user"), Settings.GetLfsUserName().IsEmpty());

	Settings.SetLfsUserName(SavedLfsUserName);
	Provider.UpdateSettings();
	return !HasAnyErrors();
}

#endif
