#pragma once

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlUtils.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * The isolated host repository prepared by Tests/framework.py, with `origin` pointing to a local bare remote and `main` as base branch.
 * `lfs.url` points to the in-memory lock server started by the framework, where requests without X-Lfs-User belong to "Designer".
 */
class FGitTestRepository
{
public:
	/** @param bConnect false leaves the connection to the caller, such as an E2E test selecting the provider like a user. */
	explicit FGitTestRepository(FAutomationTestBase& InTest, bool bUseLfsLocking = false, bool bConnect = true)
		: Test(InTest)
	{
		FGitSourceControlModule& Module = FGitSourceControlModule::Get();
		bWasUsingLocking = Module.AccessSettings().IsUsingGitLfsLocking();
		if (bConnect)
		{
			Connect(bUseLfsLocking);
		}
		else
		{
			Module.AccessSettings().SetUsingGitLfsLocking(bUseLfsLocking);
		}
		RepositoryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		GitBinary = Module.GetProvider().GetGitBinaryPath();
		if (GitBinary.IsEmpty())
		{
			// The provider resolves its binary when it connects: before that, the one on PATH is the one a user's Git client runs.
			GitBinary = TEXT("git");
		}
		// An interrupted earlier run can leave the checkout behind the remote, which would reject this scenario's pushes.
		RunGitQuietly(TEXT("fetch --quiet origin main"));
		RunGitQuietly(TEXT("merge --quiet --ff-only origin/main"));
	}

	~FGitTestRepository()
	{
		// Restores the setting only: reconnecting with it would require the lockable attributes the test removed.
		FGitSourceControlModule::Get().AccessSettings().SetUsingGitLfsLocking(bWasUsingLocking);
	}

	bool IsAvailable() const
	{
		return FGitSourceControlModule::Get().GetProvider().IsEnabled();
	}

	/** Runs git in the host repository, or in another directory such as the bare remote. Failures are reported as test errors. */
	bool RunGit(const FString& Arguments, FString& Output, const FString& Directory = FString()) const
	{
		int32 ExitCode = INDEX_NONE;
		FString ErrorOutput;
		const FString CommandLine = FString::Printf(TEXT("-C \"%s\" %s"), Directory.IsEmpty() ? *RepositoryRoot : *Directory, *Arguments);
		FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ExitCode, &Output, &ErrorOutput);
		Output.TrimStartAndEndInline();
		if (ExitCode != 0)
		{
			Test.AddError(FString::Printf(TEXT("git %s failed: %s"), *Arguments, *ErrorOutput));
		}
		return ExitCode == 0;
	}

	bool RunGit(const FString& Arguments) const
	{
		FString Output;
		return RunGit(Arguments, Output);
	}

	/** Runs git without reporting failures, for cleanup steps and for commands whose exit code is the observed result. */
	int32 RunGitQuietly(const FString& Arguments) const
	{
		int32 ExitCode = INDEX_NONE;
		FString Output;
		FString ErrorOutput;
		FPlatformProcess::ExecProcess(*GitBinary, *FString::Printf(TEXT("-C \"%s\" %s"), *RepositoryRoot, *Arguments), &ExitCode, &Output, &ErrorOutput);
		return ExitCode;
	}

	/** Runs git as another LFS user: the test lock server attributes requests to the X-Lfs-User header. */
	bool RunGitAs(const FString& LfsUser, const FString& Arguments) const
	{
		FString Output;
		return RunGit(FString::Printf(TEXT("-c http.extraHeader=\"X-Lfs-User: %s\" %s"), *LfsUser, *Arguments), Output);
	}

	/** Owner name of the lock the LFS server holds on a repository-relative file, or an empty string when it is free. */
	FString GetServerLockOwner(const FString& RelativeFile) const
	{
		FString Json;
		if (!RunGit(FString::Printf(TEXT("lfs locks --json --path=\"%s\""), *RelativeFile), Json))
		{
			return FString();
		}
		TMap<FString, FGitLfsLockInfo> Locks;
		GitSourceControlUtils::ParseLfsLocksListJson(RepositoryRoot, Json, Locks);
		for (const TPair<FString, FGitLfsLockInfo>& Lock : Locks)
		{
			return Lock.Value.LockUser;
		}
		return FString();
	}

	/** Releases any server lock on the file, whoever holds it, so scenarios never leak locks into each other. */
	void ForceUnlockOnServer(const FString& RelativeFile) const
	{
		RunGitQuietly(FString::Printf(TEXT("lfs unlock --force \"%s\""), *RelativeFile));
	}

	/** Builds, without touching the working tree, the tree of the given remote branch where one file holds the given content. */
	bool MakeRemoteTreeWithFileContent(const FString& Branch, const FString& RelativeFile, const FString& Content, FString& OutTree) const
	{
		const FString ContentFile = FPaths::ConvertRelativePathToFull(FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("GitTestContent")));
		const bool bResult = FFileHelper::SaveStringToFile(Content, *ContentFile)
			&& MakeRemoteTreeWithFile(Branch, RelativeFile, ContentFile, OutTree);
		IFileManager::Get().Delete(*ContentFile);
		return bResult;
	}

	/** Same as MakeRemoteTreeWithFileContent, with the bytes of an existing file such as a saved package. */
	bool MakeRemoteTreeWithFile(const FString& Branch, const FString& RelativeFile, const FString& SourceFile, FString& OutTree) const
	{
		FString Blob;
		const bool bResult = RunGit(FString::Printf(TEXT("hash-object -w \"%s\""), *SourceFile), Blob)
			&& RunGit(FString::Printf(TEXT("read-tree origin/%s"), *Branch))
			&& RunGit(FString::Printf(TEXT("update-index --add --cacheinfo 100644,%s,%s"), *Blob, *RelativeFile))
			&& RunGit(TEXT("write-tree"), OutTree);
		RunGitQuietly(TEXT("reset --quiet"));
		return bResult;
	}

	/** Publishes on the remote a commit with the given tree on top of the given remote branch, without touching the local checkout. */
	bool PublishTreeOnRemote(const FString& Tree, const FString& Branch, FString& OutCommit) const
	{
		return RunGit(FString::Printf(TEXT("commit-tree %s -p origin/%s -m \"Remote change\""), *Tree, *Branch), OutCommit)
			&& RunGit(FString::Printf(TEXT("push --quiet origin %s:refs/heads/%s"), *OutCommit, *Branch));
	}

	FString RepositoryRoot;
	FString GitBinary;

private:
	static void Connect(bool bUseLfsLocking)
	{
		FGitSourceControlModule& Module = FGitSourceControlModule::Get();
		Module.AccessSettings().SetUsingGitLfsLocking(bUseLfsLocking);
		Module.GetProvider().CheckGitAvailability();
	}

	FAutomationTestBase& Test;
	bool bWasUsingLocking = false;
};
