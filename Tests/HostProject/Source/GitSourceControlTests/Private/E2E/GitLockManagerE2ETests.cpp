#include "GitE2ESession.h"

#include "Editor.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Widgets/Input/SEditableTextBox.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace GitLockManagerE2ETests
{
	const FName LockSelfIcon(TEXT("GitSourceControl.LockSelf"));
	const FName LockOtherIcon(TEXT("GitSourceControl.LockOther"));
	const FName OutdatedIcon(TEXT("GitSourceControl.Outdated"));

	FName IconOf(const FSourceControlStatePtr& State)
	{
		return State.IsValid() ? State->GetIcon().GetStyleName() : NAME_None;
	}

	bool HasOutdatedBadge(const FSourceControlStatePtr& State)
	{
		const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("GitSourceControlStyle"));
		const FSlateBrush* Badge = Style ? Style->GetOptionalBrush(TEXT("GitSourceControl.OutdatedBadge"), nullptr, nullptr) : nullptr;
		return State.IsValid() && Badge && State->GetIcon().GetOverlayIcon() == Badge;
	}

	/** Clicks an entry of the Git section of the status bar Revision Control menu. */
	void ClickGitMenu(const TSharedRef<FGitE2ESession>& Session, const FName EntryName)
	{
		GitE2E::Do(Session, [Session, EntryName]()
		{
			return Session->Click(FGitE2ESession::FindEntry(Session->OpenRevisionControlMenu(), EntryName));
		});
	}

	/** Collects every text box of a widget tree, depth first. */
	void FindTextBoxes(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SEditableTextBox>>& OutTextBoxes)
	{
		if (Widget->GetType() == FName(TEXT("SEditableTextBox")))
		{
			OutTextBoxes.Add(StaticCastSharedRef<SEditableTextBox>(Widget));
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			FindTextBoxes(Children->GetChildAt(Index), OutTextBoxes);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowGreenPencilAndLockOnServer_When_DesignerChecksOutAsset,
	"UEGitPlugin.E2E.Should_ShowGreenPencilAndLockOnServer_When_DesignerChecksOutAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowGreenPencilAndLockOnServer_When_DesignerChecksOutAsset::RunTest(const FString& Parameters)
{
	// Arrange
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);

	// Act: right-click > Revision Control > Check Out.
	GitE2E::CheckOutFromContentBrowser(Session, 0);

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the plugin learns the designer's LFS name from the server"),
		[]() { return FGitSourceControlModule::Get().GetProvider().GetLockUser() == TEXT("Designer"); });
	GitE2E::Do(Session, [this, Session]()
	{
		const FGitE2EAsset& Asset = Session->Assets[0];
		TestEqual(TEXT("The server holds the designer's lock"), Session->ServerLockOwner(Asset), FString(TEXT("Designer")));
		TestEqual(TEXT("The asset shows the green pencil"), GitLockManagerE2ETests::IconOf(Session->GetState(Asset)), GitLockManagerE2ETests::LockSelfIcon);
		TestFalse(TEXT("The asset can be saved"), FPlatformFileManager::Get().GetPlatformFile().IsReadOnly(*Asset.AbsoluteFile));
		TestTrue(TEXT("The detected name is not saved without the designer"), FGitSourceControlModule::Get().AccessSettings().GetLfsUserName().IsEmpty());
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowRedPadlockWithOwnerAndDate_When_ColleagueLockedAsset,
	"UEGitPlugin.E2E.Should_ShowRedPadlockWithOwnerAndDate_When_ColleagueLockedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowRedPadlockWithOwnerAndDate_When_ColleagueLockedAsset::RunTest(const FString& Parameters)
{
	// Arrange: a colleague locks the asset from their machine.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitE2E::Do(Session, [Session]() { return Session->Repository.RunGitAs(TEXT("Colleague"), FString::Printf(TEXT("lfs lock \"%s\""), *Session->Assets[0].RelativeFile)); });

	// Act: Revision Control > Git > Refresh.
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the asset shows the colleague's lock"),
		[Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])) == GitLockManagerE2ETests::LockOtherIcon; });
	GitE2E::Do(Session, [this, Session]()
	{
		const FSourceControlStatePtr State = Session->GetState(Session->Assets[0]);
		const FString Tooltip = State.IsValid() ? State->GetDisplayTooltip().ToString() : FString();
		const FString Today = FText::AsDate(FDateTime::UtcNow(), EDateTimeStyle::Short).ToString();
		TestTrue(FString::Printf(TEXT("The hover names the colleague: '%s'"), *Tooltip), Tooltip.Contains(TEXT("Colleague")));
		TestTrue(FString::Printf(TEXT("The hover shows the lock date '%s': '%s'"), *Today, *Tooltip), Tooltip.Contains(Today));
		TestFalse(TEXT("The designer cannot check the asset out"), State.IsValid() && State->CanCheckout());
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_RestoreAssetsAndKeepLocks_When_DesignerRevertsAllAssets,
	"UEGitPlugin.E2E.Should_RestoreAssetsAndKeepLocks_When_DesignerRevertsAllAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_RestoreAssetsAndKeepLocks_When_DesignerRevertsAllAssets::RunTest(const FString& Parameters)
{
	// Arrange: a locked and edited asset, plus a local config edit that must survive.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	const FString RelativeConfig = FString::Printf(TEXT("Config/E2E-%s.ini"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AbsoluteConfig = FPaths::ConvertRelativePathToFull(Session->Repository.RepositoryRoot, RelativeConfig);
	GitE2E::Start(Session, 1);
	GitE2E::Do(Session, [Session, RelativeConfig, AbsoluteConfig]()
	{
		return FFileHelper::SaveStringToFile(FString(TEXT("[E2E]\nValue=Committed\n")), *AbsoluteConfig)
			&& Session->Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeConfig))
			&& Session->Repository.RunGit(TEXT("commit --quiet -m \"Add config\""))
			&& FFileHelper::SaveStringToFile(FString(TEXT("[E2E]\nValue=Local\n")), *AbsoluteConfig);
	});
	GitE2E::CheckOutFromContentBrowser(Session, 0);
	GitE2E::Do(Session, [Session]() { return Session->EditAndSave(Session->Assets[0], FGitE2ESession::EditedValue); });
	GitE2E::WaitUntil(Session, TEXT("Revert All Assets is enabled"),
		[Session]() { return FGitE2ESession::CanClick(FGitE2ESession::FindEntry(Session->OpenRevisionControlMenu(), "GitRevertAllAssets")); });

	// Act: Revision Control > Git > Revert All Assets, then OK.
	GitE2E::Do(Session, [Session]() { Session->AnswerNextDialog(EAppReturnType::Ok); return true; });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRevertAllAssets");

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the editor shows the committed asset again"),
		[Session]() { return Session->LoadedValue(Session->Assets[0]) == FGitE2ESession::CommittedValue; });
	GitE2E::Do(Session, [this, Session, AbsoluteConfig, RelativeConfig]()
	{
		FString Config;
		FFileHelper::LoadFileToString(Config, *AbsoluteConfig);
		TestEqual(TEXT("One confirmation was asked"), Session->DialogMessages.Num(), 1);
		TestEqual(TEXT("The server still holds the designer's lock"), Session->ServerLockOwner(Session->Assets[0]), FString(TEXT("Designer")));
		TestEqual(TEXT("The asset keeps the green pencil"), GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])), GitLockManagerE2ETests::LockSelfIcon);
		TestTrue(TEXT("The config edit is left alone"), Config.Contains(TEXT("Value=Local")));
		Session->Repository.RunGitQuietly(FString::Printf(TEXT("checkout -- \"%s\""), *RelativeConfig));
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_RestoreAssetsAndReleaseLocks_When_DesignerRevertsAndUnlocksAllAssets,
	"UEGitPlugin.E2E.Should_RestoreAssetsAndReleaseLocks_When_DesignerRevertsAndUnlocksAllAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_RestoreAssetsAndReleaseLocks_When_DesignerRevertsAndUnlocksAllAssets::RunTest(const FString& Parameters)
{
	// Arrange
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitE2E::CheckOutFromContentBrowser(Session, 0);
	GitE2E::Do(Session, [Session]() { return Session->EditAndSave(Session->Assets[0], FGitE2ESession::EditedValue); });
	GitE2E::WaitUntil(Session, TEXT("Revert and Unlock All Assets is enabled"),
		[Session]() { return FGitE2ESession::CanClick(FGitE2ESession::FindEntry(Session->OpenRevisionControlMenu(), "GitRevertAndUnlockAllAssets")); });

	// Act: Revision Control > Git > Revert and Unlock All Assets, then OK.
	GitE2E::Do(Session, [Session]() { Session->AnswerNextDialog(EAppReturnType::Ok); return true; });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRevertAndUnlockAllAssets");

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the server releases the lock"), [Session]() { return Session->ServerLockOwner(Session->Assets[0]).IsEmpty(); });
	GitE2E::WaitUntil(Session, TEXT("the asset icon disappears"), [Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])).IsNone(); });
	GitE2E::Do(Session, [this, Session]()
	{
		const FGitE2EAsset& Asset = Session->Assets[0];
		TestEqual(TEXT("The editor shows the committed asset again"), Session->LoadedValue(Asset), FGitE2ESession::CommittedValue);
		TestTrue(TEXT("The asset is read-only again"), FPlatformFileManager::Get().GetPlatformFile().IsReadOnly(*Asset.AbsoluteFile));
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_RevertAndUnlockOnlySelectedAsset_When_DesignerUsesAssetMenu,
	"UEGitPlugin.E2E.Should_RevertAndUnlockOnlySelectedAsset_When_DesignerUsesAssetMenu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_RevertAndUnlockOnlySelectedAsset_When_DesignerUsesAssetMenu::RunTest(const FString& Parameters)
{
	// Arrange: two locked and edited assets.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 2);
	GitE2E::CheckOutFromContentBrowser(Session, 0);
	GitE2E::CheckOutFromContentBrowser(Session, 1);
	GitE2E::Do(Session, [Session]()
	{
		return Session->EditAndSave(Session->Assets[0], FGitE2ESession::EditedValue)
			&& Session->EditAndSave(Session->Assets[1], FGitE2ESession::EditedValue);
	});
	TSharedRef<UToolMenu*> Menu = MakeShared<UToolMenu*>(nullptr);
	GitE2E::Do(Session, [Session, Menu]() { *Menu = Session->OpenAssetRevisionControlMenu({ Session->Assets[0] }); return *Menu != nullptr; });
	GitE2E::Do(Session, [this, Menu]()
	{
		TestEqual(TEXT("The native Revert reads Revert Changes"), FGitE2ESession::GetLabel(FGitE2ESession::FindEntry(*Menu, "SCCRevert")), FString(TEXT("Revert Changes")));
		TestEqual(TEXT("Revert and Unlock is offered"), FGitE2ESession::GetLabel(FGitE2ESession::FindEntry(*Menu, "GitRevertAndUnlock")), FString(TEXT("Revert and Unlock")));
		return true;
	});

	// Act: right-click on the first asset > Revision Control > Revert and Unlock, then Yes.
	GitE2E::Do(Session, [Session]() { Session->AnswerNextDialog(EAppReturnType::Yes); return true; });
	GitE2E::Do(Session, [Session, Menu]() { return Session->Click(FGitE2ESession::FindEntry(*Menu, "GitRevertAndUnlock")); });

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the server releases the selected asset"), [Session]() { return Session->ServerLockOwner(Session->Assets[0]).IsEmpty(); });
	GitE2E::Do(Session, [this, Session]()
	{
		TestEqual(TEXT("The selected asset is restored"), Session->LoadedValue(Session->Assets[0]), FGitE2ESession::CommittedValue);
		TestEqual(TEXT("The other asset keeps its edit"), Session->LoadedValue(Session->Assets[1]), FGitE2ESession::EditedValue);
		TestEqual(TEXT("The other asset stays locked"), Session->ServerLockOwner(Session->Assets[1]), FString(TEXT("Designer")));
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_StackDownloadArrowOnLock_When_ColleaguePushedNewerVersion,
	"UEGitPlugin.E2E.Should_StackDownloadArrowOnLock_When_ColleaguePushedNewerVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_StackDownloadArrowOnLock_When_ColleaguePushedNewerVersion::RunTest(const FString& Parameters)
{
	// Arrange: the designer holds the lock, and a newer version reaches the base branch anyway.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitE2E::CheckOutFromContentBrowser(Session, 0);
	GitE2E::Do(Session, [Session]() { return Session->PublishColleagueVersion(Session->Assets[0], 7.0f); });

	// Act: Revision Control > Git > Refresh.
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the asset shows the download arrow on its lock"),
		[Session]() { return GitLockManagerE2ETests::HasOutdatedBadge(Session->GetState(Session->Assets[0])); });
	GitE2E::Do(Session, [this, Session]()
	{
		TestEqual(TEXT("The green pencil stays the main icon"), GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])), GitLockManagerE2ETests::LockSelfIcon);
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_DisableRevertAllAssets_When_OnlyConfigChanged,
	"UEGitPlugin.E2E.Should_DisableRevertAllAssets_When_OnlyConfigChanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_DisableRevertAllAssets_When_OnlyConfigChanged::RunTest(const FString& Parameters)
{
	// Arrange: no asset edited, only a tracked config file.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	const FString RelativeConfig = FString::Printf(TEXT("Config/E2E-%s.ini"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AbsoluteConfig = FPaths::ConvertRelativePathToFull(Session->Repository.RepositoryRoot, RelativeConfig);
	GitE2E::Start(Session, 0);
	GitE2E::Do(Session, [Session, RelativeConfig, AbsoluteConfig]()
	{
		return FFileHelper::SaveStringToFile(FString(TEXT("[E2E]\nValue=Committed\n")), *AbsoluteConfig)
			&& Session->Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeConfig))
			&& Session->Repository.RunGit(TEXT("commit --quiet -m \"Add config\""))
			&& FFileHelper::SaveStringToFile(FString(TEXT("[E2E]\nValue=Local\n")), *AbsoluteConfig);
	});

	// Act: Revision Control > Git > Refresh, then open the menu again.
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");
	GitE2E::WaitUntil(Session, TEXT("the editor sees the config edit"), [AbsoluteConfig]()
	{
		const FSourceControlStatePtr State = ISourceControlModule::Get().GetProvider().GetState(AbsoluteConfig, EStateCacheUsage::Use);
		return State.IsValid() && State->IsModified();
	});

	// Assert
	GitE2E::Do(Session, [this, Session, RelativeConfig]()
	{
		UToolMenu* Menu = Session->OpenRevisionControlMenu();
		TestFalse(TEXT("Revert All Assets is disabled"), FGitE2ESession::CanClick(FGitE2ESession::FindEntry(Menu, "GitRevertAllAssets")));
		TestFalse(TEXT("Revert and Unlock All Assets is disabled"), FGitE2ESession::CanClick(FGitE2ESession::FindEntry(Menu, "GitRevertAndUnlockAllAssets")));
		Session->Repository.RunGitQuietly(FString::Printf(TEXT("checkout -- \"%s\""), *RelativeConfig));
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_PrefillLfsUserNameWithoutSaving_When_DesignerOpensSettings,
	"UEGitPlugin.E2E.Should_PrefillLfsUserNameWithoutSaving_When_DesignerOpensSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_PrefillLfsUserNameWithoutSaving_When_DesignerOpensSettings::RunTest(const FString& Parameters)
{
	// Arrange: first configuration, no LFS username saved yet.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 0);

	// Act: the Connect to Revision Control dialog builds the plugin settings panel.
	GitE2E::Do(Session, [this, Session]()
	{
		FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
		const TSharedRef<SWidget> SettingsPanel = Provider.MakeSettingsWidget();
		TArray<TSharedRef<SEditableTextBox>> TextBoxes;
		GitLockManagerE2ETests::FindTextBoxes(SettingsPanel, TextBoxes);
		const FString ExpectedName = Provider.GetLockUser();

		// Assert
		TestFalse(TEXT("A name is pre-filled"), ExpectedName.IsEmpty());
		TestTrue(FString::Printf(TEXT("The LFS username field shows '%s'"), *ExpectedName), TextBoxes.ContainsByPredicate(
			[&ExpectedName](const TSharedRef<SEditableTextBox>& TextBox) { return TextBox->GetText().ToString() == ExpectedName; }));
		TestTrue(TEXT("Nothing is saved before the designer confirms"), FGitSourceControlModule::Get().AccessSettings().GetLfsUserName().IsEmpty());
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_BlockCheckOut_When_ColleaguePushedNewerVersion,
	"UEGitPlugin.E2E.Should_BlockCheckOut_When_ColleaguePushedNewerVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_BlockCheckOut_When_ColleaguePushedNewerVersion::RunTest(const FString& Parameters)
{
	// Arrange: a colleague pushed a newer version the designer has not pulled yet.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitE2E::Do(Session, [Session]() { return Session->PublishColleagueVersion(Session->Assets[0], 7.0f); });

	// Act: Revision Control > Git > Refresh.
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the asset shows the download arrow"),
		[Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])) == GitLockManagerE2ETests::OutdatedIcon; });
	GitE2E::Do(Session, [this, Session]()
	{
		const FSourceControlStatePtr State = Session->GetState(Session->Assets[0]);
		TestFalse(TEXT("Check Out is greyed out"), State.IsValid() && State->CanCheckout());
		TestTrue(TEXT("No lock was taken on the server"), Session->ServerLockOwner(Session->Assets[0]).IsEmpty());
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReopenRestoredMap_When_DesignerRevertsAllAssetsWithMapOpen,
	"UEGitPlugin.E2E.Should_ReopenRestoredMap_When_DesignerRevertsAllAssetsWithMapOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReopenRestoredMap_When_DesignerRevertsAllAssetsWithMapOpen::RunTest(const FString& Parameters)
{
	// Arrange: the designer has a level open, locks it, changes its world settings and saves.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 0);
	GitE2E::Do(Session, [Session]() { return Session->CreateCommittedOpenMap(); });
	GitE2E::CheckOutFromContentBrowser(Session, 0);
	TSharedRef<float> CommittedKillZ = MakeShared<float>(0.0f);
	GitE2E::Do(Session, [Session, CommittedKillZ]()
	{
		*CommittedKillZ = FGitE2ESession::GetOpenMapKillZ();
		return Session->EditOpenMapAndSave(Session->Assets[0], *CommittedKillZ - 1000.0f);
	});
	GitE2E::WaitUntil(Session, TEXT("Revert All Assets is enabled"),
		[Session]() { return FGitE2ESession::CanClick(FGitE2ESession::FindEntry(Session->OpenRevisionControlMenu(), "GitRevertAllAssets")); });

	// Act: Revision Control > Git > Revert All Assets, then OK.
	GitE2E::Do(Session, [Session]() { Session->AnswerNextDialog(EAppReturnType::Ok); return true; });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRevertAllAssets");

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the open level is back to its committed settings"),
		[CommittedKillZ]() { return FGitE2ESession::GetOpenMapKillZ() == *CommittedKillZ; });
	GitE2E::Do(Session, [this, Session]()
	{
		const UWorld* World = GEditor->GetEditorWorldContext().World();
		TestEqual(TEXT("The reverted level is still the open level"), World ? World->GetOutermost()->GetName() : FString(), Session->Assets[0].PackageName);
		TestEqual(TEXT("The server still holds the designer's lock"), Session->ServerLockOwner(Session->Assets[0]), FString(TEXT("Designer")));
		UEditorLoadingAndSavingUtils::NewBlankMap(false);
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_RefuseSaveAndKeepDisk_When_ColleagueLockedAsset,
	"UEGitPlugin.E2E.Should_RefuseSaveAndKeepDisk_When_ColleagueLockedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_RefuseSaveAndKeepDisk_When_ColleagueLockedAsset::RunTest(const FString& Parameters)
{
	// Arrange: a colleague locked the asset without changing it; the designer sees the red padlock.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	TSharedRef<bool> bSaved = MakeShared<bool>(true);
	GitE2E::Start(Session, 1);
	GitE2E::Do(Session, [Session]() { return Session->ColleagueLocks(Session->Assets[0]); });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");
	GitE2E::WaitUntil(Session, TEXT("the asset shows the colleague's lock"),
		[Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])) == GitLockManagerE2ETests::LockOtherIcon; });

	// Act: the designer edits the asset anyway, presses Ctrl+S and closes the Check Out Assets window.
	GitE2E::Do(Session, [Session, bSaved]()
	{
		Session->CloseNextCheckOutAssetsWindow();
		*bSaved = Session->SaveFromEditor(Session->Assets[0], FGitE2ESession::EditedValue);
		return true;
	});

	// Assert
	GitE2E::Do(Session, [this, Session, bSaved]()
	{
		const FGitE2EAsset& Asset = Session->Assets[0];
		const bool bDiskMatchesCommit = Session->Repository.RunGitQuietly(FString::Printf(TEXT("diff --quiet HEAD -- \"%s\""), *Asset.RelativeFile)) == 0;
		const FString Shown = FString::Join(Session->DialogMessages, TEXT(" | "));
		TestFalse(TEXT("The editor does not save the asset"), *bSaved);
		TestTrue(TEXT("The file on disk is the committed version"), bDiskMatchesCommit);
		// The editor's own text, resolved in the editor language: the Check Out Assets window warns that the asset is locked.
		const FString LockedWarning = NSLOCTEXT("PackagesDialogModule", "CheckoutPackagesWarnMessage", "Warning: There are modified assets which you will not be able to check out as they are locked or not at the head revision. You may lose your changes if you continue, as you will be unable to submit them to revision control.").ToString();
		TestTrue(FString::Printf(TEXT("The Check Out Assets window warns the asset is locked: '%s'"), *Shown), Shown.Contains(LockedWarning));
		TestEqual(TEXT("The colleague keeps the lock"), Session->ServerLockOwner(Asset), FString(TEXT("Colleague")));
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_CheckOutWithoutWarning_When_ColleagueReleasedLock,
	"UEGitPlugin.E2E.Should_CheckOutWithoutWarning_When_ColleagueReleasedLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_CheckOutWithoutWarning_When_ColleagueReleasedLock::RunTest(const FString& Parameters)
{
	// Arrange: the designer saw the colleague's lock, then the colleague released it.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 1);
	GitE2E::Do(Session, [Session]() { return Session->ColleagueLocks(Session->Assets[0]); });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");
	GitE2E::WaitUntil(Session, TEXT("the asset shows the colleague's lock"),
		[Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])) == GitLockManagerE2ETests::LockOtherIcon; });
	GitE2E::Do(Session, [Session]() { return Session->ColleagueUnlocks(Session->Assets[0]); });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");
	GitE2E::WaitUntil(Session, TEXT("the asset icon disappears"),
		[Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])).IsNone(); });

	// Act: right-click > Revision Control > Check Out.
	GitE2E::CheckOutFromContentBrowser(Session, 0);

	// Assert
	GitE2E::Do(Session, [this, Session]()
	{
		const FGitE2EAsset& Asset = Session->Assets[0];
		TestEqual(TEXT("No dialog interrupts the designer"), Session->DialogMessages.Num(), 0);
		TestEqual(TEXT("The server holds the designer's lock"), Session->ServerLockOwner(Asset), FString(TEXT("Designer")));
		TestEqual(TEXT("The asset shows the green pencil"), GitLockManagerE2ETests::IconOf(Session->GetState(Asset)), GitLockManagerE2ETests::LockSelfIcon);
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_CheckOutOtherAsset_When_ColleagueLocksSeveralAssets,
	"UEGitPlugin.E2E.Should_CheckOutOtherAsset_When_ColleagueLocksSeveralAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_CheckOutOtherAsset_When_ColleagueLocksSeveralAssets::RunTest(const FString& Parameters)
{
	// Arrange: the colleague holds two of three assets.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 3);
	GitE2E::Do(Session, [Session]() { return Session->ColleagueLocks(Session->Assets[0]) && Session->ColleagueLocks(Session->Assets[1]); });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");
	GitE2E::WaitUntil(Session, TEXT("both assets show the colleague's lock"), [Session]()
	{
		return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])) == GitLockManagerE2ETests::LockOtherIcon
			&& GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[1])) == GitLockManagerE2ETests::LockOtherIcon;
	});

	// Act: right-click on the third asset > Revision Control > Check Out.
	GitE2E::CheckOutFromContentBrowser(Session, 2);

	// Assert
	GitE2E::Do(Session, [this, Session]()
	{
		TestEqual(TEXT("The server holds the designer's lock on the third asset"), Session->ServerLockOwner(Session->Assets[2]), FString(TEXT("Designer")));
		TestEqual(TEXT("The colleague keeps the first asset"), Session->ServerLockOwner(Session->Assets[0]), FString(TEXT("Colleague")));
		TestEqual(TEXT("The colleague keeps the second asset"), Session->ServerLockOwner(Session->Assets[1]), FString(TEXT("Colleague")));
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_CheckOutAsset_When_ColleagueReleasesOneOfSeveralLocks,
	"UEGitPlugin.E2E.Should_CheckOutAsset_When_ColleagueReleasesOneOfSeveralLocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_CheckOutAsset_When_ColleagueReleasesOneOfSeveralLocks::RunTest(const FString& Parameters)
{
	// Arrange: the colleague holds two assets, so Check Out is greyed out; then they release the first one.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	GitE2E::Start(Session, 2);
	GitE2E::Do(Session, [Session]() { return Session->ColleagueLocks(Session->Assets[0]) && Session->ColleagueLocks(Session->Assets[1]); });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");
	GitE2E::WaitUntil(Session, TEXT("the first asset shows the colleague's lock"),
		[Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])) == GitLockManagerE2ETests::LockOtherIcon; });
	GitE2E::Do(Session, [this, Session]()
	{
		UToolMenu* Menu = Session->OpenAssetRevisionControlMenu({ Session->Assets[0] });
		TestFalse(TEXT("Check Out is greyed out while the colleague holds the asset"), FGitE2ESession::CanClick(FGitE2ESession::FindEntry(Menu, "SCCCheckOut")));
		return true;
	});
	GitE2E::Do(Session, [Session]() { return Session->ColleagueUnlocks(Session->Assets[0]); });
	GitLockManagerE2ETests::ClickGitMenu(Session, "GitRefresh");
	GitE2E::WaitUntil(Session, TEXT("the first asset icon disappears"),
		[Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])).IsNone(); });

	// Act
	GitE2E::CheckOutFromContentBrowser(Session, 0);

	// Assert
	GitE2E::Do(Session, [this, Session]()
	{
		TestEqual(TEXT("The server holds the designer's lock on the first asset"), Session->ServerLockOwner(Session->Assets[0]), FString(TEXT("Designer")));
		TestEqual(TEXT("The colleague keeps the second asset"), Session->ServerLockOwner(Session->Assets[1]), FString(TEXT("Colleague")));
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowColleagueLock_When_ColleagueLocksJustBeforeCheckOut,
	"UEGitPlugin.E2E.Should_ShowColleagueLock_When_ColleagueLocksJustBeforeCheckOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowColleagueLock_When_ColleagueLocksJustBeforeCheckOut::RunTest(const FString& Parameters)
{
	// Arrange: the designer opens the menu on a free asset; the colleague locks it before the click.
	const TSharedRef<FGitE2ESession> Session = MakeShared<FGitE2ESession>(*this);
	TSharedRef<UToolMenu*> Menu = MakeShared<UToolMenu*>(nullptr);
	// The refused lock is reported in the Revision Control log.
	AddExpectedError(TEXT("already created lock"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("locked by Colleague"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Command 'CheckOut' Failed!"), EAutomationExpectedErrorFlags::Contains, 0);
	GitE2E::Start(Session, 1);
	GitE2E::Do(Session, [Session, Menu]() { *Menu = Session->OpenAssetRevisionControlMenu({ Session->Assets[0] }); return *Menu != nullptr; });
	GitE2E::WaitUntil(Session, TEXT("Check Out is available in the asset menu"),
		[Menu]() { return FGitE2ESession::CanClick(FGitE2ESession::FindEntry(*Menu, "SCCCheckOut")); });
	GitE2E::Do(Session, [Session]() { return Session->ColleagueLocks(Session->Assets[0]); });

	// Act: the designer clicks Check Out and closes the plugin's message, then the editor's.
	GitE2E::Do(Session, [Session]() { Session->AnswerNextDialog(EAppReturnType::Ok); Session->AnswerNextDialog(EAppReturnType::Ok); return true; });
	GitE2E::Do(Session, [Session, Menu]() { return Session->Click(FGitE2ESession::FindEntry(*Menu, "SCCCheckOut")); });

	// Assert
	GitE2E::WaitUntil(Session, TEXT("the asset shows the colleague's lock"),
		[Session]() { return GitLockManagerE2ETests::IconOf(Session->GetState(Session->Assets[0])) == GitLockManagerE2ETests::LockOtherIcon; });
	GitE2E::Do(Session, [this, Session]()
	{
		const FSourceControlStatePtr State = Session->GetState(Session->Assets[0]);
		const FString Tooltip = State.IsValid() ? State->GetDisplayTooltip().ToString() : FString();
		const FString Shown = FString::Join(Session->DialogMessages, TEXT(" | "));
		TestTrue(FString::Printf(TEXT("The message names the colleague: '%s'"), *Shown), Shown.Contains(TEXT("locked by Colleague")));
		TestFalse(FString::Printf(TEXT("The message does not blame the connection: '%s'"), *Shown), Shown.Contains(TEXT("connection")));
		TestTrue(FString::Printf(TEXT("The hover names the colleague: '%s'"), *Tooltip), Tooltip.Contains(TEXT("Colleague")));
		TestEqual(TEXT("The colleague keeps the lock"), Session->ServerLockOwner(Session->Assets[0]), FString(TEXT("Colleague")));
		TestTrue(TEXT("The asset stays read-only"), FPlatformFileManager::Get().GetPlatformFile().IsReadOnly(*Session->Assets[0].AbsoluteFile));
		return true;
	});
	GitE2E::Finish(Session);
	return true;
}

#endif
