// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlUtils.h"

#include "GitMessageLog.h"
#include "GitSourceControlCommand.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlSettings.h"
#include "HAL/PlatformProcess.h"

#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"

#include "HAL/PlatformProcess.h"
#include "ISourceControlModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Logging/MessageLog.h"
#include "Dom/JsonObject.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"
#include "Misc/Timespan.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "PackageTools.h"
#include "FileHelpers.h"
#include "Misc/MessageDialog.h"

#include "Runtime/Launch/Resources/Version.h"

#include "Async/Async.h"
#include "UObject/Linker.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

#ifndef GIT_DEBUG_STATUS
#define GIT_DEBUG_STATUS 0
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitSourceControlConstants
{
/** The maximum number of files we submit in a single Git command */
const int32 MaxFilesPerBatch = 50;
} // namespace GitSourceControlConstants

FDateTime FGitLockedFilesCache::LastUpdated = FDateTime::MinValue();
TMap<FString, FGitLfsLockInfo> FGitLockedFilesCache::LockedFiles;
FCriticalSection FGitLockedFilesCache::LockedFilesLock;
FCriticalSection FGitLockedFilesCache::ReadOnlyFlagsLock;
TSet<FString> FGitLockedFilesCache::ProtectedFreeFiles;
FCriticalSection FGitLockedFilesCache::ProtectedFreeFilesLock;

TMap<FString, FGitLfsLockInfo> FGitLockedFilesCache::GetLockedFiles()
{
	FScopeLock ScopeLock(&LockedFilesLock);
	return LockedFiles;
}

bool FGitLockedFilesCache::IsExpired(const FDateTime& InNow, const FTimespan& InMaxAge)
{
	FScopeLock ScopeLock(&LockedFilesLock);
	return InNow - LastUpdated > InMaxAge;
}

void FGitLockedFilesCache::MarkQueried(const FDateTime& InNow)
{
	FScopeLock ScopeLock(&LockedFilesLock);
	LastUpdated = InNow;
}

void FGitLockedFilesCache::SetLockedFiles(const TMap<FString, FGitLfsLockInfo>& newLocks, const FDateTime& InNow)
{
	TArray<FString> ChangedFiles;
	{
		FScopeLock ScopeLock(&LockedFilesLock);
		for (const TPair<FString, FGitLfsLockInfo>& lock : LockedFiles)
		{
			if (!newLocks.Contains(lock.Key))
			{
				ChangedFiles.Add(lock.Key);
			}
		}
		for (const TPair<FString, FGitLfsLockInfo>& lock : newLocks)
		{
			if (!LockedFiles.Contains(lock.Key))
			{
				ChangedFiles.Add(lock.Key);
			}
		}
		LockedFiles = newLocks;
		LastUpdated = InNow;
	}

	// Read-only flags touch the disk: they change outside the cache lock so no thread waits on file I/O for the cache.
	for (const FString& File : ChangedFiles)
	{
		OnFileLockChanged(File);
	}
}

void FGitLockedFilesCache::AddLockedFile(const FString& filePath, const FGitLfsLockInfo& lockInfo)
{
	{
		FScopeLock ScopeLock(&LockedFilesLock);
		LockedFiles.Add(filePath, lockInfo);
	}
	OnFileLockChanged(filePath);
}

void FGitLockedFilesCache::RemoveLockedFile(const FString& filePath)
{
	{
		FScopeLock ScopeLock(&LockedFilesLock);
		LockedFiles.Remove(filePath);
	}
	OnFileLockChanged(filePath);
}

void FGitLockedFilesCache::OnFileLockChanged(const FString& filePath)
{
	if (!FPaths::FileExists(filePath))
	{
		return;
	}
	const FString LfsUserName = FGitSourceControlModule::Get().GetProvider().GetLockUser();
	// Decided from the cache as it is when the flag is written, one file change at a time:
	// a thread holding an older lock state can never undo the flag of a newer one.
	FScopeLock FlagsLock(&ReadOnlyFlagsLock);
	bool bLocked = false;
	bool bOwnLock = false;
	{
		FScopeLock ScopeLock(&LockedFilesLock);
		if (const FGitLfsLockInfo* Lock = LockedFiles.Find(filePath))
		{
			bLocked = true;
			bOwnLock = Lock->IsOwnedBy(LfsUserName);
		}
	}
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*filePath, !bOwnLock);
	if (!bLocked)
	{
		FScopeLock ScopeLock(&ProtectedFreeFilesLock);
		ProtectedFreeFiles.Add(filePath);
	}
}

void FGitLockedFilesCache::ProtectFromUnlockedSave(const FString& filePath, ELockState::Type lockState)
{
	if (lockState == ELockState::NotLocked)
	{
		FScopeLock ScopeLock(&ProtectedFreeFilesLock);
		bool bAlreadyProtected = false;
		ProtectedFreeFiles.Add(filePath, &bAlreadyProtected);
		if (bAlreadyProtected)
		{
			return;
		}
	}
	else if (lockState != ELockState::LockedOther)
	{
		return;
	}
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*filePath, true);
}

namespace GitSourceControlUtils
{
// Launch the Git command line process and extract its results & errors
bool RunCommandInternalRaw(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors, const int32 ExpectedReturnCode /* = 0 */)
{
	int32 ReturnCode = 0;
	FString FullCommand;
	FString LogableCommand; // short version of the command for logging purpose

	// Without it Git escapes non-ASCII bytes in the paths it prints ("Caf\303\251.uasset"), which then match no asset.
	FullCommand = TEXT("-c core.quotePath=false ");
	if (!InRepositoryRoot.IsEmpty())
	{
		// Specify the working copy (the root) of the git repository (before the command itself)
		FullCommand += TEXT("-C \"");
		FullCommand += InRepositoryRoot;
		FullCommand += TEXT("\" ");
	}
	// then the git command itself ("status", "log", "commit"...)
	LogableCommand += InCommand;

	// Append to the command all parameters, and then finally the files
	for (const auto& Parameter : InParameters)
	{
		LogableCommand += TEXT(" ");
		LogableCommand += Parameter;
	}
	for (const auto& File : InFiles)
	{
		LogableCommand += TEXT(" \"");
		LogableCommand += File;
		LogableCommand += TEXT("\"");
	}
	// Also, Git does not have a "--non-interactive" option, as it auto-detects when there are no connected standard input/output streams

	FullCommand += LogableCommand;

#if UE_BUILD_DEBUG
	UE_LOG(LogSourceControl, Log, TEXT("RunCommand: 'git %s'"), *LogableCommand);
#endif

	FString PathToGitOrEnvBinary = InPathToGitBinary;
#if PLATFORM_MAC
	// The Cocoa application does not inherit shell environment variables, so add the path expected to have git-lfs to PATH
	FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	FString GitInstallPath = FPaths::GetPath(InPathToGitBinary);

	TArray<FString> PathArray;
	PathEnv.ParseIntoArray(PathArray, FPlatformMisc::GetPathVarDelimiter());
	bool bHasGitInstallPath = false;
	for (auto Path : PathArray)
	{
		if (GitInstallPath.Equals(Path, ESearchCase::CaseSensitive))
		{
			bHasGitInstallPath = true;
			break;
		}
	}

	if (!bHasGitInstallPath)
	{
		PathToGitOrEnvBinary = FString("/usr/bin/env");
		FullCommand = FString::Printf(TEXT("PATH=\"%s%s%s\" \"%s\" %s"), *GitInstallPath, FPlatformMisc::GetPathVarDelimiter(), *PathEnv, *InPathToGitBinary, *FullCommand);
	}
#endif

	FPlatformProcess::ExecProcess(*PathToGitOrEnvBinary, *FullCommand, &ReturnCode, &OutResults, &OutErrors);

#if UE_BUILD_DEBUG
	// TODO: add a setting to easily enable Verbose logging
	UE_LOG(LogSourceControl, Verbose, TEXT("RunCommand(%s):\n%s"), *InCommand, *OutResults);
	if (ReturnCode != ExpectedReturnCode)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("RunCommand(%s) ReturnCode=%d:\n%s"), *InCommand, ReturnCode, *OutErrors);
	}
#endif

	// Move push/pull progress information from the error stream to the info stream
	if(ReturnCode == ExpectedReturnCode && OutErrors.Len() > 0)
	{
		OutResults.Append(OutErrors);
		OutErrors.Empty();
	}

	return ReturnCode == ExpectedReturnCode;
}

// Basic parsing or results & errors from the Git command line process
static bool RunCommandInternal(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters,
							   const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult;
	FString Results;
	FString Errors;

	bResult = RunCommandInternalRaw(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, Results, Errors);
	Results.ParseIntoArray(OutResults, TEXT("\n"), true);
	Errors.ParseIntoArray(OutErrorMessages, TEXT("\n"), true);

	return bResult;
}

FString FindGitBinaryPath()
{
#if PLATFORM_WINDOWS
	// 1) First of all, look into standard install directories
	// NOTE using only "git" (or "git.exe") relying on the "PATH" envvar does not always work as expected, depending on the installation:
	// If the PATH is set with "git/cmd" instead of "git/bin",
	// "git.exe" launch "git/cmd/git.exe" that redirect to "git/bin/git.exe" and ExecProcess() is unable to catch its outputs streams.
	// First check the 64-bit program files directory:
	FString GitBinaryPath(TEXT("C:/Program Files/Git/bin/git.exe"));
	bool bFound = CheckGitAvailability(GitBinaryPath);
	if (!bFound)
	{
		// otherwise check the 32-bit program files directory.
		GitBinaryPath = TEXT("C:/Program Files (x86)/Git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
	}
	if (!bFound)
	{
		// else the install dir for the current user: C:\Users\UserName\AppData\Local\Programs\Git\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		GitBinaryPath = FString::Printf(TEXT("%s/Programs/Git/cmd/git.exe"), *AppDataLocalPath);
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 2) Else, look for the version of Git bundled with SmartGit "Installer with JRE"
	if (!bFound)
	{
		GitBinaryPath = TEXT("C:/Program Files (x86)/SmartGit/git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
		if (!bFound)
		{
			// If git is not found in "git/bin/" subdirectory, try the "bin/" path that was in use before
			GitBinaryPath = TEXT("C:/Program Files (x86)/SmartGit/bin/git.exe");
			bFound = CheckGitAvailability(GitBinaryPath);
		}
	}

	// 3) Else, look for the local_git provided by SourceTree
	if (!bFound)
	{
		// C:\Users\UserName\AppData\Local\Atlassian\SourceTree\git_local\bin
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		GitBinaryPath = FString::Printf(TEXT("%s/Atlassian/SourceTree/git_local/bin/git.exe"), *AppDataLocalPath);
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 4) Else, look for the PortableGit provided by GitHub Desktop
	if (!bFound)
	{
		// The latest GitHub Desktop adds its binaries into the local appdata directory:
		// C:\Users\UserName\AppData\Local\GitHub\PortableGit_c2ba306e536fdf878271f7fe636a147ff37326ad\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString SearchPath = FString::Printf(TEXT("%s/GitHub/PortableGit_*"), *AppDataLocalPath);
		TArray<FString> PortableGitFolders;
		IFileManager::Get().FindFiles(PortableGitFolders, *SearchPath, false, true);
		if (PortableGitFolders.Num() > 0)
		{
			// FindFiles just returns directory names, so we need to prepend the root path to get the full path.
			GitBinaryPath = FString::Printf(TEXT("%s/GitHub/%s/cmd/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last PortableGit found
			bFound = CheckGitAvailability(GitBinaryPath);
			if (!bFound)
			{
				// If Portable git is not found in "cmd/" subdirectory, try the "bin/" path that was in use before
				GitBinaryPath = FString::Printf(TEXT("%s/GitHub/%s/bin/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last
																																	// PortableGit found
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

	// 5) Else, look for the version of Git bundled with Tower
	if (!bFound)
	{
		GitBinaryPath = TEXT("C:/Program Files (x86)/fournova/Tower/vendor/Git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 6) Else, look for the PortableGit provided by Fork
	if (!bFound)
	{
		// The latest Fork adds its binaries into the local appdata directory:
		// C:\Users\UserName\AppData\Local\Fork\gitInstance\2.39.1\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString SearchPath = FString::Printf(TEXT("%s/Fork/gitInstance/*"), *AppDataLocalPath);
		TArray<FString> PortableGitFolders;
		IFileManager::Get().FindFiles(PortableGitFolders, *SearchPath, false, true);
		if (PortableGitFolders.Num() > 0)
		{
			// FindFiles just returns directory names, so we need to prepend the root path to get the full path.
			GitBinaryPath = FString::Printf(TEXT("%s/Fork/gitInstance/%s/cmd/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last PortableGit found
			bFound = CheckGitAvailability(GitBinaryPath);
			if (!bFound)
			{
				// If Portable git is not found in "cmd/" subdirectory, try the "bin/" path that was in use before
				GitBinaryPath = FString::Printf(TEXT("%s/Fork/gitInstance/%s/bin/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last
																																	// PortableGit found
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

#elif PLATFORM_MAC
	// 1) First of all, look for the version of git provided by official git
	FString GitBinaryPath = TEXT("/usr/local/git/bin/git");
	bool bFound = CheckGitAvailability(GitBinaryPath);

	// 2) Else, look for the version of git provided by Homebrew
	if (!bFound)
	{
		GitBinaryPath = TEXT("/usr/local/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 2.1) else apple silicon brew stores git in different place
	if (!bFound)
	{
		GitBinaryPath = TEXT("/opt/homebrew/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 3) Else, look for the version of git provided by MacPorts
	if (!bFound)
	{
		GitBinaryPath = TEXT("/opt/local/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 4) Else, look for the version of git provided by Command Line Tools
	if (!bFound)
	{
		GitBinaryPath = TEXT("/usr/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	{
		SCOPED_AUTORELEASE_POOL;
		NSWorkspace* SharedWorkspace = [NSWorkspace sharedWorkspace];

		// 5) Else, look for the version of local_git provided by SmartGit
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.syntevo.smartgit"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 6) Else, look for the version of local_git provided by SourceTree
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.torusknot.SourceTreeNotMAS"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git_local/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 7) Else, look for the version of local_git provided by GitHub Desktop
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.github.GitHubClient"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/app/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 8) Else, look for the version of local_git provided by Tower2
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.fournova.Tower2"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

#else
	FString GitBinaryPath = TEXT("/usr/bin/git");
	bool bFound = CheckGitAvailability(GitBinaryPath);
#endif

	if (bFound)
	{
		FPaths::MakePlatformFilename(GitBinaryPath);
	}
	else
	{
		// If we did not find a path to Git, set it empty
		GitBinaryPath.Empty();
	}

	return GitBinaryPath;
}

bool CheckGitAvailability(const FString& InPathToGitBinary, FGitVersion* OutVersion)
{
	FString InfoMessages;
	FString ErrorMessages;
	bool bGitAvailable = RunCommandInternalRaw(TEXT("version"), InPathToGitBinary, FString(), FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bGitAvailable)
	{
		if (!InfoMessages.StartsWith("git version"))
		{
			bGitAvailable = false;
		}
		else if (OutVersion)
		{
			ParseGitVersion(InfoMessages, OutVersion);
		}
	}

	return bGitAvailable;
}

void ParseGitVersion(const FString& InVersionString, FGitVersion* OutVersion)
{
	// Parse "git version 2.31.1.vfs.0.3" into the string "2.31.1.vfs.0.3"
	const FString& TokenVersionStringPtr = InVersionString.RightChop(12);
	if (!TokenVersionStringPtr.IsEmpty())
	{
		// Parse the version into its numerical components
		TArray<FString> ParsedVersionString;
		TokenVersionStringPtr.ParseIntoArray(ParsedVersionString, TEXT("."));
		const int Num = ParsedVersionString.Num();
		if (Num >= 3)
		{
			if (ParsedVersionString[0].IsNumeric() && ParsedVersionString[1].IsNumeric() && ParsedVersionString[2].IsNumeric())
			{
				OutVersion->Major = FCString::Atoi(*ParsedVersionString[0]);
				OutVersion->Minor = FCString::Atoi(*ParsedVersionString[1]);
				OutVersion->Patch = FCString::Atoi(*ParsedVersionString[2]);
				if (Num >= 5)
				{
					// If labeled with fork
					if (!ParsedVersionString[3].IsNumeric())
					{
						OutVersion->Fork = ParsedVersionString[3];
						OutVersion->bIsFork = true;
						OutVersion->ForkMajor = FCString::Atoi(*ParsedVersionString[4]);
						if (Num >= 6)
						{
							OutVersion->ForkMinor = FCString::Atoi(*ParsedVersionString[5]);
							if (Num >= 7)
							{
								OutVersion->ForkPatch = FCString::Atoi(*ParsedVersionString[6]);
							}
						}
					}
				}
				if (OutVersion->bIsFork)
				{
					UE_LOG(LogSourceControl, Log, TEXT("Git version %d.%d.%d.%s.%d.%d.%d"), OutVersion->Major, OutVersion->Minor, OutVersion->Patch, *OutVersion->Fork, OutVersion->ForkMajor, OutVersion->ForkMinor, OutVersion->ForkPatch);
				}
				else
				{
					UE_LOG(LogSourceControl, Log, TEXT("Git version %d.%d.%d"), OutVersion->Major, OutVersion->Minor, OutVersion->Patch);
				}
			}
		}
	}
}

// Find the root of the Git repository, looking from the provided path and upward in its parent directories.
bool FindRootDirectory(const FString& InPath, FString& OutRepositoryRoot)
{
	OutRepositoryRoot = InPath;

	auto TrimTrailing = [](FString& Str, const TCHAR Char) {
		int32 Len = Str.Len();
		while (Len && Str[Len - 1] == Char)
		{
			Str = Str.LeftChop(1);
			Len = Str.Len();
		}
	};

	TrimTrailing(OutRepositoryRoot, '\\');
	TrimTrailing(OutRepositoryRoot, '/');

	bool bFound = false;
	FString PathToGitSubdirectory;
	while (!bFound && !OutRepositoryRoot.IsEmpty())
	{
		// Look for the ".git" subdirectory (or file) present at the root of every Git repository
		PathToGitSubdirectory = OutRepositoryRoot / TEXT(".git");
		bFound = IFileManager::Get().DirectoryExists(*PathToGitSubdirectory) || IFileManager::Get().FileExists(*PathToGitSubdirectory);
		if (!bFound)
		{
			int32 LastSlashIndex;
			if (OutRepositoryRoot.FindLastChar('/', LastSlashIndex))
			{
				OutRepositoryRoot = OutRepositoryRoot.Left(LastSlashIndex);
			}
			else
			{
				OutRepositoryRoot.Empty();
			}
		}
	}
	if (!bFound)
	{
		OutRepositoryRoot = InPath; // If not found, return the provided dir as best possible root.
	}
	return bFound;
}

void GetUserConfig(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutUserName, FString& OutUserEmail)
{
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("user.name"));
	bResults = RunCommandInternal(TEXT("config"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutUserName = InfoMessages[0];
	}
	else
	{
		OutUserName = TEXT("");
	}

	Parameters.Reset(1);
	Parameters.Add(TEXT("user.email"));
	InfoMessages.Reset();
	bResults &= RunCommandInternal(TEXT("config"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutUserEmail = InfoMessages[0];
	}
	else
	{
		OutUserEmail = TEXT("");
	}
}

bool GetBranchName(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutBranchName)
{
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--short"));
	Parameters.Add(TEXT("--quiet")); // no error message while in detached HEAD
	Parameters.Add(TEXT("HEAD"));
	bResults = RunCommand(TEXT("symbolic-ref"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutBranchName = InfoMessages[0];
	}
	else
	{
		Parameters.Reset(2);
		Parameters.Add(TEXT("-1"));
		Parameters.Add(TEXT("--format=\"%h\"")); // no error message while in detached HEAD
		bResults = RunCommand(TEXT("log"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
		if (bResults && InfoMessages.Num() > 0)
		{
			OutBranchName = "HEAD detached at ";
			OutBranchName += InfoMessages[0];
		}
		else
		{
			bResults = false;
		}
	}

	return bResults;
}

bool GetRemoteBranchName(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutBranchName)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--abbrev-ref"));
	Parameters.Add(TEXT("--symbolic-full-name"));
	Parameters.Add(TEXT("@{u}"));
	bool bResults = RunCommand(TEXT("rev-parse"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(),
								InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutBranchName = InfoMessages[0];
	}
	if (!bResults)
	{
		static bool bRunOnce = true;
		if (bRunOnce)
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Upstream branch not found for the current branch, skipping current branch for remote check. Please push a remote branch."));
			bRunOnce = false;
		}
	}
	return bResults;
}

bool GetCommitInfo(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutCommitId, FString& OutCommitSummary)
{
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("-1"));
	Parameters.Add(TEXT("--format=\"%H %s\""));
	bResults = RunCommandInternal(TEXT("log"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutCommitId = InfoMessages[0].Left(40);
		OutCommitSummary = InfoMessages[0].RightChop(41);
	}

	return bResults;
}

bool GetRemoteUrl(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutRemoteUrl)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("get-url"));
	Parameters.Add(TEXT("origin"));
	const bool bResults = RunCommandInternal(TEXT("remote"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutRemoteUrl = InfoMessages[0];
	}

	return bResults;
}

TArray<FString> GetSourceControlledAssetPaths()
{
	return
	{
		FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir()),
		FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())
	};
}

bool RunCommand(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters,
				const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult = true;

	if (InFiles.Num() > GitSourceControlConstants::MaxFilesPerBatch)
	{
		// Batch files up so we dont exceed command-line limits
		int32 FileCount = 0;
		while (FileCount < InFiles.Num())
		{
			TArray<FString> FilesInBatch;
			for (int32 FileIndex = 0; FileCount < InFiles.Num() && FileIndex < GitSourceControlConstants::MaxFilesPerBatch; FileIndex++, FileCount++)
			{
				FilesInBatch.Add(InFiles[FileCount]);
			}

			TArray<FString> BatchResults;
			TArray<FString> BatchErrors;
			bResult &= RunCommandInternal(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, FilesInBatch, BatchResults, BatchErrors);
			OutResults += BatchResults;
			OutErrorMessages += BatchErrors;
		}
	}
	else
	{
		bResult = RunCommandInternal(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
	}

	return bResult;
}

bool RunLFSCommand(const FString& InCommand, const FString& InRepositoryRoot, const FString& InPathToGitBinary, const TArray<FString>& InParameters, const TArray<FString>& InFiles,
				   TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	// Runs the Git LFS the team's Git client and the unlock hook use.
	return GitSourceControlUtils::RunCommand(TEXT("lfs ") + InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
}

/**
 * Parse informations on a file locked with Git LFS
 *
 * Examples output of "git lfs locks":
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset    SRombauts       ID:891
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset                    ID:891
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset    ID:891
 */
namespace
{
	bool ParseLfsLockJsonObject(const FString& InRepositoryRoot, const TSharedPtr<FJsonObject>& InJsonObject, bool bHasVerifiedOwnership, bool bIsOwnLock, FString& OutLocalFilename, FGitLfsLockInfo& OutLockInfo)
	{
		if (!InJsonObject.IsValid())
		{
			return false;
		}

		FString RelativePath;
		if (!InJsonObject->TryGetStringField(TEXT("path"), RelativePath) || RelativePath.IsEmpty())
		{
			return false;
		}

		OutLocalFilename = FPaths::ConvertRelativePathToFull(InRepositoryRoot, RelativePath);
		FPaths::NormalizeFilename(OutLocalFilename);

		OutLockInfo = FGitLfsLockInfo();
		OutLockInfo.bHasVerifiedOwnership = bHasVerifiedOwnership;
		OutLockInfo.bIsOwnLock = bIsOwnLock;

		const TSharedPtr<FJsonObject>* OwnerObject = nullptr;
		if (InJsonObject->TryGetObjectField(TEXT("owner"), OwnerObject) && OwnerObject && (*OwnerObject).IsValid())
		{
			(*OwnerObject)->TryGetStringField(TEXT("name"), OutLockInfo.LockUser);
			OutLockInfo.LockUser.TrimStartAndEndInline();
		}

		FString LockedAtString;
		if (InJsonObject->TryGetStringField(TEXT("locked_at"), LockedAtString) && !LockedAtString.IsEmpty())
		{
			FDateTime ParsedDate;
			if (FDateTime::ParseIso8601(*LockedAtString, ParsedDate))
			{
				OutLockInfo.LockedAt = ParsedDate;
			}
		}

		return true;
	}

	void CollectLfsLocksFromJsonArray(
		const FString& InRepositoryRoot,
		const TArray<TSharedPtr<FJsonValue>>& InArray,
		bool bHasVerifiedOwnership,
		bool bIsOwnLock,
		TMap<FString, FGitLfsLockInfo>& OutLocks,
		FString* OutDetectedOwnUser = nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Entry : InArray)
		{
			if (!Entry.IsValid() || Entry->Type != EJson::Object)
			{
				continue;
			}

			FString LocalFilename;
			FGitLfsLockInfo LockInfo;
			if (ParseLfsLockJsonObject(InRepositoryRoot, Entry->AsObject(), bHasVerifiedOwnership, bIsOwnLock, LocalFilename, LockInfo))
			{
				if (OutDetectedOwnUser && OutDetectedOwnUser->IsEmpty() && bHasVerifiedOwnership && bIsOwnLock && !LockInfo.LockUser.IsEmpty())
				{
					*OutDetectedOwnUser = LockInfo.LockUser;
				}
				OutLocks.Add(MoveTemp(LocalFilename), MoveTemp(LockInfo));
			}
		}
	}
}

/**
 * @brief Extract the relative filename from a Git status result.
 *
 * Examples of status results:
M  Content/Textures/T_Perlin_Noise_M.uasset
R  Content/Textures/T_Perlin_Noise_M.uasset -> Content/Textures/T_Perlin_Noise_M2.uasset
?? Content/Materials/M_Basic_Wall.uasset
!! BasicCode.sln
 *
 * @param[in] InResult One line of status
 * @return Relative filename extracted from the line of status
 *
 * @see FGitStatusFileMatcher and StateFromGitStatus()
 */
static FString FilenameFromGitStatus(const FString& InResult)
{
	int32 RenameIndex;
	FString Result;
	if (InResult.FindLastChar('>', RenameIndex))
	{
		// Extract only the second part of a rename "from -> to"
		Result = InResult.RightChop(RenameIndex + 2);
	}
	else
	{
		// Extract the relative filename from the Git status result (after the 2 letters status and 1 space)
		Result = InResult.RightChop(3);
	}
	return Result.TrimQuotes();
}

/** Match the relative filename of a Git status result with a provided absolute filename */
class FGitStatusFileMatcher
{
public:
	FGitStatusFileMatcher(const FString& InAbsoluteFilename) : AbsoluteFilename(InAbsoluteFilename)
	{}

	bool operator()(const FString& InResult) const
	{
		return AbsoluteFilename.Contains(FilenameFromGitStatus(InResult));
	}

private:
	const FString& AbsoluteFilename;
};

/**
 * Extract and interpret the file state from the given Git status result.
 * @see http://git-scm.com/docs/git-status
 * ' ' = unmodified
 * 'M' = modified
 * 'A' = added
 * 'D' = deleted
 * 'R' = renamed
 * 'C' = copied
 * 'U' = updated but unmerged
 * '?' = unknown/untracked
 * '!' = ignored
 */
class FGitStatusParser
{
public:
	FGitStatusParser(const FString& InResult)
	{
		TCHAR IndexState = InResult[0];
		TCHAR WCopyState = InResult[1];
		if ((IndexState == 'U' || WCopyState == 'U') || (IndexState == 'A' && WCopyState == 'A') || (IndexState == 'D' && WCopyState == 'D'))
		{
			// "Unmerged" conflict cases are generally marked with a "U",
			// but there are also the special cases of both "A"dded, or both "D"eleted
			FileState = EFileState::Unmerged;
			TreeState = ETreeState::Working;
			return;
		}

		if (IndexState == ' ')
		{
			TreeState = ETreeState::Working;
		}
		else if (WCopyState == ' ')
		{
			TreeState = ETreeState::Staged;
		}

		if (IndexState == '?' || WCopyState == '?')
		{
			TreeState = ETreeState::Untracked;
			FileState = EFileState::Unknown;
		}
		else if (IndexState == '!' || WCopyState == '!')
		{
			TreeState = ETreeState::Ignored;
			FileState = EFileState::Unknown;
		}
		else if (IndexState == 'A')
		{
			FileState = EFileState::Added;
		}
		else if (IndexState == 'D')
		{
			FileState = EFileState::Deleted;
		}
		else if (WCopyState == 'D')
		{
			FileState = EFileState::Missing;
		}
		else if (IndexState == 'M' || WCopyState == 'M')
		{
			FileState = EFileState::Modified;
		}
		else if (IndexState == 'R')
		{
			FileState = EFileState::Renamed;
		}
		else if (IndexState == 'C')
		{
			FileState = EFileState::Copied;
		}
		else
		{
			// Unmodified never yield a status
			FileState = EFileState::Unknown;
		}
	}

	EFileState::Type FileState;
	ETreeState::Type TreeState;
};

void ReloadPackages(TArray<UPackage*>& InPackagesToReload)
{
	// UE-COPY: ContentBrowserUtils::SyncPathsFromSourceControl()
	// Syncing may have deleted some packages, so we need to unload those rather than re-load them...
	TArray<UPackage*> PackagesToUnload;
	InPackagesToReload.RemoveAll([&](UPackage* InPackage) -> bool {
		const FString PackageExtension = InPackage->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(InPackage->GetName(), PackageExtension);
		if (!FPaths::FileExists(PackageFilename))
		{
			PackagesToUnload.Emplace(InPackage);
			return true; // remove package
		}
		return false; // keep package
	});

	// Hot-reload the new packages...
	UPackageTools::ReloadPackages(InPackagesToReload);

	// Unload any deleted packages...
	UPackageTools::UnloadPackages(PackagesToUnload);
}

/// Convert filename relative to the repository root to absolute path (inplace)
void AbsoluteFilenames(const FString& InRepositoryRoot, TArray<FString>& InFileNames)
{
	for (auto& FileName : InFileNames)
	{
		FileName = FPaths::ConvertRelativePathToFull(InRepositoryRoot, FileName);
	}
}

/** Run a 'git ls-files' command to get all files tracked by Git recursively in a directory.
 *
 * Called in case of a "directory status" (no file listed in the command) when using the "Submit to Revision Control" menu.
 */
bool ListFilesInDirectoryRecurse(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InDirectory, TArray<FString>& OutFiles)
{
	TArray<FString> ErrorMessages;
	TArray<FString> Directory;
	Directory.Add(InDirectory);
	const bool bResult = RunCommandInternal(TEXT("ls-files"), InPathToGitBinary, InRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), Directory, OutFiles, ErrorMessages);
	AbsoluteFilenames(InRepositoryRoot, OutFiles);
	return bResult;
}

/** Parse the array of strings results of a 'git status' command for a directory
 *
 *  Called in case of a "directory status" (no file listed in the command) ONLY to detect Deleted/Missing/Untracked files
 * since those files are not listed by the 'git ls-files' command.
 *
 * @see #ParseFileStatusResult() above for an example of a 'git status' results
 */
static void ParseDirectoryStatusResult(const bool InUsingLfsLocking, const TMap<FString, FString>& InResults, TMap<FString, FGitSourceControlState>& OutStates)
{
	// Iterate on each line of result of the status command
	for (const auto& Result : InResults)
	{
		FGitSourceControlState FileState(Result.Key);
		if (!InUsingLfsLocking)
		{
			FileState.State.LockState = ELockState::Unlockable;
		}
		FGitStatusParser StatusParser(Result.Value);
		if ((EFileState::Deleted == StatusParser.FileState) || (EFileState::Missing == StatusParser.FileState) || (ETreeState::Untracked == StatusParser.TreeState))
		{
			FileState.State.FileState = StatusParser.FileState;
			FileState.State.TreeState = StatusParser.TreeState;
			OutStates.Add(Result.Key, MoveTemp(FileState));
		}
	}
}

/** Parse the array of strings results of a 'git status' command for a provided list of files all in a common directory
 *
 * Called in case of a normal refresh of status on a list of assets in a the Content Browser (or user selected "Refresh" context menu).
 *
 * Example git status results:
M  Content/Textures/T_Perlin_Noise_M.uasset
R  Content/Textures/T_Perlin_Noise_M.uasset -> Content/Textures/T_Perlin_Noise_M2.uasset
?? Content/Materials/M_Basic_Wall.uasset
!! BasicCode.sln
*/
static void ParseFileStatusResult(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const bool InUsingLfsLocking, const TSet<FString>& InFiles,
								  const TMap<FString, FString>& InResults, TMap<FString, FGitSourceControlState>& OutStates)
{
	FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return;
	}
	FGitSourceControlProvider& Provider = GitSourceControl->GetProvider();
	const FString LfsUserName = Provider.GetLockUser();

	TMap<FString, FGitLfsLockInfo> LockedFiles;
	TMap<FString, FString> Results = InResults;
	bool bCheckedLockedFiles = false;

	FString Result;

	// Iterate on all files explicitly listed in the command
	for (const auto& File : InFiles)
	{
		FGitSourceControlState FileState(File);
		FileState.State.FileState = EFileState::Unset;
		FileState.State.TreeState = ETreeState::Unset;
		FileState.State.LockState = ELockState::Unset;
		// Search the file in the list of status
		bool bFound = Results.RemoveAndCopyValue(File, Result);
		if (bFound)
		{
			// File found in status results; only the case for "changed" files
			FGitStatusParser StatusParser(Result);
#if UE_BUILD_DEBUG && GIT_DEBUG_STATUS
			UE_LOG(LogSourceControl, Log, TEXT("Status(%s) = '%s' => File:%d, Tree:%d"), *File, *Result, static_cast<int>(StatusParser.FileState), static_cast<int>(StatusParser.TreeState));
#endif

			FileState.State.FileState = StatusParser.FileState;
			FileState.State.TreeState = StatusParser.TreeState;
		}
		else
		{
			FileState.State.FileState = EFileState::Unknown;
			// File not found in status
			if (FPaths::FileExists(File))
			{
				// usually means the file is unchanged,
				FileState.State.TreeState = ETreeState::Unmodified;
#if UE_BUILD_DEBUG && GIT_DEBUG_STATUS
				UE_LOG(LogSourceControl, Log, TEXT("Status(%s) not found but exists => unchanged"), *File);
#endif
			}
			else
			{
				// but also the case for newly created content: there is no file on disk until the content is saved for the first time
				FileState.State.TreeState = ETreeState::NotInRepo;
#if UE_BUILD_DEBUG && GIT_DEBUG_STATUS
				UE_LOG(LogSourceControl, Log, TEXT("Status(%s) not found and does not exists => new/not controled"), *File);
#endif
			}
		}
		if (!InUsingLfsLocking)
		{
			FileState.State.LockState = ELockState::Unlockable;
		}
		else
		{
			if (IsFileLFSLockable(File))
			{
				if (!bCheckedLockedFiles)
				{
					bCheckedLockedFiles = true;
					TArray<FString> ErrorMessages;
					GetAllLocks(InRepositoryRoot, InPathToGitBinary, ErrorMessages, LockedFiles);
					FTSMessageLog SourceControlLog("SourceControl");
					for (int32 ErrorIndex = 0; ErrorIndex < ErrorMessages.Num(); ++ErrorIndex)
					{
						SourceControlLog.Error(FText::FromString(ErrorMessages[ErrorIndex]));
					}
				}
				if (const FGitLfsLockInfo* LockInfo = LockedFiles.Find(File))
				{
					FileState.State.LockUser = LockInfo->LockUser;
					FileState.State.LockDate = LockInfo->LockedAt;
					if (LockInfo->IsOwnedBy(LfsUserName))
					{
						FileState.State.LockState = ELockState::Locked;
					}
					else
					{
						FileState.State.LockState = ELockState::LockedOther;
					}
				}
				else
				{
					FileState.State.LockState = ELockState::NotLocked;
#if UE_BUILD_DEBUG && GIT_DEBUG_STATUS
					UE_LOG(LogSourceControl, Log, TEXT("Status(%s) Not Locked"), *File);
#endif
				}
				// New content stays writable until committed: designers create assets without taking a lock.
				const bool bTracked = FileState.State.FileState != EFileState::Added
					&& (FileState.State.TreeState == ETreeState::Unmodified || FileState.State.TreeState == ETreeState::Working || FileState.State.TreeState == ETreeState::Staged);
				if (bTracked)
				{
					FGitLockedFilesCache::ProtectFromUnlockedSave(File, FileState.State.LockState);
				}
			}
			else
			{
				FileState.State.LockState = ELockState::Unlockable;
			}
			
			
#if UE_BUILD_DEBUG && GIT_DEBUG_STATUS
			UE_LOG(LogSourceControl, Log, TEXT("Status(%s) Locked by '%s'"), *File, *FileState.State.LockUser);
#endif
		}
		OutStates.Add(File, MoveTemp(FileState));
	}

	// The above cannot detect deleted assets since there is no file left to enumerate (either by the Content Browser or by git ls-files)
	// => so we also parse the status results to explicitly look for Deleted/Missing assets
	ParseDirectoryStatusResult(InUsingLfsLocking, Results, OutStates);
}

void ParseStatusResults(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const bool InUsingLfsLocking, const TArray<FString>& InFiles,
							   const TMap<FString, FString>& InResults, TMap<FString, FGitSourceControlState>& OutStates)
{
	TSet<FString> Files;
	for (const auto& File : InFiles)
	{
		if (FPaths::DirectoryExists(File))
		{
			TArray<FString> DirectoryFiles;
			const bool bResult = ListFilesInDirectoryRecurse(InPathToGitBinary, InRepositoryRoot, File, DirectoryFiles);
			if (bResult)
			{
				for (const auto& InnerFile : DirectoryFiles)
				{
					Files.Add(InnerFile);
				}
			}
		}
		else
		{
			Files.Add(File);
		}
	}
	ParseFileStatusResult(InPathToGitBinary, InRepositoryRoot, InUsingLfsLocking, Files, InResults, OutStates);
}

static bool RemoteRefExists(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InRemoteBranch)
{
	FString Results;
	FString Errors;
	const FString FullRef = InRemoteBranch.StartsWith(TEXT("refs/"))
		? InRemoteBranch
		: (InRemoteBranch.StartsWith(TEXT("origin/"))
			? (TEXT("refs/remotes/") + InRemoteBranch)
			: (TEXT("refs/remotes/origin/") + InRemoteBranch));
	return RunCommandInternalRaw(TEXT("show-ref"), InPathToGitBinary, InRepositoryRoot, { TEXT("--verify"), TEXT("--quiet"), FullRef }, FGitSourceControlModule::GetEmptyStringArray(), Results, Errors);
}

static bool ResolveRemoteBaseBranch(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutRemoteBaseBranch)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	if (RunCommandInternal(TEXT("symbolic-ref"), InPathToGitBinary, InRepositoryRoot, { TEXT("--quiet"), TEXT("--short"), TEXT("refs/remotes/origin/HEAD") }, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages)
		&& InfoMessages.Num() > 0 && !InfoMessages[0].IsEmpty() && InfoMessages[0] != TEXT("origin/HEAD"))
	{
		OutRemoteBaseBranch = InfoMessages[0];
		return true;
	}

	static const TCHAR* CandidateBranches[] = {
		TEXT("origin/develop"),
		TEXT("origin/dev"),
		TEXT("origin/main"),
		TEXT("origin/master")
	};
	for (const TCHAR* Candidate : CandidateBranches)
	{
		if (RemoteRefExists(InPathToGitBinary, InRepositoryRoot, Candidate))
		{
			OutRemoteBaseBranch = Candidate;
			return true;
		}
	}

	return false;
}

void CheckRemote(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& Files,
				 TArray<FString>& OutErrorMessages, TMap<FString, FGitSourceControlState>& OutStates)
{
	// We can obtain a list of files that were modified between our remote branches and HEAD. Assumes that fetch has been run to get accurate info.

	TSet<FString> BranchesToDiff;

	FString RemoteBaseBranch;
	if (ResolveRemoteBaseBranch(InPathToGitBinary, InRepositoryRoot, RemoteBaseBranch))
	{
		BranchesToDiff.Add(RemoteBaseBranch);
	}

	bool bDiffAgainstRemoteCurrent = false;

	// Get the current branch's remote.
	FString CurrentBranchName;
	if (GetRemoteBranchName(InPathToGitBinary, InRepositoryRoot, CurrentBranchName))
	{
		// We have a valid remote, so diff against it.
		bDiffAgainstRemoteCurrent = true;
		// Ensure that the remote branch is in there.
		BranchesToDiff.Add(CurrentBranchName);
	}
	else
	{
		FString LocalBranchName;
		if (GetBranchName(InPathToGitBinary, InRepositoryRoot, LocalBranchName)
			&& !LocalBranchName.StartsWith(TEXT("HEAD detached"))
			&& RemoteRefExists(InPathToGitBinary, InRepositoryRoot, TEXT("origin/") + LocalBranchName))
		{
			CurrentBranchName = TEXT("origin/") + LocalBranchName;
			bDiffAgainstRemoteCurrent = true;
			BranchesToDiff.Add(CurrentBranchName);
		}
	}

	if (!BranchesToDiff.Num())
	{
		return;
	}

	TArray<FString> ErrorMessages;

	TArray<FString> LogResults;
	TArray<FString> DiffResults;
	TArray<FString> Intersection;

	TMap<FString, FString> NewerFiles;

	const FString AbsoluteProjectDirPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString AbsoluteContentDirPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	const FString AbsolutePluginsDirPath = FPaths::Combine(AbsoluteProjectDirPath, TEXT("Plugins/"));

	TArray<FString> FilesToDiff;
	if (AbsoluteContentDirPath.StartsWith(InRepositoryRoot))
	{
		FilesToDiff.AddUnique(AbsoluteContentDirPath);
	}
	if (AbsolutePluginsDirPath.StartsWith(InRepositoryRoot))
	{
		FilesToDiff.AddUnique(AbsolutePluginsDirPath);
	}
	for (const FString& File : Files)
	{
		if (File.StartsWith(InRepositoryRoot))
		{
			FilesToDiff.AddUnique(File);
		}
	}
	if (FilesToDiff.IsEmpty())
	{
		return;
	}

	TArray<FString> ParametersLog{TEXT("--pretty="), TEXT("--name-only"), TEXT(""), TEXT("--")};
	for (auto& Branch : BranchesToDiff)
	{
		const bool bCurrentBranch = bDiffAgainstRemoteCurrent && Branch.Equals(CurrentBranchName);
		// empty defaults to HEAD
		// .. means commits in the right that are not in the left
		ParametersLog[2] = FString::Printf(TEXT("..%s"), *Branch);

		const bool bResultLog = RunCommand(TEXT("log"), InPathToGitBinary, InRepositoryRoot, ParametersLog, FilesToDiff, LogResults, ErrorMessages);
		if (bResultLog)
		{
			if (!bCurrentBranch)
			{
				// Compare against HEAD, not the merge base: after a squash merge the base branch holds the same content
				// in a commit HEAD does not contain, and that file must stay current.
				TArray<FString> DiffParametersLog{ TEXT("--name-only"), TEXT("HEAD"), Branch, TEXT("--") };
				const bool bResultDiff = RunCommand(TEXT("diff"), InPathToGitBinary, InRepositoryRoot, DiffParametersLog, FilesToDiff, DiffResults, ErrorMessages);
				if (bResultDiff)
				{
					Intersection = DiffResults.FilterByPredicate([&LogResults](const FString& ChangedFile) { return LogResults.Contains(ChangedFile); });
				}
				else
				{
					Intersection = LogResults;
				}
			}
			else
			{
				Intersection = LogResults;
			}

			for (const FString& NewerFileName : Intersection)
			{
				const FString& NewerFilePath = FPaths::ConvertRelativePathToFull(InRepositoryRoot, NewerFileName);

				// Don't care about mergeable files (.collection, .ini, .uproject, etc)
				if (!IsFileLFSLockable(NewerFileName))
				{
					continue;
				}

				if (bCurrentBranch || !NewerFiles.Contains(NewerFilePath))
				{
					NewerFiles.Add(NewerFilePath, Branch);
				}
			}
		}
		LogResults.Reset();
		DiffResults.Reset();
		Intersection.Reset();
	}

	for (const auto& NewFile : NewerFiles)
	{
		if (FGitSourceControlState* FileState = OutStates.Find(NewFile.Key))
		{
			FileState->State.RemoteState = (bDiffAgainstRemoteCurrent && NewFile.Value.Equals(CurrentBranchName)) ? ERemoteState::NotAtHead : ERemoteState::NotLatest;
			FileState->State.HeadBranch = NewFile.Value;
		}
	}

	OutErrorMessages.Append(ErrorMessages);
}

const FTimespan CacheLimit = FTimespan::FromSeconds(30);

bool AreLockUsersEqual(const FString& UserA, const FString& UserB)
{
	return !UserA.IsEmpty() && UserA.Equals(UserB, ESearchCase::IgnoreCase);
}

bool ParseLfsLocksVerifyJson(const FString& InRepositoryRoot, const FString& InJsonText, TMap<FString, FGitLfsLockInfo>& OutLocks, FString& OutDetectedOwnUser)
{
	OutDetectedOwnUser.Empty();
	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InJsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return false;
	}

	if (!RootObject->HasField(TEXT("ours")) && !RootObject->HasField(TEXT("theirs")))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* OursArray = nullptr;
	if (RootObject->TryGetArrayField(TEXT("ours"), OursArray) && OursArray)
	{
		CollectLfsLocksFromJsonArray(InRepositoryRoot, *OursArray, true, true, OutLocks, &OutDetectedOwnUser);
	}

	const TArray<TSharedPtr<FJsonValue>>* TheirsArray = nullptr;
	if (RootObject->TryGetArrayField(TEXT("theirs"), TheirsArray) && TheirsArray)
	{
		CollectLfsLocksFromJsonArray(InRepositoryRoot, *TheirsArray, true, false, OutLocks);
	}

	return true;
}

bool ParseLfsLocksListJson(const FString& InRepositoryRoot, const FString& InJsonText, TMap<FString, FGitLfsLockInfo>& OutLocks)
{
	const FString Trimmed = InJsonText.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || Trimmed == TEXT("null"))
	{
		return true;
	}

	TSharedPtr<FJsonValue> RootValue;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
	if (!FJsonSerializer::Deserialize(Reader, RootValue) || !RootValue.IsValid())
	{
		return false;
	}

	if (RootValue->Type == EJson::Array)
	{
		CollectLfsLocksFromJsonArray(InRepositoryRoot, RootValue->AsArray(), false, false, OutLocks);
		return true;
	}

	if (RootValue->Type == EJson::Object)
	{
		FString LocalFilename;
		FGitLfsLockInfo LockInfo;
		if (ParseLfsLockJsonObject(InRepositoryRoot, RootValue->AsObject(), false, false, LocalFilename, LockInfo))
		{
			OutLocks.Add(MoveTemp(LocalFilename), MoveTemp(LockInfo));
		}
		return true;
	}

	if (RootValue->Type == EJson::Null)
	{
		return true;
	}

	return false;
}

bool GetAllLocks(const FString& InRepositoryRoot, const FString& InPathToGitBinary, TArray<FString>& OutErrorMessages, TMap<FString, FGitLfsLockInfo>& OutLocks, bool bInvalidateCache)
{
	// You may ask, why are we ignoring state cache, and instead maintaining our own lock cache?
	// The answer is that state cache updating is another operation, and those that update status
	// (and thus the state cache) are using GetAllLocks. However, querying remote locks are almost always
	// irrelevant in most of those update status cases. So, we need to provide a fast way to provide
	// an updated local lock state. We could do this through the relevant lfs lock command arguments, which
	// as you will see below, we use only for offline cases, but the exec cost of doing this isn't worth it
	// when we can easily maintain this cache here. So, we are really emulating an internal Git LFS locks cache
	// call, which gets fed into the state cache, rather than reimplementing the state cache :)
	const FDateTime CurrentTime = FDateTime::Now();
	const bool bCacheExpired = bInvalidateCache || FGitLockedFilesCache::IsExpired(CurrentTime, CacheLimit);
	bool bResult = false;
	if (bCacheExpired)
	{
		// First try `git lfs locks --verify --json`, which separates `ours` and `theirs` server-side
		// and exposes the exact authenticated LFS username.
		TArray<FString> VerifyErrorMessages;
		TArray<FString> Results;
		if (RunLFSCommand(TEXT("locks"), InRepositoryRoot, InPathToGitBinary, { TEXT("--verify"), TEXT("--json") }, FGitSourceControlModule::GetEmptyStringArray(),
						  Results, VerifyErrorMessages))
		{
			FString DetectedOwnUser;
			if (ParseLfsLocksVerifyJson(InRepositoryRoot, FString::Join(Results, TEXT("\n")), OutLocks, DetectedOwnUser))
			{
				bResult = true;
				if (!DetectedOwnUser.IsEmpty())
				{
					if (FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe())
					{
						GitSourceControl->GetProvider().QueueAutoDetectedLockUser(DetectedOwnUser);
					}
				}
			}
		}

		// Fall back to `git lfs locks --json` if `--verify` is unsupported (e.g. file:// remote or older server).
		if (!bResult)
		{
			Results.Reset();
			TArray<FString> LocksErrorMessages;
			if (RunLFSCommand(TEXT("locks"), InRepositoryRoot, InPathToGitBinary, { TEXT("--json") }, FGitSourceControlModule::GetEmptyStringArray(),
							  Results, LocksErrorMessages))
			{
				if (ParseLfsLocksListJson(InRepositoryRoot, FString::Join(Results, TEXT("\n")), OutLocks))
				{
					bResult = true;
				}
			}
			if (!bResult)
			{
				OutErrorMessages.Append(LocksErrorMessages);
			}
		}

		if (bResult)
		{
			FGitLockedFilesCache::SetLockedFiles(OutLocks, CurrentTime);
			return true;
		}

		// Server query failed (e.g. offline): merge `--cached --json` (others) and `--local --json` (ours).
		if (FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe())
		{
			const FString LockUser = GitSourceControl->GetProvider().GetLockUser();
			auto QueryOfflineLocks = [&](const TCHAR* ScopeFlag, bool bSelectOwnLocks) -> bool
			{
				TArray<FString> LocalResults;
				if (!RunLFSCommand(TEXT("locks"), InRepositoryRoot, InPathToGitBinary, { ScopeFlag, TEXT("--json") }, FGitSourceControlModule::GetEmptyStringArray(), LocalResults, OutErrorMessages))
				{
					return false;
				}
				TMap<FString, FGitLfsLockInfo> ParsedLocks;
				if (!ParseLfsLocksListJson(InRepositoryRoot, FString::Join(LocalResults, TEXT("\n")), ParsedLocks))
				{
					return false;
				}
				for (TPair<FString, FGitLfsLockInfo>& Entry : ParsedLocks)
				{
					if (AreLockUsersEqual(Entry.Value.LockUser, LockUser) == bSelectOwnLocks)
					{
						OutLocks.Add(MoveTemp(Entry.Key), MoveTemp(Entry.Value));
					}
				}
				return true;
			};

			const bool bCachedOk = QueryOfflineLocks(TEXT("--cached"), false);
			const bool bLocalOk = QueryOfflineLocks(TEXT("--local"), true);
			bResult = bCachedOk && bLocalOk;
		}
		// Offline, the locks Git LFS remembers become the cache; either way the unreachable server is asked again only once it expires.
		if (bResult)
		{
			FGitLockedFilesCache::SetLockedFiles(OutLocks, CurrentTime);
		}
		else
		{
			FGitLockedFilesCache::MarkQueried(CurrentTime);
		}
	}
	if (!bResult)
	{
		// We can use our internally tracked local lock cache (an effective combination of --cached and --local)
		OutLocks = FGitLockedFilesCache::GetLockedFiles();
		bResult = true;
	}
	return bResult;
}

FString GetFullPathFromGitStatus(const FString& Result, const FString& InRepositoryRoot)
{
	const FString& RelativeFilename = FilenameFromGitStatus(Result);
	FString File = FPaths::ConvertRelativePathToFull(InRepositoryRoot, RelativeFilename);
	return File;
}

// Run a batch of Git "status" command to update status of given files and/or directories.
bool RunUpdateStatus(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const bool InUsingLfsLocking, const TArray<FString>& InFiles,
					 TArray<FString>& OutErrorMessages, TMap<FString, FGitSourceControlState>& OutStates)
{
	// Remove files that aren't in the repository
	const TArray<FString>& RepoFiles = InFiles.FilterByPredicate([InRepositoryRoot](const FString& File) { return File.StartsWith(InRepositoryRoot); });

	if (!RepoFiles.Num())
	{
		return false;
	}

	TArray<FString> Parameters;
	Parameters.Add(TEXT("--porcelain"));
	Parameters.Add(TEXT("-uall")); // make sure we use -uall to list all files instead of directories
	// We skip checking ignored since no one ignores files that Unreal would read in as revision controlled (Content/{*.uasset,*.umap},Config/*.ini).
	TArray<FString> Results;
	// avoid locking the index when not needed (useful for status updates)
	const bool bResult = RunCommand(TEXT("--no-optional-locks status"), InPathToGitBinary, InRepositoryRoot, Parameters, RepoFiles, Results, OutErrorMessages);
	TMap<FString, FString> ResultsMap;
	for (const auto& Result : Results)
	{
		const FString& RelativeFilename = FilenameFromGitStatus(Result);
		const FString& File = FPaths::ConvertRelativePathToFull(InRepositoryRoot, RelativeFilename);
		ResultsMap.Add(File, Result);
	}
	if (bResult)
	{
		ParseStatusResults(InPathToGitBinary, InRepositoryRoot, InUsingLfsLocking, RepoFiles, ResultsMap, OutStates);
	}

	CheckRemote(InPathToGitBinary, InRepositoryRoot, RepoFiles, OutErrorMessages, OutStates);

	return bResult;
}

// Run a Git `cat-file --filters` command to dump the binary content of a revision into a file.
bool RunDumpToFile(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InParameter, const FString& InDumpFileName)
{
	int32 ReturnCode = -1;
	FString FullCommand;

	if (!InRepositoryRoot.IsEmpty())
	{
		// Specify the working copy (the root) of the git repository (before the command itself)
		FullCommand = TEXT("-C \"");
		FullCommand += InRepositoryRoot;
		FullCommand += TEXT("\" ");
	}

	// then the git command itself
	// Newer versions (2.9.3.windows.2) support smudge/clean filters used by Git LFS, git-fat, git-annex, etc
	FullCommand += TEXT("cat-file --filters ");

	// Append to the command the parameter
	FullCommand += TEXT("\"") + InParameter + TEXT("\"");

	const bool bLaunchDetached = false;
	const bool bLaunchHidden = true;
	const bool bLaunchReallyHidden = bLaunchHidden;

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;

	verify(FPlatformProcess::CreatePipe(PipeRead, PipeWrite));

	UE_LOG(LogSourceControl, Log, TEXT("RunDumpToFile: 'git %s'"), *FullCommand);

    FString PathToGitOrEnvBinary = InPathToGitBinary;
    #if PLATFORM_MAC
        // The Cocoa application does not inherit shell environment variables, so add the path expected to have git-lfs to PATH
        FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
        FString GitInstallPath = FPaths::GetPath(InPathToGitBinary);

        TArray<FString> PathArray;
        PathEnv.ParseIntoArray(PathArray, FPlatformMisc::GetPathVarDelimiter());
        bool bHasGitInstallPath = false;
        for (auto Path : PathArray)
        {
            if (GitInstallPath.Equals(Path, ESearchCase::CaseSensitive))
            {
                bHasGitInstallPath = true;
                break;
            }
        }

        if (!bHasGitInstallPath)
        {
            PathToGitOrEnvBinary = FString("/usr/bin/env");
            FullCommand = FString::Printf(TEXT("PATH=\"%s%s%s\" \"%s\" %s"), *GitInstallPath, FPlatformMisc::GetPathVarDelimiter(), *PathEnv, *InPathToGitBinary, *FullCommand);
        }
    #endif

	FProcHandle ProcessHandle = FPlatformProcess::CreateProc(*PathToGitOrEnvBinary, *FullCommand, bLaunchDetached, bLaunchHidden, bLaunchReallyHidden, nullptr, 0, *InRepositoryRoot, PipeWrite);
	if(ProcessHandle.IsValid())
	{
		FPlatformProcess::Sleep(0.01f);

		TArray<uint8> BinaryFileContent;
		bool bShouldContinue = true;
		while (FPlatformProcess::IsProcRunning(ProcessHandle) || bShouldContinue)
		{
			TArray<uint8> BinaryData;
			bShouldContinue = FPlatformProcess::ReadPipeToArray(PipeRead, BinaryData);
			if (BinaryData.Num() > 0)
			{
				// @todo: this is hacky!
				bool bIsLFSMessage = BinaryData[0] == 68 // Check for D in "Downloading"
									&& BinaryData.Last() == 10; // Check for new line
				if (bIsLFSMessage)
				{
					continue;
				}
				BinaryFileContent.Append(MoveTemp(BinaryData));
			}
		}

		FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
		if (ReturnCode == 0)
		{
			// Save buffer into temp file
			if (FFileHelper::SaveArrayToFile(BinaryFileContent, *InDumpFileName))
			{
				UE_LOG(LogSourceControl, Log, TEXT("Wrote '%s' (%do)"), *InDumpFileName, BinaryFileContent.Num());
			}
			else
			{
				UE_LOG(LogSourceControl, Error, TEXT("Could not write %s"), *InDumpFileName);
				ReturnCode = -1;
			}
		}
		else
		{
			UE_LOG(LogSourceControl, Error, TEXT("DumpToFile: ReturnCode=%d"), ReturnCode);
		}

		FPlatformProcess::CloseProc(ProcessHandle);
	}
	else
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to launch 'git cat-file'"));
	}

	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	return (ReturnCode == 0);
}

/**
 * Translate file actions from the given Git log --name-status command to keywords used by the Editor UI.
 *
 * @see https://www.kernel.org/pub/software/scm/git/docs/git-log.html
 * ' ' = unmodified
 * 'M' = modified
 * 'A' = added
 * 'D' = deleted
 * 'R' = renamed
 * 'C' = copied
 * 'T' = type changed
 * 'U' = updated but unmerged
 * 'X' = unknown
 * 'B' = broken pairing
 *
 * @see SHistoryRevisionListRowContent::GenerateWidgetForColumn(): "add", "edit", "delete", "branch" and "integrate" (everything else is taken like "edit")
 */
static FString LogStatusToString(TCHAR InStatus)
{
	switch (InStatus)
	{
		case TEXT(' '):
			return FString("unmodified");
		case TEXT('M'):
			return FString("modified");
		case TEXT('A'): // added: keyword "add" to display a specific icon instead of the default "edit" action one
			return FString("add");
		case TEXT('D'): // deleted: keyword "delete" to display a specific icon instead of the default "edit" action one
			return FString("delete");
		case TEXT('R'): // renamed keyword "branch" to display a specific icon instead of the default "edit" action one
			return FString("branch");
		case TEXT('C'): // copied keyword "branch" to display a specific icon instead of the default "edit" action one
			return FString("branch");
		case TEXT('T'):
			return FString("type changed");
		case TEXT('U'):
			return FString("unmerged");
		case TEXT('X'):
			return FString("unknown");
		case TEXT('B'):
			return FString("broked pairing");
	}

	return FString();
}

/**
 * Parse the array of strings results of a 'git log' command
 *
 * Example git log results:
commit 97a4e7626681895e073aaefd68b8ac087db81b0b
Author: Sébastien Rombauts <sebastien.rombauts@gmail.com>
Date:   2014-2015-05-15 21:32:27 +0200

	Another commit used to test History

	 - with many lines
	 - some <xml>
	 - and strange characteres $*+

M	Content/Blueprints/Blueprint_CeilingLight.uasset
R100	Content/Textures/T_Concrete_Poured_D.uasset Content/Textures/T_Concrete_Poured_D2.uasset

commit 355f0df26ebd3888adbb558fd42bb8bd3e565000
Author: Sébastien Rombauts <sebastien.rombauts@gmail.com>
Date:   2014-2015-05-12 11:28:14 +0200

	Testing git status, edit, and revert

A	Content/Blueprints/Blueprint_CeilingLight.uasset
C099	Content/Textures/T_Concrete_Poured_N.uasset Content/Textures/T_Concrete_Poured_N2.uasset
*/
static void ParseLogResults(const TArray<FString>& InResults, TGitSourceControlHistory& OutHistory)
{
	TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe> SourceControlRevision = MakeShareable(new FGitSourceControlRevision);
	for (const auto& Result : InResults)
	{
		if (Result.StartsWith(TEXT("commit "))) // Start of a new commit
		{
			// End of the previous commit
			if (SourceControlRevision->RevisionNumber != 0)
			{
				OutHistory.Add(MoveTemp(SourceControlRevision));

				SourceControlRevision = MakeShareable(new FGitSourceControlRevision);
			}
			SourceControlRevision->CommitId = Result.RightChop(7); // Full commit SHA1 hexadecimal string
			SourceControlRevision->ShortCommitId = SourceControlRevision->CommitId.Left(8); // Short revision ; first 8 hex characters (max that can hold a 32
																							// bit integer)
			SourceControlRevision->CommitIdNumber = FParse::HexNumber(*SourceControlRevision->ShortCommitId);
			SourceControlRevision->RevisionNumber = -1; // RevisionNumber will be set at the end, based off the index in the History
		}
		else if (Result.StartsWith(TEXT("Author: "))) // Author name & email
		{
			// Remove the 'email' part of the UserName
			FString UserNameEmail = Result.RightChop(8);
			int32 EmailIndex = 0;
			if (UserNameEmail.FindLastChar('<', EmailIndex))
			{
				SourceControlRevision->UserName = UserNameEmail.Left(EmailIndex - 1);
			}
		}
		else if (Result.StartsWith(TEXT("Date:   "))) // Commit date
		{
			FString Date = Result.RightChop(8);
			SourceControlRevision->Date = FDateTime::FromUnixTimestamp(FCString::Atoi(*Date));
		}
		//	else if(Result.IsEmpty()) // empty line before/after commit message has already been taken care by FString::ParseIntoArray()
		else if (Result.StartsWith(TEXT("    "))) // Multi-lines commit message
		{
			SourceControlRevision->Description += Result.RightChop(4);
			SourceControlRevision->Description += TEXT("\n");
		}
		else // Name of the file, starting with an uppercase status letter ("A"/"M"...)
		{
			const TCHAR Status = Result[0];
			SourceControlRevision->Action = LogStatusToString(Status); // Readable action string ("Added", Modified"...) instead of "A"/"M"...
			// Take care of special case for Renamed/Copied file: extract the second filename after second tabulation
			int32 IdxTab;
			if (Result.FindLastChar('\t', IdxTab))
			{
				SourceControlRevision->Filename = Result.RightChop(IdxTab + 1); // relative filename
			}
		}
	}
	// End of the last commit
	if (SourceControlRevision->RevisionNumber != 0)
	{
		OutHistory.Add(MoveTemp(SourceControlRevision));
	}

	// Then set the revision number of each Revision based on its index (reverse order since the log starts with the most recent change)
	for (int32 RevisionIndex = 0; RevisionIndex < OutHistory.Num(); RevisionIndex++)
	{
		const auto& SourceControlRevisionItem = OutHistory[RevisionIndex];
		SourceControlRevisionItem->RevisionNumber = OutHistory.Num() - RevisionIndex;

		// Special case of a move ("branch" in Perforce term): point to the previous change (so the next one in the order of the log)
		if ((SourceControlRevisionItem->Action == "branch") && (RevisionIndex < OutHistory.Num() - 1))
		{
			SourceControlRevisionItem->BranchSource = OutHistory[RevisionIndex + 1];
		}
	}
}

/**
 * Extract the SHA1 identifier and size of a blob (file) from a Git "ls-tree" command.
 *
 * Example output for the command git ls-tree --long 7fdaeb2 Content/Blueprints/BP_Test.uasset
100644 blob a14347dc3b589b78fb19ba62a7e3982f343718bc   70731	Content/Blueprints/BP_Test.uasset
*/
class FGitLsTreeParser
{
public:
	/** Parse the unmerge status: extract the base SHA1 identifier of the file */
	FGitLsTreeParser(const TArray<FString>& InResults)
	{
		const FString& FirstResult = InResults[0];
		FileHash = FirstResult.Mid(12, 40);
		int32 IdxTab;
		if (FirstResult.FindChar('\t', IdxTab))
		{
			const FString SizeString = FirstResult.Mid(53, IdxTab - 53);
			FileSize = FCString::Atoi(*SizeString);
		}
	}

	FString FileHash; ///< SHA1 Id of the file (warning: not the commit Id)
	int32 FileSize; ///< Size of the file (in bytes)
};

// Run a Git "log" command and parse it.
bool RunGetHistory(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InFile, bool bMergeConflict,
				   TArray<FString>& OutErrorMessages, TGitSourceControlHistory& OutHistory)
{
	bool bResults;
	{
		TArray<FString> Results;
		TArray<FString> Parameters;
		Parameters.Add(TEXT("--no-color"));
		Parameters.Add(TEXT("--follow")); // follow file renames
		Parameters.Add(TEXT("--date=raw"));
		Parameters.Add(TEXT("--name-status")); // relative filename at this revision, preceded by a status character
		Parameters.Add(TEXT("--pretty=medium")); // make sure format matches expected in ParseLogResults
		if (bMergeConflict)
		{
			// In case of a merge conflict, we also need to get the tip of the "remote branch" (MERGE_HEAD) before the log of the "current branch" (HEAD)
			// @todo does not work for a cherry-pick! Test for a rebase.
			Parameters.Add(TEXT("MERGE_HEAD"));
			Parameters.Add(TEXT("--max-count 1"));
		}
		else
		{
			Parameters.Add(TEXT("--max-count 250")); // Increase default count to 250 from 100
		}
		TArray<FString> Files;
		Files.Add(*InFile);
		bResults = RunCommand(TEXT("log"), InPathToGitBinary, InRepositoryRoot, Parameters, Files, Results, OutErrorMessages);
		if (bResults)
		{
			ParseLogResults(Results, OutHistory);
		}
	}
	for (auto& Revision : OutHistory)
	{
		// Get file (blob) sha1 id and size
		TArray<FString> Results;
		TArray<FString> Parameters;
		Parameters.Add(TEXT("--long")); // Show object size of blob (file) entries.
		Parameters.Add(Revision->CommitId);
		TArray<FString> Files;
		Files.Add(*Revision->GetFilename());
		bResults &= RunCommand(TEXT("ls-tree"), InPathToGitBinary, InRepositoryRoot, Parameters, Files, Results, OutErrorMessages);
		if (bResults && Results.Num())
		{
			FGitLsTreeParser LsTree(Results);
			Revision->FileHash = LsTree.FileHash;
			Revision->FileSize = LsTree.FileSize;
		}
		Revision->PathToRepoRoot = InRepositoryRoot;
	}

	return bResults;
}

TArray<FString> RelativeFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo)
{
	TArray<FString> RelativeFiles;
	FString RelativeTo = InRelativeTo;

	// Ensure that the path ends w/ '/'
	if ((RelativeTo.Len() > 0) && (RelativeTo.EndsWith(TEXT("/"), ESearchCase::CaseSensitive) == false) &&
		(RelativeTo.EndsWith(TEXT("\\"), ESearchCase::CaseSensitive) == false))
	{
		RelativeTo += TEXT("/");
	}
	for (FString FileName : InFileNames) // string copy to be able to convert it inplace
	{
		if (FPaths::MakePathRelativeTo(FileName, *RelativeTo))
		{
			RelativeFiles.Add(FileName);
		}
	}

	return RelativeFiles;
}

TArray<FString> AbsoluteFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo)
{
	TArray<FString> AbsFiles;

	for(FString FileName : InFileNames) // string copy to be able to convert it inplace
	{
		AbsFiles.Add(FPaths::Combine(InRelativeTo, FileName));
	}

	return AbsFiles;
}

bool UpdateCachedStates(const TMap<const FString, FGitState>& InResults)
{
	if (InResults.Num() == 0)
	{
		return false;
	}
	
	FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return false;
	}
	FGitSourceControlProvider& Provider = GitSourceControl->GetProvider();
	const bool bUsingGitLfsLocking = Provider.UsesCheckout();

	// TODO without LFS : Workaround a bug with the Source Control Module not updating file state after a simple "Save" with no "Checkout" (when not using File Lock)
	const FDateTime Now = bUsingGitLfsLocking ? FDateTime::Now() : FDateTime::MinValue();

	for (const auto& Pair : InResults)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(Pair.Key);
		const FGitState& NewState = Pair.Value;
		if (NewState.FileState != EFileState::Unset)
		{
			// Invalid transition
			if (NewState.FileState == EFileState::Added && !State->IsUnknown() && !State->CanAdd())
			{
				continue;
			}
			State->State.FileState = NewState.FileState;
		}
		if (NewState.TreeState != ETreeState::Unset)
		{
			State->State.TreeState = NewState.TreeState;
		}
		// If we're updating lock state, also update user
		if (NewState.LockState != ELockState::Unset)
		{
			State->State.LockState = NewState.LockState;
			State->State.LockUser = NewState.LockUser;
			State->State.LockDate = NewState.LockDate;
		}
		if (NewState.RemoteState != ERemoteState::Unset)
		{
			State->State.RemoteState = NewState.RemoteState;
			if (NewState.RemoteState == ERemoteState::UpToDate)
			{
				State->State.HeadBranch = TEXT("");
			}
			else
			{
				State->State.HeadBranch = NewState.HeadBranch;
			}
		}
		State->TimeStamp = Now;

		// We've just updated the state, no need for UpdateStatus to be ran for this file again.
		Provider.AddFileToIgnoreForceCache(State->LocalFilename);
	}

	return true;
}

bool CollectNewStates(const TMap<FString, FGitSourceControlState>& InStates, TMap<const FString, FGitState>& OutResults)
{
	if (InStates.Num() == 0)
	{
		return false;
	}
	
	for (const auto& InState : InStates)
	{
		OutResults.Add(InState.Key, InState.Value.State);
	}

	return true;
}

bool CollectNewStates(const TArray<FString>& InFiles, TMap<const FString, FGitState>& OutResults, EFileState::Type FileState, ETreeState::Type TreeState, ELockState::Type LockState, ERemoteState::Type RemoteState)
{
	if (InFiles.Num() == 0)
	{
		return false;
	}

	FGitState NewState;
	NewState.FileState = FileState;
	NewState.TreeState = TreeState;
	NewState.LockState = LockState;
	NewState.RemoteState = RemoteState;

	for (const auto& File : InFiles)
	{
		FGitState& State = OutResults.FindOrAdd(File, NewState);
		if (NewState.FileState != EFileState::Unset)
		{
			State.FileState = NewState.FileState;
		}
		if (NewState.TreeState != ETreeState::Unset)
		{
			State.TreeState = NewState.TreeState;
		}
		if (NewState.LockState != ELockState::Unset)
		{
			State.LockState = NewState.LockState;
		}
		if (NewState.RemoteState != ERemoteState::Unset)
		{
			State.RemoteState = NewState.RemoteState;
		}
	}

	return true;
}

/**
 * Helper struct for RemoveRedundantErrors()
 */
struct FRemoveRedundantErrors
{
	FRemoveRedundantErrors(const FString& InFilter) : Filter(InFilter)
	{}

	bool operator()(const FString& String) const
	{
		if (String.Contains(Filter))
		{
			return true;
		}

		return false;
	}

	/** The filter string we try to identify in the reported error */
	FString Filter;
};

void RemoveRedundantErrors(FGitSourceControlCommand& InCommand, const FString& InFilter)
{
	bool bFoundRedundantError = false;
	for (auto Iter(InCommand.ResultInfo.ErrorMessages.CreateConstIterator()); Iter; Iter++)
	{
		if (Iter->Contains(InFilter))
		{
			InCommand.ResultInfo.InfoMessages.Add(*Iter);
			bFoundRedundantError = true;
		}
	}

	InCommand.ResultInfo.ErrorMessages.RemoveAll(FRemoveRedundantErrors(InFilter));

	// if we have no error messages now, assume success!
	if (bFoundRedundantError && InCommand.ResultInfo.ErrorMessages.Num() == 0 && !InCommand.bCommandSuccessful)
	{
		InCommand.bCommandSuccessful = true;
	}
}

static TArray<FString> LockableTypes;
// Filled by the connection thread while status workers read it.
static FCriticalSection LockableTypesLock;

bool IsFileLFSLockable(const FString& InFile)
{
	FScopeLock ScopeLock(&LockableTypesLock);
	for (const auto& Type : LockableTypes)
	{
		if (InFile.EndsWith(Type))
		{
			return true;
		}
	}
	return false;
}

bool CheckLFSLockable(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages)
{
	TArray<FString> Results;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("lockable"));

	const bool bResults = RunCommand(TEXT("check-attr"), InPathToGitBinary, InRepositoryRoot, Parameters, InFiles, Results, OutErrorMessages);
	TArray<FString> NewLockableTypes;
	for (int32 i = 0; bResults && i < InFiles.Num() && i < Results.Num(); i++)
	{
		const FString& Result = Results[i];
		if (Result.EndsWith("set") && !Result.EndsWith("unset"))
		{
			NewLockableTypes.Add(InFiles[i].RightChop(1)); // Remove wildcard (*)
		}
	}

	FScopeLock ScopeLock(&LockableTypesLock);
	LockableTypes = MoveTemp(NewLockableTypes);
	return bResults;
}

namespace
{
	const TCHAR* UnlockHookMarker = TEXT("# UEGitPlugin managed hook");

	FString BuildUnlockHookScript(EGitUnlockMode InUnlockMode)
	{
		const FString ModeValue = FGitSourceControlSettings::UnlockModeToString(InUnlockMode);
		const TArray<FString> Lines = {
			TEXT("#!/bin/sh"),
			UnlockHookMarker,
			FString::Printf(TEXT("UNLOCK_MODE=\"%s\""), *ModeValue),
			TEXT(""),
			TEXT("[ \"$1\" = \"committed\" ] || exit 0"),
			TEXT("[ -z \"${UEGITPLUGIN_UNLOCK_HOOK_ACTIVE:-}\" ] || exit 0"),
			TEXT(""),
			TEXT("# Local-only transactions (commit, checkout, each rebase step) must exit before spawning any git process."),
			TEXT("REMOTE_UPDATES=\"\""),
			TEXT("while read -r old_oid new_oid ref_name; do"),
			TEXT("\tcase \"$ref_name\" in"),
			TEXT("\t\trefs/remotes/origin/HEAD) ;;"),
			TEXT("\t\trefs/remotes/origin/*) REMOTE_UPDATES=\"$REMOTE_UPDATES$old_oid $new_oid $ref_name"),
			TEXT("\" ;;"),
			TEXT("\tesac"),
			TEXT("done"),
			TEXT("[ -n \"$REMOTE_UPDATES\" ] || exit 0"),
			TEXT(""),
			TEXT("export UEGITPLUGIN_UNLOCK_HOOK_ACTIVE=1"),
			TEXT("[ \"$(git rev-parse --is-inside-work-tree </dev/null 2>/dev/null)\" = \"true\" ] || exit 0"),
			TEXT(""),
			TEXT("resolve_base_ref() {"),
			TEXT("\tbase_ref=$(git symbolic-ref --quiet refs/remotes/origin/HEAD </dev/null 2>/dev/null)"),
			TEXT("\tif [ -n \"$base_ref\" ] && [ \"$base_ref\" != \"refs/remotes/origin/HEAD\" ]; then"),
			TEXT("\t\tprintf '%s\\n' \"$base_ref\""),
			TEXT("\t\treturn 0"),
			TEXT("\tfi"),
			TEXT("\tfor candidate in refs/remotes/origin/develop refs/remotes/origin/dev refs/remotes/origin/main refs/remotes/origin/master; do"),
			TEXT("\t\tif git show-ref --verify --quiet \"$candidate\" </dev/null 2>/dev/null; then"),
			TEXT("\t\t\tprintf '%s\\n' \"$candidate\""),
			TEXT("\t\t\treturn 0"),
			TEXT("\t\tfi"),
			TEXT("\tdone"),
			TEXT("\treturn 1"),
			TEXT("}"),
			TEXT(""),
			TEXT("is_zero_oid() {"),
			TEXT("\tcase \"$1\" in"),
			TEXT("\t\t\"\"|*[!0]*) return 1 ;;"),
			TEXT("\t\t*) return 0 ;;"),
			TEXT("\tesac"),
			TEXT("}"),
			TEXT(""),
			TEXT("BASE_REF=\"\""),
			TEXT("if [ \"$UNLOCK_MODE\" = \"OnMerge\" ]; then"),
			TEXT("\tBASE_REF=$(resolve_base_ref) || exit 0"),
			TEXT("fi"),
			TEXT(""),
			TEXT("LOCKS_CHECKED=0"),
			TEXT("OWN_LOCKS=\"\""),
			TEXT(""),
			TEXT("load_own_locks() {"),
			TEXT("\tif [ \"$LOCKS_CHECKED\" -eq 1 ]; then"),
			TEXT("\t\t[ -n \"$OWN_LOCKS\" ]"),
			TEXT("\t\treturn $?"),
			TEXT("\tfi"),
			TEXT("\tLOCKS_CHECKED=1"),
			TEXT("\tlocks_out=$(git lfs locks --verify </dev/null 2>/dev/null)"),
			TEXT("\tif [ $? -eq 0 ]; then"),
			TEXT("\t\tOWN_LOCKS=$(printf '%s\\n' \"$locks_out\" | grep -E '^O[[:space:]]')"),
			TEXT("\telse"),
			TEXT("\t\tOWN_LOCKS=$(git lfs locks </dev/null 2>/dev/null) || OWN_LOCKS=\"\""),
			TEXT("\tfi"),
			TEXT("\t[ -n \"$OWN_LOCKS\" ]"),
			TEXT("}"),
			TEXT(""),
			TEXT("printf '%s' \"$REMOTE_UPDATES\" | while read -r old_oid new_oid ref_name; do"),
			TEXT("\tis_zero_oid \"$new_oid\" && continue"),
			TEXT(""),
			TEXT("\tif [ \"$UNLOCK_MODE\" = \"OnMerge\" ]; then"),
			TEXT("\t\t[ \"$ref_name\" = \"$BASE_REF\" ] || continue"),
			TEXT("\t\tis_zero_oid \"$old_oid\" && continue"),
			TEXT("\t\tchanged_files=$(git -c core.quotePath=false diff --name-only \"$old_oid\" \"$new_oid\" </dev/null 2>/dev/null) || continue"),
			TEXT("\telif [ \"$UNLOCK_MODE\" = \"OnPush\" ]; then"),
			TEXT("\t\tif ! is_zero_oid \"$old_oid\"; then"),
			TEXT("\t\t\tchanged_files=$(git -c core.quotePath=false diff --name-only \"$old_oid\" \"$new_oid\" </dev/null 2>/dev/null) || continue"),
			TEXT("\t\telse"),
			TEXT("\t\t\tif [ -z \"$BASE_REF\" ]; then"),
			TEXT("\t\t\t\tBASE_REF=$(resolve_base_ref || true)"),
			TEXT("\t\t\tfi"),
			TEXT("\t\t\tif [ -n \"$BASE_REF\" ] && [ \"$BASE_REF\" != \"$ref_name\" ]; then"),
			TEXT("\t\t\t\tchanged_files=$(git -c core.quotePath=false diff --name-only \"$BASE_REF...$new_oid\" </dev/null 2>/dev/null) || continue"),
			TEXT("\t\t\telse"),
			TEXT("\t\t\t\tchanged_files=$(git -c core.quotePath=false diff-tree --no-commit-id --name-only -r --root \"$new_oid\" </dev/null 2>/dev/null) || continue"),
			TEXT("\t\t\tfi"),
			TEXT("\t\tfi"),
			TEXT("\telse"),
			TEXT("\t\texit 0"),
			TEXT("\tfi"),
			TEXT(""),
			TEXT("\t[ -n \"$changed_files\" ] || continue"),
			TEXT("\tload_own_locks || continue"),
			TEXT(""),
			TEXT("\tprintf '%s\\n' \"$changed_files\" | while IFS= read -r file_path; do"),
			TEXT("\t\t[ -n \"$file_path\" ] || continue"),
			TEXT("\t\tprintf '%s\\n' \"$OWN_LOCKS\" | grep -F -q -- \"$file_path\" || continue"),
			TEXT("\t\t[ -z \"$(git status --porcelain -- \"$file_path\" </dev/null 2>/dev/null)\" ] || continue"),
			TEXT("\t\tgit diff --quiet \"$new_oid\" -- \"$file_path\" </dev/null 2>/dev/null || continue"),
			TEXT("\t\tif ! git lfs unlock -- \"$file_path\" </dev/null >/dev/null 2>&1; then"),
			TEXT("\t\t\tif [ ! -e \"$file_path\" ]; then"),
			TEXT("\t\t\t\tlock_id=$(git lfs locks --path=\"$file_path\" </dev/null 2>/dev/null | sed -n 's/.*ID:\\([0-9][0-9]*\\).*/\\1/p' | head -n 1)"),
			TEXT("\t\t\t\t[ -n \"$lock_id\" ] && git lfs unlock --id=\"$lock_id\" </dev/null >/dev/null 2>&1 || true"),
			TEXT("\t\t\tfi"),
			TEXT("\t\tfi"),
			TEXT("\tdone"),
			TEXT("done"),
			TEXT(""),
			TEXT("exit 0")
		};
		return FString::Join(Lines, TEXT("\n")) + TEXT("\n");
	}
}

bool CheckLfsLocking(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutProblem)
{
	FString InfoMessages;
	FString ErrorMessages;
	if (!RunCommandInternalRaw(TEXT("lfs"), InPathToGitBinary, InRepositoryRoot, { TEXT("version") }, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages)
		|| !InfoMessages.Contains(TEXT("git-lfs")))
	{
		OutProblem = TEXT("Git LFS was not found: install it, then reconnect.");
		return false;
	}

	TArray<FString> LockableErrorMessages;
	if (!CheckLFSLockable(InPathToGitBinary, InRepositoryRoot, { TEXT("*.uasset"), TEXT("*.umap") }, LockableErrorMessages))
	{
		OutProblem = FString::Printf(TEXT("Git could not read the repository attributes: %s"), *FString::Join(LockableErrorMessages, TEXT(" ")));
		return false;
	}
	if (!IsFileLFSLockable(TEXT(".uasset")) || !IsFileLFSLockable(TEXT(".umap")))
	{
		OutProblem = TEXT("Mark assets as lockable in the .gitattributes file at the root of the repository (\"*.uasset lockable\" and \"*.umap lockable\"), commit it, then reconnect.");
		return false;
	}
	return true;
}

bool SyncUnlockHook(const FString& InPathToGitBinary, const FString& InRepositoryRoot, EGitUnlockMode InUnlockMode, FString& OutErrorMessage)
{
	if (InPathToGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty())
	{
		OutErrorMessage = TEXT("Cannot install auto-unlock hook: Git binary or repository root is missing.");
		return false;
	}

	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	if (!RunCommandInternal(TEXT("rev-parse"), InPathToGitBinary, InRepositoryRoot, { TEXT("--git-path"), TEXT("hooks/reference-transaction") },
		FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages) || InfoMessages.IsEmpty())
	{
		OutErrorMessage = FString::Printf(TEXT("Cannot locate Git hooks directory: %s"), *FString::Join(ErrorMessages, TEXT(" ")));
		return false;
	}

	FString HookPath = InfoMessages[0].TrimStartAndEnd();
	if (FPaths::IsRelative(HookPath))
	{
		HookPath = FPaths::Combine(InRepositoryRoot, HookPath);
	}
	FPaths::NormalizeFilename(HookPath);

	const bool bExists = IFileManager::Get().FileExists(*HookPath);
	FString ExistingContent;
	if (bExists)
	{
		FFileHelper::LoadFileToString(ExistingContent, *HookPath);
	}
	const bool bIsManaged = bExists && ExistingContent.Contains(UnlockHookMarker);

	if (InUnlockMode == EGitUnlockMode::None)
	{
		if (bIsManaged && !IFileManager::Get().Delete(*HookPath))
		{
			OutErrorMessage = FString::Printf(TEXT("Failed to remove auto-unlock hook '%s'."), *HookPath);
			return false;
		}
		return true;
	}

	if (bExists && !bIsManaged)
	{
		OutErrorMessage = FString::Printf(TEXT("Cannot install auto-unlock hook: an unmanaged '%s' hook already exists."), *HookPath);
		return false;
	}

	const FString DesiredContent = BuildUnlockHookScript(InUnlockMode);
	if (bIsManaged && ExistingContent.Replace(TEXT("\r\n"), TEXT("\n")) == DesiredContent)
	{
		return true;
	}

	const FString HooksDir = FPaths::GetPath(HookPath);
	if (!HooksDir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*HooksDir, true);
	}

	const FTCHARToUTF8 Utf8Script(*DesiredContent);
	TArray<uint8> RawBytes(reinterpret_cast<const uint8*>(Utf8Script.Get()), Utf8Script.Length());
	if (!FFileHelper::SaveArrayToFile(RawBytes, *HookPath))
	{
		OutErrorMessage = FString::Printf(TEXT("Failed to write auto-unlock hook '%s'."), *HookPath);
		return false;
	}

#if PLATFORM_MAC || PLATFORM_LINUX
	chmod(TCHAR_TO_UTF8(*HookPath), 0755);
#endif
	return true;
}

bool FetchRemote(const FString& InPathToGitBinary, const FString& InPathToRepositoryRoot, bool InUsingGitLfsLocking, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	FString RemoteUrl;
	if (!GetRemoteUrl(InPathToGitBinary, InPathToRepositoryRoot, RemoteUrl))
	{
		return true;
	}

	auto StripOriginPrefix = [](FString Branch) -> FString
	{
		Branch.RemoveFromStart(TEXT("refs/remotes/"));
		Branch.RemoveFromStart(TEXT("origin/"));
		return Branch;
	};

	FString BaseBranchShort;
	FString RemoteBaseBranch;
	if (ResolveRemoteBaseBranch(InPathToGitBinary, InPathToRepositoryRoot, RemoteBaseBranch))
	{
		BaseBranchShort = StripOriginPrefix(RemoteBaseBranch);
	}

	TArray<FString> BranchesToFetch;
	FString UpstreamBranch;
	if (GetRemoteBranchName(InPathToGitBinary, InPathToRepositoryRoot, UpstreamBranch))
	{
		const FString UpstreamShort = StripOriginPrefix(UpstreamBranch);
		if (!UpstreamShort.IsEmpty())
		{
			BranchesToFetch.AddUnique(UpstreamShort);
		}
	}
	else
	{
		FString LocalBranchName;
		if (GetBranchName(InPathToGitBinary, InPathToRepositoryRoot, LocalBranchName)
			&& !LocalBranchName.StartsWith(TEXT("HEAD detached"))
			&& RemoteRefExists(InPathToGitBinary, InPathToRepositoryRoot, TEXT("origin/") + LocalBranchName))
		{
			BranchesToFetch.AddUnique(LocalBranchName);
		}
	}

	BranchesToFetch.Remove(BaseBranchShort);
	if (BaseBranchShort.IsEmpty() && BranchesToFetch.IsEmpty())
	{
		BaseBranchShort = TEXT("HEAD");
	}

	auto FetchBranch = [&](const FString& Branch, TArray<FString>& ErrorMessages)
	{
		return RunCommand(TEXT("fetch"), InPathToGitBinary, InPathToRepositoryRoot, { TEXT("--no-tags"), TEXT("origin"), Branch },
						  FGitSourceControlModule::GetEmptyStringArray(), OutResults, ErrorMessages);
	};

	// A single fetch of several branches aborts entirely when one of them is gone from the remote,
	// which is the normal state of a feature branch once its pull request is merged.
	const bool bBaseFetched = BaseBranchShort.IsEmpty() || FetchBranch(BaseBranchShort, OutErrorMessages);
	for (const FString& Branch : BranchesToFetch)
	{
		TArray<FString> OptionalErrorMessages;
		if (!FetchBranch(Branch, OptionalErrorMessages))
		{
			UE_LOG(LogSourceControl, Verbose, TEXT("Skipped fetching 'origin/%s': %s"), *Branch, *FString::Join(OptionalErrorMessages, TEXT(" ")));
		}
	}

	// Refresh lock states after fetching so any locks released by the reference-transaction hook are reflected immediately.
	if (InUsingGitLfsLocking)
	{
		TMap<FString, FGitLfsLockInfo> Locks;
		GetAllLocks(InPathToRepositoryRoot, InPathToGitBinary, OutErrorMessages, Locks, true);
	}
	return bBaseFetched;
}

} // namespace GitSourceControlUtils

#undef LOCTEXT_NAMESPACE
