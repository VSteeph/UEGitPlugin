#include "GitLockScenario.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_RefuseCheckout_When_AssetHasNewerChangesOnBase,
	"UEGitPlugin.Integration.Should_RefuseCheckout_When_AssetHasNewerChangesOnBase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_RefuseCheckout_When_AssetHasNewerChangesOnBase::RunTest(const FString& Parameters)
{
	// Arrange: a colleague pushed a newer version of the asset, not yet fetched locally.
	FGitLockScenario Scenario(*this);
	FString ColleagueTree;
	FString ColleagueCommit;
	FString LocalBase;
	if (!Scenario.Prepare()
		|| !Scenario.Repository.RunGit(TEXT("rev-parse origin/main"), LocalBase)
		|| !Scenario.Repository.MakeRemoteTreeWithFileContent(TEXT("main"), Scenario.RelativeAsset, TEXT("Colleague content\n"), ColleagueTree)
		|| !Scenario.Repository.PublishTreeOnRemote(ColleagueTree, TEXT("main"), ColleagueCommit)
		|| !Scenario.Repository.RunGit(FString::Printf(TEXT("update-ref refs/remotes/origin/main %s"), *LocalBase)))
	{
		return false;
	}
	AddExpectedError(TEXT("file has newer changes on remote branch"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Command 'CheckOut' Failed"), EAutomationExpectedErrorFlags::Contains, 0);

	// Act
	const TArray<FString> Errors = Scenario.CheckOutAsset();

	// Assert
	TestTrue(TEXT("The check-out is refused because the asset is out of date"), FGitLockScenario::ContainsMessage(Errors, TEXT("file has newer changes on remote branch")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_RefuseCheckout_When_BaseBranchCannotBeFetched,
	"UEGitPlugin.Integration.Should_RefuseCheckout_When_BaseBranchCannotBeFetched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_RefuseCheckout_When_BaseBranchCannotBeFetched::RunTest(const FString& Parameters)
{
	// Arrange: an up-to-date asset, but the remote cannot be reached.
	FGitLockScenario Scenario(*this);
	const FString UnreachableRemote = FPaths::Combine(Scenario.Repository.RepositoryRoot, TEXT("../MissingRemote.git"));
	if (!Scenario.Prepare()
		|| !Scenario.Repository.RunGit(TEXT("remote get-url origin"), Scenario.OriginalRemoteUrl)
		|| !Scenario.Repository.RunGit(FString::Printf(TEXT("remote set-url origin \"%s\""), *UnreachableRemote)))
	{
		return false;
	}
	AddExpectedError(TEXT("could not be fetched"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Command 'CheckOut' Failed"), EAutomationExpectedErrorFlags::Contains, 0);
	// Git reports the unreachable remote on several stderr lines.
	AddExpectedError(TEXT("MissingRemote.git"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Could not read from remote repository"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("correct access rights"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("the repository exists"), EAutomationExpectedErrorFlags::Contains, 0);

	// Act
	const TArray<FString> Errors = Scenario.CheckOutAsset();

	// Assert
	TestTrue(TEXT("The check-out is refused because the base branch cannot be verified"), FGitLockScenario::ContainsMessage(Errors, TEXT("remote base branch could not be fetched")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepKnownAssetState_When_CheckOutIsRefusedForOutdatedAsset,
	"UEGitPlugin.Integration.Should_KeepKnownAssetState_When_CheckOutIsRefusedForOutdatedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepKnownAssetState_When_CheckOutIsRefusedForOutdatedAsset::RunTest(const FString& Parameters)
{
	// Arrange: the editor knows the asset as tracked, then a colleague pushes a newer version not fetched yet.
	FGitLockScenario Scenario(*this);
	FString ColleagueTree;
	FString ColleagueCommit;
	FString LocalBase;
	if (!Scenario.Prepare()
		|| !TestTrue(TEXT("The editor reads the asset status"), Scenario.Execute(ISourceControlOperation::Create<FUpdateStatus>()).IsEmpty())
		|| !Scenario.Repository.RunGit(TEXT("rev-parse origin/main"), LocalBase)
		|| !Scenario.Repository.MakeRemoteTreeWithFileContent(TEXT("main"), Scenario.RelativeAsset, TEXT("Colleague content\n"), ColleagueTree)
		|| !Scenario.Repository.PublishTreeOnRemote(ColleagueTree, TEXT("main"), ColleagueCommit)
		|| !Scenario.Repository.RunGit(FString::Printf(TEXT("update-ref refs/remotes/origin/main %s"), *LocalBase)))
	{
		return false;
	}
	AddExpectedError(TEXT("file has newer changes on remote branch"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Command 'CheckOut' Failed"), EAutomationExpectedErrorFlags::Contains, 0);

	// Act
	Scenario.CheckOutAsset();

	// Assert
	const FSourceControlStatePtr State = Scenario.GetCachedState();
	TestTrue(TEXT("The asset is still known as tracked"), State.IsValid() && State->IsSourceControlled());
	TestFalse(TEXT("The asset is not reported as unknown"), State.IsValid() && State->IsUnknown());
	TestFalse(TEXT("The asset is reported as outdated"), State.IsValid() && State->IsCurrent());
	return !HasAnyErrors();
}

#endif
