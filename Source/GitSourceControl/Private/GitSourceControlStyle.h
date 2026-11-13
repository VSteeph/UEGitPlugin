// Copyright (c) 2014-2022 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FSlateStyleSet;

/** Slate style set for the asset status icons: own lock, lock by other, outdated asset and its badge. */
class FGitSourceControlStyle
{
public:
	static void Initialize();
	static void Shutdown();

	static FName GetStyleSetName();

	static const FName LockSelfBrushName;
	static const FName LockOtherBrushName;
	static const FName OutdatedBrushName;
	static const FName OutdatedBadgeBrushName;

private:
	static TSharedPtr<FSlateStyleSet> StyleInstance;
};
