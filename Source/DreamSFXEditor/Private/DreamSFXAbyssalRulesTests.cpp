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
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAbyssalProgramCapture, "DreamSFX.DSL.AbyssalGarden",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FAbyssalProgramCapture::RunTest(const FString&)
    {
        FString Out, List;
        if (!FParse::Value(FCommandLine::Get(), TEXT("DreamSFXAbyssOut="), Out)) return true;
        if (!FParse::Value(FCommandLine::Get(), TEXT("DreamSFXAbyssNames="), List)) return false;
        struct FSetting { FName Input; float Value; };
        const TMap<FString,FSetting> Settings = {
            {TEXT("rumble"),{TEXT("cutoffHz"),95}}, {TEXT("wash"),{TEXT("depthM"),-22}}, {TEXT("rush"),{TEXT("speed"),9}},
            {TEXT("drone"),{TEXT("detuneScale"),2}}, {TEXT("fullDive"),{TEXT("speedScale"),.5f}},
            {TEXT("riseDive"),{TEXT("travelM"),14.75f}}, {TEXT("sprint"),{TEXT("maxSpeed"),13}}};
        auto* Async=IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if (!Async) return false;
        const int32 Prior=Async->GetInt();Async->Set(0,ECVF_SetByCode);ON_SCOPE_EXIT{Async->Set(Prior,ECVF_SetByCode);};
        TArray<FString> Names;List.ParseIntoArray(Names,TEXT("+"));
        uint64 Unique=826500;
        for (const FString& Name : Names)
        {
            const FString Package=GraphPackageName(FName(TEXT("abyssal.")+Name.ToLower()));
            auto* Source=LoadObject<UMetaSoundSource>(nullptr,*(Package+TEXT(".")+FPaths::GetCleanFilename(Package)));
            auto* Data=Source?FindObject<UDreamSFXGraphAsset>(Source,TEXT("DreamSFXGraph")):nullptr;
            if (!TestTrue(Name+TEXT(" cold-loads an executable DSL"), Data && Data->GetGraph() && Data->GetGraph()->Program)) return false;
            const FGraphPtr Graph=Data->GetGraph();
            if (!TestEqual(TEXT("Zero baked performance events"),Graph->Timeline.Num(),0)) return false;
            for (int32 Variant=0; Variant<2; ++Variant)
            {
                FGraphInstance Witness;FString Error;
                if (!Witness.Prepare(Graph,48000,Error)){AddError(Error);return false;}
                TArray<FAudioParameter> Params;
                if (Variant)
                {
                    const FSetting* Found=Settings.Find(Name);
                    const FSetting Setting=Found?*Found:FSetting{TEXT("pitchScale"),.8f};
                    if (!Witness.SetInput(Setting.Input,Setting.Value)){AddError(Name+TEXT(" missing performance input"));return false;}
                    Params.Emplace(Setting.Input,Setting.Value);
                }
                FString Trace=TEXT("voice,seconds,fromHz,toHz\n");
                Witness.ObserveProgram({},[&](const FPortRef& Port,double At,double From,double To)
                {
                    if (Port.Port==TEXT("frequencyHz"))
                        Trace+=FString::Printf(TEXT("%s,%.15g,%.12g,%.12g\n"),*Port.Node.ToString(),At,From,To);
                });
                Source->InitResources();
                FSoundGeneratorInitParams Init;Init.AudioDeviceID=0;Init.InstanceID=Init.AudioComponentId=++Unique;
                Init.SampleRate=48000;Init.NumChannels=2;Init.NumFramesPerCallback=Init.AudioMixerNumOutputFrames=256;
                Init.GraphName=TEXT("Abyssal DSL");
                auto Generator=Source->CreateSoundGenerator(Init,MoveTemp(Params));
                if (!Generator) return false;
                Generator->OnBeginGenerate();ON_SCOPE_EXIT{Generator->OnEndGenerate();Source->OnEndGenerate(Generator);};
                const int32 Frames=FMath::RoundToInt(Graph->DurationSeconds*48000.);
                float Peak=0;int32 Natural=0;TArray<float> Audio;Audio.Reserve(2*Frames+2048);
                for (int32 At=0;At<Frames+2048&&!Generator->IsFinished();At+=256)
                {
                    float A[512]={},B[512]={};Generator->GetNextBuffer(A,512,true);Natural+=Witness.Render(B,256);
                    for (int32 I=0; I<512; ++I)
                    {
                        if (!FMath::IsFinite(A[I])||A[I]!=B[I]||(At+I/2>=Frames&&A[I]!=0)) {AddError(Name+TEXT(" PCM or padding mismatch"));return false;}
                        Peak=FMath::Max(Peak,FMath::Abs(A[I]));
                    }
                    Audio.Append(A,512);
                }
                if (!TestTrue(Name+TEXT(" finishes in bounds"),Natural==Frames&&Witness.IsFinished()&&Generator->IsFinished()&&Peak>1.e-5f&&Peak<1.f)) return false;
                const FString Take=Variant?TEXT("B"):TEXT("A"), Base=Out/(Name+TEXT("-")+Take);
                if (!FFileHelper::SaveArrayToFile(MakeArrayView(reinterpret_cast<const uint8*>(Audio.GetData()),Frames*8),*(Base+TEXT(".f32"))) ||
                    !FFileHelper::SaveStringToFile(Trace,*(Base+TEXT(".csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
                AddInfo(FString::Printf(TEXT("DSFX_ABYSS case=%s take=%s frames=%d nodes=%d timeline=0 peak=%.8g"),*Name,*Take,Frames,Graph->Nodes.Num(),Peak));
            }
        }
        return !HasAnyErrors()&&!Names.IsEmpty();
    }
}
#endif
