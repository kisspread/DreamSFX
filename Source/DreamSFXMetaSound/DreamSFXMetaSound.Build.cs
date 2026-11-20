using UnrealBuildTool;

public class DreamSFXMetaSound : ModuleRules
{
	public DreamSFXMetaSound(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DreamSFXCore",
			"DreamSFXRuntime",
			"MetasoundFrontend",
			"MetasoundGraphCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AudioExtensions"
		});

		PrivateDefinitions.AddRange(new string[]
		{
			"METASOUND_PLUGIN=DreamSFX",
			"METASOUND_MODULE=DreamSFXMetaSound"
		});
	}
}