#include "GitTestRepository.h"

#include "Curves/CurveFloat.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "SourceControlOperations.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_NotCreateCommit_When_SubmitRequested,
	"UEGitPlugin.Integration.Should_NotCreateCommit_When_SubmitRequested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_NotCreateCommit_When_SubmitRequested::RunTest(const FString& Parameters)
{
	// Arrange: a staged file that a submit would commit.
	const FGitTestRepository Repository(*this);
	const FString RelativeFile = FString::Printf(TEXT("Fixtures/Submit-%s.txt"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(Repository.RepositoryRoot, RelativeFile);
	ON_SCOPE_EXIT
	{
		Repository.RunGitQuietly(TEXT("fetch --quiet origin main"));
		Repository.RunGitQuietly(TEXT("reset --quiet --hard origin/main"));
		IFileManager::Get().Delete(*AbsoluteFile);
	};

	FString PreviousCommit;
	if (!TestTrue(TEXT("The isolated Git repository is available"), Repository.IsAvailable())
		|| !FFileHelper::SaveStringToFile(FString(TEXT("Submit fixture\n")), *AbsoluteFile)
		|| !Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeFile))
		|| !Repository.RunGit(TEXT("rev-parse HEAD"), PreviousCommit))
	{
		return false;
	}
	// The editor language may be localized: only the operation name is stable in the message.
	AddExpectedError(TEXT("'CheckIn'"), EAutomationExpectedErrorFlags::Contains, 0);

	// Act
	const TSharedRef<FCheckIn> Operation = ISourceControlOperation::Create<FCheckIn>();
	Operation->SetDescription(FText::FromString(TEXT("Submit from the editor")));
	FGitSourceControlModule::Get().GetProvider().Execute(Operation, { AbsoluteFile }, EConcurrency::Synchronous);

	// Assert
	FString CurrentCommit;
	Repository.RunGit(TEXT("rev-parse HEAD"), CurrentCommit);
	TestEqual(TEXT("No commit is created"), CurrentCommit, PreviousCommit);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_DeleteFileWithoutStaging_When_EditorDeletesCommittedAsset,
	"UEGitPlugin.Integration.Should_DeleteFileWithoutStaging_When_EditorDeletesCommittedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_DeleteFileWithoutStaging_When_EditorDeletesCommittedAsset::RunTest(const FString& Parameters)
{
	// Arrange: a committed asset, read-only on disk as an unlocked asset is.
	const FGitTestRepository Repository(*this);
	const FString RelativeFile = FString::Printf(TEXT("Fixtures/Delete-%s.uasset"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(Repository.RepositoryRoot, RelativeFile);
	ON_SCOPE_EXIT
	{
		Repository.RunGitQuietly(TEXT("fetch --quiet origin main"));
		Repository.RunGitQuietly(TEXT("reset --quiet --hard origin/main"));
		IFileManager::Get().Delete(*AbsoluteFile, false, true);
	};
	if (!TestTrue(TEXT("The isolated Git repository is available"), Repository.IsAvailable())
		|| !FFileHelper::SaveStringToFile(FString(TEXT("Committed\n")), *AbsoluteFile)
		|| !Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeFile))
		|| !Repository.RunGit(TEXT("commit --quiet -m \"Add asset to delete\""))
		|| !FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*AbsoluteFile, true))
	{
		return false;
	}

	// Act: the Content Browser deletes a committed, up-to-date asset through the provider.
	const ECommandResult::Type Result = FGitSourceControlModule::Get().GetProvider().Execute(
		ISourceControlOperation::Create<FDelete>(), { AbsoluteFile }, EConcurrency::Synchronous);

	// Assert
	TestEqual(TEXT("The delete succeeds"), Result, ECommandResult::Succeeded);
	TestFalse(TEXT("The file is gone from disk"), FPaths::FileExists(AbsoluteFile));
	TestEqual(TEXT("Nothing is staged"), Repository.RunGitQuietly(TEXT("diff --cached --quiet")), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_NotStageAsset_When_PackageSaved,
	"UEGitPlugin.Integration.Should_NotStageAsset_When_PackageSaved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_NotStageAsset_When_PackageSaved::RunTest(const FString& Parameters)
{
	// Arrange: a staged asset, known as staged by the provider, then modified in the editor.
	const FGitTestRepository Repository(*this);
	const FString PackageName = FString::Printf(TEXT("/Game/Fixtures/StageOnSave_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
	UPackage* Package = CreatePackage(*PackageName);
	UCurveFloat* Curve = NewObject<UCurveFloat>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
	ON_SCOPE_EXIT
	{
		Repository.RunGitQuietly(FString::Printf(TEXT("rm --quiet --cached --force -- \"%s\""), *AbsoluteFile));
		Curve->ClearFlags(RF_Public | RF_Standalone);
		Curve->MarkAsGarbage();
		Package->MarkAsGarbage();
		IFileManager::Get().Delete(*AbsoluteFile);
	};

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("The isolated Git repository is available"), Repository.IsAvailable())
		|| !TestTrue(TEXT("The asset is saved"), UPackage::SavePackage(Package, Curve, *AbsoluteFile, SaveArgs))
		|| !Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *AbsoluteFile)))
	{
		return false;
	}
	// Initialization is where the provider subscribes to editor events such as package saves.
	ISourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	Provider.Init();
	// The editor refreshes the pending changelists periodically; staging on save used to apply to files it found staged.
	AddExpectedError(TEXT("'UpdateChangelistsStatus'"), EAutomationExpectedErrorFlags::Contains, 0);
	Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), { AbsoluteFile }, EConcurrency::Synchronous);
	Provider.Execute(ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>(), EConcurrency::Synchronous);
	Curve->FloatCurve.AddKey(1.0f, 2.0f);

	// Act
	UPackage::SavePackage(Package, Curve, *AbsoluteFile, SaveArgs);

	// Assert
	TestEqual(TEXT("The saved change stays unstaged"), Repository.RunGitQuietly(FString::Printf(TEXT("diff --quiet -- \"%s\""), *AbsoluteFile)), 1);
	return !HasAnyErrors();
}

#endif
