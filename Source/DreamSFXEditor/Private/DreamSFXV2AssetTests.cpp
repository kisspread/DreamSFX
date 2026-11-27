#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphInstance.h"
#include "HAL/PlatformMemory.h"
#include "MetasoundSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

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
