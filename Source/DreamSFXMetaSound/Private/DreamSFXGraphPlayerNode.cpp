#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphInstance.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundDataReferenceMacro.h"
#include "MetasoundDataTypeRegistrationMacro.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundParamHelper.h"
#include "MetasoundTrigger.h"

#define LOCTEXT_NAMESPACE "DreamSFX_GraphPlayer"

namespace Metasound
{
    struct FDreamSFXGraphRef
    {
        FDreamSFXGraphRef() = default;
        explicit FDreamSFXGraphRef(const TSharedPtr<const Audio::IProxyData>& Proxy)
        {
            if (Proxy && Proxy->CheckTypeCast<FDreamSFXGraphProxy>())
            {
                Graph = Proxy->GetAs<const FDreamSFXGraphProxy>().Graph;
            }
        }
        DreamSFX::V2::FGraphPtr Graph;
    };
    DECLARE_METASOUND_DATA_REFERENCE_TYPES(FDreamSFXGraphRef, DREAMSFXMETASOUND_API,
        FDreamSFXGraphRefTypeInfo, FDreamSFXGraphRefReadRef, FDreamSFXGraphRefWriteRef)
    template<> struct TIsExplicit<FDreamSFXGraphRef> { static constexpr bool Value = true; };
}

REGISTER_METASOUND_DATATYPE(Metasound::FDreamSFXGraphRef, "DreamSFXGraph", Metasound::ELiteralType::UObjectProxy, UDreamSFXGraphAsset);

namespace DreamSFX::V2
{
    using namespace Metasound;

    namespace Pins
    {
        METASOUND_PARAM(Play, "Play", "Starts the graph on the trigger frame.");
        METASOUND_PARAM(Loop, "Loop", "Repeat immediately on natural completion.");
        METASOUND_PARAM(Graph, "Graph", "Compiled DreamSFX graph.");
        METASOUND_PARAM(State, "State", "Named live state. Leave empty to run the authored audition.");
        METASOUND_PARAM(Left, "Out Left", "Left channel.");
        METASOUND_PARAM(Right, "Out Right", "Right channel.");
        METASOUND_PARAM(Finished, "On Finished", "Emitted when the graph reaches its last frame.");
    }

    using namespace Pins;

    class FGraphPlayerOperator final : public TExecutableOperator<FGraphPlayerOperator>
    {
    public:
        FGraphPlayerOperator(const FOperatorSettings& Settings, const FTriggerReadRef& InPlay, const FDreamSFXGraphRefReadRef& InGraph, const FBoolReadRef& InLoop,const FBuildOperatorParams& Params)
            : Play(InPlay), Graph(InGraph), Loop(InLoop), Left(FAudioBufferWriteRef::CreateNew(Settings)), Right(FAudioBufferWriteRef::CreateNew(Settings)),
            Finished(FTriggerWriteRef::CreateNew(Settings)),
            StateInput(Params.InputData.GetOrCreateDefaultDataReadReference<FString>(METASOUND_GET_PARAM_NAME(State), Settings))
        {
            for (int32 Index = 0; Index < 8; ++Index)
                InputValues.Add(Params.InputData.GetOrCreateDefaultDataReadReference<float>(ProgramControlPin(Index), Settings));

            if (Graph->Graph)
            {
                for (FName Name : Graph->Graph->StateNames) StateLabels.Add(Name.ToString());
                for (const auto& Input : Graph->Graph->Inputs) ControlDefaults.Add(Input.MetaSoundDefault());
            }
            FString Error;
            bPrepared = Instance.Prepare(Graph->Graph, FMath::RoundToInt(Settings.GetSampleRate()), Error);
            if (Graph->Graph && !bPrepared) UE_LOG(LogTemp, Error, TEXT("DreamSFX graph prepare failed: %s"), *Error);
            // Retain enough trigger storage for every frame, including a completion on the block boundary.
            for (int32 Frame = 0; Frame <= Settings.GetNumFramesPerBlock() + 1; ++Frame) Finished->TriggerFrame(Frame);
            Finished->Reset();
        }

        static const FVertexInterface& DeclareVertexInterface()
        {
            static const FVertexInterface Interface=[](){
                FInputVertexInterface Inputs(
                    TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(Play)),
                    TInputDataVertex<FDreamSFXGraphRef>(METASOUND_GET_PARAM_NAME_AND_METADATA(Graph)),
                    TInputDataVertex<bool>(METASOUND_GET_PARAM_NAME_AND_METADATA(Loop), false),
                    TInputDataVertex<FString>(METASOUND_GET_PARAM_NAME_AND_METADATA(State),FString()));
                for(int32 I=0;I<8;++I)Inputs.Add(TInputDataVertex<float>(ProgramControlPin(I),FDataVertexMetadata::EmptyAdvanced,0.f));
                return FVertexInterface(MoveTemp(Inputs),
                FOutputVertexInterface(
                    TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Left)),
                    TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Right)),
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(Finished))));}();
            return Interface;
        }

        static const FNodeClassMetadata& GetNodeInfo()
        {
            static const FNodeClassMetadata Metadata = []
            {
                FNodeClassMetadata Info;
                Info.ClassName = {TEXT("DreamSFX"), TEXT("GraphPlayer"), NAME_None};
                Info.MajorVersion = 1;
                Info.MinorVersion = 2;
                Info.DisplayName = LOCTEXT("GraphPlayerName", "DreamSFX Graph Player");
                Info.Description = LOCTEXT("GraphPlayerDescription", "Plays one immutable DreamSFX graph.");
                Info.Author = TEXT("DreamSFX");
                Info.DefaultInterface = DeclareVertexInterface();
                Info.CategoryHierarchy = {LOCTEXT("Category", "DreamSFX")};
                return Info;
            }();
            return Metadata;
        }

        static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& Params, FBuildResults&)
        {
            const FOperatorSettings& Settings = Params.OperatorSettings;
            return MakeUnique<FGraphPlayerOperator>(Settings,
                Params.InputData.GetOrCreateDefaultDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(Play), Settings),
                Params.InputData.GetOrCreateDefaultDataReadReference<FDreamSFXGraphRef>(METASOUND_GET_PARAM_NAME(Graph), Settings),
                Params.InputData.GetOrCreateDefaultDataReadReference<bool>(METASOUND_GET_PARAM_NAME(Loop), Settings),Params);
        }

        void BindInputs(FInputVertexInterfaceData& Inputs) override
        {
            Inputs.BindReadVertex(METASOUND_GET_PARAM_NAME(Play), Play);
            Inputs.BindReadVertex(METASOUND_GET_PARAM_NAME(Graph), Graph);
            Inputs.BindReadVertex(METASOUND_GET_PARAM_NAME(Loop), Loop);
            Inputs.BindReadVertex(METASOUND_GET_PARAM_NAME(State), StateInput);
            for(int32 I=0;I<InputValues.Num();++I)Inputs.BindReadVertex(ProgramControlPin(I),InputValues[I]);
        }

        void BindOutputs(FOutputVertexInterfaceData& Outputs) override
        {
            Outputs.BindReadVertex(METASOUND_GET_PARAM_NAME(Left), Left);
            Outputs.BindReadVertex(METASOUND_GET_PARAM_NAME(Right), Right);
            Outputs.BindReadVertex(METASOUND_GET_PARAM_NAME(Finished), Finished);
        }

        void Reset(const IOperator::FResetParams&)
        {
            bPlaying = false;
            Left->Zero();
            Right->Zero();
            Finished->Reset();
        }

        void Execute()
        {
            Left->Zero();
            Right->Zero();
            Finished->AdvanceBlock();
            if (Graph->Graph && Graph->Graph->Program)
            {
                const FString& Requested = *StateInput;
                if(Requested.IsEmpty())
                {
                    if(LastState!=INDEX_NONE){Instance.SetState(NAME_None);LastState=INDEX_NONE;}
                }
                else if(!StateLabels.IsValidIndex(LastState)||StateLabels[LastState]!=Requested)
                {
                    const int32 I=StateLabels.IndexOfByKey(Requested);
                    if(I!=INDEX_NONE){Instance.SetState(Graph->Graph->StateNames[I]);LastState=I;}
                }
                for (int32 Index = 0; Index < ControlDefaults.Num(); ++Index)
                {
                    const FName Name = Graph->Graph->Inputs[Index].Name;
                    const float Value = *InputValues[Index];
                    if (FMath::AsUInt(Value) == FMath::AsUInt(ControlDefaults[Index]) || Value == UnsetProgramControl)
                        Instance.ClearInput(Name);
                    else
                        Instance.SetInput(Name, Value);
                }
            }
            const int32 BlockFrames = Left->Num();
            int32 Cursor = 0;
            for (int32 Frame : Play->GetTriggeredFrames())
            {
                if (Frame >= BlockFrames) break;
                Render(Cursor, Frame);
                Cursor = Frame;
                if (bPrepared)
                {
                    Instance.Restart();
                    bPlaying = true;
                }
                else Finished->TriggerFrame(Frame);
            }
            Render(Cursor, BlockFrames);
        }

    private:
        void Render(int32 Begin, int32 End)
        {
            while (bPlaying && Begin < End)
            {
                float Stereo[512];
                const int32 Count = Instance.Render(Stereo, FMath::Min(End - Begin, 256));
                for (int32 Frame = 0; Frame < Count; ++Frame)
                {
                    Left->GetData()[Begin + Frame] = Stereo[Frame * 2];
                    Right->GetData()[Begin + Frame] = Stereo[Frame * 2 + 1];
                }
                Begin += Count;
                if (Instance.IsFinished())
                {
                    if (*Loop && (Count > 0 || Instance.GetLengthFrames() > 0)) Instance.Restart();
                    else
                    {
                        bPlaying = false;
                        Finished->TriggerFrame(Begin);
                    }
                }
            }
        }

        FTriggerReadRef Play;
        FDreamSFXGraphRefReadRef Graph;
        FBoolReadRef Loop;
        FAudioBufferWriteRef Left, Right;
        FTriggerWriteRef Finished;
        TArray<FFloatReadRef> InputValues;
        TArray<float, TInlineAllocator<8>> ControlDefaults;
        FStringReadRef StateInput;
        TArray<FString> StateLabels;
        int32 LastState=INDEX_NONE;
        FGraphInstance Instance;
        bool bPlaying = false;
        bool bPrepared = false;
    };

    using FGraphPlayerNode = TNodeFacade<FGraphPlayerOperator>;
    METASOUND_REGISTER_NODE(FGraphPlayerNode)
}

#undef LOCTEXT_NAMESPACE
