// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlMenu.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlOperations.h"
#include "GitSourceControlRevertAndUnlock.h"
#include "GitSourceControlUtils.h"

#include "ContentBrowserMenuContexts.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"
#include "SourceControlWindows.h"

#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
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

		if (UToolMenu* SubMenu = ToolMenus->ExtendMenu("ContentBrowser.AssetContextMenu.SourceControlSubMenu"))
		{
			SubMenu->AddDynamicSection(
				"GitSourceControlRevertActions",
				FNewToolMenuDelegate::CreateStatic(&FGitSourceControlMenu::ExtendContentBrowserSourceControlSubMenu));
		}
	}
}

void FGitSourceControlMenu::Unregister()
{
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		ToolMenus->UnregisterOwnerByName("GitSourceControlMenu");
	}
}

namespace
{
	bool HasLocalChangesOrDirtyPackage(const FString& PackageName, const FSourceControlStatePtr& State)
	{
		if (!State.IsValid() || !State->IsSourceControlled() || State->IsCheckedOutOther())
		{
			return false;
		}
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			if (Package->IsDirty())
			{
				return true;
			}
		}
		return State->IsModified() || State->IsAdded();
	}

	// The global actions only touch packages: config, plugin code and the project file are edited outside the lock workflow.
	bool IsGlobalRevertCandidate(const FSourceControlStateRef& State, bool bUnlockAfterRevert)
	{
		if (!FPackageName::IsPackageExtension(*FPaths::GetExtension(State->GetFilename(), true)))
		{
			return false;
		}
		return bUnlockAfterRevert ? State->CanRevert() : (State->IsModified() || State->IsAdded());
	}

	bool ContainsMapPackage(const TArray<FString>& PackageNames)
	{
		for (const FString& PackageName : PackageNames)
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				if (Package->ContainsMap())
				{
					return true;
				}
			}
		}
		return false;
	}
}

void FGitSourceControlMenu::RevertChangesClicked()
{
	RevertClickedInternal(false);
}

void FGitSourceControlMenu::RevertAndUnlockClicked()
{
	RevertClickedInternal(true);
}

bool FGitSourceControlMenu::CanRevertClicked(bool bUnlockAfterRevert) const
{
	FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	TArray<FSourceControlStateRef> States = Provider.GetCachedStateByPredicate(
		[bUnlockAfterRevert](const FSourceControlStateRef& State)
		{
			return IsGlobalRevertCandidate(State, bUnlockAfterRevert);
		});
	return States.Num() > 0;
}

void FGitSourceControlMenu::RevertClickedInternal(bool bUnlockAfterRevert)
{
	if (OperationInProgressNotification.IsValid())
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Revision control operation already in progress"));
		SourceControlLog.Notify();
		return;
	}

	const FText DialogText = bUnlockAfterRevert
		? LOCTEXT("SourceControlMenu_RevertAndUnlock_Ask", "Revert all modified assets and release their Git LFS locks?")
		: LOCTEXT("SourceControlMenu_RevertChanges_Ask", "Revert all modified assets while keeping their Git LFS locks?");
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::OkCancel, DialogText);
	if (Choice != EAppReturnType::Ok)
	{
		return;
	}

	// make sure we update the SCC status of all packages (this could take a long time, so we will run it as a background task)
	const TArray<FString> Filenames = GitSourceControlUtils::GetSourceControlledAssetPaths();

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	FSourceControlOperationRef Operation = ISourceControlOperation::Create<FUpdateStatus>();

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

	const ECommandResult::Type ExecuteResult = SourceControlProvider.Execute(
		Operation,
		FSourceControlChangelistPtr(),
		Filenames,
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateStatic(&FGitSourceControlMenu::RevertAllCallback, bUnlockAfterRevert));
	if (ExecuteResult == ECommandResult::Failed)
	{
		RemoveInProgressNotification();
		DisplayFailureNotification(bUnlockAfterRevert ? FName(TEXT("RevertAndUnlock")) : FName(TEXT("Revert")));
	}
}

void FGitSourceControlMenu::RevertAllCallback(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult, bool bUnlockAfterRevert)
{
	const FName OperationName = bUnlockAfterRevert ? FName(TEXT("RevertAndUnlock")) : FName(TEXT("Revert"));
	if (InResult != ECommandResult::Succeeded)
	{
		RemoveInProgressNotification();
		DisplayFailureNotification(OperationName);
		return;
	}

	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	TArray<FSourceControlStateRef> RevertableStates = Provider.GetCachedStateByPredicate(
		[bUnlockAfterRevert](const FSourceControlStateRef& State)
		{
			return IsGlobalRevertCandidate(State, bUnlockAfterRevert);
		});

	TArray<FString> FileNames;
	TArray<UPackage*> LoadedPackages;
	FileNames.Reserve(RevertableStates.Num());
	for (const FSourceControlStateRef& State : RevertableStates)
	{
		const FString& Filename = State->GetFilename();
		FileNames.Add(Filename);

		FString PackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				LoadedPackages.Add(Package);

				if (!Package->IsFullyLoaded())
				{
					FlushAsyncLoading();
					Package->FullyLoad();
				}
				ResetLoaders(Package);
			}
		}
	}

	if (FileNames.IsEmpty())
	{
		RemoveInProgressNotification();
		FNotificationInfo EmptyInfo(bUnlockAfterRevert
			? LOCTEXT("SourceControlMenu_NothingToRevertAndUnlock", "No locked or modified assets to revert and unlock.")
			: LOCTEXT("SourceControlMenu_NothingToRevert", "No modified or added assets to revert."));
		FSlateNotificationManager::Get().AddNotification(EmptyInfo);
		return;
	}

	FSourceControlOperationRef RevertOperation = bUnlockAfterRevert
		? StaticCastSharedRef<ISourceControlOperation>(ISourceControlOperation::Create<FGitRevertAndUnlock>())
		: StaticCastSharedRef<ISourceControlOperation>(ISourceControlOperation::Create<FRevert>());
	const ECommandResult::Type Result = Provider.Execute(RevertOperation, FSourceControlChangelistPtr(), FileNames);

	RemoveInProgressNotification();
	if (Result != ECommandResult::Succeeded)
	{
		DisplayFailureNotification(OperationName);
	}
	else
	{
		DisplaySucessNotification(OperationName);
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

void FGitSourceControlMenu::ExtendContentBrowserSourceControlSubMenu(UToolMenu* Menu)
{
	if (!Menu)
	{
		return;
	}

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	if (SourceControlProvider.GetName() != FGitSourceControlModule::Get().GetProvider().GetName())
	{
		return;
	}

	const UContentBrowserAssetContextMenuContext* AssetContext = Menu->FindContext<UContentBrowserAssetContextMenuContext>();
	if (!AssetContext || !AssetContext->bCanBeModified || AssetContext->SelectedAssets.IsEmpty())
	{
		return;
	}

	TArray<FString> PackageNames;
	PackageNames.Reserve(AssetContext->SelectedAssets.Num());
	for (const FAssetData& AssetData : AssetContext->SelectedAssets)
	{
		if (!AssetData.PackageName.IsNone())
		{
			PackageNames.AddUnique(AssetData.PackageName.ToString());
		}
	}
	if (PackageNames.IsEmpty())
	{
		return;
	}

	bool bCanRevertChanges = false;
	bool bCanRevertAndUnlock = false;
	for (const FString& PackageName : PackageNames)
	{
		const FSourceControlStatePtr State = SourceControlProvider.GetState(SourceControlHelpers::PackageFilename(PackageName), EStateCacheUsage::Use);
		if (HasLocalChangesOrDirtyPackage(PackageName, State))
		{
			bCanRevertChanges = true;
			bCanRevertAndUnlock = true;
			break;
		}
		if (State.IsValid() && State->CanRevert())
		{
			bCanRevertAndUnlock = true;
		}
	}

	// A dynamic section is built in a temporary menu, so the engine's own section cannot be edited here. ToolMenus merges
	// sections by name and lets an extension's entry replace a same-name entry in place: reusing "SCCRevert" swaps the
	// engine's Revert for Revert Changes at its position.
	const FSlateIcon RevertIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Revert");
	FToolMenuSection& Section = Menu->AddSection("AssetSourceControlActions");
	Section.AddMenuEntry(
		"SCCRevert",
		LOCTEXT("GitRevertChanges", "Revert Changes"),
		LOCTEXT("GitRevertChangesTooltip", "Reverts local changes to the selected assets while keeping your Git LFS locks."),
		RevertIcon,
		FUIAction(
			FExecuteAction::CreateStatic(&FGitSourceControlMenu::ExecuteContentBrowserRevertChanges, PackageNames),
			FCanExecuteAction::CreateLambda([bCanRevertChanges]() { return bCanRevertChanges; })
		));

	FToolMenuEntry& UnlockEntry = Section.AddMenuEntry(
		"GitRevertAndUnlock",
		LOCTEXT("GitRevertAndUnlock", "Revert and Unlock"),
		LOCTEXT("GitRevertAndUnlockTooltip", "Reverts local changes to the selected assets and releases your Git LFS locks."),
		RevertIcon,
		FUIAction(
			FExecuteAction::CreateStatic(&FGitSourceControlMenu::ExecuteContentBrowserRevertAndUnlock, PackageNames),
			FCanExecuteAction::CreateLambda([bCanRevertAndUnlock]() { return bCanRevertAndUnlock; })
		));
	UnlockEntry.InsertPosition = FToolMenuInsert("SCCRevert", EToolMenuInsertType::After);
}

void FGitSourceControlMenu::ExecuteContentBrowserRevertChanges(TArray<FString> PackageNames)
{
	const bool bReloadWorld = ContainsMapPackage(PackageNames);
	FSourceControlWindows::PromptForRevert(PackageNames, bReloadWorld);
}

void FGitSourceControlMenu::ExecuteContentBrowserRevertAndUnlock(TArray<FString> PackageNames)
{
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	bool bHasChanges = false;
	for (const FString& PackageName : PackageNames)
	{
		const FSourceControlStatePtr State = Provider.GetState(SourceControlHelpers::PackageFilename(PackageName), EStateCacheUsage::Use);
		if (HasLocalChangesOrDirtyPackage(PackageName, State))
		{
			bHasChanges = true;
			break;
		}
	}

	if (bHasChanges)
	{
		const FText ConfirmText = LOCTEXT("GitRevertAndUnlock_Confirm", "Discard local changes and release Git LFS locks for the selected assets?");
		if (FMessageDialog::Open(EAppMsgType::YesNo, ConfirmText) != EAppReturnType::Yes)
		{
			return;
		}
	}

	const bool bReloadWorld = ContainsMapPackage(PackageNames);
	USourceControlHelpers::ApplyOperationAndReloadPackages(
		PackageNames,
		[](const TArray<FString>& InFilenames)
		{
			return ISourceControlModule::Get().GetProvider().Execute(
				ISourceControlOperation::Create<FGitRevertAndUnlock>(),
				InFilenames) == ECommandResult::Succeeded;
		},
		bReloadWorld,
		true);
}

void FGitSourceControlMenu::AddMenuExtension(FToolMenuSection& Builder)
{
	Builder.AddMenuEntry(
		"GitRevertAllAssets",
		LOCTEXT("GitRevertAllAssets",			"Revert All Assets"),
		LOCTEXT("GitRevertAllAssetsTooltip",	"Discard local modifications of every asset (.uasset, .umap) while keeping your Git LFS locks. Config, code and project files are not touched."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Revert"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RevertChangesClicked),
			FCanExecuteAction::CreateRaw(this, &FGitSourceControlMenu::CanRevertClicked, false)
		)
	);

	Builder.AddMenuEntry(
		"GitRevertAndUnlockAllAssets",
		LOCTEXT("GitRevertAndUnlockAllAssets",			"Revert and Unlock All Assets"),
		LOCTEXT("GitRevertAndUnlockAllAssetsTooltip",	"Discard local modifications of every asset (.uasset, .umap) and release your Git LFS locks. Config, code and project files are not touched."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Revert"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RevertAndUnlockClicked),
			FCanExecuteAction::CreateRaw(this, &FGitSourceControlMenu::CanRevertClicked, true)
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
