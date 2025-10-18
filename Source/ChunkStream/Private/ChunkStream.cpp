// Copyright (C) 2025 Isaac Cooper - All Rights Reserved

#include "ChunkStream.h"

#include "HttpModule.h"
#include "ChunkStreamDownloader.h"
#include "ChunkStreamLogs.h"

#define LOCTEXT_NAMESPACE "FChunkStreamModule"

// Console variable to control HTTP thread tick rate (in Hz)
// Higher values = more responsive downloads but more CPU overhead
// Default: 400 Hz (checks for new data 400 times per second)
TAutoConsoleVariable<int32> CVarHttpThreadTickRate(
	TEXT("ChunkStream.HttpThreadTickRate"),
	400,
	TEXT("HTTP thread tick rate in Hz (times per second the thread checks for new data). Higher values give faster downloads but use more CPU. Range: 60-1000 Hz. Default: 400 Hz"),
	ECVF_Default
);

// Console variable to control max read buffer size (in KB)
// Larger buffers = potentially faster downloads but more memory usage
// Default: 512 KB
TAutoConsoleVariable<int32> CVarHttpMaxReadBufferSize(
	TEXT("ChunkStream.HttpMaxReadBufferSize"),
	512,
	TEXT("Maximum HTTP read buffer size in KB. Larger buffers can improve download speed but use more memory. Range: 64-2048 KB. Default: 512 KB"),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarMaxConcurrentDownloads(
	TEXT("ChunkStream.MaxConcurrentDownloads"),
	3,
	TEXT("Max number of downloads that can be running at once."),
	ECVF_Default);



void FChunkStreamModule::StartupModule()
{
	KitchenSinkHandle = IConsoleManager::Get().RegisterConsoleVariableSink_Handle(FConsoleCommandDelegate::CreateLambda([this]
	{
		UpdateHttpVars();
	}));
}

void FChunkStreamModule::ShutdownModule()
{
	IConsoleManager::Get().UnregisterConsoleVariableSink_Handle(KitchenSinkHandle);
}

void FChunkStreamModule::UpdateHttpVars()
{
	if (auto Module = FModuleManager::Get().GetModule("HTTP"))
	{
		FHttpModule* HttpModule = static_cast<FHttpModule*>(Module);
		if (HttpModule)
		{
			
			int32 TickRateHz = CVarHttpThreadTickRate.GetValueOnAnyThread();
			TickRateHz = FMath::Clamp(TickRateHz, 60, 1000);
			float TargetFrameTime = 1.0f / static_cast<float>(TickRateHz);
			
	
			if (HttpModule->GetHttpThreadActiveFrameTimeInSeconds() > TargetFrameTime)
			{
				HttpModule->SetHttpThreadActiveFrameTimeInSeconds(TargetFrameTime);
			}
			
		
			int32 BufferSizeKB = CVarHttpMaxReadBufferSize.GetValueOnAnyThread();
			BufferSizeKB = FMath::Clamp(BufferSizeKB, 64, 2048);
			int32 BufferSizeBytes = BufferSizeKB * 1024;
			
		
			if (HttpModule->GetMaxReadBufferSize() < BufferSizeBytes)
			{
				HttpModule->SetMaxReadBufferSize(BufferSizeBytes);
			}
		}
	}
}

int32 FChunkStreamModule::GetNumDownloadsThatCanStart()
{
	int32 MaxDownloads = FMath::Clamp(CVarMaxConcurrentDownloads.GetValueOnAnyThread(), 1, 1000);
	if (RegisteredDownloaders.Num() < 1)
		return MaxDownloads;

	
	int32 ActiveDownloads = 0;
	for (int32 i = RegisteredDownloaders.Num() - 1; i >= 0; i--)
	{
		auto Downloader = RegisteredDownloaders[i];
		if (Downloader.IsValid() && IsValid(Downloader.Get()))
		{
			if (Downloader->IsActive())
				ActiveDownloads++;
		}
		else
		{
			RegisteredDownloaders.RemoveAt(i);
		}
	}

	return MaxDownloads - ActiveDownloads;
}

bool FChunkStreamModule::CanStartMoreDownloads() 
{
	return	GetNumDownloadsThatCanStart() > 0;
}

void FChunkStreamModule::RegisterDownloader( UChunkStreamDownloader* Downloader)
{
	bool bFound = false;
	for (int32 i = RegisteredDownloaders.Num() - 1; i >= 0; i--)
	{
		if (RegisteredDownloaders[i] == Downloader)
		{
			bFound = true;
				return;
		}
	}
	RegisteredDownloaders.Add(Downloader);
}

void FChunkStreamModule::UnRegisterDownloader( UChunkStreamDownloader* Downloader)
{
	if (!IsValid(Downloader))
		return;



	for (int32 i = RegisteredDownloaders.Num() - 1; i >= 0; i--)
	{
		if (RegisteredDownloaders[i] == Downloader)
		{
			RegisteredDownloaders.RemoveAt(i);
		}
	}
	int32 NewToStart = GetNumDownloadsThatCanStart();
	int32 Started=0;
	if (NewToStart > 0)
	{
		for (auto& DownloaderToStart : RegisteredDownloaders)
		{
			if (DownloaderToStart.IsValid() && !DownloaderToStart->IsActive())
			{
				DownloaderToStart->Activate();
				NewToStart-=1;
				Started++;
			}
			if (NewToStart <= 0)
			{
				LOG("Started %d new downloads",Started);
				break;
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FChunkStreamModule, ChunkStream);