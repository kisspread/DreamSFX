#pragma once

#include "Commandlets/Commandlet.h"
#include "DreamSFXGraphAssetsCommandlet.generated.h"

UCLASS()
class UDreamSFXGraphAssetsCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    int32 Main(const FString& Params) override;
};
