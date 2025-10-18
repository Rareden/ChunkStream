// Copyright (C) 2025 Isaac Cooper - All Rights Reserved

#pragma once

#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"

class UChunkStreamDownloader;

class FChunkStreamModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void UpdateHttpVars();
	int32 GetNumDownloadsThatCanStart();
	bool CanStartMoreDownloads() ;
	void RegisterDownloader( UChunkStreamDownloader* Downloader );
	void UnRegisterDownloader( UChunkStreamDownloader* Downloader );
protected:
	FConsoleVariableSinkHandle KitchenSinkHandle;

	TArray<TWeakObjectPtr< UChunkStreamDownloader>> RegisteredDownloaders;
};
