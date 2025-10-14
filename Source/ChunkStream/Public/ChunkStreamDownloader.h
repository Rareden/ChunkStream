// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StreamChunkDownloader.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "UObject/Object.h"
#include "ChunkStreamDownloader.generated.h"

class FChunkStreamDownloaderUtils
{
public:
	static uint64 GetMaxChunkSize();
};

USTRUCT(BlueprintType)
struct FChunkStreamResultParams
{
	GENERATED_BODY()
	// The Downloader object.
	UPROPERTY(BlueprintReadOnly)
	TObjectPtr<class UChunkStreamDownloader> Downloader;
	// 0 -> 1 Progress of the download
	UPROPERTY(BlueprintReadOnly)
	float Progress;
	UPROPERTY(BlueprintReadOnly)
	int32 HttpStatusCode;
	// Current download task result 
	UPROPERTY(BlueprintReadOnly)
	EChunkStreamDownloadResult DownloadTaskResult;
	
};

DECLARE_MULTICAST_DELEGATE_OneParam(FNativeStreamOnDownloadProgress, FChunkStreamResultParams);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FStreamOnDownloadProgress, FChunkStreamResultParams,ResultParams);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FStreamOnDownloadFinished, FChunkStreamResultParams, ResultParams);
/**
 * 
 */
UCLASS()
class CHUNKSTREAM_API UChunkStreamDownloader : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	virtual void BeginDestroy() override;
	virtual bool IsReadyForFinishDestroy() override;
	
	/** Download a file to storage with chunk streaming.
	 * @param URL : HTTPS URL to download the file from
	 * @param ContentType : Content type for the request, can be left empty if unknown
	 * @param FileSavePathAndName : Location to save to with the file name, eg C:/MyGame/MyFile.mp4
	 */
	UFUNCTION(BlueprintCallable,Category = "ChunkStreamDownloader",meta=(BlueprintInternalUseOnly=true,WorldContext="WorldContext",DefaultToSelf="WorldContext",HidePin="WorldContext"))
	static UChunkStreamDownloader* DownloadFileToStorage(const UObject* WorldContext,const FString& URL,
		const FString& ContentType = TEXT("application/json"), const FString& FileSavePathAndName = TEXT("")
			);


	UFUNCTION(BlueprintCallable, Category = "ChunkStreamDownloader")
	static FString LoadFileToString(const FString FilePath);
	
	virtual void Activate() override;
	// native / Non BP delegate for progress
	FNativeStreamOnDownloadProgress Native_DownloadProgress;
	// native / Non BP delegate for when download is complete
	FNativeStreamOnDownloadProgress Native_DownloadFinished;
	
	UPROPERTY(BlueprintAssignable)
	FStreamOnDownloadProgress OnProgress;
	UPROPERTY(BlueprintAssignable)
	FStreamOnDownloadFinished OnComplete;

	UFUNCTION(BlueprintCallable, Category = "ChunkStreamDownloader")
	bool CancelDownload();
	UFUNCTION(BlueprintCallable, Category = "ChunkStreamDownloader")
	float GetProgress() const { return CurrentResultParams.Progress; };
	UFUNCTION(BlueprintCallable, Category = "ChunkStreamDownloader")
	bool IsComplete() const { return bCompleted; }
	UFUNCTION(BlueprintCallable, Category = "ChunkStreamDownloader")
	bool IsActive() const;
	
	UPROPERTY(BlueprintReadOnly)
	FString URL;
	UPROPERTY(BlueprintReadOnly)
	FString ContentType;
	// Where to save the file, name and extension included: eg C:/MyGame/Video.mp4
	UPROPERTY(BlueprintReadOnly)
	FString FileSavePath;
	UPROPERTY(BlueprintReadOnly)
	bool bCanceled;
	
protected:
	void OnDownloadProgress(uint64 BytesReceived , float InProgress);
	void OnChunkCompleted(TUniquePtr<StreamChunkDownloader::FChunkInfo>&&  ChunkData);
	void WriteChunkToFile(TUniquePtr<StreamChunkDownloader::FChunkInfo>&&  ChunkData);
	void OnDownloadComplete(EChunkStreamDownloadResult Result);
	void Completed(EChunkStreamDownloadResult InResult);
	bool OpenFileForWriting(const FString& InFilePath);
	void CloseFile();
	/*
	 *  Move the file from Temp location to FileSavePath
	 *  @param return: Returns true if moved, false if failed
	 */
	bool MoveTempFileToFinalSave();
	/* 
	 * Get a temp file name for this save path for the download to stream to
	 */
	static FString GetTempPathForSavePath(const FString& SavePath);

	UPROPERTY()
	FChunkStreamResultParams CurrentResultParams;
	
	FCriticalSection WriteFileLock;
	FString TempDownloadDir;
	TSharedPtr<class FStreamChunkDownloader> StreamChunkDownloader;
	IFileHandle* OpenFile = nullptr;

	bool bCompleted = false;
	std::atomic<bool> bChunkPendingWrite{false};
};
