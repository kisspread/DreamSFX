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
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThreePoleLoopTest, "DreamSFX.V2.ThreePoleLoop",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FThreePoleLoopTest::RunTest(const FString&)
    {
        const FString Text = TEXT(R"({
            "schema":"dreamsfx/2","id":"test.poles","durationSeconds":9,"resources":[
                {"id":"wind","type":"buffer.three_pole_loop","version":1,"seed":690030787,"framesAt48k":432000,
                 "parameters":{"random":"mulberry32","crossfadeFrames":4096,"slowPole":0.997,"slowInput":0.029,
                 "midPole":0.985,"midInput":0.032,"fastPole":0.95,"fastInput":0.048,"direct":0.02,"level":1.6}},
                {"id":"space","type":"impulse.diffuse","version":1,"seed":3308561347,"framesAt48k":192000,
                 "parameters":{"random":"mulberry32","dampingFloor":0.05,"dampingAmount":0.6,
                 "dampingDecayPerSecond":2.5,"attackSeconds":0.025,"decaySeconds":0.5882352941176471,"taperEnd":false}}],
            "nodes":[{"id":"bed","type":"source.noise_buffer","version":1,"parameters":{"buffer":"wind","offsetSeconds":0,"playbackRate":1}}],
            "timeline":[{"action":"trigger","atSeconds":0,"targets":[{"node":"bed","port":"start"}]},
            {"action":"trigger","atSeconds":9,"targets":[{"node":"bed","port":"stop"}]}],
            "connections":[],"output":{"node":"bed","port":"audio"}})");
        const auto Built = FGraphCompiler::Compile(Text);
        if (!TestTrue(TEXT("Three parallel poles compile"), Built.Graph.IsValid())) return false;
        const TCHAR* Expected[] = {TEXT("108b920a285a8b5d2a809face9e1a77f86921c19"), TEXT("e681e9abf2ddb6462d05074add3fec4094bfe2f8")};
        for (int32 I = 0; I < Built.Graph->Resources.Num(); ++I)
        {
            const auto& Resource = Built.Graph->Resources[I];
            const auto Buffer = PrepareBuffer(Resource, *FindBufferType(Resource.Type));
            TArray<float> Interleaved;
            Interleaved.SetNumUninitialized(2 * Resource.FramesAt48k);
            for (int32 Frame = 0; Frame < Resource.FramesAt48k; ++Frame)
                for (int32 Channel = 0; Channel < 2; ++Channel)
                    Interleaved[2 * Frame + Channel] = Buffer.Channel[Channel][Frame];
            FSHAHash Hash;
            FSHA1::HashBuffer(Interleaved.GetData(), Interleaved.Num() * sizeof(float), Hash.Hash);
            AddInfo(FString::Printf(TEXT("DSFX_WORLD_RESOURCE id=%s frames=%d sha1=%s"),
                *Resource.Id.ToString(), Resource.FramesAt48k, *Hash.ToString()));
            TestTrue(TEXT("Stereo bytes match the independent world buffer"), Hash.ToString().Equals(Expected[I], ESearchCase::IgnoreCase));
        }
        const auto Short = FGraphCompiler::Compile(Text.Replace(TEXT("\"framesAt48k\":432000"), TEXT("\"framesAt48k\":4095")));
        TestTrue(TEXT("Crossfade exceeding period is diagnosed"), !Short.Graph && Short.Errors.Num() && Short.Errors[0].Pointer.EndsWith(TEXT("crossfadeFrames")));
        const auto Unstable = FGraphCompiler::Compile(Text.Replace(TEXT("\"slowPole\":0.997"), TEXT("\"slowPole\":1.01")));
        TestTrue(TEXT("Unstable pole is diagnosed"), !Unstable.Graph && Unstable.Errors.Num() && Unstable.Errors[0].Pointer.EndsWith(TEXT("slowPole")));
        return !HasAnyErrors();
    }

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

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlResponseTest, "DreamSFX.V2.ControlResponse",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FControlResponseTest::RunTest(const FString&)
    {
        const FString Text = TEXT(R"({
            "schema":"dreamsfx/2","id":"test.response","durationSeconds":0.03,
            "resources":[{"id":"fixed","type":"buffer.white","version":1,"seed":123,"framesAt48k":1}],
            "nodes":[{"id":"sample","type":"source.mono_noise_buffer","version":1,"parameters":{"buffer":"fixed","offsetSeconds":0,"playbackRate":1}},
              {"id":"depth","type":"control.value","version":1,"parameters":{"value":0}},
              {"id":"map","type":"control.response","version":1,"parameters":{"value":0,"enabled":1,"inputMin":0,"inputMax":10,"outputMin":0,"outputMax":1,"exponent":2,"timeConstantSeconds":0.005,"initial":0}},
              {"id":"amp","type":"mix.gain","version":1,"parameters":{"level":0}}],
            "connections":[{"from":{"node":"sample","port":"audio"},"to":{"node":"amp","port":"input"},"order":0},
              {"from":{"node":"depth","port":"control"},"to":{"node":"map","port":"value"},"order":0},
              {"from":{"node":"map","port":"control"},"to":{"node":"amp","port":"level"},"order":0}],
            "timeline":[{"atSeconds":0,"action":"trigger","targets":[{"node":"sample","port":"start"}]},
              {"atSeconds":0.03,"action":"trigger","targets":[{"node":"sample","port":"stop"}]},
              {"atSeconds":0.002,"action":"set","value":5,"targets":[{"node":"depth","port":"value"}]},
              {"atSeconds":0.014,"action":"set","value":20,"targets":[{"node":"depth","port":"value"}]}],
            "output":{"node":"amp","port":"audio"}})");
        auto G = FGraphCompiler::Compile(Text);
        FGraphInstance Voice;
        FString Error;
        if (!TestTrue(TEXT("Named controls prepare"), G.Graph && Voice.Prepare(G.Graph, 48000, Error))) return false;
        TArray<float> Audio;
        Audio.SetNumUninitialized(2880);
        Voice.Render(Audio.GetData(), 1440);
        const double Input = float(double(123u * 1664525u + 1013904223u) / 2147483648. - 1.);
        const double At672 = .25 * (1. - std::exp(-(672. - 96.) / 240.));
        double Worst = 0.;
        for (int32 I = 0; I < 1440; ++I)
        {
            const double Amp = I <= 96 ? 0. : I <= 672 ? .25 * (1. - std::exp(-(I - 96.) / 240.)) :
                1. + (At672 - 1.) * std::exp(-(I - 672.) / 240.);
            Worst = FMath::Max(Worst, FMath::Abs(Input * Amp - Audio[2 * I]));
        }
        TestTrue(TEXT("Clamped squared target and smoothing match the continuous exponential"), Worst < 1.e-7);
        const auto Bad = FGraphCompiler::Compile(Text.Replace(TEXT("\"inputMax\":10"), TEXT("\"inputMax\":0")));
        TestTrue(TEXT("Degenerate control range identifies its field"), !Bad.Graph && Bad.Errors.Num() && Bad.Errors[0].Pointer.EndsWith(TEXT("inputMax")));
        AddInfo(FString::Printf(TEXT("DSFX_RESPONSE maxAbs=%.12g"), Worst));
        return !HasAnyErrors();
    }
}
#endif
