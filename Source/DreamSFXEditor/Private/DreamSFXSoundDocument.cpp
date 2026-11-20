#include "DreamSFXSoundDocument.h"
#include "DreamSFXGraphPreview.h"
#include "DreamSFXGraphAsset.h"
#include "DreamSFXPreviewSubsystem.h"
#include "DreamSFXSourceSave.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"


namespace
{
    bool ValidId(const FString& Id)
    {
        if (Id.IsEmpty() || Id.Len() > 96) return false;
        for (const TCHAR Ch : Id)
            if (!(Ch >= 'a' && Ch <= 'z') && !(Ch >= '0' && Ch <= '9') && Ch != '-' && Ch != '_' && Ch != '.') return false;
        return true;
    }

    DreamSFX::V2::FGraphPtr CompileGraph(const FString& Text, FString& Error)
    {
        auto Result = DreamSFX::V2::FGraphCompiler::Compile(Text);
        if (!Result.Graph)
            for (const auto& Failure : Result.Errors)
                Error += Failure.Code + TEXT(" ") + Failure.Pointer + TEXT(" ") + Failure.Message + TEXT("\n");
        return Result.Graph;
    }

    TArray<FString> Statements(const FString& Text)
    {
        TArray<FString> Lines,Result;Text.ParseIntoArrayLines(Lines);
        for(auto S:Lines)
        {
            int32 Comment;if(S.FindChar('#',Comment))S.LeftInline(Comment);
            S.TrimStartAndEndInline();if(!S.IsEmpty())Result.Add(MoveTemp(S));
        }
        return Result;
    }
    bool Header(const FString& Text,FString& Id,FString& Parent)
    {
        TArray<FString> Words;
        int32 Start=0;
        while(Start<Text.Len())
        {
            int32 End=Start;while(End<Text.Len()&&Text[End]!='\n')++End;
            FString Line=Text.Mid(Start,End-Start);Start=End+1;
            int32 Comment;if(Line.FindChar('#',Comment))Line.LeftInline(Comment);
            Line.ParseIntoArrayWS(Words);if(!Words.IsEmpty())break;
        }
        if(Words.Num()==2&&Words[0]==TEXT("sound")&&ValidId(Words[1])){Id=Words[1];Parent.Reset();return true;}
        if(Words.Num()==4&&Words[0]==TEXT("instance")&&Words[2]==TEXT("of")&&ValidId(Words[1])&&ValidId(Words[3]))
        {Id=Words[1];Parent=Words[3];return true;}
        return false;
    }
    FString InstanceText(const FString& Id,const FString& Mother,const TMap<FName,double>& Values)
    {
        FString Out=TEXT("instance ")+Id+TEXT(" of ")+Mother+TEXT("\n");
        TArray<FName> Keys;Values.GetKeys(Keys);Keys.Sort(FNameLexicalLess());
        for(FName K:Keys)Out+=FString::Printf(TEXT("%s = %.17g\n"),*K.ToString(),Values[K]);
        return Out;
    }
    bool MakeTextFile(const FString& Path, const FString& Text, FString& Error)
    {
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true) ||
            !FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
                &IFileManager::Get(), FILEWRITE_NoReplaceExisting))
        {
            Error = TEXT("Cannot create sound text at ") + Path;
            return false;
        }
        return true;
    }

    bool WriteInstance(const FString& Root, const FString& Id, const FString& SourceId, FString& Error)
    {
        return MakeTextFile(Root / TEXT("Instances") / (Id + TEXT(".instance.dsfx")), InstanceText(Id,SourceId,{}), Error);
    }
}

bool FDreamSFXLibrarySnapshot::Load(const FString& Folder)
{
    Root = FPaths::ConvertRelativePathToFull(Folder);
    FPaths::NormalizeDirectoryName(Root);
    Entries.Reset();
    Error.Reset();
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.dsfx"), true, false);
    Files.Sort();
    TSet<FString> Ids;
    for (const FString& Path : Files)
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Path))
        {
            Error = TEXT("Cannot read ") + Path;
            return false;
        }
        FString Id, Parent;
        if(!Header(Text,Id,Parent)||Ids.Contains(Id)){Error=Path+TEXT(":1:1: expected a unique sound or instance declaration");return false;}
        const bool bInstance=!Parent.IsEmpty();
        Ids.Add(Id);
        Entries.Add({Id, bInstance ? TEXT("instance") : TEXT("definition"), Path, MoveTemp(Text), Parent, {}});
    }
    return true;
}

const FDreamSFXGraphTextEntry* FDreamSFXLibrarySnapshot::Find(const FString& Id) const
{
    return Entries.FindByPredicate([&](const auto& Entry) { return Entry.Id == Id; });
}

bool FDreamSFXLibrarySnapshot::TryCategory(const FDreamSFXGraphTextEntry& Entry, TArray<FString>& Out) const
{
    const auto* Mother = Entry.ParentId.IsEmpty() ? nullptr : Find(Entry.ParentId);
    FString Relative = FPaths::GetPath(Mother ? Mother->Path : Entry.Path) / TEXT("_");
    FPaths::MakePathRelativeTo(Relative, *(Root / TEXT("_")));
    FPaths::GetPath(Relative).ParseIntoArray(Out, TEXT("/"));
    if (Out.Num() > 0 && Out[0] == TEXT("Definitions")) Out.RemoveAt(0);
    return !Out.IsEmpty();
}

bool FDreamSFXLibrarySnapshot::AreSourcesCurrent(const FString& Id, const TArray<FString>& Dependencies, FString& OutError) const
{
    TArray<FString> Selected = Dependencies;
    Selected.Add(Id);
    for (const auto& SelectedId : Selected)
    {
        const auto* Entry = Find(SelectedId);
        FString Current;
        if (!Entry || !FFileHelper::LoadFileToString(Current, *Entry->Path) || Current != Entry->Text)
        {
            OutError = TEXT("Sound text changed on disk: ") + SelectedId;
            return false;
        }
    }
    return true;
}

bool FDreamSFXSoundDocument::Load(const FString& Root, const FString& Id, FString& Error)
{
    auto Snapshot = MakeShared<FDreamSFXLibrarySnapshot, ESPMode::ThreadSafe>();
    if (!Snapshot->Load(Root)) { Error = Snapshot->Error; return false; }
    return LoadFromSnapshot(Snapshot, Id, Error);
}

bool FDreamSFXSoundDocument::LoadFromSnapshot(FSnapshot Snapshot, const FString& Id, FString& Error,
    const FString& DraftText, const FString& Baseline)
{
    const auto* Entry = Snapshot ? Snapshot->Find(Id) : nullptr;
    if (!Entry) { Error = TEXT("Sound not found: ") + Id; return false; }
    if (!DraftText.IsEmpty() && Entry->Text != Baseline) { Error = TEXT("Instance changed on disk. Reload or save a copy."); return false; }
    FDreamSFXSoundDocument Next;
    Next.Library = Snapshot;
    Next.SelectedId = Id;
    Next.SelectedPath = Entry->Path;
    Next.LoadedText = Entry->Text;
    Next.Draft = DraftText;
    Next.MotherId = Entry->ParentId;
    if (Next.IsInstance())
    {
        const auto* Mother = Snapshot->Find(Entry->ParentId);
        if (!Mother || !Mother->ParentId.IsEmpty()) { Error = TEXT("Instance requires a V2 graph in this library"); return false; }
        auto Parent = CompileGraph(Mother->Text, Error);
        if (!Parent) return false;
        const auto Lines=Statements(DraftText.IsEmpty()?Entry->Text:DraftText);
        FString Resolved=Mother->Text;
        const int32 NameAt=Resolved.Find(TEXT("sound ")+Mother->Id);
        if(NameAt==INDEX_NONE){Error=TEXT("Missing mother sound declaration");return false;}
        Resolved.RemoveAt(NameAt+6,Mother->Id.Len());Resolved.InsertAt(NameAt+6,Id);
        for(const auto& In:Parent->Inputs)
            Next.Controls.Add({In.Name,TEXT("default"),In.Name.ToString(),In.Name.ToString(),TEXT(""),In.Minimum,In.Maximum,In.Default,false});
        for(int32 I=1;I<Lines.Num();++I)
        {
            FString K,V;if(!Lines[I].Split(TEXT("="),&K,&V)){Error=TEXT("Expected input = value");return false;}
            K.TrimStartAndEndInline();V.TrimStartAndEndInline();const FName Key(K);
            const auto* In=Parent->Inputs.FindByPredicate([&](const auto& P){return P.Name==Key;});
            double Value=0;
            if(!In||!LexTryParseString(Value,*V)||!FMath::IsFinite(Value)||!FMath::IsWithinInclusive(Value,In->Minimum,In->Maximum)||Next.Instance.Contains(Key))
            {Error=FString::Printf(TEXT("Instance line %d: expected one declared input and a value inside its range"),I+1);return false;}
            Next.Instance.Add(Key,Value);Resolved+=FString::Printf(TEXT("\nbind %s = %.17g\n"),*K,Value);
        }
        Next.Graph=CompileGraph(Resolved,Error);
        Next.CacheKey = Mother->Text + Resolved;
    }
    else
    {
        Next.Graph = CompileGraph(Entry->Text, Error);
        Next.CacheKey = Entry->Text;
    }
    if (!Next.Graph) return false;
    FTCHARToUTF8 Bytes(*Next.CacheKey);
    Next.CacheKey = FMD5::HashBytes(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    *this = MoveTemp(Next);
    return true;
}

bool FDreamSFXSoundDocument::HasOverride(const FDreamSFXInstanceParameter& Parameter) const
{
    return Instance.Contains(Parameter.Node);
}
double FDreamSFXSoundDocument::Value(const FDreamSFXInstanceParameter& Parameter) const
{
    const double* V=Instance.Find(Parameter.Node);return V?*V:Parameter.Default;
}
bool FDreamSFXSoundDocument::SetOverride(const FDreamSFXInstanceParameter& Parameter,TOptional<double> Value,FString& Error)
{
    if(!IsInstance()){Error=TEXT("Create an instance to override its inputs");return false;}
    auto Fields=Instance;if(Value)Fields.Add(Parameter.Node,*Value);else Fields.Remove(Parameter.Node);
    return LoadFromSnapshot(Library,SelectedId,Error,InstanceText(SelectedId,MotherId,Fields),LoadedText);
}

bool FDreamSFXSoundDocument::Save(FString& Error)
{
    if (!IsDirty()) return true;
    if (!Library->AreSourcesCurrent(SelectedId, Dependencies(), Error)) return false;
    const auto Result = FDreamSFXSourceSave::Save(SelectedPath, LoadedText, Draft);
    if (!Result.bSuccess) { Error = Result.Error; return false; }
    return Load(Root(), Id(), Error);
}

bool FDreamSFXSoundDocument::CreateInstance(const FString&, const FString& Name, FString& Error)
{
    const FString NewId = Name.TrimStartAndEnd().ToLower().Replace(TEXT(" "), TEXT("-"));
    if (!IsLoaded() || !ValidId(NewId) || Library->Find(NewId)) { Error = TEXT("Enter a unique instance name using letters, digits, dots or dashes"); return false; }
    const FString SourceId = IsInstance() ? MotherId : SelectedId;
    return WriteInstance(Root(), NewId, SourceId, Error) && Load(Root(), NewId, Error);
}

bool FDreamSFXSoundDocument::CreateProjectInstance(const FString& ProjectRoot, const FString& Name, FString& Error)
{
    const FString NewId = Name.TrimStartAndEnd().ToLower().Replace(TEXT(" "), TEXT("-"));
    FDreamSFXLibrarySnapshot Project;
    if (!Project.Load(ProjectRoot)) { Error = Project.Error; return false; }
    const auto* Mother = Library->Find(IsInstance() ? MotherId : SelectedId);
    if (!Mother || !ValidId(NewId) || NewId == Mother->Id || Project.Find(NewId)) { Error = TEXT("Enter a unique project instance name"); return false; }
    if (const auto* Existing = Project.Find(Mother->Id))
    {
        if (Existing->Text != Mother->Text) { Error = TEXT("Project contains a different graph with this ID"); return false; }
    }
    else
    {
        TArray<FString> Category;
        Library->TryCategory(*Mother, Category);
        const FString Target = ProjectRoot / TEXT("Definitions") / FString::Join(Category, TEXT("/")) / FPaths::GetCleanFilename(Mother->Path);
        if (!MakeTextFile(Target, Mother->Text, Error)) return false;
    }
    return WriteInstance(ProjectRoot, NewId, Mother->Id, Error) && Load(ProjectRoot, NewId, Error);
}

UMetaSoundSource* FDreamSFXSoundDocument::PreviewSource(FString& Error) const
{
    auto* Preview = GEditor ? GEditor->GetEditorSubsystem<UDreamSFXPreviewSubsystem>() : nullptr;
    if (Preview)
        if (auto* Cached = Preview->CachedSource(CacheKey)) return Cached;
    auto* Source = DreamSFX::V2::BuildGraphPreview(Graph, Error);
    if (Source && Preview) Preview->CacheSource(CacheKey, Source);
    return Source;
}

UMetaSoundSource* FDreamSFXSoundDocument::SaveAsset(FString& Error) const
{
    if (!Graph || IsDirty()) { Error = TEXT("Save the instance before building the asset"); return nullptr; }
    if (!Library->AreSourcesCurrent(SelectedId, Dependencies(), Error)) return nullptr;
    return DreamSFX::V2::SaveGraphSource(Graph, DreamSFX::V2::GraphPackageName(Graph->Id), Error);
}
