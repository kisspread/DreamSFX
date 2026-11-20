#pragma once

#include "DreamSFXGraphIR.h"

namespace DreamSFX::V2
{
    struct FDiagnostic
    {
        FString Code;
        FString Pointer;
        FString Message;
    };

    struct FCompileResult
    {
        FGraphPtr Graph;
        TArray<FDiagnostic> Errors;
    };

    struct FEditableNumber
    {
        FName Node, Name;
        double Value, Minimum, Maximum;
        FString Unit;
        bool bInteger;
    };

    class DREAMSFXCORE_API FGraphCompiler
    {
    public:
        static FCompileResult Compile(const FString& Text);
        static TArray<FEditableNumber> EditableNumbers(const FGraphIR& Graph);
    };
}
