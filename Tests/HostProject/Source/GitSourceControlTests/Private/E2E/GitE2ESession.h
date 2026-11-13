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
		UCurveFloat* Curve = FindObject<UCurveFloat>(nullptr, *Asset.ObjectPath);
		if (!Curve || Curve->FloatCurve.GetNumKeys() == 0)
		{
			Test.AddError(FString::Printf(TEXT("Asset %s is not loaded"), *Asset.ObjectPath));
			return false;
		}
		Curve->FloatCurve.Keys[0].Value = Value;
		Curve->MarkPackageDirty();
		if (!UEditorLoadingAndSavingUtils::SavePackages({ Curve->GetPackage() }, true))
		{
			Test.AddError(FString::Printf(TEXT("The editor could not save %s"), *Asset.PackageName));
			return false;
		}
		// The Content Browser polls the status of visible assets; a headless editor has no visible asset to poll.
		ISourceControlModule::Get().QueueStatusUpdate(Asset.AbsoluteFile);
		return true;
	}

	/** Publishes on the base branch a colleague's version of the asset, a valid package holding another value. */
	bool PublishColleagueVersion(const FGitE2EAsset& Asset, float Value)
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
		bool bPublished = Save(Curve) && IFileManager::Get().Copy(*ColleagueFile, *Asset.AbsoluteFile) == COPY_OK;
		Curve->FloatCurve.Keys[0].Value = LocalValue;
		bPublished &= Save(Curve);
		PlatformFile.SetReadOnly(*Asset.AbsoluteFile, bWasReadOnly);
		FString Tree;
		FString Commit;
		bPublished = bPublished
			&& Repository.MakeRemoteTreeWithFile(TEXT("main"), Asset.RelativeFile, ColleagueFile, Tree)
			&& Repository.PublishTreeOnRemote(Tree, TEXT("main"), Commit);
		IFileManager::Get().Delete(*ColleagueFile);
		return bPublished;
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
		Repository.RunGitQuietly(TEXT("fetch --quiet origin main"));
		Repository.RunGitQuietly(TEXT("merge --quiet --ff-only origin/main"));
		FGitSourceControlModule::Get().AccessSettings().SetLfsUserName(SavedLfsUserName);
	}

	FAutomationTestBase& Test;
	FGitLockableAttributes Attributes;
	FGitTestRepository Repository;
	/** Assets created by the scenario, in creation order. */
	TArray<FGitE2EAsset> Assets;
	TArray<FString> DialogMessages;
	bool bFailed = false;

private:
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

	inline void Finish(const TSharedRef<FGitE2ESession>& Session)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Session]() { Session->TearDown(); return true; }));
	}
}
