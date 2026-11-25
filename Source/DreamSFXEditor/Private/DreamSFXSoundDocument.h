#pragma once

#include "DreamSFXGraphCompiler.h"


class UMetaSoundSource;

struct FDreamSFXSourceDiagnostic
{
    FString Path, Code, Message;
    int32 Line = 0;
    int32 Column = 0;
};

struct FDreamSFXGraphTextEntry
{
    FString Id, Kind, Path, Text, ParentId;
    struct { FString DisplayName, Description; } Meta;
    int32 DeclarationIdOffset = INDEX_NONE;
};

struct FDreamSFXLibrarySnapshot
{
    FString Root, Error;
    TArray<FDreamSFXGraphTextEntry> Entries;
    bool Load(const FString& Folder, TArray<FDreamSFXSourceDiagnostic>* Diagnostics = nullptr);
    const FDreamSFXGraphTextEntry* Find(const FString& Id) const;
    bool TryCategory(const FDreamSFXGraphTextEntry& Entry, TArray<FString>& Out) const;
    bool AreSourcesCurrent(const FString& Id, const TArray<FString>& Dependencies, FString& Error) const;
};

struct FDreamSFXInstanceParameter
{
    FName Node, Field;
    FString Key, Label, Unit;
    double Minimum, Maximum, Default;
    bool bInteger;
};

class FDreamSFXSoundDocument
{
public:
    using FSnapshot = TSharedPtr<const FDreamSFXLibrarySnapshot, ESPMode::ThreadSafe>;
    bool Load(const FString& Root, const FString& Id, FString& Error);
    bool LoadFromSnapshot(FSnapshot Snapshot, const FString& Id, FString& Error,
        const FString& Draft = {}, const FString& Baseline = {}, FDreamSFXSourceDiagnostic* Diagnostic = nullptr);
    bool SetOverride(const FDreamSFXInstanceParameter& Parameter, TOptional<double> Value, FString& Error);
    bool Save(FString& Error);
    bool CreateInstance(const FString& RelativeFolder, const FString& Name, FString& Error);
    bool CreateProjectInstance(const FString& ProjectRoot, const FString& Name, FString& Error);
    UMetaSoundSource* PreviewSource(FString& Error) const;
    UMetaSoundSource* SaveAsset(FString& Error, bool bForce = true, bool* bWritten = nullptr) const;

    bool IsLoaded() const { return Graph.IsValid(); }
    bool IsInstance() const { return !MotherId.IsEmpty(); }
    bool IsDirty() const { return !Draft.IsEmpty(); }
    const FString& Id() const { return SelectedId; }
    const FString& Path() const { return SelectedPath; }
    const FString& Root() const { return Library->Root; }
    const FString& DisplayName() const { return SelectedId; }
    const FString& ParentName() const { return MotherId; }
    const FString& DraftText() const { return Draft; }
    const FString& BaselineText() const { return LoadedText; }
    FSnapshot Snapshot() const { return Library; }
    DreamSFX::V2::FGraphPtr GetGraph() const { return Graph; }
    TArray<FString> Dependencies() const { return IsInstance() ? TArray<FString>{MotherId} : TArray<FString>{}; }
    const TArray<FDreamSFXInstanceParameter>& Parameters() const { return Controls; }
    bool HasOverride(const FDreamSFXInstanceParameter& Parameter) const;
    double Value(const FDreamSFXInstanceParameter& Parameter) const;
private:
    FSnapshot Library;
    FString SelectedId, SelectedPath, MotherId, LoadedText, Draft, CacheKey;
    DreamSFX::V2::FGraphPtr Graph;
    TArray<FDreamSFXInstanceParameter> Controls;
    TMap<FName,double> Instance;
};
