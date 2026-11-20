#pragma once

#include "CoreMinimal.h"
#include "DreamSFXInstrument.h"

/**
 * Additive/modal piano prototype used to answer one question at P1B:
 * can a pure synthesis model read as "piano", not as a bell or an organ?
 *
 * Model summary
 *  - Each note is a bank of inharmonic partials: f_n = n * f0 * sqrt(1 + B*n^2)
 *    where B is the string stiffness factor (Railsback curve, rising for
 *    higher notes and shorter strings).
 *  - Partials have two-stage decay: a fast initial "prompt sound" and a long
 *    aftersound, with the split frequency rising with pitch and velocity.
 *  - Velocity changes both the spectral tilt (harder hits excite more upper
 *    partials) and the attack transient length, not just loudness.
 *  - Note-off applies damper damping, which is much faster than natural decay;
 *    the sustain pedal suppresses it, matching a real right-pedal hold.
 *  - A short hammer thump adds the mechanical click before the tone speaks.
 *
 * This is deliberately dry: no reverb, no delay, no bitcrusher, so the model
 * can be judged on its own.
 */
class DREAMSFXCORE_API FDreamSFXPianoEngine : public IDreamSFXInstrumentEngine
{
public:
	struct FParams
	{
		/** Inharmonicity multiplier at C4; scaled up towards the treble. */
		float Inharmonicity = 0.00027f;

		/** Long aftersound decay time at the fundamental, in seconds, at C4. */
		float BaseDecaySeconds = 1.6f;

		/** How much faster upper partials decay relative to the fundamental. */
		float DecayTilt = 0.55f;

		/** Extra brightness scale applied to the top partials at full velocity. */
		float VelocityBrightness = 1.0f;

		/** Hammer noise level, 0..1. */
		float HammerLevel = 0.35f;

		/** Overall output gain in dB. */
		float GainDb = -9.0f;

		/** Max partials per voice. Higher is richer but more expensive. */
		int32 MaxPartials = 24;

		/** Usable voices; clamped to the compile-time array size. */
		int32 VoiceLimit = 16;

		/** Numerical regression only; authoring adapter always leaves this false. */
		bool bReferenceOscillator = false;
	};

	void SetParams(const FParams& InParams) { Params = InParams; }
	const FParams& GetParams() const { return Params; }

	// IDreamSFXInstrumentEngine
	virtual void Prepare(int32 SampleRate) override;
	virtual void ResetVoices() override { for (FVoice& Voice : Voices) { Voice = FVoice(); } bSustainPedal = false; NextRetiring=0;PitchRatio=1;bPitchModulated=false; }
	virtual void SetPitchRatio(float Ratio) override {PitchRatio=Ratio;bPitchModulated=true;}
	virtual void NoteOn(int32 NoteId, int32 MidiNote, float Velocity, int32 StartFrame) override;
	virtual void NoteOff(int32 NoteId, bool bSustainPedal) override;
	virtual void SetSustainPedal(bool bDown) override;
	virtual void Render(float* OutStereoBuffer, int32 NumFrames) override;
	virtual bool HasActiveVoices() const override;
	virtual bool CanStartVoice() const override;
	virtual float GetMaxTailSeconds() const override;
	virtual void Advance(int32 NumFrames) override;

	/** Diagnostic: number of partials actually synthesised for a note. */
	int32 GetPartialsForNote(int32 MidiNote) const;

private:
	static constexpr int32 MaxVoices = 16;
	static constexpr int32 MaxPartials = 32;

	struct FVoice
	{
		bool bActive = false;
		bool bKeyHeld = false;
		bool bDamping = false;
		int32 NoteId = 0;
		int32 MidiNote = 60;
		float Velocity = 1.0f;
		double PhaseOffset = 0.0;
		int32 NumPartials = 0;
		int32 SampleOffset = 0;
		int64 FramesElapsed = 0;

		double PartialPhase[MaxPartials] = { 0.0 };
		double PartialDelta[MaxPartials] = { 0.0 };
		double PartialSin[MaxPartials] = { 0.0 };
		double PartialCos[MaxPartials] = { 0.0 };
		double RotationSin[MaxPartials] = { 0.0 };
		double RotationCos[MaxPartials] = { 0.0 };
		uint32 OscillatorFrames = 0;
		double PartialAmp[MaxPartials] = { 0.0 };
		double PartialFastDecay[MaxPartials] = { 0.0 };
		double PartialSlowDecay[MaxPartials] = { 0.0 };
		double PartialFastMix[MaxPartials] = { 0.0 };
		double FastEnv[MaxPartials] = { 1.0 };
		double SlowEnv[MaxPartials] = { 1.0 };

		double DamperGain = 1.0;
		float PeakEnvelope = 0.0f;
		int32 QuietFrames = 0;
		int32 StealRemaining=0,StealDelay=0;
		double DamperCoeff = 0.0;

		// Simple one-pole filtered noise for the hammer thump.
		double NoiseState = 0.0;
		double NoiseCoeff = 0.0;
		double NoiseGain = 0.0;
		int32 NoiseFramesRemaining = 0;
	};

	void BuildVoice(FVoice& Voice, int32 MidiNote, float Velocity) const;
	void EndVoice(int32 VoiceIndex);
	int32 FindVoice(int32 NoteId) const;

	FVoice Voices[MaxVoices*2];
	int32 NextRetiring=0,StealLength=220;
	FParams Params;
	int32 SampleRate = 44100;
	float EngineGain = 1.0f;
	float PitchRatio=1;
	bool bPitchModulated=false;
	bool bSustainPedal = false;
	bool bPrepared = false;
};
