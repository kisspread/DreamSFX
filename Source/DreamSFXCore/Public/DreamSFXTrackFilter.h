#pragma once
#include "CoreMinimal.h"

/** Stable matched one-pole low-pass. Cutoffs at/above Nyquist are explicitly open. */
struct FDreamSFXTrackFilter
{
    double State[2]={0,0},Feedback=0;
    float LastCutoff=-1;
    int32 Rate=48000;
    bool Open=true;
    void Prepare(int32 SampleRate){Rate=SampleRate;Reset();}
    void Reset(){State[0]=State[1]=0;LastCutoff=-1;Feedback=0;Open=true;}
    void SetCutoff(float Hz)
    {
        if(Hz==LastCutoff)return;
        LastCutoff=Hz;Open=Hz>=Rate*.5;
        Feedback=Open?0:FMath::Exp(-2.0*UE_DOUBLE_PI*Hz/Rate);
    }
    float Process(float Input,int32 Channel)
    {
        const double Safe=FMath::IsFinite(Input)?Input:0;
        State[Channel]=Open?Safe:(1-Feedback)*Safe+Feedback*State[Channel];
        return float(State[Channel]);
    }
};
