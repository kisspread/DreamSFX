#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXGraphPreview.h"
#include "MetasoundSource.h"
#include "MetasoundOperatorSettings.h"
#include "IAudioParameterTransmitter.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundGenerator.h"
#include <cmath>

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    namespace BlastRecording
    {
        constexpr int32 Rate = 48000;
        constexpr int32 DurationFrames = 19 * Rate;
        enum class ETake { Default, Half, Far };

        // Read-only graph taps keep the same program, seed consumption and connections.
        FGraphPtr Tap(FGraphPtr Source, const FString& Name)
        {
            auto Copy = MakeShared<FGraphIR, ESPMode::ThreadSafe>(*Source);
            TArray<FPortRef> Outputs;
            if (Name == TEXT("roomIn"))
            {
                for (const FConnection& Edge : Source->Connections)
                    if (Edge.To.Node == TEXT("world.room") && Edge.Kind == EPortKind::Audio) Outputs.Add(Edge.From);
            }
            else if (Name == TEXT("ember") || Name == TEXT("glyph") || Name == TEXT("glyphTone") || Name == TEXT("glass"))
            {
                for (const FNode& Node : Source->Nodes)
                {
                    const FString Id = Node.Id.ToString();
                    if ((Name == TEXT("ember") && Id.StartsWith(TEXT("ember.")) && Node.Type.Name == TEXT("source.grain_once")) ||
                        (Name == TEXT("glyph") && Id.StartsWith(TEXT("glyph.")) && Id.EndsWith(TEXT(".sweepPan"))) ||
                        (Name == TEXT("glyphTone") && Id.StartsWith(TEXT("glyph.")) && Id.EndsWith(TEXT(".tone"))) ||
                        (Name == TEXT("glass") && Id.StartsWith(TEXT("glass.")) && Id.EndsWith(TEXT(".out"))))
                        Outputs.Add({Node.Id, TEXT("audio")});
                }
            }
            else
            {
                static const TMap<FString, FName> Ports = {
                    {TEXT("gust"), TEXT("gust._0")}, {TEXT("crack"), TEXT("firstCrack._0")},
                    {TEXT("roomOut"), TEXT("world.room")}, {TEXT("worldMix"), TEXT("world.mix")},
                    {TEXT("chargeBus"), TEXT("gatherBus.bus")}, {TEXT("hush"), TEXT("world.hush")},
                    {TEXT("ears"), TEXT("world.ears")}, {TEXT("pre"), TEXT("world.pre")},
                    {TEXT("press"), TEXT("world._0")}, {TEXT("soft"), TEXT("world._1")}
                    ,{TEXT("rune0"),TEXT("glyph.0.t4")},{TEXT("rune1"),TEXT("glyph.1.t4")},
                    {TEXT("rune2"),TEXT("glyph.2.t4")},{TEXT("rune3"),TEXT("glyph.3.t4")},
                    {TEXT("falling"),TEXT("fallingSpark.zip")}
                };
                const FName* Port = Ports.Find(Name);
                if (!Port || !Copy->ExecutionOrder.Contains(*Port)) return {};
                Outputs.Add({*Port, TEXT("audio")});
            }
            if (Outputs.IsEmpty()) return {};
            if (Outputs.Num() == 1)
            {
                Copy->Output = Outputs[0];
                return Copy;
            }
            FNode Sum;
            Sum.Id = TEXT("measure.sum"); Sum.Type = {TEXT("mix.gain"),1};
            Sum.Parameters.Add(TEXT("level"), FValue(TInPlaceType<double>(),1.));
            Copy->Nodes.Add(Sum); Copy->ExecutionOrder.Add(Sum.Id);
            for (int32 I = 0; I < Outputs.Num(); ++I)
                Copy->Connections.Add({Outputs[I], {Sum.Id,TEXT("input")}, EPortKind::Audio, I});
            Copy->Output = {Sum.Id,TEXT("audio")};
            return Copy;
        }

        bool Record(FAutomationTestBase& Test, UMetaSoundSource& Source, FGraphPtr Graph,
            const FString& Folder, const FString& Name, ETake Take, uint64 Id, int32 Frames, bool bCheckCore)
        {
            Source.InitResources();
            const int32 Quantum = Source.GetOperatorSettings(Rate).GetNumFramesPerBlock();
            const int32 Callback = Take == ETake::Half ? Rate/100 : 256;
            if (Take == ETake::Half && !Test.TestTrue(TEXT("Game controls arrive on consistent MetaSound blocks"),
                Quantum > 0 && Callback % Quantum == 0)) return false;
            FSoundGeneratorInitParams Init;
            Init.AudioDeviceID=0; Init.InstanceID=Init.AudioComponentId=Id;
            Init.SampleRate=Rate; Init.NumChannels=2; Init.NumFramesPerCallback=Callback; Init.AudioMixerNumOutputFrames=Callback;
            Init.GraphName=TEXT("Blast DSL capture");
            Audio::FParameterTransmitterInitParams TxInit;
            TxInit.AudioDeviceID=0; TxInit.InstanceID=Id; TxInit.SampleRate=Rate;
            auto Tx = Source.CreateParameterTransmitter(MoveTemp(TxInit));
            TArray<FAudioParameter> Defaults;
            if (Take == ETake::Far) Defaults.Emplace(TEXT("distance"),60.f);
            auto Generator = Source.CreateSoundGenerator(Init,MoveTemp(Defaults));
            if (!Generator || !Tx) return false;
            Generator->OnBeginGenerate();
            ON_SCOPE_EXIT { Generator->OnEndGenerate(); Source.OnEndGenerate(Generator); };
            FGraphInstance Core;
            FString Error;
            if (bCheckCore && !Core.Prepare(Graph,Rate,Error)) {Test.AddError(Error);return false;}
            if (bCheckCore && Take==ETake::Far) Core.SetInput(TEXT("distance"),60.);
            auto Number = [&](FName Input,float Value)
            {
                if (bCheckCore && !Core.SetInput(Input,Value)) return false;
                TArray<FAudioParameter> Update;Update.Emplace(Input,Value);return Tx->SetParameters(MoveTemp(Update));
            };
            auto State = [&](const TCHAR* Value)
            {
                if (bCheckCore && !Core.SetState(FName(Value))) return false;
                TArray<FAudioParameter> Update;Update.Emplace(TEXT("State"),FString(Value));return Tx->SetParameters(MoveTemp(Update));
            };
            const bool bFull = Frames==DurationFrames;
            TArray<float> PCM, Buffer, Direct;
            Buffer.SetNumUninitialized(Callback*2);Direct.SetNumUninitialized(Callback*2);
            PCM.Reserve((Frames+2048)*2);
            double Peak=0.,Delta=0.;
            for(int32 At=0;At<Frames+(bFull?2048:0) && !Generator->IsFinished();At+=Callback)
            {
                if(Take==ETake::Half)
                {
                    const int32 Tick=At/Callback;
                    if(Tick==220 && !Number(TEXT("charge"),.5f))return false;
                    if(Tick==224 && (!Number(TEXT("pan"),float(96./(std::sqrt(292.)*20.)))||!State(TEXT("fall"))))return false;
                    if(Tick==260&&!State(TEXT("silence")))return false;
                    if(Tick==352&&!State(TEXT("impact")))return false;
                    if(Tick>=352 && Tick<=360 && !Number(TEXT("shockRadius"),float(FMath::Max(0.,(Tick/100.-3.52)*340.))))return false;
                    if(Tick==384 && !Number(TEXT("broken"),90.f))return false;
                    if(Tick==1760 && !State(TEXT("")))return false;
                }
                Generator->GetNextBuffer(Buffer.GetData(),Buffer.Num(),true);
                if(bCheckCore)Core.Render(Direct.GetData(),Callback);
                for(int32 I=0;I<Buffer.Num();++I)
                {
                    const float Sample=Buffer[I];
                    if(!FMath::IsFinite(Sample) || (At+I/2>=DurationFrames && Sample!=0.f))
                    {Test.AddError(Name+TEXT(": invalid sample or nonzero transport padding"));return false;}
                    Peak=FMath::Max(Peak,double(FMath::Abs(Sample)));
                    if(bCheckCore)Delta=FMath::Max(Delta,double(FMath::Abs(Sample-Direct[I])));
                }
                PCM.Append(Buffer);
            }
            Test.AddInfo(FString::Printf(TEXT("DSFX_BLAST take=%s valid=%d returned=%d completion=%d operatorBlock=%d callback=%d peak=%.10g coreCompared=%d coreDelta=%.10g pulses=%u runes=%u embers=%u stones=%u"),
                *Name,Frames,PCM.Num()/2,Generator->IsFinished(),Quantum,Callback,Peak,bCheckCore,Delta,
                Core.GetVoiceStarts(TEXT("pulse")),Core.GetVoiceStarts(TEXT("glyph")),Core.GetVoiceStarts(TEXT("ember")),Core.GetVoiceStarts(TEXT("stone"))));
            if(!Test.TestTrue(Name+TEXT(" spans the full request"),PCM.Num()>=2*Frames&&(!bFull||Generator->IsFinished())))return false;
            if(bCheckCore)
            {
                Test.TestTrue(Name+TEXT(" finite nonclipping mix"),Peak>0.01&&Peak<1);
                Test.TestEqual(Name+TEXT(" reaches the same output through public pins and Core"),Delta,0.);
                Test.TestEqual(Name+TEXT(" has all 70 embers"),Core.GetVoiceStarts(TEXT("ember")),70u);
                Test.TestEqual(Name+TEXT(" has all 29 stones"),Core.GetVoiceStarts(TEXT("stone")),29u);
                Test.TestEqual(Name+TEXT(" has the expected charge-dependent rune count"),Core.GetVoiceStarts(TEXT("glyph")),Take==ETake::Half?4u:6u);
            }
            const FString File=Folder/(Name+TEXT(".f32"));
            return !FPaths::FileExists(File) && FFileHelper::SaveArrayToFile(
                MakeArrayView(reinterpret_cast<const uint8*>(PCM.GetData()),Frames*2*sizeof(float)),*File);
        }
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDslBlastRecording,"DreamSFX.DSL.BlastAudio",
        EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

    bool FDslBlastRecording::RunTest(const FString&)
    {
        using namespace BlastRecording;
        FString Out;
        if(!FParse::Value(FCommandLine::Get(),TEXT("DreamSFXBlastAudio="),Out))return true;
        auto* Source=LoadObject<UMetaSoundSource>(nullptr,TEXT("/Game/DreamSFX/Graphs/MS_small_umagic_dblastfull.MS_small_umagic_dblastfull"));
        auto* Data=Source?FindObject<UDreamSFXGraphAsset>(Source,TEXT("DreamSFXGraph")):nullptr;
        if(!TestTrue(TEXT("Cold Source contains an executable DSL"),Data&&Data->GetGraph()&&Data->GetGraph()->Program&&Data->GetGraph()->Timeline.IsEmpty()))return false;
        auto* Async=IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if(!Async)return false;
        const int32 Old=Async->GetInt();Async->Set(0,ECVF_SetByCode);ON_SCOPE_EXIT{Async->Set(Old,ECVF_SetByCode);};
        auto Graph=Data->GetGraph();
        uint64 Id=913000;
        const bool Details=FParse::Param(FCommandLine::Get(),TEXT("DreamSFXBlastDetails"));
        if(!Details)for(ETake Take:{ETake::Default,ETake::Half,ETake::Far})
        {
            const FString Name=Take==ETake::Default?TEXT("default"):Take==ETake::Half?TEXT("half"):TEXT("far");
            if(!Record(*this,*Source,Graph,Out,Name,Take,++Id,DurationFrames,true))return false;
        }
        const TArray<FString> Names= Details ?
            TArray<FString>{TEXT("glyphTone"),TEXT("rune0"),TEXT("rune1"),TEXT("rune2"),TEXT("rune3"),TEXT("half-glyphTone"),
                TEXT("half-falling"),TEXT("half-glass"),TEXT("half-roomIn"),TEXT("half-roomOut"),TEXT("half-worldMix")} :
            TArray<FString>{TEXT("gust"),TEXT("crack"),TEXT("ember"),TEXT("roomIn"),TEXT("roomOut"),TEXT("worldMix"),
                TEXT("chargeBus"),TEXT("glyph"),TEXT("hush"),TEXT("ears"),TEXT("pre"),TEXT("press"),TEXT("soft")};
        for(const FString& Name:Names)
        {
            const bool Half=Name.StartsWith(TEXT("half-"));
            FGraphPtr Stem=Tap(Graph,Half?Name.RightChop(5):Name);
            FString Error;
            auto* Probe=Stem?BuildGraphPreview(Stem,Error):nullptr;
            if(!Probe){AddError(TEXT("Cannot build diagnostic tap ")+Name+TEXT(" ")+Error);return false;}
            const int32 Frames=Details?5*Rate:Name==TEXT("ember")?DurationFrames:8*Rate;
            if(!Record(*this,*Probe,Stem,Out,Name,Half?ETake::Half:ETake::Default,++Id,Frames,false))return false;
        }
        return !HasAnyErrors();
    }
}
#endif
