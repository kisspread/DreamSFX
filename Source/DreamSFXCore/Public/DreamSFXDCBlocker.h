#pragma once
#include "CoreMinimal.h"
/** Fixed20Hz pole DC blocker. Setup computes coefficient; processing no IO/alloc. */
struct FDreamSFXDCBlocker
{
 double Pole=0,PreviousInput[2]={0,0},PreviousOutput[2]={0,0};
 void Prepare(int32 Rate){Pole=FMath::Exp(-2.0*PI*20.0/Rate);Reset();}
 void Reset(){PreviousInput[0]=PreviousInput[1]=PreviousOutput[0]=PreviousOutput[1]=0;}
 float Process(float Input,int32 Channel)
 {
  const double Output=double(Input)-PreviousInput[Channel]+Pole*PreviousOutput[Channel];PreviousInput[Channel]=Input;PreviousOutput[Channel]=FMath::IsFinite(Output)?Output:0;return float(PreviousOutput[Channel]);
 }
};
