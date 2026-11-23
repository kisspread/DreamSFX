#include "DreamSFXGraphCompiler.h"
#include "DreamSFXGraphArchive.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXNodeRegistry.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLongNoiseResourceTest, "DreamSFX.V2.LongNoiseResources",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FLongNoiseResourceTest::RunTest(const FString&)
    {
        const FString Text = TEXT(R"(sound test.longnoise
preview 60 s
seed 20261110 mulberry32
resources {
  loop=buffer.brown_loop(11s,random=mulberry32,step=.02,level=3.5,crossfadeFrames=4096)
  hall=impulse.diffuse(5s,random=mulberry32,dampingFloor=.04,dampingAmount=.5,dampingDecayPerSecond=2,attackSeconds=.03333333333333333,decaySeconds=.6666666666666666,taperEnd=1)
}
bus bed { out=noise(buffer=loop) }
output bed.out
)");
        auto Result = FGraphCompiler::Compile(Text);
        if (!TestTrue(TEXT("Long scene and named resource recipes compile"), Result.Graph.IsValid())) return false;
        TArray<uint8> Packed;
        if (!EncodeGraph(*Result.Graph, Packed)) return false;
        TArray<uint8> LegacyPayload = {'D', 'F', 'X', '2', 1, 0, 0, 0};
        TestFalse(TEXT("Retired binary graph payload is refused"), DecodeGraph(LegacyPayload).IsValid());
        const auto Graph = DecodeGraph(Packed);
        if (!TestTrue(TEXT("Archived resources retain their named types"), Graph && Graph->Resources.Num() == 2)) return false;
        FString Dir;
        FParse::Value(FCommandLine::Get(), TEXT("DreamSFXResourceReference="), Dir);
        for (int32 I = 0; I < 2; ++I)
        {
            const auto& Recipe = Graph->Resources[I];
            const auto Buffer = PrepareBuffer(Recipe, *FindBufferType(Recipe.Type));
            const int32 Frames = Recipe.FramesAt48k;
            TestEqual(TEXT("Both channels have authored period"), Buffer.Channel[0].Num(), Frames);
            const double Left = I ? 0.00029451510636135936 : 0.11885559558868408;
            const double Right = I ? 0.0001309371873503551 : 0.32497847080230713;
            TestTrue(TEXT("Fixed browser samples after the stereo RNG split"),
                Buffer.Channel[0][1] == float(Left) && Buffer.Channel[1][1] == float(Right));
            if (!Dir.IsEmpty())
            {
                TArray<uint8> Data;
                const FString Path = Dir / (I ? TEXT("impulse.f32") : TEXT("noise.f32"));
                if (!TestTrue(TEXT("Full browser float reference available"), FFileHelper::LoadFileToArray(Data, *Path) && Data.Num() == Frames * 8)) return false;
                const float* Raw = reinterpret_cast<const float*>(Data.GetData());
                int32 Different = 0;
                double Worst = 0.;
                for (int32 F = 0; F < Frames; ++F)
                    for (int32 Ch = 0; Ch < 2; ++Ch)
                    {
                        const float A = Buffer.Channel[Ch][F], B = Raw[2 * F + Ch];
                        Different += FMemory::Memcmp(&A, &B, sizeof(float)) != 0;
                        Worst = FMath::Max(Worst, FMath::Abs(double(A) - B));
                    }
                AddInfo(FString::Printf(TEXT("DSFX_RESOURCE name=%s frames=%d differingSamples=%d maxAbs=%.12g"),
                    *Recipe.Id.ToString(), Frames, Different, Worst));
                TestTrue(TEXT("Every browser resource sample numerical error below 1e-7"), Worst < 1.e-7);
            }
        }
        const auto Invalid=FGraphCompiler::Compile(Text.Replace(TEXT("crossfadeFrames=4096"),TEXT("crossfadeFrames=999999")));
        const FString Needle=TEXT("crossfadeFrames="); int32 Key=Text.Find(Needle);
        const int32 LineStart=Text.Find(TEXT("\n"),ESearchCase::CaseSensitive,ESearchDir::FromEnd,Key);
        const FString Expected=FString::Printf(TEXT("5:%d"),Key-LineStart);
        TestTrue(TEXT("Oversized crossfade identifies its DSL field and exact position"), !Invalid.Graph && Invalid.Errors.Num() &&
            Invalid.Errors[0].Pointer==Expected && Invalid.Errors[0].Message.Contains(TEXT("crossfadeFrames")));
        return !HasAnyErrors();
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImpulseScaleTest, "DreamSFX.V2.ImpulseScale",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FImpulseScaleTest::RunTest(const FString&)
    {
        const FString Text = TEXT(R"(sound test.hall
preview .012 s
resources {
  pulse=buffer.white(.000020833333333333333s,seed=123)
  ir=impulse.diffuse(.005333333333333333s,seed=93,random=lcg32,dampingFloor=.03,dampingAmount=.4,dampingDecayPerSecond=1.2,attackSeconds=.025,decaySeconds=.9523809523809523,taperEnd=1)
}
bus field {
  source.mono_noise_buffer(buffer=pulse,offsetSeconds=0,life=.000020833333333333333) -> hall=room.convolution@2(ir=ir,normalize=0)
}
output field.hall
)");
        const auto Built = FGraphCompiler::Compile(Text);
        if (!TestTrue(TEXT("Unnormalised convolution compiles"), Built.Graph.IsValid())) return false;
        const auto& IR = Built.Graph->Resources[1];
        const auto Impulse = PrepareBuffer(IR, *FindBufferType(IR.Type));
        const float Pulse = float(double(123u * 1664525u + 1013904223u) / 2147483648. - 1.);
        FGraphInstance Voice;
        FString Error;
        if (!TestTrue(TEXT("Impulse probe prepares"), Voice.Prepare(Built.Graph, 48000, Error))) return false;
        float Audio[1152] = {};
        Voice.Render(Audio, 576);
        double Worst = 0.;
        for (int32 F = 0; F < 576; ++F)
            for (int32 Ch = 0; Ch < 2; ++Ch)
            {
                const float Expected = F < 256 ? Impulse.Channel[Ch][F] * Pulse : 0.f;
                Worst = FMath::Max(Worst, double(FMath::Abs(Expected - Audio[2*F+Ch])));
            }
        AddInfo(FString::Printf(TEXT("DSFX_RAW_IR maxAbs=%.12g"), Worst));
        return TestTrue(TEXT("Convolution preserves the procedural IR amplitude and silent tail"), Worst < 1.e-7);
    }
}
#endif
