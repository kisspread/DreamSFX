#include "DreamSFXGraphAsset.h"
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
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMusicPerformanceCapture, "DreamSFX.DSL.MusicPerformance",
        EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

    bool FMusicPerformanceCapture::RunTest(const FString&)
    {
        FString Folder;
        if(!FParse::Value(FCommandLine::Get(),TEXT("DreamSFXScoreOut="),Folder))return true;
        if(!IFileManager::Get().MakeDirectory(*Folder,true))return false;
        auto* Async=IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if(!Async)return false;
        const int32 Old=Async->GetInt();Async->Set(0,ECVF_SetByCode);ON_SCOPE_EXIT{Async->Set(Old,ECVF_SetByCode);};
        struct FCase{const TCHAR* Id;const TCHAR* Name;const TCHAR* Input;float Value;};
        TArray<FCase> Cases={
            {TEXT("benchmark.musicbox_single"),TEXT("musicbox-single"),TEXT("touch"),.95f},
            {TEXT("benchmark.piano_sustain"),TEXT("piano-sustain"),TEXT("damperPedal"),0.f},
            {TEXT("benchmark.musicbox_small_clock"),TEXT("musicbox-small-clock"),TEXT("transposeBy"),7.f},
            {TEXT("benchmark.piano_small_clock"),TEXT("piano-small-clock"),TEXT("touch"),.92f},
            {TEXT("music.lantern_duet"),TEXT("lantern-duet"),TEXT("touch"),.36f},
            {TEXT("music.shattered_box"),TEXT("shattered-box"),TEXT("breakAt"),.5f}
        };
        if(FParse::Param(FCommandLine::Get(),TEXT("DreamSFXCoverageRest")))
            Cases={
                {TEXT("benchmark.retro_square"),TEXT("retro-square"),TEXT("transposeBy"),7.f},
                {TEXT("benchmark.retro_crickets"),TEXT("retro-crickets"),TEXT("noiseMix"),1.f},
                {TEXT("benchmark.oneshot_coin"),TEXT("oneshot-coin"),TEXT("brightnessHz"),1500.f},
                {TEXT("benchmark.oneshot_explosion"),TEXT("oneshot-explosion"),TEXT("brightnessHz"),420.f},
                {TEXT("benchmark.sine_confirm_soft"),TEXT("sine-confirm-soft"),TEXT("transposeBy"),5.f},
                {TEXT("benchmark.noise_steady"),TEXT("noise-steady"),TEXT("cutoffHz"),650.f},
                {TEXT("benchmark.noise_wind_gust"),TEXT("noise-wind-gust"),TEXT("strength"),.35f},
                {TEXT("architecture.stone_gate"),TEXT("stoneGate"),TEXT("friction"),.45f}};
        uint64 Id=735100;
        for(const auto& Case:Cases)
        {
            const FString Package=GraphPackageName(FName(Case.Id));
            auto* Saved=LoadObject<UMetaSoundSource>(nullptr,*(Package+TEXT(".")+FPaths::GetCleanFilename(Package)));
            auto* Asset=Saved?FindObject<UDreamSFXGraphAsset>(Saved,TEXT("DreamSFXGraph")):nullptr;
            if(!Asset||!Asset->GetGraph()||!Asset->GetGraph()->Program){AddError(TEXT("Missing saved program: ")+FString(Case.Id));return false;}
            auto Graph=Asset->GetGraph();
            TArray<FString> Takes={TEXT("A"),TEXT("B")};
            if(FString(Case.Name)==TEXT("lantern-duet"))Takes.Append({TEXT("boxbus.side"),TEXT("pianobus.side"),TEXT("boxbus._0"),TEXT("pianobus._0"),TEXT("melody.tine"),TEXT("answer.keys")});
            if(FString(Case.Name)==TEXT("shattered-box"))Takes.Append({TEXT("casebus.in"),TEXT("chips")});
            for(const FString& Take:Takes)
            {
                FGraphPtr RenderGraph=Graph;auto* Source=Saved;
                if(Take!=TEXT("A")&&Take!=TEXT("B"))
                {
                    auto Copy=MakeShared<FGraphIR,ESPMode::ThreadSafe>(*Graph);
                    if(Take==TEXT("chips"))
                    {
                        auto& Sum=Copy->Nodes.AddDefaulted_GetRef();Sum.Id=TEXT("measure.grains");Sum.Type={TEXT("mix.gain"),1};
                        Sum.Parameters.Add(TEXT("level"),FValue(TInPlaceType<double>(),1.));
                        Copy->ExecutionOrder.Add(Sum.Id);
                        int32 Order=0;
                        for(const auto& Edge:Graph->Connections)
                            if(Edge.To.Node==TEXT("master.out")&&Edge.From.Node.ToString().StartsWith(TEXT("shard.")))
                                Copy->Connections.Add({Edge.From,{Sum.Id,TEXT("input")},EPortKind::Audio,Order++});
                        if(!Order)return false;
                        Copy->Output={Sum.Id,TEXT("audio")};
                    }
                    else
                    {
                        FName Tap(Take);if(!Copy->ExecutionOrder.Contains(Tap)){AddError(TEXT("Unknown tap: ")+Take);return false;}
                        Copy->Output={Tap,TEXT("audio")};
                    }
                    RenderGraph=Copy;FString Error;Source=BuildGraphPreview(RenderGraph,Error);
                    if(!Source){AddError(Error);return false;}
                }
                FGraphInstance Direct;FString Error;
                if(!Direct.Prepare(RenderGraph,48000,Error)){AddError(Error);return false;}
                const int32 MaxFrames=Direct.GetLengthFrames();
                TArray<FAudioParameter> Params;
                if(Take==TEXT("B"))
                {
                    Params.Emplace(FName(Case.Input),Case.Value);if(!Direct.SetInput(Case.Input,Case.Value))return false;
                    if(FString(Case.Name)==TEXT("shattered-box")){Params.Emplace(TEXT("force"),.95f);Direct.SetInput(TEXT("force"),.95f);}
                }
                Source->InitResources();
                FSoundGeneratorInitParams Init;Init.InstanceID=Init.AudioComponentId=++Id;Init.AudioDeviceID=0;Init.SampleRate=48000;
                Init.NumChannels=2;Init.NumFramesPerCallback=256;Init.AudioMixerNumOutputFrames=256;Init.GraphName=TEXT("DSL music recording");
                auto Generator=Source->CreateSoundGenerator(Init,MoveTemp(Params));
                if(!Generator)return false;
                Generator->OnBeginGenerate();ON_SCOPE_EXIT{Generator->OnEndGenerate();Source->OnEndGenerate(Generator);};
                TArray<float> PCM;PCM.Reserve((MaxFrames+1024)*2);int32 Natural=0;float Delta=0,Peak=0;
                for(int32 Frame=0;Frame<MaxFrames+2048&&!Generator->IsFinished();Frame+=256)
                {
                    float A[512]={},B[512]={};Generator->GetNextBuffer(A,512,true);
                    const int32 Count=Direct.Render(B,256);Natural+=Count;
                    for(int32 I=0;I<512;++I){if(!FMath::IsFinite(A[I]))return false;Delta=FMath::Max(Delta,FMath::Abs(A[I]-B[I]));Peak=FMath::Max(Peak,FMath::Abs(A[I]));}
                    PCM.Append(A,512);
                }
                AddInfo(FString::Printf(TEXT("DSFX_MUSIC case=%s take=%s natural=%d captured=%d maxDelta=%.10g peak=%.10g timeline=%d shards=%u"),
                    Case.Name,*Take,Natural,PCM.Num()/2,Delta,Peak,Graph->Timeline.Num(),Direct.GetVoiceStarts(TEXT("shard"))));
                if(!TestTrue(TEXT("Cold generator completed, agrees with Core, audible and finite"),
                    Generator->IsFinished()&&Direct.IsFinished()&&Natural>0&&Natural<=PCM.Num()/2&&Peak>1.e-5f&&Peak<1.f&&Delta==0.f))return false;
                const auto Data=MakeArrayView(reinterpret_cast<const uint8*>(PCM.GetData()),Natural*8);
                if(!FFileHelper::SaveArrayToFile(Data,*(Folder/(FString(Case.Name)+TEXT("-")+Take+TEXT(".f32")))))return false;
            }
        }
        return !HasAnyErrors();
    }
}
#endif
