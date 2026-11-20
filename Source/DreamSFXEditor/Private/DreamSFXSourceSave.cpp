#include "DreamSFXSourceSave.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
FDreamSFXSourceSaveResult FDreamSFXSourceSave::Save(const FString& Path,const FString& Expected,const FString& Draft)
{
 FDreamSFXSourceSaveResult R;auto& Files=IFileManager::Get();const FString Target=FPaths::ConvertRelativePathToFull(Path);
 if(!Target.EndsWith(TEXT(".dsfx"),ESearchCase::IgnoreCase)){R.Error=TEXT("Save requires an existing .dsfx source");return R;}
 if(Files.FileSize(*Target)<0||Files.FileSize(*Target)>4*1024*1024||FTCHARToUTF8(*Draft).Length()>4*1024*1024){R.Error=TEXT("Source missing/unreadable or source/draft exceeds 4MiB");return R;}
 FString Disk;if(!FFileHelper::LoadFileToString(Disk,*Target)||Disk!=Expected){R.Error=TEXT("External source change detected; no write performed");return R;}
 if(Files.IsReadOnly(*Target)){R.Error=TEXT("Source is read-only; no write performed");return R;}
 const FString Token=FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString BackupDir=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("DreamSFX/SourceBackups"));
 if(!Files.MakeDirectory(*BackupDir,true)){R.Error=TEXT("Cannot create source backup directory; no write performed");return R;}
 const FTCHARToUTF8 Bytes(*Target);
 R.BackupPath=BackupDir/TEXT("Source-")+FMD5::HashBytes(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length())+TEXT("-")+Token+TEXT(".bak");
 // Backup original bytes, not a re-encoded approximation of the baseline text.
 if(Files.Copy(*R.BackupPath,*Target,false,false,false)!=COPY_OK){R.Error=TEXT("Cannot preserve original source backup; no write performed");R.BackupPath.Reset();return R;}
 const FString Stage=Target+TEXT(".stage-")+Token;
 auto Fail=[&](const TCHAR* Error){R.Error=FString(Error)+TEXT(". Original backup retained at ")+R.BackupPath;return R;};
 if(!FFileHelper::SaveStringToFile(Draft,*Stage,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&Files,FILEWRITE_NoReplaceExisting))return Fail(TEXT("Staged write failed; target unchanged"));
 FString Readback;if(!FFileHelper::LoadFileToString(Readback,*Stage)||Readback!=Draft)return Fail(TEXT("Staged readback mismatch; target unchanged"));
 if(!FFileHelper::LoadFileToString(Disk,*Target)||Disk!=Expected)return Fail(TEXT("Source changed during staging; target not replaced"));
 // Move uses platform replacement semantics. Does not prove crash-atomicity or
 // close the race between last baseline read and replacement by another writer.
 if(!Files.Move(*Target,*Stage,true,false,false,true))return Fail(TEXT("Staged replacement failed; backup available for recovery"));
 if(!FFileHelper::LoadFileToString(Readback,*Target)||Readback!=Draft)return Fail(TEXT("Post-save readback mismatch; inspect target and backup before retrying"));
 R.bSuccess=true;return R;
}
