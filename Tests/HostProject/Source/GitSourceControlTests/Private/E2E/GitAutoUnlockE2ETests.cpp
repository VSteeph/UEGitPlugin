#include "GitE2ESession.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace GitAutoUnlockE2ETests
{
	using EMergeMethod = FGitE2ESession::EMergeMethod;

	/** Right-click > Check Out, edit and save in the editor, then commit and push from Fork. */
	void LockEditAndPush(const TSharedRef<FGitE2ESession>& Session)
	{
		GitE2E::CheckOutFromContentBrowser(Session, 0);
		GitE2E::Do(Session, [Session]() { return Session->EditAndSave(Session->Assets[0], FGitE2ESession::EditedValue); });
		GitE2E::Do(Session, [Session]() { return Session->CommitAndPush(Session->Assets[0]); });
	}

	/** The pull request evolves: another edit saved in the editor, committed and pushed to the same branch. */
	void EditAndPushAgain(const TSharedRef<FGitE2ESession>& Session)
	{
		GitE2E::Do(Session, [Session]() { return Session->EditAndSave(Session->Assets[0], 6.0f); });
		GitE2E::Do(Session, [Session]() { return Session->CommitAndPush(Session->Assets[0]); });
	}

	bool IsLockedBy(const TSharedRef<FGitE2ESession>& Session, const TCHAR* Owner)
	{
		return Session->ServerLockOwner(Session->Assets[0]) == Owner;
	}

	bool IsFreeOnServer(const TSharedRef<FGitE2ESession>& Session)
	{
		return Session->ServerLockOwner(Session->Assets[0]).IsEmpty();
	}

	void SelectUnlockMode(const TSharedRef<FGitE2ESession>& Session, EGitUnlockMode Mode)
	{
		GitE2E::Do(Session, [Session, Mode]() { return Session->SelectUnlockMode(Mode); });
	}

	void MergeOnServer(const TSharedRef<FGitE2ESession>& Session, EMergeMethod Method)
	{
		GitE2E::Do(Session, [Session, Method]() { return Session->MergePullRequestOnServer(Method); });
	}

	void WaitUntilFreeOnServer(const TSharedRef<FGitE2ESession>& Session)
	{
		GitE2E::WaitUntil(Session, TEXT("the server releases the lock"), [Session]() { return IsFreeOnServer(Session); });
	}

	void KeepsLockOf(const TSharedRef<FGitE2ESession>& Session, const TCHAR* Owner)
	{
		GitE2E::KeepsHolding(Session, FString::Printf(TEXT("the server keeps the lock of %s"), Owner), [Session, Owner]() { return IsLockedBy(Session, Owner); });
	}

	void WaitUntilFetched(const TSharedRef<FGitE2ESession>& Session, const FString& Branch)
	{
		GitE2E::WaitUntil(Session, FString::Printf(TEXT("origin/%s shows the server's latest commit"), *Branch), [Session, Branch]() { return Session->HasFetched(Branch); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReleaseLockAndClearIcon_When_MergedWorkIsFetchedInEditor,
	"UEGitPlugin.E2E.Should_ReleaseLockAndClearIcon_When_MergedWorkIsFetchedInEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReleaseLockAndClearIcon_When_MergedWorkIsFetchedInEditor::RunTest(const FString& Parameters)
{
	// Arrange: default OnMerge mode, the designer's pull request is merged with a merge commit.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::LockEditAndPush(Session);
	GitAutoUnlockE2ETests::MergeOnServer(Session, GitAutoUnlockE2ETests::EMergeMethod::Merge);

	// Act
	GitE2E::Refresh(Session);

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFreeOnServer(Session);
	GitE2E::WaitUntil(Session, TEXT("the asset icon disappears"), [Session]()
	{
		const FSourceControlStatePtr State = Session->GetState(Session->Assets[0]);
		return State.IsValid() && GitE2E::IconOf(State).IsNone();
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReleaseLock_When_GitClientFetchesSquashMerge,
	"UEGitPlugin.E2E.Should_ReleaseLock_When_GitClientFetchesSquashMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReleaseLock_When_GitClientFetchesSquashMerge::RunTest(const FString& Parameters)
{
	// Arrange: default OnMerge mode, the pull request is squash-merged.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::LockEditAndPush(Session);
	GitAutoUnlockE2ETests::MergeOnServer(Session, GitAutoUnlockE2ETests::EMergeMethod::Squash);

	// Act: the designer fetches from Fork, the editor plays no part.
	GitE2E::Do(Session, [Session]() { return Session->FetchFromGitClient(); });

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFreeOnServer(Session);
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReleaseLock_When_GitClientFetchesRebaseMerge,
	"UEGitPlugin.E2E.Should_ReleaseLock_When_GitClientFetchesRebaseMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReleaseLock_When_GitClientFetchesRebaseMerge::RunTest(const FString& Parameters)
{
	// Arrange: default OnMerge mode, a two-commit pull request is rebase-merged.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::LockEditAndPush(Session);
	GitAutoUnlockE2ETests::EditAndPushAgain(Session);
	GitAutoUnlockE2ETests::MergeOnServer(Session, GitAutoUnlockE2ETests::EMergeMethod::Rebase);

	// Act
	GitE2E::Do(Session, [Session]() { return Session->FetchFromGitClient(); });

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFreeOnServer(Session);
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepLock_When_WorkIsPushedTwiceButNotMerged,
	"UEGitPlugin.E2E.Should_KeepLock_When_WorkIsPushedTwiceButNotMerged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepLock_When_WorkIsPushedTwiceButNotMerged::RunTest(const FString& Parameters)
{
	// Arrange: default OnMerge mode.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);

	// Act: the designer pushes the branch, pushes a second edit, then refreshes; the pull request stays open.
	GitAutoUnlockE2ETests::LockEditAndPush(Session);
	GitAutoUnlockE2ETests::EditAndPushAgain(Session);
	GitE2E::Refresh(Session);

	// Assert
	GitAutoUnlockE2ETests::KeepsLockOf(Session, TEXT("Designer"));
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepLock_When_AssetHasLocalEditAfterMerge,
	"UEGitPlugin.E2E.Should_KeepLock_When_AssetHasLocalEditAfterMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepLock_When_AssetHasLocalEditAfterMerge::RunTest(const FString& Parameters)
{
	// Arrange: the pull request is merged, but the designer kept working on the asset and saved again.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::LockEditAndPush(Session);
	GitAutoUnlockE2ETests::MergeOnServer(Session, GitAutoUnlockE2ETests::EMergeMethod::Merge);
	GitE2E::Do(Session, [Session]() { return Session->EditAndSave(Session->Assets[0], 9.0f); });

	// Act
	GitE2E::Refresh(Session);

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFetched(Session, TEXT("main"));
	GitAutoUnlockE2ETests::KeepsLockOf(Session, TEXT("Designer"));
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReleaseLock_When_DesignerPushesNewBranchInOnPushMode,
	"UEGitPlugin.E2E.Should_ReleaseLock_When_DesignerPushesNewBranchInOnPushMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReleaseLock_When_DesignerPushesNewBranchInOnPushMode::RunTest(const FString& Parameters)
{
	// Arrange: the designer picks OnPush in the plugin settings.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::SelectUnlockMode(Session, EGitUnlockMode::OnPush);

	// Act: lock in the editor, then commit and push a new branch from Fork; nothing is merged.
	GitAutoUnlockE2ETests::LockEditAndPush(Session);

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFreeOnServer(Session);
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReleaseLock_When_DesignerPushesExistingBranchInOnPushMode,
	"UEGitPlugin.E2E.Should_ReleaseLock_When_DesignerPushesExistingBranchInOnPushMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReleaseLock_When_DesignerPushesExistingBranchInOnPushMode::RunTest(const FString& Parameters)
{
	// Arrange: OnPush mode, and the designer's branch already exists on the server.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::SelectUnlockMode(Session, EGitUnlockMode::OnPush);
	GitE2E::Do(Session, [Session]() { return Session->StartFeatureBranch(); });

	// Act
	GitAutoUnlockE2ETests::LockEditAndPush(Session);

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFreeOnServer(Session);
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepLock_When_WorkIsPushedInNoneMode,
	"UEGitPlugin.E2E.Should_KeepLock_When_WorkIsPushedInNoneMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepLock_When_WorkIsPushedInNoneMode::RunTest(const FString& Parameters)
{
	// Arrange: the designer picks None in the plugin settings.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::SelectUnlockMode(Session, EGitUnlockMode::None);

	// Act
	GitAutoUnlockE2ETests::LockEditAndPush(Session);
	GitAutoUnlockE2ETests::EditAndPushAgain(Session);

	// Assert
	GitAutoUnlockE2ETests::KeepsLockOf(Session, TEXT("Designer"));
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepLock_When_WorkIsMergedInNoneMode,
	"UEGitPlugin.E2E.Should_KeepLock_When_WorkIsMergedInNoneMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepLock_When_WorkIsMergedInNoneMode::RunTest(const FString& Parameters)
{
	// Arrange: None mode, and the pull request is merged.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::SelectUnlockMode(Session, EGitUnlockMode::None);
	GitAutoUnlockE2ETests::LockEditAndPush(Session);
	GitAutoUnlockE2ETests::MergeOnServer(Session, GitAutoUnlockE2ETests::EMergeMethod::Merge);

	// Act
	GitE2E::Refresh(Session);

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFetched(Session, TEXT("main"));
	GitAutoUnlockE2ETests::KeepsLockOf(Session, TEXT("Designer"));
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepColleagueLock_When_ColleagueWorkIsMergedInOnMergeMode,
	"UEGitPlugin.E2E.Should_KeepColleagueLock_When_ColleagueWorkIsMergedInOnMergeMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepColleagueLock_When_ColleagueWorkIsMergedInOnMergeMode::RunTest(const FString& Parameters)
{
	// Arrange: default OnMerge mode; a colleague holds the lock and their work lands on main.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitE2E::Do(Session, [Session]() { return Session->ColleagueLocks(Session->Assets[0]); });
	GitE2E::Do(Session, [Session]() { return Session->PublishColleagueVersionOnServer(Session->Assets[0], 7.0f, TEXT("main")); });

	// Act
	GitE2E::Refresh(Session);

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFetched(Session, TEXT("main"));
	GitAutoUnlockE2ETests::KeepsLockOf(Session, TEXT("Colleague"));
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepColleagueLock_When_ColleagueBranchIsFetchedInOnPushMode,
	"UEGitPlugin.E2E.Should_KeepColleagueLock_When_ColleagueBranchIsFetchedInOnPushMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepColleagueLock_When_ColleagueBranchIsFetchedInOnPushMode::RunTest(const FString& Parameters)
{
	// Arrange: OnPush mode; a colleague holds the lock and pushed their branch.
	const FString ColleagueBranch = FString::Printf(TEXT("feature/colleague-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitAutoUnlockE2ETests::SelectUnlockMode(Session, EGitUnlockMode::OnPush);
	GitE2E::Do(Session, [Session]() { return Session->ColleagueLocks(Session->Assets[0]); });
	GitE2E::Do(Session, [Session, ColleagueBranch]() { return Session->PublishColleagueVersionOnServer(Session->Assets[0], 7.0f, ColleagueBranch); });

	// Act
	GitE2E::Do(Session, [Session]() { return Session->FetchFromGitClient(); });

	// Assert
	GitAutoUnlockE2ETests::WaitUntilFetched(Session, ColleagueBranch);
	GitAutoUnlockE2ETests::KeepsLockOf(Session, TEXT("Colleague"));
	GitE2E::Finish(Session);
	return true;
}

#endif
