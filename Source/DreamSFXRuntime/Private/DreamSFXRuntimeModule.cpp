#include "Modules/ModuleManager.h"

class FDreamSFXRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FDreamSFXRuntimeModule, DreamSFXRuntime)
