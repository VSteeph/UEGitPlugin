#pragma once

#include "GitTestRepository.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "SourceControlOperations.h"

/** Lockable attributes must exist before connecting, otherwise the provider disables LFS locking. */
struct FGitLockableAttributes
{
	FString File;

	FGitLockableAttributes()
		: File(FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), TEXT(".gitattributes")))
	{
		FFileHelper::SaveStringToFile(FString(TEXT("*.uasset lockable\n*.umap lockable\n")), *File);
	}

	~FGitLockableAttributes()
	{
		IFileManager::Get().Delete(*File);
	}
};

/** A fake asset committed and pushed on the base branch, with the provider connected in LFS locking mode. */
struct FGitLockScenario
{
	static constexpr const TCHAR* BaseContent = TEXT("Base content\n");

	FGitLockableAttributes Attributes;
	FGitTestRepository Repository;
	FString RelativeAsset;
	FString AbsoluteAsset;
	FString OriginalRemoteUrl;

	explicit FGitLockScenario(FAutomationTestBase& Test)
		: Repository(Test)
	{
		// Outside Content/ so the asset registry never tries to load this fake package.
		RelativeAsset = FString::Printf(TEXT("Fixtures/Lock-%s.uasset"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		AbsoluteAsset = FPaths::ConvertRelativePathToFull(Repository.RepositoryRoot, RelativeAsset);
	}

	~FGitLockScenario()
	{
		if (!OriginalRemoteUrl.IsEmpty())
		{
			Repository.RunGitQuietly(FString::Printf(TEXT("remote set-url origin \"%s\""), *OriginalRemoteUrl));
		}
		Repository.ForceUnlockOnServer(RelativeAsset);
		Repository.RunGitQuietly(FString::Printf(TEXT("checkout -- \"%s\""), *RelativeAsset));
		Repository.RunGitQuietly(TEXT("fetch --quiet origin main"));
		Repository.RunGitQuietly(TEXT("merge --quiet --ff-only origin/main"));
	}

	bool Prepare()
	{
		return Repository.IsAvailable()
			&& FGitSourceControlModule::Get().GetProvider().UsesCheckout()
			&& FFileHelper::SaveStringToFile(FString(BaseContent), *AbsoluteAsset)
			&& Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeAsset))
			&& Repository.RunGit(TEXT("commit --quiet -m \"Add lockable asset\""))
			&& Repository.RunGit(TEXT("push --quiet origin main"));
	}

	/** Runs an operation on the asset and returns the errors it reported, or an empty array when it succeeded. */
	TArray<FString> Execute(const FSourceControlOperationRef& Operation) const
	{
		const ECommandResult::Type Result = FGitSourceControlModule::Get().GetProvider().Execute(Operation, { AbsoluteAsset }, EConcurrency::Synchronous);
		TArray<FString> Errors;
		if (Result != ECommandResult::Succeeded)
		{
			for (const FText& Error : Operation->GetResultInfo().ErrorMessages)
			{
				Errors.Add(Error.ToString());
			}
			if (Errors.IsEmpty())
			{
				Errors.Add(FString::Printf(TEXT("%s failed without message"), *Operation->GetName().ToString()));
			}
		}
		return Errors;
	}

	TArray<FString> CheckOutAsset() const
	{
		return Execute(ISourceControlOperation::Create<FCheckOut>());
	}

	/** Locks the asset through the plugin, then edits it on disk as a save from the editor would. */
	bool LockAndModifyAsset() const
	{
		return CheckOutAsset().IsEmpty()
			&& !FPlatformFileManager::Get().GetPlatformFile().IsReadOnly(*AbsoluteAsset)
			&& FFileHelper::SaveStringToFile(FString(TEXT("Local edit\n")), *AbsoluteAsset);
	}

	FString ReadAsset() const
	{
		FString Content;
		FFileHelper::LoadFileToString(Content, *AbsoluteAsset);
		return Content;
	}

	FSourceControlStatePtr GetCachedState() const
	{
		ISourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
		return Provider.GetState(AbsoluteAsset, EStateCacheUsage::Use);
	}

	static bool ContainsMessage(const TArray<FString>& Errors, const TCHAR* Message)
	{
		return Errors.ContainsByPredicate([Message](const FString& Error) { return Error.Contains(Message); });
	}
};
