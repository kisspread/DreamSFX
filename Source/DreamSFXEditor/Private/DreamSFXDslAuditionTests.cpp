#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphInstance.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "IAudioParameterTransmitter.h"
#include "MetasoundSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    namespace
    {
        using FParameters = TArray<FAudioParameter>;
        struct FInputTick
        {
            int32 Frame;
            FParameters Values;
        };

        bool ParseParameters(const FJsonObject& Object, FParameters& Values)
        {
            for (const auto& Item : Object.Values)
            {
                double Number;
                FString Label;
                if (Item.Value->TryGetNumber(Number) && FMath::IsFinite(float(Number)))
                    Values.Emplace(FName(Item.Key), float(Number));
                else if (Item.Value->TryGetString(Label))
                    Values.Emplace(FName(Item.Key), MoveTemp(Label));
                else return false;
            }
            return true;
        }

        bool ApplyParameters(FGraphInstance& Instance, const FParameters& Values)
        {
            for (const FAudioParameter& Value : Values)
            {
                if (Value.ParamType == EAudioParameterType::Float)
                {
                    if (!Instance.SetInput(Value.ParamName, Value.FloatParam)) return false;
                }
                else if (Value.ParamName != TEXT("State") || !Instance.SetState(FName(*Value.StringParam))) return false;
            }
            return true;
        }
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDslAudition, "DreamSFX.DSL.Audition",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FDslAudition::RunTest(const FString&)
    {
        constexpr int32 Rate = 48000;
        FString SpecPath, Output;
        if (!FParse::Value(FCommandLine::Get(), TEXT("DreamSFXAuditionSpec="), SpecPath)) return true;
        if (!FParse::Value(FCommandLine::Get(), TEXT("DreamSFXAuditionOut="), Output)) return false;
        FString Text;
        TSharedPtr<FJsonObject> Spec;
        if (!FFileHelper::LoadFileToString(Text, *SpecPath) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Spec) || !Spec) return false;

        TArray<FInputTick> Ticks;
        const TArray<TSharedPtr<FJsonValue>>* InputTicks;
        if (Spec->TryGetArrayField(TEXT("inputs"), InputTicks))
            for (const auto& Value : *InputTicks)
            {
                const auto Tick = Value->AsObject();
                double At;
                const TSharedPtr<FJsonObject>* Inputs;
                if (!Tick || !Tick->TryGetNumberField(TEXT("frame"), At) || At < 0 || At > MAX_int32 ||
                    At != FMath::FloorToDouble(At) || (!Ticks.IsEmpty() && At <= Ticks.Last().Frame) ||
                    !Tick->TryGetObjectField(TEXT("values"), Inputs)) return false;
                auto& Entry = Ticks.AddDefaulted_GetRef();
                Entry.Frame = int32(At);
                if (!ParseParameters(**Inputs, Entry.Values)) return false;
            }
        const TArray<TSharedPtr<FJsonValue>>* Takes;
        if (!Spec->TryGetArrayField(TEXT("takes"), Takes) || Takes->IsEmpty()) return false;
        bool bTraceVoiceGates = false;
        Spec->TryGetBoolField(TEXT("traceVoiceGates"), bTraceVoiceGates);
        auto* Async = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if (!Async) return false;
        const int32 Previous = Async->GetInt();
        Async->Set(0, ECVF_SetByCode);
        ON_SCOPE_EXIT { Async->Set(Previous, ECVF_SetByCode); };

        uint64 NextId = 923760;
        for (const auto& Take : *Takes)
        {
            const auto Row = Take->AsObject();
            FString Id, Name;
            if (!Row || !Row->TryGetStringField(TEXT("id"), Id) || !Row->TryGetStringField(TEXT("name"), Name) ||
                Name.IsEmpty() || FPaths::GetCleanFilename(Name) != Name || Name.Contains(TEXT(".."))) return false;
            const FString Package = GraphPackageName(FName(*Id));
            auto* Source = LoadObject<UMetaSoundSource>(nullptr, *(Package + TEXT(".") + FPaths::GetCleanFilename(Package)));
            const auto* Data = Source ? FindObject<UDreamSFXGraphAsset>(Source, TEXT("DreamSFXGraph")) : nullptr;
            const FGraphPtr Graph = Data ? Data->GetGraph() : nullptr;
            if (!TestTrue(Id + TEXT(" cold loads a zero-timeline program"), Graph && Graph->Program && Graph->Timeline.IsEmpty())) return false;
            const int32 Frames = FMath::RoundToInt(Graph->DurationSeconds * Rate);
            FParameters Initial;
            const TSharedPtr<FJsonObject>* Settings;
            if (Row->TryGetObjectField(TEXT("initial"), Settings) && !ParseParameters(**Settings, Initial)) return false;

            Source->InitResources();
            const int32 BlockFrames = Source->GetOperatorSettings(Rate).GetNumFramesPerBlock();
            if (BlockFrames <= 0) return false;
            FSoundGeneratorInitParams Init;
            Init.InstanceID = Init.AudioComponentId = ++NextId;
            Init.AudioDeviceID = 0;
            Init.SampleRate = Rate;
            Init.NumChannels = 2;
            Init.NumFramesPerCallback = Init.AudioMixerNumOutputFrames = BlockFrames;
            Init.GraphName = TEXT("DreamSFX DSL Audition");
            FGraphInstance Core;
            TArray<float> CoreBlock;
            FString Trace;
            if (bTraceVoiceGates)
            {
                FString Error;
                if (!Core.Prepare(Graph, Rate, Error))
                {
                    AddError(Name + TEXT(" schedule capture: ") + Error);
                    return false;
                }
                if (!ApplyParameters(Core, Initial)) return false;
                CoreBlock.SetNumUninitialized(BlockFrames * 2);
                Trace = TEXT("bank,voice,node,first_frame,end_frame\n");
                Core.ObserveProgram({}, {}, [&](FName Bank, int32 Voice, FName Node, int64 Begin, int64 End)
                {
                    Trace += FString::Printf(TEXT("%s,%d,%s,%lld,%lld\n"), *Bank.ToString(), Voice, *Node.ToString(), Begin, End);
                });
            }
            Audio::FParameterTransmitterInitParams TxInit;
            TxInit.InstanceID = NextId;
            TxInit.AudioDeviceID = 0;
            TxInit.SampleRate = Rate;
            auto Tx = Source->CreateParameterTransmitter(MoveTemp(TxInit));
            auto Generator = Source->CreateSoundGenerator(Init, MoveTemp(Initial));
            if (!Tx || !Generator) return false;
            Generator->OnBeginGenerate();
            ON_SCOPE_EXIT { Generator->OnEndGenerate(); Source->OnEndGenerate(Generator); };
            TArray<float> PCM, Block;
            PCM.Reserve((Frames + BlockFrames) * 2);
            Block.SetNumUninitialized(BlockFrames * 2);
            int32 Tick = 0;
            float Peak = 0.f;
            for (int32 At = 0; At < Frames + BlockFrames * 8 && !Generator->IsFinished(); At += BlockFrames)
            {
                while (Ticks.IsValidIndex(Tick) && Ticks[Tick].Frame <= At)
                {
                    FParameters Update = Ticks[Tick++].Values;
                    if (bTraceVoiceGates && !ApplyParameters(Core, Update)) return false;
                    if (!Tx->SetParameters(MoveTemp(Update))) return false;
                }
                Generator->GetNextBuffer(Block.GetData(), Block.Num(), true);
                if (bTraceVoiceGates)
                {
                    Core.Render(CoreBlock.GetData(), BlockFrames);
                    if (!TestTrue(Name + TEXT(" traced core matches the cold MetaSound output"),
                        FMemory::Memcmp(Block.GetData(), CoreBlock.GetData(), Block.Num() * sizeof(float)) == 0)) return false;
                }
                for (int32 I = 0; I < Block.Num(); ++I)
                {
                    const float Value = Block[I];
                    if (!FMath::IsFinite(Value) || (At + I / 2 >= Frames && Value != 0.f)) return false;
                    Peak = FMath::Max(Peak, FMath::Abs(Value));
                }
                PCM.Append(Block);
            }
            if (!TestTrue(Name + TEXT(" finishes with finite unclipped PCM"), Generator->IsFinished() &&
                PCM.Num() >= Frames * 2 && Peak > 1.e-5f && Peak < 1.f)) return false;
            if (!FFileHelper::SaveArrayToFile(MakeArrayView(reinterpret_cast<const uint8*>(PCM.GetData()), Frames * 8),
                *(Output / (Name + TEXT(".f32"))))) return false;
            if (bTraceVoiceGates && !FFileHelper::SaveStringToFile(Trace, *(Output / (Name + TEXT(".voices.csv"))),
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
            AddInfo(FString::Printf(TEXT("DSFX_AUDITION name=%s id=%s frames=%d timeline=%d block=%d peak=%.9g"),
                *Name, *Id, Frames, Graph->Timeline.Num(), BlockFrames, Peak));
        }
        return !HasAnyErrors();
    }
}
#endif
