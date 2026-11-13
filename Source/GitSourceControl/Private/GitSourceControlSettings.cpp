// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "SourceControlHelpers.h"

namespace GitSettingsConstants
{

/** The section of the ini file we load our settings from */
static const FString SettingsSection = TEXT("GitSourceControl.GitSourceControlSettings");

/** Section of the project's DefaultEditor.ini holding settings shared by the whole team */
static const TCHAR* ProjectSection = TEXT("GitSourceControl");
static const TCHAR* UnlockModeKey = TEXT("UnlockMode");

}

const FString & FGitSourceControlSettings::GetBinaryPath() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return BinaryPath; // Return a copy to be thread-safe
}

bool FGitSourceControlSettings::SetBinaryPath(const FString& InString)
{
	FScopeLock ScopeLock(&CriticalSection);
	const bool bChanged = (BinaryPath != InString);
	if(bChanged)
	{
		BinaryPath = InString;
	}
	return bChanged;
}

const FString FGitSourceControlSettings::GetLfsUserName() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return LfsUserName; // Return a copy to be thread-safe
}

bool FGitSourceControlSettings::SetLfsUserName(const FString& InString)
{
	FScopeLock ScopeLock(&CriticalSection);
	const bool bChanged = (LfsUserName != InString);
	if (bChanged)
	{
		LfsUserName = InString;
	}
	return bChanged;
}

FString FGitSourceControlSettings::GetProjectConfigFile()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultEditor.ini"));
}

bool FGitSourceControlSettings::ReadProjectUnlockMode(EGitUnlockMode& OutUnlockMode)
{
	FConfigFile ProjectConfig;
	ProjectConfig.Read(GetProjectConfigFile());
	FString Value;
	return ProjectConfig.GetString(GitSettingsConstants::ProjectSection, GitSettingsConstants::UnlockModeKey, Value)
		&& ParseUnlockMode(Value, OutUnlockMode);
}

EGitUnlockMode FGitSourceControlSettings::GetUnlockMode()
{
	EGitUnlockMode UnlockMode = EGitUnlockMode::OnMerge;
	ReadProjectUnlockMode(UnlockMode);
	return UnlockMode;
}

bool FGitSourceControlSettings::WriteProjectUnlockMode(EGitUnlockMode InUnlockMode)
{
	// Only this key changes: the rest of the team's file keeps its layout and comments.
	FConfigFile Change;
	Change.SetString(GitSettingsConstants::ProjectSection, GitSettingsConstants::UnlockModeKey, *UnlockModeToString(InUnlockMode));
	return Change.UpdateSinglePropertyInSection(*GetProjectConfigFile(), GitSettingsConstants::UnlockModeKey, GitSettingsConstants::ProjectSection);
}

FString FGitSourceControlSettings::UnlockModeToString(EGitUnlockMode InUnlockMode)
{
	switch (InUnlockMode)
	{
	case EGitUnlockMode::OnMerge:
		return TEXT("OnMerge");
	case EGitUnlockMode::OnPush:
		return TEXT("OnPush");
	case EGitUnlockMode::None:
		return TEXT("None");
	default:
		return TEXT("OnMerge");
	}
}

bool FGitSourceControlSettings::ParseUnlockMode(const FString& InString, EGitUnlockMode& OutUnlockMode)
{
	const FString Trimmed = InString.TrimStartAndEnd();
	if (Trimmed.Equals(TEXT("OnMerge"), ESearchCase::IgnoreCase))
	{
		OutUnlockMode = EGitUnlockMode::OnMerge;
		return true;
	}
	if (Trimmed.Equals(TEXT("OnPush"), ESearchCase::IgnoreCase))
	{
		OutUnlockMode = EGitUnlockMode::OnPush;
		return true;
	}
	if (Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		OutUnlockMode = EGitUnlockMode::None;
		return true;
	}
	OutUnlockMode = EGitUnlockMode::OnMerge;
	return false;
}

// This is called at startup nearly before anything else in our module: BinaryPath will then be used by the provider
void FGitSourceControlSettings::LoadSettings()
{
	FScopeLock ScopeLock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->GetString(*GitSettingsConstants::SettingsSection, TEXT("BinaryPath"), BinaryPath, IniFile);
	GConfig->GetString(*GitSettingsConstants::SettingsSection, TEXT("LfsUserName"), LfsUserName, IniFile);
}

void FGitSourceControlSettings::SaveSettings() const
{
	FScopeLock ScopeLock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->SetString(*GitSettingsConstants::SettingsSection, TEXT("BinaryPath"), *BinaryPath, IniFile);
	GConfig->SetString(*GitSettingsConstants::SettingsSection, TEXT("LfsUserName"), *LfsUserName, IniFile);
	GConfig->Flush(false, IniFile);
}
