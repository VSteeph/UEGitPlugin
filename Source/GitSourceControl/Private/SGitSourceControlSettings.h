// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlSettings.h"
#include "Widgets/SCompoundWidget.h"
#include "ISourceControlProvider.h"
#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MINOR_VERSION >= 2
namespace ETextCommit { enum Type : int; }
#else
namespace ETextCommit { enum Type; }
#endif

namespace ESelectInfo { enum Type : int; }

class SGitSourceControlSettings : public SCompoundWidget
{
public:
	
	SLATE_BEGIN_ARGS(SGitSourceControlSettings) {}
	
	SLATE_END_ARGS()

public:

	void Construct(const FArguments& InArgs);

private:
	void ConstructBasedOnEngineVersion( );

	/** Delegates to get Git binary path from/to settings */
	FString GetBinaryPathString() const;
	void OnBinaryPathPicked(const FString & PickedPath) const;

	/** Delegate to get repository root, user name and email from provider */
	FText GetPathToRepositoryRoot() const;
	FText GetUserName() const;
	FText GetUserEmail() const;

	void OnLfsUserNameCommited(const FText& InText, ETextCommit::Type InCommitType);
	FText GetLfsUserName() const;

	TSharedRef<SWidget> OnGenerateUnlockModeWidget(TSharedPtr<EGitUnlockMode> InMode) const;
	void OnUnlockModeChanged(TSharedPtr<EGitUnlockMode> InMode, ESelectInfo::Type InSelectInfo);
	FText GetSelectedUnlockModeText() const;
	static FText GetUnlockModeLabel(EGitUnlockMode InMode);
	static FText GetUnlockModeTooltip(EGitUnlockMode InMode);

	TArray<TSharedPtr<EGitUnlockMode>> UnlockModeOptions;
	TSharedPtr<EGitUnlockMode> SelectedUnlockMode;
};
