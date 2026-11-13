// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "Containers/UnrealString.h"
#include "HAL/CriticalSection.h"

/** Policy for automatic Git LFS lock release via the reference-transaction hook. */
enum class EGitUnlockMode : uint8
{
	OnMerge,
	OnPush,
	None
};

class GITSOURCECONTROL_API FGitSourceControlSettings
{
public:
	/** Get the Git Binary Path */
	const FString & GetBinaryPath() const;

	/** Set the Git Binary Path */
	bool SetBinaryPath(const FString& InString);

	/** Tell if using the Git LFS file Locking workflow */
	bool IsUsingGitLfsLocking() const;

	/** Configure the usage of Git LFS file Locking workflow */
	bool SetUsingGitLfsLocking(const bool InUsingGitLfsLocking);

	/** Get the username used by the Git LFS 2 File Locks server */
	const FString GetLfsUserName() const;

	/** Set the username used by the Git LFS 2 File Locks server */
	bool SetLfsUserName(const FString& InString);

	/** Get the automatic Git LFS unlock mode */
	EGitUnlockMode GetUnlockMode() const;

	/** Set the automatic Git LFS unlock mode */
	bool SetUnlockMode(EGitUnlockMode InUnlockMode);

	/** Convert an unlock mode enum value to its persistent string representation */
	static FString UnlockModeToString(EGitUnlockMode InUnlockMode);

	/** Parse an unlock mode string; defaults to OnMerge if unrecognized */
	static bool ParseUnlockMode(const FString& InString, EGitUnlockMode& OutUnlockMode);

	/** Load settings from ini file */
	void LoadSettings();

	/** Save settings to ini file */
	void SaveSettings() const;

private:
	/** A critical section for settings access */
	mutable FCriticalSection CriticalSection;

	/** Git binary path */
	FString BinaryPath;

	/** Tells if using the Git LFS file Locking workflow */
	bool bUsingGitLfsLocking = true;

	/** Username used by the Git LFS 2 File Locks server */
	FString LfsUserName;

	/** Automatic Git LFS unlock mode */
	EGitUnlockMode UnlockMode = EGitUnlockMode::OnMerge;
};
