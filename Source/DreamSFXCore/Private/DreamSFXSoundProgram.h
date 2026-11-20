#pragma once
#include "DreamSFXGraphCompiler.h"
#include "DreamSFXScoreParser.h"
#include "DreamSFXProgramRandom.h"

namespace DreamSFX::V2
{
    struct FProgramExpr
    {
        struct FOp { uint8 Op; double Operand; };
        TArray<FOp> Code;
        int32 Line=1,Column=1;
        bool Constant() const;
        int32 CaptureReads(TArray<int32>& Registers,int32 GlobalCount) const;
        double Eval(TConstArrayView<double> Registers, uint32& Random,bool* Valid=nullptr,
            EProgramRandom Algorithm=EProgramRandom::Lcg32) const;
        bool Parse(const FString& Source, const TMap<FString,int32>& Names, const TMap<FString,double>& Constants, FString& Error,int32* Offset=nullptr);
    };

    enum class ERuleCurve : uint8 { Set, Target, Follow, Linear, Exponential, Swell, ExponentialKeys, ExactExponentialKeys };
    struct FRuleCurve { ERuleCurve Kind = ERuleCurve::Set; TArray<FProgramExpr> Args; };
    struct FProgramBinding { FPortRef Port; FRuleCurve Curve; };
    struct FProgramSource { FName Node; FProgramExpr Life, Offset; };
    struct FAssignment { int32 Register; FProgramExpr Expr; };
    struct FProgramVoice
    {
        TArray<FAssignment> Init;
        TArray<FName> Nodes;
        // Source gates and parameter draws are applied in authored signal-path order.
        struct FOperation { bool bGate; int32 Index; };
        TArray<FOperation> Operations;
        TArray<FProgramBinding> Bindings;
        TArray<FProgramSource> Sources;
    };
    struct FVoiceBank
    {
        FName Name;
        TArray<FProgramVoice> Voices;
        TArray<int32> LaunchInputs;
        int32 LaunchDraws=0;
        bool bPool=false;
    };
    // Structural voices and future event records have independent budgets.
    constexpr int32 MaxRuleRepeat=128;
    constexpr int32 MaxLaunchInputs=64;
    uint32 AdvanceProgramRandom(uint32 Seed,uint32 Count,EProgramRandom Algorithm=EProgramRandom::Lcg32);
    enum class ERuleAction : uint8 { Assign, Write, Play, All, Stop, Call, Condition, Repeat, Enter };
    struct FRuleAction
    {
        ERuleAction Kind{};
        int32 Index = INDEX_NONE;
        FProgramExpr Expr;
        FProgramBinding Write;
        FProgramExpr VoiceIndex;
        TArray<FPortRef> IndexedPorts;
    };
    struct FRuleList { TArray<FRuleAction> Actions; };
    struct FProgramState { FName Name; FRuleList Enter, Tick; };
    struct FProgramInput { FName Name; int32 Register; double Min, Max, Default; FProgramExpr Preview; };
    struct FMusicTrack
    {
        FName Name, Instrument;
        int32 Bank = INDEX_NONE;
        double AtSeconds = 0., SecondsPerBeat = .5, Gate = 1., PeriodBeats = 0., HoldSeconds = 0.;
        int32 Repeats = 1;
        FProgramExpr Velocity, Transpose, PedalEnabled;
        TArray<TPair<double,double>> PedalSeconds;
        TSharedPtr<const FDreamSFXScoreResult,ESPMode::ThreadSafe> Phrase;
    };
    struct FSoundProgram
    {
        FString Source;
        int32 TickHz = 100;
        uint32 Random = 0;
        EProgramRandom Algorithm = EProgramRandom::Lcg32;
        TArray<FProgramInput> Inputs;
        int32 RegisterCount = 0;
        TArray<FAssignment> Memory;
        TArray<FVoiceBank> Banks;
        TArray<FRuleList> Routines;
        TArray<FProgramState> States;
        TArray<FMusicTrack> Tracks;
        uint32 ScoreSeed = 0;
        TArray<TPair<double,int32>> Audition;
        FRuleList Start;
    };
    // Produces ordinary typed DSP nodes and a separately clocked control program.
    bool CompileSoundProgram(const FString& Text,FGraphIR& Graph,TArray<FDiagnostic>& Errors);
}
