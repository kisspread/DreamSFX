#include "DreamSFXGraphInstance.h"
#include "DreamSFXProgramRunner.h"
#include "DreamSFXNodeRegistry.h"
#include "DreamSFXSeed.h"

namespace DreamSFX::V2
{
    using namespace DSFXGraphDSP;

    struct FGraphInstance::FImpl
    {
        struct FVoiceEvent
        {
            int64 Frame = 0;
            EAction Action = EAction::Fire;
            int32 NoteId = 0;
            int32 Midi = 0;
            float Velocity = 0;
            uint32 Seed = 0;
            bool bPedalDown = false;
        };

        struct FAudioRoute
        {
            int32 Node = INDEX_NONE;
            int32 Tap = INDEX_NONE;
        };

        struct FBoundNode
        {
            const FNodeSpec* Spec = nullptr;
            TMap<FName, int32> ControlSlots;
            TArray<FAudioRoute> AudioInputs;
            FKernel Kernel;
            TArray<FVoiceEvent> Events;
            int32 EventCursor = 0;
            int32 MusicTrack = INDEX_NONE;
        };

        TArray<FBoundNode> Nodes;
        TSharedPtr<const FSoundProgram, ESPMode::ThreadSafe> Program;
        TUniquePtr<FProgramRunner> Runner;
        int64 AuditionEnd=0;
        TArray<FState> Signals;
        TArray<FControlState> Controls;
        TArray<DSFXGraphDSP::FGeneratedBuffer> Buffers;
        TArray<int32> ChangingControls;
        TArray<int32> DelayedNodes;
        int32 Rate = 48000;
        FAudioRoute Output;
        int32 Available = 0;
        int64 Next = 0;
        int64 Position = 0;
        int64 End = 0;
        int64 NotatedEnd = 0;
        int64 Deadline = 0;
        int32 FadeFrames = 0;

        static int32 EventPriority(EAction Action)
        {
            if (Action == EAction::Sustain) return 0;
            if (Action == EAction::NoteOff) return 1;
            return 2;
        }

        bool Bind(const FGraphIR& Graph, FString& Error)
        {
            if (Graph.Nodes.IsEmpty() || Graph.ExecutionOrder.Num() != Graph.Nodes.Num())
            {
                Error = TEXT("Expected a compiled graph with a complete execution schedule");
                return false;
            }
            NotatedEnd = FMath::RoundToInt64(Graph.DurationSeconds * Rate);
            AuditionEnd=NotatedEnd;
            Deadline = NotatedEnd + FMath::RoundToInt64(Graph.VoiceTailSeconds * Rate);
            if (Deadline > NotatedEnd) FadeFrames = FMath::Max(1, FMath::RoundToInt(Rate * .005));
            TMap<FName, int32> BufferSlots;
            Buffers.Reserve(Graph.Resources.Num());
            int64 ResourceFrames = 0;
            for (const auto& Buffer : Graph.Resources)
            {
                const auto Recipe = FindBufferType(Buffer.Type);
                ResourceFrames += Buffer.FramesAt48k;
                if (!Recipe || Buffer.FramesAt48k < 1 || Buffer.FramesAt48k > 576000 || ResourceFrames > 768000 ||
                    !ValidBufferParameters(Buffer, *Recipe))
                {
                    Error = TEXT("Invalid generated resource: ") + Buffer.Id.ToString();
                    return false;
                }
                BufferSlots.Add(Buffer.Id, Buffers.Add(PrepareBuffer(Buffer, *Recipe)));
            }

            TMap<FName, const FNode*> ById;
            for (const auto& Node : Graph.Nodes) ById.Add(Node.Id, &Node);
            TMap<FName, int32> NodeSlots;
            const int32 Count = Graph.ExecutionOrder.Num();
            Nodes.SetNum(Count);
            Signals.SetNum(Count);
            for (int32 Index = 0; Index < Count; ++Index)
            {
                const FName Id = Graph.ExecutionOrder[Index];
                const FNode& Node = *ById.FindChecked(Id);
                NodeSlots.Add(Id, Index);
                FBoundNode& Bound = Nodes[Index];
                Bound.Spec = FindNodeType(Node.Type);
                if (Bound.Spec->bDelayedAudioInput) DelayedNodes.Add(Index);
                for (const auto& Control : Bound.Spec->Controls)
                {
                    const int32 Slot = Controls.AddDefaulted();
                    Bound.ControlSlots.Add(Control.Name, Slot);
                    Controls[Slot].Base = float(Node.Parameters.FindChecked(Control.Name).Get<double>());
                }
            }
            Output = {NodeSlots.FindChecked(Graph.Output.Node), AudioTapIndex(Graph.Output.Port)};
            for (const auto& Edge : Graph.Connections)
            {
                const int32 Source = NodeSlots.FindChecked(Edge.From.Node);
                FBoundNode& Target = Nodes[NodeSlots.FindChecked(Edge.To.Node)];
                if (Edge.Kind == EPortKind::Audio) Target.AudioInputs.Add({Source, AudioTapIndex(Edge.From.Port)});
                else if (Edge.Kind == EPortKind::Control) Controls[Target.ControlSlots.FindChecked(Edge.To.Port)].Modulators.Add(Source);
            }
            TArray<TArray<FPoint>> Points;
            Points.SetNum(Controls.Num());
            TArray<TMap<FName, int32>> NoteIds;
            NoteIds.SetNum(Count);
            for (const auto& Event : Graph.Timeline)
            {
                const int32 NodeIndex = NodeSlots.FindChecked(Event.To.Node);
                if (Event.Action == EAction::Trigger || Event.Action == EAction::ContinuousGate)
                {
                    FState& State = Signals[NodeIndex];
                    const bool Continuous = Event.Action == EAction::ContinuousGate;
                    const double Exact = Event.AtSeconds * Rate;
                    const int64 Frame = Continuous ? FMath::CeilToInt64(Exact) : FMath::RoundToInt64(Exact);
                    if (Event.To.Port == TEXT("start"))
                    {
                        State.Begin = Frame;
                        State.ElapsedAtFirstFrame = Continuous ? Frame - Exact : 0.;
                    }
                    else State.End = Frame;
                }
                else if (!Event.Payload.IsType<FEmptyVariantState>())
                {
                    auto& Scheduled = Nodes[NodeIndex].Events.AddDefaulted_GetRef();
                    Scheduled.Frame = FMath::RoundToInt64(Event.AtSeconds * Rate);
                    Scheduled.Action = Event.Action;
                    auto& Ids = NoteIds[NodeIndex];
                    if (const auto* Note = Event.Payload.TryGet<FNoteOn>())
                    {
                        const uint32 Ordinal = Ids.Num();
                        Scheduled.NoteId = Ordinal + 1;
                        Scheduled.Midi = Note->Midi;
                        Scheduled.Velocity = Note->Velocity;
                        Scheduled.Seed = DreamSFXSeed::Note(DreamSFXSeed::Track(Graph.Seed, Event.To.Node.ToString()), Ordinal);
                        Ids.Add(Note->Id, Scheduled.NoteId);
                    }
                    else if (const auto* Off = Event.Payload.TryGet<FNoteOff>())
                    {
                        Scheduled.NoteId = Ids.FindChecked(Off->Id);
                    }
                    else if (const auto* Sustain = Event.Payload.TryGet<FSustain>())
                    {
                        Scheduled.bPedalDown = Sustain->bDown;
                    }
                    else
                    {
                        Scheduled.Velocity = Event.Payload.Get<FFire>().Velocity;
                    }
                }
                else
                {
                    const auto Mode = Event.Action == EAction::Set ? ECurve::Set : Event.Action == EAction::Linear ? ECurve::Linear :
                        Event.Action == EAction::Exponential ? ECurve::Exponential : ECurve::Target;
                    const int32 Slot = Nodes[NodeIndex].ControlSlots.FindChecked(Event.To.Port);
                    Points[Slot].Add({Event.AtSeconds, Event.Value, Event.TimeConstantSeconds, Mode});
                }
            }
            for (int32 Slot = 0; Slot < Controls.Num(); ++Slot)
            {
                if (!Points[Slot].IsEmpty())
                {
                    Controls[Slot].Curve.Prepare(Controls[Slot].Base, Points[Slot], Rate);
                    ChangingControls.Add(Slot);
                }
            }
            // Infer channels over the entire audio graph. A delayed input can have a
            // producer later in the instantaneous schedule, including one in its own cycle.
            TArray<TArray<int32>> AudioConsumers;
            AudioConsumers.SetNum(Count);
            TArray<int32> StereoOutputs;
            for (int32 Index = 0; Index < Count; ++Index)
            {
                for (const FAudioRoute& Input : Nodes[Index].AudioInputs) AudioConsumers[Input.Node].Add(Index);
                if (Nodes[Index].Spec->bStereoOutput)
                {
                    Signals[Index].Channels = 2;
                    StereoOutputs.Add(Index);
                }
            }
            for (int32 Cursor = 0; Cursor < StereoOutputs.Num(); ++Cursor)
            {
                for (int32 Consumer : AudioConsumers[StereoOutputs[Cursor]])
                {
                    if (Signals[Consumer].Channels == 2) continue;
                    Signals[Consumer].Channels = 2;
                    StereoOutputs.Add(Consumer);
                }
            }
            for (int32 Index = 0; Index < Count; ++Index)
            {
                FBoundNode& Bound = Nodes[Index];
                FState& Signal = Signals[Index];
                const bool bStereo = Bound.AudioInputs.ContainsByPredicate([&](const FAudioRoute& Input) { return Signals[Input.Node].Channels == 2; });
                Bound.Kernel = Bound.Spec->Bind({*ById.FindChecked(Graph.ExecutionOrder[Index]), Bound.ControlSlots, Controls,
                    BufferSlots, Buffers, Signal, Rate, bStereo});
                if ((!Bound.Kernel.Process && !Bound.Kernel.Voice) ||
                    Bound.Spec->bDelayedAudioInput != bool(Bound.Kernel.CommitDelayedInput))
                {
                    Error = FString::Printf(TEXT("Cannot prepare graph node '%s'"), *Graph.ExecutionOrder[Index].ToString());
                    return false;
                }
                Bound.Events.StableSort([](const FVoiceEvent& A, const FVoiceEvent& B)
                {
                    return A.Frame == B.Frame ? EventPriority(A.Action) < EventPriority(B.Action) : A.Frame < B.Frame;
                });
                if (Bound.Kernel.Voice)
                {
                    TMap<int32, int64> OnFrames;
                    for (const auto& Event : Bound.Events)
                    {
                        if (Event.Action == EAction::NoteOn) OnFrames.Add(Event.NoteId, Event.Frame);
                        if (Event.Action == EAction::NoteOff && !OnFrames.Contains(Event.NoteId))
                        {
                            Error = TEXT("A note gate rounds to zero frames at this sample rate");
                            return false;
                        }
                    }
                }
            }
            if(Graph.Program)
            {
                Program=Graph.Program;
                TMap<FPortRef,int32> Ports;
                for(int32 I=0;I<Count;++I)for(const auto& Slot:Nodes[I].ControlSlots)
                    Ports.Add({Graph.ExecutionOrder[I],Slot.Key},Slot.Value);
                for(int32 I=0;I<Program->Tracks.Num();++I)
                {
                    auto& Bound=Nodes[NodeSlots.FindChecked(Program->Tracks[I].Instrument)];
                    if(!Bound.Events.IsEmpty()){Error=TEXT("Track has a second event source");return false;}
                    Bound.MusicTrack=I;
                    const auto& T=Program->Tracks[I];Bound.Events.Reserve(2*T.Phrase->Notes.Num()+2*T.PedalSeconds.Num()+4);
                }
                Runner=MakeUnique<FProgramRunner>(*Program,Rate,Controls,Signals,NodeSlots,Ports,[this](int32 I){ResetNode(I);});
            }
            for(auto& Node:Nodes) Node.ControlSlots.Empty();
            Reset();
            return true;
        }

        void ResetNode(int32 Index)
            {
                FState& Signal = Signals[Index];
                Signal.Active = false;
                Signal.Phase = Signal.Read = 0;
                Signal.Filter = {};
                Signal.DelayPos = Signal.DelayTail = 0;
                if (Signal.Oversampler)
                {
                    for (auto& History : Signal.Oversampler->History) History = {};
                }
                for (auto& Channel : Signal.Delay)
                {
                    if (!Channel.IsEmpty()) FMemory::Memzero(Channel.GetData(), Channel.Num() * sizeof(float));
                }
                if (Signal.Room) Signal.Room->ResetAudioHistory();
                auto& Node = Nodes[Index];
                if (Node.Kernel.Reset) Node.Kernel.Reset(Signal);
                if (Node.Kernel.Voice) Node.Kernel.Voice->ResetVoices();
                Node.EventCursor = 0;
            }
        void Reset()
        {
            Next = Position = Available = 0;
            End = Deadline;
            for (int32 Slot : ChangingControls) Controls[Slot].Curve.Reset();
            for (int32 Index = 0; Index < Signals.Num(); ++Index)
            {
                ResetNode(Index);
                if(Runner)Signals[Index].Begin=Signals[Index].End=0;
            }
            if(Runner)Runner->Restart();
        }

        int64 RenderVoice(FBoundNode& Node, FState& Signal)
        {
            auto& Voice = *Node.Kernel.Voice;
            int64 At = Next;
            const int64 Limit = FMath::Min<int64>(Next + Block, Deadline);
            int64 RequiredEnd = NotatedEnd;
            float Interleaved[2 * Block] = {};
            while (At < Limit)
            {
                while (Node.Events.IsValidIndex(Node.EventCursor) && Node.Events[Node.EventCursor].Frame == At)
                {
                    const auto& Event = Node.Events[Node.EventCursor++];
                    switch (Event.Action)
                    {
                    case EAction::NoteOn: Voice.NoteOnSeeded(Event.NoteId, Event.Midi, Event.Velocity, 0, Event.Seed); break;
                    case EAction::NoteOff: Voice.NoteOff(Event.NoteId, false); break;
                    case EAction::Sustain: Voice.SetSustainPedal(Event.bPedalDown); break;
                    case EAction::Fire: Voice.NoteOn(0, 0, Event.Velocity, 0); break;
                    default: checkNoEntry(); break;
                    }
                }
                int32 Count = 1;
                if (At < NotatedEnd)
                {
                    int64 To = FMath::Min(NotatedEnd, Limit);
                    if (Node.Events.IsValidIndex(Node.EventCursor)) To = FMath::Min(To, Node.Events[Node.EventCursor].Frame);
                    Count = int32(To - At);
                }
                else if (!Voice.HasActiveVoices()) break;
                Voice.Render(Interleaved + 2 * (At - Next), Count);
                At += Count;
                RequiredEnd = FMath::Max(RequiredEnd, At);
            }
            for (int32 Frame = 0; Frame < Block; ++Frame)
            {
                Signal.Audio[0][Frame] = Interleaved[2 * Frame];
                Signal.Audio[1][Frame] = Interleaved[2 * Frame + 1];
            }
            Signal.Active = true;
            return RequiredEnd;
        }

        static const float (&RouteAudio(const FState& Signal, int32 Tap))[2][Block]
        {
            return Tap == INDEX_NONE ? Signal.Audio : Signal.Sends->Taps[Tap];
        }

        bool MixInputs(float (&Mixed)[2][Block], TConstArrayView<FAudioRoute> Inputs) const
        {
            bool bHasInput = false;
            for (const FAudioRoute& Input : Inputs)
            {
                const FState& Upstream = Signals[Input.Node];
                if (!Upstream.Active) continue;
                const auto& Samples = RouteAudio(Upstream, Input.Tap);
                if (!bHasInput)
                {
                    FMemory::Memcpy(Mixed, Samples, sizeof(Mixed));
                    bHasInput = true;
                }
                else
                {
                    for (int32 Channel = 0; Channel < 2; ++Channel)
                        for (int32 Frame = 0; Frame < Block; ++Frame)
                            Mixed[Channel][Frame] += Samples[Channel][Frame];
                }
            }
            return bHasInput;
        }

        void Quantum()
        {
            if(Runner)
            {
                Runner->Quantum(Next);
                if(Runner->HasError()){End=Position;return;}
            }
            for (int32 Slot : ChangingControls) Controls[Slot].Curve.Fill(Next);
            // A voice that ends on the previous boundary cannot retract those frames.
            int64 RequiredEnd = FMath::Max(NotatedEnd, Next);
            for (int32 Index = 0; Index < Nodes.Num(); ++Index)
            {
                auto& Node = Nodes[Index];
                FState& Signal = Signals[Index];
                if (Node.Kernel.Voice)
                {
                    if(Node.MusicTrack!=INDEX_NONE)
                    {
                        Node.Events.Reset();Node.EventCursor=0;
                        for(const auto& E:Runner->MusicEvents(Node.MusicTrack))
                            Node.Events.Add({E.Frame,E.Action,E.NoteId,E.Midi,E.Velocity,E.Seed,E.Down});
                    }
                    RequiredEnd = FMath::Max(RequiredEnd, RenderVoice(Node, Signal));
                    continue;
                }
                Signal.Active = false;
                const bool bSource = Node.Spec->bSource;
                if (bSource && (Signal.Begin >= Next + Block || Signal.End <= Next)) continue;
                const bool bHasInput = !Node.Spec->bDelayedAudioInput && MixInputs(Signal.Audio, Node.AudioInputs);
                if (!bHasInput)
                {
                    if (!bSource && !Node.Spec->bProcessSilence && !Signal.Room && !Signal.Comp &&
                        Signal.DelayTail == 0 && !Signal.Filter.HasTail()) continue;
                    FMemory::Memzero(Signal.Audio, sizeof(Signal.Audio));
                }
                Signal.Active = true;
                const int32 First = bSource ? int32(FMath::Max<int64>(0, Signal.Begin - Next)) : 0;
                const int32 Last = bSource ? int32(FMath::Min<int64>(Block, Signal.End - Next)) : Block;
                FKernelContext Context{Signal, Signals, Controls, Rate, First, Last, bHasInput, Next};
                Node.Kernel.Process(Context);
            }
            for (int32 Index : DelayedNodes)
            {
                auto& Node = Nodes[Index];
                alignas(16) float Input[2][Block];
                if (!MixInputs(Input, Node.AudioInputs)) FMemory::Memzero(Input, sizeof(Input));
                Node.Kernel.CommitDelayedInput(Signals[Index], Input);
            }
            Available = Block;
            Next += Block;
            if (Deadline > NotatedEnd && RequiredEnd < Next) End = FMath::Min(End, RequiredEnd);
        }
    };

    FGraphInstance::FGraphInstance() = default;
    FGraphInstance::~FGraphInstance() = default;

    bool FGraphInstance::Prepare(FGraphPtr Graph, int32 SampleRate, FString& Error)
    {
        Error.Reset();
        if (!Graph || SampleRate < 8000 || SampleRate > 192000)
        {
            Error = TEXT("Expected a compiled graph and a valid sample rate");
            return false;
        }
        auto State = MakeUnique<FImpl>();
        State->Rate = SampleRate;
        if (!State->Bind(*Graph, Error)) return false;
        Impl = MoveTemp(State);
        return true;
    }

    void FGraphInstance::Restart()
    {
        if (Impl) Impl->Reset();
    }

    bool FGraphInstance::IsFinished() const
    {
        return !Impl || Impl->Position == Impl->End;
    }

    bool FGraphInstance::HasRenderError() const
    {
        return Impl && Impl->Runner && Impl->Runner->HasError();
    }

    bool FGraphInstance::SetInput(FName Name,double Value)
    { return Impl&&Impl->Runner&&Impl->Runner->Input(Name,Value); }
    void FGraphInstance::ClearInput(FName Name)
    { if(Impl&&Impl->Runner)Impl->Runner->ClearInput(Name); }
    bool FGraphInstance::SetState(FName Name)
    {
        if(!Impl||!Impl->Runner||!Impl->Runner->State(Name))return false;
        Impl->End=Impl->Deadline=Impl->NotatedEnd=Impl->Runner->Live()?MAX_int64/4:FMath::Max(Impl->Position,Impl->AuditionEnd);
        return true;
    }
    uint32 FGraphInstance::GetVoiceStarts(FName Bank)const
    { return Impl&&Impl->Runner?Impl->Runner->PlayCount(Bank):0; }
#if WITH_DEV_AUTOMATION_TESTS
    void FGraphInstance::ObserveProgram(TFunction<void(FName,double)> OnLaunch,
        TFunction<void(const FPortRef&,double,double,double)> OnTarget,
        TFunction<void(FName,int32,FName,int64,int64)> OnGate)
    {
        if(Impl&&Impl->Runner)
        {
            Impl->Runner->ObserveLaunch=MoveTemp(OnLaunch);
            Impl->Runner->ObserveTarget=MoveTemp(OnTarget);
            Impl->Runner->ObserveGate=MoveTemp(OnGate);
        }
    }
#endif

    int64 FGraphInstance::GetLengthFrames() const
    {
        return Impl ? Impl->End : 0;
    }

    int32 FGraphInstance::Render(float* Stereo, int32 Frames)
    {
        if (!Stereo || Frames <= 0) return 0;
        int32 Written = 0;
        while (Impl && Written < Frames && Impl->Position < Impl->End)
        {
            if (!Impl->Available) Impl->Quantum();
            // Quantum may discover natural completion at the current position.
            if (Impl->Position >= Impl->End) break;
            const int32 Count = int32(FMath::Min<int64>(FMath::Min(Frames - Written, Impl->Available), Impl->End - Impl->Position));
            const int32 Offset = Block - Impl->Available;
            const auto& Signal = Impl->Signals[Impl->Output.Node];
            const auto& Audio = FImpl::RouteAudio(Signal, Impl->Output.Tap);
            float* Dest = Stereo + Written * 2;
            if (!Signal.Active)
            {
                FMemory::Memzero(Dest, Count * 2 * sizeof(float));
            }
            else if (!Impl->FadeFrames || Impl->Deadline - Impl->Position - Count >= Impl->FadeFrames)
            {
                for (int32 Frame = 0; Frame < Count; ++Frame)
                {
                    const float Left = Audio[0][Offset + Frame];
                    const float Right = Audio[1][Offset + Frame];
                    Dest[Frame * 2] = FMath::IsFinite(Left) ? Left * 1.f : 0.f;
                    Dest[Frame * 2 + 1] = FMath::IsFinite(Right) ? Right * 1.f : 0.f;
                }
            }
            else
            {
                for (int32 Frame = 0; Frame < Count; ++Frame)
                {
                    const float Fade = FMath::Min(1.f, float(Impl->Deadline - Impl->Position - Frame - 1) / Impl->FadeFrames);
                    for (int32 Channel = 0; Channel < 2; ++Channel)
                    {
                        const float Sample = Audio[Channel][Frame + Offset];
                        Dest[Frame * 2 + Channel] = Fade > 0.f && FMath::IsFinite(Sample) ? Sample * Fade : 0.f;
                    }
                }
            }
            Written += Count;
            Impl->Available -= Count;
            Impl->Position += Count;
        }
        if (Written < Frames) FMemory::Memzero(Stereo + Written * 2, (Frames - Written) * 2 * sizeof(float));
        return Written;
    }
}
