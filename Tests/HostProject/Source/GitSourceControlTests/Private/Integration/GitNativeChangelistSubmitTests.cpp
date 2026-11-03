#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_CommitStagedFiles_When_SubmittingNativeChangelistWithoutExplicitFiles,
	"UEGitPlugin.Integration.Should_CommitStagedFiles_When_SubmittingNativeChangelistWithoutExplicitFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_CommitStagedFiles_When_SubmittingNativeChangelistWithoutExplicitFiles::RunTest(const FString& Parameters)
{
	FGitSourceControlModule& Module = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = Module.GetProvider();
	const bool bWasUsingLocking = Module.AccessSettings().IsUsingGitLfsLocking();
	Module.AccessSettings().SetUsingGitLfsLocking(false);
	Provider.CheckGitAvailability();
	Module.AccessSettings().SetUsingGitLfsLocking(bWasUsingLocking);
	if (!TestTrue(TEXT("The isolated Git repository is available"), Provider.IsEnabled()))
	{
		return false;
	}

	const FString RepositoryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString GitBinary = Provider.GetGitBinaryPath();
	const FString RelativeFixture = FString::Printf(TEXT("Content/NativeSubmit-%s.txt"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AbsoluteFixture = FPaths::Combine(RepositoryRoot, RelativeFixture);
	const FString FixtureContent = TEXT("Native changelist submit fixture\n");
	if (!TestTrue(TEXT("The fixture file is created"), FFileHelper::SaveStringToFile(FixtureContent, *AbsoluteFixture)))
	{
		return false;
	}

	auto RunGit = [&](const FString& Arguments, FString& Output) -> bool
	{
		int32 ExitCode = INDEX_NONE;
		FString ErrorOutput;
		const FString CommandLine = FString::Printf(TEXT("-C \"%s\" %s"), *RepositoryRoot, *Arguments);
		FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ExitCode, &Output, &ErrorOutput);
		if (ExitCode != 0)
		{
			AddError(FString::Printf(TEXT("git %s failed: %s"), *Arguments, *ErrorOutput));
		}
		return ExitCode == 0;
	};

	FString PreviousCommit;
	if (!RunGit(TEXT("rev-parse HEAD"), PreviousCommit))
	{
		return false;
	}
	FString GitOutput;
	if (!RunGit(FString::Printf(TEXT("add -- \"%s\""), *RelativeFixture), GitOutput))
	{
		return false;
	}

	const TSharedRef<FUpdatePendingChangelistsStatus> UpdateOperation = ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>();
	if (!TestTrue(TEXT("The native Staged changelist is refreshed"),
		Provider.Execute(UpdateOperation, EConcurrency::Synchronous) == ECommandResult::Succeeded))
	{
		return false;
	}

	FSourceControlChangelistPtr Changelist;
	for (const FSourceControlChangelistRef& Candidate : Provider.GetChangelists(EStateCacheUsage::Use))
	{
		if (Candidate->GetIdentifier() == TEXT("Staged"))
		{
			Changelist = Candidate;
			break;
		}
	}
	if (!TestTrue(TEXT("The native Staged changelist is available"), Changelist.IsValid()))
	{
		return false;
	}
	TArray<FSourceControlChangelistStateRef> ChangelistStates;
	if (!TestTrue(TEXT("The Staged changelist is available"),
		Provider.GetState({Changelist.ToSharedRef()}, ChangelistStates, EStateCacheUsage::Use) == ECommandResult::Succeeded && ChangelistStates.Num() == 1))
	{
		return false;
	}
	if (!TestTrue(TEXT("The submitted asset is in Staged"),
		ChangelistStates[0]->GetFilesStates().ContainsByPredicate([&AbsoluteFixture](const FSourceControlStateRef& State)
		{
			return State->GetFilename() == AbsoluteFixture;
		})))
	{
		return false;
	}

	const TSharedRef<FCheckIn> SubmitOperation = ISourceControlOperation::Create<FCheckIn>();
	SubmitOperation->SetDescription(FText::FromString(TEXT("Submit staged asset from native changelist")));
	const ECommandResult::Type SubmitResult = Provider.Execute(SubmitOperation, Changelist, {}, EConcurrency::Synchronous);
	FString CurrentCommit;
	const bool bCommitReadable = RunGit(TEXT("rev-parse HEAD"), CurrentCommit);
	FString RemoteCommit;
	const bool bRemoteReadable = RunGit(TEXT("rev-parse origin/main"), RemoteCommit);
	FString CommittedContent;
	const bool bContentReadable = RunGit(FString::Printf(TEXT("show HEAD:%s"), *RelativeFixture), CommittedContent);

	TestTrue(TEXT("The native submit reports success"), SubmitResult == ECommandResult::Succeeded);
	TestTrue(TEXT("The submit creates a commit"), bCommitReadable && CurrentCommit.TrimStartAndEnd() != PreviousCommit.TrimStartAndEnd());
	TestTrue(TEXT("The new commit reaches the local remote"), bRemoteReadable && RemoteCommit.TrimStartAndEnd() == CurrentCommit.TrimStartAndEnd());
	TestTrue(TEXT("The committed asset has the expected content"), bContentReadable && CommittedContent.TrimStartAndEnd() == FixtureContent.TrimStartAndEnd());
	return !HasAnyErrors();
}

#endif
