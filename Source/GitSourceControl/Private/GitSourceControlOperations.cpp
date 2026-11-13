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
	const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToGitRoot);

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
		const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(InCommand.PathToGitRoot, RelativeFile);
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
		GitSourceControlUtils::CollectNewStates(CheckoutStates, States);
		InCommand.bCommandSuccessful = false;
		return false;
	}

	const bool bSuccess = GitSourceControlUtils::RunLFSCommand(TEXT("lock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), LockableRelativeFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	InCommand.bCommandSuccessful = bSuccess;
	const FString& LockUser = FGitSourceControlModule::Get().GetProvider().GetLockUser();
	if (bSuccess)
	{
		TArray<FString> AbsoluteFiles;
		for (const auto& RelativeFile : RelativeFiles)
		{
			FString AbsoluteFile = FPaths::Combine(InCommand.PathToGitRoot, RelativeFile);
			FGitLockedFilesCache::AddLockedFile(AbsoluteFile, LockUser);
			FPaths::NormalizeFilename(AbsoluteFile);
			AbsoluteFiles.Add(AbsoluteFile);
		}

		GitSourceControlUtils::CollectNewStates(AbsoluteFiles, States, EFileState::Unset, ETreeState::Unset, ELockState::Locked);
		for (auto& State : States)
		{
			State.Value.LockUser = LockUser;
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCheckOutWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitMarkForAddWorker::GetName() const
{
	return "MarkForAdd";
}

bool FGitMarkForAddWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Added, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return InCommand.bCommandSuccessful;
}

bool FGitMarkForAddWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitDeleteWorker::GetName() const
{
	return "Delete";
}

bool FGitDeleteWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Deleted, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return InCommand.bCommandSuccessful;
}

bool FGitDeleteWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}


// Get lists of Missing files (ie "deleted"), Modified files, and "other than Added" Existing files
void GetMissingVsExistingFiles(const TArray<FString>& InFiles, TArray<FString>& OutMissingFiles, TArray<FString>& OutAllExistingFiles, TArray<FString>& OutOtherThanAddedExistingFiles)
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	const TArray<FString> Files = (InFiles.Num() > 0) ? (InFiles) : (Provider.GetFilesInCache());

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(Files, LocalStates, EStateCacheUsage::Use);
	for (const auto& State : LocalStates)
	{
		if (FPaths::FileExists(State->GetFilename()))
		{
			if (State->IsAdded())
			{
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if (State->IsModified())
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if (State->CanRevert()) // for locked but unmodified files
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
			}
		}
		else
		{
			// If already queued for deletion, don't try to delete again
			if (State->IsSourceControlled() && !State->IsDeleted())
			{
				OutMissingFiles.Add(State->GetFilename());
			}
		}
	}
}

FName FGitRevertWorker::GetName() const
{
	return "Revert";
}

bool FGitRevertWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = true;

	// Filter files by status
	TArray<FString> MissingFiles;
	TArray<FString> AllExistingFiles;
	TArray<FString> OtherThanAddedExistingFiles;
	GetMissingVsExistingFiles(InCommand.Files, MissingFiles, AllExistingFiles, OtherThanAddedExistingFiles);

	const bool bRevertAll = InCommand.Files.Num() < 1;
	if (bRevertAll)
	{
		TArray<FString> Parms;
		Parms.Add(TEXT("--hard"));
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parms, FGitSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

		Parms.Reset(2);
		Parms.Add(TEXT("-f")); // force
		Parms.Add(TEXT("-d")); // remove directories
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("clean"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parms, FGitSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	else
	{
		if (MissingFiles.Num() > 0)
		{
			// "Added" files that have been deleted needs to be removed from revision control
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), MissingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
		if (AllExistingFiles.Num() > 0)
		{
			// reset and revert any changes already added to the index
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), AllExistingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), AllExistingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
		if (OtherThanAddedExistingFiles.Num() > 0)
		{
			// revert any changes in working copy (this would fails if the asset was in "Added" state, since after "reset" it is now "untracked")
			// may need to try a few times due to file locks from prior operations
			bool CheckoutSuccess = false;
			int32 Attempts = 10;
			while( Attempts-- > 0 )
			{
				CheckoutSuccess = GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), OtherThanAddedExistingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
				if (CheckoutSuccess)
				{
					break;
				}

				FPlatformProcess::Sleep(0.1f);
			}
			
			InCommand.bCommandSuccessful &= CheckoutSuccess;
		}
	}

	if (InCommand.bUsingGitLfsLocking)
	{
		// unlock files: execute the LFS command on relative filenames
		// (unlock only locked files, that is, not Added files)
		TArray<FString> LockedFiles;
		GitSourceControlUtils::GetLockedFiles(OtherThanAddedExistingFiles, LockedFiles);
		if (LockedFiles.Num() > 0)
		{
			const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(LockedFiles, InCommand.PathToGitRoot);
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunLFSCommand(TEXT("unlock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), RelativeFiles,
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

	// If no files were specified (full revert), refresh all relevant files instead of the specified files (which is an empty list in full revert)
	// This is required so that files that were "Marked for add" have their status updated after a full revert.
	TArray<FString> FilesToUpdate = InCommand.Files;
	if (InCommand.Files.Num() <= 0)
	{
		for (const auto& File : MissingFiles) FilesToUpdate.Add(File);
		for (const auto& File : AllExistingFiles) FilesToUpdate.Add(File);
		for (const auto& File : OtherThanAddedExistingFiles) FilesToUpdate.Add(File);
	}

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, FilesToUpdate, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
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

FName FGitCopyWorker::GetName() const
{
	return "Copy";
}

bool FGitCopyWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	// Copy or Move operation on a single file : Git does not need an explicit copy nor move,
	// but after a Move the Editor create a redirector file with the old asset name that points to the new asset.
	// The redirector needs to be committed with the new asset to perform a real rename.
	// => the following is to "MarkForAdd" the redirector, but it still need to be committed by selecting the whole directory and "check-in"
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Added, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		const bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCopyWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitResolveWorker::GetName() const
{
	return "Resolve";
}

bool FGitResolveWorker::Execute( class FGitSourceControlCommand& InCommand )
{
	check(InCommand.Operation->GetName() == GetName());

	// mark the conflicting files as resolved:
	TArray<FString> Results;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, Results, InCommand.ResultInfo.ErrorMessages);

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	const bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}

	return InCommand.bCommandSuccessful;
}

bool FGitResolveWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

#undef LOCTEXT_NAMESPACE
