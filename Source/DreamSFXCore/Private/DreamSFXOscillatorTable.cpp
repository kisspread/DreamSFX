#include "DreamSFXOscillatorTable.h"
#include <cmath>

namespace
{
    constexpr int32 SineTableLength=2048,SineTableMask=SineTableLength-1;
    double GSourceSineSamples[SineTableLength];
    bool GSourceSinePrepared=false;

    constexpr int32 SquareLength = 4096;
    constexpr int32 SquareMask = SquareLength - 1;
    constexpr int32 BandsPerOctave = 3;
    constexpr int32 SquareBands = 12 * BandsPerOctave;

    struct FSquareTables
    {
        float Samples[SquareBands * SquareLength];

        FSquareTables()
        {
            double Sine[SquareLength];
            for (int32 I = 0; I < SquareLength; ++I)
                Sine[I] = std::sin(2.0 * UE_DOUBLE_PI * I / SquareLength);

            double Sum[SquareLength / 2] = {};
            int32 Harmonic = 1;
            // Start with the narrowest band and retain its partials in wider bands.
            for (int32 Band = SquareBands - 1; Band >= 0; --Band)
            {
                const double Fraction = std::exp2(-double(Band) / BandsPerOctave);
                const int32 Limit = FMath::Min(SquareLength / 2 - 1, int32(Fraction * (SquareLength / 2)));
                for (; Harmonic <= Limit; Harmonic += 2)
                {
                    const double Amplitude = 1.0 / Harmonic;
                    for (int32 I = 0; I < SquareLength / 2; ++I)
                        Sum[I] += Amplitude * Sine[(I * Harmonic) & SquareMask];
                }
                float* Wave = Samples + Band * SquareLength;
                for (int32 I = 0; I < SquareLength / 2; ++I)
                {
                    Wave[I] = float(Sum[I]);
                    Wave[I + SquareLength / 2] = -Wave[I];
                }
            }
            float Peak = 0.f;
            for (int32 I = 0; I < SquareLength; ++I) Peak = FMath::Max(Peak, FMath::Abs(Samples[I]));
            const float Scale = 1.f / Peak;
            for (float& Sample : Samples) Sample *= Scale;
        }
    };
}
void DreamSFXOscillatorTable::Initialize()
{
    if(GSourceSinePrepared)return;
    for(int32 Index=0;Index<SineTableLength;++Index)GSourceSineSamples[Index]=FMath::Sin(2.0*UE_DOUBLE_PI*Index/SineTableLength);
    GSourceSinePrepared=true;
}
double DreamSFXOscillatorTable::SinCycles(double Phase)
{
    const double Position=Phase*SineTableLength;
    const int32 Index=int32(Position);
    const double T=Position-Index;
    const double A=GSourceSineSamples[(Index-1)&SineTableMask],B=GSourceSineSamples[Index&SineTableMask];
    const double C=GSourceSineSamples[(Index+1)&SineTableMask],D=GSourceSineSamples[(Index+2)&SineTableMask];
    return B+.5*T*(C-A+T*(2*A-5*B+4*C-D+T*(3*(B-C)+D-A)));
}

const float* DreamSFXOscillatorTable::GetBandLimitedSquare()
{
    static const FSquareTables Tables;
    return Tables.Samples;
}

float DreamSFXOscillatorTable::SquareCycles(const float* Tables, double Phase, double Step)
{
    // Guard the Nyquist limit by crossfading adjacent harmonic cutoffs.
    const double Band = Step > 0.0 ? FMath::Clamp(1.0 + BandsPerOctave * std::log2(Step * SquareLength),
        0.0, double(SquareBands - 1)) : 0.0;
    const int32 Lower = int32(Band);
    const int32 Upper = FMath::Min(Lower + 1, SquareBands - 1);
    const double Position = Phase * SquareLength;
    const int32 Index = int32(Position);
    const float Fraction = float(Position - Index);
    const auto Read = [&](int32 Row)
    {
        const float* Wave = Tables + Row * SquareLength;
        return FMath::Lerp(Wave[Index & SquareMask], Wave[(Index + 1) & SquareMask], Fraction);
    };
    return FMath::Lerp(Read(Lower), Read(Upper), float(Band - Lower));
}
