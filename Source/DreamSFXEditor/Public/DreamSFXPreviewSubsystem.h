#pragma once
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "DreamSFXPreviewSubsystem.generated.h"
class UAudioComponent;
class UMetaSoundSource;

/** Dedicated preview ownership: never stops another editor tool's preview. */
UCLASS(Config=EditorPerProjectUserSettings)
class DREAMSFXEDITOR_API UDreamSFXPreviewSubsystem : public UEditorSubsystem
{
 GENERATED_BODY()
public:
 FString PreviewPreparedSource(UMetaSoundSource* Source,int32 Iterations);
 UMetaSoundSource* CachedSource(const FString& Key) const;
 void CacheSource(const FString& Key,UMetaSoundSource* Source);
 UFUNCTION(BlueprintCallable,Category="DreamSFX|Preview")
 FString StopPreview();
 UFUNCTION(BlueprintCallable,Category="DreamSFX|Preview")
 FString GetPreviewStatus() const;
 /** Global monitor gain; never changes source/instance parameters. */
 UFUNCTION(BlueprintCallable,Category="DreamSFX|Preview")
 bool SetPreviewVolumeMultiplier(float Multiplier);
 UFUNCTION(BlueprintPure,Category="DreamSFX|Preview")
 float GetPreviewVolumeMultiplier() const{return PreviewVolumeMultiplier;}
 void SavePreviewSettings(){SaveConfig();}
 virtual void Initialize(FSubsystemCollectionBase& Collection) override;
 virtual void Deinitialize() override;
private:
 friend class FDreamSFXV2AuditionTransport;
 UPROPERTY(Transient) TObjectPtr<UAudioComponent> Component;
 FString CurrentSource;
 UPROPERTY(Config) float PreviewVolumeMultiplier=1.f;
 UPROPERTY(Transient) TArray<TObjectPtr<UMetaSoundSource>> PreparedSources;
 TArray<FString> PreparedKeys;
};
