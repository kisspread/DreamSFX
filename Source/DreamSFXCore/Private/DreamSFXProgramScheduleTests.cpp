#include "DreamSFXProgramRunner.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDslBlastSchedule,"DreamSFX.DSL.BlastSchedule",
        EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

    bool FDslBlastSchedule::RunTest(const FString&)
    {
        FString Path;
        if(!FParse::Value(FCommandLine::Get(),TEXT("DreamSFXScheduleSource="),Path))return true;
        FString Source;
        if(!FFileHelper::LoadFileToString(Source,*Path))return false;
        const auto Compiled=FGraphCompiler::Compile(Source);
        if(!Compiled.Graph){for(const auto& E:Compiled.Errors)AddError(E.Pointer+TEXT(" ")+E.Message);return false;}
        const auto& Graph=*Compiled.Graph;
        if(!TestTrue(TEXT("The blast owns rules and has no recorded timeline"),Graph.Program&&Graph.Timeline.IsEmpty()))return false;

        // Exercise the control interpreter only. No audio kernels, generated buffers or PCM renderer are constructed.
        TArray<FControlState> Controls;
        TArray<DSFXGraphDSP::FState> Signals;
        TMap<FName,int32> Nodes;
        TMap<FPortRef,int32> Ports;
        for(const FNode& Node:Graph.Nodes)
        {
            Nodes.Add(Node.Id,Signals.AddDefaulted());
            for(const auto& Field:FindNodeType(Node.Type)->Controls)
            {
                const int32 Slot=Controls.AddDefaulted();Ports.Add({Node.Id,Field.Name},Slot);
                Controls[Slot].Base=float(Node.Parameters.FindChecked(Field.Name).Get<double>());
            }
        }
        FProgramRunner Performance(*Graph.Program,48000,Controls,Signals,Nodes,Ports,[](int32){});
        auto Run=[&](bool VaryWorld)
        {
            Performance.Restart();
            if(VaryWorld)
            {
                Performance.Input(TEXT("charge"),.3);
                Performance.Input(TEXT("shockRadius"),0);
                Performance.Input(TEXT("broken"),0);
            }
            TArray<FName> States;
            for(int64 Frame=0;Frame<912000;Frame+=DSFXGraphDSP::Block)
            {
                Performance.Quantum(Frame);
                if(Performance.HasError())return false;
                const FName Name=Performance.StateName();
                if(States.IsEmpty()||States.Last()!=Name)States.Add(Name);
            }
            const TArray<FName> Expected={TEXT("idle"),TEXT("gather"),TEXT("fall"),TEXT("silence"),TEXT("impact"),TEXT("mainBlast"),TEXT("ringing"),TEXT("tail"),TEXT("idle")};
            TestTrue(TEXT("State trace through all seven sound phases"),States==Expected);
            TestEqual(TEXT("70 separate ember onsets"),Performance.PlayCount(TEXT("ember")),70u);
            TestEqual(TEXT("Debris responds to the destruction counter"),Performance.PlayCount(TEXT("stone")),VaryWorld?0u:29u);
            TestEqual(TEXT("Shock responds to its radius"),Performance.PlayCount(TEXT("gust")),VaryWorld?0u:1u);
            TestEqual(TEXT("Charge threshold controls the number of runes"),Performance.PlayCount(TEXT("glyph")),VaryWorld?2u:6u);
            TestEqual(TEXT("Nine glints per written rune"),Performance.PlayCount(TEXT("runeGlint")),VaryWorld?18u:54u);
            TestEqual(TEXT("Six pairs of glass partials"),Performance.PlayCount(TEXT("glass")),6u);
            AddInfo(FString::Printf(TEXT("DSFX_SCHEDULE variant=%s frames=912000 states=%d pulse=%u glyph=%u glint=%u glass=%u gust=%u ember=%u stone=%u"),
                VaryWorld?TEXT("limited"):TEXT("audition"),States.Num(),Performance.PlayCount(TEXT("pulse")),Performance.PlayCount(TEXT("glyph")),
                Performance.PlayCount(TEXT("runeGlint")),Performance.PlayCount(TEXT("glass")),Performance.PlayCount(TEXT("gust")),
                Performance.PlayCount(TEXT("ember")),Performance.PlayCount(TEXT("stone"))));
            return !HasAnyErrors();
        };
        return Run(false)&&Run(true);
    }
}
#endif
