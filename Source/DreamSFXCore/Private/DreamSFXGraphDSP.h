#pragma once

#include "CoreMinimal.h"
#include "DSP/ConvolutionAlgorithm.h"
#include <cmath>

// WebAudio compressor, pinned source and license in ThirdParty/sndfilter.
THIRD_PARTY_INCLUDES_START
namespace DSFXCompressor
{
#include "../../ThirdParty/sndfilter/compressor.h"
}
THIRD_PARTY_INCLUDES_END

namespace DSFXGraphDSP
{
constexpr int32 Block=128;
constexpr double ReferenceRate=48000.0;
constexpr double Pi=UE_DOUBLE_PI;
enum class EWave { Sine, Sawtooth, Square, Triangle };
enum class EFilter { Lowpass, Bandpass, Highpass };
enum class ERecipe { White, Pink, Room, Brown, BrownLoop, DiffuseImpulse, ThreePoleLoop };
enum class ECurve { Set, Linear, Exponential, Target };

struct FPoint
{
    double At;
    float Value;
    float TimeConstant;
    ECurve Mode;
};

struct FGeneratedBuffer
{
    TArray<float> Channel[2];
};

inline FGeneratedBuffer Generate(ERecipe Recipe, uint32 Seed, int32 Frames, int32 Channels=2)
{
    uint32 State=Seed;
    auto Random=[&State]() { State=State*1664525u+1013904223u; return double(State)/4294967296.; };
    FGeneratedBuffer Result;
    for (int32 ChannelIndex=0;ChannelIndex<Channels;++ChannelIndex)
    {
        auto& Channel=Result.Channel[ChannelIndex];
        Channel.SetNumUninitialized(Frames);
        double Low=0,B0=0,B1=0,B2=0;
        for (int32 i=0;i<Frames;++i)
        {
            const double White=2*Random()-1;
            const double T=i/ReferenceRate;
            switch (Recipe)
            {
            case ERecipe::White: Channel[i]=float(White); break;
            case ERecipe::Pink:
                B0=.99765*B0+.099046*White; B1=.963*B1+.2965164*White; B2=.57*B2+1.0526913*White;
                Channel[i]=float(.24*(B0+B1+B2+.1848*White)); break;
            case ERecipe::Brown:
                Low = (Low + .02 * White) / 1.02;
                Channel[i] = float(Low * 3.2);
                break;
            case ERecipe::Room:
                Low+=(.03+.55*std::exp(-1.6*T))*(White-Low);
                Channel[i]=float(Low*std::exp(-2.1*T)*FMath::Min(1.,T/.01)); break;
            }
        }
        if (Recipe==ERecipe::Room)
            for (int32 k=0;k<7;++k)
            {
                const int32 At=int32(ReferenceRate*(.018+.14*Random()));
                const float Reflection=float((Random()<.5?-1:1)*(.9-.08*k));
                if (Channel.IsValidIndex(At)) Channel[At]+=Reflection;
            }
    }
    if(Channels==1)Result.Channel[1]=Result.Channel[0];
    return Result;
}

// Evaluate each AudioParam once per quantum. Time constants and exponential ramps
// become recurrences at Prepare; Render performs no exponentials for those curves.
struct FSpan
{
    int64 End=0;
    double First=0,Multiply=1,Add=0;
};
struct FParameter
{
    TArray<FSpan> Spans;
    double Value=0;
    int32 Index=0;
    float Values[Block]={};

    void Prepare(float Base, TConstArrayView<FPoint> Points, double FramesPerSecond)
    {
        double Previous=Base, Target=Previous,Tau=0;
        int64 From=0;
        auto Span=[&](int64 End,ECurve Next,double To)
        {
            const int64 Count=End-From;
            if (Count<=0) return;
            FSpan S; S.End=End; S.First=Previous;
            if (Next==ECurve::Exponential&&Previous>0&&To>0) S.Multiply=std::pow(To/Previous,1./Count);
            else if (Next==ECurve::Linear || Next==ECurve::Exponential) S.Add=(To-Previous)/Count;
            else if (Tau>0)
            {
                S.Multiply=std::exp(-1./(FramesPerSecond*Tau)); S.Add=Target*(1-S.Multiply);
                Previous=Target+(Previous-Target)*std::exp(-Count/(FramesPerSecond*Tau));
            }
            Spans.Add(S);
            From=End;
        };
        for (const auto& E:Points)
        {
            Span(FMath::RoundToInt64(E.At*FramesPerSecond),E.Mode,E.Value);
            if (E.Mode==ECurve::Target){ Target=E.Value; Tau=E.TimeConstant; }
            else { Previous=E.Value; Tau=0; }
        }
        Span(MAX_int32,ECurve::Set,Previous);
        Reset();
    }
    void Reset(){ Index=0; Value=Spans[0].First; }
    void Fill(int64 Frame)
    {
        for (int32 i=0;i<Block;++i)
        {
            while (Index+1<Spans.Num() && Frame+i>=Spans[Index].End) Value=Spans[++Index].First;
            Values[i]=float(Value);
            Value=Value*Spans[Index].Multiply+Spans[Index].Add;
        }
    }
};

struct FBiquad
{
    double B0=1,B1=0,B2=0,A1=0,A2=0, X1[2]={},X2[2]={},Y1[2]={},Y2[2]={};
    float LastHz=-1,LastQ=-1,LastGainDb=0;
    void Set(EFilter Filter,float Freq,float Q,double Rate)
    {
        Freq=FMath::Clamp(Freq,0.f,float(Rate*.49999));
        if (Freq==LastHz&&Q==LastQ) return;
        LastHz=Freq;LastQ=Q;
        const double W=2*Pi*Freq/Rate, Cos=std::cos(W);
        // WebAudio low/high pass resonance is in dB; band-pass Q is linear.
        const double Resonance=Filter==EFilter::Bandpass?FMath::Max(.0001,double(Q)):std::pow(10.,FMath::Clamp(double(Q),-100.,100.)/20);
        const double Alpha=std::sin(W)/(2*Resonance), Inv=1/(1+Alpha);
        A1=-2*Cos*Inv; A2=(1-Alpha)*Inv;
        if (Filter==EFilter::Lowpass){ B0=.5*(1-Cos)*Inv; B1=2*B0; B2=B0; }
        else if (Filter==EFilter::Highpass){ B0=.5*(1+Cos)*Inv; B1=-2*B0; B2=B0; }
        else { B0=Alpha*Inv; B1=0; B2=-B0; }
    }
    // RBJ shelf at unit slope. Freq is the half-gain frequency.
    void SetLowShelf(float Frequency, float GainDb, double Rate)
    {
        Frequency = FMath::Clamp(Frequency, 0.f, float(Rate * .49999));
        GainDb = FMath::Clamp(GainDb, -48.f, 48.f);
        if (Frequency == LastHz && GainDb == LastGainDb) return;
        LastHz = Frequency;
        LastGainDb = GainDb;
        if (Frequency == 0.f || GainDb == 0.f)
        {
            B0 = 1.;
            B1 = B2 = A1 = A2 = 0.;
            return;
        }
        const double A = std::pow(10., GainDb / 40.);
        const double Angle = 2. * Pi * Frequency / Rate;
        const double Cosine = std::cos(Angle);
        const double Plus = A + 1.;
        const double Minus = A - 1.;
        const double Beta = std::sqrt(2. * A) * std::sin(Angle);
        const double Inv = 1. / (Plus + Minus * Cosine + Beta);
        B0 = A * (Plus - Minus * Cosine + Beta) * Inv;
        B1 = 2. * A * (Minus - Plus * Cosine) * Inv;
        B2 = A * (Plus - Minus * Cosine - Beta) * Inv;
        A1 = -2. * (Minus + Plus * Cosine) * Inv;
        A2 = (Plus + Minus * Cosine - Beta) * Inv;
    }
    float Sample(int32 Ch,float X)
    {
        const double Y=B0*X+B1*X1[Ch]+B2*X2[Ch]-A1*Y1[Ch]-A2*Y2[Ch];
        X2[Ch]=X1[Ch];X1[Ch]=X;Y2[Ch]=Y1[Ch];Y1[Ch]=Y;
        return float(Y);
    }
    bool HasTail() const { return std::abs(Y1[0])+std::abs(Y2[0])+std::abs(Y1[1])+std::abs(Y2[1])>1.e-9; }
};
// The half-sample interpolation phase of a Blackman-windowed half-band FIR.
// Even samples take the matched 64-frame delay in both rate conversions.
struct FShape2x
{
    struct FHistory{float Input[256]={},Odd[256]={},Even[128]={};int32 Head=0;};
    float Kernel[128];
    FHistory History[2];
    FShape2x()
    {
        for(int32 i=0;i<128;++i)
        {
            const double X=(i+.5)/128, Z=Pi*(i-63.5);
            Kernel[i]=float(std::sin(Z)/Z*(.42-.5*std::cos(2*Pi*X)+.08*std::cos(4*Pi*X)));
        }
    }
    float Dot(const float* Values) const
    {
        float Sum=0;
        for(int32 i=0;i<64;++i) Sum+=Kernel[i]*(Values[i]+Values[127-i]);
        return Sum;
    }
    float Process(int32 Channel,float X,float Drive,float Scale)
    {
        auto& H=History[Channel];const int32 Pos=H.Head;
        H.Input[Pos]=H.Input[Pos+128]=X;
        auto Shape=[Drive,Scale](float V){return float(std::tanh(Drive*FMath::Clamp(V,-1.f,1.f))*Scale);};
        const float Out=.5f*(H.Even[(Pos+64)&127]+Dot(H.Odd+Pos));
        H.Even[Pos]=Shape(H.Input[(Pos+64)&127]);
        H.Odd[Pos]=H.Odd[Pos+128]=Shape(Dot(H.Input+Pos+1));
        H.Head=(Pos+1)&127;
        return Out;
    }
};

struct FSendBlock
{
    alignas(16) float Taps[2][2][Block];
};

struct FState
{
    alignas(16) float Audio[2][Block]={};
    FBiquad Filter;
    int64 Begin=0,End=0;
    double ElapsedAtFirstFrame=0;
    double MappedControl=0.;
    double Phase=0,Read=0;
    int32 DelayPos=0,DelayTail=0,Channels=1;
    int32 GrainCursor = 0;
    TArray<float> Delay[2];
    TUniquePtr<DSFXCompressor::sf_compressor_state_st> Comp;
    TUniquePtr<Audio::IConvolutionAlgorithm> Room;
    TUniquePtr<FShape2x> Oversampler;
    TUniquePtr<FSendBlock> Sends;
    bool Active=false;
};
inline double Blep(double T,double D)
{
    if (D<=0) return 0;
    if (T<D) { T/=D; return 2*T-T*T-1; }
    if (T>1-D) { T=(T-1)/D; return T*T+2*T+1; }
    return 0;
}
inline float Oscillator(EWave Wave,double Phase,double Step)
{
    switch (Wave)
    {
    case EWave::Sine: return float(std::sin(2*Pi*Phase));
    case EWave::Triangle: return float(1-4*std::abs((Phase<.75?Phase+.25:Phase-.75)-.5));
    case EWave::Sawtooth:
    {
        const double T=Phase<.5?Phase+.5:Phase-.5;
        return float(.85*(2*T-1-Blep(T,Step)));
    }
    default:
        return float(.85*((Phase<.5?1.:-1.)+Blep(Phase,Step)-Blep(Phase<.5?Phase+.5:Phase-.5,Step)));
    }
}
}
