#pragma once
#include "CoreMinimal.h"
#include <atomic>

/** Editor-only recursive source notification. Callback only sets shared state;
 * callers drain/debounce on the game thread. No source reads/writes in callback.
 */
class FDreamSFXLibraryWatcher
{
public:
 ~FDreamSFXLibraryWatcher(){Stop();}
 bool Start(const FString& Root);
 void Stop();
 bool ConsumeChange(){return Changed->exchange(false);}
 bool IsWatching() const{return Handle.IsValid();}
private:
 FString Directory;
 FDelegateHandle Handle;
 TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe> Changed=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
};
