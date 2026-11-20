#include "DreamSFXPreviewSubsystem.h"
#include "Components/AudioComponent.h"
#include "AudioDevice.h"
#include "Editor.h"
#include "MetasoundSource.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace DreamSFXPreviewPrivate
{
 FString Response(bool Ok,const FString& Code,const FString& Message,const FString& Path,bool Playing,float VolumeMultiplier)
 {
  auto O=MakeShared<FJsonObject>(); O->SetBoolField(TEXT("ok"),Ok); O->SetStringField(TEXT("code"),Code);
  O->SetStringField(TEXT("message"),Message); O->SetStringField(TEXT("sourceAsset"),Path); O->SetBoolField(TEXT("playing"),Playing);
  O->SetNumberField(TEXT("volumeMultiplier"),VolumeMultiplier);
  FString Json; auto Writer=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
  FJsonSerializer::Serialize(O,Writer); return Json;
 }
}
void UDreamSFXPreviewSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
 Super::Initialize(Collection);LoadConfig();
 if(!FMath::IsFinite(PreviewVolumeMultiplier)||PreviewVolumeMultiplier<0||PreviewVolumeMultiplier>16)PreviewVolumeMultiplier=1;
}
bool UDreamSFXPreviewSubsystem::SetPreviewVolumeMultiplier(float Multiplier)
{
 if(!FMath::IsFinite(Multiplier)||Multiplier<0||Multiplier>16)return false;
 PreviewVolumeMultiplier=Multiplier;if(Component)Component->SetVolumeMultiplier(Multiplier);return true;
}
UMetaSoundSource* UDreamSFXPreviewSubsystem::CachedSource(const FString& Key) const
{const int32 Index=PreparedKeys.IndexOfByKey(Key);return PreparedSources.IsValidIndex(Index)?PreparedSources[Index]:nullptr;}
void UDreamSFXPreviewSubsystem::CacheSource(const FString& Key,UMetaSoundSource* Source)
{
 if(!Source)return;const int32 Existing=PreparedKeys.IndexOfByKey(Key);
 if(Existing!=INDEX_NONE){PreparedSources[Existing]=Source;return;}
 if(PreparedKeys.Num()>=16){PreparedKeys.RemoveAt(0);PreparedSources.RemoveAt(0);}
 PreparedKeys.Add(Key);PreparedSources.Add(Source);
}
FString UDreamSFXPreviewSubsystem::PreviewPreparedSource(UMetaSoundSource* Source,int32 Iterations)
{
 using namespace DreamSFXPreviewPrivate;
 if(!Source)return Response(false,TEXT("DSFX7102"),TEXT("Preview Source unavailable"),CurrentSource,Component&&Component->IsPlaying(),PreviewVolumeMultiplier);
 const FString SourceObjectPath=Source->GetPathName();
 Source->InitResources();
 const bool HasLoop=Source->IsParameterValid(FAudioParameter(FName(TEXT("Loop")),Iterations==0));
 if(Iterations!=0&&Iterations!=1)return Response(false,TEXT("DSFX7104"),TEXT("Preview supports one-shot or indefinite loop"),CurrentSource,Component&&Component->IsPlaying(),PreviewVolumeMultiplier);
 if(Iterations==0&&!HasLoop)return Response(false,TEXT("DSFX7104"),TEXT("Source needs current loop inputs"),CurrentSource,Component&&Component->IsPlaying(),PreviewVolumeMultiplier);
 UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
 if(!World||!World->GetAudioDeviceRaw()) return Response(false,TEXT("DSFX7103"),TEXT("Editor audio device unavailable"),CurrentSource,Component&&Component->IsPlaying(),PreviewVolumeMultiplier);
 FAudioDevice::FCreateComponentParams Params(World);
 Params.bAutoDestroy=false;
 UAudioComponent* NewComponent=FAudioDevice::CreateComponent(Source,Params);
 if(!NewComponent) return Response(false,TEXT("DSFX7103"),TEXT("Cannot create preview component"),CurrentSource,Component&&Component->IsPlaying(),PreviewVolumeMultiplier);
 NewComponent->bIsUISound=true;
 NewComponent->bAllowSpatialization=false;
 NewComponent->bIsPreviewSound=true;
 StopPreview();
 Component=NewComponent; CurrentSource=SourceObjectPath;
 if(HasLoop)Component->SetBoolParameter(TEXT("Loop"),Iterations==0);
 Component->SetVolumeMultiplier(PreviewVolumeMultiplier);
 Component->Play();
 return Response(true,TEXT("DSFX0000"),TEXT("Preview started through MetaSound; hardware audibility is not inferred from component state"),CurrentSource,Component->IsPlaying(),PreviewVolumeMultiplier);
}
FString UDreamSFXPreviewSubsystem::StopPreview()
{
 if(Component) { Component->Stop(); Component->DestroyComponent(); Component=nullptr; }
 CurrentSource.Reset();
 return DreamSFXPreviewPrivate::Response(true,TEXT("DSFX0000"),TEXT("DreamSFX preview stopped"),FString(),false,PreviewVolumeMultiplier);
}
FString UDreamSFXPreviewSubsystem::GetPreviewStatus() const
{
 return DreamSFXPreviewPrivate::Response(true,TEXT("DSFX0000"),TEXT("Component transport status, not an audio meter"),CurrentSource,Component&&Component->IsPlaying(),PreviewVolumeMultiplier);
}
void UDreamSFXPreviewSubsystem::Deinitialize()
{
 StopPreview();PreparedSources.Reset();PreparedKeys.Reset(); Super::Deinitialize();
}
