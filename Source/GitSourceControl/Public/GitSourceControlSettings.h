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

	/** Get the username used by the Git LFS 2 File Locks server */
	const FString GetLfsUserName() const;

	/** Set the username used by the Git LFS 2 File Locks server */
	bool SetLfsUserName(const FString& InString);

	/**
	 * Reads the unlock mode the team chose, from the project's versioned Config/DefaultEditor.ini.
	 * The file is read directly so a value saved in a user's own config layer can never override the team's choice.
	 * @returns false when no team member chose a mode yet.
	 */
	static bool ReadProjectUnlockMode(EGitUnlockMode& OutUnlockMode);

	/** The mode the hook applies: the team's choice, else OnMerge. */
	static EGitUnlockMode GetUnlockMode();

	/** Writes the team's unlock mode into Config/DefaultEditor.ini, to be committed with the project. */
	static bool WriteProjectUnlockMode(EGitUnlockMode InUnlockMode);

	/** Absolute path of the versioned file holding the team's unlock mode. */
	static FString GetProjectConfigFile();

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

	/** Username used by the Git LFS 2 File Locks server */
	FString LfsUserName;
};
