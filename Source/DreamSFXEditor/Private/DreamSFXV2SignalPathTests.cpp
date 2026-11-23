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
}
#endif
