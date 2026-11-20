#include "DreamSFXScoreTrack.h"
#include "DreamSFXSeed.h"
#include <cmath>

namespace DreamSFX::V2
{
    FWrittenPhrase ReadWrittenPhrase(const TArray<Dsl::FStatement>& Lines,FDiagnostic& Error)
    {
        if(Lines.IsEmpty()){Error={TEXT("DSFX211"),TEXT("1:1"),TEXT("Expected written music")};return {};}
        FString Text;
        TArray<int32> Starts;
        for(const auto& Line:Lines)
        {
            if(Line.Block){Error=Dsl::Expected(Line,TEXT("Expected notes on a staff line"));return {};}
            if(Text.Len())Text.AppendChar(' ');
            Starts.Add(Text.Len());Text+=Line.Text;
        }
        auto Parsed=MakeShared<FDreamSFXScoreResult,ESPMode::ThreadSafe>(FDreamSFXScoreParser::Parse(Text,2048));
        if(!Parsed->IsValid())
        {
            const auto& E=Parsed->Diagnostics[0];int32 Index=Starts.Num()-1;
            while(Index>0&&E.SourceOffset<Starts[Index])--Index;
            Error=Dsl::Expected(Lines[Index],E.Message,E.SourceOffset-Starts[Index]);return {};
        }
        if(Parsed->DurationBeats<=0){Error=Dsl::Expected(Lines[0],TEXT("Expected a note or rest"));return {};}
        return Parsed;
    }

    bool ReadTrackSettings(const Dsl::FStatement& Decl,const TMap<FString,FWrittenPhrase>& Phrases,
        const TMap<FString,int32>& Names,double Window,FMusicTrack& T,FDiagnostic& Error)
    {
        const auto Fail=[&](const Dsl::FStatement& L,const FString& Why){Error=Dsl::Expected(L,Why);return false;};
        const auto Expression=[&](const Dsl::FStatement& L,const FString& S,FProgramExpr& E)
        {
            FString Why;int32 Offset=0;const TMap<FString,double> Constants{{TEXT("pi"),UE_DOUBLE_PI}};
            E.Line=L.Line;E.Column=L.Column+FMath::Max(0,L.Text.Find(S));
            if(!E.Parse(S,Names,Constants,Why,&Offset)){Error=Dsl::Expected(L,Why,E.Column-L.Column+Offset);return false;}
            return true;
        };
        auto Number=[](const FString& S,double& V){return S.IsNumeric()&&LexTryParseString(V,*S)&&FMath::IsFinite(V);};
        TArray<Dsl::FStatement> Staff;TSet<FString> Seen;
        double PeriodSeconds=0.;
        const TSet<FString> Options={TEXT("at"),TEXT("tempo"),TEXT("repeat"),TEXT("every"),TEXT("velocity"),TEXT("transpose"),TEXT("gate"),TEXT("play"),TEXT("pedal"),TEXT("beatSeconds"),TEXT("holdSeconds"),TEXT("periodSeconds")};
        for(const auto& L:Decl.Children)
        {
            if(L.Block)return Fail(L,TEXT("Expected a track setting or written notes"));
            FString K,V;
            if(!L.Text.Split(TEXT(" "),&K,&V)||!Options.Contains(K)){Staff.Add(L);continue;}
            if(Seen.Contains(K))return Fail(L,TEXT("Duplicate track setting ")+K);
            Seen.Add(K);V.TrimStartAndEndInline();double N=0;
            if(K==TEXT("at")||K==TEXT("tempo")||K==TEXT("repeat")||K==TEXT("gate"))
            {
                if(!Number(V,N))return Fail(L,TEXT("Expected a finite number for ")+K);
                if(K==TEXT("at")){if(N<0||N>Window)return Fail(L,TEXT("Track offset outside preview"));T.AtSeconds=N;}
                else if(K==TEXT("tempo")){if(N<20||N>600)return Fail(L,TEXT("Expected tempo between 20 and 600 BPM"));T.SecondsPerBeat=60./N;}
                else if(K==TEXT("gate")){if(N<=0||N>1)return Fail(L,TEXT("Gate must be in (0,1]"));T.Gate=N;}
                else{if(N!=std::floor(N)||N<1||N>256)return Fail(L,TEXT("Repeat count must be 1–256"));T.Repeats=int32(N);}
            }
            else if(K==TEXT("beatSeconds")||K==TEXT("holdSeconds")||K==TEXT("periodSeconds"))
            {
                if(!Number(V,N)||N<=0||N>120)return Fail(L,TEXT("Expected a positive duration up to 120 seconds"));
                if(K==TEXT("beatSeconds"))T.SecondsPerBeat=N;
                else if(K==TEXT("holdSeconds"))T.HoldSeconds=N;
                else PeriodSeconds=N;
            }
            else if(K==TEXT("velocity")||K==TEXT("transpose"))
            {
                if(!Expression(L,V,K==TEXT("velocity")?T.Velocity:T.Transpose))return false;
            }
            else if(K==TEXT("play"))
            {
                if(!Phrases.Contains(V))return Fail(L,TEXT("Expected a declared phrase name"));
                T.Phrase=Phrases[V];
            }
            else if(K==TEXT("every"))
            {
                TArray<FString> W;V.ParseIntoArrayWS(W);double Den=0,Reps=0;
                if(W.Num()!=3||W[1]!=TEXT("x")||!W[0].StartsWith(TEXT("1/"))||!Number(W[0].Mid(2),Den)||Den!=std::floor(Den)||Den<1||Den>128||
                    (int32(Den)&(int32(Den)-1))||!Number(W[2],Reps)||Reps!=std::floor(Reps)||Reps<1||Reps>256)return Fail(L,TEXT("Expected every 1/16 x 8 with a power-of-two duration and 1–256 repeats"));
                T.PeriodBeats=4./Den;T.Repeats=int32(Reps);
            }
            else
            {
                FString List,Condition;
                if(V.Split(TEXT(" when "),&List,&Condition)){if(!Expression(L,Condition,T.PedalEnabled))return false;}
                else List=V;
                if(!List.StartsWith(TEXT("["))||!List.EndsWith(TEXT("]")))return Fail(L,TEXT("Expected pedal [down-up, ...] in track-relative seconds"));
                TArray<FString> Ranges;List.Mid(1,List.Len()-2).ParseIntoArray(Ranges,TEXT(","),false);
                for(auto R:Ranges)
                {
                    FString A,B;double Down,Up;R.TrimStartAndEndInline();
                    if(!R.Split(TEXT("-"),&A,&B)||!Number(A.TrimStartAndEnd(),Down)||!Number(B.TrimStartAndEnd(),Up)||Down<0||Up<=Down||
                       (T.PedalSeconds.Num()&&Down<T.PedalSeconds.Last().Value))return Fail(L,TEXT("Expected ordered non-overlapping pedal ranges"));
                    T.PedalSeconds.Emplace(Down,Up);
                }
            }
        }
        if(Seen.Contains(TEXT("every"))&&Seen.Contains(TEXT("repeat")))return Fail(Decl,TEXT("Choose a phrase repeat or an ornament cadence"));
        if(T.Phrase&&!Staff.IsEmpty())return Fail(Decl,TEXT("Use a named phrase or an inline score in one track"));
        if(!T.Phrase){if(Staff.IsEmpty())return Fail(Decl,TEXT("Track has no music"));T.Phrase=ReadWrittenPhrase(Staff,Error);if(!T.Phrase)return false;}
        if((Seen.Contains(TEXT("beatSeconds"))&&Seen.Contains(TEXT("tempo")))||
            (Seen.Contains(TEXT("holdSeconds"))&&Seen.Contains(TEXT("gate")))||
            (Seen.Contains(TEXT("periodSeconds"))&&Seen.Contains(TEXT("every"))))return Fail(Decl,TEXT("Ambiguous score clock settings"));
        if(PeriodSeconds>0)T.PeriodBeats=PeriodSeconds/T.SecondsPerBeat;
        if(T.PeriodBeats==0)T.PeriodBeats=T.Phrase->DurationBeats;
        if(T.PeriodBeats+1.e-10<T.Phrase->DurationBeats)return Fail(Decl,TEXT("Pattern exceeds its repeat interval"));
        double LastBeat=T.Phrase->DurationBeats;
        if(T.HoldSeconds>0 && !T.Phrase->Notes.IsEmpty())LastBeat=T.Phrase->Notes.Last().AtBeats+T.HoldSeconds/T.SecondsPerBeat;
        if(T.AtSeconds+((T.Repeats-1)*T.PeriodBeats+LastBeat)*T.SecondsPerBeat>Window+1.e-9||
            (T.PedalSeconds.Num()&&T.AtSeconds+T.PedalSeconds.Last().Value>Window+1.e-9))return Fail(Decl,TEXT("Score or pedal lies outside the preview duration"));
        return true;
    }

    void FMusicTrackPlayer::Prepare(const FMusicTrack& InTrack,int32 InRate,uint32 Root)
    {
        Track=&InTrack;Rate=InRate;InitialSeed=DreamSFXSeed::Track(Root,Track->Name.ToString());
        // One notation cycle is resident. Repeated performances reuse the same note and gate storage.
        const int32 Cycles=FMath::Min(Track->Repeats,2+FMath::CeilToInt((128./Rate+Track->HoldSeconds)/(Track->PeriodBeats*Track->SecondsPerBeat)));
        const int32 Capacity=Cycles*Track->Phrase->Notes.Num();
        Releases.Reserve(Capacity);
        BlockEvents.Reserve(2*Capacity+2*Track->PedalSeconds.Num()+4);
        Reset();
    }
    void FMusicTrackPlayer::Reset()
    {
        Pass=NoteIndex=NextId=PedalIndex=0;StopAt=MAX_int64;Random=InitialSeed;Releases.Reset();BlockEvents.Reset();
    }
    void FMusicTrackPlayer::Stop(int64 At){StopAt=FMath::Min(StopAt,At);}
    void FMusicTrackPlayer::Fill(int64 First,int32 Count,TConstArrayView<double> Values)
    {
        BlockEvents.Reset();
        const int64 End=First+Count;
        const int64 Until=FMath::Min(End,StopAt);
        const auto Read=[&](const FProgramExpr& Expr,double Fallback){return Expr.Code.IsEmpty()?Fallback:Expr.Eval(Values,Random);};
        const auto Frame=[&](double Seconds){return FMath::RoundToInt64((Track->AtSeconds+Seconds)*Rate);};
        const auto& Notes=Track->Phrase->Notes;
        while(Pass<Track->Repeats&&!Notes.IsEmpty())
        {
            const auto& Note=Notes[NoteIndex];
            const double Beats=Pass*Track->PeriodBeats+Note.AtBeats;
            const int64 Start=Frame(Beats*Track->SecondsPerBeat);
            if(Start>=Until)break;
            int32 Midi=Note.Midi;
            if(!Note.Choices.IsEmpty())
            {
                Random=1664525u*Random+1013904223u;
                Midi=Note.Choices[(uint64(Random)*Note.Choices.Num())>>32];
            }
            Midi=FMath::Clamp(Midi+FMath::RoundToInt(FMath::Clamp(Read(Track->Transpose,0),-127.,127.)),0,127);
            const float Velocity=float(FMath::Clamp(Read(Track->Velocity,1)*Note.Accent,0.,1.));
            const uint32 Seed=DreamSFXSeed::Note(InitialSeed,NextId);
            BlockEvents.Add({Start,EAction::NoteOn,++NextId,Midi,Velocity,Seed,false});
            const double Hold=Track->HoldSeconds>0?Track->HoldSeconds:Note.DurationBeats*Track->Gate*Track->SecondsPerBeat;
            const int64 Release=FMath::Max(Start+1,Frame(Beats*Track->SecondsPerBeat+Hold));
            Releases.Add({Release,NextId});
            if(++NoteIndex==Notes.Num()){NoteIndex=0;++Pass;}
        }
        while(PedalIndex<2*Track->PedalSeconds.Num())
        {
            const auto& Range=Track->PedalSeconds[PedalIndex/2];
            const bool Down=(PedalIndex%2)==0;
            const int64 At=Frame(Down?Range.Key:Range.Value);
            if(At>=Until)break;
            BlockEvents.Add({At,EAction::Sustain,0,0,0,0,Down&&Read(Track->PedalEnabled,1)>.5});
            ++PedalIndex;
        }
        if(StopAt>=First&&StopAt<End)BlockEvents.Add({StopAt,EAction::Sustain,0,0,0,0,false});
        for(int32 I=Releases.Num()-1;I>=0;--I)
        {
            const int64 At=FMath::Min(Releases[I].Frame,StopAt);
            if(At<End)
            {
                BlockEvents.Add({At,EAction::NoteOff,Releases[I].Id,0,0,0,false});
                Releases.RemoveAtSwap(I,EAllowShrinking::No);
            }
        }
        const auto Priority=[](EAction A){return A==EAction::Sustain?0:A==EAction::NoteOff?1:2;};
        BlockEvents.Sort([&](const auto& A,const auto& B)
        {
            if(A.Frame!=B.Frame)return A.Frame<B.Frame;
            if(A.Action!=B.Action)return Priority(A.Action)<Priority(B.Action);
            if(A.Action==EAction::Sustain&&A.Down!=B.Down)return !A.Down;
            return A.NoteId<B.NoteId;
        });
    }
}
