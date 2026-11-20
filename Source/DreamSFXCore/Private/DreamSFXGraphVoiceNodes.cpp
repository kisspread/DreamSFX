#include "DreamSFXNodeRegistry.h"
#include "DreamSFXDCBlocker.h"
#include "DreamSFXSfxrVoice.h"
#include "DreamSFXMusicBoxEngine.h"
#include "DreamSFXPianoEngine.h"
#include "DreamSFXRetroEngine.h"
#include "DreamSFXSineEngine.h"
#include "DreamSFXTrackFilter.h"

namespace DreamSFX::V2
{
    namespace
    {
        template<class TVoice, class... TArgs>
        FKernel PrepareVoice(int32 Rate, TArgs&&... Args)
        {
            FKernel Kernel;
            Kernel.Voice = MakeUnique<TVoice>(Forward<TArgs>(Args)...);
            Kernel.Voice->Prepare(Rate);
            return Kernel;
        }

        template<bool bNoise>
        FKernel BindRetro(const FKernelSetup& Setup)
        {
            const float Duty = bNoise ? .5f : float(Setup.Number(TEXT("dutyCycle")));
            return PrepareVoice<FDreamSFXRetroEngine>(Setup.Rate, float(Setup.Number(TEXT("gainDb"))),
                int32(Setup.Number(TEXT("voiceLimit"))), bNoise, Duty);
        }

        template<class TVoice>
        FKernel BindInstrument(const FKernelSetup& Setup)
        {
            typename TVoice::FParams Parameters;
            Parameters.GainDb = float(Setup.Number(TEXT("gainDb")));
            Parameters.VoiceLimit = int32(Setup.Number(TEXT("voiceLimit")));
            auto Voice = MakeUnique<TVoice>();
            Voice->SetParams(Parameters);
            Voice->Prepare(Setup.Rate);
            FKernel Kernel;
            Kernel.Voice = MoveTemp(Voice);
            return Kernel;
        }

        // The sfxr kernel consumes these named normalized synthesis controls directly.
        struct FHitParameter { const TCHAR* Name; float FEightBitSynthParams::* Member; double Minimum = 0.; };
        constexpr FHitParameter HitParameters[] = {
            {TEXT("attack"), &FEightBitSynthParams::EnvAttack},
            {TEXT("sustain"), &FEightBitSynthParams::EnvSustain},
            {TEXT("punch"), &FEightBitSynthParams::EnvPunch},
            {TEXT("decay"), &FEightBitSynthParams::EnvDecay},
            {TEXT("baseFrequency"), &FEightBitSynthParams::BaseFreq},
            {TEXT("frequencyLimit"), &FEightBitSynthParams::FreqLimit},
            {TEXT("frequencySlide"), &FEightBitSynthParams::FreqRamp, -1.},
            {TEXT("frequencySlideDelta"), &FEightBitSynthParams::FreqDeltaRamp, -1.},
            {TEXT("vibratoDepth"), &FEightBitSynthParams::VibStrength},
            {TEXT("vibratoSpeed"), &FEightBitSynthParams::VibSpeed},
            {TEXT("pitchChange"), &FEightBitSynthParams::ArpMod, -1.},
            {TEXT("pitchChangeSpeed"), &FEightBitSynthParams::ArpSpeed},
            {TEXT("pulseWidth"), &FEightBitSynthParams::Duty},
            {TEXT("pulseWidthSlide"), &FEightBitSynthParams::DutyRamp, -1.},
            {TEXT("repeatSpeed"), &FEightBitSynthParams::RepeatSpeed},
            {TEXT("flangerOffset"), &FEightBitSynthParams::FlangerOffset, -1.},
            {TEXT("flangerSlide"), &FEightBitSynthParams::FlangerRamp, -1.},
            {TEXT("lowpassCutoff"), &FEightBitSynthParams::LpfFreq},
            {TEXT("lowpassSlide"), &FEightBitSynthParams::LpfRamp, -1.},
            {TEXT("lowpassResonance"), &FEightBitSynthParams::LpfResonance},
            {TEXT("highpassCutoff"), &FEightBitSynthParams::HpfFreq},
            {TEXT("highpassSlide"), &FEightBitSynthParams::HpfRamp, -1.},
            {TEXT("outputLevel"), &FEightBitSynthParams::SoundVolume},
        };

        FKernel BindHit(const FKernelSetup& Setup)
        {
            FEightBitSynthParams Parameters;
            for (const auto& Field : HitParameters)
                Parameters.*(Field.Member) = float(Setup.Number(Field.Name));
            const FName Wave = Setup.NameValue(TEXT("wave"));
            if (Wave == TEXT("noise")) Parameters.WaveShape = EEightBitWaveShape::Noise;
            else if (Wave == TEXT("sawtooth")) Parameters.WaveShape = EEightBitWaveShape::Sawtooth;
            else if (Wave == TEXT("sine")) Parameters.WaveShape = EEightBitWaveShape::Sine;
            Parameters.SampleRateMode = FEightBitSynthParams::HzToSampleRateMode(int32(Setup.Number(TEXT("kernelRateHz"))));
            Parameters.BitDepth = Setup.Number(TEXT("sampleBits")) == 16 ? EEightBitBitDepth::Bits16 : EEightBitBitDepth::Bits8;
            Parameters.Seed = int32(Setup.Number(TEXT("seed")));
            return PrepareVoice<FDreamSFXSfxrVoice>(Setup.Rate, Parameters, float(Setup.Number(TEXT("gainDb"))),
                int32(Setup.Number(TEXT("voiceLimit"))));
        }

        FKernel BindTrackLowpass(const FKernelSetup& Setup)
        {
            auto State = MakeShared<FDreamSFXTrackFilter>();
            State->Prepare(Setup.Rate);
            return {[State, Cutoff = Setup.Control(TEXT("cutoffHz"))](FKernelContext& Context)
            {
                for (int32 Frame = 0; Frame < DSFXGraphDSP::Block; ++Frame)
                {
                    State->SetCutoff(Context.Control(Cutoff, Frame));
                    for (int32 Channel = 0; Channel < 2; ++Channel)
                        Context.Signal.Audio[Channel][Frame] = State->Process(Context.Signal.Audio[Channel][Frame], Channel);
                }
            }, [State](DSFXGraphDSP::FState&) { State->Reset(); }};
        }

        FKernel BindDcBlock(const FKernelSetup& Setup)
        {
            auto State = MakeShared<FDreamSFXDCBlocker>();
            State->Prepare(Setup.Rate);
            return {[State](FKernelContext& Context)
            {
                for (int32 Channel = 0; Channel < 2; ++Channel)
                    for (int32 Frame = 0; Frame < DSFXGraphDSP::Block; ++Frame)
                        Context.Signal.Audio[Channel][Frame] = State->Process(Context.Signal.Audio[Channel][Frame], Channel);
            }, [State](DSFXGraphDSP::FState&) { State->Reset(); }};
        }

        FKernel BindDecibelGain(const FKernelSetup& Setup)
        {
            return {[Slot = Setup.Control(TEXT("gainDb"))](FKernelContext& Context)
            {
                for (int32 Frame = 0; Frame < DSFXGraphDSP::Block; ++Frame)
                {
                    const float Gain = FMath::Pow(10.f, Context.Control(Slot, Frame) / 20.f);
                    for (int32 Channel = 0; Channel < 2; ++Channel)
                        Context.Signal.Audio[Channel][Frame] *= Gain;
                }
            }};
        }
    }

    TConstArrayView<FNodeSpec> VoiceAndFilterTypes()
    {
        static const TArray<FNodeSpec> Types = []
        {
            using EProperty = EPropertyKind;
            const TArray<FPropertySpec> Voice = {
                {TEXT("gainDb"), EProperty::Number, -192., 24.},
                {TEXT("voiceLimit"), EProperty::Integer, 1., 16.},
            };
            auto Pulse = Voice;
            Pulse.Add({TEXT("dutyCycle"), EProperty::Number, .05, .5, .5});
            auto Hit = Voice;
            Hit.Add({TEXT("wave"), EProperty::Choice, 0., 0., {}, {TEXT("square"), TEXT("sawtooth"), TEXT("sine"), TEXT("noise")}});
            Hit.Add({TEXT("kernelRateHz"), EProperty::Integer, 8000., 44100., {}, {}, {8000., 11025., 22050., 44100.}});
            Hit.Add({TEXT("sampleBits"), EProperty::Integer, 8., 16., {}, {}, {8., 16.}});
            Hit.Add({TEXT("seed"), EProperty::Integer, 0., MAX_int32});
            const FEightBitSynthParams Defaults;
            for (const auto& Field : HitParameters)
                Hit.Add({Field.Name, EProperty::Number, Field.Minimum, 1., double(Defaults.*(Field.Member))});
            return TArray<FNodeSpec>{
                {{TEXT("voice.sine"), 1}, true, false, {}, Voice, [](const FKernelSetup& Setup)
                {
                    return PrepareVoice<FDreamSFXSineEngine>(Setup.Rate, float(Setup.Number(TEXT("gainDb"))), int32(Setup.Number(TEXT("voiceLimit"))));
                }, EEventMode::NotesWithSustain},
                {{TEXT("voice.retro_pulse"), 1}, true, false, {}, MoveTemp(Pulse), &BindRetro<false>, EEventMode::Notes},
                {{TEXT("voice.retro_noise"), 1}, true, false, {}, Voice, &BindRetro<true>, EEventMode::Notes},
                {{TEXT("voice.music_box"), 1}, true, true, {}, Voice, &BindInstrument<FDreamSFXMusicBoxEngine>, EEventMode::Notes},
                {{TEXT("voice.modal_piano"), 1}, true, true, {}, Voice, &BindInstrument<FDreamSFXPianoEngine>, EEventMode::NotesWithSustain},
                {{TEXT("source.sfxr"), 1}, true, false, {}, MoveTemp(Hit), &BindHit, EEventMode::Hit},
                {{TEXT("filter.one_pole_lowpass"), 1}, false, false, {{TEXT("cutoffHz"), TEXT("Hz"), 20, 96000}}, {}, &BindTrackLowpass, EEventMode::Gate, true},
                {{TEXT("filter.dc_block_20hz"), 1}, false, false, {}, {}, &BindDcBlock, EEventMode::Gate, true},
                {{TEXT("mix.gain_db"), 1}, false, false, {{TEXT("gainDb"), TEXT("dB"), -192, 24}}, {}, &BindDecibelGain},
            };
        }();
        return Types;
    }
}
