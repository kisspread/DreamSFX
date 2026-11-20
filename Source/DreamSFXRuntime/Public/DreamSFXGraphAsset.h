#pragma once

#include "DreamSFXGraphIR.h"
#include "IAudioProxyInitializer.h"
#include "UObject/Object.h"
#include "DreamSFXGraphAsset.generated.h"

class FDreamSFXGraphProxy : public Audio::TProxyData<FDreamSFXGraphProxy>
{
public:
    IMPL_AUDIOPROXY_CLASS(FDreamSFXGraphProxy);
    explicit FDreamSFXGraphProxy(DreamSFX::V2::FGraphPtr InGraph) : Graph(MoveTemp(InGraph)) {}
    const DreamSFX::V2::FGraphPtr Graph;
};

/** Compiled graph data owned by the MetaSound Source that plays it. */
UCLASS()
class DREAMSFXRUNTIME_API UDreamSFXGraphAsset : public UObject, public IAudioProxyDataFactory
{
    GENERATED_BODY()

public:
    bool Initialize(DreamSFX::V2::FGraphPtr InGraph, bool bPersist);
    DreamSFX::V2::FGraphPtr GetGraph() const { return Graph; }
    void PostLoad() override;
    TSharedPtr<Audio::IProxyData> CreateProxyData(const Audio::FProxyDataInitParams&) override;

private:
    UPROPERTY()
    TArray<uint8> Payload;

    DreamSFX::V2::FGraphPtr Graph;
};

namespace DreamSFX::V2
{
    DREAMSFXRUNTIME_API FString GraphPackageName(FName Id);
}
