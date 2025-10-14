// Fill out your copyright notice in the Description page of Project Settings.


#include "ChunkStreamDownloader.h"
#include "StreamChunkDownloader.h"
#include "ChunkStream.h"
#include "ChunkStreamLogs.h"

static constexpr uint64 MB = 1024 * 1024;
uint64 inline MbToBytes(const uint64& InVal) { return InVal * MB;}
uint64 inline MbToBytes(const int64& InVal) { return static_cast<uint64>(InVal) * MB;}
uint64 inline MbToBytes(const int32& InVal) { return static_cast<uint64>(FMath::Abs(InVal)) * MB;}

TAutoConsoleVariable<int32> CVarFileDownloadMaxChunkSize(TEXT("ChunkStream.MaxChunkSize"),
	100,
	TEXT("Max Chunk size in MB to reserve for a download before the chunk has to be saved to storage. Type the number in MB eg 100 = 100MB\n Larger values are faster to download but reserve more memory.")
	TEXT(" 1 = 1MB.\n")
	TEXT(" 100 = 100MB\n")
	);

uint64 FChunkStreamDownloaderUtils::GetMaxChunkSize()
{
	int32 ValueInMB = CVarFileDownloadMaxChunkSize.GetValueOnAnyThread();
	uint64 ValueInBytes = MbToBytes(ValueInMB) ;
    
	constexpr uint64 MinSize = MB;        // 1 MB
	constexpr uint64 MaxSize = MB * 1024; // 1 GB
	constexpr uint64 Alignment = 4096  ; // 4kb alingment


	uint64 ChunkSizeInBytes = ( ValueInBytes / Alignment) * Alignment;
	
	// less than 1GB and equal or more than 1MB
	if (ChunkSizeInBytes < MaxSize && ChunkSizeInBytes >= MinSize)
	{
		return ChunkSizeInBytes;
	}
	else
	{
		LOG_WARN("GetMaxChunkSize - Value if %d is outside the limits, enter value in MB ", ValueInMB);
		// default 100MB if above failed
		return (MbToBytes(100) / Alignment) *  Alignment;
	}
}

void UChunkStreamDownloader::BeginDestroy()
{
	FChunkStreamModule& Module = FModuleManager::Get().GetModuleChecked<FChunkStreamModule>(TEXT("ChunkStream"));
	Module.UnRegisterDownloader(this);
	
	if (StreamChunkDownloader)
	{
		StreamChunkDownloader->Shutdown();
	}
	Native_DownloadProgress.Clear();
	Native_DownloadFinished.Clear();
	OnProgress.Clear();
	OnComplete.Clear();
	
	Super::BeginDestroy();
}

bool UChunkStreamDownloader::IsReadyForFinishDestroy()
{
	if (StreamChunkDownloader)
	{
		StreamChunkDownloader.Reset();
		return false;
	}
	bool Expected = false;
	if (!bChunkPendingWrite.compare_exchange_strong(Expected, false))
	{
		// file still holding write lock
		return false;
	}
	if (OpenFile)
	{
		CloseFile();
		return false;
	}
	return true;
}

UChunkStreamDownloader* UChunkStreamDownloader::DownloadFileToStorage(const UObject* WorldContext, const FString& URL,
                                                                      const FString& ContentType, const FString& LocationToSaveTo)
{
	UChunkStreamDownloader* Downloader = NewObject<UChunkStreamDownloader>();
	if (IsValid(WorldContext))
	{
		Downloader->RegisterWithGameInstance(WorldContext);
	}
	Downloader->StreamChunkDownloader = MakeShared<FStreamChunkDownloader>(URL, ContentType);
	Downloader->FileSavePath=LocationToSaveTo;
	Downloader->CurrentResultParams.Downloader = Downloader;
	Downloader->URL=URL;
	return Downloader;
}

FString UChunkStreamDownloader::LoadFileToString(const FString FilePath)
{
	FString Result;
	if (IFileManager::Get().FileExists(*FilePath))
	{
		FFileHelper::LoadFileToString(Result, *FilePath);
	}

	return Result;
}

void UChunkStreamDownloader::Activate()
{
	Super::Activate();
	
	{
		FChunkStreamModule& Module = FModuleManager::Get().GetModuleChecked<FChunkStreamModule>(TEXT("ChunkStream"));
		Module.RegisterDownloader(this);
		if (!Module.CanStartMoreDownloads())
		{
			CurrentResultParams.Progress=0.0f;
			CurrentResultParams.DownloadTaskResult = EChunkStreamDownloadResult::WaitingForOtherDownload;
			LOG("Cant start download for '%s' Waiting for space to start", *URL);
			return;
		}
		
	}
		
	TempDownloadDir = GetTempPathForSavePath(FileSavePath);
	
	StreamChunkDownloader->BeginDownload(FChunkStreamDownloaderUtils::GetMaxChunkSize(),
		FStreamDownloadProgressSignature::CreateUObject(this,&UChunkStreamDownloader::OnDownloadProgress),
		FOnSingleChunkCompleteSignature::CreateUObject(this,&UChunkStreamDownloader::OnChunkCompleted),
		FOnDownloadCompleteSignature::CreateUObject(this,&UChunkStreamDownloader::OnDownloadComplete)
		);
	
	LOG("Started Download of '%s'",*URL)
	
	if (!OpenFileForWriting(TempDownloadDir))
	{
		CurrentResultParams.DownloadTaskResult = EChunkStreamDownloadResult::FileSystemError;
		CurrentResultParams.Progress=0.0f;
		Native_DownloadProgress.Broadcast(CurrentResultParams);
		OnProgress.Broadcast(CurrentResultParams);
		LOG_ERROR("Failed to open temporary file for writing! '%s' ",*TempDownloadDir);
		CloseFile();
		CancelDownload();
	}
	else
	{
		CurrentResultParams.DownloadTaskResult = EChunkStreamDownloadResult::InProgress;
		CurrentResultParams.Progress=0.0f;
		Native_DownloadProgress.Broadcast(CurrentResultParams);
		OnProgress.Broadcast(CurrentResultParams);
	}
}

bool UChunkStreamDownloader::CancelDownload()
{
	bCanceled = StreamChunkDownloader->CancelDownload();
	CurrentResultParams.DownloadTaskResult = EChunkStreamDownloadResult::UserCancelled;

	return bCanceled;
}

bool UChunkStreamDownloader::IsActive() const
{
	return !bCanceled && StreamChunkDownloader.IsValid() && !StreamChunkDownloader->IsCanceled() && StreamChunkDownloader->HasStarted();
}

void UChunkStreamDownloader::OnDownloadProgress(uint64 BytesReceived, float InProgress)
{
	CurrentResultParams.Progress=InProgress;
	CurrentResultParams.DownloadTaskResult = EChunkStreamDownloadResult::InProgress;
	if (StreamChunkDownloader)
	{
		CurrentResultParams.HttpStatusCode = StreamChunkDownloader->GetHttpStatusCode();
	}
	Native_DownloadProgress.Broadcast(CurrentResultParams);
	OnProgress.Broadcast(CurrentResultParams);
}

void UChunkStreamDownloader::OnChunkCompleted(TUniquePtr<StreamChunkDownloader::FChunkInfo>&& ChunkData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UChunkStreamDownloader::OnChunkCompleted)
	if (StreamChunkDownloader)
	{
		CurrentResultParams.HttpStatusCode = StreamChunkDownloader->GetHttpStatusCode();
	}
	
	check(ChunkData);
	TWeakObjectPtr<UChunkStreamDownloader> WeakThis = this;
	AsyncTask(ENamedThreads::Type::AnyHiPriThreadNormalTask, [WeakThis,  ChunkData = MoveTemp(ChunkData)]() mutable 
		{
			if (IsValid(WeakThis.Get()) && !WeakThis.IsStale() )
				WeakThis->WriteChunkToFile(MoveTemp(ChunkData) );
			else
			{
				LOG_ERROR("OnChunkReceived:: Invalid downloader object!");
			}
		});
}

void UChunkStreamDownloader::WriteChunkToFile(TUniquePtr<StreamChunkDownloader::FChunkInfo>&& ChunkData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UChunkStreamDownloader::WriteChunkToFile)
	FScopeLock WriteLock(&WriteFileLock);
	bChunkPendingWrite.store(true);
	check(ChunkData);
	if (!OpenFile)
	{
#if !UE_BUILD_SHIPPING
		check(0);
#endif
		if (!OpenFile)
		{
			bChunkPendingWrite.store(false);
			return;
		}
	}
	
	// Calculate the bytes needed for this chunk
	uint64 BytesWritten = (ChunkData->EndOffset - ChunkData->StartOffset) + 1;
	
	// Check if there's enough disk space before writing
	uint64 TotalDiskSpace = 0;
	uint64 FreeDiskSpace = 0;
	if (FPlatformMisc::GetDiskTotalAndFreeSpace(FPaths::GetPath(TempDownloadDir), TotalDiskSpace, FreeDiskSpace))
	{
		// Add a small buffer (1MB) to the required space for safety
		const uint64 SafetyBuffer = 1024 * 1024;
		uint64 RequiredSpace = ChunkData->EndOffset + SafetyBuffer;
		
		if (FreeDiskSpace < RequiredSpace)
		{
			LOG_ERROR("Insufficient disk space! Required: %llu bytes, Available: %llu bytes", 
				RequiredSpace, FreeDiskSpace);
			bChunkPendingWrite.store(false);
			
			// Cancel the download and trigger completion with error
			if (StreamChunkDownloader.IsValid())
			{
				StreamChunkDownloader->CancelDownload();
			}
			
			// Trigger the completion callback with insufficient disk space error
			OnDownloadComplete(EChunkStreamDownloadResult::InsufficientDiskSpace);
			return;
		}
	}
	else
	{
		// If we can't check disk space, log a warning but continue
		LOG_WARN("Unable to check disk space for path: %s", *TempDownloadDir);
	}
	
	// Seek to correct position and write
	OpenFile->Seek(ChunkData->StartOffset);

	if (OpenFile->Write(ChunkData->Data.GetData(),BytesWritten)	)
	{
		// flushes the in memory version of the file to storage
		OpenFile->Flush();
		LOG_VERBOSE("Written chunk [%lld-%lld] of %lld total bytes", 
			ChunkData->StartOffset, ChunkData->EndOffset, ChunkData->TotalFileSize);
	}
	else
	{
		LOG_VERBOSE("Failed to write chunk region [%lld-%lld] to drive storage!", 
			ChunkData->StartOffset, ChunkData->EndOffset);
	}
	bChunkPendingWrite.store(false);
}

void UChunkStreamDownloader::OnDownloadComplete(EChunkStreamDownloadResult Result)
{
	if (StreamChunkDownloader)
	{
		CurrentResultParams.HttpStatusCode = StreamChunkDownloader->GetHttpStatusCode();
	}
	TWeakObjectPtr<UChunkStreamDownloader> WeakDownloader = this;
	AsyncTask(ENamedThreads::Type::AnyHiPriThreadNormalTask,[WeakDownloader, Result = MoveTemp(Result)]() mutable
	{
		// expect no file to be writing to storage
		bool Expected = false;
		if (IsValid(WeakDownloader.Get()) && !WeakDownloader.IsStale() )
		{
			while (
				!WeakDownloader->bChunkPendingWrite.compare_exchange_strong(Expected,false))
			{
				LOG_VERBOSE("Chunk is pending write, waiting for completion...");
				Expected=false;
				// writing is running so dont complete until its done, wait untill its free
				FPlatformProcess::Sleep(0.05);
			}
			// close it now so we can move it
			WeakDownloader->CloseFile();
			// move file to final location
			if (Result == EChunkStreamDownloadResult::Success)
			{
				uint8 MoveAttempts = 0;
				// try move to final save location
				while (!WeakDownloader->MoveTempFileToFinalSave())
				{
					MoveAttempts++;
					FPlatformProcess::Sleep(0.5);
					if (MoveAttempts >= 6)
					{
						Result = EChunkStreamDownloadResult::FileSystemError;
						LOG_ERROR("Failed to move to final saving location!");
						break;
					}
				}
			}
			// delete temp file if not successful
			else
			{
				if (IFileManager::Get().FileExists(*WeakDownloader->TempDownloadDir))
				{
					if (IFileManager::Get().Delete(*WeakDownloader->TempDownloadDir))
						LOG("Deleted temp file after download failed");
				}
			}
			
			
				
			AsyncTask(ENamedThreads::Type::GameThread,[WeakDownloader, Result = MoveTemp(Result)]() mutable
			{
				WeakDownloader->Completed(Result);
			});
					
				
		}
		else
		{
			LOG_ERROR("OnResult:: Invalid downloader object!");
		}
			
	});
	
	
}

void UChunkStreamDownloader::Completed(EChunkStreamDownloadResult InResult)
{
	FChunkStreamModule& Module = FModuleManager::Get().GetModuleChecked<FChunkStreamModule>(TEXT("ChunkStream"));
	
	bCompleted = true;
	CurrentResultParams.DownloadTaskResult = InResult;
	Native_DownloadFinished.Broadcast(CurrentResultParams);
	OnComplete.Broadcast(CurrentResultParams);
	
	Module.UnRegisterDownloader(this);
	SetReadyToDestroy();

	ConditionalBeginDestroy();
}

bool UChunkStreamDownloader::OpenFileForWriting(const FString& InFilePath)
{
	FScopeLock WriteLock(&WriteFileLock);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// Create save directory if it does not exist
	{
		FString Path, Filename, Extension;
		FPaths::Split(InFilePath, Path, Filename, Extension);
		if (!PlatformFile.DirectoryExists(*Path))
		{
			if (!PlatformFile.CreateDirectoryTree(*Path))
			{
				LOG_ERROR("Unable to create a directory '%s' to save the downloaded file", *Path);
				OpenFile=nullptr;
				return false;
			}
		}
	}

	// Delete the file if it already exists
	if (FPaths::FileExists(*InFilePath))
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.Delete(*InFilePath))
		{
			LOG_ERROR("Something went wrong while deleting the existing file '%s'", *InFilePath);
			return false;
		}
	}

	OpenFile = PlatformFile.OpenWrite(*InFilePath);
	if (OpenFile != nullptr)
	{
		LOG("File '%s' opened", *InFilePath);
		return true;
	}
	else
	{
		LOG_ERROR("Failed to open file for '%s'", *InFilePath);
	}
	return false;
}

void UChunkStreamDownloader::CloseFile()
{
	if (OpenFile)
	{
		// temp fallback check
		bool Expected = false;
		while (!bChunkPendingWrite.compare_exchange_strong(Expected,false))
		{
			LOG_ERROR("Attempting to close file while write still pending!");
			Expected=false;
			// writing is running so dont complete until its done, wait untill its free
			FPlatformProcess::Sleep(0.01);
		}
		///

		
		FScopeLock WriteLock(&WriteFileLock);
		OpenFile->Flush();
		delete OpenFile;
		OpenFile=nullptr;
	}
}

bool UChunkStreamDownloader::MoveTempFileToFinalSave()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UChunkStreamDownloader::MoveTempFileToFinalSave)
	if (FPaths::FileExists(*TempDownloadDir))
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		if (FPaths::FileExists(*FileSavePath))
		{
			IFileManager& FileManager = IFileManager::Get();
			if (FileManager.Delete(*FileSavePath,false,true))
			{
				LOG("MoveTempFileToFinalSave:: Deleted existing file %s",*FileSavePath);
			}
			else
			{
				LOG("MoveTempFileToFinalSave:: Failed trying to remove existing file %s \n May be open already",*FileSavePath);
				return false;
			}
		}
		FString Path, Filename, Extension;
		FPaths::Split(FileSavePath, Path, Filename, Extension);
		if (!PlatformFile.DirectoryExists(*Path))
		{
			if (!PlatformFile.CreateDirectoryTree(*Path))
			{
				LOG_ERROR("MoveTempFileToFinalSave::Unable to create a directory '%s' to save the downloaded file", *Path);
				return false;
			}
		}
		LOG("MoveTempFileToFinalSave:: Moving to final path %s",*FileSavePath);
		if (!PlatformFile.MoveFile(*FileSavePath,*TempDownloadDir))
		{
			LOG_ERROR("MoveTempFileToFinalSave:: Error Moving file %s \n to %s",*TempDownloadDir, *FileSavePath);
			return false;
		}
		return true;
	}
	return false;

}

FString UChunkStreamDownloader::GetTempPathForSavePath(const FString& SavePath)
{
	FString AbsoluteSavePath = FPaths::ConvertRelativePathToFull(SavePath);
	
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString SavedDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
    
	FString RelativePath;
    
	// check if the path is within the project directory
	if (AbsoluteSavePath.StartsWith(ProjectDir))
	{
		// get the relative path from project root
		RelativePath = AbsoluteSavePath.RightChop(ProjectDir.Len());
        
		//  remove any leading slashes
		while (RelativePath.StartsWith(TEXT("/")) || RelativePath.StartsWith(TEXT("\\")))
		{
			RelativePath = RelativePath.RightChop(1);
		}
		// Path is outside project dir  use filename with hash
		uint32 PathHash = GetTypeHash(RelativePath);
		return FPaths::ProjectSavedDir() / TEXT("temp") / 
			   FString::Printf(TEXT("%u_%s"), PathHash, *FPaths::GetCleanFilename(SavePath));
	}
	else
	{
		// Path is outside project dir - use filename with hash
		uint32 PathHash = GetTypeHash(AbsoluteSavePath);
		return FPaths::ProjectSavedDir() / TEXT("temp") / 
			   FString::Printf(TEXT("%u_%s"), PathHash, *FPaths::GetCleanFilename(SavePath));
	}
	
//	return FPaths::ProjectSavedDir() / TEXT("temp") / RelativePath;
}
