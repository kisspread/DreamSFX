#include "DreamSFXPianoEngine.h"
#include "DreamSFXOscillatorTable.h"

#include "DreamSFXCoreLog.h"

namespace DreamSFXPianoPrivate
{
	constexpr double TwoPi = 6.283185307179586476925286766559;

	/** MIDI note to frequency with A4 = 440 Hz, equal temperament. */
	FORCEINLINE double MidiToHz(double Midi)
	{
		return 440.0 * FMath::Pow(2.0, (Midi - 69.0) / 12.0);
	}

	/** Railsback-style stiffness: rises towards the treble, tapers in the bass. */
	FORCEINLINE double InharmonicityFor(double Midi, double Base)
	{
		const double Normalised = (Midi - 60.0) / 24.0;
		return Base * FMath::Exp(2.2 * Normalised);
	}

	/** Decay time scaling by pitch: shorter strings ring less. */
	FORCEINLINE double DecayScaleFor(double Midi)
	{
		const double Octaves = (Midi - 60.0) / 12.0;
		return FMath::Clamp(FMath::Pow(0.9, Octaves), 0.18, 3.0);
	}

	/** Exponential gain envelope coefficient for a given time constant in seconds. */
	FORCEINLINE double DecayCoeff(double Seconds, int32 SampleRate)
	{
		const double SafeSeconds = FMath::Max(0.0005, Seconds);
		return FMath::Exp(-1.0 / (SafeSeconds * static_cast<double>(SampleRate)));
	}

	constexpr double VoiceNormalise = 0.30;

	FORCEINLINE double LinearToDbInverse(float Db)
	{
		return FMath::Pow(10.0, static_cast<double>(Db) / 20.0);
	}
}

void FDreamSFXPianoEngine::Prepare(int32 InSampleRate)
{
	DreamSFXOscillatorTable::Initialize();PitchRatio=1;bPitchModulated=false;
	SampleRate = FMath::Max(8000, InSampleRate);
	NextRetiring=0;StealLength=FMath::Max(2,FMath::RoundToInt(SampleRate*.005));
	EngineGain = static_cast<float>(DreamSFXPianoPrivate::LinearToDbInverse(Params.GainDb));
	bSustainPedal = false;
	bPrepared = true;

	for (FVoice& Voice : Voices)
	{
		Voice = FVoice();
	}

	UE_LOG(LogDreamSFXCore, Log, TEXT("Piano engine prepared: rate=%d partials=%d harmonics=%.6f decay=%.2fs"),
		SampleRate, Params.MaxPartials, Params.Inharmonicity, Params.BaseDecaySeconds);
}

int32 FDreamSFXPianoEngine::GetPartialsForNote(int32 MidiNote) const
{
	const double Frequency = DreamSFXPianoPrivate::MidiToHz(FMath::Clamp(MidiNote, 0, 127));
	const double Nyquist = static_cast<double>(SampleRate) * 0.5;
	const double Inharmonicity = DreamSFXPianoPrivate::InharmonicityFor(FMath::Clamp(MidiNote, 0, 127), Params.Inharmonicity);

	int32 Count = 0;
	for (int32 Partial = 1; Partial <= FMath::Clamp(Params.MaxPartials, 1, MaxPartials); ++Partial)
	{
		const double FrequencyRatio = static_cast<double>(Partial) *
			FMath::Sqrt(1.0 + Inharmonicity * static_cast<double>(Partial * Partial));
		if (Frequency * FrequencyRatio >= Nyquist * 0.94)
		{
			break;
		}
		++Count;
	}
	return FMath::Max(1, Count);
}

void FDreamSFXPianoEngine::BuildVoice(FVoice& Voice, int32 MidiNote, float Velocity) const
{
	const int32 ClampedNote = FMath::Clamp(MidiNote, 0, 127);
	const double Frequency = DreamSFXPianoPrivate::MidiToHz(ClampedNote);
	const double Nyquist = static_cast<double>(SampleRate) * 0.5;
	const double Inharmonicity = DreamSFXPianoPrivate::InharmonicityFor(ClampedNote, Params.Inharmonicity);
	const double DecayScale = DreamSFXPianoPrivate::DecayScaleFor(ClampedNote);
	const double VelocityCurve = static_cast<double>(FMath::Clamp(Velocity, 0.0f, 1.0f));

	// Harder hits shorten the time until the string's aftersound dominates.
	const double PromptCutoff = 550.0 + 900.0 * VelocityCurve * static_cast<double>(Params.VelocityBrightness);
	const double FundAftersound = static_cast<double>(Params.BaseDecaySeconds) * DecayScale;
	const double PromptTime = 0.035 + 0.030 * (1.0 - VelocityCurve);

	// Register balance. A real piano has a heavy, dark low register and a thin,
	// bright treble; without this the instrument reads as "too high and not
	// thick enough" regardless of how correct the partial structure is.
	// Bass gets more level and more weight on its first few partials; treble is
	// pulled down and its energy pushed up into the upper partials.
	const double OctavesFromC4 = (static_cast<double>(ClampedNote) - 60.0) / 12.0;
	// Low notes are louder and heavier; the treble is quieter but must not
	// collapse, so the slope is gentler than a pure "bass boost" curve.
	const double RegisterLevelDb = FMath::Clamp(-2.2 * OctavesFromC4, -7.0, 7.0);
	const double RegisterLevel = FMath::Pow(10.0, RegisterLevelDb / 20.0);

	// A piano hammer excites fewer, weaker upper partials the higher the note:
	// short treble strings radiate most of their energy near the fundamental.
	// Without this steep absolute-frequency rolloff a C6 reads as a bright synth
	// pad instead of a struck string.
	const double TrebleRolloff = FMath::Clamp(1.0 - 0.30 * FMath::Max(0.0, OctavesFromC4) / 2.0, 0.35, 1.0);

	Voice.NumPartials = 0;
	for (int32 Partial = 1; Partial <= FMath::Clamp(Params.MaxPartials, 1, MaxPartials); ++Partial)
	{
		const double Ratio = static_cast<double>(Partial) *
			FMath::Sqrt(1.0 + Inharmonicity * static_cast<double>(Partial * Partial));
		const double PartialHz = Frequency * Ratio;
		if (PartialHz >= Nyquist * 0.94)
		{
			break;
		}

		const int32 Index = Voice.NumPartials++;
		Voice.PartialPhase[Index] = 0.0;
		Voice.PartialDelta[Index] = PartialHz / static_cast<double>(SampleRate);
		Voice.PartialSin[Index] = 0.0;
		Voice.PartialCos[Index] = 1.0;
		const double Rotation = Voice.PartialDelta[Index] * DreamSFXPianoPrivate::TwoPi;
		Voice.RotationSin[Index] = FMath::Sin(Rotation);
		Voice.RotationCos[Index] = FMath::Cos(Rotation);

		// Amplitude falls steeply with partial number. Velocity adds only a small
		// amount of upper-partial energy: a real hammer produces a modest
		// brightness change, not a different instrument.
		// Low notes keep more energy in their fundamental so they sound like a
		// body rather than a buzz; upper partials fall off steeply.
		const double HarmonicRolloff = FMath::Pow(1.0 / static_cast<double>(Partial), 1.55);
		// Absolute-frequency rolloff: partials above ~2.5 kHz lose energy quickly,
		// which is what keeps a melody from sounding thin and shrill.
		const double HzRolloff = 1.0 / (1.0 + FMath::Pow(PartialHz / 2500.0, 1.6) * 1.8);
		const double VelocityTilt = FMath::Pow(0.82 + 0.18 * VelocityCurve, static_cast<double>(Partial - 1) * 0.30);
		Voice.PartialAmp[Index] = HarmonicRolloff * HzRolloff * TrebleRolloff * VelocityTilt * RegisterLevel;

		// Two-stage decay: modal energy moves from the prompt sound to the after
		// sound. The crossover frequency rises with velocity.
		// Two-stage decay. The prompt (bright) component must be strongest for
		// upper partials so the note starts bright and darkens quickly; a piano
		// never gets brighter over time.
		const double Crossover = PromptCutoff * (1.0 + 0.25 * static_cast<double>(Partial) / 8.0);
		const double Mix = 1.0 / (1.0 + FMath::Pow(PartialHz / Crossover, 2.0));
		// Bias the prompt component towards upper partials: they must be the
		// transient that disappears, not the thing that survives.
		Voice.PartialFastMix[Index] = FMath::Clamp(Mix * (0.55 + 0.45 * static_cast<double>(Partial > 1)), 0.0, 1.0);
		Voice.PartialFastDecay[Index] = DreamSFXPianoPrivate::DecayCoeff(
			PromptTime / (1.0 + 0.12 * static_cast<double>(Partial - 1)), SampleRate);

		// Aftersound decay must shorten faster than the fundamental for upper
		// partials, otherwise the tail brightens instead of darkening.
		// Upper partials must ring noticeably shorter than the fundamental; this is
		// what makes a struck string darken instead of getting brighter.
		const double PartialAftersound = FundAftersound /
			FMath::Pow(1.0 + 0.55 * static_cast<double>(Partial - 1), static_cast<double>(Params.DecayTilt) + 0.35);
		Voice.PartialSlowDecay[Index] = DreamSFXPianoPrivate::DecayCoeff(PartialAftersound, SampleRate);

		Voice.FastEnv[Index] = 1.0;
		Voice.SlowEnv[Index] = 1.0;
	}

	// Hammer thump: a very short, dark noise burst for the mechanical attack.
	// It must stay quiet and short: an over-bright noise layer reads as an
	// electronic "hiss" on top of every note (measured as a 5 kHz spectral
	// centroid in melody passages).
	const double TrebleDamping = FMath::Clamp(1.0 - 0.5 * FMath::Max(0.0, OctavesFromC4) / 2.0, 0.25, 1.0);
	Voice.NoiseGain = static_cast<double>(Params.HammerLevel) * (0.10 + 0.35 * VelocityCurve) *
		TrebleDamping * RegisterLevel;
	Voice.NoiseCoeff = 1.0 - FMath::Exp(-2.0 * UE_DOUBLE_PI * (700.0 + 900.0 * VelocityCurve) / static_cast<double>(SampleRate));
	Voice.NoiseState = 0.0;
	Voice.NoiseFramesRemaining = FMath::RoundToInt(0.005 * SampleRate);

	// Damping after key release: bass strings ring longer than treble.
	Voice.DamperCoeff = DreamSFXPianoPrivate::DecayCoeff(
		FMath::Clamp(0.35 * DreamSFXPianoPrivate::DecayScaleFor(ClampedNote), 0.05, 0.8), SampleRate);
	Voice.DamperGain = 1.0;
	Voice.bDamping = false;

	(void)FundAftersound;
}

void FDreamSFXPianoEngine::NoteOn(int32 NoteId, int32 MidiNote, float Velocity, int32 StartFrame)
{
	if (!bPrepared)
	{
		return;
	}

	const int32 Limit = FMath::Clamp(Params.VoiceLimit, 1, MaxVoices);
	int32 Index = INDEX_NONE;
	for (int32 Candidate = 0; Candidate < Limit; ++Candidate)
	{
		if (!Voices[Candidate].bActive)
		{
			Index = Candidate;
			break;
		}
	}

	if (Index == INDEX_NONE)
	{
		// Prefer stealing the oldest releasing voice.
		int64 BestAge = -1;
		for (int32 Candidate = 0; Candidate < Limit; ++Candidate)
		{
			const FVoice& Voice = Voices[Candidate];
			if (Voice.bActive && !Voice.bKeyHeld && Voice.FramesElapsed > BestAge)
			{
				BestAge = Voice.FramesElapsed;
				Index = Candidate;
			}
		}
		if (Index == INDEX_NONE)
		{
			return;
		}
		if(Voices[Index].FramesElapsed>Voices[Index].SampleOffset)
		{
			int32 Tail=INDEX_NONE;for(int32 I=MaxVoices;I<MaxVoices*2;++I)if(!Voices[I].bActive){Tail=I;break;}
			if(Tail==INDEX_NONE){Tail=MaxVoices+NextRetiring;NextRetiring=(NextRetiring+1)%MaxVoices;}
			Voices[Tail]=Voices[Index];Voices[Tail].bKeyHeld=false;Voices[Tail].StealRemaining=StealLength;Voices[Tail].StealDelay=FMath::Max(0,StartFrame);
		}
		EndVoice(Index);
	}

	FVoice& Voice = Voices[Index];
	Voice = FVoice();
	Voice.bActive = true;
	Voice.bKeyHeld = true;
	Voice.NoteId = NoteId;
	Voice.MidiNote = FMath::Clamp(MidiNote, 0, 127);
	Voice.Velocity = FMath::Clamp(Velocity, 0.0f, 1.0f);
	Voice.SampleOffset = FMath::Max(0, StartFrame);
	Voice.FramesElapsed = 0;
	BuildVoice(Voice, Voice.MidiNote, Voice.Velocity);
}

void FDreamSFXPianoEngine::NoteOff(int32 NoteId, bool bSustainHeld)
{
	const int32 Index = FindVoice(NoteId);
	if (Index == INDEX_NONE)
	{
		return;
	}

	FVoice& Voice = Voices[Index];
	Voice.bKeyHeld = false;
	if (!bSustainHeld && !bSustainPedal)
	{
		Voice.bDamping = true;
	}
}

void FDreamSFXPianoEngine::SetSustainPedal(bool bDown)
{
	if (bSustainPedal == bDown)
	{
		return;
	}
	bSustainPedal = bDown;

	if (!bDown)
	{
		// Releasing the pedal damps every note whose key is no longer held.
		for (int32 VoiceIndex=0;VoiceIndex<MaxVoices;++VoiceIndex)
		{
			FVoice& Voice=Voices[VoiceIndex];
			if (Voice.bActive && !Voice.bKeyHeld)
			{
				Voice.bDamping = true;
			}
		}
	}
}

int32 FDreamSFXPianoEngine::FindVoice(int32 NoteId) const
{
	for (int32 Index = 0; Index < MaxVoices; ++Index)
	{
		if (Voices[Index].bActive && Voices[Index].NoteId == NoteId)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void FDreamSFXPianoEngine::EndVoice(int32 VoiceIndex)
{
	Voices[VoiceIndex] = FVoice();
}

bool FDreamSFXPianoEngine::HasActiveVoices() const
{
	for (const FVoice& Voice : Voices)
	{
		if (Voice.bActive)
		{
			return true;
		}
	}
	return false;
}

bool FDreamSFXPianoEngine::CanStartVoice() const
{
	const int32 Limit = FMath::Clamp(Params.VoiceLimit, 1, MaxVoices);
	for (int32 Index = 0; Index < Limit; ++Index)
	{
		if (!Voices[Index].bActive)
		{
			return true;
		}
	}
	return false;
}

float FDreamSFXPianoEngine::GetMaxTailSeconds() const
{
	return static_cast<float>(FMath::Clamp(static_cast<double>(Params.BaseDecaySeconds) * 3.5, 0.5, 12.0));
}

void FDreamSFXPianoEngine::Advance(int32 NumFrames)
{
	// Same sample-clock DSP path, with fixed stack scratch and discarded output.
	float Scratch[512];
	while(NumFrames>0)
	{
		const int32 Count=FMath::Min(256,NumFrames);
		FMemory::Memzero(Scratch,Count*2*sizeof(float));
		Render(Scratch,Count);NumFrames-=Count;
	}
}

void FDreamSFXPianoEngine::Render(float* OutStereoBuffer, int32 NumFrames)
{
	if (!OutStereoBuffer || NumFrames <= 0)
	{
		return;
	}

	for (int32 VoiceIndex = 0; VoiceIndex < MaxVoices*2; ++VoiceIndex)
	{
		FVoice& Voice = Voices[VoiceIndex];
		if (!Voice.bActive)
		{
			continue;
		}

		// Silence threshold: once every partial envelope has decayed below the
		// audible floor the voice can be released.
		float VoicePeak = 0.0f;

		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			const int64 LocalFrame = Voice.FramesElapsed;
			if (LocalFrame < Voice.SampleOffset)
			{
				++Voice.FramesElapsed;
				continue;
			}

			double Sample = 0.0;
            int32 FirstScalarPartial = 0;
            // Compute independent modes in parallel, then sum in exactly the authored scalar order.
            if (!bPitchModulated && !Params.bReferenceOscillator && (Voice.OscillatorFrames & 1023u) != 1023u)
            {
                const auto One = MakeVectorRegisterDouble(1., 1., 1., 1.);
                for (; FirstScalarPartial + 4 <= Voice.NumPartials; FirstScalarPartial += 4)
                {
                    const int32 I = FirstScalarPartial;
                    const auto FastEnv = VectorLoad(Voice.FastEnv + I);
                    const auto SlowEnv = VectorLoad(Voice.SlowEnv + I);
                    const auto FastMix = VectorLoad(Voice.PartialFastMix + I);
                    const auto Sin = VectorLoad(Voice.PartialSin + I);
                    const auto Cos = VectorLoad(Voice.PartialCos + I);
                    const auto Rs = VectorLoad(Voice.RotationSin + I);
                    const auto Rc = VectorLoad(Voice.RotationCos + I);
                    const auto Envelope = VectorAdd(VectorMultiply(FastMix, FastEnv), VectorMultiply(VectorSubtract(One, FastMix), SlowEnv));
                    double Modes[4];
                    VectorStore(VectorMultiply(VectorMultiply(VectorLoad(Voice.PartialAmp + I), Envelope), Sin), Modes);
                    for (double Mode : Modes) Sample += Mode;
                    VectorStore(VectorAdd(VectorMultiply(Sin, Rc), VectorMultiply(Cos, Rs)), Voice.PartialSin + I);
                    VectorStore(VectorSubtract(VectorMultiply(Cos, Rc), VectorMultiply(Sin, Rs)), Voice.PartialCos + I);
                    const auto Phase = VectorAdd(VectorLoad(Voice.PartialPhase + I), VectorLoad(Voice.PartialDelta + I));
                    VectorStore(VectorSelect(VectorCompareGE(Phase, One), VectorSubtract(Phase, One), Phase), Voice.PartialPhase + I);
                    VectorStore(VectorMultiply(FastEnv, VectorLoad(Voice.PartialFastDecay + I)), Voice.FastEnv + I);
                    VectorStore(VectorMultiply(SlowEnv, VectorLoad(Voice.PartialSlowDecay + I)), Voice.SlowEnv + I);
                }
            }
			for (int32 Partial = FirstScalarPartial; Partial < Voice.NumPartials; ++Partial)
			{
				const double Envelope = Voice.PartialFastMix[Partial] * Voice.FastEnv[Partial]
					+ (1.0 - Voice.PartialFastMix[Partial]) * Voice.SlowEnv[Partial];
				const double Tone = bPitchModulated
                    ? (Params.bReferenceOscillator?FMath::Sin(Voice.PartialPhase[Partial]*DreamSFXPianoPrivate::TwoPi):DreamSFXOscillatorTable::SinCycles(Voice.PartialPhase[Partial]))
                    : Params.bReferenceOscillator
					? FMath::Sin(Voice.PartialPhase[Partial] * DreamSFXPianoPrivate::TwoPi)
					: Voice.PartialSin[Partial];
				if(!bPitchModulated||Voice.PartialDelta[Partial]*PitchRatio<.49)Sample += Voice.PartialAmp[Partial] * Envelope * Tone;
                if(bPitchModulated)
                {
                    Voice.PartialPhase[Partial]+=Voice.PartialDelta[Partial]*PitchRatio;
                    Voice.PartialPhase[Partial]-=int32(Voice.PartialPhase[Partial]);
                }
                else
                {
				const double NextSin = Voice.PartialSin[Partial] * Voice.RotationCos[Partial]
					+ Voice.PartialCos[Partial] * Voice.RotationSin[Partial];
				Voice.PartialCos[Partial] = Voice.PartialCos[Partial] * Voice.RotationCos[Partial]
					- Voice.PartialSin[Partial] * Voice.RotationSin[Partial];
				Voice.PartialSin[Partial] = NextSin;

				Voice.PartialPhase[Partial] += Voice.PartialDelta[Partial];
				if (Voice.PartialPhase[Partial] >= 1.0)
				{
					Voice.PartialPhase[Partial] -= 1.0;
				}
				if ((Voice.OscillatorFrames & 1023u) == 1023u)
				{
					const double Angle=Voice.PartialPhase[Partial]*DreamSFXPianoPrivate::TwoPi;
					Voice.PartialSin[Partial]=FMath::Sin(Angle);
					Voice.PartialCos[Partial]=FMath::Cos(Angle);
				}
                }
				Voice.FastEnv[Partial] *= Voice.PartialFastDecay[Partial];
				Voice.SlowEnv[Partial] *= Voice.PartialSlowDecay[Partial];
			}

			++Voice.OscillatorFrames;
			if (Voice.NoiseFramesRemaining > 0)
			{
				// Cheap deterministic noise from the partial phase avoids any
				// global RNG on the audio thread.
				const double White = 2.0 * FMath::Frac(Voice.PartialPhase[0] * 9973.0) - 1.0;
				Voice.NoiseState += (White - Voice.NoiseState) * Voice.NoiseCoeff;
				Sample += Voice.NoiseState * Voice.NoiseGain;
				--Voice.NoiseFramesRemaining;
			}

			if (Voice.bDamping)
			{
				Voice.DamperGain *= Voice.DamperCoeff;
			}

			// Per-voice normalisation keeps a full triad inside headroom.
			const bool Fading=Voice.StealRemaining>0&&Voice.StealDelay==0;
			const float Fade=Fading?float(Voice.StealRemaining-1)/(StealLength-1):1.0f;
			const float Output = Fade * static_cast<float>(Sample * Voice.DamperGain * Voice.Velocity) * EngineGain * DreamSFXPianoPrivate::VoiceNormalise;
			OutStereoBuffer[Frame * 2] += Output;
			OutStereoBuffer[Frame * 2 + 1] += Output;
			VoicePeak = FMath::Max(VoicePeak, FMath::Abs(Output));
			++Voice.FramesElapsed;
			// Sample-clock tracking: zero crossings cannot retire a voice.
			Voice.PeakEnvelope = FMath::Max(FMath::Abs(Output), Voice.PeakEnvelope * 0.9995f);
			Voice.QuietFrames = Voice.PeakEnvelope < 1.0e-4f ? Voice.QuietFrames + 1 : 0;
			const bool EndTail=Fading&&--Voice.StealRemaining==0;if(Voice.StealDelay>0)--Voice.StealDelay;
			if (EndTail || (Voice.QuietFrames > SampleRate / 10&&(!bPitchModulated||(!Voice.bKeyHeld&&(!bSustainPedal||Voice.bDamping)))))
			{
				EndVoice(VoiceIndex);
				break;
			}
		}

	}
}
