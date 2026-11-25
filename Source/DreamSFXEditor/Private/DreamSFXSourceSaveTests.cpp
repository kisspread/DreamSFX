#include "Misc/AutomationTest.h"
#include "DreamSFXSoundDocument.h"
#include "DreamSFXSourceSave.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamSFXSourceSaveTest,"DreamSFX.Editor.StagedSourceSave",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDreamSFXSourceSaveTest::RunTest(const FString&)
{
 const FString Dir=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("DreamSFX/Tests/StagedSave-")+FGuid::NewGuid().ToString());IFileManager::Get().MakeDirectory(*Dir,true);const FString File=Dir/TEXT("test.instance.dsfx");
 const FString Old=TEXT("instance test.instance of test.parent\r\nlevel = 0.2\r\n");
 const FString Draft=TEXT("instance test.instance of test.parent\nlevel = 0.4\n");
 FFileHelper::SaveStringToFile(Old,*File,FFileHelper::EEncodingOptions::ForceUTF8);TArray<uint8> Bytes;FFileHelper::LoadFileToArray(Bytes,*File);
 auto Saved=FDreamSFXSourceSave::Save(File,Old,Draft);if(!TestTrue(TEXT("Staged source save succeeds"),Saved.bSuccess)){AddError(Saved.Error);return false;}
 FString Current;FFileHelper::LoadFileToString(Current,*File);TestEqual(TEXT("New source committed exactly"),Current,Draft);TArray<uint8> Backup;FFileHelper::LoadFileToArray(Backup,*Saved.BackupPath);TestTrue(TEXT("Original backup preserves exact bytes incl encoding/line endings"),Bytes==Backup);
 auto Conflict=FDreamSFXSourceSave::Save(File,Old,TEXT("bad"));TestFalse(TEXT("Stale baseline rejected"),Conflict.bSuccess);TestTrue(TEXT("Conflict creates no misleading backup"),Conflict.BackupPath.IsEmpty());FFileHelper::LoadFileToString(Current,*File);TestEqual(TEXT("Conflict keeps current disk"),Current,Draft);
 FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File,true);auto ReadOnly=FDreamSFXSourceSave::Save(File,Draft,TEXT("bad"));FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File,false);TestFalse(TEXT("Read-only source rejected"),ReadOnly.bSuccess);FFileHelper::LoadFileToString(Current,*File);TestEqual(TEXT("Read-only original remains"),Current,Draft);
 TestFalse(TEXT("Missing source is not silently created"),FDreamSFXSourceSave::Save(Dir/TEXT("missing.instance.dsfx"),TEXT(""),Draft).bSuccess);
 TestFalse(TEXT("Non-source target refused"),FDreamSFXSourceSave::Save(Dir/TEXT("asset.uasset"),TEXT(""),Draft).bSuccess);
 TestFalse(TEXT("Over-budget encoded draft refused"),FDreamSFXSourceSave::Save(File,Draft,FString::ChrN(4*1024*1024+1,TCHAR('a'))).bSuccess);
 auto Second=FDreamSFXSourceSave::Save(File,Draft,Old);TestTrue(TEXT("Repeated save succeeds with fresh backup"),Second.bSuccess);TestTrue(TEXT("Backups never overwrite one another"),Saved.BackupPath!=Second.BackupPath&&IFileManager::Get().FileExists(*Saved.BackupPath));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamSFXInstanceDeclarationTest, "DreamSFX.Editor.InstanceDeclaration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDreamSFXInstanceDeclarationTest::RunTest(const FString&)
{
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("DreamSFX/Tests") / FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*Dir, true);
    const FString MotherPath = Dir / TEXT("mother.dsfx");
    const FString ChildPath = Dir / TEXT("child.dsfx");
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*MotherPath);
        IFileManager::Get().Delete(*ChildPath);
        IFileManager::Get().DeleteDirectory(*Dir);
    };
    const FString Mother = TEXT("# Example: sound test.mother\r\n\r\n  sound  test.mother\r\npreview .1 s\r\n")
        TEXT("input pitch range 200 800 default 440\r\nvoice tone { out=sine(hz=pitch) }\r\n")
        TEXT("start { play tone }\r\noutput tone.out\r\n");
    const auto Write = [](const FString& Path, const FString& Text)
    {
        return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    };
    if (!TestTrue(TEXT("Create unambiguous source files"), Write(MotherPath, Mother) &&
        Write(ChildPath, TEXT("instance test.child of test.mother\npitch = 640\n"))))
        return false;

    FString Error;
    FDreamSFXSoundDocument MotherDoc, ChildDoc;
    if (!TestTrue(TEXT("Mother loads"), MotherDoc.Load(Dir, TEXT("test.mother"), Error)) ||
        !TestTrue(TEXT("Child loads"), ChildDoc.Load(Dir, TEXT("test.child"), Error)))
    {
        AddError(Error);
        return false;
    }
    TestEqual(TEXT("Mother graph keeps its ID"), MotherDoc.GetGraph()->Id, FName(TEXT("test.mother")));
    TestEqual(TEXT("Instance graph gets its own ID"), ChildDoc.GetGraph()->Id, FName(TEXT("test.child")));
    TestEqual(TEXT("Child binds its pitch"), ChildDoc.GetGraph()->Inputs[0].Default, 640.);
    TestEqual(TEXT("Mother keeps its original pitch"), MotherDoc.GetGraph()->Inputs[0].Default, 440.);

    if (!TestTrue(TEXT("Replace instance with an invalid binding"),
        Write(ChildPath, TEXT("instance test.child of test.mother\n\n# pitch in Hz\n  pitch = 900\n"))))
        return false;
    auto Snapshot = MakeShared<FDreamSFXLibrarySnapshot, ESPMode::ThreadSafe>();
    if (!TestTrue(TEXT("Load the source headers"), Snapshot->Load(Dir))) return false;
    FDreamSFXSourceDiagnostic Issue;
    TestFalse(TEXT("Out-of-range instance binding rejected"),
        ChildDoc.LoadFromSnapshot(Snapshot, TEXT("test.child"), Error, {}, {}, &Issue));
    TestEqual(TEXT("Binding error line includes comments and blank lines"), Issue.Line, 4);
    TestEqual(TEXT("Binding error column includes indentation"), Issue.Column, 3);
    return !HasAnyErrors();
}
#endif
