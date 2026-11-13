#include "GitSourceControlState.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace GitStateIconTests
{
	const FName StyleSetName(TEXT("GitSourceControlStyle"));
	const FName LockSelf(TEXT("GitSourceControl.LockSelf"));
	const FName LockOther(TEXT("GitSourceControl.LockOther"));
	const FName Outdated(TEXT("GitSourceControl.Outdated"));
	const FName OutdatedBadge(TEXT("GitSourceControl.OutdatedBadge"));

	FSlateIcon IconFor(ELockState::Type LockState, ERemoteState::Type RemoteState)
	{
		FGitSourceControlState State(TEXT("C:/Repo/Content/Asset.uasset"));
		State.State.LockState = LockState;
		State.State.RemoteState = RemoteState;
		return State.GetIcon();
	}

	bool HasBadge(const FSlateIcon& Icon, const FName& BadgeName)
	{
		const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(StyleSetName);
		const FSlateBrush* Badge = Style ? Style->GetOptionalBrush(BadgeName, nullptr, nullptr) : nullptr;
		return Badge && Icon.GetOverlayIcon() == Badge;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowNoIcon_When_AssetIsFreeAndCurrent,
	"UEGitPlugin.Unit.Should_ShowNoIcon_When_AssetIsFreeAndCurrent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowNoIcon_When_AssetIsFreeAndCurrent::RunTest(const FString& Parameters)
{
	// Act
	const FSlateIcon Icon = GitStateIconTests::IconFor(ELockState::NotLocked, ERemoteState::UpToDate);

	// Assert
	TestFalse(TEXT("A free asset has no icon"), Icon.IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowGreenPencil_When_AssetIsLockedByMe,
	"UEGitPlugin.Unit.Should_ShowGreenPencil_When_AssetIsLockedByMe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowGreenPencil_When_AssetIsLockedByMe::RunTest(const FString& Parameters)
{
	// Act
	const FSlateIcon Icon = GitStateIconTests::IconFor(ELockState::Locked, ERemoteState::UpToDate);

	// Assert
	TestEqual(TEXT("The own lock icon is shown"), Icon.GetStyleName(), GitStateIconTests::LockSelf);
	TestFalse(TEXT("No badge is stacked"), GitStateIconTests::HasBadge(Icon, GitStateIconTests::OutdatedBadge));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowRedPadlock_When_AssetIsLockedByOther,
	"UEGitPlugin.Unit.Should_ShowRedPadlock_When_AssetIsLockedByOther",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowRedPadlock_When_AssetIsLockedByOther::RunTest(const FString& Parameters)
{
	// Act
	const FSlateIcon Icon = GitStateIconTests::IconFor(ELockState::LockedOther, ERemoteState::UpToDate);

	// Assert
	TestEqual(TEXT("The other user's lock icon is shown"), Icon.GetStyleName(), GitStateIconTests::LockOther);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowDownloadArrow_When_FreeAssetIsOutdated,
	"UEGitPlugin.Unit.Should_ShowDownloadArrow_When_FreeAssetIsOutdated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowDownloadArrow_When_FreeAssetIsOutdated::RunTest(const FString& Parameters)
{
	// Act
	const FSlateIcon NotAtHeadIcon = GitStateIconTests::IconFor(ELockState::NotLocked, ERemoteState::NotAtHead);
	const FSlateIcon NotLatestIcon = GitStateIconTests::IconFor(ELockState::NotLocked, ERemoteState::NotLatest);

	// Assert
	TestEqual(TEXT("Newer on my remote branch shows the arrow"), NotAtHeadIcon.GetStyleName(), GitStateIconTests::Outdated);
	TestEqual(TEXT("Newer on another branch shows the arrow"), NotLatestIcon.GetStyleName(), GitStateIconTests::Outdated);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_StackOutdatedBadgeOnLock_When_LockedAssetIsOutdated,
	"UEGitPlugin.Unit.Should_StackOutdatedBadgeOnLock_When_LockedAssetIsOutdated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_StackOutdatedBadgeOnLock_When_LockedAssetIsOutdated::RunTest(const FString& Parameters)
{
	// Act
	const FSlateIcon MineIcon = GitStateIconTests::IconFor(ELockState::Locked, ERemoteState::NotLatest);
	const FSlateIcon TheirsIcon = GitStateIconTests::IconFor(ELockState::LockedOther, ERemoteState::NotAtHead);

	// Assert
	TestEqual(TEXT("My lock stays the main icon"), MineIcon.GetStyleName(), GitStateIconTests::LockSelf);
	TestTrue(TEXT("The arrow badge is stacked on my lock"), GitStateIconTests::HasBadge(MineIcon, GitStateIconTests::OutdatedBadge));
	TestEqual(TEXT("The other lock stays the main icon"), TheirsIcon.GetStyleName(), GitStateIconTests::LockOther);
	TestTrue(TEXT("The arrow badge is stacked on the other lock"), GitStateIconTests::HasBadge(TheirsIcon, GitStateIconTests::OutdatedBadge));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_LoadEveryStatusIconFromDisk_When_PluginStarts,
	"UEGitPlugin.Unit.Should_LoadEveryStatusIconFromDisk_When_PluginStarts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_LoadEveryStatusIconFromDisk_When_PluginStarts::RunTest(const FString& Parameters)
{
	// Arrange
	const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(GitStateIconTests::StyleSetName);
	if (!TestNotNull(TEXT("The plugin style is registered"), Style))
	{
		return false;
	}

	for (const FName& BrushName : { GitStateIconTests::LockSelf, GitStateIconTests::LockOther, GitStateIconTests::Outdated, GitStateIconTests::OutdatedBadge })
	{
		// Act
		const FSlateBrush* Brush = Style->GetOptionalBrush(BrushName, nullptr, nullptr);

		// Assert: a missing SVG would render as an empty slot without any error.
		const FString ImagePath = Brush ? Brush->GetResourceName().ToString() : FString();
		TestTrue(FString::Printf(TEXT("%s is defined"), *BrushName.ToString()), Brush != nullptr);
		TestTrue(FString::Printf(TEXT("%s image exists at '%s'"), *BrushName.ToString(), *ImagePath), FPaths::FileExists(ImagePath));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShould_ShowOwnerAndDateInTooltip_When_AssetIsLockedByOther,
	"UEGitPlugin.Unit.Should_ShowOwnerAndDateInTooltip_When_AssetIsLockedByOther",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShould_ShowOwnerAndDateInTooltip_When_AssetIsLockedByOther::RunTest(const FString& Parameters)
{
	// Arrange
	FGitSourceControlState State(TEXT("C:/Repo/Content/Asset.uasset"));
	State.State.LockState = ELockState::LockedOther;
	State.State.LockUser = TEXT("Colleague");
	State.State.LockDate = FDateTime(2026, 11, 12, 8, 0, 0);
	const FString ExpectedDate = FText::AsDateTime(State.State.LockDate, EDateTimeStyle::Short, EDateTimeStyle::Short).ToString();

	// Act
	const FString Tooltip = State.GetDisplayTooltip().ToString();

	// Assert
	TestTrue(TEXT("The tooltip names the owner"), Tooltip.Contains(TEXT("Colleague")));
	TestTrue(FString::Printf(TEXT("The tooltip shows the lock date '%s' in '%s'"), *ExpectedDate, *Tooltip), Tooltip.Contains(ExpectedDate));
	return !HasAnyErrors();
}

#endif
