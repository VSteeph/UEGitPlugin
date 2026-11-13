#pragma once

#include "../Integration/GitLockScenario.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserDataMenuContexts.h"
#include "ContentBrowserMenuContexts.h"
#include "Curves/CurveFloat.h"
#include "GameFramework/WorldSettings.h"
#include "FileHelpers.h"
#include "GitSourceControlSettings.h"
#include "HAL/PlatformTime.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SToolTip.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace GitE2EPrivateAccess
{
	// A headless editor renders no menu widget to click, and ToolMenus keeps an entry's action private:
	// explicit template instantiation is the standard way to name a private member without changing the engine.
	template <typename TTag, typename TTag::Type Member>
	struct TExposer
	{
		friend typename TTag::Type Get(TTag) { return Member; }
	};

	struct FEntryAction
	{
		using Type = FToolUIActionChoice FToolMenuEntry::*;
		friend Type Get(FEntryAction);
	};

	template struct TExposer<FEntryAction, &FToolMenuEntry::Action>;

	inline const FUIAction* GetUIAction(const FToolMenuEntry* Entry)
	{
		return Entry ? (Entry->*Get(FEntryAction())).GetUIAction() : nullptr;
	}
}

/** A real asset (a float curve package under Content/E2E) committed and pushed on the base branch. */
struct FGitE2EAsset
{
	FString PackageName;
	FString ObjectPath;
	FString RelativeFile;
	FString AbsoluteFile;
};

/**
 * Drives the editor like a designer: selects the provider, clicks menu entries and answers dialogs.
 * Arrange steps that happen outside Unreal (commits, a colleague's lock or push) go through Git, as Fork would.
 * Requires an attended editor (suite "e2e"): an unattended one auto-cancels dialogs before they reach the responder.
 */
class FGitE2ESession : public TSharedFromThis<FGitE2ESession>
{
public:
	static constexpr float CommittedValue = 1.0f;
	static constexpr float EditedValue = 5.0f;

	explicit FGitE2ESession(FAutomationTestBase& InTest)
		: Test(InTest)
		, Repository(InTest, true, false)
	{
		FGitSourceControlSettings& Settings = FGitSourceControlModule::Get().AccessSettings();
		SavedLfsUserName = Settings.GetLfsUserName();
		SavedUnlockMode = Settings.GetUnlockMode();
		Settings.SetLfsUserName(FString());
		PreviousDialogHandler = FCoreDelegates::ModalMessageDialog;
		FCoreDelegates::ModalMessageDialog.BindRaw(this, &FGitE2ESession::AnswerDialog);
	}

	~FGitE2ESession()
	{
		TearDown();
	}

	/** Selects "Git LFS 2" as the revision control provider, as the Connect to Revision Control dialog does. */
	bool Connect()
	{
		ISourceControlModule::Get().SetProvider(FName(TEXT("Git LFS 2")));
		bConnected = true;
		return ISourceControlModule::Get().GetProvider().GetName() == FName(TEXT("Git LFS 2"));
	}

	bool IsReady() const
	{
		const FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
		return Provider.IsEnabled() && Provider.IsAvailable() && Provider.UsesCheckout();
	}

	bool CreateCommittedAsset(const TCHAR* Prefix)
	{
		FGitE2EAsset& OutAsset = Assets.AddDefaulted_GetRef();
		const FString AssetName = FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		OutAsset.PackageName = FString::Printf(TEXT("/Game/E2E/%s"), *AssetName);
		OutAsset.ObjectPath = FString::Printf(TEXT("%s.%s"), *OutAsset.PackageName, *AssetName);
		OutAsset.AbsoluteFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(OutAsset.PackageName, FPackageName::GetAssetPackageExtension()));
		OutAsset.RelativeFile = OutAsset.AbsoluteFile;
		FPaths::MakePathRelativeTo(OutAsset.RelativeFile, *(Repository.RepositoryRoot / TEXT("")));

		UPackage* Package = CreatePackage(*OutAsset.PackageName);
		UCurveFloat* Curve = NewObject<UCurveFloat>(Package, *AssetName, RF_Public | RF_Standalone);
		Curve->FloatCurve.AddKey(0.0f, CommittedValue);
		FAssetRegistryModule::AssetCreated(Curve);
		return Save(Curve)
			&& Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *OutAsset.RelativeFile))
			&& Repository.RunGit(FString::Printf(TEXT("commit --quiet -m \"Add %s\""), *AssetName))
			&& Repository.RunGit(TEXT("push --quiet origin main"));
	}

	/** Creates a new level, opens it in the editor and commits it, as a designer starting a map. */
	bool CreateCommittedOpenMap()
	{
		FGitE2EAsset& OutMap = Assets.AddDefaulted_GetRef();
		const FString MapName = FString::Printf(TEXT("Map_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		OutMap.PackageName = FString::Printf(TEXT("/Game/E2E/%s"), *MapName);
		OutMap.ObjectPath = FString::Printf(TEXT("%s.%s"), *OutMap.PackageName, *MapName);
		OutMap.AbsoluteFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(OutMap.PackageName, FPackageName::GetMapPackageExtension()));
		OutMap.RelativeFile = OutMap.AbsoluteFile;
		FPaths::MakePathRelativeTo(OutMap.RelativeFile, *(Repository.RepositoryRoot / TEXT("")));

		UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
		return World
			&& UEditorLoadingAndSavingUtils::SaveMap(World, OutMap.PackageName)
			&& Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *OutMap.RelativeFile))
			&& Repository.RunGit(FString::Printf(TEXT("commit --quiet -m \"Add %s\""), *MapName))
			&& Repository.RunGit(TEXT("push --quiet origin main"));
	}

	/** KillZ of the level the editor has open: a world setting stored in the .umap itself, unlike actors saved one file per actor. */
	static float GetOpenMapKillZ()
	{
		const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
		return Settings ? Settings->KillZ : NAN;
	}

	/** Changes a world setting of the open level and saves it with Ctrl+S. */
	bool EditOpenMapAndSave(const FGitE2EAsset& Map, float KillZ)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
		if (!Settings || World->GetOutermost()->GetName() != Map.PackageName)
		{
			Test.AddError(FString::Printf(TEXT("The open level is not %s"), *Map.PackageName));
			return false;
		}
		Settings->Modify();
		Settings->KillZ = KillZ;
		World->GetOutermost()->MarkPackageDirty();
		if (!UEditorLoadingAndSavingUtils::SavePackages({ World->GetOutermost() }, true))
		{
			Test.AddError(FString::Printf(TEXT("The editor could not save %s"), *Map.PackageName));
			return false;
		}
		ISourceControlModule::Get().QueueStatusUpdate(Map.AbsoluteFile);
		return true;
	}

	/** Edits the asset in memory and saves it through the editor, as a designer does with Ctrl+S. */
	bool EditAndSave(const FGitE2EAsset& Asset, float Value)
	{
		if (!SaveFromEditor(Asset, Value))
		{
			Test.AddError(FString::Printf(TEXT("The editor could not save %s"), *Asset.PackageName));
			return false;
		}
		return true;
	}

	/** Edits the asset in memory and presses Ctrl+S; returns whether the editor saved it. */
	bool SaveFromEditor(const FGitE2EAsset& Asset, float Value)
	{
		UCurveFloat* Curve = FindObject<UCurveFloat>(nullptr, *Asset.ObjectPath);
		if (!Curve || Curve->FloatCurve.GetNumKeys() == 0)
		{
			Test.AddError(FString::Printf(TEXT("Asset %s is not loaded"), *Asset.ObjectPath));
			return false;
		}
		Curve->FloatCurve.Keys[0].Value = Value;
		Curve->MarkPackageDirty();
		// Ctrl+S comes from a person, like a click: windows it opens must reach the designer instead of being skipped.
		TGuardValue<bool> AttendedSave(GIsAutomationTesting, false);
		// Same call as Ctrl+S in the asset editor, which asks to check out a read-only file before saving.
		const bool bSaved = FEditorFileUtils::PromptForCheckoutAndSave({ Curve->GetPackage() }, false, false) == FEditorFileUtils::PR_Success;
		// The Content Browser polls the status of visible assets; a headless editor has no visible asset to poll.
		ISourceControlModule::Get().QueueStatusUpdate(Asset.AbsoluteFile);
		return bSaved;
	}

	/** The designer closes the next "Check Out Assets" window the editor opens, after reading what it lists. */
	void CloseNextCheckOutAssetsWindow()
	{
		// The window is modal: only Slate keeps ticking while it is open, so the designer acts from a Slate tick.
		CheckOutWindowTicks = 0;
		CheckOutWindowHandle = FSlateApplication::Get().OnPreTick().AddLambda([this](float)
		{
			const TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveModalWindow();
			// The title follows the editor language: the window is recognised by the dialog it hosts.
			if (!Window.IsValid() || !ContainsWidgetOfType(Window.ToSharedRef(), FName(TEXT("SPackagesDialog"))))
			{
				return;
			}
			// Leaves the window a few frames to build its rows before reading them.
			if (++CheckOutWindowTicks < 5)
			{
				return;
			}
			TArray<FString> Texts;
			CollectTexts(Window.ToSharedRef(), Texts);
			DialogMessages.Add(FString::Printf(TEXT("%s: %s"), *Window->GetTitle().ToString(), *FString::Join(Texts, TEXT(" | "))));
			FSlateApplication::Get().OnPreTick().Remove(CheckOutWindowHandle);
			CheckOutWindowHandle.Reset();
			Window->RequestDestroyWindow();
		});
	}

	/** Builds, from the designer's copy, the tree of the base branch where the asset holds a colleague's value. */
	bool MakeColleagueTree(const FGitE2EAsset& Asset, float Value, FString& OutTree)
	{
		UCurveFloat* Curve = FindObject<UCurveFloat>(nullptr, *Asset.ObjectPath);
		if (!Curve || Curve->FloatCurve.GetNumKeys() == 0)
		{
			Test.AddError(FString::Printf(TEXT("Asset %s is not loaded"), *Asset.ObjectPath));
			return false;
		}
		// The colleague saves on their machine: the local file may be read-only while this designer has no lock.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const bool bWasReadOnly = PlatformFile.IsReadOnly(*Asset.AbsoluteFile);
		PlatformFile.SetReadOnly(*Asset.AbsoluteFile, false);
		const float LocalValue = Curve->FloatCurve.Keys[0].Value;
		const FString ColleagueFile = FPaths::ConvertRelativePathToFull(FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("Colleague"), TEXT(".uasset")));
		Curve->FloatCurve.Keys[0].Value = Value;
		bool bMade = Save(Curve) && IFileManager::Get().Copy(*ColleagueFile, *Asset.AbsoluteFile) == COPY_OK;
		Curve->FloatCurve.Keys[0].Value = LocalValue;
		bMade &= Save(Curve);
		PlatformFile.SetReadOnly(*Asset.AbsoluteFile, bWasReadOnly);
		bMade = bMade && Repository.MakeRemoteTreeWithFile(TEXT("main"), Asset.RelativeFile, ColleagueFile, OutTree);
		IFileManager::Get().Delete(*ColleagueFile);
		return bMade;
	}

	/** Publishes on the base branch a colleague's version of the asset, a valid package holding another value. */
	bool PublishColleagueVersion(const FGitE2EAsset& Asset, float Value)
	{
		FString Tree;
		FString Commit;
		return MakeColleagueTree(Asset, Value, Tree)
			&& Repository.PublishTreeOnRemote(Tree, TEXT("main"), Commit);
	}

	/** A colleague's version of the asset reaches the given server branch from their machine; this clone has not fetched it. */
	bool PublishColleagueVersionOnServer(const FGitE2EAsset& Asset, float Value, const FString& Branch)
	{
		FString Tree;
		FString Commit;
		if (!MakeColleagueTree(Asset, Value, Tree)
			|| !Repository.RunGit(FString::Printf(TEXT("commit-tree %s -p origin/main -m \"Colleague change\""), *Tree), Commit)
			// A ref outside refs/heads carries the objects without moving this clone's remote-tracking branches.
			|| !Repository.RunGit(FString::Printf(TEXT("push --quiet --force origin %s:refs/e2e/colleague"), *Commit))
			|| !RunGitOnServer(FString::Printf(TEXT("update-ref refs/heads/%s %s"), *Branch, *Commit)))
		{
			return false;
		}
		ServerBranches.AddUnique(TEXT("refs/e2e/colleague"));
		if (Branch != TEXT("main"))
		{
			ServerBranches.AddUnique(Branch);
		}
		ExpectedServerCommits.Add(Branch, Commit);
		return true;
	}

	/** Creates the designer's branch and pushes it, as Fork does before the first commit. */
	bool StartFeatureBranch()
	{
		FeatureBranch = FString::Printf(TEXT("feature/e2e-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		ServerBranches.AddUnique(FeatureBranch);
		return Repository.RunGit(FString::Printf(TEXT("checkout --quiet -b %s"), *FeatureBranch))
			&& Repository.RunGit(FString::Printf(TEXT("push --quiet --set-upstream origin %s"), *FeatureBranch));
	}

	/** Commits the saved asset on the designer's branch and pushes it, as the designer does in Fork. */
	bool CommitAndPush(const FGitE2EAsset& Asset)
	{
		if (FeatureBranch.IsEmpty())
		{
			FeatureBranch = FString::Printf(TEXT("feature/e2e-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			ServerBranches.AddUnique(FeatureBranch);
			if (!Repository.RunGit(FString::Printf(TEXT("checkout --quiet -b %s"), *FeatureBranch)))
			{
				return false;
			}
		}
		return Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *Asset.RelativeFile))
			&& Repository.RunGit(TEXT("commit --quiet -m \"Edit asset\""))
			&& Repository.RunGit(FString::Printf(TEXT("push --quiet --set-upstream origin %s"), *FeatureBranch));
	}

	enum class EMergeMethod { Merge, Squash, Rebase };

	/** Merges the designer's pull request into main on the server, as the Git host does; the local clone is untouched. */
	bool MergePullRequestOnServer(EMergeMethod Method)
	{
		FString Base;
		if (!RunGitOnServer(TEXT("rev-parse main"), Base))
		{
			return false;
		}
		if (Method == EMergeMethod::Rebase)
		{
			// The base branch has not moved since the branch started: each commit replays with its own tree.
			FString Commits;
			if (!RunGitOnServer(FString::Printf(TEXT("rev-list --reverse main..%s"), *FeatureBranch), Commits))
			{
				return false;
			}
			TArray<FString> CommitList;
			Commits.ParseIntoArrayLines(CommitList);
			for (const FString& Commit : CommitList)
			{
				if (!RunGitOnServer(FString::Printf(TEXT("-c user.name=Host -c user.email=host@example.invalid commit-tree %s^{tree} -p %s -m \"Rebased commit\""), *Commit, *Base), Base))
				{
					return false;
				}
			}
		}
		else
		{
			const FString Parents = Method == EMergeMethod::Squash ? FString::Printf(TEXT("-p %s"), *Base) : FString::Printf(TEXT("-p %s -p %s"), *Base, *FeatureBranch);
			if (!RunGitOnServer(FString::Printf(TEXT("-c user.name=Host -c user.email=host@example.invalid commit-tree %s^{tree} %s -m \"Merge pull request\""), *FeatureBranch, *Parents), Base))
			{
				return false;
			}
		}
		ExpectedServerCommits.Add(TEXT("main"), Base);
		return RunGitOnServer(FString::Printf(TEXT("update-ref refs/heads/main %s"), *Base));
	}

	/** Fetch from the designer's Git client, with the editor not involved. */
	bool FetchFromGitClient()
	{
		return Repository.RunGit(TEXT("fetch --quiet origin"));
	}

	/** What Fork shows once a fetch received the server's latest commit on the branch. */
	bool HasFetched(const FString& Branch) const
	{
		const FString* Expected = ExpectedServerCommits.Find(Branch);
		FString Tracking;
		return Expected
			&& Repository.RunGitQuietly(FString::Printf(TEXT("rev-parse --verify --quiet origin/%s"), *Branch)) == 0
			&& Repository.RunGit(FString::Printf(TEXT("rev-parse origin/%s"), *Branch), Tracking)
			&& Tracking == *Expected;
	}

	bool ColleagueLocks(const FGitE2EAsset& Asset) const
	{
		return Repository.RunGitAs(TEXT("Colleague"), FString::Printf(TEXT("lfs lock \"%s\""), *Asset.RelativeFile));
	}

	bool ColleagueUnlocks(const FGitE2EAsset& Asset) const
	{
		return Repository.RunGitAs(TEXT("Colleague"), FString::Printf(TEXT("lfs unlock \"%s\""), *Asset.RelativeFile));
	}

	/** Picks an Unlock Mode in the plugin settings panel with the keyboard: arrows on the closed combo box. */
	bool SelectUnlockMode(EGitUnlockMode Mode)
	{
		const TSharedRef<SWidget> Panel = FGitSourceControlModule::Get().GetProvider().MakeSettingsWidget();
		const TSharedPtr<SComboBox<TSharedPtr<EGitUnlockMode>>> ComboBox = FindUnlockModeComboBox(Panel);
		if (!ComboBox.IsValid())
		{
			Test.AddError(TEXT("The settings panel has no Unlock Mode combo box"));
			return false;
		}
		// Slate dispatches key presses through the public SWidget interface; SComboBox narrows the override to protected.
		const TSharedRef<SWidget> FocusedWidget = ComboBox.ToSharedRef();
		const auto PressKey = [&FocusedWidget](const FKey& Key)
		{
			FocusedWidget->OnKeyDown(FGeometry(), FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0));
		};
		const auto IsSelected = [&ComboBox, Mode]()
		{
			const TSharedPtr<EGitUnlockMode> Selected = ComboBox->GetSelectedItem();
			return Selected.IsValid() && *Selected == Mode;
		};
		for (int32 Press = 0; Press < 3; ++Press)
		{
			PressKey(EKeys::Up);
		}
		for (int32 Press = 0; Press < 3 && !IsSelected(); ++Press)
		{
			PressKey(EKeys::Down);
		}
		if (!IsSelected())
		{
			Test.AddError(TEXT("The Unlock Mode combo box never reached the requested mode"));
			return false;
		}
		return true;
	}

	/** Value of the asset the editor currently holds in memory, NaN when the asset is not loaded. */
	float LoadedValue(const FGitE2EAsset& Asset) const
	{
		const UCurveFloat* Curve = FindObject<UCurveFloat>(nullptr, *Asset.ObjectPath);
		return Curve && Curve->FloatCurve.GetNumKeys() > 0 ? Curve->FloatCurve.Keys[0].Value : NAN;
	}

	FSourceControlStatePtr GetState(const FGitE2EAsset& Asset) const
	{
		return ISourceControlModule::Get().GetProvider().GetState(Asset.AbsoluteFile, EStateCacheUsage::Use);
	}

	FString ServerLockOwner(const FGitE2EAsset& Asset) const
	{
		return Repository.GetServerLockOwner(Asset.RelativeFile);
	}

	/** The Revision Control menu of the status bar, where the plugin adds its "Git" section. */
	UToolMenu* OpenRevisionControlMenu()
	{
		// Its entries are bound to the plugin, not to the generated menu: no need to keep it alive past the current frame.
		return UToolMenus::Get()->GenerateMenu("StatusBar.ToolBar.SourceControl", FToolMenuContext());
	}

	/** The Revision Control sub-menu of a right-click on the given assets in the Content Browser. */
	UToolMenu* OpenAssetRevisionControlMenu(const TArray<FGitE2EAsset>& Selection)
	{
		UContentBrowserAssetContextMenuContext* Context = NewObject<UContentBrowserAssetContextMenuContext>();
		Context->bCanBeModified = true;
		for (const FGitE2EAsset& Asset : Selection)
		{
			if (UObject* Object = FindObject<UObject>(nullptr, *Asset.ObjectPath))
			{
				Context->SelectedAssets.Add(FAssetData(Object));
			}
		}
		// The Content Browser always adds a file-menu context; engine extensions dereference it unchecked. Its items only feed
		// plugin-content actions, so it stays empty here.
		UContentBrowserDataMenuContext_FileMenu* FileContext = NewObject<UContentBrowserDataMenuContext_FileMenu>();
		FToolMenuContext MenuContext(Context);
		MenuContext.AddObject(FileContext);
		UToolMenu* ContextMenu = KeepAlive(UToolMenus::Get()->GenerateMenu("ContentBrowser.AssetContextMenu", MenuContext));
		return ContextMenu ? KeepAlive(UToolMenus::Get()->GenerateSubMenu(ContextMenu, "SourceControlSubMenu")) : nullptr;
	}

	static const FToolMenuEntry* FindEntry(UToolMenu* Menu, const FName EntryName)
	{
		if (!Menu)
		{
			return nullptr;
		}
		for (FToolMenuSection& Section : Menu->Sections)
		{
			if (const FToolMenuEntry* Entry = Section.FindEntry(EntryName))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	static FString GetLabel(const FToolMenuEntry* Entry)
	{
		return Entry ? Entry->Label.Get().ToString() : FString();
	}

	static bool CanClick(const FToolMenuEntry* Entry)
	{
		const FUIAction* Action = GitE2EPrivateAccess::GetUIAction(Entry);
		return Action && Action->CanExecute();
	}

	bool Click(const FToolMenuEntry* Entry)
	{
		const FUIAction* Action = GitE2EPrivateAccess::GetUIAction(Entry);
		if (!Action || !Action->CanExecute())
		{
			Test.AddError(FString::Printf(TEXT("Menu entry '%s' cannot be clicked"), *GetLabel(Entry)));
			return false;
		}
		// FApp::IsUnattended() is true for the whole automation run, which makes every dialog return its default answer
		// silently. A click comes from a person: dialogs it opens must reach the responder, which also fails unexpected ones.
		TGuardValue<bool> AttendedClick(GIsAutomationTesting, false);
		return Action->Execute();
	}

	/** What a designer would inspect when a step never completes: disk, editor state, server lock and dialogs seen. */
	FString Describe() const
	{
		FString Description = FString::Printf(TEXT("Dialogs: [%s]"), *FString::Join(DialogMessages, TEXT(" | ")));
		for (const FGitE2EAsset& Asset : Assets)
		{
			const FSourceControlStatePtr State = GetState(Asset);
			const bool bDiskMatchesCommit = Repository.RunGitQuietly(FString::Printf(TEXT("diff --quiet HEAD -- \"%s\""), *Asset.RelativeFile)) == 0;
			Description += FString::Printf(TEXT("\n%s: loaded=%f diskMatchesCommit=%d modified=%d checkedOut=%d server='%s' tooltip='%s'"),
				*Asset.PackageName, LoadedValue(Asset), bDiskMatchesCommit,
				State.IsValid() && State->IsModified(), State.IsValid() && State->IsCheckedOut(), *ServerLockOwner(Asset),
				State.IsValid() ? *State->GetDisplayTooltip().ToString() : TEXT("none"));
		}
		return Description;
	}

	/** Queues the button the designer presses on the next dialog; any other dialog fails the test. */
	void AnswerNextDialog(EAppReturnType::Type Answer)
	{
		DialogAnswers.Add(Answer);
	}

	/** Restores the editor as the test found it; also releases every lock the scenario took on the server. */
	void TearDown()
	{
		if (bTornDown)
		{
			return;
		}
		bTornDown = true;
		FCoreDelegates::ModalMessageDialog = PreviousDialogHandler;
		if (bConnected)
		{
			ISourceControlModule::Get().SetProvider(FName(TEXT("None")));
		}
		for (const FGitE2EAsset& Asset : Assets)
		{
			Repository.ForceUnlockOnServer(Asset.RelativeFile);
			Repository.RunGitQuietly(FString::Printf(TEXT("checkout -- \"%s\""), *Asset.RelativeFile));
		}
		if (CheckOutWindowHandle.IsValid())
		{
			FSlateApplication::Get().OnPreTick().Remove(CheckOutWindowHandle);
		}
		if (!FeatureBranch.IsEmpty())
		{
			Repository.RunGitQuietly(TEXT("checkout --quiet main"));
			Repository.RunGitQuietly(FString::Printf(TEXT("branch --quiet -D %s"), *FeatureBranch));
		}
		for (const FString& Branch : ServerBranches)
		{
			Repository.RunGitQuietly(FString::Printf(TEXT("push --quiet origin --delete %s"), *Branch));
		}
		Repository.RunGitQuietly(TEXT("fetch --quiet origin main"));
		Repository.RunGitQuietly(TEXT("merge --quiet --ff-only origin/main"));
		FGitSourceControlSettings& Settings = FGitSourceControlModule::Get().AccessSettings();
		Settings.SetLfsUserName(SavedLfsUserName);
		if (Settings.SetUnlockMode(SavedUnlockMode))
		{
			Settings.SaveSettings();
			FGitSourceControlModule::Get().GetProvider().SyncUnlockHook();
		}
	}

	FAutomationTestBase& Test;
	FGitLockableAttributes Attributes;
	FGitTestRepository Repository;
	/** Assets created by the scenario, in creation order. */
	TArray<FGitE2EAsset> Assets;
	TArray<FString> DialogMessages;
	bool bFailed = false;

private:
	static TSharedPtr<SComboBox<TSharedPtr<EGitUnlockMode>>> FindUnlockModeComboBox(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == FName(TEXT("SComboBox<TSharedPtr<EGitUnlockMode>>")))
		{
			return StaticCastSharedRef<SComboBox<TSharedPtr<EGitUnlockMode>>>(Widget);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SComboBox<TSharedPtr<EGitUnlockMode>>> Found = FindUnlockModeComboBox(Children->GetChildAt(Index)))
			{
				return Found;
			}
		}
		return nullptr;
	}

	static bool ContainsWidgetOfType(const TSharedRef<SWidget>& Widget, const FName Type)
	{
		if (Widget->GetType() == Type)
		{
			return true;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (ContainsWidgetOfType(Children->GetChildAt(Index), Type))
			{
				return true;
			}
		}
		return false;
	}

	/** Every label and hover text of a widget tree, depth first: what the designer can read in a window. */
	static void CollectTexts(const TSharedRef<SWidget>& Widget, TArray<FString>& OutTexts)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			OutTexts.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		const TSharedPtr<IToolTip> ToolTip = Widget->GetToolTip();
		if (ToolTip.IsValid() && ToolTip->AsWidget()->GetType() == FName(TEXT("SToolTip")))
		{
			OutTexts.Add(StaticCastSharedRef<SToolTip>(ToolTip->AsWidget())->GetTextTooltip().ToString());
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			CollectTexts(Children->GetChildAt(Index), OutTexts);
		}
	}

	/** Runs git in the bare remote, standing for the Git host. */
	bool RunGitOnServer(const FString& Arguments, FString& Output) const
	{
		FString RemoteDirectory;
		return Repository.RunGit(TEXT("remote get-url origin"), RemoteDirectory)
			&& Repository.RunGit(Arguments, Output, RemoteDirectory);
	}

	bool RunGitOnServer(const FString& Arguments) const
	{
		FString Output;
		return RunGitOnServer(Arguments, Output);
	}

	bool Save(UCurveFloat* Curve)
	{
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString Filename = FPackageName::LongPackageNameToFilename(Curve->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Curve->GetPackage(), Curve, *Filename, Args))
		{
			Test.AddError(FString::Printf(TEXT("Saving %s failed"), *Filename));
			return false;
		}
		return true;
	}

	UToolMenu* KeepAlive(UToolMenu* Menu)
	{
		if (Menu)
		{
			Menus.Emplace(Menu);
		}
		return Menu;
	}

	EAppReturnType::Type AnswerDialog(EAppMsgCategory Category, EAppMsgType::Type Type, const FText& Message, const FText& Title)
	{
		DialogMessages.Add(Message.ToString());
		if (DialogAnswers.IsEmpty())
		{
			Test.AddError(FString::Printf(TEXT("Unexpected dialog: %s"), *Message.ToString()));
			return EAppReturnType::Cancel;
		}
		const EAppReturnType::Type Answer = DialogAnswers[0];
		DialogAnswers.RemoveAt(0);
		return Answer;
	}

	TArray<TStrongObjectPtr<UToolMenu>> Menus;
	TArray<EAppReturnType::Type> DialogAnswers;
	TDelegate<EAppReturnType::Type(EAppMsgCategory, EAppMsgType::Type, const FText&, const FText&)> PreviousDialogHandler;
	FString SavedLfsUserName;
	EGitUnlockMode SavedUnlockMode = EGitUnlockMode::OnMerge;
	FString FeatureBranch;
	/** Branches this scenario created on the server, deleted on tear down. */
	TArray<FString> ServerBranches;
	/** Latest commit the server holds on each branch the scenario published to. */
	TMap<FString, FString> ExpectedServerCommits;
	FDelegateHandle CheckOutWindowHandle;
	int32 CheckOutWindowTicks = 0;
	bool bConnected = false;
	bool bTornDown = false;
};

/** Latent steps run one per editor frame, so asynchronous operations and the provider tick progress between them. */
namespace GitE2E
{
	inline void Do(const TSharedRef<FGitE2ESession>& Session, TFunction<bool()> Action)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Session, Action]()
		{
			if (!Session->bFailed && !Action())
			{
				Session->bFailed = true;
			}
			return true;
		}));
	}

	inline void WaitUntil(const TSharedRef<FGitE2ESession>& Session, const FString& What, TFunction<bool()> Condition, double TimeoutSeconds = 60.0)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Session, What, Condition, TimeoutSeconds, StartTime = 0.0]() mutable
		{
			if (Session->bFailed || Condition())
			{
				return true;
			}
			const double Now = FPlatformTime::Seconds();
			StartTime = StartTime > 0.0 ? StartTime : Now;
			if (Now - StartTime > TimeoutSeconds)
			{
				Session->Test.AddError(FString::Printf(TEXT("Timed out waiting until %s\n%s"), *What, *Session->Describe()));
				Session->bFailed = true;
				return true;
			}
			return false;
		}));
	}

	/** Fails as soon as the condition stops holding during the given time, as a designer watching it would notice. */
	inline void KeepsHolding(const TSharedRef<FGitE2ESession>& Session, const FString& What, TFunction<bool()> Condition, double Seconds = 10.0)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Session, What, Condition, Seconds, StartTime = 0.0]() mutable
		{
			if (Session->bFailed)
			{
				return true;
			}
			if (!Condition())
			{
				Session->Test.AddError(FString::Printf(TEXT("Stopped holding: %s\n%s"), *What, *Session->Describe()));
				Session->bFailed = true;
				return true;
			}
			const double Now = FPlatformTime::Seconds();
			StartTime = StartTime > 0.0 ? StartTime : Now;
			return Now - StartTime > Seconds;
		}));
	}

	/** Connects the provider like a designer, then commits and pushes the given number of fresh assets. */
	inline void Start(const TSharedRef<FGitE2ESession>& Session, int32 AssetCount)
	{
		Do(Session, [Session]() { return Session->Connect(); });
		WaitUntil(Session, TEXT("the Git LFS 2 provider is connected with LFS locking"), [Session]() { return Session->IsReady(); });
		Do(Session, [Session, AssetCount]()
		{
			bool bCreated = true;
			for (int32 Index = 0; Index < AssetCount && bCreated; ++Index)
			{
				bCreated = Session->CreateCommittedAsset(TEXT("Curve"));
			}
			return bCreated;
		});
	}

	/** Right-click > Revision Control > Check Out on one asset, then waits for the plugin to show it as checked out. */
	inline void CheckOutFromContentBrowser(const TSharedRef<FGitE2ESession>& Session, int32 AssetIndex)
	{
		TSharedRef<UToolMenu*> Menu = MakeShared<UToolMenu*>(nullptr);
		Do(Session, [Session, AssetIndex, Menu]() { *Menu = Session->OpenAssetRevisionControlMenu({ Session->Assets[AssetIndex] }); return *Menu != nullptr; });
		WaitUntil(Session, TEXT("Check Out is available in the asset menu"),
			[Menu]() { return FGitE2ESession::CanClick(FGitE2ESession::FindEntry(*Menu, "SCCCheckOut")); });
		Do(Session, [Session, Menu]() { return Session->Click(FGitE2ESession::FindEntry(*Menu, "SCCCheckOut")); });
		WaitUntil(Session, TEXT("the asset is checked out"), [Session, AssetIndex]()
		{
			const FSourceControlStatePtr State = Session->GetState(Session->Assets[AssetIndex]);
			return State.IsValid() && State->IsCheckedOut();
		});
	}

	/** Revision Control > Git > Refresh. */
	inline void Refresh(const TSharedRef<FGitE2ESession>& Session)
	{
		Do(Session, [Session]()
		{
			return Session->Click(FGitE2ESession::FindEntry(Session->OpenRevisionControlMenu(), "GitRefresh"));
		});
	}

	inline FName IconOf(const FSourceControlStatePtr& State)
	{
		return State.IsValid() ? State->GetIcon().GetStyleName() : NAME_None;
	}

	inline void Finish(const TSharedRef<FGitE2ESession>& Session)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Session]() { Session->TearDown(); return true; }));
	}
}
