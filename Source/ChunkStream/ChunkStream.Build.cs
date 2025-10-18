// Copyright (C) 2025 Isaac Cooper - All Rights Reserved

using UnrealBuildTool;

public class ChunkStream : ModuleRules
{
	public ChunkStream(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"HTTP", "Engine",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
	
		if (Target.Type == TargetType.Editor 
		    || (Target.Platform != UnrealTargetPlatform.Android
		    && Target.Platform != UnrealTargetPlatform.IOS
		    && Target.Platform != UnrealTargetPlatform.TVOS))
		{
			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"AutomationTest",
					"AutomationUtils"
				}
			);
		}
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"HTTP"
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
