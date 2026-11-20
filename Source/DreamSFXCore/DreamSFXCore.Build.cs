using UnrealBuildTool;

public class DreamSFXCore : ModuleRules
{
	public DreamSFXCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Graph playback must keep the same floating point order in Editor and Game targets.
		FPSemantics = FPSemanticsMode.Precise;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject"
		});

		// JSON parsing for the text compiler; also needs JsonUtilities for the reader.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
            "SignalProcessing",
			"EightBitMetaSound"
		});

	}
}
