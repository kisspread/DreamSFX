#include "DreamSFXNodeRegistry.h"
#include "DreamSFXOscillatorTable.h"

namespace DreamSFX::V2
{
    using namespace DSFXGraphDSP;

    float FKernelContext::Control(int32 Slot, int32 Frame) const
    {
        const FControlState& Parameter = Controls[Slot];
        float Value = Parameter.bProgramDriven || !Parameter.Curve.Spans.IsEmpty() ? Parameter.Curve.Values[Frame] : Parameter.Base;
        for (int32 Index : Parameter.Modulators)
        {
            const auto& Source = Signals[Index];
            if (Source.Active)
            {
                Value += .5f * (Source.Audio[0][Frame] + Source.Audio[1][Frame]);
            }
        }
        return Value;
    }

    static FKernel BindControlValue(const FKernelSetup& Setup)
    {
        return {[Value = Setup.Control(TEXT("value"))](FKernelContext& C)
        {
            for (int32 I = 0; I < Block; ++I)
                C.Signal.Audio[0][I] = C.Signal.Audio[1][I] = C.Control(Value, I);
        }};
    }

    static FKernel BindControlResponse(const FKernelSetup& Setup)
    {
        const double Min = Setup.Number(TEXT("inputMin")), Max = Setup.Number(TEXT("inputMax"));
        if (Max <= Min) return {};
        const double Seconds = Setup.Number(TEXT("timeConstantSeconds"));
        return {[Value = Setup.Control(TEXT("value")), Enabled = Setup.Control(TEXT("enabled")), Min, Max,
            Low = Setup.Number(TEXT("outputMin")), High = Setup.Number(TEXT("outputMax")),
            Power = Setup.Number(TEXT("exponent")), Alpha = Seconds > 0. ? -std::expm1(-1. / (Setup.Rate * Seconds)) : 1.](FKernelContext& C)
        {
            double& State = C.Signal.MappedControl;
            for (int32 I = 0; I < Block; ++I)
            {
                const bool Follow = C.Control(Enabled, I) > .5f;
                const double T = FMath::Clamp((C.Control(Value, I) - Min) / (Max - Min), 0., 1.);
                const double Target = FMath::Lerp(Low, High, Power == 1. ? T : std::pow(T, Power));
                if (Follow && Alpha == 1.) State = Target;
                C.Signal.Audio[0][I] = C.Signal.Audio[1][I] = float(State);
                if (Follow) State += Alpha * (Target - State);
            }
        }, [Initial = Setup.Number(TEXT("initial"))](FState& S) { S.MappedControl = Initial; }};
    }

    static FKernel BindGain(const FKernelSetup& Setup)
    {
        return {[Level = Setup.Control(TEXT("level"))](FKernelContext& C)
        {
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                const float Gain = C.Control(Level, Frame);
                C.Signal.Audio[0][Frame] *= Gain;
                C.Signal.Audio[1][Frame] *= Gain;
            }
        }};
    }

    static FKernel BindSend(const FKernelSetup& Setup)
    {
        Setup.Signal.Sends = MakeUnique<FSendBlock>();
        return {[A = Setup.Control(TEXT("auxAGain")), B = Setup.Control(TEXT("auxBGain"))](FKernelContext& Context)
        {
            const auto& Audio = Context.Signal.Audio;
            auto& Taps = Context.Signal.Sends->Taps;
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                const float GainA = Context.Control(A, Frame);
                const float GainB = Context.Control(B, Frame);
                for (int32 Channel = 0; Channel < 2; ++Channel)
                {
                    Taps[0][Channel][Frame] = Audio[Channel][Frame] * GainA;
                    Taps[1][Channel][Frame] = Audio[Channel][Frame] * GainB;
                }
            }
        }};
    }

    template<EWave Wave, bool bPeriodicSquare = false>
    static FKernel BindOscillator(const FKernelSetup& Setup)
    {
        const auto* StartAtZero = Setup.Node.Parameters.Find(TEXT("startAtZero"));
        const bool bStartAtZero = StartAtZero && StartAtZero->Get<bool>();
        const auto* FrequencyClock = Setup.Node.Parameters.Find(TEXT("frequencyFromBlockStart"));
        const bool bFrequencyFromBlockStart = FrequencyClock && FrequencyClock->Get<bool>();
        const float* SquareTables = bPeriodicSquare ? DreamSFXOscillatorTable::GetBandLimitedSquare() : nullptr;
        return {[Frequency = Setup.Control(TEXT("frequencyHz")), Detune = Setup.Control(TEXT("detuneCents")),
            bStartAtZero,bFrequencyFromBlockStart,SquareTables](FKernelContext& C)
        {
            auto& Signal = C.Signal;
            const auto StepAt = [&](int32 Frame)
            {
                const double Cents = FMath::Clamp(double(C.Control(Detune, Frame)), -9600., 9600.);
                // An automated browser oscillator starts at offset zero in the current
                // control quantum. Later quanta resume the global parameter clock.
                const int32 FrequencyFrame = bFrequencyFromBlockStart ? FMath::Max(C.First, Frame - C.First) : Frame;
                const double Hz = FMath::Clamp(C.Control(Frequency, FrequencyFrame) * std::exp2(Cents / 1200), 0., C.Rate * .499);
                return Hz / C.Rate;
            };
            if (const double Elapsed = C.StartingFraction(); Elapsed > 0. && !bStartAtZero)
                Signal.Phase = Elapsed * StepAt(C.First);
            for (int32 Frame = C.First; Frame < C.Last; ++Frame)
            {
                const double Step = StepAt(Frame);
                const float Sample = bPeriodicSquare ? DreamSFXOscillatorTable::SquareCycles(SquareTables, Signal.Phase, Step)
                    : Oscillator(Wave, Signal.Phase, Step);
                Signal.Audio[0][Frame] = Signal.Audio[1][Frame] = Sample;
                Signal.Phase += Step;
                if (Signal.Phase >= 1) Signal.Phase -= 1;
            }
        }};
    }

    static FKernel BindSineModulator(const FKernelSetup& Setup)
    {
        return {[Frequency = Setup.Control(TEXT("frequencyHz")), Amplitude = Setup.Control(TEXT("amplitude")),
            Offset = Setup.Control(TEXT("offset"))](FKernelContext& Context)
        {
            auto& Signal = Context.Signal;
            if (const double Elapsed = Context.StartingFraction(); Elapsed > 0.)
            {
                const double Hz = FMath::Clamp(double(Context.Control(Frequency, Context.First)), 0., Context.Rate * .499);
                Signal.Phase = std::fmod(Signal.Phase + Elapsed * Hz / Context.Rate, 1.);
            }
            for (int32 Frame = Context.First; Frame < Context.Last; ++Frame)
            {
                const double Hz = FMath::Clamp(double(Context.Control(Frequency, Frame)), 0., Context.Rate * .499);
                const float Wave = Oscillator(EWave::Sine, Signal.Phase, Hz / Context.Rate);
                const float Value = Context.Control(Offset, Frame) + Wave * Context.Control(Amplitude, Frame);
                Signal.Audio[0][Frame] = Signal.Audio[1][Frame] = Value;
                Signal.Phase += Hz / Context.Rate;
                if (Signal.Phase >= 1.) Signal.Phase -= 1.;
            }
        }, [Phase = Setup.Number(TEXT("phaseCycles"))](FState& Signal)
        {
            Signal.Phase = Phase == 1. ? 0. : Phase;
        }};
    }

    template<bool Mono>
    static FKernel BindNoise(const FKernelSetup& Setup)
    {
        const auto* Buffer = &Setup.Buffer(TEXT("buffer"));
        const auto* RateHold = Setup.Node.Parameters.Find(TEXT("rateHoldFrames"));
        const bool bHoldRate = RateHold && RateHold->Get<double>() == Block;
        return {[Buffer, bHoldRate, Rate = Setup.Control(TEXT("playbackRate"))](FKernelContext& C)
        {
            auto& Signal = C.Signal;
            const int32 Length = Buffer->Channel[0].Num();
            const auto ReadStep = [&](int32 Frame)
            {
                return FMath::Clamp(double(C.Control(Rate, bHoldRate ? 0 : Frame)), .001, 32.) * ReferenceRate / C.Rate;
            };
            if (const double Elapsed = C.StartingFraction(); Elapsed > 0.)
                Signal.Read += Elapsed * ReadStep(C.First);
            for (int32 Frame = C.First; Frame < C.Last; ++Frame)
            {
                if (Signal.Read >= Length) Signal.Read = std::fmod(Signal.Read, Length);
                const int32 Base = int32(Signal.Read);
                const int32 Next = Base + 1 == Length ? 0 : Base + 1;
                for (int32 Channel = 0; Channel < 2; ++Channel)
                {
                    const auto& Samples = Buffer->Channel[Mono ? 0 : Channel];
                    Signal.Audio[Channel][Frame] = FMath::Lerp(Samples[Base], Samples[Next], float(Signal.Read - Base));
                }
                Signal.Read += ReadStep(Frame);
            }
        }, [Offset = FMath::RoundToDouble(Setup.Number(TEXT("offsetSeconds")) * ReferenceRate)](FState& Signal)
        {
            Signal.Read = Offset;
        }};
    }

    // A finite mono grain reads a generated resource once. Apply its decay to
    // the buffer samples before interpolation, so transposition also scales the decay.
    static FKernel BindSingleGrain(const FKernelSetup& Setup)
    {
        const auto* Samples = &Setup.Buffer(TEXT("buffer")).Channel[0];
        const double Slope = -1. / (ReferenceRate * Setup.Number(TEXT("decaySeconds")));
        const double NextEnvelope = std::exp(Slope);
        return {[Samples, Slope, NextEnvelope, Rate = Setup.Control(TEXT("playbackRate"))](FKernelContext& Context)
        {
            FState& Signal = Context.Signal;
            if (const double Elapsed = Context.StartingFraction(); Elapsed > 0.)
                Signal.Read = Elapsed * FMath::Clamp(double(Context.Control(Rate, Context.First)), .001, 32.) * ReferenceRate / Context.Rate;
            for (int32 Frame = Context.First; Frame < Context.Last && Signal.Read < Samples->Num(); ++Frame)
            {
                const int32 Base = int32(Signal.Read);
                const double Envelope = std::exp(Slope * Base);
                const float A = float((*Samples)[Base] * Envelope);
                const float B = Base + 1 < Samples->Num() ? float((*Samples)[Base + 1] * Envelope * NextEnvelope) : 0.f;
                const float Value = FMath::Lerp(A, B, float(Signal.Read - Base));
                Signal.Audio[0][Frame] = Signal.Audio[1][Frame] = Value;
                Signal.Read += FMath::Clamp(double(Context.Control(Rate, Frame)), .001, 32.) * ReferenceRate / Context.Rate;
            }
        }};
    }

    template<EFilter Filter>
    static FKernel BindBiquad(const FKernelSetup& Setup)
    {
        const FName Resonance = Filter == EFilter::Bandpass ? TEXT("q") : TEXT("resonanceDb");
        return {[Frequency = Setup.Control(TEXT("cutoffHz")), Q = Setup.Control(Resonance)](FKernelContext& C)
        {
            auto& Signal = C.Signal;
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                Signal.Filter.Set(Filter, C.Control(Frequency, Frame), C.Control(Q, Frame), C.Rate);
                for (int32 Channel = 0; Channel < 2; ++Channel)
                {
                    Signal.Audio[Channel][Frame] = Signal.Filter.Sample(Channel, Signal.Audio[Channel][Frame]);
                }
            }
        }};
    }

    static FKernel BindLowShelf(const FKernelSetup& Setup)
    {
        return {[Frequency = Setup.Control(TEXT("cutoffHz")), Gain = Setup.Control(TEXT("gainDb"))](FKernelContext& C)
        {
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                C.Signal.Filter.SetLowShelf(C.Control(Frequency, Frame), C.Control(Gain, Frame), C.Rate);
                for (int32 Channel = 0; Channel < 2; ++Channel)
                {
                    float& Sample = C.Signal.Audio[Channel][Frame];
                    Sample = C.Signal.Filter.Sample(Channel, Sample);
                }
            }
        }};
    }

    static FKernel BindPan(const FKernelSetup& Setup)
    {
        return {[Position = Setup.Control(TEXT("position")), bStereo = Setup.bStereoInput](FKernelContext& C)
        {
            auto& Audio = C.Signal.Audio;
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                const double Pan = FMath::Clamp(double(C.Control(Position, Frame)), -1., 1.);
                const float Left = Audio[0][Frame], Right = Audio[1][Frame];
                if (!bStereo)
                {
                    const double Angle = (Pan + 1) * Pi * .25;
                    Audio[0][Frame] = float(Left * std::cos(Angle));
                    Audio[1][Frame] = float(Left * std::sin(Angle));
                }
                else if (Pan < 0)
                {
                    const double Angle = (Pan + 1) * Pi * .5;
                    Audio[0][Frame] = float(Left + Right * std::cos(Angle));
                    Audio[1][Frame] = float(Right * std::sin(Angle));
                }
                else
                {
                    const double Angle = Pan * Pi * .5;
                    Audio[0][Frame] = float(Left * std::cos(Angle));
                    Audio[1][Frame] = float(Right + Left * std::sin(Angle));
                }
            }
        }};
    }

    static FKernel BindDelay(const FKernelSetup& Setup)
    {
        for (auto& Channel : Setup.Signal.Delay)
        {
            Channel.SetNumZeroed(int32(Setup.Rate * .05) + 2);
        }
        return {[Time = Setup.Control(TEXT("delaySeconds"))](FKernelContext& C)
        {
            auto& Signal = C.Signal;
            const int32 Size = Signal.Delay[0].Num();
            Signal.DelayTail = C.bHasInput ? Size : FMath::Max(0, Signal.DelayTail - Block);
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                double Read = Signal.DelayPos + Size - FMath::Clamp(double(C.Control(Time, Frame)) * C.Rate, 0., double(Size - 2));
                if (Read >= Size) Read -= Size;
                const int32 Base = int32(Read);
                const int32 Next = Base + 1 == Size ? 0 : Base + 1;
                for (int32 Channel = 0; Channel < 2; ++Channel)
                {
                    Signal.Delay[Channel][Signal.DelayPos] = Signal.Audio[Channel][Frame];
                    Signal.Audio[Channel][Frame] = FMath::Lerp(Signal.Delay[Channel][Base], Signal.Delay[Channel][Next], float(Read - Base));
                }
                if (++Signal.DelayPos == Size) Signal.DelayPos = 0;
            }
        }};
    }

    // Read the preceding history during the schedule. Commit this quantum only after
    // every producer has run, so the buffer can break a feedback cycle without extra delay.
    static FKernel BindCausalDelay(const FKernelSetup& Setup)
    {
        const double DelayFrames = Setup.Number(TEXT("delaySeconds")) * Setup.Rate;
        if (DelayFrames < Block) return {};
        const int32 Capacity = FMath::CeilToInt(DelayFrames) + 2;
        for (auto& Channel : Setup.Signal.Delay) Channel.SetNumZeroed(Capacity);
        FKernel Kernel;
        Kernel.Process = [Capacity, DelayFrames](FKernelContext& Context)
        {
            auto& Signal = Context.Signal;
            double Read = Signal.DelayPos + Capacity - DelayFrames;
            if (Read >= Capacity) Read -= Capacity;
            int32 Index = int32(Read);
            const float Fraction = float(Read - Index);
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                const int32 Next = Index + 1 == Capacity ? 0 : Index + 1;
                for (int32 Channel = 0; Channel < 2; ++Channel)
                {
                    const auto& History = Signal.Delay[Channel];
                    Signal.Audio[Channel][Frame] = FMath::Lerp(History[Index], History[Next], Fraction);
                }
                Index = Next;
            }
        };
        Kernel.CommitDelayedInput = [Capacity](FState& Signal, const float (&Input)[2][Block])
        {
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                for (int32 Channel = 0; Channel < 2; ++Channel)
                    Signal.Delay[Channel][Signal.DelayPos] = Input[Channel][Frame];
                if (++Signal.DelayPos == Capacity) Signal.DelayPos = 0;
            }
        };
        return Kernel;
    }

    static FKernel BindShape(const FKernelSetup& Setup)
    {
        if (Setup.Flag(TEXT("oversample2x")))
        {
            Setup.Signal.Oversampler = MakeUnique<FShape2x>();
        }
        return {[Drive = float(Setup.Number(TEXT("drive"))), Scale = float(Setup.Number(TEXT("scale")))](FKernelContext& C)
        {
            auto& Signal = C.Signal;
            Signal.DelayTail = Signal.Oversampler ? (C.bHasInput ? 2 * Block : FMath::Max(0, Signal.DelayTail - Block)) : 0;
            for (int32 Channel = 0; Channel < 2; ++Channel)
            {
                for (float& Sample : Signal.Audio[Channel])
                {
                    Sample = Signal.Oversampler ? Signal.Oversampler->Process(Channel, Sample, Drive, Scale) :
                        float(std::tanh(Drive * FMath::Clamp(Sample, -1.f, 1.f)) * Scale);
                }
            }
        }};
    }

    static FKernel BindCompressor(const FKernelSetup& Setup)
    {
        Setup.Signal.Comp = MakeUnique<DSFXCompressor::sf_compressor_state_st>();
        return {[](FKernelContext& C)
        {
            DSFXCompressor::sf_sample_st Signal[Block];
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                Signal[Frame] = {C.Signal.Audio[0][Frame], C.Signal.Audio[1][Frame]};
            }
            DSFXCompressor::sf_compressor_process(C.Signal.Comp.Get(), Block, Signal, Signal);
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                C.Signal.Audio[0][Frame] = Signal[Frame].L;
                C.Signal.Audio[1][Frame] = Signal[Frame].R;
            }
        }, [Rate = Setup.Rate, Threshold = FMath::Clamp(Setup.Base(TEXT("thresholdDb")), -100.f, 0.f),
            Knee = FMath::Clamp(Setup.Base(TEXT("kneeDb")), 0.f, 40.f), Ratio = FMath::Clamp(Setup.Base(TEXT("ratio")), 1.f, 20.f),
            Attack = FMath::Clamp(Setup.Base(TEXT("attackSeconds")), .0001f, 1.f),
            Release = FMath::Clamp(Setup.Base(TEXT("releaseSeconds")), .001f, 1.f)](FState& Signal)
        {
            *Signal.Comp = {};
            DSFXCompressor::sf_simplecomp(Signal.Comp.Get(), Rate, 0, Threshold, Knee, Ratio, Attack, Release);
        }};
    }

    static FKernel BindRoomImpulse(const FKernelSetup& Setup, const DSFXGraphDSP::FGeneratedBuffer& Buffer, bool Normalize = true)
    {
        Audio::FConvolutionSettings Settings;
        Settings.BlockNumSamples = Block;
        Settings.NumInputChannels = Settings.NumOutputChannels = Settings.NumImpulseResponses = 2;
        const int32 Length = Buffer.Channel[0].Num();
        Settings.MaxNumImpulseResponseSamples = FMath::Max(1, FMath::RoundToInt(double(Length) * Setup.Rate / ReferenceRate));
        Settings.bEnableHardwareAcceleration = false;
        auto& Room = Setup.Signal.Room;
        Room = Audio::FConvolutionFactory::NewConvolutionAlgorithm(Settings);
        if (!Room) return {};

        double Scale = 1.;
        if (Normalize)
        {
            double Energy = 0;
            for (const auto& Channel : Buffer.Channel)
                for (float Value : Channel) Energy += double(Value) * Value;
            const double Rms = std::sqrt(Energy / (2 * Length));
            Scale = .00125 / FMath::Max(Rms, .000125) * 44100. / Setup.Rate;
        }
        TArray<float> Impulse;
        Impulse.SetNumUninitialized(Settings.MaxNumImpulseResponseSamples);
        for (int32 Channel = 0; Channel < 2; ++Channel)
        {
            const auto& Samples = Buffer.Channel[Channel];
            for (int32 Frame = 0; Frame < Impulse.Num(); ++Frame)
            {
                const double Position = Frame * ReferenceRate / Setup.Rate;
                const int32 Index = FMath::Min(int32(Position), Length - 1);
                Impulse[Frame] = float(Scale * FMath::Lerp(Samples[Index], Samples[FMath::Min(Index + 1, Length - 1)], Position - Index));
            }
            Room->SetImpulseResponse(Channel, Impulse.GetData(), Impulse.Num());
            Room->SetMatrixGain(Channel, Channel, Channel, 1.f);
        }
        return {[](FKernelContext& C)
        {
            auto& Audio = C.Signal.Audio;
            alignas(16) float Wet[2][Block];
            const float* Input[] = {Audio[0], Audio[1]};
            float* Output[] = {Wet[0], Wet[1]};
            C.Signal.Room->ProcessAudioBlock(Input, Output);
            FMemory::Memcpy(Audio, Wet, sizeof(Wet));
        }};
    }

    static FKernel BindRoom(const FKernelSetup& Setup)
    {
        return BindRoomImpulse(Setup, Setup.Buffer(TEXT("impulse")));
    }

    static FKernel BindRoomWithNormalization(const FKernelSetup& Setup)
    {
        return BindRoomImpulse(Setup, Setup.Buffer(TEXT("impulse")), Setup.Flag(TEXT("normalize")));
    }

    static FKernel BindDecayingNoiseRoom(const FKernelSetup& Setup)
    {
        auto Impulse = Setup.Buffer(TEXT("noise"));
        const double Attack = Setup.Number(TEXT("attackSeconds"));
        const double Decay = Setup.Number(TEXT("decaySeconds"));
        for (int32 Frame = 0; Frame < Impulse.Channel[0].Num(); ++Frame)
        {
            const double Time = Frame / ReferenceRate;
            const double Gain = std::exp(-Time / Decay) * (Attack > 0. ? FMath::Min(1., Time / Attack) : 1.);
            for (auto& Channel : Impulse.Channel) Channel[Frame] = float(Channel[Frame] * Gain);
        }
        return BindRoomImpulse(Setup, Impulse);
    }

    const FControlSpec* FNodeSpec::FindControl(FName Name) const
    {
        return Controls.FindByPredicate([Name](const FControlSpec& Port) { return Port.Name == Name; });
    }

    TOptional<EPortKind> FNodeSpec::Input(FName Name) const
    {
        if (bSource)
        {
            switch (EventMode)
            {
            case EEventMode::Gate:
                if (Name == TEXT("start") || Name == TEXT("stop")) return EPortKind::Event;
                break;
            case EEventMode::NotesWithSustain:
                if (Name == TEXT("sustain")) return EPortKind::Event;
                [[fallthrough]];
            case EEventMode::Notes:
                if (Name == TEXT("noteOn") || Name == TEXT("noteOff")) return EPortKind::Event;
                break;
            case EEventMode::Hit:
                if (Name == TEXT("fire")) return EPortKind::Event;
                break;
            }
        }
        if (!bSource && !bControlOnly && Name == TEXT("input")) return EPortKind::Audio;
        if (FindControl(Name)) return EPortKind::Control;
        return {};
    }

    TOptional<EPortKind> FNodeSpec::Output(FName Name) const
    {
        if (Name == TEXT("audio") && !bControlOnly) return EPortKind::Audio;
        if (bHasAuxSends && AudioTapIndex(Name) != INDEX_NONE) return EPortKind::Audio;
        if (Name == TEXT("control")) return EPortKind::Control;
        return {};
    }

    const FNodeSpec* FindNodeType(FNodeTypeId Type)
    {
        static const TArray<FControlSpec> OscillatorControls = {
            {TEXT("frequencyHz"), TEXT("Hz"), 0}, {TEXT("detuneCents"), TEXT("cents")}
        };
        static const TArray<FPropertySpec> OscillatorTiming = {
            {TEXT("startAtZero"), EPropertyKind::Flag},
            {TEXT("frequencyFromBlockStart"), EPropertyKind::Flag, 0., 1., 0.}
        };
        static const TArray<FControlSpec> ResonantFilter = {
            {TEXT("cutoffHz"), TEXT("Hz"), 0}, {TEXT("resonanceDb"), TEXT("dB")}
        };
        static const TArray<FNodeSpec> Types = {
            {.Type = {TEXT("control.value"), 1}, .Controls = {{TEXT("value"), TEXT("scalar")}}, .Bind = &BindControlValue,
                .bProcessSilence = true, .bControlOnly = true},
            {.Type = {TEXT("control.response"), 1}, .Controls = {{TEXT("value"), TEXT("scalar")}, {TEXT("enabled"), TEXT("gate"), 0., 1.}},
                .Properties = {{TEXT("inputMin"), EPropertyKind::Number, -100000., 100000.}, {TEXT("inputMax"), EPropertyKind::Number, -100000., 100000.},
                    {TEXT("outputMin"), EPropertyKind::Number, -100000., 100000.}, {TEXT("outputMax"), EPropertyKind::Number, -100000., 100000.},
                    {TEXT("exponent"), EPropertyKind::Number, .01, 32.}, {TEXT("timeConstantSeconds"), EPropertyKind::Number, 0., 60.},
                    {TEXT("initial"), EPropertyKind::Number, -100000., 100000.}},
                .Bind = &BindControlResponse, .bProcessSilence = true, .bControlOnly = true,
                .ValidateParameters = [](const FNode& N) -> FParameterIssue
                {
                    if (N.Parameters.FindChecked(TEXT("inputMax")).Get<double>() <= N.Parameters.FindChecked(TEXT("inputMin")).Get<double>())
                        return {TEXT("inputMax"), TEXT("inputMax must exceed inputMin")};
                    return {};
                }},
            {{TEXT("mix.gain"), 1}, false, false, {{TEXT("level"), TEXT("linear gain")}}, {}, &BindGain},
            {.Type = {TEXT("mix.send"), 1}, .bSource = false, .bStereoOutput = false,
                .Controls = {{TEXT("auxAGain"), TEXT("linear gain"), 0., 4.}, {TEXT("auxBGain"), TEXT("linear gain"), 0., 4.}},
                .Bind = &BindSend, .bHasAuxSends = true},
            {.Type = {TEXT("osc.sine"), 1}, .bSource = true, .Controls = OscillatorControls,
                .Bind = &BindOscillator<EWave::Sine>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("osc.sine"), 2}, .bSource = true, .Controls = OscillatorControls,
                .Properties = OscillatorTiming,
                .Bind = &BindOscillator<EWave::Sine>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("osc.sawtooth"), 1}, .bSource = true, .Controls = OscillatorControls,
                .Bind = &BindOscillator<EWave::Sawtooth>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("osc.square"), 1}, .bSource = true, .Controls = OscillatorControls,
                .Bind = &BindOscillator<EWave::Square>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("osc.square"), 2}, .bSource = true, .Controls = OscillatorControls,
                .Properties = OscillatorTiming,
                .Bind = &BindOscillator<EWave::Square>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("osc.square"), 3}, .bSource = true, .Controls = OscillatorControls,
                .Bind = &BindOscillator<EWave::Square, true>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("osc.triangle"), 1}, .bSource = true, .Controls = OscillatorControls,
                .Bind = &BindOscillator<EWave::Triangle>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("mod.sine"), 1}, .bSource = true, .bStereoOutput = false,
                .Controls = {{TEXT("frequencyHz"), TEXT("Hz"), 0}, {TEXT("amplitude"), TEXT("linear")}, {TEXT("offset"), TEXT("linear")}},
                .Properties = {{TEXT("phaseCycles"), EPropertyKind::Number, 0., 1., 0.}},
                .Bind = &BindSineModulator, .bControlOnly = true, .bSupportsContinuousGate = true},
            {.Type = {TEXT("source.noise_buffer"), 1}, .bSource = true, .bStereoOutput = true,
                .Controls = {{TEXT("playbackRate"), TEXT("ratio"), 0}},
                .Properties = {{TEXT("buffer"), EPropertyKind::NoiseBuffer}, {TEXT("offsetSeconds"), EPropertyKind::Number, 0, 30}},
                .Bind = &BindNoise<false>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("source.noise_buffer"), 2}, .bSource = true, .bStereoOutput = true,
                .Controls = {{TEXT("playbackRate"), TEXT("ratio"), 0}},
                .Properties = {{TEXT("buffer"), EPropertyKind::NoiseBuffer}, {TEXT("offsetSeconds"), EPropertyKind::Number, 0, 30},
                    {TEXT("rateHoldFrames"), EPropertyKind::Integer, 1, Block, 1., {}, {1., double(Block)}}},
                .Bind = &BindNoise<false>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("source.mono_noise_buffer"), 1}, .bSource = true, .bStereoOutput = false,
                .Controls = {{TEXT("playbackRate"), TEXT("ratio"), 0}},
                .Properties = {{TEXT("buffer"), EPropertyKind::NoiseBuffer}, {TEXT("offsetSeconds"), EPropertyKind::Number, 0, 30}},
                .Bind = &BindNoise<true>, .bSupportsContinuousGate = true},
            {.Type = {TEXT("source.grain_once"), 1}, .bSource = true, .bStereoOutput = false,
                .Controls = {{TEXT("playbackRate"), TEXT("ratio"), .001, 32}},
                .Properties = {{TEXT("buffer"), EPropertyKind::NoiseBuffer}, {TEXT("decaySeconds"), EPropertyKind::Number, .0001, 10.}},
                .Bind = &BindSingleGrain, .bSupportsContinuousGate = true},
            {{TEXT("filter.lowpass"), 1}, false, false, ResonantFilter, {}, &BindBiquad<EFilter::Lowpass>},
            {{TEXT("filter.highpass"), 1}, false, false, ResonantFilter, {}, &BindBiquad<EFilter::Highpass>},
            {{TEXT("filter.bandpass"), 1}, false, false,
                {{TEXT("cutoffHz"), TEXT("Hz"), 0}, {TEXT("q"), TEXT("ratio")}}, {}, &BindBiquad<EFilter::Bandpass>},
            {{TEXT("filter.lowshelf"), 1}, false, false,
                {{TEXT("cutoffHz"), TEXT("Hz"), 0}, {TEXT("gainDb"), TEXT("dB"), -48, 48}}, {}, &BindLowShelf},
            {{TEXT("mix.pan"), 1}, false, true, {{TEXT("position"), TEXT("pan")}}, {}, &BindPan},
            {.Type = {TEXT("delay.linear"), 1}, .bSource = false, .bStereoOutput = false,
                .Controls = {{TEXT("delaySeconds"), TEXT("seconds"), 0, .05}}, .Bind = &BindDelay, .MaxInstances = 16},
            {.Type = {TEXT("delay.causal"), 1}, .bSource = false, .bStereoOutput = false,
                .Properties = {{TEXT("delaySeconds"), EPropertyKind::Number, double(Block) / 8000., 2.}},
                .Bind = &BindCausalDelay, .bProcessSilence = true, .bDelayedAudioInput = true, .MaxInstances = 8},
            {{TEXT("shape.tanh"), 1}, false, false, {},
                {{TEXT("drive"), EPropertyKind::Number, .000001, 16}, {TEXT("scale"), EPropertyKind::Number, .000001, 4},
                 {TEXT("oversample2x"), EPropertyKind::Flag}}, &BindShape},
            {.Type = {TEXT("room.convolution"), 1}, .bSource = false, .bStereoOutput = true,
                .Properties = {{TEXT("impulse"), EPropertyKind::Impulse}}, .Bind = &BindRoom, .MaxInstances = 4},
            {.Type = {TEXT("room.convolution"), 2}, .bSource = false, .bStereoOutput = true,
                .Properties = {{TEXT("impulse"), EPropertyKind::Impulse}, {TEXT("normalize"), EPropertyKind::Flag}},
                .Bind = &BindRoomWithNormalization, .MaxInstances = 4},
            {.Type = {TEXT("room.decaying_noise"), 1}, .bSource = false, .bStereoOutput = true,
                .Properties = {{TEXT("noise"), EPropertyKind::NoiseBuffer}, {TEXT("attackSeconds"), EPropertyKind::Number, 0., 4.},
                    {TEXT("decaySeconds"), EPropertyKind::Number, .0001, 10.}}, .Bind = &BindDecayingNoiseRoom, .MaxInstances = 1},
            {.Type = {TEXT("dynamics.compressor"), 1}, .bSource = false, .bStereoOutput = true,
                .Controls = {{TEXT("thresholdDb"), TEXT("dB"), -100, 0, true}, {TEXT("kneeDb"), TEXT("dB"), 0, 40, true},
                    {TEXT("ratio"), TEXT("ratio"), 1, 20, true}, {TEXT("attackSeconds"), TEXT("seconds"), 0, 1, true},
                    {TEXT("releaseSeconds"), TEXT("seconds"), 0, 1, true}}, .Bind = &BindCompressor, .MaxInstances = 2},
        };
        const auto Matches = [Type](const FNodeSpec& Spec) { return Spec.Type == Type; };
        if (const auto* Spec = Types.FindByPredicate(Matches)) return Spec;
        if (Matches(GrainNodeType())) return &GrainNodeType();
        return VoiceAndFilterTypes().FindByPredicate(Matches);
    }

    TOptional<ERecipe> FindBufferType(FNodeTypeId Type)
    {
        if (Type == FNodeTypeId{TEXT("buffer.white"), 1}) return ERecipe::White;
        if (Type == FNodeTypeId{TEXT("buffer.pink"), 1}) return ERecipe::Pink;
        if (Type == FNodeTypeId{TEXT("buffer.brown"), 1}) return ERecipe::Brown;
        if (Type == FNodeTypeId{TEXT("impulse.room"), 1}) return ERecipe::Room;
        if (Type == FNodeTypeId{TEXT("buffer.brown_loop"), 1}) return ERecipe::BrownLoop;
        if (Type == FNodeTypeId{TEXT("buffer.three_pole_loop"), 1}) return ERecipe::ThreePoleLoop;
        if (Type == FNodeTypeId{TEXT("impulse.diffuse"), 1}) return ERecipe::DiffuseImpulse;
        return {};
    }
}
