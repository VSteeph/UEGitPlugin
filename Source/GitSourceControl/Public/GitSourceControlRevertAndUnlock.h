// Copyright (c) 2014-2022 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlOperation.h"

/**
 * Custom source control operation that reverts local file changes and unlocks the corresponding Git LFS locks.
 */
class GITSOURCECONTROL_API FGitRevertAndUnlock : public ISourceControlOperation
{
public:
	virtual FName GetName() const override
	{
		return TEXT("RevertAndUnlock");
	}

	virtual FText GetInProgressString() const override
	{
		return NSLOCTEXT("GitSourceControl", "SourceControl_RevertAndUnlock", "Reverting and unlocking file(s) from Git...");
	}
};
