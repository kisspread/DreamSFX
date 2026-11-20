#pragma once

#include "DreamSFXGraphIR.h"

namespace DreamSFX::V2
{
    // Asset payload for the compiled graph. The authoring format remains dreamsfx/2.
    DREAMSFXCORE_API bool EncodeGraph(const FGraphIR& Graph, TArray<uint8>& Payload);
    DREAMSFXCORE_API FGraphPtr DecodeGraph(const TArray<uint8>& Payload);
}
