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
#include "Widgets/Input/SFilePathPicker.h"
#include "Widgets/Layout/SSeparator.h"
#include "EditorDirectories.h"
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
	#define TT_LFS LOCTEXT("UseGitLfsLocking_Tooltip", "Uses Git LFS 2 File Locking workflow.")

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
		// LFS Config
		+SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(SCheckBox)
				.IsChecked(Self::IsUsingGitLfsLocking())
				.OnCheckStateChanged(this, &Self::OnCheckedUseGitLfsLocking)
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UseGitLfsLocking", "Uses Git LFS"))
					.ToolTipText( TT_LFS )
				]
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(SEditableTextBox)
				.Text(this, &Self::GetLfsUserName)
				.OnTextCommitted(this, &Self::OnLfsUserNameCommited)
				.IsEnabled(this, &Self::GetIsUsingGitLfsLocking)
				.HintText(LOCTEXT("LfsUserName_Hint", "Username to lock files on the LFS server"))
			]
		]
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

void SGitSourceControlSettings::OnCheckedUseGitLfsLocking(ECheckBoxState NewCheckedState)
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	GitSourceControl.AccessSettings().SetUsingGitLfsLocking(NewCheckedState == ECheckBoxState::Checked);
	GitSourceControl.AccessSettings().SaveSettings();
	GitSourceControl.GetProvider().UpdateSettings();
}

bool SGitSourceControlSettings::GetIsUsingGitLfsLocking() const
{
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	return GitSourceControl.AccessSettings().IsUsingGitLfsLocking();
}

ECheckBoxState SGitSourceControlSettings::IsUsingGitLfsLocking() const
{
	return (GetIsUsingGitLfsLocking() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked);
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

#undef LOCTEXT_NAMESPACE
