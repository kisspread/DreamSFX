#pragma once

#include "DreamSFXGraphIR.h"

namespace DreamSFX::V2
{
    class DREAMSFXCORE_API FGraphInstance
    {
    public:
        FGraphInstance();
        ~FGraphInstance();

        bool Prepare(FGraphPtr Graph, int32 SampleRate, FString& Error);
        void Restart();
        bool SetInput(FName Name,double Value);
        void ClearInput(FName Name);
        bool SetState(FName State);
        uint32 GetVoiceStarts(FName Bank) const;
#if WITH_DEV_AUTOMATION_TESTS
        void ObserveProgram(TFunction<void(FName,double)> OnLaunch,
            TFunction<void(const FPortRef&,double,double,double)> OnTarget,
            TFunction<void(FName,int32,FName,int64,int64)> OnGate = {});
#endif
        // Writes stereo PCM, zeroing the portion after the last frame. Returns frames consumed.
        int32 Render(float* Stereo, int32 Frames);
        bool IsFinished() const;
        int64 GetLengthFrames() const;

    private:
        struct FImpl;
        TUniquePtr<FImpl> Impl;
    };
}
