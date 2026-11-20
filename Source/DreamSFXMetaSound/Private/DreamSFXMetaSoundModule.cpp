#include "Modules/ModuleManager.h"
#include "MetasoundFrontendModuleRegistrationMacros.h"

class FDreamSFXMetaSoundModule final : public IModuleInterface
{
public:
    void StartupModule() override { METASOUND_REGISTER_ITEMS_IN_MODULE }
    void ShutdownModule() override { METASOUND_UNREGISTER_ITEMS_IN_MODULE }
};

METASOUND_IMPLEMENT_MODULE_REGISTRATION_LIST
IMPLEMENT_MODULE(FDreamSFXMetaSoundModule, DreamSFXMetaSound)
