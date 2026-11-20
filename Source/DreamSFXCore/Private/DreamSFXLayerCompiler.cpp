#include "DreamSFXLayerCompiler.h"

namespace DreamSFX::V2
{
    namespace
    {
        using FObject = TSharedPtr<FJsonObject>;
        using FValuePtr = TSharedPtr<FJsonValue>;
        using FList = TArray<FValuePtr>;

        bool ValidPart(const FString& Name)
        {
            if (Name.IsEmpty() || Name.Len() > 64 || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return false;
            for (const TCHAR C : Name)
                if (!(C >= 'a' && C <= 'z') && !(C >= 'A' && C <= 'Z') && !(C >= '0' && C <= '9') && C != '_' && C != '-') return false;
            return true;
        }

        FObject Object(const FValuePtr& Value)
        {
            return Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        }

        FObject FieldObject(const FObject& Parent, const TCHAR* Key)
        {
            return Parent ? Object(Parent->TryGetField(Key)) : nullptr;
        }

        bool NumberAt(const FObject& Parent, const TCHAR* Key, double& Out)
        {
            const auto V = Parent->TryGetField(Key);
            if (!V || V->Type != EJson::Number) return false;
            Out = V->AsNumber();
            return FMath::IsFinite(Out);
        }

        FObject Endpoint(const FString& Node, const TCHAR* Port)
        {
            auto Ref = MakeShared<FJsonObject>();
            Ref->SetStringField(TEXT("node"), Node);
            Ref->SetStringField(TEXT("port"), Port);
            return Ref;
        }

        struct FLayerReader
        {
            TArray<FDiagnostic>& Errors;
            TMap<FString, FString>& Paths;
            FList Nodes, Wires, Events;
            TSet<FName> Names;

            bool Fail(const FString& Path, const FString& Message)
            {
                Errors.Add({TEXT("DSFXV206"), Path, Message});
                return false;
            }

            bool Fields(const FObject& Obj, const FString& Where, std::initializer_list<const TCHAR*> Keys)
            {
                if (!Obj) return Fail(Where, TEXT("Expected an object"));
                for (const auto& Field : Obj->Values)
                {
                    bool Found = false;
                    for (const auto Key : Keys) if (Field.Key == Key) { Found = true; break; }
                    if (!Found) return Fail(Where / FString(FStringView(Field.Key)), TEXT("Unknown field"));
                }
                return true;
            }

            void Wire(const FString& From, const FString& To, int32 Order, const FString& Where)
            {
                auto Edge = MakeShared<FJsonObject>();
                Edge->SetObjectField(TEXT("from"), Endpoint(From, TEXT("audio")));
                Edge->SetObjectField(TEXT("to"), Endpoint(To, TEXT("input")));
                Edge->SetNumberField(TEXT("order"), Order);
                Paths.Add(FString::Printf(TEXT("/connections/%d"), Wires.Num()), Where);
                Wires.Add(MakeShared<FJsonValueObject>(Edge));
            }

            void Gate(const FString& Node, const TCHAR* Port, double Time, const FString& Timing, const FString& Where)
            {
                auto E = MakeShared<FJsonObject>();
                E->SetNumberField(TEXT("atSeconds"), Time);
                E->SetStringField(TEXT("action"), TEXT("trigger"));
                if (!Timing.IsEmpty()) E->SetStringField(TEXT("timing"), Timing);
                E->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueObject>(Endpoint(Node, Port))});
                Paths.Add(FString::Printf(TEXT("/timeline/%d"), Events.Num()), Where);
                Events.Add(MakeShared<FJsonValueObject>(E));
            }

            bool Voice(const FObject& Row, const FList& Chain, const FString& Layer, const FString& PatchPath,
                const FString& GateId, const FString& GainId, const FString& Timing, const FString& DefaultTo,
                double TrimDb, const FString& Where)
            {
                if (!Fields(Row, Where, {TEXT("id"), TEXT("args"), TEXT("startSeconds"), TEXT("stopSeconds"), TEXT("to"), TEXT("order")})) return false;
                FString Id, Destination = DefaultTo;
                double Start, Stop, Order;
                if (!Row->TryGetStringField(TEXT("id"), Id) || !ValidPart(Id)) return Fail(Where / TEXT("id"), TEXT("Expected a stable voice ID"));
                if (Row->HasField(TEXT("to")) && !Row->TryGetStringField(TEXT("to"), Destination)) return Fail(Where / TEXT("to"), TEXT("Expected an audio destination node ID"));
                if (!NumberAt(Row, TEXT("order"), Order) || Order < 0 || Order > 127 || Order != FMath::FloorToDouble(Order))
                    return Fail(Where / TEXT("order"), TEXT("Expected integer fan-in order 0..127"));
                if (!NumberAt(Row, TEXT("startSeconds"), Start) || !NumberAt(Row, TEXT("stopSeconds"), Stop) || Start < 0. || Stop <= Start)
                    return Fail(Where, TEXT("Expected startSeconds < stopSeconds"));
                const auto Args = FieldObject(Row, TEXT("args"));
                if (!Args) return Fail(Where / TEXT("args"), TEXT("Expected named patch arguments"));
                if (Nodes.Num() + Chain.Num() > 2048 || Wires.Num() + Chain.Num() > 4096 || Events.Num() + 2 > 32768)
                    return Fail(Where, TEXT("Expanded layer exceeds the graph budget"));

                const FString Prefix = Layer + TEXT(".") + Id + TEXT(".");
                FString Previous;
                TSet<FString> Used;
                bool HasGate = false, HasGain = false;
                for (int32 Index = 0; Index < Chain.Num(); ++Index)
                {
                    const auto Template = Object(Chain[Index]);
                    const FString At = PatchPath + FString::Printf(TEXT("/chain/%d"), Index);
                    if (!Fields(Template, At, {TEXT("id"), TEXT("type"), TEXT("version"), TEXT("parameters")})) return false;
                    FString Local;
                    if (!Template->TryGetStringField(TEXT("id"), Local) || !ValidPart(Local)) return Fail(At / TEXT("id"), TEXT("Expected a local node ID"));
                    const FString Full = Prefix + Local;
                    if (Full.Len() > 96 || Names.Contains(FName(Full))) return Fail(Where / TEXT("id"), TEXT("Duplicate or oversized expanded node ID: ") + Full);
                    Names.Add(FName(Full));
                    auto Params = MakeShared<FJsonObject>();
                    const auto Inputs = FieldObject(Template, TEXT("parameters"));
                    if (!Inputs) return Fail(At / TEXT("parameters"), TEXT("Expected named node parameters"));
                    const FString Pointer = FString::Printf(TEXT("/nodes/%d"), Nodes.Num());
                    Paths.Add(Pointer, At);
                    for (const auto& Pair : Inputs->Values)
                    {
                        auto Value = Pair.Value;
                        if (const auto Ref = Object(Value))
                        {
                            FString Arg;
                            if (Ref->Values.Num() != 1 || !Ref->TryGetStringField(TEXT("arg"), Arg) || !ValidPart(Arg))
                                return Fail(At / TEXT("parameters") / FString(FStringView(Pair.Key)), TEXT("Expected a named argument reference"));
                            Value = Args->TryGetField(Arg);
                            if (!Value || (Value->Type != EJson::Number && Value->Type != EJson::String && Value->Type != EJson::Boolean))
                                return Fail(Where / TEXT("args") / Arg, TEXT("Missing or non-scalar patch argument"));
                            Used.Add(Arg);
                            Paths.Add(Pointer / TEXT("parameters") / FString(FStringView(Pair.Key)), Where / TEXT("args") / Arg);
                        }
                        Params->SetField(Pair.Key, Value);
                    }
                    if (Local == GainId)
                    {
                        FString Type;
                        double Level = 0.;
                        if (!Template->TryGetStringField(TEXT("type"), Type) || Type != TEXT("mix.gain") || !NumberAt(Params, TEXT("level"), Level))
                            return Fail(At, TEXT("Layer gain must bind a linear mix.gain node"));
                        if (TrimDb != 0.) Params->SetNumberField(TEXT("level"), Level * FMath::Pow(10., TrimDb / 20.));
                        HasGain = true;
                    }
                    auto N = MakeShared<FJsonObject>();
                    N->Values = Template->Values;
                    N->SetStringField(TEXT("id"), Full);
                    N->SetObjectField(TEXT("parameters"), Params);
                    Nodes.Add(MakeShared<FJsonValueObject>(N));
                    if (!Previous.IsEmpty()) Wire(Previous, Full, 0, At);
                    Previous = Full;
                    HasGate |= Local == GateId;
                }
                for (const auto& Pair : Args->Values)
                    if (!Used.Contains(FString(FStringView(Pair.Key)))) return Fail(Where / TEXT("args") / FString(FStringView(Pair.Key)), TEXT("Unknown patch argument"));
                if (!HasGate || !HasGain) return Fail(PatchPath, TEXT("gate and gain must identify nodes in this patch"));
                Wire(Previous, Destination, int32(Order), Where);
                Gate(Prefix + GateId, TEXT("start"), Start, Timing, Where / TEXT("startSeconds"));
                Gate(Prefix + GateId, TEXT("stop"), Stop, Timing, Where / TEXT("stopSeconds"));
                return true;
            }

            bool Read(const FObject& Root)
            {
                const auto Patches = FieldObject(Root, TEXT("patches")), Layers = FieldObject(Root, TEXT("layers"));
                if (!Patches || !Layers || Layers->Values.IsEmpty() || Patches->Values.IsEmpty() || Layers->Values.Num() > 128 || Patches->Values.Num() > 128)
                    return Fail(TEXT("/layers"), TEXT("Expected 1..128 named patches and layers"));
                const FList *N, *W, *E;
                if (!Root->TryGetArrayField(TEXT("nodes"), N) || !Root->TryGetArrayField(TEXT("connections"), W) || !Root->TryGetArrayField(TEXT("timeline"), E))
                    return Fail(TEXT("/layers"), TEXT("Missing root nodes, connections or timeline"));
                Nodes = *N; Wires = *W; Events = *E;
                for (const auto& V : Nodes)
                {
                    FString Id;
                    if (const auto Base = Object(V); Base && Base->TryGetStringField(TEXT("id"), Id)) Names.Add(FName(Id));
                }
                TArray<FString> Keys;
                for (const auto& Pair : Layers->Values) Keys.Emplace(FStringView(Pair.Key));
                Keys.Sort();
                TSet<FString> UsedPatches;
                for (const auto& Key : Keys)
                {
                    const FString Where = TEXT("/layers/") + Key;
                    if (!ValidPart(Key) || Names.Contains(FName(Key))) return Fail(Where, TEXT("Layer requires a unique ID"));
                    Names.Add(FName(Key));
                    const auto Layer = Object(Layers->TryGetField(Key));
                    if (!Fields(Layer, Where, {TEXT("patch"), TEXT("gainDb"), TEXT("to"), TEXT("voices")})) return false;
                    FString PatchName, To, GateId, GainId, Timing;
                    double GainDb;
                    if (!NumberAt(Layer, TEXT("gainDb"), GainDb) || GainDb < -96. || GainDb > 24.) return Fail(Where / TEXT("gainDb"), TEXT("Expected -96..24 dB"));
                    if (!Layer->TryGetStringField(TEXT("to"), To) || To.IsEmpty()) return Fail(Where / TEXT("to"), TEXT("Expected a destination node ID"));
                    if (!Layer->TryGetStringField(TEXT("patch"), PatchName) || !ValidPart(PatchName)) return Fail(Where / TEXT("patch"), TEXT("Expected a patch ID"));
                    const auto Patch = Object(Patches->TryGetField(PatchName));
                    if (!Patch) return Fail(Where / TEXT("patch"), TEXT("Unknown patch: ") + PatchName);
                    UsedPatches.Add(PatchName);
                    const FString PatchPath = TEXT("/patches/") + PatchName;
                    if (!Fields(Patch, PatchPath, {TEXT("chain"), TEXT("gate"), TEXT("gain"), TEXT("timing")})) return false;
                    const FList *Chain, *Voices;
                    if (!Patch->TryGetArrayField(TEXT("chain"), Chain) || Chain->IsEmpty() || Chain->Num() > 64 ||
                        !Patch->TryGetStringField(TEXT("gate"), GateId) || !Patch->TryGetStringField(TEXT("gain"), GainId))
                        return Fail(PatchPath, TEXT("Expected an audio chain, gate node and gain node"));
                    if (Patch->HasField(TEXT("timing")) && (!Patch->TryGetStringField(TEXT("timing"), Timing) || Timing != TEXT("continuous")))
                        return Fail(PatchPath / TEXT("timing"), TEXT("Expected continuous or omit timing for frame-aligned gates"));
                    if (!Layer->TryGetArrayField(TEXT("voices"), Voices) || Voices->IsEmpty()) return Fail(Where / TEXT("voices"), TEXT("Expected at least one voice"));
                    for (int32 I = 0; I < Voices->Num(); ++I)
                        if (!Voice(Object((*Voices)[I]), *Chain, Key, PatchPath, GateId, GainId, Timing, To, GainDb,
                            Where + FString::Printf(TEXT("/voices/%d"), I))) return false;
                }
                if (UsedPatches.Num() != Patches->Values.Num()) return Fail(TEXT("/patches"), TEXT("Unused patch definition"));
                Root->SetArrayField(TEXT("nodes"), Nodes);
                Root->SetArrayField(TEXT("connections"), Wires);
                Root->SetArrayField(TEXT("timeline"), Events);
                Root->RemoveField(TEXT("patches"));
                Root->RemoveField(TEXT("layers"));
                return true;
            }
        };
    }

    bool LowerLayers(const FObject& Root, TArray<FDiagnostic>& Errors, TMap<FString,FString>& SourcePaths)
    {
        if (!Root || (!Root->HasField(TEXT("patches")) && !Root->HasField(TEXT("layers")))) return true;
        return FLayerReader{Errors, SourcePaths}.Read(Root);
    }
}
