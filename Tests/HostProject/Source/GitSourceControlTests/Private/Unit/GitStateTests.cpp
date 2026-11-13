#include "GitSourceControlState.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace GitStateTests
{
	FGitSourceControlState MakeTrackedState()
	{
		FGitSourceControlState State(TEXT("C:/Repo/Content/Asset.uasset"));
		State.State.FileState = EFileState::Unknown;
		State.State.TreeState = ETreeState::Unmodified;
		return State;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_AllowRevertAndUnlock_When_LockedAssetIsOutdated,
	"UEGitPlugin.Unit.Should_AllowRevertAndUnlock_When_LockedAssetIsOutdated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_AllowRevertAndUnlock_When_LockedAssetIsOutdated::RunTest(const FString& Parameters)
{
	// Arrange: my lock, no local edit, and a newer version merged on another branch.
	FGitSourceControlState State = GitStateTests::MakeTrackedState();
	State.State.LockState = ELockState::Locked;
	State.State.RemoteState = ERemoteState::NotLatest;

	// Act
	const bool bCanRevert = State.CanRevert();

	// Assert
	TestTrue(TEXT("The lock can be released to get the newer version"), bCanRevert);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowOwnerAndBranch_When_ColleagueLockedOutdatedAsset,
	"UEGitPlugin.Unit.Should_ShowOwnerAndBranch_When_ColleagueLockedOutdatedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowOwnerAndBranch_When_ColleagueLockedOutdatedAsset::RunTest(const FString& Parameters)
{
	// Arrange
	FGitSourceControlState State = GitStateTests::MakeTrackedState();
	State.State.LockState = ELockState::LockedOther;
	State.State.LockUser = TEXT("Colleague");
	State.State.RemoteState = ERemoteState::NotLatest;
	State.State.HeadBranch = TEXT("origin/develop");

	// Act
	const FString Tooltip = State.GetDisplayTooltip().ToString();

	// Assert
	TestTrue(FString::Printf(TEXT("The hover names the owner: '%s'"), *Tooltip), Tooltip.Contains(TEXT("Colleague")));
	TestTrue(FString::Printf(TEXT("The hover names the branch with the newer version: '%s'"), *Tooltip), Tooltip.Contains(TEXT("origin/develop")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowOnlyBranch_When_AssetIsModifiedInOtherBranch,
	"UEGitPlugin.Unit.Should_ShowOnlyBranch_When_AssetIsModifiedInOtherBranch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowOnlyBranch_When_AssetIsModifiedInOtherBranch::RunTest(const FString& Parameters)
{
	// Arrange: the plugin knows the branch, never the commit or the action.
	FGitSourceControlState State = GitStateTests::MakeTrackedState();
	State.State.RemoteState = ERemoteState::NotLatest;
	State.State.HeadBranch = TEXT("origin/develop");

	// Act
	const FString Tooltip = State.GetDisplayTooltip().ToString();

	// Assert
	TestTrue(FString::Printf(TEXT("The hover names the branch: '%s'"), *Tooltip), Tooltip.Contains(TEXT("origin/develop")));
	TestFalse(FString::Printf(TEXT("The hover shows no placeholder commit: '%s'"), *Tooltip), Tooltip.Contains(TEXT("Unknown")) || Tooltip.Contains(TEXT("CL:")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowRemoteBranch_When_AssetIsNotAtHead,
	"UEGitPlugin.Unit.Should_ShowRemoteBranch_When_AssetIsNotAtHead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowRemoteBranch_When_AssetIsNotAtHead::RunTest(const FString& Parameters)
{
	// Arrange: my lock, and a newer version pushed on my own branch from another machine.
	FGitSourceControlState State = GitStateTests::MakeTrackedState();
	State.State.LockState = ELockState::Locked;
	State.State.LockUser = TEXT("Designer");
	State.State.RemoteState = ERemoteState::NotAtHead;
	State.State.HeadBranch = TEXT("origin/feature/door");

	// Act
	const FString Tooltip = State.GetDisplayTooltip().ToString();

	// Assert
	TestTrue(FString::Printf(TEXT("The hover names my remote branch: '%s'"), *Tooltip), Tooltip.Contains(TEXT("origin/feature/door")));
	TestTrue(FString::Printf(TEXT("The hover still names the lock owner: '%s'"), *Tooltip), Tooltip.Contains(TEXT("Designer")));
	return !HasAnyErrors();
}

#endif
