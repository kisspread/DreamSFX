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

    DreamSFX::V2::FGraphPtr CompileGraph(const FString& Text, const FString& Path, FString& Error,
        FDreamSFXSourceDiagnostic* Diagnostic)
    {
        auto Result = DreamSFX::V2::FGraphCompiler::Compile(Text);
        if (!Result.Graph)
        {
            if (Diagnostic && !Result.Errors.IsEmpty())
            {
                const auto& Failure = Result.Errors[0];
                *Diagnostic = {Path, Failure.Code, Failure.Message};
                FString Line, Column;
                if (Failure.Pointer.Split(TEXT(":"), &Line, &Column))
                {
                    LexTryParseString(Diagnostic->Line, *Line);
                    LexTryParseString(Diagnostic->Column, *Column);
                }
            }
            for (const auto& Failure : Result.Errors)
                Error += Failure.Code + TEXT(" ") + Failure.Pointer + TEXT(" ") + Failure.Message + TEXT("\n");
        }
        return Result.Graph;
    }

    struct FInstanceStatement
    {
        FString Text;
        int32 Line, Column;
    };

    TArray<FInstanceStatement> Statements(const FString& Text)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, false);
        TArray<FInstanceStatement> Result;
        for (int32 Index = 0; Index < Lines.Num(); ++Index)
        {
            FString& Line = Lines[Index];
            int32 Comment;
            if (Line.FindChar('#', Comment)) Line.LeftInline(Comment);
            int32 Indent = 0;
            while (Indent < Line.Len() && FChar::IsWhitespace(Line[Indent])) ++Indent;
            if (Indent < Line.Len()) Result.Add({Line.Mid(Indent).TrimEnd(), Index + 1, Indent + 1});
        }
        return Result;
    }

    bool Header(const FString& Text, FDreamSFXGraphTextEntry& Entry, int32& LineNumber)
    {
        TArray<FString> Words;
        for (int32 Start = 0; Start < Text.Len(); ++LineNumber)
        {
            int32 End = Start;
            while (End < Text.Len() && Text[End] != '\n') ++End;
            FString Line = Text.Mid(Start, End - Start);
            int32 Comment;
            if (Line.FindChar('#', Comment)) Line.LeftInline(Comment);
            Line.ParseIntoArrayWS(Words);
            if (!Words.IsEmpty())
            {
                const bool bDefinition = Words.Num() == 2 && Words[0] == TEXT("sound") && ValidId(Words[1]);
                const bool bInstance = Words.Num() == 4 && Words[0] == TEXT("instance") &&
                    Words[2] == TEXT("of") && ValidId(Words[1]) && ValidId(Words[3]);
                if (!bDefinition && !bInstance) return false;
                const int32 KeywordEnd = Line.Find(Words[0], ESearchCase::CaseSensitive) + Words[0].Len();
                const int32 IdStart = Line.Find(Words[1], ESearchCase::CaseSensitive, ESearchDir::FromStart, KeywordEnd);
                if (IdStart == INDEX_NONE) return false;
                Entry.Id = Words[1];
                Entry.Kind = bDefinition ? TEXT("definition") : TEXT("instance");
                if (bInstance) Entry.ParentId = Words[3];
                Entry.DeclarationIdOffset = Start + IdStart;
                return true;
            }
            Start = End + 1;
        }
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

bool FDreamSFXLibrarySnapshot::Load(const FString& Folder, TArray<FDreamSFXSourceDiagnostic>* Diagnostics)
{
    Root = FPaths::ConvertRelativePathToFull(Folder);
    FPaths::NormalizeDirectoryName(Root);
    Entries.Reset();
    Error.Reset();
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.dsfx"), true, false);
    Files.Sort();
    TMap<FString, int32> Ids;
    TSet<FString> DuplicateIds;
    const auto Report = [&](const FString& Path, const TCHAR* Code, const FString& Message, int32 Line, int32 Column)
    {
        if (Error.IsEmpty()) Error = Path + TEXT(": ") + Message;
        if (Diagnostics) Diagnostics->Add({Path, Code, Message, Line, Column});
    };
    for (const FString& Path : Files)
    {
        FDreamSFXGraphTextEntry Entry;
        Entry.Path = Path;
        if (!FFileHelper::LoadFileToString(Entry.Text, *Path))
        {
            Report(Path, TEXT("DSFX220"), TEXT("Cannot read sound text"), 0, 0);
            if (!Diagnostics) return false;
            continue;
        }
        int32 Line = 1;
        if (!Header(Entry.Text, Entry, Line))
        {
            Report(Path, TEXT("DSFX221"), TEXT("Expected a sound or instance declaration"), Line, 1);
            if (!Diagnostics) return false;
            continue;
        }
        if (const int32* Previous = Ids.Find(Entry.Id))
        {
            const FString Message = TEXT("Duplicate sound ID: ") + Entry.Id;
            Report(Path, TEXT("DSFX222"), Message, Line, 1);
            if (!Diagnostics) return false;
            if (!DuplicateIds.Contains(Entry.Id))
            {
                Report(Entries[*Previous].Path, TEXT("DSFX222"), Message, 0, 0);
                DuplicateIds.Add(Entry.Id);
            }
            continue;
        }
        Ids.Add(Entry.Id, Entries.Num());
        Entries.Add(MoveTemp(Entry));
    }
    if (!DuplicateIds.IsEmpty())
        Entries.RemoveAll([&](const FDreamSFXGraphTextEntry& Entry) { return DuplicateIds.Contains(Entry.Id); });
    return Error.IsEmpty();
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
    const FString& DraftText, const FString& Baseline, FDreamSFXSourceDiagnostic* Diagnostic)
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
    const auto Fail = [&](const TCHAR* Code, const FString& Message, int32 Line = 0, int32 Column = 0)
    {
        Error = Message;
        if (Diagnostic) *Diagnostic = {Entry->Path, Code, Message, Line, Column};
        return false;
    };
    if (Next.IsInstance())
    {
        const auto* Mother = Snapshot->Find(Entry->ParentId);
        if (!Mother || !Mother->ParentId.IsEmpty())
            return Fail(TEXT("DSFX223"), TEXT("Instance requires a sound definition in this library: ") + Entry->ParentId);
        auto Parent = CompileGraph(Mother->Text, Mother->Path, Error, Diagnostic);
        if (!Parent) return false;
        const auto Lines=Statements(DraftText.IsEmpty()?Entry->Text:DraftText);
        FString Resolved=Mother->Text;
        const int32 NameAt = Mother->DeclarationIdOffset;
        if (NameAt == INDEX_NONE || !Resolved.Mid(NameAt, Mother->Id.Len()).Equals(Mother->Id, ESearchCase::CaseSensitive))
            return Fail(TEXT("DSFX224"), TEXT("Cannot locate the mother sound ID"));
        Resolved.RemoveAt(NameAt, Mother->Id.Len());
        Resolved.InsertAt(NameAt, Id);
        for(const auto& In:Parent->Inputs)
            Next.Controls.Add({In.Name,TEXT("default"),In.Name.ToString(),In.Name.ToString(),TEXT(""),In.Minimum,In.Maximum,In.Default,false});
        for(int32 I=1;I<Lines.Num();++I)
        {
            const auto& Statement = Lines[I];
            FString K,V;
            if (!Statement.Text.Split(TEXT("="), &K, &V))
                return Fail(TEXT("DSFX225"), TEXT("Expected input = value"), Statement.Line, Statement.Column);
            K.TrimStartAndEndInline();V.TrimStartAndEndInline();const FName Key(K);
            const auto* In=Parent->Inputs.FindByPredicate([&](const auto& P){return P.Name==Key;});
            double Value=0;
            if(!In||!LexTryParseString(Value,*V)||!FMath::IsFinite(Value)||!FMath::IsWithinInclusive(Value,In->Minimum,In->Maximum)||Next.Instance.Contains(Key))
                return Fail(TEXT("DSFX225"), TEXT("Expected one declared input and a value inside its range: ") + K,
                    Statement.Line, Statement.Column);
            Next.Instance.Add(Key,Value);Resolved+=FString::Printf(TEXT("\nbind %s = %.17g\n"),*K,Value);
        }
        Next.Graph=CompileGraph(Resolved, Mother->Path, Error, Diagnostic);
        Next.CacheKey = Mother->Text + Resolved;
    }
    else
    {
        Next.Graph = CompileGraph(Entry->Text, Entry->Path, Error, Diagnostic);
        Next.CacheKey = Entry->Text;
    }
    if (!Next.Graph) return false;
    if (Next.Graph->Id != FName(*Id))
        return Fail(TEXT("DSFX224"), TEXT("Compiled sound ID differs from the source declaration"));
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

UMetaSoundSource* FDreamSFXSoundDocument::SaveAsset(FString& Error, bool bForce, bool* bWritten) const
{
    if (!Graph || IsDirty()) { Error = TEXT("Save the instance before building the asset"); return nullptr; }
    if (!Library->AreSourcesCurrent(SelectedId, Dependencies(), Error)) return nullptr;
    const FName ExpectedId(*SelectedId);
    if (Graph->Id != ExpectedId) { Error = TEXT("Compiled sound ID differs from the source declaration"); return nullptr; }
    return DreamSFX::V2::SaveGraphSource(Graph, DreamSFX::V2::GraphPackageName(ExpectedId), Error, bForce, bWritten);
}
