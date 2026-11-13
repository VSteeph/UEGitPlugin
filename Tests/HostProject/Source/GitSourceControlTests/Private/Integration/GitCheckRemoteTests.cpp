#include "GitTestRepository.h"
#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace GitCheckRemoteTests
{
	/** A feature branch with one lockable asset committed on it, while the remote base branch can be advanced independently. */
	struct FScenario
	{
		FGitTestRepository Repository;
		FString FeatureBranch;
		FString RelativeAsset;
		FString AbsoluteAsset;
		FString AttributesFile;
		TArray<FString> ExtraRemoteBranches;

		explicit FScenario(FAutomationTestBase& Test)
			: Repository(Test)
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			FeatureBranch = FString::Printf(TEXT("feature-%s"), *Suffix);
			// Outside Content/ so the asset registry never tries to load this fake package.
			RelativeAsset = FString::Printf(TEXT("Fixtures/CheckRemote-%s.uasset"), *Suffix);
			AbsoluteAsset = FPaths::ConvertRelativePathToFull(Repository.RepositoryRoot, RelativeAsset);
			AttributesFile = FPaths::Combine(Repository.RepositoryRoot, TEXT(".gitattributes"));
		}

		~FScenario()
		{
			Repository.RunGitQuietly(TEXT("remote set-head origin --delete"));
			for (const FString& Branch : ExtraRemoteBranches)
			{
				Repository.RunGitQuietly(FString::Printf(TEXT("push --quiet origin --delete %s"), *Branch));
			}
			Repository.RunGitQuietly(TEXT("checkout --quiet --force main"));
			Repository.RunGitQuietly(FString::Printf(TEXT("branch --quiet -D %s"), *FeatureBranch));
			Repository.RunGitQuietly(TEXT("fetch --quiet --prune origin"));
			Repository.RunGitQuietly(TEXT("merge --quiet --ff-only origin/main"));
			IFileManager::Get().Delete(*AttributesFile);
		}

		bool Prepare(const FString& FeatureContent)
		{
			TArray<FString> ErrorMessages;
			return Repository.IsAvailable()
				&& FFileHelper::SaveStringToFile(FString(TEXT("*.uasset lockable\n")), *AttributesFile)
				&& GitSourceControlUtils::CheckLFSLockable(Repository.GitBinary, Repository.RepositoryRoot, { TEXT("*.uasset") }, ErrorMessages)
				&& Repository.RunGit(FString::Printf(TEXT("checkout --quiet -b %s"), *FeatureBranch))
				&& FFileHelper::SaveStringToFile(FeatureContent, *AbsoluteAsset)
				&& Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeAsset))
				&& Repository.RunGit(TEXT("commit --quiet -m \"Edit asset on feature branch\""));
		}

		FGitSourceControlState CheckAsset() const
		{
			TMap<FString, FGitSourceControlState> States;
			States.Emplace(AbsoluteAsset, FGitSourceControlState(AbsoluteAsset));
			TArray<FString> ErrorMessages;
			GitSourceControlUtils::CheckRemote(Repository.GitBinary, Repository.RepositoryRoot, { AbsoluteAsset }, ErrorMessages, States);
			return States.FindChecked(AbsoluteAsset);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepAssetCurrent_When_BaseReceivedSameContentThroughSquashMerge,
	"UEGitPlugin.Integration.Should_KeepAssetCurrent_When_BaseReceivedSameContentThroughSquashMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepAssetCurrent_When_BaseReceivedSameContentThroughSquashMerge::RunTest(const FString& Parameters)
{
	// Arrange
	GitCheckRemoteTests::FScenario Scenario(*this);
	FString FeatureTree;
	FString SquashCommit;
	if (!Scenario.Prepare(TEXT("Feature content\n"))
		|| !Scenario.Repository.RunGit(TEXT("rev-parse HEAD^{tree}"), FeatureTree)
		|| !Scenario.Repository.PublishTreeOnRemote(FeatureTree, TEXT("main"), SquashCommit)
		|| !Scenario.Repository.RunGit(TEXT("fetch --quiet origin main")))
	{
		return false;
	}

	// Act
	const FGitSourceControlState State = Scenario.CheckAsset();

	// Assert
	TestTrue(TEXT("The asset identical to the base branch is current"), State.IsCurrent());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_MarkAssetNotLatest_When_BaseHasDifferentContent,
	"UEGitPlugin.Integration.Should_MarkAssetNotLatest_When_BaseHasDifferentContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_MarkAssetNotLatest_When_BaseHasDifferentContent::RunTest(const FString& Parameters)
{
	// Arrange
	GitCheckRemoteTests::FScenario Scenario(*this);
	FString ColleagueTree;
	FString ColleagueCommit;
	if (!Scenario.Prepare(TEXT("Feature content\n"))
		|| !Scenario.Repository.MakeRemoteTreeWithFileContent(TEXT("main"), Scenario.RelativeAsset, TEXT("Colleague content\n"), ColleagueTree)
		|| !Scenario.Repository.PublishTreeOnRemote(ColleagueTree, TEXT("main"), ColleagueCommit)
		|| !Scenario.Repository.RunGit(TEXT("fetch --quiet origin main")))
	{
		return false;
	}

	// Act
	const FGitSourceControlState State = Scenario.CheckAsset();

	// Assert
	TestTrue(TEXT("The asset changed by a colleague on the base branch is not latest"), State.State.RemoteState == ERemoteState::NotLatest);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_UseOriginHeadAsBase_When_RemoteDefinesIt,
	"UEGitPlugin.Integration.Should_UseOriginHeadAsBase_When_RemoteDefinesIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_UseOriginHeadAsBase_When_RemoteDefinesIt::RunTest(const FString& Parameters)
{
	// Arrange: the remote default branch is not one of the conventional names, and only it holds the colleague change.
	GitCheckRemoteTests::FScenario Scenario(*this);
	const FString BaseBranch = FString::Printf(TEXT("trunk-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Scenario.ExtraRemoteBranches.Add(BaseBranch);
	FString ColleagueTree;
	FString ColleagueCommit;
	if (!Scenario.Prepare(TEXT("Feature content\n"))
		|| !Scenario.Repository.RunGit(FString::Printf(TEXT("push --quiet origin origin/main:refs/heads/%s"), *BaseBranch))
		|| !Scenario.Repository.RunGit(FString::Printf(TEXT("fetch --quiet origin %s"), *BaseBranch))
		|| !Scenario.Repository.MakeRemoteTreeWithFileContent(BaseBranch, Scenario.RelativeAsset, TEXT("Colleague content\n"), ColleagueTree)
		|| !Scenario.Repository.PublishTreeOnRemote(ColleagueTree, BaseBranch, ColleagueCommit)
		|| !Scenario.Repository.RunGit(FString::Printf(TEXT("fetch --quiet origin %s"), *BaseBranch))
		|| !Scenario.Repository.RunGit(FString::Printf(TEXT("remote set-head origin %s"), *BaseBranch)))
	{
		return false;
	}

	// Act
	const FGitSourceControlState State = Scenario.CheckAsset();

	// Assert
	TestTrue(TEXT("The asset changed on the remote default branch is not latest"), State.State.RemoteState == ERemoteState::NotLatest);
	return !HasAnyErrors();
}

#endif
