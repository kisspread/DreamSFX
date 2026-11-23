#pragma once

#include "DreamSFXGraphIR.h"

namespace DreamSFX::V2
{
    // DSP2 packages persist UTF-8 .dsfx source for compilation at load.
    DREAMSFXCORE_API bool EncodeGraph(const FGraphIR& Graph, TArray<uint8>& Payload);
    DREAMSFXCORE_API FGraphPtr DecodeGraph(const TArray<uint8>& Payload);
}
