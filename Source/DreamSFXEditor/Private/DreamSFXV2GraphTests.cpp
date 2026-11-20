#include "DreamSFXGraphCompiler.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXGraphPreview.h"
#include "HAL/IConsoleManager.h"
#include "MetasoundSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphSchemaTest, "DreamSFX.V2.GraphSchema",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FGraphSchemaTest::RunTest(const FString&)
    {
        const FString Text = TEXT(R"({
            "schema":"dreamsfx/2","id":"test.graph","durationSeconds":0.02,
            "resources":[],
            "nodes":[{"id":"master","type":"mix.gain","version":1,"parameters":{"level":0.5}},
                     {"id":"voice","type":"osc.sine","version":1,"parameters":{"frequencyHz":440,"detuneCents":0}}],
            "connections":[{"from":{"node":"voice","port":"audio"},"to":{"node":"master","port":"input"},"order":0}],
            "timeline":[{"atSeconds":0.005,"action":"trigger","targets":[{"node":"voice","port":"start"}]},
                        {"atSeconds":0.01,"action":"trigger","targets":[{"node":"voice","port":"stop"}]}],
            "output":{"node":"master","port":"audio"}
        })");
        const auto Result = FGraphCompiler::Compile(Text);
        if (!TestTrue(TEXT("Named graph compiles"), Result.Graph.IsValid())) return false;
        TestEqual(TEXT("Producer can be declared after consumer"), Result.Graph->ExecutionOrder[0], FName(TEXT("voice")));
        TestEqual(TEXT("Output names master, which was declared first"), Result.Graph->Output.Node, FName(TEXT("master")));

        FString Error;
        FGraphInstance Instance;
        if (!TestTrue(*Error, Instance.Prepare(Result.Graph, 48000, Error))) return false;
        TArray<float> Audio;
        Audio.SetNumUninitialized(1920);
        Instance.Render(Audio.GetData(), 960);
        bool bSilence = true;
        for (int32 Frame = 0; Frame < 240; ++Frame) bSilence &= Audio[2 * Frame] == 0 && Audio[2 * Frame + 1] == 0;
        for (int32 Frame = 480; Frame < 960; ++Frame) bSilence &= Audio[2 * Frame] == 0 && Audio[2 * Frame + 1] == 0;
        TestTrue(TEXT("Start and stop event inputs gate exact sample frames"), bSilence && Audio[482] > 0 && Instance.IsFinished());

        auto Reject = [this](const FString& Changed, const TCHAR* Code, const TCHAR* Pointer)
        {
            const auto Failure = FGraphCompiler::Compile(Changed);
            if (TestTrue(TEXT("Invalid graph rejected"), !Failure.Graph && Failure.Errors.Num() == 1))
            {
                TestEqual(TEXT("Diagnostic code"), Failure.Errors[0].Code, FString(Code));
                TestEqual(TEXT("Diagnostic field"), Failure.Errors[0].Pointer, FString(Pointer));
            }
        };
        Reject(Text.Replace(TEXT("dreamsfx/2"), TEXT("dreamsfx/1")), TEXT("DSFXV200"), TEXT("/schema"));
        Reject(Text.Replace(TEXT("\"type\":\"osc.sine\",\"version\":1"), TEXT("\"type\":\"osc.sine\",\"version\":99")),
            TEXT("DSFXV201"), TEXT("/nodes/1"));
        Reject(Text.Replace(TEXT("frequencyHz"), TEXT("freq")), TEXT("DSFXV202"), TEXT("/nodes/1/parameters/frequencyHz"));
        Reject(Text.Replace(TEXT("\"port\":\"input\""), TEXT("\"port\":\"level\"")), TEXT("DSFXV203"), TEXT("/connections/0"));
        Reject(Text.Replace(TEXT("\"port\":\"start\""), TEXT("\"port\":\"frequencyHz\"")), TEXT("DSFXV204"), TEXT("/timeline/0/targets/0"));
        Reject(Text.Replace(TEXT("\"order\":0}]"), TEXT("\"order\":0},{\"from\":{\"node\":\"master\",\"port\":\"control\"},\"to\":{\"node\":\"voice\",\"port\":\"frequencyHz\"},\"order\":0}]")),
            TEXT("DSFXV205"), TEXT("/connections"));
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphMetaSoundTest, "DreamSFX.V2.BlastFullMetaSound",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FGraphMetaSoundTest::RunTest(const FString&)
    {
        FString Path, Output;
        FParse::Value(FCommandLine::Get(), TEXT("DreamSFXV2Input="), Path);
        FParse::Value(FCommandLine::Get(), TEXT("DreamSFXV2Output="), Output);
        if (Path.IsEmpty() && Output.IsEmpty()) return true; // Explicit capture, outside the ordinary preset library.
        if (!TestTrue(TEXT("Use a new capture file"), !Path.IsEmpty() && !Output.IsEmpty() && !FPaths::FileExists(Output))) return false;

        FString Text;
        if (!TestTrue(TEXT("Read the V2 text fixture"), FFileHelper::LoadFileToString(Text, *Path))) return false;
        const auto Result = FGraphCompiler::Compile(Text);
        for (const auto& Error : Result.Errors) AddError(Error.Code + TEXT(" ") + Error.Pointer + TEXT(" ") + Error.Message);
        if (!TestTrue(TEXT("V2 text compiles"), Result.Graph.IsValid())) return false;
        if (!TestEqual(TEXT("Full phase-one node count"), Result.Graph->Nodes.Num(), 494)) return false;

        constexpr int32 Rate = 48000, Block = 256;
        FString Error;
        FGraphInstance Core;
        if (!Core.Prepare(Result.Graph, Rate, Error)) { AddError(Error); return false; }
        const int32 Frames = int32(Core.GetLengthFrames());
        if (!TestEqual(TEXT("Authored graph duration in frames"), Frames, 280320)) return false;
        auto Render = [&Core, Frames](int32 Chunk)
        {
            Core.Restart();
            TArray<float> Output;
            Output.SetNumUninitialized(Frames * 2);
            for (int32 At = 0; At < Frames; At += Chunk)
                Core.Render(Output.GetData() + 2 * At, FMath::Min(Chunk, Frames - At));
            return Output;
        };
        const auto Direct = Render(Block);
        for (int32 Chunk : {64, 1024})
        {
            const auto Other = Render(Chunk);
            TestTrue(FString::Printf(TEXT("Seed and PCM unchanged with %d-frame caller blocks"), Chunk),
                Core.IsFinished() && FMemory::Memcmp(Direct.GetData(), Other.GetData(), Direct.Num() * sizeof(float)) == 0);
        }

        auto* Source = BuildGraphPreview(Result.Graph, Error);
        if (!Source) { AddError(Error); return false; }
        auto* Async = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if (!TestNotNull(TEXT("MetaSound generator setting"), Async)) return false;
        const int32 Previous = Async->GetInt();
        Async->Set(0, ECVF_SetByCode);
        ON_SCOPE_EXIT { Async->Set(Previous, ECVF_SetByCode); };

        Source->InitResources();
        FSoundGeneratorInitParams Params;
        Params.AudioDeviceID = 0;
        Params.AudioComponentId = 986402;
        Params.InstanceID = 986402;
        Params.SampleRate = Rate;
        Params.NumChannels = 2;
        Params.NumFramesPerCallback = Block;
        Params.AudioMixerNumOutputFrames = Block;
        Params.GraphName = TEXT("DreamSFXV2BlastFull");
        auto Generator = Source->CreateSoundGenerator(Params, TArray<FAudioParameter>());
        if (!TestTrue(TEXT("Actual MetaSound generator created"), Generator.IsValid())) return false;
        Generator->OnBeginGenerate();
        ON_SCOPE_EXIT { Generator->OnEndGenerate(); Source->OnEndGenerate(Generator); };
        TArray<float> Recorded;
        for (int32 At = 0; At < Frames + Block * 4 && !Generator->IsFinished(); At += Block)
        {
            float Buffer[Block * 2] = {};
            Generator->GetNextBuffer(Buffer, UE_ARRAY_COUNT(Buffer), true);
            Recorded.Append(Buffer, UE_ARRAY_COUNT(Buffer));
        }
        if (!TestTrue(TEXT("MetaSound finished naturally after generating the full graph"), Generator->IsFinished() && Recorded.Num() >= Direct.Num())) return false;
        TestTrue(TEXT("MetaSound has the same sample clock and PCM as the graph instance"),
            FMemory::Memcmp(Direct.GetData(), Recorded.GetData(), Direct.Num() * sizeof(float)) == 0);
        bool bFinite = true, bZeroPadding = true;
        float Peak = 0;
        for (int32 Index = 0; Index < Recorded.Num(); ++Index)
        {
            const float Sample = Recorded[Index];
            bFinite &= FMath::IsFinite(Sample);
            Peak = FMath::Max(Peak, FMath::Abs(Sample));
            if (Index >= Frames * 2) bZeroPadding &= Sample == 0.f;
        }
        if (!TestTrue(TEXT("Stereo float PCM is finite and non-clipping, with only silent transport padding"), bFinite && bZeroPadding && Peak > .00001f && Peak < .98f)) return false;
        AddInfo(FString::Printf(TEXT("DSFX_V2 nodes=%d resources=%d connections=%d events=%d naturalFrames=%d capturedFrames=%d peak=%.9f"),
            Result.Graph->Nodes.Num(), Result.Graph->Resources.Num(), Result.Graph->Connections.Num(), Result.Graph->Timeline.Num(), Frames, Recorded.Num() / 2, Peak));
        return TestTrue(TEXT("Export unscaled MetaSound interleaved float PCM"), FFileHelper::SaveArrayToFile(
            MakeArrayView(reinterpret_cast<const uint8*>(Recorded.GetData()), Recorded.Num() * sizeof(float)), *Output));
    }
}
#endif
