#include "DreamSFXGraphArchive.h"
#include "DreamSFXSoundProgram.h"
#include "DreamSFXNodeRegistry.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/NameAsStringProxyArchive.h"

namespace DreamSFX::V2
{
    namespace
    {
        constexpr uint32 GraphMagic = 0x32584644;
        constexpr uint32 PayloadVersion = 1;
        constexpr int32 PayloadLimit = 16 * 1024 * 1024;

        template <typename Item, typename SerializeItem>
        void ReadWriteList(FArchive& Archive, TArray<Item>& Items, int32 Limit, SerializeItem Serialize)
        {
            int32 Count = Items.Num();
            Archive << Count;
            if (Archive.IsError() || Count < 0 || Count > Limit)
            {
                Archive.SetError();
                return;
            }
            if (Archive.IsLoading())
                Items.SetNum(Count);
            for (Item& Value : Items)
            {
                if (Archive.IsError()) break;
                Serialize(Archive, Value);
            }
        }

        void ReadWriteType(FArchive& Archive, FNodeTypeId& Type)
        {
            Archive << Type.Name << Type.Version;
        }

        void ReadWritePort(FArchive& Archive, FPortRef& Port)
        {
            Archive << Port.Node << Port.Port;
        }

        void ReadWriteParameters(FArchive& Archive, TMap<FName, FValue>& Parameters)
        {
            TArray<FName> Names;
            if (Archive.IsSaving())
            {
                Parameters.GenerateKeyArray(Names);
                Names.Sort(FNameLexicalLess());
            }
            int32 Count = Names.Num();
            Archive << Count;
            if (Count < 0 || Count > 256)
            {
                Archive.SetError();
                return;
            }
            for (int32 Index = 0; Index < Count && !Archive.IsError(); ++Index)
            {
                FName Name = Archive.IsSaving() ? Names[Index] : NAME_None;
                Archive << Name;
                auto& Value = Archive.IsSaving() ? Parameters.FindChecked(Name) : Parameters.Add(Name);
                uint8 Kind = Value.GetIndex();
                Archive << Kind;
                if (Archive.IsLoading())
                {
                    switch (Kind)
                    {
                        case 0: Value.Set<double>(0.); break;
                        case 1: Value.Set<bool>(false); break;
                        case 2: Value.Set<FName>(NAME_None); break;
                        default: Archive.SetError(); return;
                    }
                }
                switch (Kind)
                {
                    case 0: Archive << Value.Get<double>(); break;
                    case 1: Archive << Value.Get<bool>(); break;
                    case 2: Archive << Value.Get<FName>(); break;
                    default: Archive.SetError(); return;
                }
            }
        }

        void ReadWriteNode(FArchive& Archive, FNode& Node)
        {
            Archive << Node.Id;
            ReadWriteType(Archive, Node.Type);
            ReadWriteParameters(Archive, Node.Parameters);
        }

        void ReadWriteEvent(FArchive& Archive, FEvent& Event)
        {
            ReadWritePort(Archive, Event.To);
            Archive << Event.AtSeconds << Event.Action << Event.Value << Event.TimeConstantSeconds;
            switch (Event.Action)
            {
                case EAction::NoteOn:
                {
                    if (Archive.IsLoading()) Event.Payload.Set<FNoteOn>(FNoteOn{});
                    auto& Note = Event.Payload.Get<FNoteOn>();
                    Archive << Note.Id << Note.Midi << Note.Velocity;
                    break;
                }
                case EAction::NoteOff:
                    if (Archive.IsLoading()) Event.Payload.Set<FNoteOff>(FNoteOff{});
                    Archive << Event.Payload.Get<FNoteOff>().Id;
                    break;
                case EAction::Sustain:
                    if (Archive.IsLoading()) Event.Payload.Set<FSustain>(FSustain{});
                    Archive << Event.Payload.Get<FSustain>().bDown;
                    break;
                case EAction::Fire:
                    if (Archive.IsLoading()) Event.Payload.Set<FFire>(FFire{});
                    Archive << Event.Payload.Get<FFire>().Velocity;
                    break;
                case EAction::Trigger:
                case EAction::ContinuousGate:
                case EAction::Set:
                case EAction::Linear:
                case EAction::Exponential:
                case EAction::Target:
                    break;
                default:
                    Archive.SetError();
                    break;
            }
        }

        void ReadWriteGraph(FArchive& Archive, FGraphIR& Graph)
        {
            uint32 Magic = GraphMagic;
            uint32 Version = PayloadVersion;
            Archive << Magic << Version;
            if (Magic != GraphMagic || Version != PayloadVersion)
            {
                Archive.SetError();
                return;
            }
            Archive << Graph.Id << Graph.DurationSeconds << Graph.VoiceTailSeconds << Graph.Seed;
            ReadWriteList(Archive, Graph.Resources, 256, [](FArchive& Ar, FGeneratedBuffer& Buffer)
            {
                Ar << Buffer.Id;
                ReadWriteType(Ar, Buffer.Type);
                Ar << Buffer.Seed << Buffer.FramesAt48k;
                const auto Recipe = FindBufferType(Buffer.Type);
                if (!Recipe) { Ar.SetError(); return; }
                if (BufferParameterSpec(*Recipe)) ReadWriteParameters(Ar, Buffer.Parameters);
            });
            ReadWriteList(Archive, Graph.Nodes, 4096, ReadWriteNode);
            ReadWriteList(Archive, Graph.Connections, 16384, [](FArchive& Ar, FConnection& Connection)
            {
                ReadWritePort(Ar, Connection.From);
                ReadWritePort(Ar, Connection.To);
                Ar << Connection.Kind << Connection.Order;
            });
            ReadWriteList(Archive, Graph.Timeline, 32768, ReadWriteEvent);
            ReadWritePort(Archive, Graph.Output);
            ReadWriteList(Archive, Graph.ExecutionOrder, 4096, [](FArchive& Ar, FName& Name) { Ar << Name; });
        }
    }

    bool EncodeGraph(const FGraphIR& Graph, TArray<uint8>& Payload)
    {
        if(Graph.Program)
        {
            // Keep the editable control rules in the package. Resolve them once at load;
            // each playing instance owns its registers, random stream and voice slots.
            FTCHARToUTF8 UTF8(*Graph.Program->Source);
            if(UTF8.Length()>PayloadLimit-4)return false;
            Payload.Reset(UTF8.Length()+4);
            Payload.Append(reinterpret_cast<const uint8*>("DSP2"),4);
            Payload.Append(reinterpret_cast<const uint8*>(UTF8.Get()),UTF8.Length());
            return true;
        }
        TArray<uint8> Bytes;
        FMemoryWriter Writer(Bytes, true);
        FNameAsStringProxyArchive Archive(Writer);
        // Archive operators take mutable values for the symmetric reader and writer.
        FGraphIR Copy = Graph;
        ReadWriteGraph(Archive, Copy);
        if (Archive.IsError() || Bytes.Num() > PayloadLimit) return false;
        Payload = MoveTemp(Bytes);
        return true;
    }

    FGraphPtr DecodeGraph(const TArray<uint8>& Payload)
    {
        if (Payload.IsEmpty() || Payload.Num() > PayloadLimit) return {};
        if(Payload.Num()>4 && FMemory::Memcmp(Payload.GetData(),"DSP2",4)==0)
        {
            FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Payload.GetData()+4),Payload.Num()-4);
            return FGraphCompiler::Compile(FString(Text.Length(),Text.Get())).Graph;
        }
        FMemoryReader Reader(Payload, true);
        FNameAsStringProxyArchive Archive(Reader);
        auto Graph = MakeShared<FGraphIR, ESPMode::ThreadSafe>();
        ReadWriteGraph(Archive, *Graph);
        if (Archive.IsError() || Reader.Tell() != Payload.Num() || Graph->Nodes.IsEmpty() ||
            Graph->Nodes.Num() != Graph->ExecutionOrder.Num()) return {};
        return Graph;
    }
}
