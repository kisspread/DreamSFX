#include "DreamSFXGraphCompiler.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXGraphPreview.h"
#include "HAL/FileManager.h"
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
    namespace
    {
        constexpr int32 SampleRate = 48000;
        constexpr int32 CallbackFrames = 256;

        FString MakeGraph(const TCHAR* Id, double Seconds, const TCHAR* Resources, const TCHAR* Nodes,
            const TCHAR* Edges, const TCHAR* Timeline, const TCHAR* Output)
        {
            return FString::Printf(TEXT(R"({
                "schema":"dreamsfx/2","id":"%s","durationSeconds":%.9f,
                "resources":[%s],"nodes":[%s],"connections":[%s],"timeline":[%s],
                "output":{"node":"%s","port":"audio"}
            })"), Id, Seconds, Resources, Nodes, Edges, Timeline, Output);
        }

        TArray<float> CaptureGraph(FAutomationTestBase& Test, FGraphPtr Graph, const TCHAR* Name, int32 Frames)
        {
            FString Error;
            FGraphInstance Core;
            if (!Core.Prepare(Graph, SampleRate, Error)) { Test.AddError(Error); return {}; }
            TArray<float> Direct;
            Direct.SetNumZeroed(Frames * 2);
            int32 Written = 0;
            for (int32 At = 0; At < Frames; At += 113)
                Written += Core.Render(Direct.GetData() + 2 * At, FMath::Min(113, Frames - At));
            if (!Test.TestTrue(TEXT("Core completes at the expected frame"), Core.IsFinished() && Written == Frames)) return {};

            UMetaSoundSource* Source = BuildGraphPreview(Graph, Error);
            if (!Source) { Test.AddError(Error); return {}; }
            auto* Async = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
            if (!Test.TestNotNull(TEXT("MetaSound asynchronous builder setting"), Async)) return {};
            const int32 Previous = Async->GetInt();
            Async->Set(0, ECVF_SetByCode);
            ON_SCOPE_EXIT { Async->Set(Previous, ECVF_SetByCode); };
            Source->InitResources();
            FSoundGeneratorInitParams Params;
            Params.AudioDeviceID = 0;
            Params.AudioComponentId = 986403;
            Params.InstanceID = 986403;
            Params.SampleRate = SampleRate;
            Params.NumChannels = 2;
            Params.NumFramesPerCallback = CallbackFrames;
            Params.AudioMixerNumOutputFrames = CallbackFrames;
            Params.GraphName = Name;
            auto Generator = Source->CreateSoundGenerator(Params, TArray<FAudioParameter>());
            if (!Test.TestTrue(TEXT("MetaSound generator created"), Generator.IsValid())) return {};
            Generator->OnBeginGenerate();
            ON_SCOPE_EXIT { Generator->OnEndGenerate(); Source->OnEndGenerate(Generator); };
            TArray<float> Audio;
            for (int32 At = 0; At < Frames + CallbackFrames * 4 && !Generator->IsFinished(); At += CallbackFrames)
            {
                float Buffer[CallbackFrames * 2] = {};
                Generator->GetNextBuffer(Buffer, UE_ARRAY_COUNT(Buffer), true);
                Audio.Append(Buffer, UE_ARRAY_COUNT(Buffer));
            }
            if (!Test.TestTrue(TEXT("MetaSound naturally finishes"), Generator->IsFinished() && Audio.Num() >= Direct.Num())) return {};
            Test.TestTrue(TEXT("Core and MetaSound PCM are bit identical"),
                FMemory::Memcmp(Direct.GetData(), Audio.GetData(), Direct.Num() * sizeof(float)) == 0);
            bool bValid = true;
            double Energy = 0.;
            float Peak = 0.f;
            for (int32 Index = 0; Index < Audio.Num(); ++Index)
            {
                const float Sample = Audio[Index];
                bValid &= FMath::IsFinite(Sample) && (Index < Frames * 2 || Sample == 0.f);
                Energy += double(Sample) * Sample;
                Peak = FMath::Max(Peak, FMath::Abs(Sample));
            }
            Test.TestTrue(TEXT("Original comparison sound is finite, non-clipping and has zero transport padding"),
                bValid && Peak > 0.f && Peak < 1.f);
            Audio.SetNum(Frames * 2, EAllowShrinking::No);
            Test.AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH case=%s naturalFrames=%d peak=%.9f rmsDb=%.9f"),
                Name, Frames, Peak, 10. * FMath::LogX(10., Energy / Audio.Num())));
            FString Output;
            if (FParse::Value(FCommandLine::Get(), TEXT("DreamSFXSignalPathOutput="), Output))
            {
                IFileManager::Get().MakeDirectory(*Output, true);
                const FString Path = FPaths::Combine(Output, FString(Name) + TEXT(".f32"));
                Test.TestTrue(TEXT("Export original comparison sound"), FFileHelper::SaveArrayToFile(
                    MakeArrayView(reinterpret_cast<const uint8*>(Audio.GetData()), Audio.Num() * sizeof(float)), *Path));
            }
            return Audio;
        }

        TArray<float> Capture(FAutomationTestBase& Test, const FString& Text, const TCHAR* Name, int32 Frames)
        {
            const auto Result = FGraphCompiler::Compile(Text);
            for (const auto& Diagnostic : Result.Errors)
                Test.AddError(Diagnostic.Code + TEXT(" ") + Diagnostic.Pointer + TEXT(" ") + Diagnostic.Message);
            if (!Test.TestTrue(TEXT("Text graph compiles"), Result.Graph.IsValid())) return {};
            return CaptureGraph(Test, Result.Graph, Name, Frames);
        }
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBrownBufferTest, "DreamSFX.V2.SignalPath.Brown",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FBrownBufferTest::RunTest(const FString&)
    {
        const FString Text = MakeGraph(TEXT("test.deep_current"), 1.2,
            TEXT(R"({"id":"bed","type":"buffer.brown","version":1,"seed":9051,"framesAt48k":48000})"),
            TEXT(R"({"id":"current","type":"source.noise_buffer","version":1,"parameters":{"buffer":"bed","offsetSeconds":0,"playbackRate":1}},
                {"id":"level","type":"mix.gain","version":1,"parameters":{"level":0.45}})"),
            TEXT(R"({"from":{"node":"current","port":"audio"},"to":{"node":"level","port":"input"},"order":0})"),
            TEXT(R"({"atSeconds":0,"action":"trigger","targets":[{"node":"current","port":"start"}]},
                {"atSeconds":1.2,"action":"trigger","targets":[{"node":"current","port":"stop"}]},
                {"atSeconds":1,"action":"set","targets":[{"node":"level","port":"level"}],"value":0.45},
                {"atSeconds":1.2,"action":"linear","targets":[{"node":"level","port":"level"}],"value":0})"), TEXT("level"));
        const auto Brown = Capture(*this, Text, TEXT("deep-current-brown"), 57600);
        const auto White = Capture(*this, Text.Replace(TEXT("buffer.brown"), TEXT("buffer.white")), TEXT("deep-current-white"), 57600);
        if (Brown.IsEmpty() || White.IsEmpty()) return false;
        double MaxDelta = 0;
        double BrownMovement = 0, WhiteMovement = 0;
        for (int32 Channel = 0; Channel < 2; ++Channel)
        {
            double State = 0;
            for (int32 Frame = 0; Frame < 48000; ++Frame)
            {
                const int32 Index = 2 * Frame + Channel;
                State = (State + .02 * White[Index] / .45f) / 1.02;
                MaxDelta = FMath::Max(MaxDelta, FMath::Abs(State * 3.2 * .45f - Brown[Index]));
                if (Frame > 0)
                {
                    BrownMovement += FMath::Square(double(Brown[Index] - Brown[Index - 2]));
                    WhiteMovement += FMath::Square(double(White[Index] - White[Index - 2]));
                }
            }
        }
        TestTrue(TEXT("Stereo Brown follows the recurrence applied to the matching White seed"), MaxDelta < 1.e-7);
        TestTrue(TEXT("A repeated seed produces a continuous, darker bed than white noise"), BrownMovement < .01 * WhiteMovement);
        TestTrue(TEXT("Noise buffer loops at its declared length"), Brown[2 * 48000] == Brown[0] && Brown[2 * 48000 + 1] == Brown[1]);
        AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH brownRecurrenceMax=%.12g brownToWhiteDifferenceEnergy=%.9f"),
            MaxDelta, BrownMovement / WhiteMovement));
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLowShelfTest, "DreamSFX.V2.SignalPath.LowShelf",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FLowShelfTest::RunTest(const FString&)
    {
        // An original three-tone pulse. Its sustained section makes all three gains measurable independently.
        const FString Text = MakeGraph(TEXT("test.triad_pulse"), .6, TEXT(""),
            TEXT(R"({"id":"low","type":"osc.sine","version":1,"parameters":{"frequencyHz":24,"detuneCents":0}},
                {"id":"mid","type":"osc.sine","version":1,"parameters":{"frequencyHz":240,"detuneCents":0}},
                {"id":"high","type":"osc.sine","version":1,"parameters":{"frequencyHz":4800,"detuneCents":0}},
                {"id":"level","type":"mix.gain","version":1,"parameters":{"level":0.12}},
                {"id":"shelf","type":"filter.lowshelf","version":1,"parameters":{"cutoffHz":240,"gainDb":10}})"),
            TEXT(R"({"from":{"node":"low","port":"audio"},"to":{"node":"level","port":"input"},"order":0},
                {"from":{"node":"mid","port":"audio"},"to":{"node":"level","port":"input"},"order":1},
                {"from":{"node":"high","port":"audio"},"to":{"node":"level","port":"input"},"order":2},
                {"from":{"node":"level","port":"audio"},"to":{"node":"shelf","port":"input"},"order":0})"),
            TEXT(R"({"atSeconds":0,"action":"trigger","targets":[{"node":"low","port":"start"},{"node":"mid","port":"start"},{"node":"high","port":"start"}]},
                {"atSeconds":0.5,"action":"trigger","targets":[{"node":"low","port":"stop"},{"node":"mid","port":"stop"},{"node":"high","port":"stop"}]})"), TEXT("shelf"));
        const auto Boost = Capture(*this, Text, TEXT("triad-pulse-shelf"), 28800);
        const auto Dry = Capture(*this, Text.Replace(TEXT("\"gainDb\":10"), TEXT("\"gainDb\":0")),
            TEXT("triad-pulse-flat"), 28800);
        const auto Cut = Capture(*this, Text.Replace(TEXT("\"gainDb\":10"), TEXT("\"gainDb\":-10")),
            TEXT("triad-pulse-cut"), 28800);
        if (Boost.IsEmpty() || Dry.IsEmpty() || Cut.IsEmpty()) return false;
        const auto Amplitude = [](const TArray<float>& Audio, double Hertz)
        {
            double Real = 0, Imag = 0;
            for (int32 Frame = 4000; Frame < 24000; ++Frame)
            {
                const double Phase = UE_DOUBLE_TWO_PI * Hertz * Frame / SampleRate;
                Real += Audio[Frame * 2] * FMath::Cos(Phase);
                Imag += Audio[Frame * 2] * FMath::Sin(Phase);
            }
            return FMath::Sqrt(Real * Real + Imag * Imag);
        };
        for (const auto [Hertz, Expected] : {TPair<double, double>{24., 10.}, {240., 5.}, {4800., 0.}})
        {
            const double Raise = 20. * FMath::LogX(10., Amplitude(Boost, Hertz) / Amplitude(Dry, Hertz));
            const double Lower = 20. * FMath::LogX(10., Amplitude(Cut, Hertz) / Amplitude(Dry, Hertz));
            TestTrue(TEXT("Twice the shelf gain at the low end as at the cutoff, flat at high frequencies"),
                FMath::Abs(Raise - Expected) < .01 && FMath::Abs(Lower + Expected) < .01);
            AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH shelfHz=%g boostDb=%.9f cutDb=%.9f"), Hertz, Raise, Lower));
        }
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCausalDelayTest, "DreamSFX.V2.SignalPath.CausalDelay",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FCausalDelayTest::RunTest(const FString&)
    {
        const FString Text = MakeGraph(TEXT("test.echo_beacon"), 2.2, TEXT(""),
            TEXT(R"({"id":"echo","type":"delay.causal","version":1,"parameters":{"delaySeconds":0.71}},
                {"id":"return","type":"mix.gain","version":1,"parameters":{"level":0.34}},
                {"id":"beacon","type":"osc.sine","version":1,"parameters":{"frequencyHz":329.6,"detuneCents":0}},
                {"id":"shape","type":"mix.gain","version":1,"parameters":{"level":0.24}})"),
            TEXT(R"({"from":{"node":"shape","port":"audio"},"to":{"node":"echo","port":"input"},"order":0},
                {"from":{"node":"return","port":"audio"},"to":{"node":"echo","port":"input"},"order":1},
                {"from":{"node":"echo","port":"audio"},"to":{"node":"return","port":"input"},"order":0},
                {"from":{"node":"beacon","port":"audio"},"to":{"node":"shape","port":"input"},"order":0})"),
            TEXT(R"({"atSeconds":0.001,"action":"trigger","targets":[{"node":"beacon","port":"start"}]},
                {"atSeconds":0.041,"action":"trigger","targets":[{"node":"beacon","port":"stop"}]},
                {"atSeconds":0.02,"action":"set","targets":[{"node":"shape","port":"level"}],"value":0.24},
                {"atSeconds":0.041,"action":"linear","targets":[{"node":"shape","port":"level"}],"value":0})"), TEXT("echo"));
        const auto InstantaneousLoop = FGraphCompiler::Compile(Text.Replace(TEXT("delay.causal"), TEXT("mix.gain"))
            .Replace(TEXT("delaySeconds"), TEXT("level")));
        TestTrue(TEXT("Feedback without a causal delay is rejected"), !InstantaneousLoop.Graph &&
            InstantaneousLoop.Errors.Num() == 1 && InstantaneousLoop.Errors[0].Code == TEXT("DSFXV205"));
        TestFalse(TEXT("A delay shorter than one quantum at the minimum supported rate is rejected"),
            FGraphCompiler::Compile(Text.Replace(TEXT("0.71"), TEXT("0.001"))).Graph.IsValid());
        const auto Echo = Capture(*this, Text, TEXT("echo-beacon-feedback"), 105600);
        const auto Dry = Capture(*this, Text.Replace(TEXT("\"output\":{\"node\":\"echo\",\"port\":\"audio\"}"),
            TEXT("\"output\":{\"node\":\"shape\",\"port\":\"audio\"}")), TEXT("echo-beacon-direct"), 105600);
        const auto Long = Capture(*this, Text.Replace(TEXT("0.71"), TEXT("2")), TEXT("echo-beacon-two-seconds"), 105600);
        if (Echo.IsEmpty() || Dry.IsEmpty() || Long.IsEmpty()) return false;
        constexpr int32 Delay = 34080;
        double MaxDelta = 0, LongDelta = 0;
        for (int32 Frame = 0; Frame < 105600; ++Frame)
        {
            for (int32 Channel = 0; Channel < 2; ++Channel)
            {
                const int32 Index = 2 * Frame + Channel;
                const double Expected = Frame < Delay ? 0 : Dry[Index - 2 * Delay] + .34 * Echo[Index - 2 * Delay];
                MaxDelta = FMath::Max(MaxDelta, FMath::Abs(Echo[Index] - Expected));
                const double ExpectedLong = Frame < 96000 ? 0 : Dry[Index - 192000];
                LongDelta = FMath::Max(LongDelta, FMath::Abs(Long[Index] - ExpectedLong));
            }
        }
        TestTrue(TEXT("Every feedback sample equals dry input plus scaled feedback exactly 34080 frames earlier"), MaxDelta < 1.e-8);
        TestTrue(TEXT("Two-second buffer delays the waveform by exactly 96000 frames"), LongDelta == 0.);
        AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH echoDelayFrames=%d echoRecurrenceMax=%.12g longDelayFrames=%d longDelayMax=%.12g"),
            Delay, MaxDelta, 96000, LongDelta));
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModulatorTest, "DreamSFX.V2.SignalPath.Modulator",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FModulatorTest::RunTest(const FString&)
    {
        const FString Text = MakeGraph(TEXT("test.glow_pulse"), .8, TEXT(""),
            TEXT(R"({"id":"tone","type":"osc.sine","version":1,"parameters":{"frequencyHz":521,"detuneCents":0}},
                {"id":"swell","type":"mod.sine","version":1,"parameters":{"frequencyHz":3,"amplitude":0.16,"offset":0}},
                {"id":"flutter","type":"mod.sine","version":1,"parameters":{"frequencyHz":7,"amplitude":0.07,"offset":0,"phaseCycles":0.25}},
                {"id":"level","type":"mix.gain","version":1,"parameters":{"level":0.35}})"),
            TEXT(R"({"from":{"node":"tone","port":"audio"},"to":{"node":"level","port":"input"},"order":0},
                {"from":{"node":"swell","port":"control"},"to":{"node":"level","port":"level"},"order":0},
                {"from":{"node":"flutter","port":"control"},"to":{"node":"level","port":"level"},"order":1})"),
            TEXT(R"({"atSeconds":0,"action":"trigger","targets":[{"node":"tone","port":"start"},{"node":"swell","port":"start"}]},
                {"atSeconds":0.02,"action":"trigger","targets":[{"node":"flutter","port":"start"}]},
                {"atSeconds":0.7,"action":"trigger","targets":[{"node":"tone","port":"stop"},{"node":"swell","port":"stop"},{"node":"flutter","port":"stop"}]})"), TEXT("level"));
        const auto Invalid = FGraphCompiler::Compile(Text.Replace(
            TEXT(R"("node":"flutter","port":"control"},"to":{"node":"level","port":"level"})"),
            TEXT(R"("node":"flutter","port":"audio"},"to":{"node":"level","port":"input"})")));
        TestTrue(TEXT("A control-only output cannot be wired into an audio input"), !Invalid.Graph &&
            Invalid.Errors.Num() == 1 && Invalid.Errors[0].Code == TEXT("DSFXV203"));
        const auto Modulated = Capture(*this, Text, TEXT("glow-pulse-two-lfos"), 38400);
        const auto Dry = Capture(*this, Text.Replace(TEXT("\"amplitude\":0.16"), TEXT("\"amplitude\":0"))
            .Replace(TEXT("\"amplitude\":0.07"), TEXT("\"amplitude\":0")), TEXT("glow-pulse-no-lfo"), 38400);
        if (Modulated.IsEmpty() || Dry.IsEmpty()) return false;
        double MaxDelta = 0;
        for (int32 Frame = 0; Frame < 38400; ++Frame)
        {
            // Measure the control sum at its declared float precision. The dry capture
            // contains the same carrier, already rounded by a float multiplication.
            const float Slow = .16f * float(FMath::Sin(UE_DOUBLE_TWO_PI * 3. * Frame / SampleRate));
            const float Fast = Frame < 960 ? 0.f : .07f * float(FMath::Cos(UE_DOUBLE_TWO_PI * 7. * (Frame - 960) / SampleRate));
            const float Gain = (.35f + Slow) + Fast;
            const double Expected = double(Dry[2 * Frame]) * Gain / .35f;
            MaxDelta = FMath::Max(MaxDelta, FMath::Abs(Modulated[2 * Frame] - Expected));
        }
        TestTrue(TEXT("Modulators sum with the authored gain at audio rate, including a sample-aligned start"), MaxDelta < 1.e-7);
        TestTrue(TEXT("The second LFO changes the output after it starts"), Modulated != Dry);
        AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH modulatorSumMaxDelta=%.12g secondStartFrame=960"), MaxDelta));
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuxSendTest, "DreamSFX.V2.SignalPath.AuxSend",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FAuxSendTest::RunTest(const FString&)
    {
        const FString Text = MakeGraph(TEXT("test.ceramic_gallery"), 1.1,
            TEXT(R"({"id":"ir","type":"impulse.room","version":1,"seed":24080,"framesAt48k":19200})"),
            TEXT(R"({"id":"ping","type":"osc.triangle","version":1,"parameters":{"frequencyHz":659.25,"detuneCents":0}},
                {"id":"env","type":"mix.gain","version":1,"parameters":{"level":0.34}},
                {"id":"sends","type":"mix.send","version":1,"parameters":{"auxAGain":0.61,"auxBGain":0.43}},
                {"id":"room","type":"room.convolution","version":1,"parameters":{"impulse":"ir"}},
                {"id":"echo","type":"delay.causal","version":1,"parameters":{"delaySeconds":0.18}},
                {"id":"color","type":"filter.lowpass","version":1,"parameters":{"cutoffHz":1450,"resonanceDb":0.5}},
                {"id":"reflected","type":"mix.gain","version":1,"parameters":{"level":0.35}},
                {"id":"out","type":"mix.gain","version":1,"parameters":{"level":1}})"),
            TEXT(R"({"from":{"node":"ping","port":"audio"},"to":{"node":"env","port":"input"},"order":0},
                {"from":{"node":"env","port":"audio"},"to":{"node":"sends","port":"input"},"order":0},
                {"from":{"node":"sends","port":"auxA"},"to":{"node":"room","port":"input"},"order":0},
                {"from":{"node":"sends","port":"auxB"},"to":{"node":"echo","port":"input"},"order":0},
                {"from":{"node":"echo","port":"audio"},"to":{"node":"color","port":"input"},"order":0},
                {"from":{"node":"color","port":"audio"},"to":{"node":"reflected","port":"input"},"order":0},
                {"from":{"node":"reflected","port":"audio"},"to":{"node":"echo","port":"input"},"order":1},
                {"from":{"node":"sends","port":"audio"},"to":{"node":"out","port":"input"},"order":0},
                {"from":{"node":"room","port":"audio"},"to":{"node":"out","port":"input"},"order":1},
                {"from":{"node":"color","port":"audio"},"to":{"node":"out","port":"input"},"order":2})"),
            TEXT(R"({"atSeconds":0,"action":"trigger","targets":[{"node":"ping","port":"start"}]},
                {"atSeconds":0.012,"action":"set","targets":[{"node":"env","port":"level"}],"value":0.34},
                {"atSeconds":0.08,"action":"exponential","targets":[{"node":"env","port":"level"}],"value":0.0001},
                {"atSeconds":0.08,"action":"trigger","targets":[{"node":"ping","port":"stop"}]})"), TEXT("out"));
        const auto AtOutput = [&Text](const TCHAR* Output)
        {
            return Text.Replace(TEXT(R"("output":{"node":"out","port":"audio"})"),
                *FString::Printf(TEXT(R"("output":{"node":"%s","port":"audio"})"), Output));
        };
        const auto Full = Capture(*this, Text, TEXT("ceramic-gallery-combined"), 52800);
        const auto Dry = Capture(*this, AtOutput(TEXT("sends")), TEXT("ceramic-gallery-dry"), 52800);
        const auto Room = Capture(*this, AtOutput(TEXT("room")), TEXT("ceramic-gallery-room"), 52800);
        const auto Echo = Capture(*this, AtOutput(TEXT("color")), TEXT("ceramic-gallery-echo"), 52800);
        const auto Tap = Capture(*this, Text.Replace(TEXT(R"("output":{"node":"out","port":"audio"})"),
            TEXT(R"("output":{"node":"sends","port":"auxA"})")), TEXT("ceramic-gallery-send-a"), 52800);
        if (Full.IsEmpty() || Dry.IsEmpty() || Room.IsEmpty() || Echo.IsEmpty() || Tap.IsEmpty()) return false;
        float MixDelta = 0.f, TapDelta = 0.f, EchoBefore = 0.f, EchoAfter = 0.f, RoomAfter = 0.f;
        for (int32 Index = 0; Index < Full.Num(); ++Index)
        {
            MixDelta = FMath::Max(MixDelta, FMath::Abs(Full[Index] - ((Dry[Index] + Room[Index]) + Echo[Index])));
            TapDelta = FMath::Max(TapDelta, FMath::Abs(Tap[Index] - Dry[Index] * .61f));
            if (Index / 2 < 8640) EchoBefore = FMath::Max(EchoBefore, FMath::Abs(Echo[Index]));
            else EchoAfter = FMath::Max(EchoAfter, FMath::Abs(Echo[Index]));
            if (Index / 2 > 3840) RoomAfter = FMath::Max(RoomAfter, FMath::Abs(Room[Index]));
        }
        TestTrue(TEXT("Dry, convolution and feedback returns arrive separately and sum in authored order"),
            MixDelta == 0.f && TapDelta == 0.f && RoomAfter > .001f && EchoBefore == 0.f && EchoAfter > .001f);
        AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH sendMixMaxDelta=%g sendTapMaxDelta=%g roomTailPeak=%.9f feedbackPeak=%.9f delayFrames=8640"),
            MixDelta, TapDelta, RoomAfter, EchoAfter));
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrainTest, "DreamSFX.V2.SignalPath.Grain",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FGrainTest::RunTest(const FString&)
    {
        constexpr int32 TotalFrames = 50400;
        const FString Text = MakeGraph(TEXT("test.falling_porcelain"), 1.05,
            TEXT(R"({"id":"source","type":"buffer.white","version":1,"seed":12091,"framesAt48k":1920})"),
            TEXT(R"({"id":"dust","type":"source.grain","version":1,"parameters":{"buffer":"source","seed":91037,
                "densityHz":180,"grainSeconds":0.04,"decayDb":-54,"minRate":0.7,"maxRate":1.3,
                "gain":0.12,"gainJitter":0.4,"panSpread":0.8}},
                {"id":"tone","type":"filter.bandpass","version":1,"parameters":{"cutoffHz":2800,"q":1.7}})"),
            TEXT(R"({"from":{"node":"dust","port":"audio"},"to":{"node":"tone","port":"input"},"order":0})"),
            TEXT(R"({"atSeconds":0.2,"action":"trigger","targets":[{"node":"dust","port":"start"}]},
                {"atSeconds":0.64,"action":"trigger","targets":[{"node":"dust","port":"stop"}]})"), TEXT("tone"));
        const auto Spread = Capture(*this, Text, TEXT("falling-porcelain-spread"), TotalFrames);
        const auto Repeat = Capture(*this, Text, TEXT("falling-porcelain-repeat"), TotalFrames);
        const auto Mono = Capture(*this, Text.Replace(TEXT("\"panSpread\":0.8"), TEXT("\"panSpread\":0")),
            TEXT("falling-porcelain-center"), TotalFrames);
        const auto OtherSeed = Capture(*this, Text.Replace(TEXT("\"seed\":91037"), TEXT("\"seed\":91038")),
            TEXT("falling-porcelain-reseed"), TotalFrames);
        if (Spread.IsEmpty() || Repeat.IsEmpty() || Mono.IsEmpty() || OtherSeed.IsEmpty()) return false;
        TestTrue(TEXT("Same grain seed, same stereo PCM on a fresh instance"), Spread == Repeat);
        TestTrue(TEXT("Changing the timing and pan seed changes the actual generated sound"), Spread != OtherSeed);
        double StereoDifference = 0, CenterDifference = 0;
        for (int32 Frame = 0; Frame < TotalFrames; ++Frame)
        {
            StereoDifference += FMath::Square(double(Spread[2 * Frame] - Spread[2 * Frame + 1]));
            CenterDifference += FMath::Square(double(Mono[2 * Frame] - Mono[2 * Frame + 1]));
        }
        TestTrue(TEXT("Independent grain pan contributes stereo energy; centered grains are dual mono"),
            StereoDifference > .01 && CenterDifference == 0.);
        const auto BadRates = FGraphCompiler::Compile(Text.Replace(TEXT("\"minRate\":0.7"), TEXT("\"minRate\":1.4")));
        TestTrue(TEXT("Reversed rate range is diagnosed at maxRate"), !BadRates.Graph && BadRates.Errors.Num() == 1 &&
            BadRates.Errors[0].Pointer == TEXT("/nodes/0/parameters/maxRate"));

        const auto Single = Capture(*this, Text.Replace(TEXT(R"("output":{"node":"tone","port":"audio"})"),
            TEXT(R"("output":{"node":"dust","port":"audio"})"))
            .Replace(TEXT("\"densityHz\":180"), TEXT("\"densityHz\":1"))
            .Replace(TEXT("\"minRate\":0.7,\"maxRate\":1.3"), TEXT("\"minRate\":1,\"maxRate\":1"))
            .Replace(TEXT("\"panSpread\":0.8"), TEXT("\"panSpread\":0"))
            .Replace(TEXT("\"gainJitter\":0.4"), TEXT("\"gainJitter\":0"))
            .Replace(TEXT("\"atSeconds\":0.64"), TEXT("\"atSeconds\":0.74")), TEXT("falling-porcelain-single-grain"), TotalFrames);
        if (Single.IsEmpty()) return false;
        constexpr int32 First = 34449; // Known onset from seed 91037 and the 0.2 s gate.
        constexpr int32 Length = 1920;
        uint32 State = 12091;
        double MaxDelta = 0;
        for (int32 Frame = 0; Frame < TotalFrames; ++Frame)
        {
            double Expected = 0;
            if (Frame >= First && Frame < First + Length)
            {
                State = 1664525u * State + 1013904223u;
                const double White = double(State) / 2147483648. - 1.;
                const double Exponent = -54. * (Frame - First) / (20. * (Length - 1));
                Expected = White * .12 * FMath::Sqrt(.5) * FMath::Pow(10., Exponent);
            }
            MaxDelta = FMath::Max3(MaxDelta, FMath::Abs(Single[2 * Frame] - Expected), FMath::Abs(Single[2 * Frame + 1] - Expected));
        }
        TestTrue(TEXT("A single seeded grain has exactly 1920 frames of exponential decay including its release after stop"),
            MaxDelta < 1.e-7 && Single[2 * (First + Length - 1)] != 0.f && Single[2 * (First + Length)] == 0.f);
        AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH grainStart=%d grainFrames=%d grainEnvelopeMaxDelta=%.12g spreadDifferenceEnergy=%.9f centerDifferenceEnergy=%g"),
            First, Length, MaxDelta, StereoDifference, CenterDifference));
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrainBandpassTest, "DreamSFX.V2.SignalPath.GrainBandpass",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FGrainBandpassTest::RunTest(const FString&)
    {
        constexpr int32 Frames = 50400;
        const auto RenderGrains = [this, Frames](double Density, double Minimum, double Maximum, bool FilterAfter, const TCHAR* Name)
        {
            auto Graph = MakeShared<FGraphIR, ESPMode::ThreadSafe>();
            Graph->Id = TEXT("test.ceramic_specks");
            Graph->DurationSeconds = 1.05;
            Graph->Resources.Add({TEXT("noise"), {TEXT("buffer.white"), 1}, 711, 1920});
            FNode Grain{TEXT("grains"), {TEXT("source.grain"), 1}, {}};
            Grain.Parameters.Add(TEXT("buffer"), FValue(TInPlaceType<FName>(), FName(TEXT("noise"))));
            const TPair<FName, double> Values[] = {
                {TEXT("seed"), 91037.}, {TEXT("densityHz"), Density}, {TEXT("grainSeconds"), .04},
                {TEXT("decayDb"), -48.}, {TEXT("minRate"), 1.}, {TEXT("maxRate"), 1.},
                {TEXT("gain"), .12}, {TEXT("gainJitter"), 0.}, {TEXT("panSpread"), .65},
                {TEXT("minBandpassHz"), Minimum}, {TEXT("maxBandpassHz"), Maximum}, {TEXT("bandpassQ"), 1.5}
            };
            for (const auto& Value : Values)
                Grain.Parameters.Add(Value.Key, FValue(TInPlaceType<double>(), Value.Value));
            Graph->Nodes.Add(MoveTemp(Grain));
            Graph->ExecutionOrder.Add(TEXT("grains"));
            Graph->Timeline.Add({{TEXT("grains"), TEXT("start")}, .2, EAction::Trigger});
            Graph->Timeline.Add({{TEXT("grains"), TEXT("stop")}, .74, EAction::Trigger});
            Graph->Output = {TEXT("grains"), TEXT("audio")};
            if (FilterAfter)
            {
                FNode Filter{TEXT("post"), {TEXT("filter.bandpass"), 1}, {}};
                Filter.Parameters.Add(TEXT("cutoffHz"), FValue(TInPlaceType<double>(), 2400.));
                Filter.Parameters.Add(TEXT("q"), FValue(TInPlaceType<double>(), 1.5));
                Graph->Nodes.Add(MoveTemp(Filter));
                Graph->ExecutionOrder.Add(TEXT("post"));
                Graph->Connections.Add({{TEXT("grains"), TEXT("audio")}, {TEXT("post"), TEXT("input")}});
                Graph->Output = {TEXT("post"), TEXT("audio")};
            }
            return CaptureGraph(*this, Graph, Name, Frames);
        };
        const auto Post = RenderGrains(1., 0., 0., true, TEXT("ceramic-specks-post-filter"));
        const auto Inside = RenderGrains(1., 2400., 2400., false, TEXT("ceramic-specks-single"));
        if (Post.IsEmpty() || Inside.IsEmpty()) return false;
        constexpr int32 Onset = 34449;
        constexpr int32 End = Onset + 1920;
        float MaxDelta = 0.f;
        for (int32 Sample = 2 * Onset; Sample < 2 * End; ++Sample)
            MaxDelta = FMath::Max(MaxDelta, FMath::Abs(Inside[Sample] - Post[Sample]));
        TestTrue(TEXT("A single grain matches a separate band-pass during the grain window"), MaxDelta < 5.e-8f);
        TestTrue(TEXT("The independent filter history ends with its grain"), Inside[2 * End] == 0.f && Post[2 * End] != 0.f);

        const auto Fixed = RenderGrains(180., 2400., 2400., false, TEXT("ceramic-specks-fixed"));
        const auto Spread = RenderGrains(180., 900., 6100., false, TEXT("ceramic-specks-ranged"));
        const auto Replay = RenderGrains(180., 900., 6100., false, TEXT("ceramic-specks-replay"));
        if (Fixed.IsEmpty() || Spread.IsEmpty() || Replay.IsEmpty()) return false;
        TestTrue(TEXT("A fresh MetaSound instance reproduces the same random filter assignment"), Spread == Replay);
        const auto FirstActive = [](const TArray<float>& Pcm)
        {
            return Pcm.IndexOfByPredicate([](float Sample) { return Sample != 0.f; });
        };
        TestTrue(TEXT("Changing the filter range leaves the seeded onset in place"), FirstActive(Fixed) == FirstActive(Spread));
        double DifferenceEnergy = 0.;
        for (int32 Sample = 0; Sample < Fixed.Num(); ++Sample)
            DifferenceEnergy += FMath::Square(double(Fixed[Sample] - Spread[Sample]));
        TestTrue(TEXT("Individually colored overlapping grains change the render"), DifferenceEnergy > .01);
        const auto Bad = FGraphCompiler::Compile(TEXT(R"(sound test.invalid_grain
preview 1.05 s
resources { noise=buffer.white(.04s) }
bus test {
    grains=source.grain(buffer=noise,seed=91037,densityHz=180,grainSeconds=.04,decayDb=-48,minRate=1,maxRate=1,gain=.12,gainJitter=0,panSpread=.65,minBandpassHz=3200,maxBandpassHz=1200,bandpassQ=1.5)
}
output test.grains
)"));
        TestTrue(TEXT("A reversed frequency range is diagnosed at maxBandpassHz"),
            !Bad.Graph && Bad.Errors.Num() == 1 && Bad.Errors[0].Message.Contains(TEXT("maxBandpassHz")));
        AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH grainBandpassMaxDelta=%.12g firstActiveFrame=%d rangedDifferenceEnergy=%.9f"),
            MaxDelta, FirstActive(Spread) / 2, DifferenceEnergy));
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompressorControlsTest, "DreamSFX.V2.SignalPath.Compressor",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FCompressorControlsTest::RunTest(const FString&)
    {
        constexpr int32 TotalFrames = 57600;
        const FString Text = MakeGraph(TEXT("test.hydraulic_chirp"), 1.2, TEXT(""),
            TEXT(R"({"id":"tone","type":"osc.sawtooth","version":1,"parameters":{"frequencyHz":294,"detuneCents":0}},
                {"id":"env","type":"mix.gain","version":1,"parameters":{"level":0.05}},
                {"id":"press","type":"dynamics.compressor","version":1,"parameters":{"thresholdDb":-6,"kneeDb":6,
                    "ratio":8,"attackSeconds":0.003,"releaseSeconds":0.3}})"),
            TEXT(R"({"from":{"node":"tone","port":"audio"},"to":{"node":"env","port":"input"},"order":0},
                {"from":{"node":"env","port":"audio"},"to":{"node":"press","port":"input"},"order":0})"),
            TEXT(R"({"atSeconds":0,"action":"trigger","targets":[{"node":"tone","port":"start"}]},
                {"atSeconds":0.1,"action":"set","targets":[{"node":"env","port":"level"}],"value":0.73},
                {"atSeconds":0.2,"action":"set","targets":[{"node":"env","port":"level"}],"value":0.07},
                {"atSeconds":0.35,"action":"set","targets":[{"node":"env","port":"level"}],"value":0.78},
                {"atSeconds":0.55,"action":"set","targets":[{"node":"env","port":"level"}],"value":0.05},
                {"atSeconds":1.1,"action":"trigger","targets":[{"node":"tone","port":"stop"}]})"), TEXT("press"));
        const auto Reference = Capture(*this, Text, TEXT("hydraulic-chirp-compressed"), TotalFrames);
        const auto Dry = Capture(*this, Text.Replace(TEXT(R"("output":{"node":"press","port":"audio"})"),
            TEXT(R"("output":{"node":"env","port":"audio"})")), TEXT("hydraulic-chirp-dry"), TotalFrames);
        if (Reference.IsEmpty() || Dry.IsEmpty()) return false;
        TestTrue(TEXT("Compressor produces a distinct waveform"), Dry != Reference);
        struct FVariation { const TCHAR* Field; const TCHAR* Before; const TCHAR* After; };
        for (const auto& Item : {FVariation{TEXT("thresholdDb"), TEXT("-6"), TEXT("-12")},
            {TEXT("kneeDb"), TEXT("6"), TEXT("0")}, {TEXT("ratio"), TEXT("8"), TEXT("2")},
            {TEXT("attackSeconds"), TEXT("0.003"), TEXT("0.04")}, {TEXT("releaseSeconds"), TEXT("0.3"), TEXT("0.02")}})
        {
            const FString Before = FString::Printf(TEXT("\"%s\":%s"), Item.Field, Item.Before);
            const FString After = FString::Printf(TEXT("\"%s\":%s"), Item.Field, Item.After);
            const auto Modified = Capture(*this, Text.Replace(*Before, *After),
                *FString::Printf(TEXT("hydraulic-chirp-%s"), Item.Field), TotalFrames);
            if (Modified.IsEmpty()) return false;
            float MaxDelta = 0.f;
            for (int32 Index = 0; Index < Reference.Num(); ++Index)
                MaxDelta = FMath::Max(MaxDelta, FMath::Abs(Modified[Index] - Reference[Index]));
            TestTrue(FString::Printf(TEXT("%s independently changes the generated PCM"), Item.Field), MaxDelta > .001f);
            AddInfo(FString::Printf(TEXT("DSFX_SIGNAL_PATH compressorField=%s maxDelta=%.9f"), Item.Field, MaxDelta));
        }
        const auto Invalid = FGraphCompiler::Compile(Text.Replace(TEXT("\"ratio\":8"), TEXT("\"ratio\":0")));
        TestTrue(TEXT("Invalid compressor ratio is diagnosed at the named field"), !Invalid.Graph && Invalid.Errors.Num() == 1 &&
            Invalid.Errors[0].Pointer == TEXT("/nodes/2/parameters/ratio"));
        return true;
    }
}
#endif
