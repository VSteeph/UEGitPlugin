#include "GitSourceControlUtils.h"

#include "Async/ParallelFor.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_KeepEveryLock_When_ThreadsUpdateLockCacheConcurrently,
	"UEGitPlugin.Unit.Should_KeepEveryLock_When_ThreadsUpdateLockCacheConcurrently",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_KeepEveryLock_When_ThreadsUpdateLockCacheConcurrently::RunTest(const FString& Parameters)
{
	// Arrange: as a refresh, a check-out and a revert running together, each thread locks and unlocks its own files.
	// The paths do not exist, so no read-only flag is touched.
	const FString Prefix = FString::Printf(TEXT("C:/NoSuchRepository-%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	constexpr int32 ThreadCount = 8;
	constexpr int32 LocksPerThread = 2000;
	FThreadSafeCounter LostLocks;

	// Act
	ParallelFor(ThreadCount, [&Prefix, &LostLocks](int32 ThreadIndex)
	{
		for (int32 LockIndex = 0; LockIndex < LocksPerThread; ++LockIndex)
		{
			const FString File = FString::Printf(TEXT("%sThread%d/Asset%d.uasset"), *Prefix, ThreadIndex, LockIndex);
			FGitLfsLockInfo Lock;
			Lock.LockUser = TEXT("Designer");
			FGitLockedFilesCache::AddLockedFile(File, Lock);
			const TMap<FString, FGitLfsLockInfo> Snapshot = FGitLockedFilesCache::GetLockedFiles();
			if (!Snapshot.Contains(File))
			{
				LostLocks.Increment();
			}
			FGitLockedFilesCache::RemoveLockedFile(File);
		}
	});

	// Assert
	const TMap<FString, FGitLfsLockInfo> Remaining = FGitLockedFilesCache::GetLockedFiles();
	int32 LeakedLocks = 0;
	for (const TPair<FString, FGitLfsLockInfo>& Lock : Remaining)
	{
		LeakedLocks += Lock.Key.StartsWith(Prefix) ? 1 : 0;
	}
	TestEqual(TEXT("Every lock a thread took is visible to it"), LostLocks.GetValue(), 0);
	TestEqual(TEXT("Every released lock leaves the cache"), LeakedLocks, 0);
	return !HasAnyErrors();
}

#endif
