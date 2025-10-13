// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ChunkStreamTypes.h"
#include "Interfaces/IHttpRequest.h"

#if ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION >= 20
#define OLD_HTTP

#endif

#ifdef OLD_HTTP
	using FHttpProgressDelegate = FHttpRequestProgressDelegate;
	using FHttpRequestType = TSharedRef<IHttpRequest>;
	using BytesType = int32; 

#else
	using FHttpProgressDelegate = FHttpRequestProgressDelegate64;
	using FHttpRequestType =TSharedRef<IHttpRequest, ESPMode::ThreadSafe>;
	using BytesType = uint64; 
#endif


namespace StreamChunkDownloader
{
	/**
	 * Contains the data and metadata for a single downloaded chunk.
	 * Designed to be moved rather than copied to avoid unnecessary memory allocations
	 */
	struct FChunkInfo
	{
		// Raw bytes downloaded for this chunk
		TArray64<uint8> Data;
		
		// Byte offset where this chunk starts in the complete file
		uint64 StartOffset = 0;
		
		// Byte offset where this chunk ends in the complete file (inclusive)
		uint64 EndOffset = 0;
		
		// Total size of the file being downloaded (if known, api may not send total size)
		uint64 TotalFileSize = 0;
		
		FChunkInfo() = default;
		
		// NO COPY!
		FChunkInfo(const FChunkInfo& Other) = delete; 
		FChunkInfo &operator=(const FChunkInfo& Other) = delete; 
		// Use moves
		FChunkInfo(FChunkInfo&& Other) = default;
		FChunkInfo &operator=(FChunkInfo&& Other) = default;
	};
}

// Called periodically during download with bytes received and progress percentage (0.0 - 1.0)
DECLARE_DELEGATE_TwoParams(FStreamDownloadProgressSignature, uint64 /* bytes received*/, float);

// Called when a chunk completes and is ready to be written to disk
DECLARE_DELEGATE_OneParam(FOnSingleChunkCompleteSignature, TUniquePtr<StreamChunkDownloader::FChunkInfo>&& /* Chunk data*/);

// Called when the entire download finishes
DECLARE_DELEGATE_OneParam(FOnDownloadCompleteSignature, EChunkStreamDownloadResult);


/**
 * Handles downloading large files in chunks to avoid running out of memory.
 * 
 * This class streams data from HTTP endpoints and breaks it into manageable chunks that get 
 * handed off to the owner for processing (usually writing to disk). Supports both range-based 
 * chunking (when the API supports it) and stream-based chunking (for APIs that don't).

 */
class FStreamChunkDownloader : public TSharedFromThis<FStreamChunkDownloader>
{
public:
	FStreamChunkDownloader()=default;

	// Basic constructor - provide the URL and content type for the file you want to download
	FStreamChunkDownloader( const FString& InURL,const FString& InContentType) :
		URL(InURL), ContentType(InContentType), MaxChunkSize(100e+06), LastDataReceivedTime(0), TimeoutInSeconds(0),
		bCanceled(false),
		MaxRetryCount(3), CurrentRetryCount(0), RetryBackoffBaseSeconds(1.0f), RetryBackoffMultiplier(2.0f)
	{
	}

	~FStreamChunkDownloader();

	/**
	 * Starts the download process.
	 * 
	 * @param InMaxChunkSize - Maximum bytes per chunk (typically 1-100 MB depending on memory constraints)
	 * @param OnProgress - Callback for download progress updates
	 * @param OnSingleChunkComplete - Callback when each chunk finishes (for writing to disk, etc.)
	 * @param OnDownloadComplete - Callback when the entire download completes
	 * @return true if download started successfully
	 */
	bool BeginDownload(uint64 InMaxChunkSize,
		const FStreamDownloadProgressSignature& OnProgress,const FOnSingleChunkCompleteSignature& OnSingleChunkComplete, const FOnDownloadCompleteSignature& OnDownloadComplete );
	
	/**
	 * Sends a HEAD request to get the file size before downloading.
	 * Also checks if the API supports range requests and looks for content encoding.
	 * 
	 * @param InURL - URL to query
	 * @param Timeout - Request timeout in seconds (0 = no timeout)
	 * @return Future containing the HTTP response (or nullptr on failure)
	 */
	TFuture<const FHttpResponsePtr&> RequestDownloadTotalSize(const FString& InURL, float Timeout);

	/**
	 * Parses the Content-Length header from an HTTP response.
	 * Returns 0 if the header is missing, invalid, or the response has encoding (gzip/deflate).
	 */
	static uint64 GetFileSizeFromRequest( FHttpResponsePtr Response, bool bSuccess) ;

	/**
	 * Checks if the server supports HTTP range requests (Accept-Ranges header).
	 * Range requests let us download specific byte ranges, which is more reliable for large files.
	 */
	static bool DoesApiAcceptRanges( FHttpResponsePtr Response, bool bSuccess) ;
	
	/**
	 * Detects if the response uses content encoding like gzip or deflate.
	 * When encoding is present, Unreal automatically decompresses the stream,
	 * so we can't rely on Content-Length and should use actual received data sizes instead.
	 */
	static bool DoesResponseHaveEncoding(const FHttpResponsePtr& Response);
	
	// Stop the download if it is in progress
	bool CancelDownload();
	// Shutdown and cleanup without broadcasting any progress delegates
	void Shutdown();
	
	// Has the download been canceled
	bool IsCanceled() const { return bCanceled; }
	bool HasStarted() const { return bHasStarted;}
protected:

	// Cancels the download internally and notifies the owner with a specific reason
	void InternalCancelDownload(EChunkStreamDownloadResult Reason, const FString& ErrorMessage = FString(), bool bFromShutdown=false);
	
	// Called internally after we successfully retrieve the file size
	void OnTotalSizeReceived(const FHttpResponsePtr& Response);
	
	// Called internally if we fail to get the file size (will attempt download anyway)
	void OnFailedToGetTotalFileSize();
	
	// Validates that the current chunk's byte range is sensible before downloading
	bool IsValidChunkRange() const
	{
		check(ActiveChunk);
		if (!bUnknownTotalSize)
		{
			
			return ActiveChunk->EndOffset > 0
				&& ActiveChunk->StartOffset < ActiveChunk->EndOffset
				&&(ActiveChunk->EndOffset - ActiveChunk->StartOffset + 1) <= TotalFileSize;
		}
		else
		{
			return ActiveChunk->EndOffset > 0
				&& ActiveChunk->StartOffset < ActiveChunk->EndOffset;
		}
	}
	
	/* Kicks off an HTTP request to download the current chunk
	 * Future completed when the chunk has been downloaded and handed off.
	 */
	TFuture<bool> DownloadChunk( );

	bool ValidateStatusCode();
	
	// Handles progress updates during a chunk download and forwards to the owners callback
	void OnChunkDownloadProgress(const BytesType& BytesReceived);
	
	// Called when a chunk request finishes
	void ChunkDownloadRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);
	
	// Figures out if there are more chunks to download and starts the next one
	void ProcessNextChunk();
	
	// Called when all chunks have been downloaded successfully
	void OnAllChunksDownloaded();
	
	// Passes the completed chunk to the owner and updates tracking offsets
	void HandOffActiveChunk();
	
	// Allocates and initializes a fresh chunk for the next download segment
	void InitNewChunk();
	
	/**
	 * Callback for HTTP streaming - receives data as it arrives from the network.
	 * Copies incoming bytes into the active chunk's buffer. Can be called multiple times per chunk.
	 * Note: This runs on the HTTP module's thread, not the game thread.
	 */
	void OnChunkStream(void* DataPtr, int64& InOutLength);
	// Stall detection incase of network problems
	void CheckForStall();
	
	// Calculates the retry delay using exponential backoff
	float CalculateRetryDelay() const;
	
	// Retries the current chunk download after a delay
	void RetryChunkDownload();
	
	// Helper to create and configure an HTTP request with appropriate headers
	static FHttpRequestType MakeHttpRequest( const FString& URL,
		const FString& Verb,
		float Timeout,
		const FString& ContentType);
	
	// Returns the progress delegate in a way that works across UE4/UE5
	static FHttpProgressDelegate& GetHttpProgressDelegate(FHttpRequestType& Request)
	{
#ifdef OLD_HTTP
		return Request->OnRequestProgress();
#else
		return Request->OnRequestProgress64();
#endif
	}
	
	// Helper to get a weak pointer to this object (for safe async callbacks). DO NOT CALL IN CONSTRUCTOR!
	TWeakPtr<FStreamChunkDownloader> GetWeakThis() { return AsShared().ToWeakPtr(); };
	
	// Callbacks provided by the owner
	FStreamDownloadProgressSignature OnProgressDelegate;
	FOnSingleChunkCompleteSignature OnSingleChunkCompleteDelegate;
	FOnDownloadCompleteSignature OnDownloadCompleteDelegate;
	
	// Protects ActiveChunk from concurrent access during streaming (HTTP thread) and handoff
	FCriticalSection ChunkDataLock;

	FTSTicker::FDelegateHandle StallTickHandle;
	FTSTicker::FDelegateHandle RetryHandle;
	
	TFuture<bool> CurrentDownloadFuture;
	
	// Reference to the current HTTP request 
	TWeakPtr<IHttpRequest> CurrentHttpRequest;
	
	// The URL were downloading from
	FString URL;
	
	// Content type to set in request headers
	FString ContentType;
	
	// Type of encoding detected in response (gzip, deflate, etc.)
	FString ResponseEncodingType;
	
	// Maximum chunk size in bytes (configured at download start)
	uint64 MaxChunkSize;
	
	// Currently downloading chunk - gets handed off when full or when download completes
	TUniquePtr<StreamChunkDownloader::FChunkInfo> ActiveChunk;
	
	// Last time in seconds we got any data
	double LastDataReceivedTime;
	
	// Write position within the current chunk (atomic because streaming happens on HTTP thread)
	std::atomic<uint64> CurrentChunkOffset = 0;
	
	// Last byte offset that was completed in previous chunks (used to calculate next chunk range)
	uint64 LastChunkEndOffset = 0;
	
	// Total size of the file (0 if unknown)
	uint64 TotalFileSize = 0;
	// Time in seconds between no data recieved to decide its stalled and try restart the chunk
	float StallDetectionTimeout=14.0f;
	
	// HTTP request timeout in seconds
	float TimeoutInSeconds=0.0f;
	
	// Response code from the chunk request (200, 206, 404, etc.)
	std::atomic<int32> ChunkDownloadResponseCode{0}; 

	// Set to true when user calls CancelDownload()
	bool bCanceled = false;
	
	// True if we couldn't get Content-Length from the server
	bool bUnknownTotalSize = false;
	
	// True if server sent Accept-Ranges header
	bool bApiAcceptsRanges = false;
	
	// Whether to actually use range requests (disabled for small files or if API doesn't support it)
	bool bShouldUseRanges = true;

	bool bHasStarted = false;
	// Tracks if the last chunk received any data (used for unknown-size downloads)
	bool bLastChunkHadData = true;
	// Did the last chunk end its stream before expected end range, if so the file should be complete
	bool bLastChunkCompletedEarly = false;
	
	// Calculates the byte count for the current chunk
	uint64 CalculateRange() const { return ActiveChunk->EndOffset - ActiveChunk->StartOffset + 1; }
	
	// Extra space reserved in chunk buffers to handle APIs that send slightly more data than requested
	uint64 BufferPadding  = 4096 *4;
	
	// Retry mechanism configuration and tracking
	// Maximum number of retry attempts per chunk before giving up
	int32 MaxRetryCount = 3;
	
	// Current retry attempt for the active chunk (0 = first attempt, not a retry)
	int32 CurrentRetryCount = 0;
	
	// Base delay in seconds before first retry (subsequent retries use exponential backoff)
	float RetryBackoffBaseSeconds = 1.0f;
	
	// Multiplier for exponential backoff (delay = base * multiplier^retryCount)
	float RetryBackoffMultiplier = 2.0f;
};