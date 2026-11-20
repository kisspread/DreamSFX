#include "DreamSFXGraphCompiler.h"
#include "DreamSFXNodeRegistry.h"
#include "DreamSFXSoundProgram.h"

namespace DreamSFX::V2
{
    static bool Schedule(FGraphIR& Graph)
    {
        TMap<FName,const FNodeSpec*> Specs;
        for(const auto& Node:Graph.Nodes)Specs.Add(Node.Id,FindNodeType(Node.Type));
                TMap<FName, int32> Pending;
                TMap<FName, TArray<FName>> Consumers;
                for (const auto& Node : Graph.Nodes) Pending.Add(Node.Id, 0);
                for (const auto& Edge : Graph.Connections)
                {
                    if (Edge.Kind == EPortKind::Audio && Specs.FindChecked(Edge.To.Node)->bDelayedAudioInput) continue;
                    ++Pending.FindChecked(Edge.To.Node);
                    Consumers.FindOrAdd(Edge.From.Node).Add(Edge.To.Node);
                }
                TArray<FName> Ready;
                for (const auto& Item : Pending)
                {
                    if (!Item.Value) Ready.Add(Item.Key);
                }
                const auto LaterFirst = [](FName A, FName B) { return B.LexicalLess(A); };
                while (!Ready.IsEmpty())
                {
                    Ready.Sort(LaterFirst);
                    const FName Node = Ready.Pop(EAllowShrinking::No);
                    Graph.ExecutionOrder.Add(Node);
                    if (const auto* Children = Consumers.Find(Node))
                    {
                        for (FName Child : *Children)
                        {
                            if (--Pending.FindChecked(Child) == 0) Ready.Add(Child);
                        }
                    }
                }
                return Graph.ExecutionOrder.Num() == Graph.Nodes.Num();
    }

    FCompileResult FGraphCompiler::Compile(const FString& Text)
    {
        FCompileResult Result;
        if(Text.Len()>1024*1024)
        {
            Result.Errors.Add({TEXT("DSFX210"),TEXT("1:1"),TEXT("Expected a sound program under 1 Mi characters")});
            return Result;
        }
        FGraphIR Graph;
        if(!CompileSoundProgram(Text,Graph,Result.Errors))return Result;
        if(!Schedule(Graph))
        {
            const FConnection* Cycle=Graph.Connections.FindByPredicate([&](const auto& E)
                {return !Graph.ExecutionOrder.Contains(E.From.Node)&&!Graph.ExecutionOrder.Contains(E.To.Node);});
            const FString Node=Cycle?Cycle->To.Node.ToString():FString();
            const int32 Dot=Node.Find(TEXT("."));const FString Bank=Node.Left(Dot);
            int32 Offset=Text.Find(TEXT("voice ")+Bank);
            int32 Line=1;for(int32 I=0;I<Offset;++I)Line+=Text[I]=='\n';
            Result.Errors.Add({TEXT("DSFX205"),FString::Printf(TEXT("%d:1"),Line),TEXT("Instantaneous cycle at ")+Node+TEXT(". Expected a causal delay in the feedback path")});
            return Result;
        }
        Result.Graph=MakeShared<const FGraphIR,ESPMode::ThreadSafe>(MoveTemp(Graph));
        return Result;
    }

    TArray<FEditableNumber> FGraphCompiler::EditableNumbers(const FGraphIR& Graph)
    {
        TArray<FEditableNumber> Result;
        if (Graph.Program)
        {
            for (const auto& In : Graph.Inputs)
                Result.Add({In.Name,TEXT("default"),In.Default,In.Minimum,In.Maximum,TEXT(""),false});
            return Result;
        }
        for (const auto& Node : Graph.Nodes)
        {
            const FNodeSpec* Spec = FindNodeType(Node.Type);
            if (!Spec) continue;
            for (const auto& Control : Spec->Controls)
                Result.Add({Node.Id, Control.Name, Node.Parameters.FindChecked(Control.Name).Get<double>(),
                    Control.Minimum, Control.Maximum, Control.Unit, false});
            for (const auto& Field : Spec->Properties)
                if ((Field.Kind == EPropertyKind::Number || Field.Kind == EPropertyKind::Integer) && Field.AllowedNumbers.IsEmpty())
                    Result.Add({Node.Id, Field.Name, Node.Parameters.FindChecked(Field.Name).Get<double>(),
                        Field.Minimum, Field.Maximum, FString(), Field.Kind == EPropertyKind::Integer});
        }
        return Result;
    }
}
