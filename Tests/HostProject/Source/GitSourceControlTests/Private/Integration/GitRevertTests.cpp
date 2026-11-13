#include "GitLockScenario.h"

#include "GitSourceControlRevertAndUnlock.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepRepositoryUntouched_When_RevertHasNoFiles,
	"UEGitPlugin.Integration.Should_KeepRepositoryUntouched_When_RevertHasNoFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepRepositoryUntouched_When_RevertHasNoFiles::RunTest(const FString& Parameters)
{
	// Arrange: a tracked non-asset file with a local edit and an untracked file, the two victims of a repository-wide reset.
	FGitLockScenario Scenario(*this);
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString RelativeTracked = FString::Printf(TEXT("Fixtures/Tracked-%s.txt"), *Suffix);
	const FString AbsoluteTracked = FPaths::ConvertRelativePathToFull(Scenario.Repository.RepositoryRoot, RelativeTracked);
	const FString AbsoluteUntracked = FPaths::ConvertRelativePathToFull(Scenario.Repository.RepositoryRoot, FString::Printf(TEXT("Fixtures/Untracked-%s.txt"), *Suffix));
	if (!Scenario.Prepare()
		|| !FFileHelper::SaveStringToFile(FString(TEXT("Committed\n")), *AbsoluteTracked)
		|| !Scenario.Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeTracked))
		|| !Scenario.Repository.RunGit(TEXT("commit --quiet -m \"Add tracked file\""))
		|| !FFileHelper::SaveStringToFile(FString(TEXT("Local edit\n")), *AbsoluteTracked)
		|| !FFileHelper::SaveStringToFile(FString(TEXT("Untracked\n")), *AbsoluteUntracked))
	{
		return false;
	}
	AddExpectedError(TEXT("Revert requires at least one file"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Command 'Revert' Failed"), EAutomationExpectedErrorFlags::Contains, 0);

	// Act
	const ECommandResult::Type Result = FGitSourceControlModule::Get().GetProvider().Execute(
		ISourceControlOperation::Create<FRevert>(), TArray<FString>(), EConcurrency::Synchronous);

	// Assert
	FString TrackedContent;
	FFileHelper::LoadFileToString(TrackedContent, *AbsoluteTracked);
	TestEqual(TEXT("The empty revert is refused"), Result, ECommandResult::Failed);
	TestEqual(TEXT("The local edit of a tracked file survives"), TrackedContent, FString(TEXT("Local edit\n")));
	TestTrue(TEXT("The untracked file survives"), FPaths::FileExists(AbsoluteUntracked));

	Scenario.Repository.RunGitQuietly(FString::Printf(TEXT("checkout -- \"%s\""), *RelativeTracked));
	IFileManager::Get().Delete(*AbsoluteUntracked);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepLock_When_RevertingChangesOfLockedAsset,
	"UEGitPlugin.Integration.Should_KeepLock_When_RevertingChangesOfLockedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepLock_When_RevertingChangesOfLockedAsset::RunTest(const FString& Parameters)
{
	// Arrange
	FGitLockScenario Scenario(*this);
	if (!Scenario.Prepare() || !TestTrue(TEXT("The asset is locked and edited"), Scenario.LockAndModifyAsset()))
	{
		return false;
	}

	// Act
	const TArray<FString> Errors = Scenario.Execute(ISourceControlOperation::Create<FRevert>());

	// Assert
	TestTrue(TEXT("Revert Changes succeeds"), Errors.IsEmpty());
	TestEqual(TEXT("The asset content is reverted"), Scenario.ReadAsset(), FString(FGitLockScenario::BaseContent));
	TestEqual(TEXT("The server still holds the designer's lock"), Scenario.Repository.GetServerLockOwner(Scenario.RelativeAsset), FString(TEXT("Designer")));
	const FSourceControlStatePtr State = Scenario.GetCachedState();
	TestTrue(TEXT("The editor still shows the asset as checked out"), State.IsValid() && State->IsCheckedOut());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_RestoreAsset_When_RevertingDeletedCommittedAsset,
	"UEGitPlugin.Integration.Should_RestoreAsset_When_RevertingDeletedCommittedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_RestoreAsset_When_RevertingDeletedCommittedAsset::RunTest(const FString& Parameters)
{
	// Arrange: a committed asset deleted from disk, the deletion not staged.
	FGitLockScenario Scenario(*this);
	if (!Scenario.Prepare() || !TestTrue(TEXT("The asset is deleted"), IFileManager::Get().Delete(*Scenario.AbsoluteAsset, false, true)))
	{
		return false;
	}

	// Act
	const TArray<FString> Errors = Scenario.Execute(ISourceControlOperation::Create<FRevert>());

	// Assert
	TestTrue(FString::Printf(TEXT("Revert succeeds: %s"), *FString::Join(Errors, TEXT(" | "))), Errors.IsEmpty());
	TestEqual(TEXT("The committed asset is back on disk"), Scenario.ReadAsset(), FString(FGitLockScenario::BaseContent));
	TestEqual(TEXT("No deletion is staged"), Scenario.Repository.RunGitQuietly(TEXT("diff --cached --quiet")), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReleaseLock_When_RevertingAndUnlockingLockedAsset,
	"UEGitPlugin.Integration.Should_ReleaseLock_When_RevertingAndUnlockingLockedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReleaseLock_When_RevertingAndUnlockingLockedAsset::RunTest(const FString& Parameters)
{
	// Arrange
	FGitLockScenario Scenario(*this);
	if (!Scenario.Prepare() || !TestTrue(TEXT("The asset is locked and edited"), Scenario.LockAndModifyAsset()))
	{
		return false;
	}

	// Act
	const TArray<FString> Errors = Scenario.Execute(ISourceControlOperation::Create<FGitRevertAndUnlock>());

	// Assert
	TestTrue(TEXT("Revert and Unlock succeeds"), Errors.IsEmpty());
	TestEqual(TEXT("The asset content is reverted"), Scenario.ReadAsset(), FString(FGitLockScenario::BaseContent));
	TestTrue(TEXT("The server no longer holds a lock"), Scenario.Repository.GetServerLockOwner(Scenario.RelativeAsset).IsEmpty());
	TestTrue(TEXT("The asset is read-only again"), FPlatformFileManager::Get().GetPlatformFile().IsReadOnly(*Scenario.AbsoluteAsset));
	const FSourceControlStatePtr State = Scenario.GetCachedState();
	TestTrue(TEXT("The editor offers to check the asset out again"), State.IsValid() && State->CanCheckout());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReleaseLock_When_RevertingAndUnlockingUnmodifiedLockedAsset,
	"UEGitPlugin.Integration.Should_ReleaseLock_When_RevertingAndUnlockingUnmodifiedLockedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReleaseLock_When_RevertingAndUnlockingUnmodifiedLockedAsset::RunTest(const FString& Parameters)
{
	// Arrange: a lock taken by mistake, without any edit.
	FGitLockScenario Scenario(*this);
	if (!Scenario.Prepare() || !TestTrue(TEXT("The asset is locked"), Scenario.CheckOutAsset().IsEmpty()))
	{
		return false;
	}

	// Act
	const TArray<FString> Errors = Scenario.Execute(ISourceControlOperation::Create<FGitRevertAndUnlock>());

	// Assert
	TestTrue(TEXT("Revert and Unlock succeeds"), Errors.IsEmpty());
	TestTrue(TEXT("The server no longer holds a lock"), Scenario.Repository.GetServerLockOwner(Scenario.RelativeAsset).IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_Succeed_When_RevertingAndUnlockingAssetWhoseLockWasReleasedElsewhere,
	"UEGitPlugin.Integration.Should_Succeed_When_RevertingAndUnlockingAssetWhoseLockWasReleasedElsewhere",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_Succeed_When_RevertingAndUnlockingAssetWhoseLockWasReleasedElsewhere::RunTest(const FString& Parameters)
{
	// Arrange: the editor still believes it holds the lock, but it was released from another machine.
	FGitLockScenario Scenario(*this);
	if (!Scenario.Prepare() || !TestTrue(TEXT("The asset is locked"), Scenario.CheckOutAsset().IsEmpty()))
	{
		return false;
	}
	Scenario.Repository.ForceUnlockOnServer(Scenario.RelativeAsset);

	// Act
	const TArray<FString> Errors = Scenario.Execute(ISourceControlOperation::Create<FGitRevertAndUnlock>());

	// Assert
	TestTrue(FString::Printf(TEXT("Revert and Unlock succeeds: %s"), *FString::Join(Errors, TEXT(" | "))), Errors.IsEmpty());
	const FSourceControlStatePtr State = Scenario.GetCachedState();
	TestTrue(TEXT("The editor offers to check the asset out again"), State.IsValid() && State->CanCheckout());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReleaseEveryLock_When_RevertingAndUnlockingSeveralAssets,
	"UEGitPlugin.Integration.Should_ReleaseEveryLock_When_RevertingAndUnlockingSeveralAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReleaseEveryLock_When_RevertingAndUnlockingSeveralAssets::RunTest(const FString& Parameters)
{
	// Arrange: two assets locked by one check-out and both edited, as Revert and Unlock All Assets sends them.
	FGitLockScenario Scenario(*this);
	const FString RelativeSecond = FString::Printf(TEXT("Fixtures/Lock-%s.uasset"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AbsoluteSecond = FPaths::ConvertRelativePathToFull(Scenario.Repository.RepositoryRoot, RelativeSecond);
	FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	const bool bArranged = Scenario.Prepare()
		&& FFileHelper::SaveStringToFile(FString(FGitLockScenario::BaseContent), *AbsoluteSecond)
		&& Scenario.Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeSecond))
		&& Scenario.Repository.RunGit(TEXT("commit --quiet -m \"Add second lockable asset\""))
		&& Scenario.Repository.RunGit(TEXT("push --quiet origin main"))
		&& Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), { Scenario.AbsoluteAsset, AbsoluteSecond }, EConcurrency::Synchronous) == ECommandResult::Succeeded
		&& FFileHelper::SaveStringToFile(FString(TEXT("Local edit\n")), *Scenario.AbsoluteAsset)
		&& FFileHelper::SaveStringToFile(FString(TEXT("Local edit\n")), *AbsoluteSecond);
	if (!TestTrue(TEXT("Both assets are locked and edited"), bArranged))
	{
		Scenario.Repository.ForceUnlockOnServer(RelativeSecond);
		return false;
	}

	// Act
	const ECommandResult::Type Result = Provider.Execute(ISourceControlOperation::Create<FGitRevertAndUnlock>(), { Scenario.AbsoluteAsset, AbsoluteSecond }, EConcurrency::Synchronous);

	// Assert
	TestEqual(TEXT("Revert and Unlock succeeds"), Result, ECommandResult::Succeeded);
	TestTrue(TEXT("The first lock is released"), Scenario.Repository.GetServerLockOwner(Scenario.RelativeAsset).IsEmpty());
	TestTrue(TEXT("The second lock is released"), Scenario.Repository.GetServerLockOwner(RelativeSecond).IsEmpty());

	Scenario.Repository.ForceUnlockOnServer(RelativeSecond);
	Scenario.Repository.RunGitQuietly(FString::Printf(TEXT("checkout -- \"%s\""), *RelativeSecond));
	return !HasAnyErrors();
}

#endif
