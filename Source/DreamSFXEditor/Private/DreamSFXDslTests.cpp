#include "DreamSFXGraphCompiler.h"
#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphInstance.h"
#include "MetasoundSource.h"
#include "IAudioParameterTransmitter.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDslDiagnostics,"DreamSFX.DSL.Diagnostics",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
    bool FDslDiagnostics::RunTest(const FString&)
    {
        const FString Example=TEXT("sound test.tone\npreview 1 s\nclock 100 Hz\nseed 42\ninput pitch range 55 990 default 220\nvoice tone {\n  out=sine(hz=pitch)\n}\nstart { play tone }\noutput tone.out\n");
        TestTrue(TEXT("A named oscillator reaches the common graph"),FGraphCompiler::Compile(Example).Graph.IsValid());
        for(const FString& Bad:{Example.Replace(TEXT("hz=pitch"),TEXT("hz=unknown")),Example.Replace(TEXT("hz=pitch"),TEXT("p3=pitch")),
            Example.Replace(TEXT("pitch)"),TEXT("pitch]")),Example.Replace(TEXT("play tone"),TEXT("play nowhere")),Example.Replace(TEXT("55 990"),TEXT("990 55")),
            Example.Replace(TEXT("out=sine(hz=pitch)"),TEXT("out=gain() -> next=gain() -> out")),FString(TEXT("{\"schema\":\"dreamsfx/2\"}"))})
        {
            auto Result=FGraphCompiler::Compile(Bad);
            if(!TestTrue(TEXT("Invalid program rejected with a source coordinate"),!Result.Graph&&Result.Errors.Num()&&Result.Errors[0].Pointer.Contains(TEXT(":"))))return false;
            AddInfo(Result.Errors[0].Pointer+TEXT(" ")+Result.Errors[0].Message);
        }
        return !HasAnyErrors();
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDslTiers,"DreamSFX.DSL.ThreeTiers",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
    bool FDslTiers::RunTest(const FString&)
    {
        const FString Head=TEXT("sound test.sound\npreview 2 s\ntail 1 s\nseed 42\n");
        const TArray<FString> Texts={
            Head+TEXT("voice coin { out=source.sfxr(wave=square,kernelRateHz=44100,sampleBits=16,seed=1337,gainDb=-14,voiceLimit=1,baseFrequency=.6,sustain=.06,decay=.12) }\nfire coin.out at 0 velocity 1\noutput coin.out"),
            Head+TEXT("voice keys { out=voice.music_box(gainDb=-12,voiceLimit=8) }\nscore keys.out at 0 bpm 120 : C5/8 E5/8 [G5 B5]/4\noutput keys.out"),
            Head+TEXT("voice keys { out=voice.modal_piano(gainDb=-12,voiceLimit=8) }\nscore keys.out at 0 bpm 120 : C5/8 F#5/8 [G5 B5]/4\noutput keys.out")};
        for(const auto& Text:Texts)
        {
            const auto Built=FGraphCompiler::Compile(Text);
            if(!Built.Graph){for(const auto& E:Built.Errors)AddError(E.Pointer+TEXT(" ")+E.Message);return false;}
            FString Error;FGraphInstance Instance;if(!Instance.Prepare(Built.Graph,48000,Error)){AddError(Error);return false;}
            float Peak=0.;float Block[512];
            while(!Instance.IsFinished())
            {
                const int32 N=Instance.Render(Block,256);
                if(!N&&!Instance.IsFinished())return false;
                for(int32 I=0;I<2*N;++I)Peak=FMath::Max(Peak,FMath::Abs(Block[I]));
            }
            TestTrue(TEXT("Existing voice DSP produces audio"),Peak>.0001f&&Peak<1.f);
        }
        return !HasAnyErrors();
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDslHoleCapture,"DreamSFX.DSL.BlackHole",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
    bool FDslHoleCapture::RunTest(const FString&)
    {
        FString Out;
        if(!FParse::Value(FCommandLine::Get(),TEXT("DreamSFXProgramCapture="),Out))return true;
        auto* Source=LoadObject<UMetaSoundSource>(nullptr,TEXT("/Game/DreamSFX/Graphs/MS_small_umagic_dholefull.MS_small_umagic_dholefull"));
        auto* Data=Source?FindObject<UDreamSFXGraphAsset>(Source,TEXT("DreamSFXGraph")):nullptr;
        if(!TestTrue(TEXT("Cold package owns its sound program"),Data&&Data->GetGraph()&&Data->GetGraph()->Program))return false;
        auto Graph=Data->GetGraph();
        TestEqual(TEXT("No baked event tape"),Graph->Timeline.Num(),0);
        TestEqual(TEXT("Three exposed control inputs"),Graph->Inputs.Num(),3);
        if(!IFileManager::Get().MakeDirectory(*Out,true))return false;
        auto* Async=IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if(!Async)return false;
        int32 Was=Async->GetInt();Async->Set(0,ECVF_SetByCode);ON_SCOPE_EXIT{Async->Set(Was,ECVF_SetByCode);};
        Source->InitResources();
        uint64 Id=70211;
        struct FCase {const TCHAR* Name;float Grip;};
        for(const FCase Case: {FCase{TEXT("original"),-1},FCase{TEXT("weak"),.3f},FCase{TEXT("medium"),.8f},FCase{TEXT("strong"),1.2f},FCase{TEXT("switch"),.3f},FCase{TEXT("live"),.8f}})
        {
            const bool Switching=FCString::Strcmp(Case.Name,TEXT("switch"))==0;
            const bool Live=FCString::Strcmp(Case.Name,TEXT("live"))==0;
            FSoundGeneratorInitParams Init;Init.InstanceID=Init.AudioComponentId=++Id;Init.AudioDeviceID=0;Init.SampleRate=48000;Init.NumChannels=2;
            Init.NumFramesPerCallback=256;Init.AudioMixerNumOutputFrames=256;Init.GraphName=TEXT("Program input transport");
            Audio::FParameterTransmitterInitParams TxInit;TxInit.InstanceID=Id;TxInit.AudioDeviceID=0;TxInit.SampleRate=48000;
            auto Tx=Source->CreateParameterTransmitter(MoveTemp(TxInit));
            TArray<FAudioParameter> Params;
            if(Case.Grip>=0)Params.Emplace(TEXT("grip"),Case.Grip);
            if(Live)Params.Emplace(TEXT("State"),FString(TEXT("open")));
            auto Generator=Source->CreateSoundGenerator(Init,MoveTemp(Params));
            if(!Tx||!Generator)return false;
            Generator->OnBeginGenerate();ON_SCOPE_EXIT{Generator->OnEndGenerate();Source->OnEndGenerate(Generator);};
            FGraphInstance Core;FString E;if(!Core.Prepare(Graph,48000,E))return false;
            if(Case.Grip>=0)Core.SetInput(TEXT("grip"),Case.Grip);
            if(Live)Core.SetState(TEXT("open"));
            const int32 Frames=(Live?27:18)*48000;
            TArray<float> Audio;Audio.Reserve((Frames+1024)*2);double Peak=0,Delta=0;
            auto Update=[&](FAudioParameter P)
            {
                TArray<FAudioParameter> Message;Message.Add(MoveTemp(P));return Tx->SetParameters(MoveTemp(Message));
            };
            for(int32 At=0;At<Frames+1024&&!Generator->IsFinished();At+=256)
            {
                if(Switching&&At==153600)
                {if(!Update(FAudioParameter(TEXT("grip"),1.2f)))return false;Core.SetInput(TEXT("grip"),1.2f);}
                if(Live&&At==960000)
                {
                    if(!Update(FAudioParameter(TEXT("closing"),1.f))||!Update(FAudioParameter(TEXT("held"),36.f))||
                        !Update(FAudioParameter(TEXT("State"),FString(TEXT("shut")))))return false;
                    Core.SetInput(TEXT("closing"),1);Core.SetInput(TEXT("held"),36);Core.SetState(TEXT("shut"));
                }
                if(Live&&At==1008128)
                {
                    if(!Update(FAudioParameter(TEXT("State"),FString(TEXT("burst")))))return false;
                    Core.SetState(TEXT("burst"));
                }
                float A[512]={},B[512]={};Generator->GetNextBuffer(A,512,true);Core.Render(B,256);
                for(int32 I=0;I<512;++I)
                {
                    if(!FMath::IsFinite(A[I]))return false;
                    Peak=FMath::Max(Peak,double(FMath::Abs(A[I])));Delta=FMath::Max(Delta,double(FMath::Abs(A[I]-B[I])));
                }
                Audio.Append(A,512);if(Live&&At+256>=Frames)break;
            }
            AddInfo(FString::Printf(TEXT("DSFX_DSL case=%s frames=%d rendered=%d natural=%d peak=%.9g coreDelta=%.9g beats=%u shards=%u nodes=%d edges=%d timeline=%d"),
                Case.Name,Frames,Audio.Num()/2,Generator->IsFinished(),Peak,Delta,Core.GetVoiceStarts(TEXT("heart")),Core.GetVoiceStarts(TEXT("shard")),Graph->Nodes.Num(),Graph->Connections.Num(),Graph->Timeline.Num()));
            if(!TestTrue(TEXT("Source produces finite stereo audio for the requested window"),Peak>0.01&&Peak<1&&Audio.Num()>=2*Frames))return false;
            TestEqual(TEXT("Public named input transport agrees with Core"),Delta,0.);
            TestEqual(TEXT("Audition completes; explicit live state can be held beyond the audition"),Generator->IsFinished(),!Live);
            if(Live)TestEqual(TEXT("Live 36 held pieces give five debris voices"),Core.GetVoiceStarts(TEXT("shard")),5u);
            if(!FFileHelper::SaveArrayToFile(MakeArrayView(reinterpret_cast<const uint8*>(Audio.GetData()),Frames*2*sizeof(float)),*(Out/(FString(Case.Name)+TEXT(".f32")))))return false;
        }
        return !HasAnyErrors();
    }
}
#endif
