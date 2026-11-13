#include "GitSourceControlModule.h"
#include "GitSourceControlRevision.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ReturnEachAssetRevision_When_PathsShareTheSameFilename,
	"UEGitPlugin.Unit.Should_ReturnEachAssetRevision_When_PathsShareTheSameFilename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ReturnEachAssetRevision_When_PathsShareTheSameFilename::RunTest(const FString& Parameters)
{
	FGitSourceControlModule& Module = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = Module.GetProvider();
	Provider.CheckGitAvailability();
	const FString RepositoryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString GitBinary = Provider.GetGitBinaryPath();
	if (!TestFalse(TEXT("Git is available"), GitBinary.IsEmpty()))
	{
		return false;
	}

	int32 ExitCode = INDEX_NONE;
	FString CommitId;
	FString ErrorOutput;
	const FString GitArguments = FString::Printf(TEXT("-C \"%s\" rev-parse HEAD"), *RepositoryRoot);
	FPlatformProcess::ExecProcess(*GitBinary, *GitArguments, &ExitCode, &CommitId, &ErrorOutput);
	CommitId.TrimStartAndEndInline();
	if (!TestEqual(TEXT("Fixture commit is readable"), ExitCode, 0))
	{
		AddError(ErrorOutput);
		return false;
	}

	FGitSourceControlRevision FirstRevision;
	FirstRevision.CommitId = CommitId;
	FirstRevision.PathToRepoRoot = RepositoryRoot;
	FirstRevision.Filename = TEXT("Content/RevisionFixtures/FolderA/Shared.txt");
	FGitSourceControlRevision SecondRevision;
	SecondRevision.CommitId = CommitId;
	SecondRevision.PathToRepoRoot = RepositoryRoot;
	SecondRevision.Filename = TEXT("Content/RevisionFixtures/FolderB/Shared.txt");

	FString FirstOutput;
	FString SecondOutput;
	const bool bFirstRetrieved = FirstRevision.Get(FirstOutput);
	const bool bSecondRetrieved = SecondRevision.Get(SecondOutput);
	FString FirstContent;
	FString SecondContent;
	const bool bFirstLoaded = FFileHelper::LoadFileToString(FirstContent, *FirstOutput);
	const bool bSecondLoaded = FFileHelper::LoadFileToString(SecondContent, *SecondOutput);
	IFileManager::Get().Delete(*FirstOutput);
	if (SecondOutput != FirstOutput)
	{
		IFileManager::Get().Delete(*SecondOutput);
	}

	TestTrue(TEXT("First revision was retrieved"), bFirstRetrieved && bFirstLoaded);
	TestTrue(TEXT("Second revision was retrieved"), bSecondRetrieved && bSecondLoaded);
	// Fixtures are copied from a checkout whose line endings depend on the machine's core.autocrlf.
	TestEqual(TEXT("First asset content"), FirstContent.TrimEnd(), FString(TEXT("First revision fixture")));
	TestEqual(TEXT("Second asset content"), SecondContent.TrimEnd(), FString(TEXT("Second revision fixture")));
	return !HasAnyErrors();
}

#endif
