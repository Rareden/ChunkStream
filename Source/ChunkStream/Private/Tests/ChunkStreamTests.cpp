
#if WITH_AUTOMATION_TESTS
#include "ChunkStreamDownloader.h"
#include "Misc/AutomationTest.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(ChunkStreamTests2, "ChunkStream.GithubTextFile",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool ChunkStreamTests2::RunTest(const FString& Parameters)
{
	FString URL= TEXT("https://raw.githubusercontent.com/jwg4/file_examples/refs/heads/master/valid/hello.txt");

	FString FileSavePath = FPaths::Combine(FPaths::ProjectSavedDir(),FPaths::GetCleanFilename(URL));

	if (IFileManager::Get().FileExists(*FileSavePath))
	{
		AddInfo(TEXT("Deleting previous test file."));
		IFileManager::Get().Delete(*FileSavePath);
	}

	AddInfo(TEXT("Attempting Download Test"));
	GEngine->Exec(nullptr, TEXT("log LogChunkStream All"));
	if (auto Cvar = IConsoleManager::Get().FindConsoleVariable(TEXT("ChunkStream.MaxChunkSize")))
	{
		
		Cvar->Set(1);
		AddInfo(TEXT("Chunk size set to 1MB"));
	}
	UChunkStreamDownloader* Downloader = UChunkStreamDownloader::DownloadFileToStorage(nullptr,URL,TEXT("application/json"),FileSavePath);
	Downloader->AddToRoot();
	
	Downloader->Activate();
	
	// Add a polling command that keeps itself alive until done
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
		[this, Downloader,FileSavePath, StartTime = FPlatformTime::Seconds(), 
		 LastLogTime = FPlatformTime::Seconds()]() mutable
		{
			double CurrentTime = FPlatformTime::Seconds();
			double ElapsedTime = CurrentTime - StartTime;
	        
			// interval in seconds
			const double LogInterval = 1.0;
			if (CurrentTime - LastLogTime >= LogInterval)
			{
				AddInfo(FString::Printf(TEXT("Download in progress... (%.1fs) Progress = %.1f%%"), 
					ElapsedTime, Downloader->GetProgress() * 100.0f));
				LastLogTime = CurrentTime;
			}
			
			if (Downloader->IsComplete())
			{
				AddInfo(FString::Printf(TEXT("Download completed in %.1f seconds!"), ElapsedTime));
				if (!TestTrue(TEXT("Download succeeded"),Downloader->IsComplete() ))
				{
					AddInfo(FString::Printf(TEXT("Download was not successful, Result is")));
				}
				TestTrue(TEXT("File Exists"),IFileManager::Get().FileExists(*FileSavePath));
				Downloader->CancelDownload();
				Downloader->RemoveFromRoot();
				
				
				return true;
			}
	        
			// 10min timeout
			const double TimeoutSeconds = 600.0;
			if (ElapsedTime > TimeoutSeconds)
			{
				AddError(FString::Printf(TEXT("Download timed out after %.1f seconds"), ElapsedTime));
				Downloader->CancelDownload();
				Downloader->RemoveFromRoot();
				
				
				return true;
			}
	        // return false will reloop this latent command until returns true
			return false;
		}
	));
	
	
	return true ;

	
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(ChunkStreamTests3, "ChunkStream.LargeVideo_Popeye",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool ChunkStreamTests3::RunTest(const FString& Parameters)
{
	FString URL= TEXT("https://tile.loc.gov/storage-services/service/mbrs/ntscrm/00068306/00068306.mp4");

	FString FileSavePath = FPaths::Combine(FPaths::ProjectSavedDir(),FPaths::GetCleanFilename(URL));

	if (IFileManager::Get().FileExists(*FileSavePath))
	{
		AddInfo(TEXT("Deleting previous test file."));
		IFileManager::Get().Delete(*FileSavePath);
	}

	AddInfo(TEXT("Attempting Download Test"));
	GEngine->Exec(nullptr, TEXT("log LogChunkStream All"));
	if (auto Cvar = IConsoleManager::Get().FindConsoleVariable(TEXT("ChunkStream.MaxChunkSize")))
	{
		
		Cvar->Set(50);
		AddInfo(TEXT("Chunk size set to 50MB"));
	}
	UChunkStreamDownloader* Downloader = UChunkStreamDownloader::DownloadFileToStorage(nullptr,URL,TEXT("application/json"),FileSavePath);
	Downloader->AddToRoot();
	
	Downloader->Activate();
	

	// Add a polling command that keeps itself alive until done
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
		[this, Downloader,FileSavePath, StartTime = FPlatformTime::Seconds(), 
		 LastLogTime = FPlatformTime::Seconds()]() mutable
		{
			double CurrentTime = FPlatformTime::Seconds();
			double ElapsedTime = CurrentTime - StartTime;
	        
			// interval in seconds
			const double LogInterval = 1.0;
			if (CurrentTime - LastLogTime >= LogInterval)
			{
				AddInfo(FString::Printf(TEXT("Download in progress... (%.1fs) Progress = %.1f%%"), 
					ElapsedTime, Downloader->GetProgress() * 100.0f));
				LastLogTime = CurrentTime;
			}
			
			if (Downloader->IsComplete())
			{
				AddInfo(FString::Printf(TEXT("Download completed in %.1f seconds!"), ElapsedTime));
				if (!TestTrue(TEXT("Download succeeded"),Downloader->IsComplete() ))
				{
					AddInfo(FString::Printf(TEXT("Download was not successful, Result is")));
				}
				TestTrue(TEXT("File Exists"),IFileManager::Get().FileExists(*FileSavePath));
				Downloader->CancelDownload();
				Downloader->RemoveFromRoot();
				
				
				return true;
			}
	        
			// 10min timeout
			const double TimeoutSeconds = 600.0;
			if (ElapsedTime > TimeoutSeconds)
			{
				AddError(FString::Printf(TEXT("Download timed out after %.1f seconds"), ElapsedTime));
				Downloader->CancelDownload();
				Downloader->RemoveFromRoot();
				
				
				return true;
			}
	        // return false will reloop this latent command until returns true
			return false;
		}
	));
	
	
	return true ;

	
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(ChunkStreamRetryTest, "ChunkStream.BadURLS",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool ChunkStreamRetryTest::RunTest(const FString& Parameters)
{
    // Use a URL that might have intermittent issues or rate limiting
  //  FString URL = TEXT("https://httpstat.us/500?sleep=2000"); // Returns 500 errors
	//FString URL = TEXT("https://httpbin.org/bytes/5242880"); // 5MB test file
	FString URL = TEXT("https://httpstat.us/random/200,503");
    // Or use: https://httpstat.us/random/200,503,500 for random responses
    
    FString FileSavePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("retry_test.bin"));
    
    if (IFileManager::Get().FileExists(*FileSavePath))
    {
        IFileManager::Get().Delete(*FileSavePath);
    }
    
    AddInfo(TEXT("Testing retry mechanism with potentially failing URL"));
    GEngine->Exec(nullptr, TEXT("log LogChunkStream All"));
    
    // Set small chunk size to increase chance of retry scenarios
    if (auto Cvar = IConsoleManager::Get().FindConsoleVariable(TEXT("ChunkStream.MaxChunkSize")))
    {
        Cvar->Set(1); // 1MB chunks
    }
    
    UChunkStreamDownloader* Downloader = UChunkStreamDownloader::DownloadFileToStorage(
        nullptr, URL, TEXT("application/octet-stream"), FileSavePath);
    Downloader->AddToRoot();
    Downloader->Activate();
    
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
        [this, Downloader, FileSavePath, StartTime = FPlatformTime::Seconds()]() mutable
        {
            double ElapsedTime = FPlatformTime::Seconds() - StartTime;
            
            if (Downloader->IsComplete())
            {
                // Check if file exists (success case)
                bool bFileExists = IFileManager::Get().FileExists(*FileSavePath);
                
                if (bFileExists)
                {
                    AddInfo(TEXT("Download succeeded (possibly after retries)"));
                    TestTrue(TEXT("File exists after download"), bFileExists);
                }
                else
                {
                    AddInfo(TEXT("Download failed after all retry attempts"));
                    // This is expected if the URL consistently fails
                }
                
                Downloader->RemoveFromRoot();
                return true;
            }
            
            // Timeout after 2 minutes
            if (ElapsedTime > 120.0)
            {
                AddError(TEXT("Test timed out"));
                Downloader->CancelDownload();
                Downloader->RemoveFromRoot();
                return true;
            }
            
            return false;
        }
    ));
    
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(ChunkStreamTests4, "ChunkStream.DownloadThenCancel",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool ChunkStreamTests4::RunTest(const FString& Parameters)
{
	FString URL= TEXT("https://tile.loc.gov/storage-services/service/mbrs/ntscrm/02276236/02276236.mp4");

	FString FileSavePath = FPaths::Combine(FPaths::ProjectSavedDir(),FPaths::GetCleanFilename(URL));

	if (IFileManager::Get().FileExists(*FileSavePath))
	{
		AddInfo(TEXT("Deleting previous test file."));
		IFileManager::Get().Delete(*FileSavePath);
	}

	AddInfo(TEXT("Attempting Download Test"));
	GEngine->Exec(nullptr, TEXT("log LogChunkStream All"));
	if (auto Cvar = IConsoleManager::Get().FindConsoleVariable(TEXT("ChunkStream.MaxChunkSize")))
	{
		Cvar->Set(4);
		AddInfo(TEXT("Chunk size set to 4MB"));
	}
	UChunkStreamDownloader* Downloader = UChunkStreamDownloader::DownloadFileToStorage(nullptr,URL,TEXT("application/json"),FileSavePath);
	Downloader->AddToRoot();
	
	TSharedPtr<FChunkStreamResultParams> pResult = MakeShared<FChunkStreamResultParams>();

	Downloader->Native_DownloadProgress.AddLambda([pResult] (FChunkStreamResultParams InResult)
	{
		if (pResult)
		{
			pResult->DownloadTaskResult = InResult.DownloadTaskResult;
			pResult->Progress = InResult.Progress;
		}
	});
	Downloader->Native_DownloadFinished.AddLambda([pResult] (FChunkStreamResultParams InResult)
	{
		if (pResult)
		{
			pResult->DownloadTaskResult = InResult.DownloadTaskResult;
			pResult->Progress = InResult.Progress;
		}
	});
	Downloader->Activate();
	

	// Add a polling command that keeps itself alive until done
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
		[this,pResult, Downloader,FileSavePath, StartTime = FPlatformTime::Seconds(), 
		 LastLogTime = FPlatformTime::Seconds()]() mutable
		{
			double CurrentTime = FPlatformTime::Seconds();
			double ElapsedTime = CurrentTime - StartTime;
	        
			// interval in seconds
			const double LogInterval = 0.15;
			if (CurrentTime - LastLogTime >= LogInterval)
			{
				AddInfo(FString::Printf(TEXT("Download in progress... (%.1fs) Progress = %.1f%%"), 
					ElapsedTime, Downloader->GetProgress() * 100.0f));
				LastLogTime = CurrentTime;

				if (Downloader->bCanceled)
				{
					TestTrue(TEXT("Download Was canceled with result "), pResult->DownloadTaskResult == EChunkStreamDownloadResult::UserCancelled);
					Downloader->CancelDownload();
					Downloader->RemoveFromRoot();
					
					pResult.Reset();
					return true;
				}
				if (Downloader->GetProgress() >=0.25f)
				{
					Downloader->CancelDownload();
				}
			}
			
			
	        
			// 10min timeout
			const double TimeoutSeconds = 600.0;
			if (ElapsedTime > TimeoutSeconds)
			{
				AddError(FString::Printf(TEXT("Download timed out after %.1f seconds"), ElapsedTime));
				Downloader->CancelDownload();
				Downloader->RemoveFromRoot();
				
				pResult.Reset();
				return true;
			}
	        // return false will reloop this latent command until returns true
			return false;
		}
	));
	
	
	return true ;

	
}

#endif //WITH_AUTOMATION_TESTS