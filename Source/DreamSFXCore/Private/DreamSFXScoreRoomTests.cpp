#include "DreamSFXGraphCompiler.h"
#include "DreamSFXProgramRunner.h"
#include "HAL/LowLevelMemTracker.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScoreRooms,"DreamSFX.DSL.ScoreRooms",
        EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
    bool FScoreRooms::RunTest(const FString&)
    {
        FString Out;
        if(!FParse::Value(FCommandLine::Get(),TEXT("DreamSFXScoreOut="),Out))return true;
#if ENABLE_LOW_LEVEL_MEM_TRACKER
        if(!FLowLevelMemTracker::IsEnabled()){AddError(TEXT("Room memory capture needs -LLM"));return false;}
        auto Bytes=[](FName Tag)
        {
            auto& LLM=FLowLevelMemTracker::Get();LLM.UpdateStatsPerFrame();
            return LLM.GetTagAmountForTracker(ELLMTracker::Default,Tag,ELLMTagSet::None);
        };
        const FName RawTag(TEXT("DSFXRawIRProbe")),KernelTag(TEXT("DSFXRoomHeapProbe"));
        const auto* Spec=FindNodeType({TEXT("room.convolution"),1});
        TArray<DSFXGraphDSP::FGeneratedBuffer> IR;
        TMap<FName,int32> BufferIds,Slots;TArray<FControlState> Controls;
        TArray<DSFXGraphDSP::FState> States;States.SetNum(2);
        FKernel Kernels[2];
        // Warm up factories and process-wide FFT tables, outside either measurement tag.
        FNode N;N.Parameters.Add(TEXT("impulse"),FValue(TInPlaceType<FName>(),TEXT("warm")));
        IR.Add(DSFXGraphDSP::Generate(DSFXGraphDSP::ERecipe::Room,1,4096));BufferIds.Add(TEXT("warm"),0);
        {DSFXGraphDSP::FState S;auto K=Spec->Bind({N,Slots,Controls,BufferIds,IR,S,48000,true});if(!K.Process)return false;}
        IR.Reset();BufferIds.Reset();IR.Reserve(2);
        const int64 Raw0=Bytes(RawTag),Kernel0=Bytes(KernelTag);
        int64 PrevRaw=Raw0,PrevKernel=Kernel0;
        for(int32 I=0;I<2;++I)
        {
            const int32 Frames=I?153600:57600;const FName Id=I?TEXT("hall"):TEXT("wood");
            {LLM_SCOPE_BYNAME(TEXT("DSFXRawIRProbe")); IR.Add(DSFXGraphDSP::Generate(DSFXGraphDSP::ERecipe::Room,42+I,Frames));}
            BufferIds.Add(Id,I);N.Parameters[TEXT("impulse")].Set<FName>(Id);
            {LLM_SCOPE_BYNAME(TEXT("DSFXRoomHeapProbe"));
                Kernels[I]=Spec->Bind({N,Slots,Controls,BufferIds,IR,States[I],48000,true});
                if(!Kernels[I].Process)return false;
                float Input[128]={1.f},Unused[128]={},L[128],R[128];
                const float* Inputs[]={Input,Unused};float* Outputs[]={L,R};
                States[I].Room->ProcessAudioBlock(Inputs,Outputs);
            }
            const int64 Raw=Bytes(RawTag),Kernel=Bytes(KernelTag);
            AddInfo(FString::Printf(TEXT("DSFX_ROOM_RETAINED name=%s samples=%d partitions=%d raw=%lld backend=%lld total=%lld cumulative=%lld"),
                *Id.ToString(),Frames,Frames/128,Raw-PrevRaw,Kernel-PrevKernel,(Raw-PrevRaw)+(Kernel-PrevKernel),(Raw-Raw0)+(Kernel-Kernel0)));
            TestTrue(TEXT("Live allocation sample includes stereo IR and transformed partitions"),Raw-PrevRaw>=int64(Frames)*8&&Kernel-PrevKernel>Frames*8);
            PrevRaw=Raw;PrevKernel=Kernel;
        }
        TestTrue(TEXT("Simultaneously live backends have distinct identities"),States[0].Room.Get()!=States[1].Room.Get());
        // Excite each independent input, retaining every wet sample for length/leakage checks.
        for(int32 Source=0;Source<2;++Source)
        {
            for(auto& S:States)S.Room->ResetAudioHistory();
            TArray<float> Measured[2];for(auto& M:Measured)M.Reserve(2*163840);
            for(int32 Frame=0;Frame<163840;Frame+=128)
                for(int32 I=0;I<2;++I)
                {
                    float Input[128]={},L[128],R[128];if(Frame==0&&I==Source)Input[0]=1.f;
                    const float* In[]={Input,Input};float* Dest[]={L,R};States[I].Room->ProcessAudioBlock(In,Dest);
                    for(int32 K=0;K<128;++K){Measured[I].Add(L[K]);Measured[I].Add(R[K]);}
                }
            for(int32 I=0;I<2;++I)
            {
                float Peak=0;int32 Last=INDEX_NONE;
                for(int32 K=0;K<Measured[I].Num();++K){const float V=FMath::Abs(Measured[I][K]);Peak=FMath::Max(Peak,V);if(V>1.e-9f)Last=K/2;}
                AddInfo(FString::Printf(TEXT("DSFX_ROOM_PULSE excited=%d observed=%d last=%d peak=%.10g"),Source,I,Last,Peak));
                if(I!=Source)TestEqual(TEXT("No crossfeed"),Peak,0.f);
                else
                {
                    TestEqual(TEXT("Actual wet impulse has the authored length"),Last,I?153599:57599);
                    const FString File=Out/(I?TEXT("hall-impulse.f32"):TEXT("wood-impulse.f32"));
                    if(!FFileHelper::SaveArrayToFile(MakeArrayView(reinterpret_cast<const uint8*>(Measured[I].GetData()),Measured[I].Num()*4),*File))return false;
                }
            }
        }
        for(int32 I=0;I<2;++I){Kernels[I]={};States[I].Room.Reset();}
        IR.Empty();
        const int64 LeftRaw=Bytes(RawTag)-Raw0,LeftKernel=Bytes(KernelTag)-Kernel0;
        AddInfo(FString::Printf(TEXT("DSFX_ROOM_FREED rawRemaining=%lld backendRemaining=%lld"),LeftRaw,LeftKernel));
        TestEqual(TEXT("IR tag back to baseline"),LeftRaw,int64(0));TestEqual(TEXT("DSP tag back to baseline"),LeftKernel,int64(0));
        return !HasAnyErrors();
#else
        AddError(TEXT("Build has no low-level memory tracker"));return false;
#endif
    }
}
#endif
