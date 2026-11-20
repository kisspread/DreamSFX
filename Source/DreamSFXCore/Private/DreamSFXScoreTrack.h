#pragma once
#include "DreamSFXDslSyntax.h"
#include "DreamSFXSoundProgram.h"

namespace DreamSFX::V2
{
    using FWrittenPhrase = TSharedPtr<const FDreamSFXScoreResult,ESPMode::ThreadSafe>;
    FWrittenPhrase ReadWrittenPhrase(const TArray<Dsl::FStatement>& Lines,FDiagnostic& Error);
    bool ReadTrackSettings(const Dsl::FStatement& Track,const TMap<FString,FWrittenPhrase>& Phrases,
        const TMap<FString,int32>& Registers,double Window,FMusicTrack& Out,FDiagnostic& Error);

    struct FTrackEvent
    {
        int64 Frame;
        EAction Action;
        int32 NoteId, Midi;
        float Velocity;
        uint32 Seed;
        bool Down;
    };

    class FMusicTrackPlayer
    {
    public:
        void Prepare(const FMusicTrack& InTrack,int32 InRate,uint32 Root);
        void Reset();
        void Stop(int64 At);
        void Fill(int64 First,int32 Count,TConstArrayView<double> Values);
        const TArray<FTrackEvent>& Events() const {return BlockEvents;}
    private:
        const FMusicTrack* Track=nullptr;
        int32 Rate=48000,Pass=0,NoteIndex=0,NextId=0,PedalIndex=0;
        int64 StopAt=MAX_int64;
        uint32 InitialSeed=0,Random=0;
        struct FRelease {int64 Frame;int32 Id;};
        TArray<FRelease> Releases;
        TArray<FTrackEvent> BlockEvents;
    };
}
