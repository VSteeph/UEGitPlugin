// Copyright (c) 2014-2022 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlStyle.h"

#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

const FName FGitSourceControlStyle::LockSelfBrushName(TEXT("GitSourceControl.LockSelf"));
const FName FGitSourceControlStyle::LockOtherBrushName(TEXT("GitSourceControl.LockOther"));
const FName FGitSourceControlStyle::OutdatedBrushName(TEXT("GitSourceControl.Outdated"));
const FName FGitSourceControlStyle::OutdatedBadgeBrushName(TEXT("GitSourceControl.OutdatedBadge"));
TSharedPtr<FSlateStyleSet> FGitSourceControlStyle::StyleInstance;

FName FGitSourceControlStyle::GetStyleSetName()
{
	static const FName StyleSetName(TEXT("GitSourceControlStyle"));
	return StyleSetName;
}

void FGitSourceControlStyle::Initialize()
{
	if (StyleInstance.IsValid())
	{
		return;
	}

	const FName StyleName = GetStyleSetName();
	if (FSlateStyleRegistry::FindSlateStyle(StyleName) != nullptr)
	{
		return;
	}

	TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(StyleName);
	Style->SetCoreContentRoot(FPaths::EngineContentDir() / TEXT("Slate"));
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl")))
	{
		Style->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	}

	// Unreal RevisionControl status palette: checked out, checked out by other, not at head.
	const FLinearColor LockSelfColor = FLinearColor::FromSRGBColor(FColor::FromHex("#1FE44B"));
	const FLinearColor LockOtherColor = FLinearColor::FromSRGBColor(FColor::FromHex("#EF3535"));
	const FLinearColor OutdatedColor = FLinearColor::FromSRGBColor(FColor::FromHex("#E1FF3D"));
	const FVector2D IconSize(16.0f, 16.0f);

	Style->Set(LockSelfBrushName, new FSlateVectorImageBrush(Style->RootToCoreContentDir(TEXT("Starship/Common/edit"), TEXT(".svg")), IconSize, LockSelfColor));
	Style->Set(LockOtherBrushName, new FSlateVectorImageBrush(Style->RootToCoreContentDir(TEXT("Starship/Common/lock"), TEXT(".svg")), IconSize, LockOtherColor));
	Style->Set(OutdatedBrushName, new FSlateVectorImageBrush(Style->RootToCoreContentDir(TEXT("Starship/SourceControl/RC_NewerVersion"), TEXT(".svg")), IconSize, OutdatedColor));
	Style->Set(OutdatedBadgeBrushName, new FSlateVectorImageBrush(Style->RootToContentDir(TEXT("OutdatedBadge"), TEXT(".svg")), IconSize, OutdatedColor));

	FSlateStyleRegistry::RegisterSlateStyle(*Style);
	StyleInstance = MoveTemp(Style);
}

void FGitSourceControlStyle::Shutdown()
{
	if (StyleInstance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
		ensure(StyleInstance.IsUnique());
		StyleInstance.Reset();
	}
}
