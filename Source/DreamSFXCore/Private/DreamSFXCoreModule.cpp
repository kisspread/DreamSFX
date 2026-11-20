#include "Modules/ModuleManager.h"
#include "DreamSFXOscillatorTable.h"

/**
 * DreamSFXCore hosts algorithm-only code (timeline, engines, baked audio).
 * It has no UObject reflection of its own; the module exists so the runtime,
 * MetaSound and editor modules can link against it.
 */
class FDreamSFXCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override { DreamSFXOscillatorTable::Initialize(); }
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FDreamSFXCoreModule, DreamSFXCore)
