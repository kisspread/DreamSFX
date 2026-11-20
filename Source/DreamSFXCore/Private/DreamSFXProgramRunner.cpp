#include "DreamSFXProgramRunner.h"

namespace DreamSFX::V2
{
    using DSFXGraphDSP::Block;
    double FProgramRunner::FSpan::At(int64 Frame) const
    {
        const double Elapsed=double(Frame-Start);
        if(Multiplier==1.)return Initial+Increment*Elapsed;
        return Anchor+(Initial-Anchor)*std::pow(Multiplier,Elapsed);
    }
    void FProgramRunner::FLane::Replace(int64 Start)
    {
        while(Spans.Num() && Spans.Last().Start>=Start)Spans.Pop(EAllowShrinking::No);
    }
    void FProgramRunner::FLane::Push(FSpan Span)
    {
        Replace(Span.Start);
        Spans.Add(Span);
    }
    void FProgramRunner::FLane::Fill(int64 First,float* Values)
    {
        int32 At=0;
        while(At+1<Spans.Num()&&Spans[At+1].Start<=First)++At;
        if(At)Spans.RemoveAt(0,At,EAllowShrinking::No);
        int32 Offset=0,Index=0;
        while(Offset<Block)
        {
            const auto& S=Spans[Index];
            const int32 Last=Index+1<Spans.Num()?int32(FMath::Min<int64>(Block,Spans[Index+1].Start-First)):Block;
            double Value=S.At(First+Offset);
            // Target and exponential segments use the same recurrence as static V2 automation.
            const double Add=S.Increment+S.Anchor*(1.-S.Multiplier);
            for(;Offset<Last;++Offset){Values[Offset]=float(Value);Value=Value*S.Multiplier+Add;}
            ++Index;
        }
    }
    // A scheduled ramp endpoint survives an intervening note on the same oscillator.
    // Each interval is determined by its right endpoint. Recompute only its neighbours.
    void FProgramRunner::FLane::InsertKey(double Frame,double Value,bool RampEnd)
    {
        FSpan Key{FMath::CeilToInt64(Frame),Value};Key.bKey=true;Key.bIncomingExponential=RampEnd;
        Key.KeyFrame=Frame;Key.KeyValue=Value;
        const auto ExactStart=[](const FSpan& S){return S.bKey?S.KeyFrame:double(S.Start);};
        int32 At=0;
        while (At<Spans.Num() && ExactStart(Spans[At])<Frame) ++At;
        while (At<Spans.Num() && ExactStart(Spans[At])==Frame && Spans[At].bKey && Spans[At].bIncomingExponential!=RampEnd) ++At;
        if (At<Spans.Num() && ExactStart(Spans[At])==Frame) Spans[At]=Key;
        else Spans.Insert(Key,At);
        for (int32 I=FMath::Max(0,At-1); I<=At && I+1<Spans.Num(); ++I)
        {
            FSpan& Left=Spans[I];
            const FSpan& Right=Spans[I+1];
            if (!Left.bKey && !Right.bIncomingExponential) continue;
            Left.Anchor=Left.Increment=0.;Left.Multiplier=1.;
            if (Left.bKey) Left.Initial=Left.KeyValue;
            const double Distance=ExactStart(Right)-ExactStart(Left);
            if (Right.bIncomingExponential && Distance>0 && Left.Initial>0 && Right.KeyValue>0)
            {
                Left.Multiplier=std::pow(Right.KeyValue/Left.Initial,1./Distance);
                if (double(Left.Start)!=ExactStart(Left))
                    Left.Initial*=std::pow(Left.Multiplier,double(Left.Start)-ExactStart(Left));
            }
        }
    }
    FProgramRunner::FProgramRunner(const FSoundProgram& P,int32 SampleRate,TArray<FControlState>& C,
        TArray<DSFXGraphDSP::FState>& S,const TMap<FName,int32>& NodeMap,const TMap<FPortRef,int32>& Ports,TFunction<void(int32)> Reset)
        : Program(P),Rate(SampleRate),Controls(C),Signals(S),Nodes(NodeMap),ResetNode(MoveTemp(Reset))
    {
        auto Bind=[&](const FPortRef& Port)
        {
            if(PortLanes.Contains(Port))return;
            int32 I=Lanes.AddDefaulted();PortLanes.Add(Port,I);Lanes[I].Slot=Ports.FindChecked(Port);
            Controls[Lanes[I].Slot].bProgramDriven=true;Lanes[I].Spans.Reserve(66);
        };
        for(const auto& Bank:P.Banks)
        {
            Banks.AddDefaulted_GetRef().Voices.SetNum(Bank.Voices.Num());
            for(const auto& Voice:Bank.Voices)for(const auto& B:Voice.Bindings)Bind(B.Port);
        }
        auto BindList=[&](const FRuleList& L)
        {
            for(const auto& A:L.Actions)if(A.Kind==ERuleAction::Write)
            {
                Bind(A.Write.Port);
                for(const FPortRef& Port:A.IndexedPorts)Bind(Port);
            }
        };
        BindList(P.Start);for(const auto& L:P.Routines)BindList(L);
        for(const auto& State:P.States){BindList(State.Enter);BindList(State.Tick);}
        if(P.Banks.ContainsByPredicate([](const auto& B){return B.bPool;}))Pending.Reserve(256);
        for(const auto& T:P.Tracks)Music.AddDefaulted_GetRef().Prepare(T,Rate,P.ScoreSeed);
        Values.SetNumZeroed(P.RegisterCount);Overrides.SetNum(P.Inputs.Num());
    }
    void FProgramRunner::Fault(const FProgramExpr& E,const TCHAR* Why)
    {
        if(!bFault) UE_LOG(LogTemp,Error,TEXT("DreamSFX DSL %d:%d: %s"),E.Line,E.Column,Why);
        bFault=true;
    }
    double FProgramRunner::Eval(const FProgramExpr& E)
    {
        bool Ok=false;const double V=E.Eval(Values,Random,&Ok,Program.Algorithm);
        if(!Ok)Fault(E,TEXT("Expected finite arithmetic inside the expression domain"));
        return V;
    }
    void FProgramRunner::Restart()
    {
        bFault=false;Tick=Frame=AuditionCursor=0;Random=Program.Random;
        for(auto& Track:Music)Track.Reset();
        Pending.Reset();Order=0;AppliedExternal=RequestedState=INDEX_NONE;
        FMemory::Memzero(Values.GetData(),Values.Num()*sizeof(double));
        Values[2]=1./Program.TickHz;
        for(const auto& Input:Program.Inputs)Values[Input.Register]=Input.Default;
        for(const auto& M:Program.Memory)Values[M.Register]=Eval(M.Expr);
        Current=Program.States.IndexOfByPredicate([](const auto& S){return S.Name==TEXT("idle");});
        for(auto& Lane:Lanes){Lane.Spans.Reset();Lane.Spans.Add({0,Controls[Lane.Slot].Base});}
        for(auto& B:Banks){B.Next=0;B.Count=0;for(auto& Voice:B.Voices)Voice={};}
    }
    bool FProgramRunner::Input(FName Name,double V)
    {
        const int32 I=Program.Inputs.IndexOfByPredicate([&](const auto& A){return A.Name==Name;});
        if(I==INDEX_NONE||!FMath::IsFinite(V))return false;
        Overrides[I]=FMath::Clamp(V,Program.Inputs[I].Min,Program.Inputs[I].Max);return true;
    }
    void FProgramRunner::ClearInput(FName Name)
    {
        const int32 I=Program.Inputs.IndexOfByPredicate([&](const auto& A){return A.Name==Name;});
        if(I!=INDEX_NONE)Overrides[I].Reset();
    }
    bool FProgramRunner::State(FName Name)
    {
        if(Name.IsNone()){ExternalState=INDEX_NONE;return true;}
        const int32 I=Program.States.IndexOfByPredicate([&](const auto& S){return S.Name==Name;});
        if(I==INDEX_NONE)return false;
        ExternalState=I;return true;
    }
    uint32 FProgramRunner::PlayCount(FName Name)const
    {
        const int32 I=Program.Banks.IndexOfByPredicate([&](const auto& B){return B.Name==Name;});
        return I==INDEX_NONE?0:Banks[I].Count;
    }
    void FProgramRunner::Write(const FProgramBinding& Binding,double Time,bool Following,const FPortRef* IndexedPort)
    {
        const FRuleCurve& C=Binding.Curve;
        const FPortRef& Port=IndexedPort?*IndexedPort:Binding.Port;
        auto& Lane=Lanes[PortLanes.FindChecked(Port)];
        if(Lane.Spans.Num()>128){Fault(C.Args[0],TEXT("Too many pending changes on this control"));return;}
        const int64 Start=FMath::Max(Frame,FMath::RoundToInt64(Time*Rate));
        auto Sample=[&](int32 I){return double(float(Eval(C.Args[I])));};
        if(C.Kind==ERuleCurve::Follow&&!Following){Lane.Push({Start,Sample(0)});return;}
        if(C.Kind==ERuleCurve::Set){Lane.Push({Start,Sample(0)});return;}
        if(C.Kind==ERuleCurve::Target||C.Kind==ERuleCurve::Follow)
        {
            const bool bExplicitStart=C.Kind==ERuleCurve::Target&&C.Args.Num()==3;
            const double ExplicitStart=bExplicitStart?Sample(0):0.;
            const int32 I=(Following||bExplicitStart)?1:0;
            const double Target=Sample(I),Tau=Sample(I+1);
            if(Tau<=0.){Fault(C.Args[I+1],TEXT("Expected a positive target time constant"));return;}
            int32 Prev=Lane.Spans.Num()-1;
            while(Prev>0&&Lane.Spans[Prev].Start>Start)--Prev;
            const double Before=bExplicitStart?ExplicitStart:Lane.Spans.Num()?Lane.Spans[Prev].At(Start):Controls[Lane.Slot].Base;
#if WITH_DEV_AUTOMATION_TESTS
            if(ObserveTarget&&bExplicitStart)ObserveTarget(Port,Time,Before,Target);
#endif
            Lane.Push({Start,Before,Target,std::exp(-1./(Rate*Tau))});return;
        }
        const bool bExactKeys=C.Kind==ERuleCurve::ExactExponentialKeys;
        const bool bExponentKeys=bExactKeys||C.Kind==ERuleCurve::ExponentialKeys;
        TArray<TPair<double,double>,TInlineAllocator<48>> Points;
        if(C.Kind==ERuleCurve::Swell)
        {
            const double Length=Eval(C.Args[0]),Peak=Eval(C.Args[1]),Lean=Eval(C.Args[2]);
            const double Delay=C.Args.Num()==4?Eval(C.Args[3]):0.;
            if(Delay<0){Fault(C.Args[3],TEXT("Expected a nonnegative swell delay"));return;}
            if(Delay>0)Lane.Push({Start,0.});
            if(Length<=0||Lean<=0){Fault(C.Args[0],TEXT("Expected positive swell duration and shape"));return;}
            for(int32 I=0;I<48;++I)
            {
                const double Sine=std::sin(UE_DOUBLE_PI*std::pow(I/47.,Lean));
                Points.Emplace(FMath::RoundToInt64((Time+Delay+Length*I/47.)*Rate),double(float(Peak*Sine*Sine)));
            }
        }
        else for(int32 I=0;I<C.Args.Num();I+=2)
        {
            const double FrameAtKey=(Time+Eval(C.Args[I]))*Rate;
            const double T=FMath::Max(double(Frame),bExactKeys?FrameAtKey:double(FMath::RoundToInt64(FrameAtKey)));
            const double V=Sample(I+1);
            if(Points.Num()&&T<Points.Last().Key){Fault(C.Args[I],TEXT("Expected ascending curve times"));return;}
            Points.Emplace(T,V);
        }
#if WITH_DEV_AUTOMATION_TESTS
        if(IndexedPort && ObserveTarget && (C.Kind==ERuleCurve::Exponential||bExponentKeys) && Points.Num()==2)
            ObserveTarget(Port,Time,Points[0].Value,Points[1].Value);
#endif
        if(bExponentKeys)
        {
            if (Lane.Spans.Num()+Points.Num()>128){Fault(C.Args[0],TEXT("Too many pending automation keys on this control"));return;}
            for(int32 I=0;I<Points.Num();++I)
            {
                if (Points[I].Value<=0.){Fault(C.Args[2*I+1],TEXT("Exponential keys require positive values"));return;}
                Lane.InsertKey(Points[I].Key,Points[I].Value,I!=0);
            }
            return;
        }
        for(int32 I=0;I<Points.Num();++I)
        {
            FSpan Span{int64(Points[I].Key),Points[I].Value};
            if(I+1<Points.Num())
            {
                const double Count=double(Points[I+1].Key-Span.Start);
                if(Count<=0)continue;
                const double To=Points[I+1].Value;
                if(C.Kind==ERuleCurve::Exponential&&To>0&&Span.Initial>0)Span.Multiplier=std::pow(To/Span.Initial,1./Count);
                else Span.Increment=(To-Span.Initial)/Count;
            }
            Lane.Push(Span);
        }
    }
    void FProgramRunner::Play(int32 Bank,int32 Voice,double Time)
    {
#if WITH_DEV_AUTOMATION_TESTS
        if(ObserveLaunch)ObserveLaunch(Program.Banks[Bank].Name,Time);
#endif
        auto& Active=Banks[Bank].Voices[Voice];Active.Following=true;Active.Start=Time;Active.BusyUntil=0;++Banks[Bank].Count;
        const auto& V=Program.Banks[Bank].Voices[Voice];
        for(FName Id:V.Nodes)ResetNode(Nodes.FindChecked(Id));
        for(const auto& A:V.Init)Values[A.Register]=Eval(A.Expr);
        for(const auto& Op:V.Operations)
        {
            if(!Op.bGate){Write(V.Bindings[Op.Index],Time);continue;}
            const auto& Gate=V.Sources[Op.Index];auto& S=Signals[Nodes.FindChecked(Gate.Node)];
            const double Exact=Time*Rate;
            S.Begin=FMath::CeilToInt64(Exact);S.ElapsedAtFirstFrame=S.Begin-Exact;
            S.End=Gate.Life.Code.IsEmpty()?MAX_int64/4:FMath::CeilToInt64((Time+FMath::Max(0.,Eval(Gate.Life)))*Rate);
            Active.BusyUntil=FMath::Max(Active.BusyUntil,S.End);
            if(!Gate.Offset.Code.IsEmpty())S.Read=FMath::RoundToDouble(FMath::Max(0.,Eval(Gate.Offset))*DSFXGraphDSP::ReferenceRate);
#if WITH_DEV_AUTOMATION_TESTS
            if(ObserveGate)ObserveGate(Program.Banks[Bank].Name,Voice,Gate.Node,S.Begin,S.End);
#endif
        }
    }
    int32 FProgramRunner::FreeVoice(int32 Bank)const
    {
        const auto& State=Banks[Bank];
        for(int32 N=0;N<State.Voices.Num();++N)
        {
            const int32 I=(State.Next+N)%State.Voices.Num();
            if(State.Voices[I].BusyUntil>Frame)continue;
            const auto& Voice=Program.Banks[Bank].Voices[I];
            bool Tail=false;
            for(FName Id:Voice.Nodes)
            {
                const auto& Signal=Signals[Nodes.FindChecked(Id)];
                if(Signal.Filter.HasTail()||Signal.DelayTail>0){Tail=true;break;}
            }
            if(!Tail)return I;
        }
        return INDEX_NONE;
    }
    void FProgramRunner::SchedulePlay(int32 Bank,double Time)
    {
        const auto& Definition=Program.Banks[Bank];auto& State=Banks[Bank];
        if(!Definition.bPool)
        {
            Play(Bank,State.Next,Time);State.Next=(State.Next+1)%State.Voices.Num();return;
        }
        if(Pending.Num()>=256)
        {
            UE_LOG(LogTemp,Error,TEXT("DreamSFX: pending voice launch budget exceeded in %s"),*Definition.Name.ToString());
            bFault=true;return;
        }
        // Keep launch-time operands and the stream cursor. No DSP slot is occupied until onset.
        auto& Event=Pending.AddDefaulted_GetRef();
        Event.Bank=Bank;Event.Time=Time;Event.Order=Order++;Event.RandomAtIssue=Random;
        for(int32 I=0;I<Definition.LaunchInputs.Num();++I)Event.Inputs[I]=Values[Definition.LaunchInputs[I]];
        Random=AdvanceProgramRandom(Random,Definition.LaunchDraws,Program.Algorithm);
    }
    void FProgramRunner::DispatchDue()
    {
        while(!Pending.IsEmpty())
        {
            int32 Best=INDEX_NONE;
            for(int32 I=0;I<Pending.Num();++I)
            {
                const auto& E=Pending[I];
                if(E.Time*Rate>=Frame+Block)continue;
                if(Best==INDEX_NONE || E.Time<Pending[Best].Time || (E.Time==Pending[Best].Time&&E.Order<Pending[Best].Order))Best=I;
            }
            if(Best==INDEX_NONE)return;
            const auto& E=Pending[Best];const auto& Bank=Program.Banks[E.Bank];
            const int32 Voice=FreeVoice(E.Bank);
            if(Voice==INDEX_NONE)
            {
                UE_LOG(LogTemp,Error,TEXT("DreamSFX: voice bank %s exhausted at %.9f seconds"),*Bank.Name.ToString(),E.Time);
                bFault=true;return;
            }
            double Saved[MaxLaunchInputs];
            for(int32 I=0;I<Bank.LaunchInputs.Num();++I)
            {
                auto& V=Values[Bank.LaunchInputs[I]];Saved[I]=V;V=E.Inputs[I];
            }
            const uint32 Resume=Random;Random=E.RandomAtIssue;
            Play(E.Bank,Voice,E.Time);
            Random=Resume;
            Banks[E.Bank].Next=(Voice+1)%Bank.Voices.Num();
            for(int32 I=0;I<Bank.LaunchInputs.Num();++I)Values[Bank.LaunchInputs[I]]=Saved[I];
            Pending.RemoveAtSwap(Best,EAllowShrinking::No);
        }
    }
    void FProgramRunner::Run(const FRuleList& L)
    {
        for(const auto& A:L.Actions)
        {
            const double T=Values[1];
            switch(A.Kind)
            {
            case ERuleAction::Enter:RequestedState=A.Index;break;
            case ERuleAction::Assign:Values[A.Index]=Eval(A.Expr);break;
            case ERuleAction::Write:
            {
                const FPortRef* Port=nullptr;
                if (!A.IndexedPorts.IsEmpty())
                {
                    const double I=Eval(A.VoiceIndex);
                    if (I<0 || I>=A.IndexedPorts.Num() || I!=std::floor(I))
                    { Fault(A.VoiceIndex,TEXT("Expected an integer slot within this voice bank"));return; }
                    Port=&A.IndexedPorts[int32(I)];
                }
                Write(A.Write,A.Expr.Code.IsEmpty()?T:Eval(A.Expr),false,Port);
                break;
            }
            case ERuleAction::Call:Run(Program.Routines[A.Index]);break;
            case ERuleAction::Condition:if(Eval(A.Expr)!=0)Run(Program.Routines[A.Index]);break;
            case ERuleAction::Repeat:
            {
                const double Repeats=Eval(A.Expr);if(Repeats<0||Repeats>MaxRuleRepeat){Fault(A.Expr,TEXT("Expected 0–128 repetitions"));return;}
                const int32 Count=FMath::CeilToInt(Repeats);
                for(int32 I=0;I<Count;++I)Run(Program.Routines[A.Index]);break;
            }
            case ERuleAction::All:case ERuleAction::Play:
            {
                auto& Bank=Banks[A.Index];
                const double When=FMath::Clamp(A.Expr.Code.IsEmpty()?T:Eval(A.Expr),T,T+120.);
                if(A.Kind==ERuleAction::All){for(int32 I=0;I<Bank.Voices.Num();++I)Play(A.Index,I,When);}
                else SchedulePlay(A.Index,When);
                break;
            }
            case ERuleAction::Stop:
            {
                const double Seconds=A.Expr.Code.IsEmpty()?0.:FMath::Clamp(Eval(A.Expr),0.,120.);
                const int64 End=FMath::CeilToInt64((T+Seconds)*Rate);
                for(int32 I=0;I<Music.Num();++I)if(Program.Tracks[I].Bank==A.Index)Music[I].Stop(End);
                Pending.RemoveAllSwap([&](const auto& E){return E.Bank==A.Index&&E.Time>=T+Seconds;},EAllowShrinking::No);
                for(int32 I=0;I<Banks[A.Index].Voices.Num();++I)
                {
                    Banks[A.Index].Voices[I].BusyUntil=FMath::Min(Banks[A.Index].Voices[I].BusyUntil,End);
                    Banks[A.Index].Voices[I].Following=false;
                    for(const auto& Gate:Program.Banks[A.Index].Voices[I].Sources)
                    {
                        auto& S=Signals[Nodes.FindChecked(Gate.Node)];S.End=FMath::Min(S.End,End);
                    }
                }
                break;
            }
            }
        }
    }
    void FProgramRunner::Quantum(int64 Begin)
    {
        Frame=Begin;
        // First audio quantum on or after the control clock's requested sample.
        while((Tick*Rate/Program.TickHz)<=Frame)
        {
            Values[0]=double(Tick)/Program.TickHz;
            Values[1]=double(Frame)/Rate;
            for(int32 I=0;I<Program.Inputs.Num();++I)
            {
                const auto& In=Program.Inputs[I];
                const double V=Overrides[I].IsSet()?Overrides[I].GetValue():Live()||In.Preview.Code.IsEmpty()?In.Default:Eval(In.Preview);
                Values[In.Register]=FMath::Clamp(V,In.Min,In.Max);
            }
            if(!Tick)Run(Program.Start);
            int32 Desired=Current;
            if(Live()){if(AppliedExternal!=ExternalState)Desired=ExternalState;AppliedExternal=ExternalState;}
            else {AppliedExternal=INDEX_NONE;while(Program.Audition.IsValidIndex(AuditionCursor)&&Program.Audition[AuditionCursor].Key<=Values[0])
                Desired=Program.Audition[AuditionCursor++].Value;}
            if(Desired!=Current){Current=Desired;Run(Program.States[Current].Enter);}
            Run(Program.States[Current].Tick);
            if(RequestedState!=INDEX_NONE)
            {
                const int32 To=RequestedState;RequestedState=INDEX_NONE;
                if(To!=Current){Current=To;Run(Program.States[Current].Enter);}
            }
            DispatchDue();
            for(int32 B=0;B<Banks.Num();++B)for(int32 I=0;I<Banks[B].Voices.Num();++I)
            {
                const auto& V=Banks[B].Voices[I];
                if(V.Following&&V.Start<=Values[1])for(const auto& Bind:Program.Banks[B].Voices[I].Bindings)
                    if(Bind.Curve.Kind==ERuleCurve::Follow)Write(Bind,Values[1],true);
            }
            ++Tick;
        }
        DispatchDue();
        for(auto& Track:Music)Track.Fill(Frame,Block,Values);
        for(auto& Lane:Lanes)Lane.Fill(Frame,Controls[Lane.Slot].Curve.Values);
    }
}
