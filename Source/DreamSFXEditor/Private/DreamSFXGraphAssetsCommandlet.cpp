#include "DreamSFXGraphAssetsCommandlet.h"
#include "Audio.h"
#include "Dom/JsonObject.h"
#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXGraphPreview.h"
#include "DreamSFXSoundDocument.h"
#include "HAL/FileManager.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    constexpr int32 SampleRate = 48000;
    constexpr int32 Channels = 2;
    constexpr float SilenceThreshold = 1.e-5f;

    struct FSoundResult
    {
        FDreamSFXSoundDocument Document;
        FString Id, Path, Status, AssetPath;
        TArray<FDreamSFXSourceDiagnostic> Diagnostics;
        TSharedPtr<FJsonObject> Render;

        void Fail(FDreamSFXSourceDiagnostic Issue)
        {
            if (Issue.Path.IsEmpty()) Issue.Path = Path;
            const FString Position = Issue.Line > 0 ?
                FString::Printf(TEXT(" %d:%d"), Issue.Line, Issue.Column) : FString();
            UE_LOG(LogTemp, Error, TEXT("DreamSFX: %s: %s%s %s"),
                *Issue.Path, *Issue.Code, *Position, *Issue.Message);
            Diagnostics.Add(MoveTemp(Issue));
            Status = TEXT("failed");
        }

        TSharedRef<FJsonObject> ToJson() const;
    };

    TSharedRef<FJsonObject> FSoundResult::ToJson() const
    {
        auto Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("id"), Id);
        Value->SetStringField(TEXT("source"), Path);
        Value->SetStringField(TEXT("status"), Status);
        if (!AssetPath.IsEmpty()) Value->SetStringField(TEXT("assetPath"), AssetPath);
        if (Render) Value->SetObjectField(TEXT("render"), Render);

        if (const auto Graph = Document.GetGraph())
        {
            const FString Package = DreamSFX::V2::GraphPackageName(Graph->Id);
            Value->SetStringField(TEXT("targetAssetPath"), Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package));
            Value->SetNumberField(TEXT("previewSeconds"), Graph->DurationSeconds);
            if (Document.IsInstance()) Value->SetStringField(TEXT("definitionId"), Document.ParentName());
            TArray<TSharedPtr<FJsonValue>> Inputs;
            for (const auto& Input : Graph->Inputs)
            {
                auto Description = MakeShared<FJsonObject>();
                Description->SetStringField(TEXT("name"), Input.Name.ToString());
                Description->SetNumberField(TEXT("minimum"), Input.Minimum);
                Description->SetNumberField(TEXT("maximum"), Input.Maximum);
                Description->SetNumberField(TEXT("default"), Input.Default);
                Inputs.Add(MakeShared<FJsonValueObject>(Description));
            }
            Value->SetArrayField(TEXT("inputs"), Inputs);
            TArray<TSharedPtr<FJsonValue>> States;
            for (FName State : Graph->StateNames) States.Add(MakeShared<FJsonValueString>(State.ToString()));
            Value->SetArrayField(TEXT("states"), States);
        }

        TArray<TSharedPtr<FJsonValue>> Issues;
        for (const auto& Issue : Diagnostics)
        {
            auto Description = MakeShared<FJsonObject>();
            Description->SetStringField(TEXT("path"), Issue.Path);
            Description->SetStringField(TEXT("code"), Issue.Code);
            Description->SetStringField(TEXT("message"), Issue.Message);
            if (Issue.Line > 0) Description->SetNumberField(TEXT("line"), Issue.Line);
            if (Issue.Column > 0) Description->SetNumberField(TEXT("column"), Issue.Column);
            Issues.Add(MakeShared<FJsonValueObject>(Description));
        }
        Value->SetArrayField(TEXT("diagnostics"), Issues);
        return Value;
    }

    void RenderAudition(FSoundResult& Result, const FString& Directory)
    {
        DreamSFX::V2::FGraphInstance Instance;
        const auto Graph = Result.Document.GetGraph();
        FString Error;
        if (!Instance.Prepare(Graph, SampleRate, Error))
        {
            Result.Fail({{}, TEXT("DSFX230"), Error});
            return;
        }

        constexpr int32 BlockFrames = 256;
        const int32 Limit = static_cast<int32>(Instance.GetLengthFrames());
        TArray<int16> PCM;
        PCM.Reserve(Limit * Channels);
        int32 Frames = 0;
        int32 ClippedSamples = 0;
        double Energy = 0;
        float Peak = 0;
        while (!Instance.IsFinished() && Frames < Limit)
        {
            float Block[BlockFrames * Channels];
            const int32 Used = Instance.Render(Block, FMath::Min(BlockFrames, Limit - Frames));
            for (int32 Index = 0; Index < Used * Channels; ++Index)
            {
                const float Sample = Block[Index];
                const float Magnitude = FMath::Abs(Sample);
                Peak = FMath::Max(Peak, Magnitude);
                ClippedSamples += Magnitude > 1.f;
                Energy += double(Sample) * Sample;
                const float Bounded = FMath::Clamp(Sample, -1.f, 1.f);
                PCM.Add(int16(FMath::RoundToInt(Bounded * (Bounded < 0 ? 32768.f : 32767.f))));
            }
            Frames += Used;
            if (Used == 0) break;
        }

        const bool bSilent = Peak <= SilenceThreshold;
        auto Metrics = MakeShared<FJsonObject>();
        Metrics->SetStringField(TEXT("backend"), TEXT("DreamSFXCore"));
        Metrics->SetNumberField(TEXT("sampleRate"), SampleRate);
        Metrics->SetNumberField(TEXT("channels"), Channels);
        Metrics->SetNumberField(TEXT("frames"), Frames);
        Metrics->SetNumberField(TEXT("durationSeconds"), double(Frames) / SampleRate);
        Metrics->SetNumberField(TEXT("peak"), Peak);
        Metrics->SetNumberField(TEXT("rms"), PCM.IsEmpty() ? 0. : FMath::Sqrt(Energy / PCM.Num()));
        Metrics->SetNumberField(TEXT("clippedSamples"), ClippedSamples);
        Metrics->SetBoolField(TEXT("silent"), bSilent);
        Result.Render = Metrics;
        if (Instance.HasRenderError() || !Instance.IsFinished())
            Result.Fail({{}, TEXT("DSFX231"), TEXT("Audition did not complete successfully; check the runtime log")});
        if (bSilent) Result.Fail({{}, TEXT("DSFX232"), TEXT("Audition has no signal above the silence threshold")});
        if (ClippedSamples > 0) Result.Fail({{}, TEXT("DSFX233"), TEXT("Audition contains samples outside [-1, 1]")});

        if (!PCM.IsEmpty())
        {
            const FString Name = FPackageName::GetLongPackageAssetName(DreamSFX::V2::GraphPackageName(Graph->Id));
            const FString WavPath = Directory / (Name + TEXT(".wav"));
            TArray<uint8> Wav;
            SerializeWaveFile(Wav, reinterpret_cast<const uint8*>(PCM.GetData()), PCM.Num() * sizeof(int16), Channels, SampleRate);
            if (FFileHelper::SaveArrayToFile(Wav, *WavPath)) Metrics->SetStringField(TEXT("wavPath"), WavPath);
            else Result.Fail({WavPath, TEXT("DSFX234"), TEXT("Cannot write audition WAV")});
        }
    }
}

int32 UDreamSFXGraphAssetsCommandlet::Main(const FString& Params)
{
    FString Root, SelectedId, RenderDir, ReportPath;
    if (!FParse::Value(*Params, TEXT("SourceDir="), Root)) Root = FPaths::ProjectDir() / TEXT("DSFX");
    Root = FPaths::ConvertRelativePathToFull(Root);
    FParse::Value(*Params, TEXT("Id="), SelectedId);
    if (FParse::Value(*Params, TEXT("Report="), ReportPath))
    {
        ReportPath = FPaths::ConvertRelativePathToFull(ReportPath);
        if (!FPaths::GetExtension(ReportPath).Equals(TEXT("json"), ESearchCase::IgnoreCase) ||
            !IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true))
        {
            UE_LOG(LogTemp, Error, TEXT("DreamSFX requires a writable .json report path: %s"), *ReportPath);
            return 1;
        }
    }
    const bool bRender = FParse::Value(*Params, TEXT("RenderDir="), RenderDir);
    bool bCanRender = true;
    if (bRender)
    {
        RenderDir = FPaths::ConvertRelativePathToFull(RenderDir);
        bCanRender = IFileManager::Get().MakeDirectory(*RenderDir, true);
    }
    const bool bValidateOnly = FParse::Param(*Params, TEXT("ValidateOnly"));
    const bool bForce = FParse::Param(*Params, TEXT("Force"));

    TArray<FSoundResult> Results;
    TArray<FDreamSFXSourceDiagnostic> LibraryIssues;
    auto Files = MakeShared<FDreamSFXLibrarySnapshot, ESPMode::ThreadSafe>();
    Files->Load(Root, &LibraryIssues);
    if (!SelectedId.IsEmpty() && Files->Find(SelectedId))
        LibraryIssues.Reset(); // The selected document resolves its own definition below.
    if (Files->Entries.IsEmpty() && LibraryIssues.IsEmpty())
        LibraryIssues.Add({Root, TEXT("DSFX226"), TEXT("No .dsfx source files found")});
    if (!SelectedId.IsEmpty() && !Files->Find(SelectedId))
        LibraryIssues.Add({Root, TEXT("DSFX227"), TEXT("Sound ID not found or ambiguous: ") + SelectedId});
    if (!bCanRender)
        LibraryIssues.Add({RenderDir, TEXT("DSFX234"), TEXT("Cannot create audition directory")});
    for (auto& Issue : LibraryIssues)
    {
        auto& Result = Results.AddDefaulted_GetRef();
        Result.Path = Issue.Path;
        Result.Fail(MoveTemp(Issue));
    }

    for (const auto& Entry : Files->Entries)
    {
        if (!SelectedId.IsEmpty() && Entry.Id != SelectedId) continue;
        auto& Result = Results.AddDefaulted_GetRef();
        Result.Id = Entry.Id;
        Result.Path = Entry.Path;
        FString Error;
        FDreamSFXSourceDiagnostic Issue;
        if (!Result.Document.LoadFromSnapshot(Files, Entry.Id, Error, {}, {}, &Issue))
        {
            if (Issue.Code.IsEmpty()) Issue = {Entry.Path, TEXT("DSFX210"), Error};
            Result.Fail(MoveTemp(Issue));
            continue;
        }
        const auto Graph = Result.Document.GetGraph();
        if (!DreamSFX::V2::BuildGraphPreview(Graph, Error))
        {
            Result.Fail({{}, TEXT("DSFX228"), Error});
            continue;
        }
        Result.Status = bValidateOnly ? TEXT("validated") : TEXT("ready");
        if (bRender && bCanRender) RenderAudition(Result, RenderDir);
        if (bValidateOnly && Result.Diagnostics.IsEmpty())
        {
            int32 Audio = 0, Control = 0;
            for (const auto& Edge : Graph->Connections)
            {
                Audio += Edge.Kind == DreamSFX::V2::EPortKind::Audio;
                Control += Edge.Kind == DreamSFX::V2::EPortKind::Control;
            }
            UE_LOG(LogTemp, Display, TEXT("DSFX_DSL_VALID id=%s nodes=%d audio=%d control=%d timeline=%d states=%d inputs=%d seconds=%.3f"),
                *Result.Id, Graph->Nodes.Num(), Audio, Control, Graph->Timeline.Num(), Graph->StateNames.Num(),
                Graph->Inputs.Num(), Graph->DurationSeconds);
        }
    }

    const bool bPreflightOK = !Results.ContainsByPredicate([](const FSoundResult& Result) { return !Result.Diagnostics.IsEmpty(); });
    if (!bValidateOnly)
    {
        for (auto& Result : Results)
        {
            if (Result.Status != TEXT("ready")) continue;
            if (!bPreflightOK)
            {
                Result.Status = TEXT("not_built");
                continue;
            }
            FString Error;
            bool bWritten = false;
            if (const auto* Source = Result.Document.SaveAsset(Error, bForce, &bWritten))
            {
                Result.AssetPath = Source->GetPathName();
                Result.Status = bWritten ? TEXT("saved") : TEXT("unchanged");
                UE_LOG(LogTemp, Display, TEXT("%s id=%s asset=%s"), bWritten ? TEXT("DSFX_V2_SAVED") : TEXT("DSFX_V2_UNCHANGED"),
                    *Result.Id, *Result.AssetPath);
            }
            else Result.Fail({{}, TEXT("DSFX229"), Error});
        }
    }
    const bool bOK = !Results.ContainsByPredicate([](const FSoundResult& Result) { return !Result.Diagnostics.IsEmpty(); });
    if (!ReportPath.IsEmpty())
    {
        auto Report = MakeShared<FJsonObject>();
        Report->SetNumberField(TEXT("version"), 1);
        Report->SetStringField(TEXT("sourceDir"), Root);
        Report->SetStringField(TEXT("operation"), bValidateOnly ? TEXT("validate") : TEXT("build"));
        Report->SetBoolField(TEXT("ok"), bOK);
        TArray<TSharedPtr<FJsonValue>> Items;
        for (const auto& Result : Results) Items.Add(MakeShared<FJsonValueObject>(Result.ToJson()));
        Report->SetArrayField(TEXT("results"), Items);
        FString Json;
        const auto Writer = TJsonWriterFactory<>::Create(&Json);
        if (!FJsonSerializer::Serialize(Report, Writer) ||
            !FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogTemp, Error, TEXT("DreamSFX could not write report: %s"), *ReportPath);
            return 1;
        }
    }
    return bOK ? 0 : 1;
}
