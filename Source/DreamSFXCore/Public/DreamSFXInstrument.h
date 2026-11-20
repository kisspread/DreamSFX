#pragma once

#include "CoreMinimal.h"

/**
 * Minimal voice interface implemented by every DreamSFX instrument engine.
 *
 * Engines are created once per program instance and driven by the timeline.
 * All methods run on the audio thread once the instance is playing, so they
 * must not allocate, lock, log or touch UObjects.
 */
class DREAMSFXCORE_API IDreamSFXInstrumentEngine
{
public:
	virtual ~IDreamSFXInstrumentEngine() = default;

	/** Prepare for a program at the given host sample rate. Must be allocation-free afterwards. */
	virtual void Prepare(int32 SampleRate) = 0;
	/** Immediate allocation-free reset for transport restart (not a fade). */
	virtual void ResetVoices() = 0;

	/**
	 * Start a note.
	 * @param NoteId   unique per active note, used to match the later NoteOff
	 * @param MidiNote pitch in MIDI numbers (60 = C4)
	 * @param Velocity 0..1
	 * @param StartFrame block-local frame offset the note starts on
	 */
	virtual void NoteOn(int32 NoteId, int32 MidiNote, float Velocity, int32 StartFrame) = 0;
	virtual void NoteOnSeeded(int32 NoteId,int32 MidiNote,float Velocity,int32 StartFrame,uint32 Seed) { NoteOn(NoteId,MidiNote,Velocity,StartFrame); }

	/** Begin release for the given note. */
	virtual void NoteOff(int32 NoteId, bool bSustainPedal) = 0;

	/** Set the sustain pedal state; held notes keep sounding while true. */
	virtual void SetSustainPedal(bool bDown) = 0;
	/** Relative note-frequency ratio; changes neither scheduling nor gate/release clocks. */
	virtual void SetPitchRatio(float Ratio) {}

	/** Add this engine's output into an interleaved stereo buffer. */
	virtual void Render(float* OutStereoBuffer, int32 NumFrames) = 0;

	/** True while any voice is still producing sound. */
	virtual bool HasActiveVoices() const = 0;

	/**
	 * True when NoteOn can allocate a free voice without stealing.
	 * The instance scheduler uses this to decide whether to retire an older note
	 * first, so stealing stays observable instead of happening invisibly.
	 */
	virtual bool CanStartVoice() const = 0;

	/** Maximum note length including release tail, in seconds. */
	virtual float GetMaxTailSeconds() const = 0;

	/**
	 * Move the engine forward by NumFrames without writing audio.
	 * Used when the host skips rendering or when validating timing offline.
	 */
	virtual void Advance(int32 NumFrames) = 0;
};
