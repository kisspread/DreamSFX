#include "Misc/AutomationTest.h"
#include "DreamSFXSourceSave.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamSFXSourceSaveTest,"DreamSFX.Editor.StagedSourceSave",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDreamSFXSourceSaveTest::RunTest(const FString&)
{
 const FString Dir=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("DreamSFX/Tests/StagedSave-")+FGuid::NewGuid().ToString());IFileManager::Get().MakeDirectory(*Dir,true);const FString File=Dir/TEXT("test.dsfx.json");
 const FString Old=TEXT("{\"schema\":\"dreamsfx/1\"}\r\n"),Draft=TEXT("{\"schema\":\"dreamsfx/1\",\"id\":\"new\"}\n");
 FFileHelper::SaveStringToFile(Old,*File,FFileHelper::EEncodingOptions::ForceUTF8);TArray<uint8> Bytes;FFileHelper::LoadFileToArray(Bytes,*File);
 auto Saved=FDreamSFXSourceSave::Save(File,Old,Draft);if(!TestTrue(TEXT("Staged source save succeeds"),Saved.bSuccess)){AddError(Saved.Error);return false;}
 FString Current;FFileHelper::LoadFileToString(Current,*File);TestEqual(TEXT("New source committed exactly"),Current,Draft);TArray<uint8> Backup;FFileHelper::LoadFileToArray(Backup,*Saved.BackupPath);TestTrue(TEXT("Original backup preserves exact bytes incl encoding/line endings"),Bytes==Backup);
 auto Conflict=FDreamSFXSourceSave::Save(File,Old,TEXT("bad"));TestFalse(TEXT("Stale baseline rejected"),Conflict.bSuccess);TestTrue(TEXT("Conflict creates no misleading backup"),Conflict.BackupPath.IsEmpty());FFileHelper::LoadFileToString(Current,*File);TestEqual(TEXT("Conflict keeps current disk"),Current,Draft);
 FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File,true);auto ReadOnly=FDreamSFXSourceSave::Save(File,Draft,TEXT("bad"));FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File,false);TestFalse(TEXT("Read-only source rejected"),ReadOnly.bSuccess);FFileHelper::LoadFileToString(Current,*File);TestEqual(TEXT("Read-only original remains"),Current,Draft);
 TestFalse(TEXT("Missing source is not silently created"),FDreamSFXSourceSave::Save(Dir/TEXT("missing.dsfx.json"),TEXT(""),Draft).bSuccess);
 TestFalse(TEXT("Non-source target refused"),FDreamSFXSourceSave::Save(Dir/TEXT("asset.uasset"),TEXT(""),Draft).bSuccess);
 TestFalse(TEXT("Over-budget encoded draft refused"),FDreamSFXSourceSave::Save(File,Draft,FString::ChrN(4*1024*1024+1,TCHAR('a'))).bSuccess);
 auto Second=FDreamSFXSourceSave::Save(File,Draft,Old);TestTrue(TEXT("Repeated save succeeds with fresh backup"),Second.bSuccess);TestTrue(TEXT("Backups never overwrite one another"),Saved.BackupPath!=Second.BackupPath&&IFileManager::Get().FileExists(*Saved.BackupPath));
 return true;
}
#endif
