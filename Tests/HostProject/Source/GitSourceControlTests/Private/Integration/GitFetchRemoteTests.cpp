#include "GitTestRepository.h"
#include "GitSourceControlUtils.h"

#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_UpdateBaseBranch_When_UpstreamBranchWasDeletedOnRemote,
	"UEGitPlugin.Integration.Should_UpdateBaseBranch_When_UpstreamBranchWasDeletedOnRemote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_UpdateBaseBranch_When_UpstreamBranchWasDeletedOnRemote::RunTest(const FString& Parameters)
{
	// Arrange: a feature branch whose remote counterpart was deleted after merge, while the base branch moved on the remote.
	const FGitTestRepository Repository(*this);
	if (!TestTrue(TEXT("The isolated Git repository is available"), Repository.IsAvailable()))
	{
		return false;
	}

	FString RemotePath;
	FString InitialBaseCommit;
	FString InitialTree;
	if (!Repository.RunGit(TEXT("remote get-url origin"), RemotePath)
		|| !Repository.RunGit(TEXT("rev-parse refs/remotes/origin/main"), InitialBaseCommit)
		|| !Repository.RunGit(TEXT("rev-parse HEAD^{tree}"), InitialTree))
	{
		return false;
	}

	const FString FeatureBranch = FString::Printf(TEXT("feature-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT
	{
		Repository.RunGitQuietly(TEXT("checkout --quiet main"));
		Repository.RunGitQuietly(FString::Printf(TEXT("branch --quiet -D %s"), *FeatureBranch));
		Repository.RunGitQuietly(FString::Printf(TEXT("update-ref -d refs/remotes/origin/%s"), *FeatureBranch));
		Repository.RunGitQuietly(TEXT("fetch --quiet origin main"));
		Repository.RunGitQuietly(TEXT("merge --quiet --ff-only origin/main"));
	};

	FString Output;
	FString NewBaseCommit;
	if (!Repository.RunGit(FString::Printf(TEXT("checkout --quiet -b %s"), *FeatureBranch))
		|| !Repository.RunGit(FString::Printf(TEXT("push --quiet --set-upstream origin %s"), *FeatureBranch))
		|| !Repository.RunGit(FString::Printf(TEXT("update-ref -d refs/heads/%s"), *FeatureBranch), Output, RemotePath)
		|| !Repository.PublishTreeOnRemote(InitialTree, TEXT("main"), NewBaseCommit)
		|| !Repository.RunGit(FString::Printf(TEXT("update-ref refs/remotes/origin/main %s"), *InitialBaseCommit)))
	{
		return false;
	}

	// Act
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	GitSourceControlUtils::FetchRemote(Repository.GitBinary, Repository.RepositoryRoot, false, InfoMessages, ErrorMessages);

	// Assert
	FString FetchedBaseCommit;
	Repository.RunGit(TEXT("rev-parse refs/remotes/origin/main"), FetchedBaseCommit);
	TestEqual(TEXT("The remote base branch is updated"), FetchedBaseCommit, NewBaseCommit);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_NotFetchOtherRemoteBranches_When_Refreshing,
	"UEGitPlugin.Integration.Should_NotFetchOtherRemoteBranches_When_Refreshing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_NotFetchOtherRemoteBranches_When_Refreshing::RunTest(const FString& Parameters)
{
	// Arrange: a colleague branch exists on the remote but has never been fetched locally.
	const FGitTestRepository Repository(*this);
	if (!TestTrue(TEXT("The isolated Git repository is available"), Repository.IsAvailable()))
	{
		return false;
	}

	const FString OtherBranch = FString::Printf(TEXT("colleague-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT
	{
		Repository.RunGitQuietly(FString::Printf(TEXT("push --quiet origin --delete %s"), *OtherBranch));
		Repository.RunGitQuietly(FString::Printf(TEXT("update-ref -d refs/remotes/origin/%s"), *OtherBranch));
	};

	if (!Repository.RunGit(FString::Printf(TEXT("push --quiet origin HEAD:refs/heads/%s"), *OtherBranch))
		|| !Repository.RunGit(FString::Printf(TEXT("update-ref -d refs/remotes/origin/%s"), *OtherBranch)))
	{
		return false;
	}

	// Act
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	GitSourceControlUtils::FetchRemote(Repository.GitBinary, Repository.RepositoryRoot, false, InfoMessages, ErrorMessages);

	// Assert
	FString Output;
	Repository.RunGit(FString::Printf(TEXT("for-each-ref refs/remotes/origin/%s"), *OtherBranch), Output);
	TestTrue(TEXT("The unrelated remote branch is not fetched"), Output.IsEmpty());
	return !HasAnyErrors();
}

#endif
