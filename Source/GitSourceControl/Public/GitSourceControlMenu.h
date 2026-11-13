// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"

struct FToolMenuSection;

/** Git extension of the Revision Control toolbar menu */
class FGitSourceControlMenu
{
public:
	void Register();
	void Unregister();
	
	/** This functions will be bound to appropriate Command. */
	void RevertChangesClicked();
	void RevertAndUnlockClicked();
	void RefreshClicked();

protected:
	static void RevertAllCallback(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult, bool bUnlockAfterRevert);

private:
	void RevertClickedInternal(bool bUnlockAfterRevert);
	bool CanRevertClicked(bool bUnlockAfterRevert) const;

	void AddMenuExtension(FToolMenuSection& Builder);
	static void ExtendContentBrowserSourceControlSubMenu(class UToolMenu* Menu);
	static void ExecuteContentBrowserRevertChanges(TArray<FString> PackageNames);
	static void ExecuteContentBrowserRevertAndUnlock(TArray<FString> PackageNames);

	static void DisplayInProgressNotification(const FText& InOperationInProgressString);
	static void RemoveInProgressNotification();
	static void DisplaySucessNotification(const FName& InOperationName);
	static void DisplayFailureNotification(const FName& InOperationName);

private:
	/** Current revision control operation from extended menu if any */
	static TWeakPtr<class SNotificationItem> OperationInProgressNotification;

	/** Delegate called when a revision control operation has completed */
	void OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);
};
