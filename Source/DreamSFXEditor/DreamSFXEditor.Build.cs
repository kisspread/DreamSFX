using UnrealBuildTool;

public class DreamSFXEditor : ModuleRules
{
	public DreamSFXEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EditorSubsystem",
			"DreamSFXCore",
			"DreamSFXRuntime",
			"MetasoundEngine",
			"MetasoundEditor",
			"MetasoundFrontend"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
            "AssetRegistry", "DirectoryWatcher", "AudioExtensions", "MetasoundGraphCore",
            "Json", "JsonUtilities", "TraceServices", "TraceAnalysis", "Projects", "DeveloperSettings",
			"Slate", "SlateCore", "ToolMenus", "InputCore", "UMG", "RenderCore", "RHI",
			"UnrealEd", "AutomationDriver"
		});
	}
}
