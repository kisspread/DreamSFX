#include "DreamSFXGraphArchive.h"
#include "DreamSFXGraphCompiler.h"
#include "DreamSFXSoundProgram.h"

namespace DreamSFX::V2
{
    namespace
    {
        constexpr uint8 ProgramMagic[] = {'D', 'S', 'P', '2'};
        constexpr int32 HeaderSize = UE_ARRAY_COUNT(ProgramMagic);
        constexpr int32 PayloadLimit = 16 * 1024 * 1024;
    }

    bool EncodeGraph(const FGraphIR& Graph, TArray<uint8>& Payload)
    {
        if (!Graph.Program) return false;

        // Compile the embedded source once at load so each playing instance can own its state.
        FTCHARToUTF8 UTF8(*Graph.Program->Source);
        if (UTF8.Length() > PayloadLimit - HeaderSize) return false;

        Payload.Reset(HeaderSize + UTF8.Length());
        Payload.Append(ProgramMagic, HeaderSize);
        Payload.Append(reinterpret_cast<const uint8*>(UTF8.Get()), UTF8.Length());
        return true;
    }

    FGraphPtr DecodeGraph(const TArray<uint8>& Payload)
    {
        if (Payload.Num() <= HeaderSize || Payload.Num() > PayloadLimit ||
            FMemory::Memcmp(Payload.GetData(), ProgramMagic, HeaderSize) != 0)
        {
            return {};
        }

        const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Payload.GetData() + HeaderSize),
            Payload.Num() - HeaderSize);
        return FGraphCompiler::Compile(FString(Text.Length(), Text.Get())).Graph;
    }
}
