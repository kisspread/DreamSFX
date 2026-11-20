#pragma once
#include "CoreMinimal.h"
struct FDreamSFXSourceSaveResult
{
 bool bSuccess=false;
 FString Error,BackupPath;
};
/** Editor-thread optimistic save: baseline check, unique backup + staged data,
 * readback, recheck and replace. Not a concurrent-writer filesystem lock/CAS.
 */
class FDreamSFXSourceSave
{
public:
 static FDreamSFXSourceSaveResult Save(const FString& Path,const FString& Expected,const FString& Draft);
};
