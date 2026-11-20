#include "DreamSFXNodeRegistry.h"
#include "DreamSFXProgramRandom.h"

namespace DreamSFX::V2
{
    using DSFXGraphDSP::ERecipe;

    const FNodeSpec* BufferParameterSpec(ERecipe Recipe)
    {
        using P = EPropertyKind;
        const static FPropertySpec Random{TEXT("random"), P::Choice, 0., 0., {}, {TEXT("mulberry32"), TEXT("lcg32")}};
        const static FNodeSpec Brown{.Type = {TEXT("buffer.brown_loop"), 1},
            .Properties = {Random, {TEXT("crossfadeFrames"), P::Integer, 0., 65536.},
                {TEXT("step"), P::Number, .00001, 1.}, {TEXT("level"), P::Number, 0., 16.}}};
        const static FNodeSpec ThreePole{.Type = {TEXT("buffer.three_pole_loop"), 1},
            .Properties = {Random, {TEXT("crossfadeFrames"), P::Integer, 0., 65536.},
                {TEXT("slowPole"), P::Number, 0., .999999}, {TEXT("slowInput"), P::Number, -4., 4.},
                {TEXT("midPole"), P::Number, 0., .999999}, {TEXT("midInput"), P::Number, -4., 4.},
                {TEXT("fastPole"), P::Number, 0., .999999}, {TEXT("fastInput"), P::Number, -4., 4.},
                {TEXT("direct"), P::Number, -4., 4.}, {TEXT("level"), P::Number, 0., 16.}}};
        const static FNodeSpec Impulse{.Type = {TEXT("impulse.diffuse"), 1},
            .Properties = {Random, {TEXT("dampingFloor"), P::Number, 0., 1.}, {TEXT("dampingAmount"), P::Number, 0., 1.},
                {TEXT("dampingDecayPerSecond"), P::Number, 0., 100.}, {TEXT("attackSeconds"), P::Number, .00001, 10.},
                {TEXT("decaySeconds"), P::Number, .00001, 60.}, {TEXT("taperEnd"), P::Flag}}};
        if (Recipe == ERecipe::BrownLoop) return &Brown;
        if (Recipe == ERecipe::ThreePoleLoop) return &ThreePole;
        if (Recipe == ERecipe::DiffuseImpulse) return &Impulse;
        return nullptr;
    }

    bool ValidBufferParameters(const FGeneratedBuffer& Resource, ERecipe Recipe)
    {
        const auto* Spec = BufferParameterSpec(Recipe);
        if (!Spec) return Resource.Parameters.IsEmpty();
        if (Spec->Properties.Num() != Resource.Parameters.Num()) return false;
        for (const auto& Field : Spec->Properties)
        {
            const auto* V = Resource.Parameters.Find(Field.Name);
            if (!V) return false;
            if (Field.Kind == EPropertyKind::Choice)
            {
                if (!V->IsType<FName>() || !Field.Choices.Contains(V->Get<FName>())) return false;
            }
            else if (Field.Kind == EPropertyKind::Flag)
            {
                if (!V->IsType<bool>()) return false;
            }
            else
            {
                if (!V->IsType<double>()) return false;
                const double N = V->Get<double>();
                if (!FMath::IsFinite(N) || N < Field.Minimum || N > Field.Maximum ||
                    (Field.Kind == EPropertyKind::Integer && FMath::FloorToDouble(N) != N)) return false;
            }
        }
        if (const auto* Crossfade = Resource.Parameters.Find(TEXT("crossfadeFrames")))
            return Crossfade->Get<double>() <= Resource.FramesAt48k;
        return Resource.Parameters.FindChecked(TEXT("dampingFloor")).Get<double>() +
            Resource.Parameters.FindChecked(TEXT("dampingAmount")).Get<double>() <= 1.;
    }

    DSFXGraphDSP::FGeneratedBuffer PrepareBuffer(const FGeneratedBuffer& Source, ERecipe Recipe)
    {
        if (!BufferParameterSpec(Recipe)) return DSFXGraphDSP::Generate(Recipe, Source.Seed, Source.FramesAt48k, Source.Channels);
        const int32 Frames = Source.FramesAt48k;
        const auto& Params = Source.Parameters;
        const auto Number = [&](FName Key) { return Params.FindChecked(Key).Get<double>(); };
        const auto Algorithm = Params.FindChecked(TEXT("random")).Get<FName>() == TEXT("mulberry32")
            ? EProgramRandom::Mulberry32 : EProgramRandom::Lcg32;
        uint32 Seed = Source.Seed;
        const auto White = [&]()
        {
            return double(DrawRandomBits(Seed, Algorithm)) / 2147483648. - 1.;
        };
        DSFXGraphDSP::FGeneratedBuffer Out;
        for (auto& Channel : Out.Channel)
        {
            Channel.SetNumUninitialized(Frames);
            double State = 0.;
            const auto FillLoop = [&](auto Next)
            {
                const int32 Overlap = int32(Number(TEXT("crossfadeFrames")));
                for (int32 I = 0; I < Frames + Overlap; ++I)
                {
                    const double Sample = Next(White());
                    if (I < Frames) Channel[I] = float(Sample);
                    else
                    {
                        const int32 Head = I - Frames;
                        const double Mix = double(Head) / Overlap;
                        Channel[Head] = float(Channel[Head] * Mix + Sample * (1. - Mix));
                    }
                }
            };
            if (Recipe == ERecipe::BrownLoop)
            {
                const double Step = Number(TEXT("step")), Level = Number(TEXT("level"));
                FillLoop([&](double Input)
                {
                    State = (State + Step * Input) / (1. + Step);
                    return State * Level;
                });
            }
            else if (Recipe == ERecipe::ThreePoleLoop)
            {
                const double SlowPole = Number(TEXT("slowPole")), SlowInput = Number(TEXT("slowInput"));
                const double MidPole = Number(TEXT("midPole")), MidInput = Number(TEXT("midInput"));
                const double FastPole = Number(TEXT("fastPole")), FastInput = Number(TEXT("fastInput"));
                const double Direct = Number(TEXT("direct")), Level = Number(TEXT("level"));
                double Slow = 0., Mid = 0., Fast = 0.;
                FillLoop([&](double Input)
                {
                    Slow = SlowPole * Slow + SlowInput * Input;
                    Mid = MidPole * Mid + MidInput * Input;
                    Fast = FastPole * Fast + FastInput * Input;
                    return (Slow + Mid + Fast + Direct * Input) * Level;
                });
            }
            else
            {
                const double Floor = Number(TEXT("dampingFloor")), Amount = Number(TEXT("dampingAmount"));
                const double TiltRate = Number(TEXT("dampingDecayPerSecond"));
                const double Attack = Number(TEXT("attackSeconds")), Decay = Number(TEXT("decaySeconds"));
                const bool Taper = Params.FindChecked(TEXT("taperEnd")).Get<bool>();
                for (int32 I = 0; I < Frames; ++I)
                {
                    const double Time = I / DSFXGraphDSP::ReferenceRate;
                    State += (White() - State) * (Floor + Amount * std::exp(-Time * TiltRate));
                    const double End = Taper ? 1. - double(I) / Frames : 1.;
                    Channel[I] = float(State * (1. - std::exp(-Time / Attack)) * std::exp(-Time / Decay) * End);
                }
            }
        }
        return Out;
    }
}
