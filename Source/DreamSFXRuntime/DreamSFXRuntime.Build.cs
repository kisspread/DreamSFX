using UnrealBuildTool;

public class DreamSFXRuntime : ModuleRules
{
	public DreamSFXRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MetasoundEngine",
			"DreamSFXCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AudioExtensions"
		});
	}
}