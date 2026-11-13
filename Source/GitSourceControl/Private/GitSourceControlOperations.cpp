// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlOperations.h"

#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SourceControlOperations.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlCommand.h"
#include "GitSourceControlUtils.h"
#include "SourceControlHelpers.h"
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"

#include <thread>

#define LOCTEXT_NAMESPACE "GitSourceControl"

FName FGitConnectWorker::GetName() const
{
	return "Connect";
}

bool FGitConnectWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// The connect worker checks if we are connected to the remote server.
	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FConnect, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FConnect>(InCommand.Operation);

	// Skip login operations, since Git does not have to login.
	// It's not a big deal for async commands though, so let those go through.
	// More information: this is a heuristic for cases where UE is trying to create
	// a valid Perforce connection as a side effect for the connect worker. For Git,
	// the connect worker has no side effects. It is simply a query to retrieve information
	// to be displayed to the user, like in the revision control settings or on init.
	// Therefore, there is no need for synchronously establishing a connection if not there.
	if (InCommand.Concurrency == EConcurrency::Synchronous)
	{
		InCommand.bCommandSuccessful = true;
		return true;
	}

	// Check Git availability
	// We already know that Git is available if PathToGitBinary is not empty, since it is validated then.
	if (InCommand.PathToGitBinary.IsEmpty())
	{
		const FText& NotFound = LOCTEXT("GitNotFound", "Failed to enable Git revision control. You need to install Git and ensure the plugin has a valid path to the git executable.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
		InCommand.bCommandSuccessful = false;
		return false;
	}

	// Get default branch: git remote show
	
	TArray<FString> Parameters {
		TEXT("-h"), // Only limit to branches
		TEXT("-q") // Skip printing out remote URL, we don't use it
	};
	
	// Check if remote matches our refs.
	// Could be useful in the future, but all we want to know right now is if connection is up.
	// Parameters.Add("--exit-code");
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("ls-remote"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (!InCommand.bCommandSuccessful)
	{
		const FText& NotFound = LOCTEXT("GitRemoteFailed", "Failed Git remote connection. Ensure your repo is initialized, and check your connection to the Git host.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
	}

	// TODO: always return true, and enter an offline mode if could not connect to remote
	return InCommand.bCommandSuccessful;
}

bool FGitConnectWorker::UpdateStates() const
{
	return false;
}

FName FGitCheckOutWorker::GetName() const
{
	return "CheckOut";
}

bool FGitCheckOutWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	if (!InCommand.bUsingGitLfsLocking)
	{
		InCommand.bCommandSuccessful = false;
		return InCommand.bCommandSuccessful;
	}

	// lock files: execute the LFS command on relative filenames
	const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);

	const TArray<FString>& LockableRelativeFiles = RelativeFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable);

	if (LockableRelativeFiles.Num() < 1)
	{
		InCommand.bCommandSuccessful = true;
		return InCommand.bCommandSuccessful;
	}

	TArray<FString> LockableAbsoluteFiles;
	TMap<FString, FGitSourceControlState> CheckoutStates;
	for (const FString& RelativeFile : LockableRelativeFiles)
	{
		const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(InCommand.PathToRepositoryRoot, RelativeFile);
		LockableAbsoluteFiles.Add(AbsoluteFile);
		CheckoutStates.Emplace(AbsoluteFile, FGitSourceControlState(AbsoluteFile));
	}

	// Refresh targeted remote refs and verify that target lockable files are up to date before locking
	TArray<FString> FetchInfoMessages;
	if (!GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, false, FetchInfoMessages, InCommand.ResultInfo.ErrorMessages))
	{
		InCommand.ResultInfo.ErrorMessages.Add(TEXT("Cannot check out: the remote base branch could not be fetched to verify that files are up to date."));
		InCommand.bCommandSuccessful = false;
		return false;
	}
	GitSourceControlUtils::CheckRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, LockableAbsoluteFiles, InCommand.ResultInfo.ErrorMessages, CheckoutStates);

	bool bAnyFileOutOfDate = false;
	for (const TPair<FString, FGitSourceControlState>& Pair : CheckoutStates)
	{
		if (Pair.Value.State.RemoteState == ERemoteState::NotAtHead || Pair.Value.State.RemoteState == ERemoteState::NotLatest)
		{
			bAnyFileOutOfDate = true;
			InCommand.ResultInfo.ErrorMessages.Add(FString::Printf(TEXT("Cannot check out '%s': file has newer changes on remote branch '%s'."), *Pair.Key, *Pair.Value.State.HeadBranch));
		}
	}

	if (bAnyFileOutOfDate)
	{
		// Only the remote check ran: the file, tree and lock states stay Unset so the cache keeps what it knew.
		for (const TPair<FString, FGitSourceControlState>& Pair : CheckoutStates)
		{
			FGitState RemoteOnly;
			RemoteOnly.FileState = EFileState::Unset;
			RemoteOnly.TreeState = ETreeState::Unset;
			RemoteOnly.LockState = ELockState::Unset;
			RemoteOnly.RemoteState = Pair.Value.State.RemoteState;
			RemoteOnly.HeadBranch = Pair.Value.State.HeadBranch;
			States.Add(Pair.Key, RemoteOnly);
		}
		InCommand.bCommandSuccessful = false;
		return false;
	}

	TArray<FString> LockResults;
	const bool bSuccess = GitSourceControlUtils::RunLFSCommand(TEXT("lock"), InCommand.PathToRepositoryRoot, InCommand.PathToGitBinary, { TEXT("--json") }, LockableRelativeFiles, LockResults, InCommand.ResultInfo.ErrorMessages);
	InCommand.ResultInfo.InfoMessages.Append(LockResults);
	InCommand.bCommandSuccessful = bSuccess;

	FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	const FString FallbackLockUser = Provider.GetLockUser();
	if (bSuccess)
	{
		TMap<FString, FGitLfsLockInfo> ParsedLocks;
		if (GitSourceControlUtils::ParseLfsLocksListJson(InCommand.PathToRepositoryRoot, FString::Join(LockResults, TEXT("\n")), ParsedLocks))
		{
			for (const TPair<FString, FGitLfsLockInfo>& Pair : ParsedLocks)
			{
				if (!Pair.Value.LockUser.IsEmpty())
				{
					Provider.QueueAutoDetectedLockUser(Pair.Value.LockUser);
					break;
				}
			}
		}

		TArray<FString> AbsoluteFiles;
		TMap<FString, FGitLfsLockInfo> ResolvedLockInfos;
		for (const auto& RelativeFile : LockableRelativeFiles)
		{
			FString AbsoluteFile = FPaths::ConvertRelativePathToFull(InCommand.PathToRepositoryRoot, RelativeFile);
			FPaths::NormalizeFilename(AbsoluteFile);

			FGitLfsLockInfo LockInfo;
			if (const FGitLfsLockInfo* ParsedInfo = ParsedLocks.Find(AbsoluteFile))
			{
				LockInfo = *ParsedInfo;
			}
			if (LockInfo.LockUser.IsEmpty())
			{
				LockInfo.LockUser = FallbackLockUser;
			}
			if (LockInfo.LockedAt.GetTicks() <= 0)
			{
				LockInfo.LockedAt = FDateTime::UtcNow();
			}
			LockInfo.bHasVerifiedOwnership = true;
			LockInfo.bIsOwnLock = true;

			FGitLockedFilesCache::AddLockedFile(AbsoluteFile, LockInfo);
			ResolvedLockInfos.Add(AbsoluteFile, LockInfo);
			AbsoluteFiles.Add(AbsoluteFile);
		}

		GitSourceControlUtils::CollectNewStates(AbsoluteFiles, States, EFileState::Unset, ETreeState::Unset, ELockState::Locked);
		for (auto& State : States)
		{
			if (const FGitLfsLockInfo* ResolvedInfo = ResolvedLockInfos.Find(State.Key))
			{
				State.Value.LockUser = ResolvedInfo->LockUser;
				State.Value.LockDate = ResolvedInfo->LockedAt;
			}
			else
			{
				State.Value.LockUser = FallbackLockUser;
			}
		}
	}
	else
	{
		ReportLocksTakenByOthers(InCommand, LockableAbsoluteFiles, FallbackLockUser);
	}

	return InCommand.bCommandSuccessful;
}

void FGitCheckOutWorker::ReportLocksTakenByOthers(FGitSourceControlCommand& InCommand, const TArray<FString>& InLockableFiles, const FString& InLockUser)
{
	TMap<FString, FGitLfsLockInfo> ServerLocks;
	TArray<FString> LockErrors;
	if (!GitSourceControlUtils::GetAllLocks(InCommand.PathToRepositoryRoot, InCommand.PathToGitBinary, LockErrors, ServerLocks, true))
	{
		return;
	}

	TArray<FText> Reasons;
	for (FString File : InLockableFiles)
	{
		FPaths::NormalizeFilename(File);
		const FGitLfsLockInfo* Lock = ServerLocks.Find(File);
		if (!Lock || Lock->IsOwnedBy(InLockUser))
		{
			continue;
		}
		const FText Reason = FText::Format(LOCTEXT("CheckOutLockedByOther", "Cannot check out '{0}': locked by {1} since {2}."),
			FText::FromString(FPaths::GetBaseFilename(File)), FText::FromString(Lock->LockUser), FText::AsDateTime(Lock->LockedAt, EDateTimeStyle::Short, EDateTimeStyle::Short));
		Reasons.Add(Reason);
		InCommand.ResultInfo.ErrorMessages.Add(Reason.ToString());

		FGitState LockedByOther;
		LockedByOther.FileState = EFileState::Unset;
		LockedByOther.TreeState = ETreeState::Unset;
		LockedByOther.LockState = ELockState::LockedOther;
		LockedByOther.LockUser = Lock->LockUser;
		LockedByOther.LockDate = Lock->LockedAt;
		LockedByOther.RemoteState = ERemoteState::Unset;
		States.Add(File, LockedByOther);
	}
	if (Reasons.Num() > 0)
	{
		InCommand.UserFacingError = FText::Join(FText::FromString(TEXT("\n")), Reasons);
	}
}

bool FGitCheckOutWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FGitStatusOnlyWorker::FGitStatusOnlyWorker(FName InOperationName)
	: OperationName(InOperationName)
{
}

FName FGitStatusOnlyWorker::GetName() const
{
	return OperationName;
}

bool FGitStatusOnlyWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());
	if (InCommand.Files.Num() == 0)
	{
		InCommand.bCommandSuccessful = true;
		return true;
	}

	TMap<FString, FGitSourceControlState> UpdatedStates;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	return InCommand.bCommandSuccessful;
}

bool FGitStatusOnlyWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FGitDeleteWorker::FGitDeleteWorker()
	: FGitStatusOnlyWorker(TEXT("Delete"))
{
}

bool FGitDeleteWorker::Execute(FGitSourceControlCommand& InCommand)
{
	TArray<FString> NotDeleted;
	for (const FString& File : InCommand.Files)
	{
		// Unlocked assets are read-only on disk: deleting them is what the designer asked for.
		if (!IFileManager::Get().Delete(*File, false, true))
		{
			NotDeleted.Add(File);
			InCommand.ResultInfo.ErrorMessages.Add(FString::Printf(TEXT("Could not delete '%s'."), *File));
		}
	}
	const bool bStatusUpdated = FGitStatusOnlyWorker::Execute(InCommand);
	InCommand.bCommandSuccessful = bStatusUpdated && NotDeleted.IsEmpty();
	return InCommand.bCommandSuccessful;
}

// Sorts the files to revert by the Git commands that revert them.
// Reads Git directly: this runs on a worker thread, where the provider's state cache must not be touched.
static bool ClassifyFilesToRevert(const FGitSourceControlCommand& InCommand, TArray<FString>& OutErrorMessages, TArray<FString>& OutAddedFilesToForget, TArray<FString>& OutFilesToUnstage, TArray<FString>& OutFilesToRestore, TArray<FString>& OutDeletedFilesToRestore)
{
	TMap<FString, FGitSourceControlState> CurrentStates;
	if (!GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, OutErrorMessages, CurrentStates))
	{
		return false;
	}
	for (const TPair<FString, FGitSourceControlState>& Pair : CurrentStates)
	{
		const FGitSourceControlState& State = Pair.Value;
		if (FPaths::FileExists(State.GetFilename()))
		{
			if (State.IsAdded())
			{
				OutFilesToUnstage.Add(State.GetFilename());
			}
			else if (State.IsModified())
			{
				OutFilesToRestore.Add(State.GetFilename());
				OutFilesToUnstage.Add(State.GetFilename());
			}
			else if (State.CanRevert()) // for locked but unmodified files
			{
				OutFilesToRestore.Add(State.GetFilename());
			}
		}
		else if (State.IsAdded())
		{
			// An added file deleted since: reverting means forgetting it.
			OutAddedFilesToForget.Add(State.GetFilename());
		}
		else if (State.IsSourceControlled())
		{
			// A committed file deleted from disk, its deletion staged or not: reverting brings it back.
			OutDeletedFilesToRestore.Add(State.GetFilename());
		}
	}
	return true;
}

FName FGitRevertWorker::GetName() const
{
	return "Revert";
}

FName FGitRevertAndUnlockWorker::GetName() const
{
	return "RevertAndUnlock";
}

bool FGitRevertWorker::Execute(FGitSourceControlCommand& InCommand)
{
	if (InCommand.Files.IsEmpty())
	{
		InCommand.ResultInfo.ErrorMessages.Add(TEXT("Revert requires at least one file."));
		InCommand.bCommandSuccessful = false;
		return false;
	}

	InCommand.bCommandSuccessful = true;

	// Filter files by status
	TArray<FString> AddedFilesToForget;
	TArray<FString> FilesToUnstage;
	TArray<FString> FilesToRestore;
	TArray<FString> DeletedFilesToRestore;
	if (!ClassifyFilesToRevert(InCommand, InCommand.ResultInfo.ErrorMessages, AddedFilesToForget, FilesToUnstage, FilesToRestore, DeletedFilesToRestore))
	{
		InCommand.ResultInfo.ErrorMessages.Add(TEXT("Revert stopped: Git could not report the status of the files to revert."));
		InCommand.bCommandSuccessful = false;
		return false;
	}

	if (AddedFilesToForget.Num() > 0)
	{
		// "Added" files that have been deleted needs to be removed from revision control
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), AddedFilesToForget, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	if (FilesToUnstage.Num() > 0)
	{
		// Unstage any staged changes (including Added and staged Modified files) before checkout.
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FilesToUnstage, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	if (DeletedFilesToRestore.Num() > 0)
	{
		// Restores both the index and the working tree from the last commit.
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, { TEXT("HEAD"), TEXT("--") }, DeletedFilesToRestore, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	if (FilesToRestore.Num() > 0)
	{
		// Revert working copy changes for tracked files only (skipping Added files unstaged by `reset` above).
		// May need to try a few times due to file locks from prior operations.
		bool CheckoutSuccess = false;
		int32 Attempts = 10;
		while (Attempts-- > 0)
		{
			CheckoutSuccess = GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FilesToRestore, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
			if (CheckoutSuccess)
			{
				break;
			}

			FPlatformProcess::Sleep(0.1f);
		}

		InCommand.bCommandSuccessful &= CheckoutSuccess;
	}

	if (ShouldUnlock() && InCommand.bUsingGitLfsLocking && InCommand.bCommandSuccessful)
	{
		// Ask the server rather than the lock cache: a lock released elsewhere (another machine, the web UI, an unlock hook)
		// still looks held locally, and unlocking it again fails the whole command.
		TMap<FString, FGitLfsLockInfo> ServerLocks;
		InCommand.bCommandSuccessful &= GitSourceControlUtils::GetAllLocks(InCommand.PathToRepositoryRoot, InCommand.PathToGitBinary, InCommand.ResultInfo.ErrorMessages, ServerLocks, true);
		const FString LockUser = FGitSourceControlModule::Get().GetProvider().GetLockUser();
		TArray<FString> LockedFiles;
		for (const FString& File : InCommand.Files)
		{
			FString NormalizedFile = File;
			FPaths::NormalizeFilename(NormalizedFile);
			const FGitLfsLockInfo* Lock = ServerLocks.Find(NormalizedFile);
			if (Lock && Lock->IsOwnedBy(LockUser))
			{
				LockedFiles.Add(NormalizedFile);
			}
		}
		if (InCommand.bCommandSuccessful && LockedFiles.Num() > 0)
		{
			const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(LockedFiles, InCommand.PathToRepositoryRoot);
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunLFSCommand(TEXT("unlock"), InCommand.PathToRepositoryRoot, InCommand.PathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), RelativeFiles,
																				 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
			if (InCommand.bCommandSuccessful)
			{
				for (const auto& File : LockedFiles)
				{
					FGitLockedFilesCache::RemoveLockedFile(File);
				}
			}
		}
	}

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));

	return InCommand.bCommandSuccessful;
}

bool FGitRevertWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitFetch::GetName() const
{
	return "Fetch";
}

FText FGitFetch::GetInProgressString() const
{
	// TODO Configure origin
	return LOCTEXT("SourceControl_Push", "Fetching from remote origin...");
}

FName FGitFetchWorker::GetName() const
{
	return "Fetch";
}

bool FGitFetchWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
																	  InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	if (!InCommand.bCommandSuccessful)
	{
		return false;
	}

	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FGitFetch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FGitFetch>(InCommand.Operation);

	if (Operation->bUpdateStatus)
	{
		// Now update the status of all our files
		const TArray<FString> ProjectDirs = GitSourceControlUtils::GetSourceControlledAssetPaths();

		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
																			  ProjectDirs, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitFetchWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitUpdateStatusWorker::GetName() const
{
	return "UpdateStatus";
}

bool FGitUpdateStatusWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);

	if(InCommand.Files.Num() > 0)
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
			if (Operation->ShouldUpdateHistory())
			{
				for (const auto& State : UpdatedStates)
				{
					const FString& File = State.Key;
					TGitSourceControlHistory History;

					if (State.Value.IsConflicted())
					{
						// In case of a merge conflict, we first need to get the tip of the "remote branch" (MERGE_HEAD)
						GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, true,
															 InCommand.ResultInfo.ErrorMessages, History);
					}
					// Get the history of the file in the current branch
					InCommand.bCommandSuccessful &= GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, false,
																						 InCommand.ResultInfo.ErrorMessages, History);
					Histories.Add(*File, History);
				}
			}
		}
	}
	else
	{
		// no path provided: only update the status of assets in Content/ directory and also Config files
		const TArray<FString> ProjectDirs = GitSourceControlUtils::GetSourceControlledAssetPaths();
		
		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, ProjectDirs, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	GitSourceControlUtils::GetBranchName(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.BranchName);
	GitSourceControlUtils::GetRemoteBranchName(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.RemoteBranchName);
	GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	// don't use the ShouldUpdateModifiedState() hint here as it is specific to Perforce: the above normal Git status has already told us this information (like Git and Mercurial)

	return InCommand.bCommandSuccessful;
}

bool FGitUpdateStatusWorker::UpdateStates() const
{
	bool bUpdated = GitSourceControlUtils::UpdateCachedStates(States);

	FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>( "GitSourceControl" );
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const bool bUsingGitLfsLocking = Provider.UsesCheckout();

	// TODO without LFS : Workaround a bug with the Source Control Module not updating file state after a simple "Save" with no "Checkout" (when not using File Lock)
	const FDateTime Now = bUsingGitLfsLocking ? FDateTime::Now() : FDateTime::MinValue();

	// add history, if any
	for(const auto& History : Histories)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(History.Key);
		State->History = History.Value;
		State->TimeStamp = Now;
		bUpdated = true;
	}

	return bUpdated;
}

#undef LOCTEXT_NAMESPACE
