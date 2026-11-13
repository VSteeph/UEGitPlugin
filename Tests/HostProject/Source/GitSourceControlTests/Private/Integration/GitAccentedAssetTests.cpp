#include "GitTestRepository.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "SourceControlOperations.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowAccentedAssetAsModified_When_DesignerSavesIt,
	"UEGitPlugin.Integration.Should_ShowAccentedAssetAsModified_When_DesignerSavesIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowAccentedAssetAsModified_When_DesignerSavesIt::RunTest(const FString& Parameters)
{
	// Arrange: a committed asset whose name has accents, then saved with a change.
	const FGitTestRepository Repository(*this);
	// Outside Content/ so the asset registry never tries to load this fake package.
	const FString RelativeFile = FString::Printf(TEXT("Fixtures/Caf\u00e9-For\u00eat-%s.uasset"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(Repository.RepositoryRoot, RelativeFile);
	ON_SCOPE_EXIT
	{
		Repository.RunGitQuietly(TEXT("fetch --quiet origin main"));
		Repository.RunGitQuietly(TEXT("reset --quiet --hard origin/main"));
		IFileManager::Get().Delete(*AbsoluteFile);
	};
	if (!TestTrue(TEXT("The isolated Git repository is available"), Repository.IsAvailable())
		|| !FFileHelper::SaveStringToFile(FString(TEXT("Committed\n")), *AbsoluteFile)
		|| !Repository.RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeFile))
		|| !Repository.RunGit(TEXT("commit --quiet -m \"Add accented asset\""))
		|| !FFileHelper::SaveStringToFile(FString(TEXT("Saved change\n")), *AbsoluteFile))
	{
		return false;
	}

	// Act: the Content Browser asks for the asset's status.
	ISourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), { AbsoluteFile }, EConcurrency::Synchronous);

	// Assert
	const FSourceControlStatePtr State = Provider.GetState(AbsoluteFile, EStateCacheUsage::Use);
	TestTrue(TEXT("The saved accented asset shows as modified"), State.IsValid() && State->IsModified());
	return !HasAnyErrors();
}

#endif
