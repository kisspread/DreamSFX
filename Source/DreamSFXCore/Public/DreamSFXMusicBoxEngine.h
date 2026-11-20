#pragma once

#include "CoreMinimal.h"
#include "DreamSFXInstrument.h"

/**
 * Music box / glockenspiel prototype.
 *
 * A metal comb tooth is a short stiff bar, so its partials are strongly
 * inharmonic and the fundamental is not the loudest. This model starts from a
 * fixed set of measured-ish bar ratios, gives every partial its own decay, and
 * excites them with a very short pluck impulse. That is a different family of
 * timbre from the piano model, not the same engine with a shorter decay.
 */
class DREAMSFXCORE_API FDreamSFXMusicBoxEngine : public IDreamSFXInstrumentEngine
{
public:
	struct FParams
	{
		/** Overall output gain in dB. */
		float GainDb = -12.0f;

		/** Base decay of the fundamental at C5, in seconds. */
		float BaseDecaySeconds = 1.1f;

		/** How quickly upper partials fade relative to the fundamental. */
		float DecayTilt = 1.6f;

		/** Pluck excitation length, seconds. */
		float PluckSeconds = 0.0025f;

		/** Small random detune in cents to avoid identical repeats. */
		float DetuneCents = 5.0f;

		/** Max partials per note. */
		int32 MaxPartials = 8;

		/** Usable voices; clamped to the compile-time array size. */
		int32 VoiceLimit = 16;
	};

	void SetParams(const FParams& InParams) { Params = InParams; }
	const FParams& GetParams() const { return Params; }

	virtual void Prepare(int32 SampleRate) override;
	virtual void ResetVoices() override { for (FVoice& Voice : Voices) { Voice = FVoice(); } ActiveVoices=0;NextRetiring=0;PitchRatio=1;bPitchModulated=false; }
	virtual void SetPitchRatio(float Ratio) override {PitchRatio=Ratio;bPitchModulated=true;}
	virtual void NoteOn(int32 NoteId, int32 MidiNote, float Velocity, int32 StartFrame) override;
	virtual void NoteOff(int32 NoteId, bool bSustainPedal) override;
	virtual void SetSustainPedal(bool bDown) override;
	virtual void Render(float* OutStereoBuffer, int32 NumFrames) override;
	virtual bool HasActiveVoices() const override;
	virtual bool CanStartVoice() const override;
	virtual float GetMaxTailSeconds() const override;
	virtual void Advance(int32 NumFrames) override;

	static int32 GetMaxPartials() { return MaxPartials; }

private:
	static constexpr int32 MaxVoices = 16;
	static constexpr int32 MaxPartials = 8;

	struct FVoice
	{
		bool bKeyHeld = false;
		int32 NoteId = 0;
		int32 SampleOffset = 0;
		int64 FramesElapsed = 0;
		float Velocity = 1.0f;
		int32 NumPartials = 0;

		double Phase[MaxPartials] = { 0.0 };
		double Delta[MaxPartials] = { 0.0 };
		double Amp[MaxPartials] = { 0.0 };
		double Decay[MaxPartials] = { 1.0 };
		double Env[MaxPartials] = { 0.0 };
		double ReleaseGain = 1.0;
		float PeakEnvelope = 0.0f;
		int32 QuietFrames = 0;
		int32 StealRemaining=0,StealDelay=0;
		double ReleaseCoeff = 0.0;
	};

	void BuildVoice(FVoice& Voice, int32 MidiNote, float Velocity) const;
	void EndVoice(int32 VoiceIndex);
	int32 FindVoice(int32 NoteId) const;

	bool IsActive(int32 Index) const { return (ActiveVoices & (1u << Index)) != 0; }

	FVoice Voices[MaxVoices*2];
	uint32 ActiveVoices = 0;
	int32 NextRetiring=0,StealLength=220;
	FParams Params;
	int32 SampleRate = 44100;
	float EngineGain = 1.0f;
	float PitchRatio=1;
	bool bPitchModulated=false;
	bool bPrepared = false;
};
