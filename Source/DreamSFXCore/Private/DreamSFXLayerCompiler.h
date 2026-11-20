#pragma once

#include "DreamSFXGraphCompiler.h"
#include "Dom/JsonObject.h"

namespace DreamSFX::V2
{
    // Authoring sugar only. The ordinary typed graph compiler validates the expanded nodes and wires.
    bool LowerLayers(const TSharedPtr<FJsonObject>& Root, TArray<FDiagnostic>& Errors, TMap<FString, FString>& SourcePaths);
}
