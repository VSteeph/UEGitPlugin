#include "GitSourceControlUtils.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace GitLfsLocksParsingTests
{
	const FString RepositoryRoot = TEXT("C:/Repo/");

	FString AbsolutePath(const TCHAR* RelativePath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(RepositoryRoot, RelativePath);
		FPaths::NormalizeFilename(Path);
		return Path;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_SeparateOwnAndOtherLocks_When_ParsingVerifiedLocks,
	"UEGitPlugin.Unit.Should_SeparateOwnAndOtherLocks_When_ParsingVerifiedLocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_SeparateOwnAndOtherLocks_When_ParsingVerifiedLocks::RunTest(const FString& Parameters)
{
	// Arrange: the output of `git lfs locks --verify --json`.
	const FString Json = TEXT(R"({"ours":[{"id":"1","path":"Content/Mine.uasset","owner":{"name":"Designer"},"locked_at":"2026-11-13T10:30:00Z"}],)"
		R"("theirs":[{"id":"2","path":"Content/Theirs.uasset","owner":{"name":"Colleague"},"locked_at":"2026-11-12T08:00:00Z"}]})");
	TMap<FString, FGitLfsLockInfo> Locks;
	FString DetectedOwnUser;

	// Act
	const bool bParsed = GitSourceControlUtils::ParseLfsLocksVerifyJson(GitLfsLocksParsingTests::RepositoryRoot, Json, Locks, DetectedOwnUser);

	// Assert
	const FGitLfsLockInfo* Mine = Locks.Find(GitLfsLocksParsingTests::AbsolutePath(TEXT("Content/Mine.uasset")));
	const FGitLfsLockInfo* Theirs = Locks.Find(GitLfsLocksParsingTests::AbsolutePath(TEXT("Content/Theirs.uasset")));
	TestTrue(TEXT("The output is parsed"), bParsed);
	TestEqual(TEXT("The own LFS name is detected from our locks"), DetectedOwnUser, FString(TEXT("Designer")));
	TestTrue(TEXT("Our lock is ours whatever the configured name"), Mine && Mine->IsOwnedBy(TEXT("Someone Else")));
	TestTrue(TEXT("Their lock is not ours even with a matching name"), Theirs && !Theirs->IsOwnedBy(TEXT("Colleague")));
	TestEqual(TEXT("Their owner name is kept"), Theirs ? Theirs->LockUser : FString(), FString(TEXT("Colleague")));
	TestEqual(TEXT("The lock date is parsed as UTC"), Theirs ? Theirs->LockedAt : FDateTime(), FDateTime(2026, 11, 12, 8, 0, 0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReadEveryLock_When_LockCommandReturnsArray,
	"UEGitPlugin.Unit.Should_ReadEveryLock_When_LockCommandReturnsArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReadEveryLock_When_LockCommandReturnsArray::RunTest(const FString& Parameters)
{
	// Arrange: `git lfs lock --json` and `git lfs locks --json` both answer with an array.
	const FString Json = TEXT(R"([{"id":"1","path":"A.uasset","owner":{"name":"Designer"},"locked_at":"2026-11-13T10:30:00Z"},)"
		R"({"id":"2","path":"B.umap","owner":{"name":"Designer"},"locked_at":"2026-11-13T10:31:00Z"}])");
	TMap<FString, FGitLfsLockInfo> Locks;

	// Act
	const bool bParsed = GitSourceControlUtils::ParseLfsLocksListJson(GitLfsLocksParsingTests::RepositoryRoot, Json, Locks);

	// Assert
	TestTrue(TEXT("The output is parsed"), bParsed);
	TestEqual(TEXT("Every locked file is read"), Locks.Num(), 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReturnNoLocks_When_ServerHasNone,
	"UEGitPlugin.Unit.Should_ReturnNoLocks_When_ServerHasNone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReturnNoLocks_When_ServerHasNone::RunTest(const FString& Parameters)
{
	// Arrange
	TMap<FString, FGitLfsLockInfo> Locks;

	// Act
	const bool bEmptyParsed = GitSourceControlUtils::ParseLfsLocksListJson(GitLfsLocksParsingTests::RepositoryRoot, TEXT(""), Locks);
	const bool bNullParsed = GitSourceControlUtils::ParseLfsLocksListJson(GitLfsLocksParsingTests::RepositoryRoot, TEXT("null"), Locks);
	const bool bArrayParsed = GitSourceControlUtils::ParseLfsLocksListJson(GitLfsLocksParsingTests::RepositoryRoot, TEXT("[]"), Locks);

	// Assert
	TestTrue(TEXT("Every empty answer is a valid answer"), bEmptyParsed && bNullParsed && bArrayParsed);
	TestEqual(TEXT("No lock is reported"), Locks.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_FailParsing_When_OutputIsNotJson,
	"UEGitPlugin.Unit.Should_FailParsing_When_OutputIsNotJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_FailParsing_When_OutputIsNotJson::RunTest(const FString& Parameters)
{
	// Arrange: an error printed by an old git-lfs or a server without the locking API.
	const FString Output = TEXT("Remote \"origin\" does not support the Git LFS locking API");
	TMap<FString, FGitLfsLockInfo> Locks;
	FString DetectedOwnUser;

	// Act
	const bool bListParsed = GitSourceControlUtils::ParseLfsLocksListJson(GitLfsLocksParsingTests::RepositoryRoot, Output, Locks);
	const bool bVerifyParsed = GitSourceControlUtils::ParseLfsLocksVerifyJson(GitLfsLocksParsingTests::RepositoryRoot, Output, Locks, DetectedOwnUser);

	// Assert
	TestFalse(TEXT("The list parser rejects it"), bListParsed);
	TestFalse(TEXT("The verify parser rejects it"), bVerifyParsed);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_MatchOwnerIgnoringCase_When_OwnershipIsNotVerified,
	"UEGitPlugin.Unit.Should_MatchOwnerIgnoringCase_When_OwnershipIsNotVerified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_MatchOwnerIgnoringCase_When_OwnershipIsNotVerified::RunTest(const FString& Parameters)
{
	// Arrange: a lock read without --verify, from a server that changes the login case.
	FGitLfsLockInfo Lock;
	Lock.LockUser = TEXT("designer");

	// Act
	const bool bOwnedByDesigner = Lock.IsOwnedBy(TEXT("Designer"));
	const bool bOwnedByNobody = Lock.IsOwnedBy(FString());

	// Assert
	TestTrue(TEXT("The lock belongs to the configured user"), bOwnedByDesigner);
	TestFalse(TEXT("An empty configured name owns nothing"), bOwnedByNobody);
	return !HasAnyErrors();
}

#endif
