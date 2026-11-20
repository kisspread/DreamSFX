#include "DreamSFXMusicBoxEngine.h"

#include "DreamSFXCoreLog.h"

namespace DreamSFXMusicBoxPrivate
{
	constexpr double TwoPi = 6.283185307179586476925286766559;

	/**
	 * Bar-mode frequency ratios for a free-free steel tooth.
	 * Mode 1 is the fundamental; later modes are strongly inharmonic and the
	 * amplitudes reflect how much energy each mode receives from the pluck.
	 */
	constexpr double ModeRatio[8] = { 1.000, 2.756, 5.404, 8.933, 13.34, 18.64, 24.85, 31.96 };
	constexpr double ModeAmp[8] = { 0.55, 1.00, 0.42, 0.22, 0.12, 0.07, 0.04, 0.02 };
	constexpr int32 MaxPartialsCount = 8;

	FORCEINLINE double MidiToHz(double Midi)
	{
		return 440.0 * FMath::Pow(2.0, (Midi - 69.0) / 12.0);
	}

	FORCEINLINE double DecayCoeff(double Seconds, int32 SampleRate)
	{
		return FMath::Exp(-1.0 / (FMath::Max(0.0005, Seconds) * static_cast<double>(SampleRate)));
	}
}

void FDreamSFXMusicBoxEngine::Prepare(int32 InSampleRate)
{
	SampleRate = FMath::Max(8000, InSampleRate);
	StealLength = FMath::Max(2, FMath::RoundToInt(SampleRate * .005));
	EngineGain = static_cast<float>(FMath::Pow(10.0, static_cast<double>(Params.GainDb) / 20.0));
	bPrepared = true;
	ResetVoices();

	UE_LOG(LogDreamSFXCore, Log, TEXT("MusicBox engine prepared: rate=%d decay=%.2fs"), SampleRate, Params.BaseDecaySeconds);
}

void FDreamSFXMusicBoxEngine::BuildVoice(FVoice& Voice, int32 MidiNote, float Velocity) const
{
	const double Frequency = DreamSFXMusicBoxPrivate::MidiToHz(FMath::Clamp(MidiNote, 0, 127));
	const double Nyquist = static_cast<double>(SampleRate) * 0.5;
	const double VelocityCurve = static_cast<double>(FMath::Clamp(Velocity, 0.0f, 1.0f));
	const double PluckGain = 0.35 + 0.65 * VelocityCurve;

	// Higher teeth are shorter and decay faster.
	const double PitchScale = FMath::Clamp(FMath::Pow(0.86, (MidiNote - 72) / 12.0), 0.25, 2.5);

	Voice.NumPartials = 0;
	for (int32 Mode = 0; Mode < DreamSFXMusicBoxPrivate::MaxPartialsCount; ++Mode)
	{
		if (Voice.NumPartials >= FMath::Clamp(Params.MaxPartials, 1, MaxPartials))
		{
			break;
		}

		const double ModeHz = Frequency * DreamSFXMusicBoxPrivate::ModeRatio[Mode];
		if (ModeHz >= Nyquist * 0.92)
		{
			break;
		}

		const int32 Index = Voice.NumPartials++;
		Voice.Phase[Index] = 0.0;
		Voice.Delta[Index] = ModeHz / static_cast<double>(SampleRate);
		Voice.Amp[Index] = DreamSFXMusicBoxPrivate::ModeAmp[Mode] * PluckGain;

		// Upper modes radiate faster into the air, so they die first, which is
		// what gives a struck bar its bright attack and metallic tail.
		const double ModeDecay = static_cast<double>(Params.BaseDecaySeconds) * PitchScale *
			FMath::Pow(0.42, static_cast<double>(Mode));
		Voice.Decay[Index] = DreamSFXMusicBoxPrivate::DecayCoeff(ModeDecay, SampleRate);
		Voice.Env[Index] = 1.0;
	}

	Voice.ReleaseGain = 1.0;
	Voice.ReleaseCoeff = DreamSFXMusicBoxPrivate::DecayCoeff(0.12, SampleRate);
}

void FDreamSFXMusicBoxEngine::NoteOn(int32 NoteId, int32 MidiNote, float Velocity, int32 StartFrame)
{
	if (!bPrepared)
	{
		return;
	}

	const int32 Limit = FMath::Clamp(Params.VoiceLimit, 1, MaxVoices);
	int32 Index = INDEX_NONE;
	for (int32 Candidate = 0; Candidate < Limit; ++Candidate)
	{
		if (!IsActive(Candidate))
		{
			Index = Candidate;
			break;
		}
	}
	if (Index == INDEX_NONE)
	{
		int64 BestAge = -1;
		for (int32 Candidate = 0; Candidate < Limit; ++Candidate)
		{
			const FVoice& Voice = Voices[Candidate];
			if (IsActive(Candidate) && !Voice.bKeyHeld && Voice.FramesElapsed > BestAge)
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
			int32 Tail=INDEX_NONE;for(int32 I=MaxVoices;I<MaxVoices*2;++I)if(!IsActive(I)){Tail=I;break;}
			if(Tail==INDEX_NONE){Tail=MaxVoices+NextRetiring;NextRetiring=(NextRetiring+1)%MaxVoices;}
			Voices[Tail]=Voices[Index];Voices[Tail].bKeyHeld=false;
			ActiveVoices |= (1u << Tail);
			Voices[Tail].StealRemaining=StealLength;Voices[Tail].StealDelay=FMath::Max(0,StartFrame);
		}
		EndVoice(Index);
	}

	FVoice& Voice = Voices[Index];
	Voice = FVoice();
	ActiveVoices |= (1u << Index);
	Voice.bKeyHeld = true;
	Voice.NoteId = NoteId;
	Voice.Velocity = FMath::Clamp(Velocity, 0.0f, 1.0f);
	Voice.SampleOffset = FMath::Max(0, StartFrame);
	BuildVoice(Voice, MidiNote, Voice.Velocity);
}

void FDreamSFXMusicBoxEngine::NoteOff(int32 NoteId, bool /*bSustainPedal*/)
{
	const int32 Index = FindVoice(NoteId);
	if (Index != INDEX_NONE)
	{
		// A music box has no damper: the tooth keeps ringing after the key is
		// released. Only the pluck itself matters.
		Voices[Index].bKeyHeld = false;
	}
}

void FDreamSFXMusicBoxEngine::SetSustainPedal(bool /*bDown*/)
{
	// Music boxes do not respond to sustain; declared as an unsupported control
	// so higher layers can warn instead of silently pretending.
}

int32 FDreamSFXMusicBoxEngine::FindVoice(int32 NoteId) const
{
	for (int32 Index = 0; Index < MaxVoices; ++Index)
	{
		if (IsActive(Index) && Voices[Index].NoteId == NoteId)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void FDreamSFXMusicBoxEngine::EndVoice(int32 VoiceIndex)
{
	Voices[VoiceIndex] = FVoice();
	ActiveVoices &= ~(1u << VoiceIndex);
}

bool FDreamSFXMusicBoxEngine::HasActiveVoices() const
{
	return ActiveVoices != 0;
}

bool FDreamSFXMusicBoxEngine::CanStartVoice() const
{
	const int32 Limit = FMath::Clamp(Params.VoiceLimit, 1, MaxVoices);
	for (int32 Index = 0; Index < Limit; ++Index)
	{
		if (!IsActive(Index))
		{
			return true;
		}
	}
	return false;
}

float FDreamSFXMusicBoxEngine::GetMaxTailSeconds() const
{
	return static_cast<float>(FMath::Clamp(static_cast<double>(Params.BaseDecaySeconds) * 4.0, 0.5, 8.0));
}

void FDreamSFXMusicBoxEngine::Advance(int32 NumFrames)
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

void FDreamSFXMusicBoxEngine::Render(float* OutStereoBuffer, int32 NumFrames)
{
	if (!OutStereoBuffer || NumFrames <= 0)
	{
		return;
	}

	for (uint32 Remaining = ActiveVoices; Remaining != 0; Remaining &= Remaining - 1)
	{
		const int32 VoiceIndex = FMath::CountTrailingZeros(Remaining);
		FVoice& Voice = Voices[VoiceIndex];

		float Peak = 0.0f;
		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			if (Voice.FramesElapsed < Voice.SampleOffset)
			{
				++Voice.FramesElapsed;
				continue;
			}

			double Sample = 0.0;
			for (int32 Partial = 0; Partial < Voice.NumPartials; ++Partial)
			{
				if(!bPitchModulated||Voice.Delta[Partial]*PitchRatio<.49)Sample += Voice.Amp[Partial] * Voice.Env[Partial] *
					FMath::Sin(Voice.Phase[Partial] * DreamSFXMusicBoxPrivate::TwoPi);
				Voice.Phase[Partial] += Voice.Delta[Partial]*PitchRatio;
                if(bPitchModulated)Voice.Phase[Partial]-=int32(Voice.Phase[Partial]);
                else
				if (Voice.Phase[Partial] >= 1.0)
				{
					Voice.Phase[Partial] -= 1.0;
				}
				Voice.Env[Partial] *= Voice.Decay[Partial];
			}

			const bool Fading=Voice.StealRemaining>0&&Voice.StealDelay==0;
			const int32 FadeLength=StealLength;
			const float Fade=Fading?float(Voice.StealRemaining-1)/(FadeLength-1):1.0f;
			const float Output = Fade * static_cast<float>(Sample * Voice.ReleaseGain * Voice.Velocity) * EngineGain * 0.45;
			OutStereoBuffer[Frame * 2] += Output;
			OutStereoBuffer[Frame * 2 + 1] += Output;
			Peak = FMath::Max(Peak, FMath::Abs(Output));
			++Voice.FramesElapsed;
			// Sample-clock tracking: zero crossings cannot retire a voice.
			Voice.PeakEnvelope = FMath::Max(FMath::Abs(Output), Voice.PeakEnvelope * 0.9995f);
			Voice.QuietFrames = Voice.PeakEnvelope < 1.0e-4f ? Voice.QuietFrames + 1 : 0;
			const bool EndTail=Fading&&--Voice.StealRemaining==0;if(Voice.StealDelay>0)--Voice.StealDelay;
			if (EndTail || (Voice.QuietFrames > SampleRate / 10&&(!bPitchModulated||!Voice.bKeyHeld)))
			{
				EndVoice(VoiceIndex);
				break;
			}
		}

	}
}
