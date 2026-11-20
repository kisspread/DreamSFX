#include "DreamSFXGraphArchive.h"
#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXSoundDocument.h"
#include "HAL/PlatformMemory.h"
#include "HAL/FileManager.h"
#include "MetasoundSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SDreamSFXWaveform.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    UMetaSoundSource* LoadSavedGraph(FName Id)
    {
        const FString Package = DreamSFX::V2::GraphPackageName(Id);
        const FString Path = Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
        return LoadObject<UMetaSoundSource>(nullptr, *Path);
    }

    DreamSFX::V2::FGraphPtr ReadSavedGraph(FName Id)
    {
        auto* Source = LoadSavedGraph(Id);
        auto* Data = Source ? FindObject<UDreamSFXGraphAsset>(Source, TEXT("DreamSFXGraph")) : nullptr;
        return Data ? Data->GetGraph() : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamSFXV2AssetLifecycle, "DreamSFX.V2.AssetLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDreamSFXV2AssetLifecycle::RunTest(const FString&)
{
    FString Edition, Factory;
    FParse::Value(FCommandLine::Get(), TEXT("DreamSFXV2LifecycleDir="), Edition);
    FParse::Value(FCommandLine::Get(), TEXT("DreamSFXV2Fixtures="), Factory);
    if (!TestTrue(TEXT("Provide the isolated instance folder and V2 library"), !Edition.IsEmpty() && !Factory.IsEmpty())) return false;
    const FString ParentId = TEXT("benchmark.sine_confirm_soft");
    const FString InstanceId = TEXT("v2-cutover-instance");
    FDreamSFXSoundDocument Document;
    FString Error;
    if (!TestTrue(TEXT("Read V2 factory source"), Document.Load(Factory, ParentId, Error))) { AddError(Error); return false; }
    const auto Parent = Document.GetGraph();
    const auto ParentWave = FDreamSFXWaveformData::Render(Parent);
    if (FParse::Param(FCommandLine::Get(), TEXT("DreamSFXV2SaveTestInstance")))
    {
        if (!TestTrue(TEXT("Create a project instance through the sound document"), Document.CreateProjectInstance(Edition, InstanceId, Error))) { AddError(Error); return false; }
        const auto* Gain = Document.Parameters().FindByPredicate([](const auto& Parameter) { return Parameter.Key == TEXT("chime.gainDb"); });
        if (!TestNotNull(TEXT("Named gain is editable"), Gain)) return false;
        const auto Parameter = *Gain;
        if (!TestTrue(TEXT("Override only the instance gain"), Document.SetOverride(Parameter, Parameter.Default - 6., Error))) { AddError(Error); return false; }
        const auto PreviewWave = FDreamSFXWaveformData::Render(Document.GetGraph());
        TestTrue(TEXT("Waveform RMS follows the -6 dB override"), FMath::Abs(PreviewWave.RMS / ParentWave.RMS - FMath::Pow(10., -.3)) < 1.e-6);
        if (!TestNotNull(TEXT("The preview path builds the graph player Source"), Document.PreviewSource(Error))) { AddError(Error); return false; }
        if (!TestTrue(TEXT("Save only the instance text"), Document.Save(Error))) { AddError(Error); return false; }
        if (!TestNotNull(TEXT("Save one playable MetaSound asset"), Document.SaveAsset(Error))) { AddError(Error); return false; }
        return true;
    }
    const auto Saved = ReadSavedGraph(FName(*InstanceId));
    if (!TestTrue(TEXT("Fresh process decoded embedded IR"), Saved && Saved->Id == FName(*InstanceId))) return false;
    const auto SavedWave = FDreamSFXWaveformData::Render(Saved);
    const double Db = 20.0 * FMath::LogX(10., SavedWave.RMS / ParentWave.RMS);
    TestTrue(TEXT("Cold saved instance has the persisted -6 dB override"), FMath::Abs(Db + 6) < 1.e-5);
    TestEqual(TEXT("Persisted instance ends with its parent"), SavedWave.Frames, ParentWave.Frames);
    TArray<uint8> Payload;
    TestTrue(TEXT("Compiled graph encodes"), DreamSFX::V2::EncodeGraph(*Saved, Payload));
    Payload.Reset();
    TestFalse(TEXT("Empty archived graph refused"), DreamSFX::V2::DecodeGraph(Payload).IsValid());
    AddInfo(FString::Printf(TEXT("DSFX_V2_INSTANCE overrideDb=%.9f frames=%lld"), Db, SavedWave.Frames));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamSFXV2CutoverPerformance, "DreamSFX.V2.CutoverPerformance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDreamSFXV2CutoverPerformance::RunTest(const FString&)
{
    using DreamSFX::V2::FGraphInstance;
    for (const TCHAR* Name : {TEXT("sine_confirm_soft"), TEXT("blast_full_opening")})
    {
        const FName Id(*(FString(TEXT("benchmark.")) + Name));
        const double StartLoad = FPlatformTime::Seconds();
        const auto Graph = ReadSavedGraph(Id);
        if (!TestTrue(TEXT("Cold load V2 MetaSound and embedded data"), Graph.IsValid())) return false;
        const double LoadMs = (FPlatformTime::Seconds() - StartLoad) * 1000.;
        constexpr int32 Count = 32;
        TArray<TUniquePtr<FGraphInstance>> Instances;
        Instances.Reserve(Count);
        const auto MemoryBefore = FPlatformMemory::GetStats().UsedPhysical;
        const double StartPrepare = FPlatformTime::Seconds();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            FString Error;
            auto Instance = MakeUnique<FGraphInstance>();
            if (!TestTrue(TEXT("Prepare persisted graph"), Instance->Prepare(Graph, 48000, Error))) { AddError(Error); return false; }
            Instances.Add(MoveTemp(Instance));
        }
        const double PrepareMs = (FPlatformTime::Seconds() - StartPrepare) * 1000.;
        const auto MemoryAfter = FPlatformMemory::GetStats().UsedPhysical;
        AddInfo(FString::Printf(TEXT("DSFX_CUTOVER_PERF version=v2 id=%s loadMs=%.6f prepareMeanMs=%.6f count=%d workingSetDeltaBytes=%lld"),
            Name, LoadMs, PrepareMs / Count, Count, static_cast<long long>(MemoryAfter) - static_cast<long long>(MemoryBefore)));
    }
    TArray<TUniquePtr<FGraphInstance>> Mixed;
    const TCHAR* Ids[] = {TEXT("benchmark.sine_confirm_soft"),TEXT("benchmark.retro_square"),TEXT("benchmark.noise_steady"),TEXT("benchmark.musicbox_single"),
        TEXT("benchmark.piano_small_clock"),TEXT("benchmark.oneshot_explosion"),TEXT("benchmark.noise_wind_gust"),TEXT("benchmark.retro_crickets")};
    for (int32 Index = 0; Index < 32; ++Index)
    {
        FString Error;
        auto Graph = ReadSavedGraph(FName(Ids[Index % UE_ARRAY_COUNT(Ids)]));
        auto Instance = MakeUnique<FGraphInstance>();
        if (!TestTrue(TEXT("Prepare a mixed voice"), Graph && Instance->Prepare(Graph, 48000, Error))) return false;
        Mixed.Add(MoveTemp(Instance));
    }
    const bool bProfile = FParse::Param(FCommandLine::Get(), TEXT("DreamSFXV2ProfileMixed"));
    TArray<double> Timings;
    TArray<double> PerSound[UE_ARRAY_COUNT(Ids)];
    Timings.Reserve(1000);
    for (auto& Sound : PerSound) Sound.Reserve(1000);
    for (int32 Block = 0; Block < 1000; ++Block)
    {
        double Contributions[UE_ARRAY_COUNT(Ids)] = {};
        const double Start = FPlatformTime::Seconds();
        for (int32 Index = 0; Index < Mixed.Num(); ++Index)
        {
            const double VoiceStart = bProfile ? FPlatformTime::Seconds() : 0.;
            auto& Instance = Mixed[Index];
            if (Instance->IsFinished()) Instance->Restart();
            float Samples[512];
            Instance->Render(Samples, 256);
            if (bProfile) Contributions[Index % UE_ARRAY_COUNT(Ids)] += FPlatformTime::Seconds() - VoiceStart;
        }
        Timings.Add((FPlatformTime::Seconds() - Start) * 1000.);
        if (bProfile)
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Ids); ++Index) PerSound[Index].Add(Contributions[Index] * 1000.);
    }
    if (bProfile)
    {
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Ids); ++Index)
        {
            PerSound[Index].Sort();
            AddInfo(FString::Printf(TEXT("DSFX_MIXED32_PROFILE id=%s p50Ms=%.6f p99Ms=%.6f"),
                Ids[Index], PerSound[Index][499], PerSound[Index][989]));
        }
    }
    Timings.Sort();
    const double P99 = Timings[989];
    AddInfo(FString::Printf(TEXT("DSFX_MIXED32 sampleRate=48000 block=256 blocks=1000 p99Ms=%.6f"), P99));
    return TestTrue(TEXT("32 mixed graph instances: render p99 below 1 ms"), P99 < 1.0);
}
#endif
