// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "SGitSourceControlSettings.h"

#include "Runtime/Launch/Resources/Version.h"
#include "Fonts/SlateFontInfo.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SFilePathPicker.h"
#include "Widgets/Layout/SSeparator.h"
#include "EditorDirectories.h"
#include "Logging/MessageLog.h"
#include "Styling/AppStyle.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"


#define LOCTEXT_NAMESPACE "SGitSourceControlSettings"

void SGitSourceControlSettings::Construct(const FArguments& InArgs)
{
	ConstructBasedOnEngineVersion( );
}

void SGitSourceControlSettings::ConstructBasedOnEngineVersion( )
{
	const FText FileFilterType = NSLOCTEXT("GitSourceControl", "Executables", "Executables");
#if PLATFORM_WINDOWS
	const FString FileFilterText = FString::Printf(TEXT("%s (*.exe)|*.exe"), *FileFilterType.ToString());
#else
	const FString FileFilterText = FString::Printf(TEXT("%s"), *FileFilterType.ToString());
#endif

	using Self = std::remove_pointer_t<decltype(this)>;

	#define ROW_LEFT( PADDING_HEIGHT ) +SHorizontalBox::Slot() \
			.VAlign(VAlign_Center) \
			.HAlign(HAlign_Right) \
			.FillWidth(1.0f) \
			.Padding(FMargin(0.0f, 0.0f, 16.0f, PADDING_HEIGHT))

	#define ROW_RIGHT( PADDING_HEIGHT ) +SHorizontalBox::Slot() \
			.VAlign(VAlign_Center) \
			.FillWidth(2.0f) \
			.Padding(FMargin(0.0f, 0.0f, 0.0f, PADDING_HEIGHT))

	#define TT_GitPath LOCTEXT("BinaryPathLabel_Tooltip", "Path to Git binary")
	#define TT_RepoRoot LOCTEXT("RepositoryRootLabel_Tooltip", "Path to the root of the Git repository")
	#define TT_UserName LOCTEXT("UserNameLabel_Tooltip", "Git Username fetched from local config")
	#define TT_Email LOCTEXT("GitUserEmail_Tooltip", "Git E-mail fetched from local config")
	#define TT_LFS LOCTEXT("LfsUserName_Tooltip", "Git LFS username used to identify your locks (auto-detected when possible)")
	#define TT_UnlockMode LOCTEXT("UnlockMode_Tooltip", "When Git LFS locks are released automatically. Shared by the whole team through Config/DefaultEditor.ini.")

	ChildSlot
	[
		SNew(SVerticalBox)
		// Git Path
		+SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BinaryPathLabel", "Git Path"))
				.ToolTipText( TT_GitPath )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(SFilePathPicker)
				.BrowseButtonImage(FAppStyle::GetBrush("PropertyWindow.Button_Ellipsis"))
				.BrowseButtonStyle(FAppStyle::Get(), "HoverHintOnly")
				.BrowseDirectory(FEditorDirectories::Get().GetLastDirectory(ELastDirectory::GENERIC_OPEN))
				.BrowseTitle(LOCTEXT("BinaryPathBrowseTitle", "File picker..."))
				.FilePath(this, &Self::GetBinaryPathString)
				.FileTypeFilter(FileFilterText)
				.OnPathPicked(this, &Self::OnBinaryPathPicked)
			]
		]
		// Repository Root
		+SVerticalBox::Slot()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RepositoryRootLabel", "Root of the repository"))
				.ToolTipText( TT_RepoRoot )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(STextBlock)
				.Text(this, &Self::GetPathToRepositoryRoot)
				.ToolTipText( TT_RepoRoot )
			]
		]
		// User Name
		+SVerticalBox::Slot()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("UserNameLabel", "User Name"))
				.ToolTipText( TT_UserName )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(STextBlock)
				.Text(this, &Self::GetUserName)
				.ToolTipText( TT_UserName )
			]
		]
		// Email
		+SVerticalBox::Slot()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("EmailLabel", "E-mail"))
				.ToolTipText( TT_Email )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(STextBlock)
				.Text(this, &Self::GetUserEmail )
				.ToolTipText( TT_Email )
			]
		]
		// LFS Username
		+SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("LfsUserNameLabel", "LFS Username"))
				.ToolTipText( TT_LFS )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(SEditableTextBox)
				.Text(this, &Self::GetLfsUserName)
				.OnTextCommitted(this, &Self::OnLfsUserNameCommited)
				.HintText(LOCTEXT("LfsUserName_Hint", "Username to lock files on the LFS server"))
			]
		]
		// Unlock Mode
		+SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("UnlockModeLabel", "Unlock Mode"))
				.ToolTipText( TT_UnlockMode )
			]
			ROW_RIGHT( 10.0f )
			[
				MakeUnlockModeWidget()
			]
		]
	];
}

TSharedRef<SWidget> SGitSourceControlSettings::MakeUnlockModeWidget()
{
	EGitUnlockMode ProjectUnlockMode;
	if (FGitSourceControlSettings::ReadProjectUnlockMode(ProjectUnlockMode))
	{
		// Another team member already chose: the mode is shown, never asked again.
		return SNew(STextBlock)
			.Text(GetUnlockModeLabel(ProjectUnlockMode))
			.ToolTipText(FText::Format(LOCTEXT("UnlockMode_Shared_Tooltip", "{0}\nSet for the whole team in {1}."),
				GetUnlockModeTooltip(ProjectUnlockMode), FText::FromString(FGitSourceControlSettings::GetProjectConfigFile())));
	}

	UnlockModeOptions.Reset();
	UnlockModeOptions.Add(MakeShared<EGitUnlockMode>(EGitUnlockMode::OnMerge));
	UnlockModeOptions.Add(MakeShared<EGitUnlockMode>(EGitUnlockMode::OnPush));
	UnlockModeOptions.Add(MakeShared<EGitUnlockMode>(EGitUnlockMode::None));
	return SNew(SComboBox<TSharedPtr<EGitUnlockMode>>)
		.OptionsSource(&UnlockModeOptions)
		.OnGenerateWidget(this, &SGitSourceControlSettings::OnGenerateUnlockModeWidget)
		.OnSelectionChanged(this, &SGitSourceControlSettings::OnUnlockModeChanged)
		.ToolTipText(LOCTEXT("UnlockMode_Choose_Tooltip", "First setup of this project: choose for the whole team. The choice is saved in Config/DefaultEditor.ini, commit it."))
		[
			SNew(STextBlock)
			.Text(this, &SGitSourceControlSettings::GetSelectedUnlockModeText)
		];
}

FString SGitSourceControlSettings::GetBinaryPathString() const
{
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	return GitSourceControl.AccessSettings().GetBinaryPath();
}

void SGitSourceControlSettings::OnBinaryPathPicked( const FString& PickedPath ) const
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FString PickedFullPath = FPaths::ConvertRelativePathToFull(PickedPath);
	const bool bChanged = GitSourceControl.AccessSettings().SetBinaryPath(PickedFullPath);
	if(bChanged)
	{
		// Re-Check provided git binary path for each change
		GitSourceControl.GetProvider().CheckGitAvailability();
		if(GitSourceControl.GetProvider().IsGitAvailable())
		{
			GitSourceControl.SaveSettings();
		}
	}
}

FText SGitSourceControlSettings::GetPathToRepositoryRoot() const
{
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FString& PathToRepositoryRoot = GitSourceControl.GetProvider().GetPathToRepositoryRoot();
	return FText::FromString(PathToRepositoryRoot);
}

FText SGitSourceControlSettings::GetUserName() const
{
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FString& UserName = GitSourceControl.GetProvider().GetUserName();
	return FText::FromString(UserName);
}

FText SGitSourceControlSettings::GetUserEmail() const
{
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FString& UserEmail = GitSourceControl.GetProvider().GetUserEmail();
	return FText::FromString(UserEmail);
}

void SGitSourceControlSettings::OnLfsUserNameCommited(const FText& InText, ETextCommit::Type InCommitType)
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	GitSourceControl.AccessSettings().SetLfsUserName(InText.ToString());
	GitSourceControl.AccessSettings().SaveSettings();
	GitSourceControl.GetProvider().UpdateSettings();
}

FText SGitSourceControlSettings::GetLfsUserName() const
{
	// Shows the effective name (saved, else auto-detected, else Git user.name) without saving it: only a user commit writes the setting.
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	return FText::FromString(GitSourceControl.GetProvider().GetLockUser());
}

TSharedRef<SWidget> SGitSourceControlSettings::OnGenerateUnlockModeWidget(TSharedPtr<EGitUnlockMode> InMode) const
{
	const EGitUnlockMode Mode = InMode.IsValid() ? *InMode : EGitUnlockMode::OnMerge;
	return SNew(STextBlock)
		.Text(GetUnlockModeLabel(Mode))
		.ToolTipText(GetUnlockModeTooltip(Mode));
}

void SGitSourceControlSettings::OnUnlockModeChanged(TSharedPtr<EGitUnlockMode> InMode, ESelectInfo::Type InSelectInfo)
{
	if (!InMode.IsValid())
	{
		return;
	}
	SelectedUnlockMode = InMode;
	if (!FGitSourceControlSettings::WriteProjectUnlockMode(*InMode))
	{
		FMessageLog("SourceControl").Error(FText::Format(LOCTEXT("UnlockMode_WriteFailed", "Could not save the unlock mode in {0}."),
			FText::FromString(FGitSourceControlSettings::GetProjectConfigFile())));
		return;
	}
	FGitSourceControlModule::Get().GetProvider().SyncUnlockHook();
}

FText SGitSourceControlSettings::GetSelectedUnlockModeText() const
{
	return SelectedUnlockMode.IsValid()
		? GetUnlockModeLabel(*SelectedUnlockMode)
		: LOCTEXT("UnlockMode_Choose", "Choose for the whole team...");
}

FText SGitSourceControlSettings::GetUnlockModeLabel(EGitUnlockMode InMode)
{
	switch (InMode)
	{
	case EGitUnlockMode::OnMerge:
		return LOCTEXT("UnlockMode_OnMerge", "OnMerge (Default)");
	case EGitUnlockMode::OnPush:
		return LOCTEXT("UnlockMode_OnPush", "OnPush");
	case EGitUnlockMode::None:
		return LOCTEXT("UnlockMode_None", "None");
	default:
		return LOCTEXT("UnlockMode_OnMerge", "OnMerge (Default)");
	}
}

FText SGitSourceControlSettings::GetUnlockModeTooltip(EGitUnlockMode InMode)
{
	switch (InMode)
	{
	case EGitUnlockMode::OnMerge:
		return LOCTEXT("UnlockMode_OnMerge_Tooltip", "Unlock automatically when changes land on the remote base branch.");
	case EGitUnlockMode::OnPush:
		return LOCTEXT("UnlockMode_OnPush_Tooltip", "Unlock automatically when changes are pushed to a remote branch.");
	case EGitUnlockMode::None:
		return LOCTEXT("UnlockMode_None_Tooltip", "Never unlock automatically; release locks manually via Revert and Unlock.");
	default:
		return FText::GetEmpty();
	}
}

#undef LOCTEXT_NAMESPACE
