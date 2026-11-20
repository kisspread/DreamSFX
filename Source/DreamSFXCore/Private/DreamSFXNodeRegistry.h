#pragma once

#include "DreamSFXGraphIR.h"
#include "DreamSFXInstrument.h"
#include "DreamSFXGraphDSP.h"

namespace DreamSFX::V2
{
    struct FControlSpec
    {
        FName Name;
        const TCHAR* Unit;
        double Minimum = -100000;
        double Maximum = 100000;
        bool bConstant = false;
    };

    enum class EPropertyKind { Number, Integer, Choice, Flag, NoiseBuffer, Impulse };
    enum class EEventMode : uint8 { Gate, Notes, NotesWithSustain, Hit };
    struct FPropertySpec
    {
        FName Name;
        EPropertyKind Kind;
        double Minimum = 0;
        double Maximum = 0;
        TOptional<double> Default;
        TArray<FName> Choices;
        TArray<double> AllowedNumbers;
    };

    struct FControlState
    {
        float Base = 0;
        bool bProgramDriven = false;
        DSFXGraphDSP::FParameter Curve;
        TArray<int32> Modulators;
    };

    struct FKernelContext
    {
        DSFXGraphDSP::FState& Signal;
        TConstArrayView<DSFXGraphDSP::FState> Signals;
        TConstArrayView<FControlState> Controls;
        int32 Rate;
        int32 First;
        int32 Last;
        bool bHasInput;
        int64 BlockStart;

        float Control(int32 Slot, int32 Frame) const;
        double StartingFraction() const
        {
            return BlockStart + First == Signal.Begin ? Signal.ElapsedAtFirstFrame : 0.;
        }
    };

    struct FKernel
    {
        TFunction<void(FKernelContext&)> Process;
        TFunction<void(DSFXGraphDSP::FState&)> Reset;
        TUniquePtr<IDreamSFXInstrumentEngine> Voice;
        TFunction<void(DSFXGraphDSP::FState&, const float (&)[2][DSFXGraphDSP::Block])> CommitDelayedInput;
    };

    struct FKernelSetup
    {
        const FNode& Node;
        const TMap<FName, int32>& ControlSlots;
        TConstArrayView<FControlState> Controls;
        const TMap<FName, int32>& BufferSlots;
        TConstArrayView<DSFXGraphDSP::FGeneratedBuffer> Buffers;
        DSFXGraphDSP::FState& Signal;
        int32 Rate;
        bool bStereoInput;

        int32 Control(FName Name) const { return ControlSlots.FindChecked(Name); }
        float Base(FName Name) const { return Controls[Control(Name)].Base; }
        double Number(FName Name) const { return Node.Parameters.FindChecked(Name).Get<double>(); }
        FName NameValue(FName Name) const { return Node.Parameters.FindChecked(Name).Get<FName>(); }
        bool Flag(FName Name) const { return Node.Parameters.FindChecked(Name).Get<bool>(); }
        const DSFXGraphDSP::FGeneratedBuffer& Buffer(FName Name) const
        {
            return Buffers[BufferSlots.FindChecked(Node.Parameters.FindChecked(Name).Get<FName>())];
        }
    };

    inline int32 AudioTapIndex(FName Port)
    {
        if (Port == TEXT("auxA")) return 0;
        if (Port == TEXT("auxB")) return 1;
        return INDEX_NONE;
    }

    struct FParameterIssue
    {
        const TCHAR* Field = nullptr;
        const TCHAR* Message = nullptr;
    };

    struct FNodeSpec
    {
        FNodeTypeId Type;
        bool bSource;
        bool bStereoOutput;
        TArray<FControlSpec> Controls;
        TArray<FPropertySpec> Properties;
        FKernel (*Bind)(const FKernelSetup&);
        EEventMode EventMode = EEventMode::Gate;
        bool bProcessSilence = false;
        bool bDelayedAudioInput = false;
        bool bControlOnly = false;
        bool bHasAuxSends = false;
        int32 MaxInstances = 0; // Zero has no node-specific limit.
        FParameterIssue (*ValidateParameters)(const FNode&) = nullptr;
        bool bSupportsContinuousGate = false;

        TOptional<EPortKind> Input(FName Name) const;
        TOptional<EPortKind> Output(FName Name) const;
        const FControlSpec* FindControl(FName Name) const;
    };

    const FNodeSpec& GrainNodeType();
    TConstArrayView<FNodeSpec> VoiceAndFilterTypes();
    const FNodeSpec* FindNodeType(FNodeTypeId Type);
    TOptional<DSFXGraphDSP::ERecipe> FindBufferType(FNodeTypeId Type);
    const FNodeSpec* BufferParameterSpec(DSFXGraphDSP::ERecipe Recipe);
    bool ValidBufferParameters(const FGeneratedBuffer& Resource, DSFXGraphDSP::ERecipe Recipe);
    DSFXGraphDSP::FGeneratedBuffer PrepareBuffer(const FGeneratedBuffer& Resource, DSFXGraphDSP::ERecipe Recipe);
}
