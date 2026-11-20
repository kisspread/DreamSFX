#include "Modules/ModuleManager.h"
#include "GameDelegates.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "SDreamSFXSoundEditor.h"
#include "ToolMenus.h"

class FDreamSFXEditorModule : public IModuleInterface
{
public:
 void StartupModule() override
 {
  if(IsRunningCommandlet())return;
  FGlobalTabmanager::RegisterDefaultTabWindowSize(FName(TEXT("DreamSFXWorkbench")),FVector2D(1440,900));
  FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TEXT("DreamSFXWorkbench"),FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
  {
    auto Workbench=SNew(SDreamSFXSoundEditor);
    return SNew(SDockTab).TabRole(ETabRole::NomadTab)
     .OnCanCloseTab(SDockTab::FCanCloseTab::CreateLambda([Weak=TWeakPtr<SDreamSFXSoundEditor>(Workbench)]()
     { auto P=Weak.Pin(); return !P||P->CanClose(); }))[Workbench];
   }))
   .SetDisplayName(FText::FromString(TEXT("DreamSFX Sounds"))).SetMenuType(ETabSpawnerMenuType::Hidden);
  UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,&FDreamSFXEditorModule::RegisterMenus));
 }
 void RegisterMenus()
 {
  FToolMenuOwnerScoped Owner(this);
  auto* Menu=UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
  auto& Section=Menu->FindOrAddSection(TEXT("DreamSFX"));
  Section.AddMenuEntry(TEXT("OpenDreamSFX"),FText::FromString(TEXT("DreamSFX Sounds")),FText::FromString(TEXT("Audition sound definitions and tune parameter instances")),FSlateIcon(),
   FUIAction(FExecuteAction::CreateLambda([](){FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("DreamSFXWorkbench")));})));
 }
 void ShutdownModule() override
 {
  if(IsRunningCommandlet())return;
  UToolMenus::UnRegisterStartupCallback(this); UToolMenus::UnregisterOwner(this);
  FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TEXT("DreamSFXWorkbench"));
 }
};
IMPLEMENT_MODULE(FDreamSFXEditorModule,DreamSFXEditor)
