#pragma once
#include "DreamSFXGraphCompiler.h"

namespace DreamSFX::V2::Dsl
{
    struct FStatement
    {
        FString Text;
        int32 Line=1, Column=1;
        bool Block=false;
        TArray<FStatement> Children;
    };
    bool Parse(const FString& Text,TArray<FStatement>& Tree,FDiagnostic& Error);
    FDiagnostic Expected(const FStatement& At,const FString& Message,int32 ColumnOffset=0);
}
