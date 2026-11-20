#pragma once

#include "CoreMinimal.h"
#include "Misc/TVariant.h"

namespace DreamSFX::V2
{
    inline FName ProgramControlPin(int32 Index)
    {
        static const FName Pins[]={TEXT("Control 1"),TEXT("Control 2"),TEXT("Control 3"),TEXT("Control 4"),TEXT("Control 5"),TEXT("Control 6"),TEXT("Control 7"),TEXT("Control 8")};
        return Pins[Index];
    }
    constexpr float UnsetProgramControl=-MAX_flt;
    enum class EPortKind : uint8 { Audio, Control, Event };
    enum class EAction : uint8 { Trigger, Set, Linear, Exponential, Target, NoteOn, NoteOff, Sustain, Fire, ContinuousGate };

    struct FNodeTypeId
    {
        FName Name;
        int32 Version = 1;
        bool operator==(const FNodeTypeId&) const = default;
    };

    struct FPortRef
    {
        FName Node;
        FName Port;
        bool operator==(const FPortRef&) const = default;
        friend uint32 GetTypeHash(const FPortRef& Ref)
        {
            return HashCombine(GetTypeHash(Ref.Node), GetTypeHash(Ref.Port));
        }
    };

    // A scalar with its unit supplied by the node schema, a flag, or a resource ID.
    using FValue = TVariant<double, bool, FName>;

    struct FNode
    {
        FName Id;
        FNodeTypeId Type;
        TMap<FName, FValue> Parameters;
    };

    struct FConnection
    {
        FPortRef From, To;
        EPortKind Kind = EPortKind::Audio;
        int32 Order = 0; // Explicit sum order at a fan-in port.
    };

    struct FNoteOn { FName Id; int32 Midi; float Velocity; };
    struct FNoteOff { FName Id; };
    struct FSustain { bool bDown; };
    struct FFire { float Velocity; };
    using FEventPayload = TVariant<FEmptyVariantState, FNoteOn, FNoteOff, FSustain, FFire>;

    struct FEvent
    {
        FPortRef To;
        double AtSeconds = 0;
        EAction Action = EAction::Trigger;
        float Value = 0;
        float TimeConstantSeconds = 0;
        FEventPayload Payload;
    };

    struct FGeneratedBuffer
    {
        FName Id;
        FNodeTypeId Type;
        uint32 Seed = 0;
        int32 FramesAt48k = 0;
        TMap<FName, FValue> Parameters;
        int32 Channels=2;
    };

    // Compilation owns validation. Consumers share one immutable graph.
    struct FGraphIR
    {
        FName Id;
        double DurationSeconds = 0;
        double VoiceTailSeconds = 0; // Cap on voice decay after the authored duration.
        uint32 Seed = 0;
        TArray<FGeneratedBuffer> Resources;
        TArray<FNode> Nodes;
        TArray<FConnection> Connections;
        TArray<FEvent> Timeline; // Chronological; same-time actions keep author order.
        FPortRef Output;
        TArray<FName> ExecutionOrder; // Topological schedule of instantaneous dependencies.
        TSharedPtr<const struct FSoundProgram, ESPMode::ThreadSafe> Program;
        struct FInput { FName Name; double Minimum, Maximum, Default; };
        TArray<FInput> Inputs;
        TArray<FName> StateNames;
    };

    using FGraphPtr = TSharedPtr<const FGraphIR, ESPMode::ThreadSafe>;
}
