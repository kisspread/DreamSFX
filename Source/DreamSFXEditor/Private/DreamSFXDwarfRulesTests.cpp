#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphArchive.h"
#include "DreamSFXGraphCompiler.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXGraphPreview.h"
#include "MetasoundSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/IConsoleManager.h"
#include "Sound/SoundGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwarfModelSelection, "DreamSFX.DSL.DwarfModelSelection",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FDwarfModelSelection::RunTest(const FString&)
    {
        struct FCheck { const TCHAR* Id; double Slider; int32 Model; };
        const FCheck Checks[] = {{TEXT("dwarf.gearmesh"), .5, 1}, {TEXT("dwarf.gearmesh"), 1.7, 2},
            {TEXT("dwarf.clickthud"), .49, 0}, {TEXT("dwarf.clickthud"), 2.5, 3}, {TEXT("dwarf.clickthud"), 3.8, 4}};
        for (const FCheck& Check : Checks)
        {
            const FString Package = GraphPackageName(FName(Check.Id));
            auto* Source = LoadObject<UMetaSoundSource>(nullptr, *(Package + TEXT(".") + FPaths::GetCleanFilename(Package)));
            const auto* Data = Source ? FindObject<UDreamSFXGraphAsset>(Source, TEXT("DreamSFXGraph")) : nullptr;
            if (!TestTrue(TEXT("Production preset loads"), Data && Data->GetGraph())) return false;
            FGraphInstance Slider, Discrete;
            FString Error;
            if (!Slider.Prepare(Data->GetGraph(), 48000, Error) || !Discrete.Prepare(Data->GetGraph(), 48000, Error))
            {
                AddError(Error);
                return false;
            }
            if (!Slider.SetInput(TEXT("kind"), Check.Slider) || !Discrete.SetInput(TEXT("kind"), Check.Model)) return false;
            float Peak = 0.f;
            for (int32 Frame = 0; Frame < 48000; Frame += 128)
            {
                float A[256], B[256];
                if (Slider.Render(A,128) != 128 || Discrete.Render(B,128) != 128 || FMemory::Memcmp(A,B,sizeof(A)))
                {
                    AddError(FString::Printf(TEXT("%s slider %.2f differs from model %d at block %d"), Check.Id, Check.Slider, Check.Model, Frame));
                    return false;
                }
                for (const float Sample : A) Peak = FMath::Max(Peak, FMath::Abs(Sample));
            }
            TestTrue(TEXT("Selected mechanism is audible"), Peak > .01f);
        }
        return !HasAnyErrors();
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDwarfRulesCapture,"DreamSFX.DSL.DwarfRules",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
    bool FDwarfRulesCapture::RunTest(const FString&)
    {
        FString Out;
        if(!FParse::Value(FCommandLine::Get(),TEXT("DreamSFXDwarfOut="),Out))return true;
        const bool More=FParse::Param(FCommandLine::Get(),TEXT("DreamSFXDwarfButtons"));
        const TArray<FString> Cases=More?TArray<FString>{TEXT("clickKey"),TEXT("clickLever"),TEXT("clickStomp"),TEXT("clickTick")}:
            TArray<FString>{TEXT("gearMesh"),TEXT("gearRoll"),TEXT("gearFar"),TEXT("furnace"),TEXT("clickThud")};
        auto* Async=IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if(!Async)return false;
        const int32 Old=Async->GetInt();Async->Set(0,ECVF_SetByCode);ON_SCOPE_EXIT{Async->Set(Old,ECVF_SetByCode);};
        uint64 Unique=602140;
        for(const auto& Name:Cases)
        {
            const FName Id(TEXT("dwarf.")+Name.ToLower());const FString Package=GraphPackageName(Id);
            auto* Saved=LoadObject<UMetaSoundSource>(nullptr,*(Package+TEXT(".")+FPaths::GetCleanFilename(Package)));
            auto* Data=Saved?FindObject<UDreamSFXGraphAsset>(Saved,TEXT("DreamSFXGraph")):nullptr;
            if(!Data||!Data->GetGraph()||!Data->GetGraph()->Program){AddError(TEXT("Missing cold DSL asset ")+Name);return false;}
            TArray<FString> Takes={TEXT("A"),TEXT("B")};
            if(Name==TEXT("gearRoll"))Takes.Append({TEXT("repeat"),TEXT("seedB")});
            for(const FString& Take:Takes)
            {
                auto Graph=Data->GetGraph();auto* Source=Saved;
                if(Take==TEXT("seedB"))
                {
                    TArray<uint8> Bytes;if(!EncodeGraph(*Graph,Bytes)||Bytes.Num()<4)return false;
                    FUTF8ToTCHAR Utf(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+4),Bytes.Num()-4);
                    FString Text(Utf.Length(),Utf.Get());
                    if(Text.ReplaceInline(TEXT("seed 20261120"),TEXT("seed 20261121"),ESearchCase::CaseSensitive)!=1)return false;
                    const auto New=FGraphCompiler::Compile(Text);if(!New.Graph)return false;
                    Graph=New.Graph;FString Error;Source=BuildGraphPreview(Graph,Error);if(!Source)return false;
                }
                TestEqual(TEXT("No saved performance events"),Graph->Timeline.Num(),0);
                const int32 Frames=FMath::RoundToInt(Graph->DurationSeconds*48000.);
                FGraphInstance Core;FString Error;if(!Core.Prepare(Graph,48000,Error)){AddError(Error);return false;}
                TArray<FAudioParameter> Params;
                if(Take==TEXT("B"))
                {
                    const bool Gear=Name.StartsWith(TEXT("gear")),Furnace=Name==TEXT("furnace");
                    const FName Pin=Gear?TEXT("speed"):Furnace?TEXT("stopAt"):TEXT("presses");
                    const float V=Gear?1.5f:Furnace?16.f:2.f;
                    Params.Emplace(Pin,V);if(!Core.SetInput(Pin,V))return false;
                }
                FString Trace=TEXT("kind,bankOrNode,seconds,initialHz,targetHz\n");
                Core.ObserveProgram([&](FName Bank,double Time)
                {Trace+=FString::Printf(TEXT("start,%s,%.15g,,\n"),*Bank.ToString(),Time);},
                    [&](const FPortRef& Port,double Time,double Initial,double Target)
                    {if(Port.Port==TEXT("frequencyHz"))Trace+=FString::Printf(TEXT("pitch,%s,%.15g,%.12g,%.12g\n"),*Port.Node.ToString(),Time,Initial,Target);});
                Source->InitResources();FSoundGeneratorInitParams Init;
                Init.AudioDeviceID=0;Init.AudioComponentId=Init.InstanceID=++Unique;Init.SampleRate=48000;
                Init.NumChannels=2;Init.NumFramesPerCallback=256;Init.AudioMixerNumOutputFrames=256;Init.GraphName=TEXT("Dwarf DSL");
                auto Gen=Source->CreateSoundGenerator(Init,MoveTemp(Params));if(!Gen)return false;
                Gen->OnBeginGenerate();ON_SCOPE_EXIT{Gen->OnEndGenerate();Source->OnEndGenerate(Gen);};
                TArray<float> Audio;Audio.Reserve((Frames+1024)*2);int32 Natural=0;float Peak=0,Delta=0;
                for(int32 At=0;At<Frames+2048&&!Gen->IsFinished();At+=256)
                {
                    float Mix[512]={},Direct[512]={};Gen->GetNextBuffer(Mix,512,true);Natural+=Core.Render(Direct,256);
                    for(int32 I=0;I<512;++I)
                    {
                        if(!FMath::IsFinite(Mix[I])||(At+I/2>=Frames&&Mix[I]!=0))return false;
                        Peak=FMath::Max(Peak,FMath::Abs(Mix[I]));Delta=FMath::Max(Delta,FMath::Abs(Mix[I]-Direct[I]));
                    }
                    Audio.Append(Mix,512);
                }
                AddInfo(FString::Printf(TEXT("DSFX_DWARF case=%s take=%s frames=%d captured=%d nodes=%d timeline=%d peak=%.9g delta=%.9g"),
                    *Name,*Take,Natural,Audio.Num()/2,Graph->Nodes.Num(),Graph->Timeline.Num(),Peak,Delta));
                if(!TestTrue(TEXT("Cold generator and traced Core agree, finish and do not clip"),Core.IsFinished()&&Gen->IsFinished()&&Natural==Frames&&Peak>1.e-5f&&Peak<1&&Delta==0))return false;
                const FString Base=Out/(Name+TEXT("-")+Take);
                if(!FFileHelper::SaveArrayToFile(MakeArrayView(reinterpret_cast<const uint8*>(Audio.GetData()),Frames*8),*(Base+TEXT(".f32")))||
                    !FFileHelper::SaveStringToFile(Trace,*(Base+TEXT(".csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return false;
            }
        }
        return !HasAnyErrors();
    }
}
#endif
