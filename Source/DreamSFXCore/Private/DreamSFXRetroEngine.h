#pragma once
#include "DreamSFXInstrument.h"
#include "EightBitSynthDSP.h"

/** Legacy square/noise DSP with host gates and bounded 5-ms stealing tails. */
class FDreamSFXRetroEngine final : public IDreamSFXInstrumentEngine
{
 struct FVoice { bool Active=false,Held=false,Releasing=false;int32 Id=0,Delay=0,Age=0,Release=0,StealRemaining=0,StealDelay=0;FEightBitSynthDSP DSP;float Velocity=0,ReleaseLevel=1; };
 FVoice Voices[16],Retiring[16];
 int32 Rate=44100,Limit=16,Attack=176,ReleaseLength=2646,StealLength=220,NextRetiring=0;
 float DutyCycle=.5f;float Gain=1,PitchRatio=1;bool Pedal=false,Noise=false;uint32 PendingSeed=1337;
 float Level(const FVoice& V) const{return FMath::Min(1.0f,float(V.Age)/Attack);}
 void ReleaseVoice(FVoice& V){if(!V.Releasing){V.Releasing=true;V.Release=ReleaseLength;V.ReleaseLevel=Level(V);}}
public:
 FDreamSFXRetroEngine(float GainDb,int32 VoiceLimit,bool InNoise=false,float InDutyCycle=.5f):Limit(FMath::Clamp(VoiceLimit,1,16)),DutyCycle(InDutyCycle),Gain(FMath::Pow(10.0f,GainDb/20.0f)),Noise(InNoise){}
 void Prepare(int32 SampleRate) override{Rate=FMath::Max(1,SampleRate);Attack=FMath::Max(1,FMath::RoundToInt(Rate*.004));ReleaseLength=FMath::Max(1,FMath::RoundToInt(Rate*.06));StealLength=FMath::Max(2,FMath::RoundToInt(Rate*.005));ResetVoices();}
 void ResetVoices() override{for(auto& V:Voices)V=FVoice();for(auto& V:Retiring)V=FVoice();NextRetiring=0;PendingSeed=1337;Pedal=false;PitchRatio=1;}
 void SetPitchRatio(float Ratio) override{PitchRatio=Ratio;}
 bool HasActiveVoices() const override{for(const auto& V:Voices)if(V.Active)return true;for(const auto& V:Retiring)if(V.Active)return true;return false;}
 bool CanStartVoice() const override{for(int32 I=0;I<Limit;++I)if(!Voices[I].Active)return true;return false;}
 void NoteOnSeeded(int32 Id,int32 Midi,float Velocity,int32 Offset,uint32 Seed) override{PendingSeed=Seed;NoteOn(Id,Midi,Velocity,Offset);PendingSeed=1337;}
 void NoteOn(int32 Id,int32 Midi,float Velocity,int32 Offset) override
 {
  int32 Slot=INDEX_NONE;for(int32 I=0;I<Limit;++I)if(!Voices[I].Active){Slot=I;break;}
  if(Slot==INDEX_NONE){Slot=0;for(int32 I=1;I<Limit;++I)if((Voices[I].Releasing&&!Voices[Slot].Releasing)||(Voices[I].Releasing==Voices[Slot].Releasing&&Voices[I].Age>Voices[Slot].Age))Slot=I;}
  if(Voices[Slot].Active&&Voices[Slot].Age>0)
  {
   int32 Tail=INDEX_NONE;for(int32 I=0;I<16;++I)if(!Retiring[I].Active){Tail=I;break;}
   if(Tail==INDEX_NONE){Tail=NextRetiring;NextRetiring=(NextRetiring+1)%16;}
   Retiring[Tail]=Voices[Slot];Retiring[Tail].Held=false;Retiring[Tail].StealRemaining=StealLength;Retiring[Tail].StealDelay=FMath::Max(0,Offset);
  }
  auto& V=Voices[Slot];V=FVoice();V.Active=V.Held=true;V.Id=Id;V.Delay=FMath::Max(0,Offset);V.Velocity=FMath::Clamp(Velocity,0.0f,1.0f);
  V.DSP.InitHeldToneHz(float(440.0*FMath::Pow(2.0,(FMath::Clamp(Midi,0,127)-69)/12.0)),Noise,int32(PendingSeed&0x7fffffffu),DutyCycle);
 }
 void NoteOff(int32 Id,bool Sustain) override{for(auto& V:Voices)if(V.Active&&V.Id==Id){V.Held=false;if(!Sustain&&!Pedal)ReleaseVoice(V);}}
 void SetSustainPedal(bool Down) override{Pedal=Down;if(!Down)for(auto& V:Voices)if(V.Active&&!V.Held)ReleaseVoice(V);}
 void Render(float* Out,int32 Frames) override
 {
  if(!Out||Frames<=0)return;
  auto RenderVoice=[&](FVoice& V)
  {
   if(!V.Active)return;
   for(int32 F=0;F<Frames;++F)
   {
    if(V.Delay>0){--V.Delay;continue;}
    const float Envelope=V.Releasing?V.ReleaseLevel*float(V.Release)/ReleaseLength:Level(V);
    // Held oscillator only: musical gate/release remain on the outer host clock.
    float Raw=0;int32 Finished=INDEX_NONE;V.DSP.RenderBlock(float(Rate)/PitchRatio,&Raw,1,Finished);
    const bool Fading=V.StealRemaining>0&&V.StealDelay==0;
    const float Fade=Fading?float(V.StealRemaining-1)/(StealLength-1):1.0f;
    const float Sample=Raw*Envelope*V.Velocity*Gain*Fade;Out[F*2]+=Sample;Out[F*2+1]+=Sample;++V.Age;
    const bool EndTail=Fading&&--V.StealRemaining==0;if(V.StealDelay>0)--V.StealDelay;
    const bool EndRelease=V.Releasing&&--V.Release<=0;
    if(EndTail||EndRelease){V=FVoice();break;}
   }
  };
  for(auto& V:Voices)RenderVoice(V);for(auto& V:Retiring)RenderVoice(V);
 }
 float GetMaxTailSeconds() const override{return .061f;}
 void Advance(int32 Frames) override{float Scratch[2];for(int32 I=0;I<Frames;++I){Scratch[0]=Scratch[1]=0;Render(Scratch,1);}}
};
