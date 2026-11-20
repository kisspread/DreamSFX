#include "DreamSFXLibraryWatcher.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
bool FDreamSFXLibraryWatcher::Start(const FString& Root)
{
 Stop();Directory=FPaths::ConvertRelativePathToFull(Root);FPaths::NormalizeDirectoryName(Directory);
 auto* Module=FModuleManager::LoadModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
 auto* Watcher=Module?Module->Get():nullptr;if(!Watcher){Directory.Reset();return false;}
 const auto State=Changed;
 const bool Ok=Watcher->RegisterDirectoryChangedCallback_Handle(Directory,IDirectoryWatcher::FDirectoryChanged::CreateLambda([State](const TArray<FFileChangeData>& Changes)
 {
  for(const auto& C:Changes)
   if(C.Action==FFileChangeData::FCA_RescanRequired||C.Filename.EndsWith(TEXT(".dsfx"),ESearchCase::IgnoreCase)) {State->store(true);break;}
 }),Handle,IDirectoryWatcher::IncludeDirectoryChanges);
 if(!Ok){Handle.Reset();Directory.Reset();}return Ok;
}
void FDreamSFXLibraryWatcher::Stop()
{
 if(Handle.IsValid())
  if(auto* Module=FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")))
   if(auto* Watcher=Module->Get())Watcher->UnregisterDirectoryChangedCallback_Handle(Directory,Handle);
 Handle.Reset();Directory.Reset();
 // Detach old callbacks from a subsequent Start even if delivery was queued.
 Changed=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
}
