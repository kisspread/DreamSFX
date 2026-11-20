#include "DreamSFXNodeRegistry.h"

namespace DreamSFX::V2
{
    namespace
    {
        using namespace DSFXGraphDSP;

        struct FGrain
        {
            int64 Start;
            int64 End;
            double ReadStep;
            double EnvelopeStep;
            float Left;
            float Right;
        };

        double OptionalNumber(const FNode& Node, FName Name, double Default)
        {
            const FValue* Value = Node.Parameters.Find(Name);
            return Value ? Value->Get<double>() : Default;
        }

        FKernel BindGrains(const FKernelSetup& Setup)
        {
            const auto* Buffer = &Setup.Buffer(TEXT("buffer")).Channel[0];
            uint32 Seed = uint32(Setup.Number(TEXT("seed")));
            uint32 FilterSeed = Seed ^ 0x9e3779b9u;
            auto Random = [&Seed]()
            {
                Seed = Seed * 1664525u + 1013904223u;
                return double(Seed) / 4294967296.;
            };
            const double Density = Setup.Number(TEXT("densityHz"));
            const double LengthSeconds = Setup.Number(TEXT("grainSeconds"));
            const double DecayDb = Setup.Number(TEXT("decayDb"));
            const double MinRate = Setup.Number(TEXT("minRate"));
            const double MaxRate = Setup.Number(TEXT("maxRate"));
            const double PanSpread = Setup.Number(TEXT("panSpread"));
            const double Gain = Setup.Number(TEXT("gain"));
            const double GainJitter = Setup.Number(TEXT("gainJitter"));
            const int64 Start = Setup.Signal.Begin;
            const int64 Stop = Setup.Signal.End;
            const double MinBandpass = OptionalNumber(Setup.Node, TEXT("minBandpassHz"), 0.);
            const double MaxBandpass = OptionalNumber(Setup.Node, TEXT("maxBandpassHz"), 0.);
            const float BandpassQ = float(OptionalNumber(Setup.Node, TEXT("bandpassQ"), 1.));
            if (MinRate > MaxRate) return {};

            // One seeded onset in each density interval. Build the bounded schedule in
            // Prepare, so overlapping grains do not allocate on the audio thread.
            const int32 Slots = FMath::CeilToInt(double(Stop - Start) * Density / Setup.Rate);
            TArray<FGrain> Grains;
            TArray<FBiquad> Filters;
            Grains.Reserve(Slots);
            if (MaxBandpass > 0.) Filters.Reserve(Slots);
            int64 LastEnd = Start;
            for (int32 Slot = 0; Slot < Slots; ++Slot)
            {
                const int64 At = Start + FMath::RoundToInt64((Slot + Random()) * Setup.Rate / Density);
                if (At >= Stop) break;
                const double Speed = FMath::Lerp(MinRate, MaxRate, Random());
                const int32 Count = FMath::Max(1, FMath::CeilToInt(LengthSeconds * Setup.Rate / Speed));
                const double Angle = (1. + (2. * Random() - 1.) * PanSpread) * Pi * .25;
                const double Level = Gain * (1. - GainJitter * Random());
                const double Decay = std::pow(10., DecayDb / (20. * FMath::Max(1, Count - 1)));
                Grains.Add({At, At + Count, Speed * ReferenceRate / Setup.Rate, Decay,
                    float(Level * std::cos(Angle)), float(Level * std::sin(Angle))});
                LastEnd = FMath::Max(LastEnd, At + Count);
                if (MaxBandpass > 0.)
                {
                    // A separate random stream keeps onset, rate, gain and pan stable.
                    FilterSeed = FilterSeed * 1664525u + 1013904223u;
                    const float Cutoff = float(FMath::Lerp(MinBandpass, MaxBandpass, double(FilterSeed) / 4294967296.));
                    Filters.AddDefaulted_GetRef().Set(EFilter::Bandpass, Cutoff, BandpassQ, Setup.Rate);
                }
            }
            Setup.Signal.End = LastEnd;

            return {[Buffer, Grains = MoveTemp(Grains), Filters = MoveTemp(Filters)](FKernelContext& Context) mutable
            {
                auto& Signal = Context.Signal;
                const int64 BlockEnd = Context.BlockStart + Context.Last;
                int32& Cursor = Signal.GrainCursor;
                while (Cursor < Grains.Num() && Grains[Cursor].End <= Context.BlockStart) ++Cursor;
                for (int32 Index = Cursor; Index < Grains.Num() && Grains[Index].Start < BlockEnd; ++Index)
                {
                    const auto& Grain = Grains[Index];
                    const int64 First = FMath::Max(Context.BlockStart, Grain.Start);
                    const int64 End = FMath::Min(BlockEnd, Grain.End);
                    if (First >= End) continue;
                    const int64 Offset = First - Grain.Start;
                    FBiquad* Filter = Filters.IsEmpty() ? nullptr : &Filters[Index];
                    if (Filter && Offset == 0)
                    {
                        Filter->X1[0] = Filter->X2[0] = Filter->Y1[0] = Filter->Y2[0] = 0.;
                    }
                    double Position = std::fmod(Offset * Grain.ReadStep, double(Buffer->Num()));
                    double Decay = std::pow(Grain.EnvelopeStep, double(Offset));
                    for (int64 At = First; At < End; ++At)
                    {
                        const int32 A = int32(Position);
                        const int32 B = A + 1 == Buffer->Num() ? 0 : A + 1;
                        float Sample = FMath::Lerp((*Buffer)[A], (*Buffer)[B], float(Position - A)) * float(Decay);
                        if (Filter) Sample = Filter->Sample(0, Sample);
                        const int32 Frame = int32(At - Context.BlockStart);
                        Signal.Audio[0][Frame] += Sample * Grain.Left;
                        Signal.Audio[1][Frame] += Sample * Grain.Right;
                        Position += Grain.ReadStep;
                        if (Position >= Buffer->Num()) Position = std::fmod(Position, double(Buffer->Num()));
                        Decay *= Grain.EnvelopeStep;
                    }
                }
            }, [](FState& Signal) { Signal.GrainCursor = 0; }};
        }
    }

    const FNodeSpec& GrainNodeType()
    {
        static const FNodeSpec Spec{.Type = {TEXT("source.grain"), 1}, .bSource = true, .bStereoOutput = true,
            .Properties = {{TEXT("buffer"), EPropertyKind::NoiseBuffer}, {TEXT("seed"), EPropertyKind::Integer, 0, MAX_uint32},
             {TEXT("densityHz"), EPropertyKind::Number, .1, 256.}, {TEXT("grainSeconds"), EPropertyKind::Number, .001, .25},
             {TEXT("decayDb"), EPropertyKind::Number, -96., -1.},
             {TEXT("minRate"), EPropertyKind::Number, .25, 8.}, {TEXT("maxRate"), EPropertyKind::Number, .25, 8.},
             {TEXT("gain"), EPropertyKind::Number, 0., 1.}, {TEXT("gainJitter"), EPropertyKind::Number, 0., 1.},
             {TEXT("panSpread"), EPropertyKind::Number, 0., 1.},
             {TEXT("minBandpassHz"), EPropertyKind::Number, 0., 20000., 0.},
             {TEXT("maxBandpassHz"), EPropertyKind::Number, 0., 20000., 0.},
             {TEXT("bandpassQ"), EPropertyKind::Number, .1, 50., 1.}}, .Bind = &BindGrains, .MaxInstances = 8,
            .ValidateParameters = [](const FNode& Node) -> FParameterIssue
            {
                if (Node.Parameters.FindChecked(TEXT("minRate")).Get<double>() > Node.Parameters.FindChecked(TEXT("maxRate")).Get<double>())
                    return {TEXT("maxRate"), TEXT("maxRate must be greater than or equal to minRate")};
                const double MinBandpass = Node.Parameters.FindChecked(TEXT("minBandpassHz")).Get<double>();
                const double MaxBandpass = Node.Parameters.FindChecked(TEXT("maxBandpassHz")).Get<double>();
                if (MaxBandpass > 0. && MinBandpass == 0.)
                    return {TEXT("minBandpassHz"), TEXT("Specify a positive minimum when per-grain band-pass is enabled")};
                if (MaxBandpass < MinBandpass)
                    return {TEXT("maxBandpassHz"), TEXT("maxBandpassHz must be greater than or equal to minBandpassHz")};
                return {};
            }};
        return Spec;
    }
}
