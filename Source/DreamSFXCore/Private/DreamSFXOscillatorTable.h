#pragma once
#include "CoreMinimal.h"

/** Prepared before rendering; cubic periodic sine lookup for modulated modal voices. */
namespace DreamSFXOscillatorTable
{
    void Initialize();
    double SinCycles(double Phase);
    const float* GetBandLimitedSquare();
    float SquareCycles(const float* Tables, double Phase, double Step);
}
