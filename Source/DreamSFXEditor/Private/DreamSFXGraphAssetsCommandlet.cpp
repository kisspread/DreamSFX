#include "DreamSFXGraphAssetsCommandlet.h"
#include "DreamSFXSoundDocument.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

int32 UDreamSFXGraphAssetsCommandlet::Main(const FString& Params)
{
    FString Root;
    if (!FParse::Value(*Params, TEXT("SourceDir="), Root)) Root = FPaths::ProjectDir() / TEXT("DSFX");
    auto Files = MakeShared<FDreamSFXLibrarySnapshot, ESPMode::ThreadSafe>();
    if (!Files->Load(Root) || Files->Entries.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("DreamSFX: %s (%s)"), *Files->Error, *Root);
        return 1;
    }
    TArray<FDreamSFXSoundDocument> Documents;
    for (const auto& Entry : Files->Entries)
    {
        FString Error;
        if (!Documents.AddDefaulted_GetRef().LoadFromSnapshot(Files, Entry.Id, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("DreamSFX: %s: %s"), *Entry.Path, *Error);
            return 1;
        }
    }
    if (FParse::Param(*Params,TEXT("ValidateOnly")))
    {
        for (const auto& Document : Documents)
        {
            const auto Graph=Document.GetGraph();
            int32 Audio=0,Control=0;
            for(const auto& Edge:Graph->Connections){Audio+=Edge.Kind==DreamSFX::V2::EPortKind::Audio;Control+=Edge.Kind==DreamSFX::V2::EPortKind::Control;}
            UE_LOG(LogTemp,Display,TEXT("DSFX_DSL_VALID id=%s nodes=%d audio=%d control=%d timeline=%d states=%d inputs=%d seconds=%.3f"),
                *Document.Id(),Graph->Nodes.Num(),Audio,Control,Graph->Timeline.Num(),Graph->StateNames.Num(),Graph->Inputs.Num(),Graph->DurationSeconds);
        }
        return 0;
    }
    for (const auto& Document : Documents)
    {
        FString Error;
        if (!Document.SaveAsset(Error))
        {
            UE_LOG(LogTemp, Error, TEXT("DreamSFX: %s: %s"), *Document.Id(), *Error);
            return 1;
        }
        UE_LOG(LogTemp, Display, TEXT("DSFX_V2_SAVED id=%s"), *Document.Id());
    }
    return 0;
}
