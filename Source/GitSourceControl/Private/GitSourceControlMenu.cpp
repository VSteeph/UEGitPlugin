// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlMenu.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlOperations.h"
#include "GitSourceControlUtils.h"

#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"

#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"

#include "PackageTools.h"
#include "FileHelpers.h"

#include "Logging/MessageLog.h"
#include "SourceControlHelpers.h"

#include "ToolMenus.h"
#include "ToolMenuContext.h"
#include "ToolMenuMisc.h"
#include "UObject/Linker.h"

static const FName GitSourceControlMenuTabName(TEXT("GitSourceControlMenu"));

#define LOCTEXT_NAMESPACE "GitSourceControl"

TWeakPtr<SNotificationItem> FGitSourceControlMenu::OperationInProgressNotification;

void FGitSourceControlMenu::Register()
{
	FToolMenuOwnerScoped SourceControlMenuOwner(GitSourceControlMenuTabName);
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		UToolMenu* SourceControlMenu = ToolMenus->ExtendMenu("StatusBar.ToolBar.SourceControl");
		FToolMenuSection& Section = SourceControlMenu->AddSection("GitSourceControlActions", LOCTEXT("GitSourceControlMenuHeadingActions", "Git"), FToolMenuInsert(NAME_None, EToolMenuInsertType::First));

		AddMenuExtension(Section);
	}
}

void FGitSourceControlMenu::Unregister()
{
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		ToolMenus->UnregisterOwnerByName("GitSourceControlMenu");
	}
}

void FGitSourceControlMenu::RevertClicked()
{
	if (OperationInProgressNotification.IsValid())
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Revision control operation already in progress"));
		SourceControlLog.Notify();
		return;
	}

	// Ask the user before reverting all!
	const FText DialogText(LOCTEXT("SourceControlMenu_Revert_Ask", "Revert all modifications of the working tree?"));
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::OkCancel, DialogText);
	if (Choice != EAppReturnType::Ok)
	{
		return;
	}

	// make sure we update the SCC status of all packages (this could take a long time, so we will run it as a background task)
	const TArray<FString> Filenames = GitSourceControlUtils::GetSourceControlledAssetPaths();

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	FSourceControlOperationRef Operation = ISourceControlOperation::Create<FUpdateStatus>();
	SourceControlProvider.Execute(Operation, FSourceControlChangelistPtr(), Filenames, EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateStatic(&FGitSourceControlMenu::RevertAllCallback));

	FNotificationInfo Info(LOCTEXT("SourceControlMenuRevertAll", "Checking for assets to revert..."));
	Info.bFireAndForget = false;
	Info.ExpireDuration = 0.0f;
	Info.FadeOutDuration = 1.0f;

	if (SourceControlProvider.CanCancelOperation(Operation))
	{
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("SourceControlMenuRevertAll_CancelButton", "Cancel"),
			LOCTEXT("SourceControlMenuRevertAll_CancelButtonTooltip", "Cancel the revert operation."),
			FSimpleDelegate::CreateStatic(&FGitSourceControlMenu::RevertAllCancelled, Operation)
		));
	}

	OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
	}
}

void FGitSourceControlMenu::RevertAllCallback(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	if (InResult != ECommandResult::Succeeded)
	{
		return;
	}

	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	// Get a list of all the revertable packages
	TArray<FString> PackageNames;
	TArray<UPackage*> LoadedPackages;
	TMap<FString, FSourceControlStatePtr> PackageStates;
	FEditorFileUtils::FindAllSubmittablePackageFiles(PackageStates, true);

	TArray<FSourceControlStateRef> RevertableStates = Provider.GetCachedStateByPredicate(
		[](const FSourceControlStateRef& State) { return State->CanRevert(); });
	for (const FSourceControlStateRef& State : RevertableStates)
	{
		FString PackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(State->GetFilename(), PackageName))
		{
			PackageStates.FindOrAdd(PackageName, State);
		}
	}

	for (TMap<FString, FSourceControlStatePtr>::TConstIterator PackageIter(PackageStates); PackageIter; ++PackageIter)
	{
		const FString PackageName = *PackageIter.Key();
		const FSourceControlStatePtr CurPackageSCCState = PackageIter.Value();

		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (Package != nullptr)
		{
			LoadedPackages.Add(Package);

			if (!Package->IsFullyLoaded())
			{
				FlushAsyncLoading();
				Package->FullyLoad();
			}
			ResetLoaders(Package);
		}

		PackageNames.Add(PackageName);
	}

	const auto FileNames = SourceControlHelpers::PackageFilenames(PackageNames);

	// Launch a "Revert" Operation
	const TSharedRef<FRevert, ESPMode::ThreadSafe> RevertOperation = ISourceControlOperation::Create<FRevert>();
	const auto Result = Provider.Execute(RevertOperation, FSourceControlChangelistPtr(), FileNames);

	RemoveInProgressNotification();
	if (Result != ECommandResult::Succeeded)
	{
		DisplayFailureNotification(TEXT("Revert"));
	}
	else
	{
		DisplaySucessNotification(TEXT("Revert"));
	}

	GitSourceControlUtils::ReloadPackages(LoadedPackages);
	Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), FSourceControlChangelistPtr(), FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous);
}

void FGitSourceControlMenu::RefreshClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		// Launch an "GitFetch" Operation
		TSharedRef<FGitFetch, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FGitFetch>();
		RefreshOperation->bUpdateStatus = true;
		const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FSourceControlChangelistPtr(), FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous,
															 FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(RefreshOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(RefreshOperation->GetName());
		}
	}
	else
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Revision control operation already in progress"));
		SourceControlLog.Notify();
	}
}

// Display an ongoing notification during the whole operation
void FGitSourceControlMenu::DisplayInProgressNotification(const FText& InOperationInProgressString)
{
	if (!OperationInProgressNotification.IsValid())
	{
		FNotificationInfo Info(InOperationInProgressString);
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.FadeOutDuration = 1.0f;
		OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (OperationInProgressNotification.IsValid())
		{
			OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
}

void FGitSourceControlMenu::RevertAllCancelled(FSourceControlOperationRef InOperation)
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	SourceControlProvider.CancelOperation(InOperation);

	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->ExpireAndFadeout();
	}

	OperationInProgressNotification.Reset();
}

// Remove the ongoing notification at the end of the operation
void FGitSourceControlMenu::RemoveInProgressNotification()
{
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->ExpireAndFadeout();
		OperationInProgressNotification.Reset();
	}
}

// Display a temporary success notification at the end of the operation
void FGitSourceControlMenu::DisplaySucessNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Success", "{0} operation was successful!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.bUseSuccessFailIcons = true;
	Info.Image = FAppStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
	
	FSlateNotificationManager::Get().AddNotification(Info);
#if UE_BUILD_DEBUG
	UE_LOG(LogSourceControl, Log, TEXT("%s"), *NotificationText.ToString());
#endif
}

// Display a temporary failure notification at the end of the operation
void FGitSourceControlMenu::DisplayFailureNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Failure", "Error: {0} operation failed!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.ExpireDuration = 8.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	UE_LOG(LogSourceControl, Error, TEXT("%s"), *NotificationText.ToString());
}

void FGitSourceControlMenu::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	RemoveInProgressNotification();

	if (InOperation->GetName() == "Revert")
	{
		// Reload packages that were unlinked at the beginning of the Revert operation
		GitSourceControlUtils::ReloadPackages(PackagesToReload);
	}

	// Report result with a notification
	if (InResult == ECommandResult::Succeeded)
	{
		DisplaySucessNotification(InOperation->GetName());
	}
	else
	{
		DisplayFailureNotification(InOperation->GetName());
	}
}

void FGitSourceControlMenu::AddMenuExtension(FToolMenuSection& Builder)
{
	Builder.AddMenuEntry(
		"GitRevert",
		LOCTEXT("GitRevert",			"Revert"),
		LOCTEXT("GitRevertTooltip",		"Revert all files in the repository to their unchanged state."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Revert"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RevertClicked),
			FCanExecuteAction()
		)
	);

	Builder.AddMenuEntry(
		"GitRefresh",
		LOCTEXT("GitRefresh",			"Refresh"),
		LOCTEXT("GitRefreshTooltip",	"Update the revision control status of all files in the local repository."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Refresh"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RefreshClicked),
			FCanExecuteAction()
		)
	);
}

#undef LOCTEXT_NAMESPACE
