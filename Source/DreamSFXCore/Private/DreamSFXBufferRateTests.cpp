#include "DreamSFXGraphCompiler.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXNodeRegistry.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBufferRateSampling, "DreamSFX.DSL.BufferRateSampling",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    bool FBufferRateSampling::RunTest(const FString&)
    {
        const FString Source = TEXT(R"(sound test.buffer_rate
preview .05 s
seed 93
resources { field=buffer.white(.1s) }
bus bed { sample=noise@2(buffer=field,rateHoldFrames=128,playbackRate=line(0,.5,.008,2)) }
output bed.sample
)");
        TestFalse(TEXT("Unsupported control period is rejected"),
            FGraphCompiler::Compile(Source.Replace(TEXT("Frames=128"), TEXT("Frames=65"))).Graph.IsValid());
        for (const int32 Period : {1, 128})
        {
            const auto Result = FGraphCompiler::Compile(Source.Replace(TEXT("Frames=128"), *FString::Printf(TEXT("Frames=%d"), Period)));
            if (!TestTrue(TEXT("A supported read-rate period compiles"), Result.Graph.IsValid())) return false;
            const FGeneratedBuffer& Resource = Result.Graph->Resources[0];
            const auto Reference = PrepareBuffer(Resource, *FindBufferType(Resource.Type));
            FGraphInstance Player;
            FString Error;
            if (!TestTrue(TEXT("Buffer reader prepares"), Player.Prepare(Result.Graph, 48000, Error))) return false;
            constexpr int32 Frames = 500;
            float Audio[Frames * 2];
            Player.Render(Audio, 97);
            Player.Render(Audio + 194, Frames - 97);
            double Cursor = 0.;
            float Worst = 0.f;
            for (int32 Frame = 0; Frame < Frames; ++Frame)
            {
                const int32 Base = int32(Cursor);
                for (int32 Channel = 0; Channel < 2; ++Channel)
                {
                    const auto& Samples = Reference.Channel[Channel];
                    const float Expected = FMath::Lerp(Samples[Base], Samples[Base + 1], float(Cursor - Base));
                    Worst = FMath::Max(Worst, FMath::Abs(Expected - Audio[Frame * 2 + Channel]));
                }
                const int32 ControlFrame = Frame - Frame % Period;
                Cursor += .5 + FMath::Min(ControlFrame, 384) / 256.;
            }
            TestEqual(*FString::Printf(TEXT("Exact stereo sample positions at period %d across callback split"), Period), Worst, 0.f);
        }
        return !HasAnyErrors();
    }
}
#endif
