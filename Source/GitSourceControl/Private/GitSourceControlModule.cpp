// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlModule.h"

#include "Styling/AppStyle.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

#include "ContentBrowserModule.h"
#include "ContentBrowserDelegates.h"

#include "GitSourceControlOperations.h"
#include "GitSourceControlStyle.h"
#include "GitSourceControlUtils.h"
#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/ConfigCacheIni.h"
#include "Runtime/Launch/Resources/Version.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

TArray<FString> FGitSourceControlModule::EmptyStringArray;

namespace
{
    static const FName NAME_SourceControl( TEXT( "SourceControl" ) );
    static const FName NAME_ContentBrowser( TEXT( "ContentBrowser" ) );
}

template<typename Type>
static TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe> CreateWorker()
{
	return MakeShareable( new Type() );
}

void FGitSourceControlModule::StartupModule()
{
	FGitSourceControlStyle::Initialize();

	// Register our operations (implemented in GitSourceControlOperations.cpp by subclassing from Engine\Source\Developer\SourceControl\Public\SourceControlOperations.h)
	GitSourceControlProvider.RegisterWorker( "Connect", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitConnectWorker> ) );
	// Note: this provider uses the "CheckOut" command only with Git LFS 2 "lock" command, since Git itself has no lock command (all tracked files in the working copy are always already checked-out).
	GitSourceControlProvider.RegisterWorker( "CheckOut", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitCheckOutWorker> ) );
	GitSourceControlProvider.RegisterWorker( "UpdateStatus", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitUpdateStatusWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Revert", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitRevertWorker> ) );
	GitSourceControlProvider.RegisterWorker( "RevertAndUnlock", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitRevertAndUnlockWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Fetch", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitFetchWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Delete", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitDeleteWorker> ) );
	for (const FName StatusOnlyOperation : { FName("MarkForAdd"), FName("Copy") })
	{
		GitSourceControlProvider.RegisterWorker( StatusOnlyOperation, FGetGitSourceControlWorker::CreateLambda( [StatusOnlyOperation]() -> FGitSourceControlWorkerRef
		{
			return MakeShared<FGitStatusOnlyWorker, ESPMode::ThreadSafe>(StatusOnlyOperation);
		} ) );
	}

	// load our settings
	GitSourceControlSettings.LoadSettings();

	// Bind our revision control provider to the editor
    IModularFeatures::Get().RegisterModularFeature( NAME_SourceControl, &GitSourceControlProvider );

	FContentBrowserModule & ContentBrowserModule = FModuleManager::Get().LoadModuleChecked< FContentBrowserModule >( NAME_ContentBrowser );

	CbdHandle_OnFilterChanged = ContentBrowserModule.GetOnFilterChanged().AddLambda( [this]( const FARFilter&, bool ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 2; } );
	CbdHandle_OnSearchBoxChanged = ContentBrowserModule.GetOnSearchBoxChanged().AddLambda( [this]( const FText&, bool ){ GitSourceControlProvider.TicksUntilNextForcedUpdate = 1; } );
	CbdHandle_OnAssetSelectionChanged = ContentBrowserModule.GetOnAssetSelectionChanged().AddLambda( [this]( const TArray<FAssetData>&, bool ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 1; } );
	CbdHandle_OnAssetPathChanged = ContentBrowserModule.GetOnAssetPathChanged().AddLambda( [this]( const FString& ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 2; } );
}

void FGitSourceControlModule::ShutdownModule()
{
	// shut down the provider, as this module is going away
	GitSourceControlProvider.Close();

	// unbind provider from editor
    IModularFeatures::Get().UnregisterModularFeature( NAME_SourceControl, &GitSourceControlProvider );

	FGitSourceControlStyle::Shutdown();

	// Unregister ContentBrowserDelegate Handles
    FContentBrowserModule & ContentBrowserModule = FModuleManager::Get().GetModuleChecked< FContentBrowserModule >( NAME_ContentBrowser );
	ContentBrowserModule.GetOnFilterChanged().Remove( CbdHandle_OnFilterChanged );
	ContentBrowserModule.GetOnSearchBoxChanged().Remove( CbdHandle_OnSearchBoxChanged );
	ContentBrowserModule.GetOnAssetSelectionChanged().Remove( CbdHandle_OnAssetSelectionChanged );
	ContentBrowserModule.GetOnAssetPathChanged().Remove( CbdHandle_OnAssetPathChanged );
}

void FGitSourceControlModule::SaveSettings()
{
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}

	GitSourceControlSettings.SaveSettings();
}

void FGitSourceControlModule::SetLastErrors(const TArray<FText>& InErrors)
{
	FGitSourceControlModule* Module = FModuleManager::GetModulePtr<FGitSourceControlModule>("GitSourceControl");
	if (Module)
	{
		Module->GetProvider().SetLastErrors(InErrors);
	}
}

IMPLEMENT_MODULE( FGitSourceControlModule, GitSourceControl );

#undef LOCTEXT_NAMESPACE
