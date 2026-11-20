#pragma once
#include "DreamSFXSoundProgram.h"
#include "DreamSFXScoreTrack.h"
#include "DreamSFXNodeRegistry.h"

namespace DreamSFX::V2
{
    class FProgramRunner
    {
    public:
        FProgramRunner(const FSoundProgram& InProgram,int32 SampleRate,TArray<FControlState>& InControls,
            TArray<DSFXGraphDSP::FState>& InSignals,const TMap<FName,int32>& InNodes,const TMap<FPortRef,int32>& InPorts,
            TFunction<void(int32)> InResetNode);
        void Restart();
        void Quantum(int64 FirstFrame);
        bool Input(FName Name,double Value);
        bool State(FName Name);
        void ClearInput(FName Name);
        bool Live() const {return ExternalState!=INDEX_NONE;}
        bool HasError()const{return bFault;}
        FName StateName()const{return Program.States[Current].Name;}
        const TArray<FTrackEvent>& MusicEvents(int32 Track) const { return Music[Track].Events(); }
        uint32 PlayCount(FName Bank) const;
#if WITH_DEV_AUTOMATION_TESTS
        TFunction<void(FName,double)> ObserveLaunch;
        TFunction<void(const FPortRef&,double,double,double)> ObserveTarget;
        TFunction<void(FName,int32,FName,int64,int64)> ObserveGate;
#endif

    private:
        struct FSpan
        {
            int64 Start=0;
            double Initial=0,Anchor=0,Multiplier=1,Increment=0;
            bool bKey=false, bIncomingExponential=false;
            double KeyFrame=0., KeyValue=0.;
            double At(int64 Frame) const;
        };
        struct FLane
        {
            int32 Slot;
            TArray<FSpan> Spans;
            void Replace(int64 Start);
            void Push(FSpan Span);
            void InsertKey(double Frame,double Value,bool RampEnd);
            void Fill(int64 First,float* Values);
        };
        struct FActive {bool Following=false; double Start=0; int64 BusyUntil=0;};
        struct FPendingStart
        {
            double Time;
            uint32 RandomAtIssue;
            int32 Bank;
            uint64 Order;
            double Inputs[MaxLaunchInputs];
        };
        struct FBankState {int32 Next=0;uint32 Count=0;TArray<FActive> Voices;};
        const FSoundProgram& Program;
        int32 Rate;
        TArray<FControlState>& Controls;
        TArray<DSFXGraphDSP::FState>& Signals;
        TMap<FName,int32> Nodes;
        TMap<FPortRef,int32> PortLanes;
        TFunction<void(int32)> ResetNode;
        TArray<FLane> Lanes;
        TArray<FBankState> Banks;
        TArray<FMusicTrackPlayer> Music;
        TArray<double> Values;
        TArray<TOptional<double>> Overrides;
        uint32 Random=0;
        int64 Frame=0,Tick=0;
        int32 Current=INDEX_NONE,ExternalState=INDEX_NONE,AppliedExternal=INDEX_NONE,RequestedState=INDEX_NONE,AuditionCursor=0;
        TArray<FPendingStart> Pending;
        uint64 Order=0;
        void SchedulePlay(int32 Bank,double Time);
        void DispatchDue();
        int32 FreeVoice(int32 Bank)const;
        bool bFault=false;
        void Fault(const FProgramExpr& E,const TCHAR* Why);
        double Eval(const FProgramExpr& E);
        void Write(const FProgramBinding& B,double At,bool Follow=false,const FPortRef* IndexedPort=nullptr);
        void Play(int32 Bank,int32 Voice,double At);
        void Run(const FRuleList& List);
    };
}
