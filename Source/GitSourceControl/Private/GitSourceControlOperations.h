// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "IGitSourceControlWorker.h"
#include "GitSourceControlState.h"

#include "ISourceControlOperation.h"

/**
 * Internal operation used to fetch from remote
 */
class FGitFetch : public ISourceControlOperation
{
public:
	// ISourceControlOperation interface
	virtual FName GetName() const override;

	virtual FText GetInProgressString() const override;

	bool bUpdateStatus = false;
};

/** Called when first activated on a project, and then at project load time.
 *  Look for the root directory of the git repository (where the ".git/" subdirectory is located). */
class FGitConnectWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitConnectWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Lock (check-out) a set of files using Git LFS 2. */
class FGitCheckOutWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitCheckOutWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;

private:
	/** After a failed lock, tells apart another user winning the lock from a server or network failure. */
	void ReportLocksTakenByOthers(class FGitSourceControlCommand& InCommand, const TArray<FString>& InLockableFiles, const FString& InLockUser);
};

/**
 * Answers the editor's add and move requests by refreshing the files' status only.
 * The editor sends them on every new asset or rename; staging and committing belong to the team's Git client.
 */
class FGitStatusOnlyWorker : public IGitSourceControlWorker
{
public:
	explicit FGitStatusOnlyWorker(FName InOperationName);
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;

private:
	FName OperationName;
};

/**
 * Deletes files from disk without staging the deletion.
 * The editor leaves the deletion of a committed asset to the provider; the team's Git client then shows it as deleted.
 */
class FGitDeleteWorker final : public FGitStatusOnlyWorker
{
public:
	FGitDeleteWorker();
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

/** Revert local changes to a file without releasing Git LFS locks. */
class FGitRevertWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitRevertWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;

protected:
	/** Whether to release Git LFS locks after reverting changes. */
	virtual bool ShouldUnlock() const { return false; }
};

/** Revert any changes to a file and release its Git LFS lock. */
class FGitRevertAndUnlockWorker final : public FGitRevertWorker
{
public:
	virtual FName GetName() const override;

protected:
	virtual bool ShouldUnlock() const override { return true; }
};

/** Get revision control status of files on local working copy. */
class FGitUpdateStatusWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitUpdateStatusWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	/** Temporary states for results */
	TMap<const FString, FGitState> States;

	/** Map of filenames to history */
	TMap<FString, TGitSourceControlHistory> Histories;
};

/** Fetch targeted remote branches */
class FGitFetchWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitFetchWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};
