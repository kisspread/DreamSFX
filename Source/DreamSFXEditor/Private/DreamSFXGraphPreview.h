#pragma once

#include "DreamSFXGraphIR.h"

class UMetaSoundSource;

namespace DreamSFX::V2
{
    UMetaSoundSource* BuildGraphPreview(FGraphPtr Graph, FString& Error);
    // Saves a normal MetaSound Source containing the compiled graph as a subobject.
    UMetaSoundSource* SaveGraphSource(FGraphPtr Graph, const FString& LongPackageName, FString& Error,
        bool bForce, bool* bWritten);
}
