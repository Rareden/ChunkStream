// Fill out your copyright notice in the Description page of Project Settings.


#include "StreamChunkDownloader.h"
#include "ChunkStreamLogs.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Containers/Ticker.h"

FStreamChunkDownloader::~FStreamChunkDownloader()
{
	LOG_VERBOSE("Streamer destroying");

}

bool FStreamChunkDownloader::BeginDownload(uint64 InMaxChunkSize, const FStreamDownloadProgressSignature& OnProgress,
	const FOnSingleChunkCompleteSignature& OnSingleChunkComplete, const FOnDownloadCompleteSignature& OnDownloadComplete )
{
	OnProgressDelegate = OnProgress;
	OnSingleChunkCompleteDelegate = OnSingleChunkComplete;
	OnDownloadCompleteDelegate = OnDownloadComplete;
	MaxChunkSize = InMaxChunkSize;
	

	auto pWeakThis = GetWeakThis();

	
	RequestDownloadTotalSize(URL, 0.f)
		.Next([pWeakThis](const FHttpResponsePtr& Response)
		{
			if (pWeakThis.IsValid())
			{
				if (Response)
				{
					auto Downloader = pWeakThis.Pin();
					Downloader->OnTotalSizeReceived(Response);
				}
				else
				{
					LOG_ERROR("RequestDownloadTotalSize::Next:: Invalid response.");
					pWeakThis.Pin()->InternalCancelDownload(EChunkStreamDownloadResult::InvalidResponse, 
						TEXT("Failed to receive valid response from initial HEAD request"));
				}
				
			}
			else
			{
				LOG_ERROR("RequestDownloadTotalSize::Next:: Invalid downloader, halting.");
			}
		});
	bHasStarted=true;
	return true;
}

TFuture<const FHttpResponsePtr&> FStreamChunkDownloader::RequestDownloadTotalSize(const FString& InURL, float Timeout)
{
	FHttpRequestType NewRequest = MakeHttpRequest(InURL,TEXT("HEAD"),Timeout,TEXT(""));

	auto Promise = MakeShared<TPromise<const FHttpResponsePtr&>>();
	
	auto pWeakThis = GetWeakThis();
	NewRequest->OnProcessRequestComplete().BindLambda([pWeakThis = MoveTemp(pWeakThis), Promise]
		(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
		{
			if (pWeakThis.IsValid())
			{
				Promise->SetValue(Response);
			}
			else
			{
				Promise->SetValue(nullptr);
			}
		});

	if (!NewRequest->ProcessRequest())
	{
		
		LOG_ERROR("Failed to get content size from URL '%s'",*InURL);
		// return error
		return MakeFulfilledPromise<const FHttpResponsePtr&>(nullptr).GetFuture();
	}
	
	return Promise->GetFuture();
}

uint64 FStreamChunkDownloader::GetFileSizeFromRequest( FHttpResponsePtr Response,
	bool bSuccess) 
{
	if (!bSuccess || !Response.IsValid())
	{
		LOG_WARN("Invalid response, cant get file size.");
		return 0;
	}
	
	if (DoesResponseHaveEncoding(Response))
		return 0;
	
	if (FString Header = Response->GetHeader(TEXT("Content-Length")); !Header.IsEmpty())
	{
		if (Header.IsNumeric())
		{
			if (Header.StartsWith(TEXT("-")))
			{
				LOG_WARN("Content-Length header responded with negative value '%s'. HTTP spec violation!",*Header);
				return 0;
			}
			uint64 ContentLength = FCString::Strtoui64(*Header, nullptr, 10);
			
			return ContentLength;
		}
		else
		{
			LOG_WARN("Content-Length header is non numeric '%s'  Can only parse numeric strings for file size.",*Header);
		}
	}
	
	return 0;
}

bool FStreamChunkDownloader::DoesApiAcceptRanges( FHttpResponsePtr Response, bool bSuccess)
{
	bool Result = false;
	if (!bSuccess || !Response.IsValid())
	{
		return false;
	}

	if (DoesResponseHaveEncoding(Response))
		return false;
	
	if (FString Header = Response->GetHeader(TEXT("Accept-Ranges")); !Header.IsEmpty())
	{
		Result=true;
	}
	return Result;
}

bool FStreamChunkDownloader::DoesResponseHaveEncoding(const FHttpResponsePtr& Response)
{
	if (!Response)
		return false;
	if (FString Encoding = Response->GetHeader(TEXT("Content-Encoding")); !Encoding.IsEmpty() && !Encoding.Equals(TEXT("identity"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	return false;
}

void FStreamChunkDownloader::OnTotalSizeReceived(const FHttpResponsePtr& Response)
{
	TotalFileSize = GetFileSizeFromRequest( Response, true);
	bApiAcceptsRanges = DoesApiAcceptRanges( Response, true);
	bUnknownTotalSize = TotalFileSize == 0;
	ChunkDownloadResponseCode.store(Response->GetResponseCode());
	
	if (!ValidateStatusCode())
	{
		int32 StatusCode = ChunkDownloadResponseCode.load();
		InternalCancelDownload(EChunkStreamDownloadResult::InvalidStatusCode, 
			FString::Printf(TEXT("Status code %d during initial request"), StatusCode));
		return;
	}
	if (bUnknownTotalSize)
	{
		bShouldUseRanges = false;
		LOG("Failed to get total file size!");
		if (!bApiAcceptsRanges)
		{
			LOG("API doesn't accept ranges. Falling back to non-range streaming mode.");
		}
		else
		{
			LOG("API accepts ranges, but size is unknown. Streaming without ranges");
		}
	}
	else
	{
		LOG("File Size received...");
	}
	if (TotalFileSize < MaxChunkSize)
	{
		bShouldUseRanges=false;
	}
	// Init chunk params
	InitNewChunk();
	CurrentChunkOffset.store(0);
	LastChunkEndOffset = 0;
	
	auto pWeakThis = GetWeakThis();
	// setup stall detection
	StallTickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([pWeakThis](float DT) -> bool
	{

		if (pWeakThis.IsValid() && !pWeakThis.Pin()->IsCanceled())
		{
			pWeakThis.Pin()->CheckForStall();
			return true;
		}
		return false; // dont repeat tick
		
	}), 5.0f);
	
	
	ProcessNextChunk();
}

void FStreamChunkDownloader::OnFailedToGetTotalFileSize()
{
	
}

bool FStreamChunkDownloader::CancelDownload()
{

	InternalCancelDownload(EChunkStreamDownloadResult::UserCancelled, FString());
	bCanceled = true;
	return true;
}

void FStreamChunkDownloader::Shutdown()
{
	InternalCancelDownload(EChunkStreamDownloadResult::UserCancelled, FString(),true);
}

void FStreamChunkDownloader::InternalCancelDownload(EChunkStreamDownloadResult Reason, const FString& ErrorMessage, bool bFromShutdown)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamChunkDownloader::InternalCancelDownload)
	if (bCanceled)
	{
		// Already canceled, don't duplicate the notification
		return;
	}
	if (CurrentHttpRequest.IsValid())
	{
		CurrentHttpRequest.Pin()->CancelRequest();
		CurrentHttpRequest.Reset();
	}
	if (CurrentDownloadFuture.IsValid())
	{
		CurrentDownloadFuture.Reset();
	}
	
	FTSTicker::GetCoreTicker().RemoveTicker(StallTickHandle);
	bCanceled = true;
	if (!bFromShutdown)
	{
		switch (Reason)
		{
		case EChunkStreamDownloadResult::InvalidStatusCode:
			LOG_ERROR("Download canceled due to invalid status code. %s", *ErrorMessage);
			break;
		case EChunkStreamDownloadResult::ValidationFailed:
			LOG_ERROR("Download canceled due to validation failure. %s", *ErrorMessage);
			break;
		case EChunkStreamDownloadResult::NetworkError:
			LOG_ERROR("Download canceled due to network error. %s", *ErrorMessage);
			break;
		case EChunkStreamDownloadResult::InvalidResponse:
			LOG_ERROR("Download canceled due to invalid response. %s", *ErrorMessage);
			break;
		case EChunkStreamDownloadResult::UserCancelled:
			LOG("Download canceled by user.");
			break;
		case EChunkStreamDownloadResult::InsufficientDiskSpace:
			LOG_ERROR("Download canceled due to insufficient disk space. %s", *ErrorMessage);
			break;
		default:
			LOG_ERROR("Download canceled for unknown reason. %s", *ErrorMessage);
			break;
		}
	
	}
	OnProgressDelegate.Unbind();
		
	// Notify the owner that the download failed
	if (OnDownloadCompleteDelegate.IsBound() && !bFromShutdown)
	{
		OnDownloadCompleteDelegate.Execute(Reason);
	}
}

TFuture<bool> FStreamChunkDownloader::DownloadChunk()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamChunkDownloader::DownloadChunk)
	if (bCanceled)
	{
		return MakeFulfilledPromise<bool>(false).GetFuture() ;
	}

	if (!IsValidChunkRange())
	{
		LOG_ERROR("Invalid chunk range to download \n\r Range {%llu-%llu} \n\r URL '%s", ActiveChunk->StartOffset, ActiveChunk->EndOffset, *URL);
		return MakeFulfilledPromise<bool>(false).GetFuture() ;
	}

	auto NewRequest = MakeHttpRequest( URL,TEXT("GET"), TimeoutInSeconds, ContentType);
	
	if (bApiAcceptsRanges && bShouldUseRanges)
	{
		NewRequest->SetHeader(TEXT("Range"),
		FString::Printf(TEXT("bytes=%llu-%llu"), ActiveChunk->StartOffset, ActiveChunk->EndOffset));
	
	}
	
	auto pWeakThis = GetWeakThis();

	NewRequest->OnStatusCodeReceived()
		.BindLambda([pWeakThis](FHttpRequestPtr Request, int32 StatusCode)
		{
			if (pWeakThis.IsValid())
			{
				pWeakThis.Pin()->ChunkDownloadResponseCode.store(StatusCode);
				pWeakThis.Pin()->ValidateStatusCode();
			}
		});
	// on progressed event
	GetHttpProgressDelegate(NewRequest)
		.BindLambda([pWeakThis](FHttpRequestPtr Request, BytesType BytesSent, BytesType BytesReceived)
		{
			if (pWeakThis.IsValid())
			{
				pWeakThis.Pin()->OnChunkDownloadProgress(BytesReceived);
			}
		});

	auto Promise = MakeShared<TPromise<bool>>();
	// on request complete event
	NewRequest->OnProcessRequestComplete()
		.BindLambda([pWeakThis,Promise](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
		{
			if (pWeakThis.IsValid())
			{
				pWeakThis.Pin()->ChunkDownloadRequestComplete(Request, Response, bSuccess);

				Promise->SetValue(bSuccess);
			}
			else
			{
				Promise->SetValue(false);
			}
		});
	// delegate that http will stream the data to instead of caching in response
	FHttpRequestStreamDelegateV2 StreamDelegate = FHttpRequestStreamDelegateV2::CreateSP(this,&FStreamChunkDownloader::OnChunkStream);
	NewRequest->SetResponseBodyReceiveStreamDelegateV2(StreamDelegate);
	CurrentHttpRequest = NewRequest;
	// start request
	if (!NewRequest->ProcessRequest())
	{
		LOG_ERROR("Failed to start chunk download \n\r Range {%llu-%llu} \n\r URL '%s", ActiveChunk->StartOffset, ActiveChunk->EndOffset, *URL);
		CurrentHttpRequest.Reset();
		return MakeFulfilledPromise<bool>(false).GetFuture() ;
	}
	
	return Promise->GetFuture();
}

bool FStreamChunkDownloader::ValidateStatusCode()
{
	int32 StatusCode = ChunkDownloadResponseCode.load();
	if (StatusCode == 200 || StatusCode == 206 || StatusCode == 201)
	{
		LOG_VERBOSE("Status Code is good %d",StatusCode);
		return true;
	}
	else if (StatusCode == 404)
	{
		LOG_WARN("Status Code %d Not found",StatusCode);
	}
	else if (StatusCode == 400)
	{
		LOG_WARN("Status Code %d Bad Request",StatusCode);

	}
	else if (StatusCode == 403)
	{
		LOG_WARN("Status Code %d Forbidden",StatusCode);

	}
	else if (StatusCode == 500)
	{
		LOG_WARN("Status Code %d Server Error",StatusCode);
	
	}
	else if (StatusCode == 503)
	{
		LOG_WARN("Status Code %d Service Unavailable",StatusCode);
	
	}

	return false;
}

void FStreamChunkDownloader::OnChunkDownloadProgress(const BytesType& BytesReceived)
{
	const double Progress = TotalFileSize <= 0 ? 0.0f : static_cast<double>(BytesReceived + ActiveChunk->StartOffset) /  static_cast<double>(TotalFileSize);
	if (OnProgressDelegate.IsBound())
	{
		OnProgressDelegate.Execute(BytesReceived,static_cast<float>(Progress));
	}
}

void FStreamChunkDownloader::ChunkDownloadRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response,
	bool bSuccess)
{
	FScopeLock Lock(&ChunkDataLock);
	if (bCanceled)
	{
		// drop data if canceled
		ActiveChunk.Reset();
		return;
	}
	
	if (bSuccess && CurrentChunkOffset.load() > 0)
	{
		LOG("Chunk range complete... handing off...");
		
		bLastChunkHadData = true;
		HandOffActiveChunk();
	}
	else
	{
		// No data received in this chunk
		bLastChunkHadData = false;
	}
}

void FStreamChunkDownloader::ProcessNextChunk()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamChunkDownloader::ProcessNextChunk)
	if (!ValidateStatusCode())
	{
		int32 StatusCode = ChunkDownloadResponseCode.load();
		InternalCancelDownload(EChunkStreamDownloadResult::InvalidStatusCode, 
			FString::Printf(TEXT("Status code %d during chunk download"), StatusCode));
		return;
	}

	if (bCanceled)
	{
		LOG("Download canceled. halting chunk download.");
		return;
	}
	
	// Determine if more chunks are needed
	bool bShouldContinue = false;
	
	if (bUnknownTotalSize)
	{
		// When total size is unknown, continue downloading as long as we are receiving data
		// If LastChunkEndOffset is 0, we haven't started yet, so continue
		// Otherwise, check if we received any data in the last chunk AND it didn't complete early
		// If a chunk completed early (received less than MaxChunkSize), the file has ended
		bShouldContinue = (LastChunkEndOffset == 0) || (bLastChunkHadData && !bLastChunkCompletedEarly);
		
		if (!bShouldContinue)
		{
			if (bLastChunkCompletedEarly)
			{
				LOG("Last chunk received less data than expected. Download complete for unknown-size file.");
			}
			else
			{
				LOG("No data received in last chunk. Assuming download complete for unknown-size file.");
			}
		}
	}
	else
	{
		// When total size is known, check if we have downloaded everything
		// Also stop if a chunk completed early (file ended before expected)
		bShouldContinue = (LastChunkEndOffset + 1 < TotalFileSize) && !bLastChunkCompletedEarly;
	}
	
	if (bShouldContinue)
	{
		InitNewChunk();
		
		auto pWeakThis = GetWeakThis();
		(CurrentDownloadFuture = DownloadChunk())
			.Next([pWeakThis](bool LastChunkSucceded)
			{
				if (pWeakThis.IsValid() )
				{
					if (LastChunkSucceded)
					{
						// Reset retry counter on success
						pWeakThis.Pin()->CurrentRetryCount = 0;
						pWeakThis.Pin()->ProcessNextChunk();
					}
					else if ( !pWeakThis.Pin()->IsCanceled())
					{
						// Try to retry if we haven't exceeded max retries
						auto Downloader = pWeakThis.Pin();
						if (Downloader->CurrentRetryCount < Downloader->MaxRetryCount)
						{
							LOG_WARN("Chunk download failed. Attempting retry %d/%d", 
								Downloader->CurrentRetryCount + 1, Downloader->MaxRetryCount);
							Downloader->RetryChunkDownload();
						}
						else
						{
							LOG_ERROR("Chunk download failed after %d retries", Downloader->MaxRetryCount);
							Downloader->InternalCancelDownload(EChunkStreamDownloadResult::NetworkError, 
								FString::Printf(TEXT("Chunk download failed after %d retries"), Downloader->MaxRetryCount));
						}
					}
				}
			});
	}
	else
	{
		// all chunks downloaded
		OnAllChunksDownloaded();
	}
}

void FStreamChunkDownloader::OnAllChunksDownloaded()
{
	FTSTicker::GetCoreTicker().RemoveTicker(StallTickHandle);
	OnDownloadCompleteDelegate.ExecuteIfBound(EChunkStreamDownloadResult::Success);
}

void FStreamChunkDownloader::HandOffActiveChunk()
{
	LLM_SCOPE_BYNAME("ChunkStream/HandOffActiveChunk");
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamChunkDownloader::HandOffActiveChunk)
	TUniquePtr<StreamChunkDownloader::FChunkInfo> ChunkToProcess = MoveTemp(ActiveChunk);
	uint64 CurrentChunkOffsetVal = CurrentChunkOffset.load();
	CurrentChunkOffsetVal+=ChunkToProcess->StartOffset;
	// if actual data amount is different
	if (CurrentChunkOffsetVal != ChunkToProcess->EndOffset + 1)
	{
		// new range of the actual amount of data received (CurrentChunkOffsetVal) diffeers to chunk preallocated length
		ChunkToProcess->EndOffset = ChunkToProcess->StartOffset + CurrentChunkOffset.load() - 1;
		
		bLastChunkCompletedEarly=true;
	}
	
	LastChunkEndOffset = ChunkToProcess->EndOffset;

	OnSingleChunkCompleteDelegate.Execute(MoveTemp(ChunkToProcess));
}

void FStreamChunkDownloader::InitNewChunk()
{
	LLM_SCOPE_BYNAME("ChunkStream/InitNewChunk");
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamChunkDownloader::InitNewChunk)
	// make fresh chunk
	ActiveChunk = MakeUnique<StreamChunkDownloader::FChunkInfo>();
	bLastChunkCompletedEarly = false;
	uint64 EndSize = !bUnknownTotalSize? TotalFileSize : MaxChunkSize;
	
	// Check if more chunks needed
	if (LastChunkEndOffset + 1 < EndSize)
	{
		// Update chunk range for next download
		ActiveChunk->StartOffset = LastChunkEndOffset == 0? 0 : LastChunkEndOffset + 1;
		if (!bUnknownTotalSize)
			ActiveChunk->EndOffset = FMath::Min(ActiveChunk->StartOffset + MaxChunkSize, TotalFileSize) - 1;
		else
			ActiveChunk->EndOffset = ActiveChunk->StartOffset + MaxChunkSize;
		
		ensure(ActiveChunk->EndOffset > ActiveChunk->StartOffset);
		ActiveChunk->TotalFileSize=TotalFileSize;
		ActiveChunk->Data.Reserve(CalculateRange() + BufferPadding);
		ActiveChunk->Data.SetNumUninitialized(CalculateRange(),EAllowShrinking::No);
		CurrentChunkOffset.store(0);
	}
}

void FStreamChunkDownloader::OnChunkStream(void* DataPtr, int64& InOutLength)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamChunkDownloader::OnChunkStream)
	FScopeLock Lock(&ChunkDataLock);
	if (!ActiveChunk)
	{
		LOG_ERROR("Data still streaming but no active chunk!")
		return;
	}
	const uint64 ExpectedChunkBytes = (ActiveChunk->EndOffset - ActiveChunk->StartOffset) + 1;
    uint64 CurrentChunkOffsetVal = CurrentChunkOffset.load();
	check( static_cast<int64>(CurrentChunkOffsetVal) <= ActiveChunk->Data.Num());
	
	LastDataReceivedTime = FPlatformTime::Seconds();
	// within current range
	if (CurrentChunkOffsetVal + static_cast<uint64>(InOutLength) <= ExpectedChunkBytes )
	{
		FMemory::Memcpy(ActiveChunk->Data.GetData() + CurrentChunkOffsetVal, DataPtr, static_cast<uint64>(InOutLength));
		CurrentChunkOffset.store(CurrentChunkOffsetVal +  static_cast<uint64>(InOutLength));
	}
	else
	{
		// incoming data goes beyond the initialized array size, add more to the end which should still be within the safe reserved region
		
		int64 Difference = (static_cast<int64>(CurrentChunkOffsetVal) + InOutLength) - ActiveChunk->Data.Num();
		ActiveChunk->Data.AddZeroed(Difference);
		ActiveChunk->EndOffset+=Difference;
		
		LOG_WARN("Api appended %lld more elements to data buffer",Difference);
		check(static_cast<int64>(CurrentChunkOffsetVal) + InOutLength <= ActiveChunk->Data.Num());
		
		FMemory::Memcpy(ActiveChunk->Data.GetData() + CurrentChunkOffsetVal, DataPtr, static_cast<uint64>(InOutLength));
		CurrentChunkOffset.store(CurrentChunkOffsetVal +  static_cast<uint64>(InOutLength));
		
		LOG_WARN("Api Stream overflow from requested range, end is now %llu : expected end %llu",CurrentChunkOffsetVal,ActiveChunk->EndOffset - ActiveChunk->StartOffset);

		ensure(ActiveChunk->EndOffset > ActiveChunk->StartOffset);
		
		HandOffActiveChunk();
		InitNewChunk();
	}
	
}

void FStreamChunkDownloader::CheckForStall()
{
	double CurrentTime = FPlatformTime::Seconds();
	if ( CurrentTime - LastDataReceivedTime >= StallDetectionTimeout)
	{
		// Try to retry if we haven't exceeded max retries
		if (CurrentRetryCount < MaxRetryCount)
		{
		
			LOG_WARN("Stream download stalled. Attempting retry %d/%d",CurrentRetryCount +1, MaxRetryCount);
			
			// Cancel current request
			if (auto Request = CurrentHttpRequest.Pin())
			{
				Request->CancelRequest();
			}
			
			RetryChunkDownload();
		}
		else
		{
			LOG_ERROR("Stream download timed out after %d retries", MaxRetryCount);
			InternalCancelDownload(EChunkStreamDownloadResult::NetworkError,
				FString::Printf(TEXT("Stream download timed out after %d retries"), MaxRetryCount));
		}
	}
}

float FStreamChunkDownloader::CalculateRetryDelay() const
{
	// Exponential backoff: delay = base * multiplier^retryCount
	return RetryBackoffBaseSeconds * FMath::Pow(RetryBackoffMultiplier, static_cast<float>(CurrentRetryCount));
}

void FStreamChunkDownloader::RetryChunkDownload()
{
	CurrentRetryCount++;
	
	// Calculate delay using exponential backoff
	float DelaySeconds = CalculateRetryDelay();
	
	LOG("Retrying chunk download after %.2f seconds (attempt %d/%d)", DelaySeconds, CurrentRetryCount, MaxRetryCount);
	
	// Reset chunk state for retry
	CurrentChunkOffset.store(0);
	
	// Use a timer to delay the retry
	auto pWeakThis = GetWeakThis();
	RetryHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([pWeakThis](float DeltaTime) -> bool
	{
		if (pWeakThis.IsValid() && !pWeakThis.Pin()->IsCanceled())
		{
			auto Downloader = pWeakThis.Pin();
			FTSTicker::GetCoreTicker().RemoveTicker(Downloader->RetryHandle);
			// Start the download again
			(Downloader->CurrentDownloadFuture = Downloader->DownloadChunk())
				.Next([pWeakThis](bool bSuccess)
				{
					if (pWeakThis.IsValid())
					{
						if (!pWeakThis.Pin()->ActiveChunk.IsValid())
						{
							pWeakThis.Pin()->InitNewChunk();
						}
						if (bSuccess)
						{
							
							pWeakThis.Pin()->CurrentRetryCount = 0;
							pWeakThis.Pin()->ProcessNextChunk();
						}
						else
						{
							// Retry or fail will be handled in ProcessNextChunk
							pWeakThis.Pin()->ProcessNextChunk();
						}
					}
				});
		}
		return false; // Don't repeat the ticker
	}), DelaySeconds);
}

FHttpRequestType FStreamChunkDownloader::MakeHttpRequest(const FString& URL, const FString& Verb, float Timeout,
                                                         const FString& ContentType)
{
	FHttpRequestType Request = FHttpModule::Get().CreateRequest();
	
	Request->SetVerb(Verb);
	Request->SetURL(URL);
	if (!ContentType.IsEmpty())
		Request->SetHeader("Content-Type", ContentType);
#ifndef OLD_HTTP
	Request->SetTimeout(Timeout);
#endif

	return Request;
}
