// Copyright (C) 2025 Isaac Cooper - All Rights Reserved

#pragma once

#include "ChunkStreamTypes.generated.h"

// Reasons why a download might be cancelled internally
UENUM(BlueprintType)
enum class EChunkStreamDownloadResult : uint8
{
	None = 0,
	InvalidStatusCode,      // HTTP status code indicates failure (404, 403, 500, etc.)
	ValidationFailed,       // General validation failure
	FileSystemError,        // Error with opening or writing a file on storage, 
	NetworkError,           // Network/connection error
	InvalidResponse,        // Response data is invalid or malformed
	InsufficientDiskSpace,  // Not enough disk space available to write chunk
	
	UserCancelled   ,       // User explicitly called CancelDownload()
	WaitingForOtherDownload, //  Max active downloads is currently reached, so this task is waiting for a space
	InProgress,
	Success // Download completed
};
