#pragma once

#include "DreamSFXInstrument.h"
#include "EightBitSynthDSP.h"

/** Seeded sfxr voice. The fixed-parameter kernel is evaluated at Prepare for the graph's sample rate. */
class FDreamSFXSfxrVoice final : public IDreamSFXInstrumentEngine
{
public:
    FDreamSFXSfxrVoice(const FEightBitSynthParams& InParams, float GainDb, int32 VoiceLimit)
        : Params(InParams), Limit(VoiceLimit), Gain(FMath::Pow(10.f, GainDb / 20.f)) {}

    void Prepare(int32 SampleRate) override
    {
        ResetVoices();
        Samples.Reset();
        if (SampleRate < 8000 || SampleRate > 192000) return;
        FadeFrames = FMath::Max(2, FMath::RoundToInt(SampleRate * .005));
        FEightBitSynthDSP Kernel;
        Kernel.Init(Params, true);
        // Prime the resampler separately so an envelope which ends during priming keeps its exact last frame.
        while (Kernel.IsActive())
        {
            float Block[256];
            const int32 Count = Samples.IsEmpty() ? 1 : UE_ARRAY_COUNT(Block);
            int32 Finished = INDEX_NONE;
            Kernel.RenderBlock(float(SampleRate), Block, Count, Finished);
            Samples.Append(Block, Finished == INDEX_NONE ? Count : Finished + 1);
        }
    }

    void ResetVoices() override
    {
        for (auto& Voice : Voices) Voice = {};
        for (auto& Voice : Retiring) Voice = {};
        NextRetiring = 0;
    }

    bool HasActiveVoices() const override
    {
        for (const auto& Voice : Voices) if (Voice.bActive) return true;
        for (const auto& Voice : Retiring) if (Voice.bActive) return true;
        return false;
    }

    bool CanStartVoice() const override
    {
        for (int32 Index = 0; Index < Limit; ++Index) if (!Voices[Index].bActive) return true;
        return false;
    }

    void NoteOn(int32, int32, float Velocity, int32 Offset) override
    {
        if (Samples.IsEmpty()) return;
        int32 Slot = INDEX_NONE;
        for (int32 Index = 0; Index < Limit; ++Index)
        {
            if (!Voices[Index].bActive) { Slot = Index; break; }
        }
        if (Slot == INDEX_NONE)
        {
            Slot = 0;
            for (int32 Index = 1; Index < Limit; ++Index)
                if (Voices[Index].Cursor > Voices[Slot].Cursor) Slot = Index;
        }
        if (Voices[Slot].bActive && Voices[Slot].Cursor > 0)
        {
            int32 TailSlot = INDEX_NONE;
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Retiring); ++Index)
            {
                if (!Retiring[Index].bActive) { TailSlot = Index; break; }
            }
            if (TailSlot == INDEX_NONE)
            {
                TailSlot = NextRetiring;
                NextRetiring = (NextRetiring + 1) % UE_ARRAY_COUNT(Retiring);
            }
            auto& Tail = Retiring[TailSlot];
            Tail = Voices[Slot];
            Tail.FadeRemaining = FadeFrames;
            Tail.FadeDelay = FMath::Max(0, Offset);
        }
        Voices[Slot] = {};
        Voices[Slot].bActive = true;
        Voices[Slot].Delay = FMath::Max(0, Offset);
        Voices[Slot].Velocity = FMath::Clamp(Velocity, 0.f, 1.f);
    }

    void NoteOff(int32, bool) override {}
    void SetSustainPedal(bool) override {}
    float GetMaxTailSeconds() const override { return 7.f; }

    void Render(float* Out, int32 Frames) override
    {
        if (!Out || Frames <= 0) return;
        const auto AddVoice = [&](FVoice& Voice)
        {
            if (!Voice.bActive) return;
            const int32 First = FMath::Min(Voice.Delay, Frames);
            Voice.Delay -= First;
            for (int32 Frame = First; Frame < Frames; ++Frame)
            {
                const bool bFading = Voice.FadeRemaining > 0 && Voice.FadeDelay == 0;
                const float Fade = bFading ? float(Voice.FadeRemaining - 1) / (FadeFrames - 1) : 1.f;
                const float Sample = Samples[Voice.Cursor++] * Gain * Voice.Velocity * Fade;
                Out[2 * Frame] += Sample;
                Out[2 * Frame + 1] += Sample;
                if (Voice.FadeDelay > 0) --Voice.FadeDelay;
                const bool bEndFade = bFading && --Voice.FadeRemaining == 0;
                if (bEndFade || Voice.Cursor == Samples.Num()) { Voice = {}; break; }
            }
        };
        for (auto& Voice : Voices) AddVoice(Voice);
        for (auto& Voice : Retiring) AddVoice(Voice);
    }

    void Advance(int32 Frames) override
    {
        while (Frames > 0)
        {
            float Out[512] = {};
            const int32 Count = FMath::Min(Frames, 256);
            Render(Out, Count);
            Frames -= Count;
        }
    }

private:
    struct FVoice
    {
        int32 Cursor = 0;
        int32 Delay = 0;
        int32 FadeRemaining = 0;
        int32 FadeDelay = 0;
        float Velocity = 1.f;
        bool bActive = false;
    };
    FVoice Voices[16];
    FVoice Retiring[16];
    FEightBitSynthParams Params;
    int32 Limit;
    float Gain;
    int32 FadeFrames = 220;
    int32 NextRetiring = 0;
    TArray<float> Samples;
};
