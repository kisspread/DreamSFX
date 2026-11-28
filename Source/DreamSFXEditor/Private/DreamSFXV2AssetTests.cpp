#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphInstance.h"
#include "HAL/PlatformMemory.h"
#include "MetasoundSource.h"
#include "MetasoundFrontendDocument.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamSFXGeneratedSourceLayout, "DreamSFX.Editor.GeneratedSourceLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamSFXGeneratedSourceLayout::RunTest(const FString&)
{
    // Opening the actual asset editor checks the visible pins, including UE's input templates.
    FString PackagePath;
    if (!FParse::Value(FCommandLine::Get(), TEXT("DreamSFXLayoutAsset="), PackagePath)) return true;

    auto* Source = LoadObject<UMetaSoundSource>(nullptr,
        *(PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath)));
    if (!TestNotNull(TEXT("Load saved Source"), Source)) return false;
    auto* Data = FindObject<UDreamSFXGraphAsset>(Source, TEXT("DreamSFXGraph"));
    if (!TestTrue(TEXT("Embedded sound is available"), Data && Data->GetGraph())) return false;

    const auto& Saved = static_cast<IMetaSoundDocumentInterface&>(*Source).GetConstDocument();
    const auto* PlayerClass = Saved.Dependencies.FindByPredicate([](const auto& Dependency)
    {
        return Dependency.Metadata.GetClassName() == FMetasoundFrontendClassName(TEXT("DreamSFX"), TEXT("GraphPlayer"));
    });
    if (!TestNotNull(TEXT("Saved Graph Player dependency"), PlayerClass)) return false;
    // Check before opening the editor, which can upgrade obsolete class defaults in memory.
    for (int32 Index = 0; Index < 8; ++Index)
    {
        const auto* Input = PlayerClass->GetDefaultInterface().Inputs.FindByPredicate([&](const auto& Vertex)
        {
            return Vertex.Name == DreamSFX::V2::ProgramControlPin(Index);
        });
        float Default = 0.f;
        if (!TestTrue(TEXT("Stored control pin has neutral default"), Input && !Input->GetDefaults().IsEmpty() &&
            Input->GetDefaults()[0].Literal.TryGet(Default) && Default == 0.f)) return false;
    }

    auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    const bool bAlreadyOpen = Editors->FindEditorForAsset(Source, false) != nullptr;
    if (!TestTrue(TEXT("Open MetaSound editor"), Editors->OpenEditorForAsset(Source))) return false;
    ON_SCOPE_EXIT { if (!bAlreadyOpen) Editors->CloseAllEditorsForAsset(Source); };

    UEdGraph* Canvas = Source->GetGraph();
    if (!TestNotNull(TEXT("Source has an editor canvas"), Canvas)) return false;
    UEdGraphNode* Player = nullptr;
    for (UEdGraphNode* Node : Canvas->Nodes)
    {
        if (Node && Node->FindPin(TEXT("Graph"), EGPD_Input) && Node->FindPin(TEXT("Play"), EGPD_Input))
        {
            if (!TestNull(TEXT("Exactly one player is drawn"), Player)) return false;
            Player = Node;
        }
    }
    if (!TestNotNull(TEXT("Graph Player is visible"), Player)) return false;

    TSet<FIntPoint> MemberPositions;
    auto CheckWire = [&](FName Name, EEdGraphPinDirection Direction)
    {
        const UEdGraphPin* Pin = Player->FindPin(Name, Direction);
        if (!TestTrue(Name.ToString() + TEXT(" has one visible wire"), Pin && Pin->LinkedTo.Num() == 1))
            return;
        const UEdGraphNode* Member = Pin->LinkedTo[0]->GetOwningNode();
        if (!TestNotNull(Name.ToString() + TEXT(" connects to a canvas node"), Member)) return;
        const FIntPoint Position(Member->NodePosX, Member->NodePosY);
        TestFalse(Name.ToString() + TEXT(" has its own canvas position"), MemberPositions.Contains(Position));
        MemberPositions.Add(Position);
    };
    CheckWire(TEXT("Play"), EGPD_Input);
    CheckWire(TEXT("Loop"), EGPD_Input);
    CheckWire(TEXT("State"), EGPD_Input);
    const auto& Document = static_cast<IMetaSoundDocumentInterface&>(*Source).GetConstDocument();
    for (int32 Index = 0; Index < Data->GetGraph()->Inputs.Num(); ++Index)
    {
        const auto& Control = Data->GetGraph()->Inputs[Index];
        CheckWire(DreamSFX::V2::ProgramControlPin(Index), EGPD_Input);
        const auto* Input = Document.RootGraph.GetDefaultInterface().Inputs.FindByPredicate(
            [&](const auto& Vertex) { return Vertex.Name == Control.Name; });
        if (!TestTrue(TEXT("Named control has a saved default"), Input && !Input->GetDefaults().IsEmpty())) return false;
        float Value = 0.f;
        if (!Input->GetDefaults()[0].Literal.TryGet(Value)) return false;
        TestEqual(Control.Name.ToString() + TEXT(" member default"), Value, Control.MetaSoundDefault());
        TestTrue(TEXT("Display default is finite and readable"), FMath::IsFinite(Value) && Value != DreamSFX::V2::UnsetProgramControl);
        AddInfo(FString::Printf(TEXT("Graph input %s = %g, DSL default %g, audition expression %d"),
            *Control.Name.ToString(), Value, Control.Default, Control.bHasAuditionExpression));
    }
    for (int32 Index = Data->GetGraph()->Inputs.Num(); Index < 8; ++Index)
    {
        const auto* Pin = Player->FindPin(DreamSFX::V2::ProgramControlPin(Index), EGPD_Input);
        TestTrue(TEXT("Unused control is initially hidden or advanced"), Pin && Pin->LinkedTo.IsEmpty() &&
            (Pin->bHidden || (Pin->bAdvancedView && Player->AdvancedPinDisplay == ENodeAdvancedPins::Hidden)));
        if (Pin) TestTrue(TEXT("Unused control has a neutral default"), FCString::Atof(*Pin->DefaultValue) == 0.f);
    }
    CheckWire(TEXT("Out Left"), EGPD_Output);
    CheckWire(TEXT("Out Right"), EGPD_Output);
    CheckWire(TEXT("On Finished"), EGPD_Output);
    AddInfo(FString::Printf(TEXT("Canvas %s: %d nodes, %d wired members"), *PackagePath,
        Canvas->Nodes.Num(), MemberPositions.Num()));
    return !HasAnyErrors();
}

#endif
