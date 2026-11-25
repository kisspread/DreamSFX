#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphArchive.h"

DEFINE_LOG_CATEGORY_STATIC(LogDreamSFXGraphAsset, Log, All);

bool UDreamSFXGraphAsset::Initialize(DreamSFX::V2::FGraphPtr InGraph, bool bPersist, int32 BuildVersion)
{
    if (!InGraph) return false;
    if (bPersist && !DreamSFX::V2::EncodeGraph(*InGraph, Payload)) return false;
    if (bPersist) SourceBuildVersion = BuildVersion;
    Graph = MoveTemp(InGraph);
    return true;
}

bool UDreamSFXGraphAsset::MatchesBuild(const DreamSFX::V2::FGraphIR& Candidate, int32 BuildVersion) const
{
    if (SourceBuildVersion != BuildVersion) return false;
    TArray<uint8> CandidatePayload;
    return DreamSFX::V2::EncodeGraph(Candidate, CandidatePayload) && CandidatePayload == Payload;
}

void UDreamSFXGraphAsset::PostLoad()
{
    Super::PostLoad();
    Graph = DreamSFX::V2::DecodeGraph(Payload);
    if (!Graph)
        UE_LOG(LogDreamSFXGraphAsset, Error, TEXT("DreamSFX could not read compiled graph in %s"), *GetPathName());
}

TSharedPtr<Audio::IProxyData> UDreamSFXGraphAsset::CreateProxyData(const Audio::FProxyDataInitParams&)
{
    if (!Graph) return nullptr;
    return MakeShared<FDreamSFXGraphProxy>(Graph);
}

namespace DreamSFX::V2
{
    FString GraphPackageName(FName Id)
    {
        // A reversible encoding avoids collisions between dots, hyphens, and underscores.
        FString Name(TEXT("MS_"));
        for (const TCHAR Character : Id.ToString().ToLower())
        {
            if (Character == TEXT('.')) Name += TEXT("_d");
            else if (Character == TEXT('-')) Name += TEXT("_h");
            else if (Character == TEXT('_')) Name += TEXT("_u");
            else Name.AppendChar(Character);
        }
        return TEXT("/Game/DreamSFX/Graphs/") + Name;
    }
}
