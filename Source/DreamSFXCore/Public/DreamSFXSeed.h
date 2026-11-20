#pragma once
#include "CoreMinimal.h"

/** Version 1 seed derivation. Explicit UTF-8 FNV-1a and little-endian ordinal
 * bytes; does not depend on FName hashes, track array index or global RNG.
 * Output restricted to 31 bits for the legacy DSP's nonnegative seed contract.
 */
namespace DreamSFXSeed
{
 inline uint32 Track(uint32 Root,const FString& Id)
 {
  uint32 H=2166136261u;
  for(int32 I=0;I<4;++I) {H^=(Root>>(I*8))&255u;H*=16777619u;}
  FTCHARToUTF8 Bytes(*Id);
  for(int32 I=0;I<Bytes.Length();++I) {H^=uint8(Bytes.Get()[I]);H*=16777619u;}
  return H;
 }
 inline uint32 Note(uint32 TrackSeed,uint32 Ordinal)
 {
  uint32 H=TrackSeed;
  for(int32 I=0;I<4;++I) {H^=(Ordinal>>(I*8))&255u;H*=16777619u;}
  return H&0x7fffffffu;
 }
}
