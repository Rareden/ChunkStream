// Copyright (C) 2025 Isaac Cooper - All Rights Reserved

#pragma once

#if !defined(CHUNKSTREAM_API)
	#error "ChunkStreamLogs.h should only be included from within the plugin module"
#endif


DECLARE_LOG_CATEGORY_CLASS(LogChunkStream, Log, All);

#if !PLATFORM_ANDROID
#define FUNC __FUNCTION__
#else
#define FUNC __func__
#endif

// Undefine any existing macros
#undef LOG
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_VERBOSE
//


#define LOG(Format,...) UE_LOG(LogChunkStream,Display,TEXT("%s: %s"),*FString(FUNC),*FString::Printf(TEXT(Format),##__VA_ARGS__))
#define LOG_WARN(Format,...) UE_LOG(LogChunkStream,Warning,TEXT("%s: %s"),*FString(FUNC),*FString::Printf(TEXT(Format),##__VA_ARGS__))

#define LOG_ERROR(Format,...) UE_LOG(LogChunkStream,Error,TEXT("%s: %s"),*FString(FUNC),*FString::Printf(TEXT(Format),##__VA_ARGS__))
#define LOG_VERBOSE(Format,...) UE_LOG(LogChunkStream,Verbose,TEXT("%s: %s"),*FString(FUNC),*FString::Printf(TEXT(Format),##__VA_ARGS__))


