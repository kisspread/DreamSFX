#include "DreamSFXGraphPreview.h"
#include "DreamSFXGraphAsset.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundFrontendRegistryKey.h"
#include "UObject/UnrealType.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateInput.h"
#include "MetasoundGlobals.h"
#include "MetasoundSource.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace DreamSFX::V2
{
    // Bump when the generated MetaSound document or its serialization contract changes.
    constexpr int32 EditorLayoutVersion = 2;
    constexpr int32 ControlDefaultsVersion = 4;
    constexpr int32 GeneratedSourceVersion = ControlDefaultsVersion;

    static void ConfigureControlDefaults(FMetaSoundFrontendDocumentBuilder& Builder, const FGraphIR& Graph)
    {
        for (const auto& Control : Graph.Inputs)
        {
            const auto* Input = Builder.FindGraphInput(Control.Name);
            if (!Input) continue; // A user can remove a generated input from the graph.
            const float TransportDefault = Control.MetaSoundDefault();
            if (Control.bHasAuditionExpression)
            {
                const FString Auto = TransportDefault == 0.f ? TEXT("-0") : FString::SanitizeFloat(TransportDefault, 0);
                Builder.SetGraphInputDisplayName(Control.Name, FText::FromString(
                    FString::Printf(TEXT("%s (Auto: %s)"), *Control.Name.ToString(), *Auto)));
                Builder.SetGraphInputDescription(Control.Name, FText::FromString(FString::Printf(
                    TEXT("Auto (%s) follows the authored audition. Manual range: %g to %g. DSL default: %g."),
                    *Auto, Control.Minimum, Control.Maximum, Control.Default)));
            }

            // Keep custom page defaults. Only migrate the former unassigned float.
            const auto Defaults = Input->GetDefaults();
            for (const auto& Page : Defaults)
            {
                float Value;
                if (!Page.Literal.TryGet(Value) || Value != UnsetProgramControl) continue;
                const FMetasoundFrontendLiteral Literal(TransportDefault);
                Builder.SetGraphInputDefault(Control.Name, Literal, &Page.PageID);
            }
            // UE serializes the dial range separately from the graph literal. Repair a range
            // expanded to fit the old sentinel without discarding the user's widget settings.
            if (UObject* Metadata = Builder.FindMemberMetadata(Input->NodeID))
            {
                const FStructProperty* Property = FindFProperty<FStructProperty>(Metadata->GetClass(), TEXT("Range"));
                if (Property && Property->Struct == TBaseStructure<FVector2D>::Get())
                {
                    auto& Range = *Property->ContainerPtrToValuePtr<FVector2D>(Metadata);
                    if (Range.X <= UnsetProgramControl)
                        Range = FVector2D(FMath::Min(Control.Minimum, double(TransportDefault)), Control.Maximum);
                }
            }
        }

        const auto& Pages = Builder.GetConstDocumentChecked().RootGraph.GetConstGraphPages();
        for (const auto& Page : Pages)
        {
            for (const auto& Node : Page.Nodes)
            {
                const auto* Class = Builder.FindDependency(Node.ClassID);
                if (Class && Class->Metadata.GetClassName() == FMetasoundFrontendClassName(TEXT("DreamSFX"), TEXT("GraphPlayer")))
                    Builder.SetNodeUnconnectedPinsHidden(Node.GetID(), true, &Page.PageID);
            }
        }
    }

    static void InitEditorLayout(FMetaSoundFrontendDocumentBuilder& Builder)
    {
        // UE copies input positions onto the visible input templates when it inserts them.
        Builder.InitNodeLocations();
        Metasound::Frontend::FInputNodeTemplate::GetChecked().Inject(Builder, true);
    }

    static bool UpgradePlayerDependency(FMetaSoundFrontendDocumentBuilder& Builder)
    {
        using namespace Metasound::Frontend;
        const FMetasoundFrontendClassName Name(TEXT("DreamSFX"), TEXT("GraphPlayer"));
        const FNodeClassRegistryKey Current(EMetasoundFrontendClassType::External, Name, 1, 2);
        // Persist the new pin defaults on disk as well as in the editor's registry.
        // ReplaceDependency restores node IDs, style, connections and instance literals.
        for (int32 Minor = 0; Minor < Current.Version.Minor; ++Minor)
        {
            const FNodeClassRegistryKey Old(EMetasoundFrontendClassType::External, Name, 1, Minor);
            if (Builder.FindDependency(Old) && !Builder.ReplaceDependency(Old, Current)) return false;
        }
        return true;
    }

    static UMetaSoundSource* BuildGraphSource(FGraphPtr Graph, UPackage* Package, FName Name, FString& Error, UMetaSoundSource* Existing = nullptr)
    {
        check(IsInGameThread());
        auto Fail = [&Error](const TCHAR* Message) -> UMetaSoundSource* { Error = Message; return nullptr; };
        if (!Graph) return Fail(TEXT("Expected compiled DreamSFX V2 graph"));
        auto* Subsystem = UMetaSoundBuilderSubsystem::Get();
        if (!Subsystem) return Fail(TEXT("MetaSound builder unavailable"));

        EMetaSoundBuilderResult Result;
        FMetaSoundBuilderNodeOutputHandle Play;
        FMetaSoundBuilderNodeInputHandle Finished;
        TArray<FMetaSoundBuilderNodeInputHandle> Audio;
        auto* Builder = Subsystem->CreateSourceBuilder(NAME_None, Play, Finished, Audio, Result, EMetaSoundOutputAudioFormat::Stereo, true);
        if (!Builder || Result != EMetaSoundBuilderResult::Succeeded || Audio.Num() != 2)
            return Fail(TEXT("Cannot create stereo MetaSound audition source"));
        const auto Player = Builder->AddNodeByClassName({TEXT("DreamSFX"), TEXT("GraphPlayer")}, Result, 1);
        if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(TEXT("DreamSFX GraphPlayer v1 unavailable"));
        const auto Trigger = Builder->FindNodeInputByName(Player, TEXT("Play"), Result);
        if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(TEXT("GraphPlayer Play input unavailable"));
        Builder->ConnectNodes(Play, Trigger, Result);
        if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(TEXT("Cannot connect GraphPlayer Play"));

        FName BoolType;
        const auto LoopValue = Subsystem->CreateBoolMetaSoundLiteral(false, BoolType);
        const auto Loop = Builder->AddGraphInputNode(TEXT("Loop"), BoolType, LoopValue, Result);
        if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(TEXT("Cannot add Loop input"));
        const auto LoopTarget = Builder->FindNodeInputByName(Player, TEXT("Loop"), Result);
        if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(TEXT("GraphPlayer Loop input unavailable"));
        Builder->ConnectNodes(Loop, LoopTarget, Result);
        if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(TEXT("Cannot connect GraphPlayer Loop"));

        if(Graph->Program)
        {
            auto Expose=[&](FName Name,FName Pin,FName Type,const FMetasoundFrontendLiteral& Value)
            {
                const auto Input=Builder->AddGraphInputNode(Name,Type,Value,Result);
                if(Result!=EMetaSoundBuilderResult::Succeeded)return false;
                const auto Target=Builder->FindNodeInputByName(Player,Pin,Result);
                if(Result!=EMetaSoundBuilderResult::Succeeded)return false;
                Builder->ConnectNodes(Input,Target,Result);return Result==EMetaSoundBuilderResult::Succeeded;
            };
            FName Type;
            const auto State=Subsystem->CreateStringMetaSoundLiteral(TEXT(""),Type);
            if(!Expose(TEXT("State"),TEXT("State"),Type,State))return Fail(TEXT("Cannot expose the live state input"));
            for(int32 I=0;I<Graph->Inputs.Num();++I)
            {
                const auto Value=Subsystem->CreateFloatMetaSoundLiteral(Graph->Inputs[I].MetaSoundDefault(),Type);
                if(!Expose(Graph->Inputs[I].Name,ProgramControlPin(I),Type,Value))return Fail(TEXT("Cannot expose the named program input"));
            }
        }
        auto ConnectOutput = [&](FName Name, const FMetaSoundBuilderNodeInputHandle& Target)
        {
            const auto Output = Builder->FindNodeOutputByName(Player, Name, Result);
            if (Result != EMetaSoundBuilderResult::Succeeded) return false;
            Builder->ConnectNodes(Output, Target, Result);
            return Result == EMetaSoundBuilderResult::Succeeded;
        };
        if (!ConnectOutput(TEXT("Out Left"), Audio[0]) || !ConnectOutput(TEXT("Out Right"), Audio[1]) ||
            !ConnectOutput(TEXT("On Finished"), Finished)) return Fail(TEXT("Cannot connect GraphPlayer outputs"));
        const bool bPersist = Package != GetTransientPackage();
        auto* Source = Existing ? Existing : NewObject<UMetaSoundSource>(Package, Name, bPersist ? RF_Public | RF_Standalone : RF_Transient);
        const auto Input = Builder->FindNodeInputByName(Player, TEXT("Graph"), Result);
        if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(TEXT("GraphPlayer graph input unavailable"));
        auto* Data = Existing ? FindObject<UDreamSFXGraphAsset>(Source,TEXT("DreamSFXGraph")) : NewObject<UDreamSFXGraphAsset>(Source, TEXT("DreamSFXGraph"), bPersist ? RF_Public : RF_Transient);
        if (!Data || !Data->Initialize(Graph, bPersist, GeneratedSourceVersion))
            return Fail(TEXT("Cannot encode compiled graph"));
        Builder->SetNodeInputDefault(Input, Subsystem->CreateObjectMetaSoundLiteral(Data), Result);
        if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(TEXT("Cannot bind compiled graph"));

        TScriptInterface<IMetaSoundDocumentInterface> Interface;
        Interface.SetObject(Source);
        Interface.SetInterface(Cast<IMetaSoundDocumentInterface>(Source));
        FMetaSoundFrontendDocumentBuilder Target(Interface);
        Target.InitDocument(&Builder->GetConstBuilder().GetConstDocumentChecked(), nullptr, false);
        if (bPersist)
        {
            InitEditorLayout(Target);
            ConfigureControlDefaults(Target, *Graph);
        }
        Source->ConformObjectToDocument();
        Source->RebuildReferencedAssetClasses();
        Target.FinishBuilding();
        if (Metasound::CanEverExecuteGraph())
            Source->UpdateAndRegisterForExecution(Metasound::Frontend::FMetaSoundAssetRegistrationOptions{});
        return Source;
    }

    UMetaSoundSource* BuildGraphPreview(FGraphPtr Graph, FString& Error)
    {
        return BuildGraphSource(MoveTemp(Graph), GetTransientPackage(), NAME_None, Error);
    }

    UMetaSoundSource* SaveGraphSource(FGraphPtr Graph, const FString& LongPackageName, FString& Error,
        bool bForce, bool* bWritten)
    {
        if (bWritten) *bWritten = false;
        if (!Graph || LongPackageName != GraphPackageName(Graph->Id) ||
            !FPackageName::IsValidLongPackageName(LongPackageName))
        {
            Error = TEXT("Expected the generated package path for this sound ID");
            return nullptr;
        }
        const FString Filename = FPackageName::LongPackageNameToFilename(LongPackageName, FPackageName::GetAssetPackageExtension());
        const FName Name(*FPackageName::GetLongPackageAssetName(LongPackageName));
        UPackage* Package = CreatePackage(*LongPackageName);
        // Refuse to overwrite a different type. A saved graph is rebuilt as a single package.
        const bool bPackageExists = FPackageName::DoesPackageExist(LongPackageName);
        if (bPackageExists) Package->FullyLoad();
        if (UObject* Existing = FindObject<UObject>(Package, *Name.ToString()))
        {
            auto* Source = Cast<UMetaSoundSource>(Existing);
            auto* Data = Source ? FindObject<UDreamSFXGraphAsset>(Source, TEXT("DreamSFXGraph")) : nullptr;
            if (!Data || !Data->GetGraph() || Data->GetGraph()->Id != Graph->Id)
            {
                Error = TEXT("Cannot update the existing package or it belongs to a different sound");
                return nullptr;
            }
            if (!bForce && bPackageExists && !Package->IsDirty() && Data->MatchesBuild(*Graph, GeneratedSourceVersion))
                return Source;
            if (IFileManager::Get().IsReadOnly(*Filename))
            {
                Error = TEXT("Cannot update a read-only MetaSound package");
                return nullptr;
            }
            const FGraphPtr Previous = Data->GetGraph();
            const int32 PreviousVersion = Data->GetSourceBuildVersion();
            // Upgrade generated member defaults and layout in place, preserving edited wiring.
            const bool bUpgradeDocument = !bForce && GeneratedSourceVersion == ControlDefaultsVersion &&
                PreviousVersion < GeneratedSourceVersion && Data->MatchesBuild(*Graph, PreviousVersion);
            if (bUpgradeDocument)
            {
                auto& Builder = Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(*Source);
                if (!UpgradePlayerDependency(Builder.GetBuilder()))
                {
                    Error = TEXT("Cannot update the stored Graph Player pin definitions");
                    return nullptr;
                }
                if (PreviousVersion < EditorLayoutVersion) InitEditorLayout(Builder.GetBuilder());
                ConfigureControlDefaults(Builder.GetBuilder(), *Graph);
                Source->RebuildReferencedAssetClasses();
                Data->SetSourceBuildVersion(GeneratedSourceVersion);
            }
            else if (!BuildGraphSource(MoveTemp(Graph),Package,Name,Error,Source))
            {
                Error = TEXT("Cannot encode compiled graph");
                return nullptr;
            }
            FSavePackageArgs Args;
            Args.TopLevelFlags = RF_Public | RF_Standalone;
            if (!UPackage::SavePackage(Package, Source, *Filename, Args))
            {
                if (bUpgradeDocument) Data->SetSourceBuildVersion(PreviousVersion);
                else BuildGraphSource(Previous,Package,Name,Error,Source);
                Package->SetDirtyFlag(true);
                Error = TEXT("Cannot update the MetaSound package");
                return nullptr;
            }
            Source->UpdateAndRegisterForExecution(Metasound::Frontend::FMetaSoundAssetRegistrationOptions{});
            if (bWritten) *bWritten = true;
            return Source;
        }
        if (bPackageExists)
        {
            Error = TEXT("The existing package does not contain the expected MetaSound Source");
            return nullptr;
        }
        auto* Source = BuildGraphSource(MoveTemp(Graph), Package, Name, Error);
        if (!Source) return nullptr;
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true) ||
            !UPackage::SavePackage(Package, Source, *Filename, Args))
        {
            Source->ClearFlags(RF_Public | RF_Standalone);
            Source->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
            Error = TEXT("Cannot save the MetaSound package");
            return nullptr;
        }
        FAssetRegistryModule::AssetCreated(Source);
        if (bWritten) *bWritten = true;
        return Source;
    }
}
