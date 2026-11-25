#include "DreamSFXSoundProgram.h"
#include "DreamSFXNodeRegistry.h"
#include "DreamSFXDslSyntax.h"
#include "DreamSFXScoreParser.h"
#include "DreamSFXScoreTrack.h"

namespace DreamSFX::V2
{
namespace ProgramSyntax
{
    using Dsl::FStatement;
    using FList=TArray<FStatement>;
    bool Number(const FString& Text,double& Out)
    {
        if(!Text.IsNumeric())return false;
        Out=FCString::Atod(*Text);return FMath::IsFinite(Out);
    }
    FString Trim(FString S){S.TrimStartAndEndInline();return S;}
    bool NameOk(const FString& S)
    {
        if (S.IsEmpty() || S.Len()>48 || !FChar::IsAlpha(S[0])) return false;
        for (auto C:S) if (!FChar::IsAlnum(C) && C!='_') return false;
        return true;
    }
    TArray<FString> Split(const FString& S,TCHAR Sep)
    {
        TArray<FString> Out;
        if (S.IsEmpty()) return Out;
        int32 Start=0,Depth=0;
        for (int32 I=0;I<S.Len();++I)
        {
            if (S[I]=='(') ++Depth;
            else if (S[I]==')') --Depth;
            else if (S[I]==Sep && !Depth)
            {
                Out.Add(Trim(S.Mid(Start,I-Start))); Start=I+1;
            }
        }
        Out.Add(Trim(S.Mid(Start)));return Out;
    }
    const TMap<FString,FString> Types={
        {TEXT("sine"),TEXT("osc.sine")},{TEXT("saw"),TEXT("osc.sawtooth")},{TEXT("triangle"),TEXT("osc.triangle")},
        {TEXT("square"),TEXT("osc.square")},{TEXT("noise"),TEXT("source.noise_buffer")},{TEXT("gain"),TEXT("mix.gain")},
        {TEXT("pan"),TEXT("mix.pan")},{TEXT("lowpass"),TEXT("filter.lowpass")},{TEXT("highpass"),TEXT("filter.highpass")},
        {TEXT("bandpass"),TEXT("filter.bandpass")},{TEXT("shelf"),TEXT("filter.lowshelf")},{TEXT("echo"),TEXT("delay.causal")},
        {TEXT("delay"),TEXT("delay.linear")},{TEXT("shape"),TEXT("shape.tanh")},{TEXT("room"),TEXT("room.convolution")},
        {TEXT("compressor"),TEXT("dynamics.compressor")}};
    FString Field(const FString& K,const FString& Type)
    {
        if (K==TEXT("hz")) return Type.StartsWith(TEXT("osc."))?TEXT("frequencyHz"):TEXT("cutoffHz");
        if (K==TEXT("q") && Type!=TEXT("filter.bandpass")) return TEXT("resonanceDb");
        if (K==TEXT("cents")) return TEXT("detuneCents");
        if (K==TEXT("seconds")) return TEXT("delaySeconds");
        if (K==TEXT("ir")) return TEXT("impulse");
        return K;
    }
    // Rules can refer forward to send buses or other routines. DSP input order is the order of paths.
    struct FWire { FString A,B; FStatement Origin; };
    struct FBuilder
    {
        FSoundProgram& P;
        int32 GlobalRegisters() const{return Names.Num();}
        FGraphIR& Graph;
        FStatement Origin;
        FString Error;
        int32 ErrorOffset=0;
        const TCHAR* ErrorCode = TEXT("DSFX210");
        FString Mark;
        TMap<FString,const FStatement*> Patches;
        void Locate(const FStatement& S){Origin.Text=S.Text;Origin.Line=S.Line;Origin.Column=S.Column;Mark.Reset();}
        void Focus(const FString& S){Mark=S;}

        TMap<FString,int32> Names, BankIds, RoutineIds, StateIds;
        TMap<FString,double> Constants{{TEXT("pi"),UE_DOUBLE_PI}};
        TMap<FString,FString> NodeTypes;
        TMap<FString,int32> Versions;
        TSet<FString> Writable;
        TMap<FName,int32> ResourceTypes;
        TMap<FName,int32> TypeCounts;
        TArray<FWire> Wires;
        bool Fail(const FString& S){ if (Error.IsEmpty()){Error=S;ErrorOffset=Mark.IsEmpty()?0:FMath::Max(0,Origin.Text.Find(Mark,ESearchCase::CaseSensitive));}return false; }
        bool Expr(const FString& S,FProgramExpr& E)
        {
            FString Why;int32 Offset=0;
            if(E.Parse(S,Names,Constants,Why,&Offset))
            {
                E.Line=Origin.Line;E.Column=Origin.Column+FMath::Max(0,Origin.Text.Find(S,ESearchCase::CaseSensitive));
                if(E.Constant())
                {
                    uint32 Dummy=0;bool Ok=false;E.Eval({},Dummy,&Ok);
                    if(!Ok){Focus(S);return Fail(TEXT("Expected a finite expression with valid arithmetic operands"));}
                }
                return true;
            }
            Focus(S);Fail(Why+TEXT(" in ")+S);ErrorOffset+=Offset;return false;
        }
        bool Register(const FString& Name,int32& Out)
        {
            if (!NameOk(Name)||Names.Contains(Name)||Constants.Contains(Name)||P.RegisterCount>=2048)
                return Fail(TEXT("Invalid or duplicate register ")+Name);
            Out=P.RegisterCount++;Names.Add(Name,Out);return true;
        }
        bool Assignment(const FString& Text,TArray<FAssignment>& Out)
        {
            FString Key,Value;
            if (!Text.Split(TEXT("="),&Key,&Value))return Fail(TEXT("Expected name=expression"));
            auto& A=Out.AddDefaulted_GetRef();Key=Trim(Key);
            return Expr(Value,A.Expr) && Register(Key,A.Register);
        }
        bool Curve(const FString& Text,FRuleCurve& C)
        {
            static const TMap<FString,ERuleCurve> Kinds={{TEXT("set"),ERuleCurve::Set},{TEXT("follow"),ERuleCurve::Follow},
                {TEXT("target"),ERuleCurve::Target},{TEXT("line"),ERuleCurve::Linear},{TEXT("exp"),ERuleCurve::Exponential},
                {TEXT("keyexp"),ERuleCurve::ExponentialKeys},{TEXT("exactexp"),ERuleCurve::ExactExponentialKeys},
                {TEXT("swell"),ERuleCurve::Swell}};
            int32 At;
            if (Text.FindChar('(',At) && Text.EndsWith(TEXT(")")))
                if (const auto* K=Kinds.Find(Text.Left(At)))
                {
                    C.Kind=*K;
                    for(const auto& Arg:Split(Text.Mid(At+1,Text.Len()-At-2),','))
                        if(!Expr(Arg,C.Args.AddDefaulted_GetRef()))return false;
                    const int32 N=C.Args.Num();
                    if ((*K==ERuleCurve::Set&&N==1)||(*K==ERuleCurve::Target&&(N==2||N==3))||
                        ((*K==ERuleCurve::Follow||*K==ERuleCurve::Swell)&&N==3)||(*K==ERuleCurve::Swell&&N==4)||
                        ((*K==ERuleCurve::Linear||*K==ERuleCurve::Exponential||*K==ERuleCurve::ExponentialKeys||
                          *K==ERuleCurve::ExactExponentialKeys)&&N>=2&&N<=64&&N%2==0))return true;
                    return Fail(TEXT("Invalid curve arguments: ")+Text);
                }
            return Expr(Text,C.Args.AddDefaulted_GetRef());
        }
        bool ReadResources(const FStatement& Section,uint32 Seed)
        {
            int64 Total=0;
            for(const auto& L:Section.Children)
            {
                Locate(L); FString Id,Spec;
                if(!L.Text.Split(TEXT("="),&Id,&Spec))return Fail(TEXT("Expected name = resource.type(seconds)"));
                Id=Trim(Id);Spec=Trim(Spec); double N=0;
                if(Id.StartsWith(TEXT("skip ")))
                {
                    if(!Number(Spec,N)||N<0||N>MAX_uint32||N!=std::floor(N))return Fail(TEXT("Expected a nonnegative draw count"));
                    Seed=AdvanceProgramRandom(Seed,uint32(N),P.Algorithm);continue;
                }
                int32 Open;
                if(!Spec.FindChar('(',Open)||!Spec.EndsWith(TEXT(")")))return Fail(TEXT("Expected resource.type(duration, named fields)"));
                const FNodeTypeId Type{FName(Spec.Left(Open)),1};const auto Kind=FindBufferType(Type);
                if(!Kind)return Fail(TEXT("Expected a registered resource recipe"));
                const auto Args=Split(Spec.Mid(Open+1,Spec.Len()-Open-2),',');
                if(Args.IsEmpty()||!Args[0].EndsWith(TEXT("s"))||!Number(Args[0].LeftChop(1),N)||N<=0||N>12)return Fail(TEXT("Expected a duration in (0,12] seconds"));
                if(!NameOk(Id)||ResourceTypes.Contains(FName(Id)))return Fail(TEXT("Expected a unique resource name"));
                const int32 Frames=FMath::RoundToInt(N*48000.);
                if(Frames<1||(Total+=Frames)>768000)return Fail(TEXT("Resource frame budget exceeded"));
                FGeneratedBuffer Buffer{FName(Id),Type,Seed,Frames,{}};
                const auto* Schema=BufferParameterSpec(*Kind);
                int32 Channels=2;bool PrivateSeed=false;
                for(int32 I=1;I<Args.Num();++I)
                {
                    FString K,V;Focus(Args[I]);
                    if(Args[I].Split(TEXT("="),&K,&V)&&Trim(K)==TEXT("seed"))
                    {
                        if(PrivateSeed||!Number(Trim(V),N)||N!=std::floor(N)||N<0||N>MAX_uint32)return Fail(TEXT("Expected a unique uint32 resource seed"));
                        Buffer.Seed=uint32(N);PrivateSeed=true;continue;
                    }
                    if(Args[I].Split(TEXT("="),&K,&V)&&Trim(K)==TEXT("channels"))
                    {
                        if(Trim(V)!=TEXT("1")||*Kind!=DSFXGraphDSP::ERecipe::White)return Fail(TEXT("channels=1 requires a white noise buffer"));
                        Channels=1;Buffer.Channels=1;continue;
                    }
                    if(!Schema||!Args[I].Split(TEXT("="),&K,&V))return Fail(TEXT("Expected a named resource field"));
                    const FName Key(Trim(K));V=Trim(V);
                    const auto* Prop=Schema->Properties.FindByPredicate([&](const auto& P){return P.Name==Key;});
                    if(!Prop||Buffer.Parameters.Contains(Key))return Fail(TEXT("Unknown or repeated resource field ")+K);
                    if(Prop->Kind==EPropertyKind::Choice)Buffer.Parameters.Add(Key,FValue(TInPlaceType<FName>(),FName(V)));
                    else if(Prop->Kind==EPropertyKind::Flag && (V==TEXT("0")||V==TEXT("1")))Buffer.Parameters.Add(Key,FValue(TInPlaceType<bool>(),V==TEXT("1")));
                    else if(Number(V,N))
                    {
                        if(N<Prop->Minimum || N>Prop->Maximum || (Prop->Kind==EPropertyKind::Integer && N!=std::floor(N))
                            || (Key==TEXT("crossfadeFrames") && N>Frames))
                        {Focus(Trim(K));return Fail(TEXT("Value outside the resource range for ")+Trim(K));}
                        Buffer.Parameters.Add(Key,FValue(TInPlaceType<double>(),N));
                    }
                    else return Fail(TEXT("Expected a valid literal for ")+K);
                }
                if(!ValidBufferParameters(Buffer,*Kind))return Fail(TEXT("Expected all required resource fields within their declared ranges"));
                const auto* Overlap=Buffer.Parameters.Find(TEXT("crossfadeFrames"));
                const int32 Extra=Overlap?2*int32(Overlap->Get<double>()):0;
                Graph.Resources.Add(MoveTemp(Buffer));ResourceTypes.Add(FName(Id),int32(*Kind));
                if(!PrivateSeed)Seed=AdvanceProgramRandom(Seed,Channels*Frames+Extra+(*Kind==DSFXGraphDSP::ERecipe::Room?28:0),P.Algorithm);
            }
            P.Random=Seed;return true;
        }
        bool AddNode(const FString& Id,const FString& Op,const FString& Args,FProgramVoice& V)
        {
            FString Type=Op,VersionText,TypeName;
            int32 Version=1;
            if(Op.Split(TEXT("@"),&TypeName,&VersionText))
            {
                double N;
                if(!Number(VersionText,N)||N!=std::floor(N)||N<1||N>65535)return Fail(TEXT("Expected a positive node version"));
                Type=TypeName;Version=int32(N);
            }
            const FString Native=Types.Contains(Type)?Types[Type]:Type;
            const auto* Spec=FindNodeType({FName(Native),Version});
            Focus(Op);
            if(!Spec)return Fail(TEXT("Expected a registered DSP node, found ")+Op);
            if(NodeTypes.Contains(Id)||Graph.Nodes.Num()>=2048)return Fail(TEXT("Expected a unique node within the graph budget: ")+Id);
            if(Spec->MaxInstances>0&&++TypeCounts.FindOrAdd(Spec->Type.Name)>Spec->MaxInstances)return Fail(TEXT("Node instance budget exceeded"));
            NodeTypes.Add(Id,Native);Versions.Add(Id,Version);V.Nodes.Add(FName(Id));
            auto& Node=Graph.Nodes.AddDefaulted_GetRef();Node.Id=FName(Id);Node.Type=Spec->Type;
            auto Numeric=[&](FName Key,double Value){Node.Parameters.Add(Key,FValue(TInPlaceType<double>(),Value));};
            for(const auto& C:Spec->Controls)
            {
                double Base=FMath::Clamp(0.,C.Minimum,C.Maximum);
                if(C.Name==TEXT("level")||C.Name==TEXT("playbackRate"))Base=1;
                if(C.Name==TEXT("q")||C.Name==TEXT("resonanceDb"))Base=.7;
                Numeric(C.Name,Base);
            }
            for(const auto& F:Spec->Properties)if(F.Default)
            {
                if(F.Kind==EPropertyKind::Flag)
                    Node.Parameters.Add(F.Name,FValue(TInPlaceType<bool>(),F.Default.GetValue()!=0.));
                else Numeric(F.Name,F.Default.GetValue());
            }
            if(Native==TEXT("shape.tanh")){Numeric(TEXT("scale"),1);Node.Parameters.Add(TEXT("oversample2x"),FValue(TInPlaceType<bool>(),false));}
            const bool bNoiseBuffer = Native==TEXT("source.noise_buffer") || Native==TEXT("source.mono_noise_buffer");
            if(bNoiseBuffer)Numeric(TEXT("offsetSeconds"),0.);
            FProgramSource Gate;Gate.Node=Node.Id;TSet<FName> Seen;bool Normalize=false;
            for(const auto& Item:Split(Args,','))
            {
                FString K,S;Focus(Item);
                if(!Item.Split(TEXT("="),&K,&S))return Fail(TEXT("Expected field=value"));
                K=Field(Trim(K),Native);S=Trim(S);const FName Key(K);
                if(Seen.Contains(Key))return Fail(TEXT("Duplicate field ")+K);Seen.Add(Key);
                if(K==TEXT("life") || (K==TEXT("offset") && bNoiseBuffer))
                {
                    if(!Spec->bSource)return Fail(TEXT("Invalid source field ")+K);
                    if(!Expr(S,K==TEXT("offset")?Gate.Offset:Gate.Life))return false;
                    continue;
                }
                const auto* C=Spec->FindControl(Key);
                const auto* F=Spec->Properties.FindByPredicate([&](const FPropertySpec& A){return A.Name==Key;});
                if(!C && !F && !(Native==TEXT("shape.tanh")&&K==TEXT("normalized")))return Fail(TEXT("Unknown field ")+K+TEXT(" on ")+Native);
                if(F&&(F->Kind==EPropertyKind::Impulse||F->Kind==EPropertyKind::NoiseBuffer||F->Kind==EPropertyKind::Choice))
                {
                    const FName Value(S);
                    if(F->Kind==EPropertyKind::Choice){if(!F->Choices.Contains(Value))return Fail(TEXT("Expected a supported choice for ")+K);}
                    else if(!ResourceTypes.Contains(Value)||(ResourceTypes[Value]==int32(DSFXGraphDSP::ERecipe::Room)||ResourceTypes[Value]==int32(DSFXGraphDSP::ERecipe::DiffuseImpulse))!=(F->Kind==EPropertyKind::Impulse))
                        return Fail(TEXT("Expected a declared resource of the correct type"));
                    Node.Parameters.Add(Key,FValue(TInPlaceType<FName>(),Value));continue;
                }
                FRuleCurve CurveValue;if(!Curve(S,CurveValue))return false;
                if(CurveValue.Kind==ERuleCurve::Set&&CurveValue.Args[0].Constant())
                {
                    uint32 Dummy=0;const double Value=CurveValue.Args[0].Eval({},Dummy);
                    if(K==TEXT("normalized"))Normalize=Value!=0;
                    else if(F&&F->Kind==EPropertyKind::Flag)
                    {
                        if(Value!=0 && Value!=1)return Fail(TEXT("Expected 0 or 1"));
                        Node.Parameters.Add(Key,FValue(TInPlaceType<bool>(),Value!=0));
                    }
                    else
                    {
                        if((C&&!FMath::IsWithinInclusive(Value,C->Minimum,C->Maximum))||
                            (F&&(!FMath::IsWithinInclusive(Value,F->Minimum,F->Maximum)||(F->Kind==EPropertyKind::Integer&&Value!=std::floor(Value))||
                            (!F->AllowedNumbers.IsEmpty()&&!F->AllowedNumbers.Contains(Value)))))return Fail(TEXT("Value is outside the declared range for ")+K);
                        Numeric(Key,Value);
                    }
                }
                else
                {
                    if(!C||C->bConstant)return Fail(TEXT("Expected a constant for ")+K);
                    int32 I=V.Bindings.Add({{Node.Id,Key},MoveTemp(CurveValue)});V.Operations.Add({false,I});
                }
            }
            for(const auto& F:Spec->Properties)if(!Node.Parameters.Contains(F.Name))return Fail(TEXT("Missing required field ")+F.Name.ToString());
            if(Normalize)Numeric(TEXT("scale"),1./std::tanh(Node.Parameters.FindChecked(TEXT("drive")).Get<double>()));
            if(Spec->ValidateParameters){auto Issue=Spec->ValidateParameters(Node);if(Issue.Message)return Fail(Issue.Message);}
            if(Spec->bSource && Spec->EventMode==EEventMode::Gate)
            {
                const int32 I=V.Sources.Add(MoveTemp(Gate));V.Operations.Add({true,I});
            }
            return true;
        }
        bool Paths(const FList& Lines,const FString& Prefix,const FString& Outlet,FProgramVoice& V)
        {
            int32 Anonymous=0;
            for(const auto& Line:Lines)
            {
                Locate(Line);FString Last;
                if(Line.Block)return Fail(TEXT("Expected a signal path"));
                FString Text=Line.Text.Replace(TEXT("->"),TEXT("→"));
                for(const FString& Part:Split(Text,TCHAR(0x2192)))
                {
                    FString Id;int32 At;
                    if(Part.FindChar('(',At))
                    {
                        if(!Part.EndsWith(TEXT(")")))return Fail(TEXT("Unclosed node in ")+Part);
                        FString Head=Trim(Part.Left(At)),Op;
                        if(!Head.Split(TEXT("="),&Id,&Op)){Op=Head;Id=FString::Printf(TEXT("_%d"),Anonymous++);}
                        Id=Prefix+Trim(Id);
                        if(!AddNode(Id,Trim(Op),Part.Mid(At+1,Part.Len()-At-2),V))return false;
                    }
                    else Id=Part==TEXT("outlet")?Outlet:(Part.Contains(TEXT("."))?Part:Prefix+Part);
                    if(!Last.IsEmpty())Wires.Add({Last,Id,Origin});
                    Last=Id;
                }
            }
            return true;
        }
        bool Voice(const FStatement& Decl)
        {
            Locate(Decl);
            FString Header=Trim(Decl.Text.Mid(6)),Outlet,Left;int32 Count=1;
            if(Header.Split(TEXT("->"),&Left,&Outlet)){Header=Trim(Left);Outlet=Trim(Outlet);}
            FString Id=Header,RowName;TArray<double> RowValues;
            int32 Start;
            if(Header.FindChar('(',Start)&&Header.EndsWith(TEXT(")")))
            {
                Id=Header.Left(Start);FString Row=Header.Mid(Start+1,Header.Len()-Start-2),Values;
                if(!Row.Split(TEXT("="),&RowName,&Values)||!Values.StartsWith(TEXT("["))||!Values.EndsWith(TEXT("]")))return Fail(TEXT("Expected voice name(field=[...])"));
                RowName=Trim(RowName); if(!NameOk(RowName))return Fail(TEXT("Expected a row field name"));
                for(auto& S:Split(Values.Mid(1,Values.Len()-2),',')){double N;if(!Number(S,N))return Fail(TEXT("Expected a numeric row value"));RowValues.Add(N);}
                Count=RowValues.Num();
            }
            else if(Header.FindChar('[',Start)&&Header.EndsWith(TEXT("]")))
            {
                Id=Header.Left(Start);double V;if(!Number(Header.Mid(Start+1,Header.Len()-Start-2),V)||V!=std::floor(V)||V<1||V>64)return Fail(TEXT("Expected a bank size 1–64"));Count=int32(V);
            }
            if(Count<1||Count>64||!NameOk(Id)||BankIds.Contains(Id)||P.Banks.Num()>=128)return Fail(TEXT("Expected a unique voice name and 1–64 slots"));
            int32 B=P.Banks.AddDefaulted();BankIds.Add(Id,B);auto& Bank=P.Banks[B];Bank.Name=FName(Id);
            FList PathsToBuild,Init;
            for(const auto& L:Decl.Children)
            {
                Locate(L);
                if(L.Text.StartsWith(TEXT("use ")))
                {
                    FString Use=Trim(L.Text.Mid(4));int32 Open;
                    if(!Use.FindChar('(',Open)||!Use.EndsWith(TEXT(")"))||!Patches.Contains(Use.Left(Open)))return Fail(TEXT("Expected a declared patch with named arguments"));
                    for(auto& A:Split(Use.Mid(Open+1,Use.Len()-Open-2),','))Init.Add({A,L.Line,L.Column});
                    PathsToBuild.Append(Patches[Use.Left(Open)]->Children);
                }
                else if(L.Text.StartsWith(TEXT("let ")))Init.Add({Trim(L.Text.Mid(4)),L.Line,L.Column+4});
                else PathsToBuild.Add(L);
            }
            if(PathsToBuild.IsEmpty())return Fail(TEXT("Expected a signal path inside the voice"));
            const int32 GlobalCount=GlobalRegisters();
            Bank.bPool=Count>1&&RowName.IsEmpty();
            const auto UsesIndex=[](const FStatement& L)
            {
                for(int32 At=0;At<L.Text.Len();++At)
                {
                    const auto IsName=[](TCHAR C){return FChar::IsAlnum(C)||C=='_';};
                    if(L.Text[At]=='i'&&(At==0||!IsName(L.Text[At-1]))&&(At+1==L.Text.Len()||!IsName(L.Text[At+1])))return true;
                }
                return false;
            };
            Bank.bPool&=!PathsToBuild.ContainsByPredicate(UsesIndex)&&!Init.ContainsByPredicate(UsesIndex);
            for(int32 I=0;I<Count;++I)
            {
                auto GlobalNames=Names;auto GlobalConstants=Constants;
                Constants.Add(TEXT("i"),I);if(!RowName.IsEmpty())Constants.Add(RowName,RowValues[I]);
                auto& V=Bank.Voices.AddDefaulted_GetRef();
                for(const auto& A:Init){Locate(A);if(!Assignment(A.Text,V.Init))return false;}
                const FString Prefix=Count==1?Id+TEXT("."):FString::Printf(TEXT("%s.%d."),*Id,I);
                if(!Paths(PathsToBuild,Prefix,Outlet,V))return false;
                if(Bank.bPool&&I==0)
                {
                    auto Read=[&](const FProgramExpr& E){Bank.LaunchDraws+=E.CaptureReads(Bank.LaunchInputs,GlobalCount);};
                    for(const auto& A:V.Init)Read(A.Expr);
                    for(const auto& Binding:V.Bindings)
                    {
                        if(Binding.Curve.Kind==ERuleCurve::Follow)Read(Binding.Curve.Args[0]);
                        else for(const auto& Arg:Binding.Curve.Args)Read(Arg);
                    }
                    for(const auto& G:V.Sources){Read(G.Life);Read(G.Offset);}
                    if(Bank.LaunchInputs.Num()>MaxLaunchInputs)return Fail(TEXT("Too many live controls captured by one pooled launch"));
                }
                Names=MoveTemp(GlobalNames);Constants=MoveTemp(GlobalConstants);
            }
            return true;
        }
        bool Connect()
        {
            TMap<FPortRef,int32> Orders;
            for(const auto& W:Wires)
            {
                Locate(W.Origin);FString To,FieldName;const bool Mod=W.B.Split(TEXT(":"),&To,&FieldName);if(!Mod)To=W.B;
                for(const auto& Id:{W.A,To})if(!NodeTypes.Contains(Id)){Focus(Id.Mid(Id.Find(TEXT("."),ESearchCase::CaseSensitive,ESearchDir::FromEnd)+1));return Fail(TEXT("Expected an existing node, found ")+Id);}
                const auto& Type=NodeTypes[To];const FPortRef Dst{FName(To),Mod?FName(Field(FieldName,Type)):FName(TEXT("input"))};
                const auto* Spec=FindNodeType({FName(Type),Versions[To]});const auto Kind=Mod?EPortKind::Control:EPortKind::Audio;
                const FName FromPort=Mod?TEXT("control"):TEXT("audio");
                if(FindNodeType({FName(NodeTypes[W.A]),Versions[W.A]})->Output(FromPort)!=Kind || Spec->Input(Dst.Port)!=Kind || (Mod&&Spec->FindControl(Dst.Port)->bConstant))
                    return Fail(TEXT("Expected matching writable ports on ")+W.A+TEXT(" -> ")+W.B);
                int32& Order=Orders.FindOrAdd(Dst);
                if(Order>=(Mod?8:128)||Graph.Connections.Num()>=4096)return Fail(TEXT("Connection fan-in or graph budget exceeded"));
                Graph.Connections.Add({{FName(W.A),FromPort},Dst,Kind,Order++});
            }
            return true;
        }
        bool Actions(const FList& Lines,FRuleList& List)
        {
            for(const auto& Raw:Lines)
            {
                FString S,A,B,At;
                Locate(Raw);S=Raw.Text;if(Raw.Block||S.Len()>2048)return Fail(TEXT("Expected a rule statement"));
                auto& Act=List.Actions.AddDefaulted_GetRef();
                if(S.StartsWith(TEXT("if "))||S.StartsWith(TEXT("repeat "))||S.StartsWith(TEXT("call ")))
                {
                    S.Split(TEXT(" "),&A,&B);FString Target;
                    if(A==TEXT("call")){Act.Kind=ERuleAction::Call;Target=B;}
                    else
                    {
                        int32 Sep=B.Find(TEXT(" then "));if(Sep<0)return Fail(TEXT("Expected condition then routine"));
                        Target=B.Mid(Sep+6);if(!Expr(B.Left(Sep),Act.Expr))return false;
                        Act.Kind=A==TEXT("if")?ERuleAction::Condition:ERuleAction::Repeat;
                    }
                    const auto* I=RoutineIds.Find(Trim(Target));if(!I)return Fail(TEXT("Unknown routine ")+Target);Act.Index=*I;
                }
                else if(S.StartsWith(TEXT("enter ")))
                {
                    const FString Name=Trim(S.Mid(6));
                    if(!StateIds.Contains(Name))return Fail(TEXT("Expected a declared state after enter"));
                    Act.Kind=ERuleAction::Enter;Act.Index=StateIds[Name];
                }
                else if(S.StartsWith(TEXT("play "))||S.StartsWith(TEXT("all "))||S.StartsWith(TEXT("stop ")))
                {
                    S.Split(TEXT(" "),&A,&B);FString Target;
                    if(!B.Split(A==TEXT("stop")?TEXT(" after "):TEXT(" at "),&Target,&At))Target=B;
                    const int32* I=BankIds.Find(Trim(Target));if(!I)return Fail(TEXT("Unknown voice bank ")+Target);
                    Act.Kind=A==TEXT("all")?ERuleAction::All:A==TEXT("stop")?ERuleAction::Stop:ERuleAction::Play;
                    Act.Index=*I;if(!At.IsEmpty()&&!Expr(At,Act.Expr))return false;
                }
                else if(S.Split(TEXT("="),&A,&B))
                {
                    A=Trim(A);FString Node,Port;
                    if(A.Split(TEXT(":"),&Node,&Port))
                    {
                        int32 Open;
                        if (Node.FindChar('[', Open))
                        {
                            const int32 Close=Node.Find(TEXT("]."), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open+1);
                            const FString BankName=Node.Left(Open);
                            const int32* Bank=BankIds.Find(BankName);
                            if (!Bank || Close<=Open+1 || !Expr(Node.Mid(Open+1,Close-Open-1),Act.VoiceIndex))
                                return Fail(TEXT("Expected bank[index].node:port"));
                            const int32 Count=P.Banks[*Bank].Voices.Num();
                            const FString Suffix=Node.Mid(Close+2);
                            for (int32 Slot=0; Slot<Count; ++Slot)
                            {
                                const FString Path=Count==1 ? BankName+TEXT(".")+Suffix : FString::Printf(TEXT("%s.%d.%s"),*BankName,Slot,*Suffix);
                                const FString* SlotType=NodeTypes.Find(Path);
                                if (!SlotType) return Fail(TEXT("Unknown node in indexed voice bank: ")+Path);
                                Act.IndexedPorts.Add({FName(Path),FName(Field(Port,*SlotType))});
                            }
                            Node=BankName+(Count==1?TEXT("."):TEXT(".0."))+Suffix;
                        }
                        const auto* Type=NodeTypes.Find(Node);
                        if(!Type)return Fail(TEXT("Unknown node ")+Node);
                        const auto* Spec=FindNodeType({FName(*Type),Versions[Node]});
                        Act.Write.Port={FName(Node),FName(Field(Port,*Type))};
                        const auto* C=Spec->FindControl(Act.Write.Port.Port);
                        if(!C||C->bConstant)return Fail(TEXT("Unknown writable port ")+A);
                        Act.Kind=ERuleAction::Write;
                        FString Body;
                        if(B.Split(TEXT(" at "),&Body,&At)){if(!Expr(At,Act.Expr))return false;}
                        else Body=B;
                        if(!Curve(Trim(Body),Act.Write.Curve)||Act.Write.Curve.Kind==ERuleCurve::Follow)return Fail(TEXT("Invalid write curve"));
                    }
                    else
                    {
                        if(!Writable.Contains(A))return Fail(TEXT("Unknown writable memory ")+A);
                        Act.Index=Names[A];Act.Kind=ERuleAction::Assign;if(!Expr(B,Act.Expr))return false;
                    }
                }
                else return Fail(TEXT("Unknown rule ")+S);
            }
            return true;
        }
        bool BuildTrack(const FStatement& L,const TMap<FString,FWrittenPhrase>& Phrases)
        {
            Locate(L);
            FString Header=L.Text,Destination;
            int32 Arrow=Header.Find(TEXT("->"));
            if(Arrow!=INDEX_NONE){Destination=Trim(Header.Mid(Arrow+2));Header=Trim(Header.Left(Arrow));}
            TArray<FString> W;Header.ParseIntoArrayWS(W);
            if(W.Num()!=4||W[0]!=TEXT("track")||W[2]!=TEXT("uses")||!NameOk(W[1])||!Patches.Contains(W[3]))
                return Fail(TEXT("Expected track name uses instrumentPatch [-> bus.input]"));
            const auto& Patch=Patches[W[3]]->Children;
            if(Patch.Num()!=1)return Fail(TEXT("An instrument patch needs one note instrument; route effects through the track bus"));
            FStatement Instance{TEXT("voice ")+W[1],L.Line,L.Column,true,Patch};
            if(!Voice(Instance))return false;
            const int32 Bank=BankIds[W[1]];const auto& Created=P.Banks[Bank].Voices[0].Nodes;
            Locate(L);
            if(Created.Num()!=1)return Fail(TEXT("A track instrument patch must contain exactly one instrument"));
            const FString Instrument=Created[0].ToString();
            const auto* Spec=FindNodeType({FName(NodeTypes[Instrument]),Versions[Instrument]});
            if(Spec->EventMode!=EEventMode::Notes&&Spec->EventMode!=EEventMode::NotesWithSustain)
                return Fail(TEXT("Expected a note-driven instrument for this track"));
            auto& T=P.Tracks.AddDefaulted_GetRef();T.Bank=Bank;T.Instrument=Created[0];T.Name=FName(W[1]);
            if(!Destination.IsEmpty())Wires.Add({Instrument,Destination,Origin});
            FDiagnostic D;
            if(!ReadTrackSettings(L,Phrases,Names,Graph.DurationSeconds,T,D))
            {Error=D.Message;FString A,B;D.Pointer.Split(TEXT(":"),&A,&B);Origin.Line=FCString::Atoi(*A);Origin.Column=FCString::Atoi(*B);ErrorOffset=0;return false;}
            if(T.PedalSeconds.Num()&&Spec->EventMode!=EEventMode::NotesWithSustain)return Fail(TEXT("This instrument has no damper pedal"));
            return true;
        }
        bool ReadNotes(const FStatement& L,int32 Ordinal)
        {
            Locate(L);FString Head,Score;
            const bool Music=L.Text.StartsWith(TEXT("score "));
            if(Music&&!L.Text.Split(TEXT(":"),&Head,&Score))return Fail(TEXT("Expected score node at seconds bpm tempo : notes"));
            if(!Music)Head=L.Text;
            TArray<FString> W;Head.ParseIntoArrayWS(W);
            double At=0,Arg=0;
            if(W.Num()!=6||W[2]!=TEXT("at")||!Number(W[3],At)||At<0||At>Graph.DurationSeconds||!Number(W[5],Arg)||!NodeTypes.Contains(W[1]))
                return Fail(TEXT("Expected a declared node, at <seconds> and a named value"));
            const FName Node(W[1]);const auto Mode=FindNodeType({FName(NodeTypes[W[1]]),Versions[W[1]]})->EventMode;
            if(!Music)
            {
                if(Mode!=EEventMode::Hit||W[4]!=TEXT("velocity")||Arg<0||Arg>1)return Fail(TEXT("Expected a one-shot node and velocity 0–1"));
                FEvent E;E.AtSeconds=At;E.To={Node,TEXT("fire")};E.Action=EAction::Fire;E.Payload.Set<FFire>(FFire{float(Arg)});Graph.Timeline.Add(MoveTemp(E));return true;
            }
            if((Mode!=EEventMode::Notes&&Mode!=EEventMode::NotesWithSustain)||W[4]!=TEXT("bpm")||Arg<20||Arg>400)return Fail(TEXT("Expected a note instrument and bpm 20–400"));
            Score=Trim(Score);const auto Parsed=FDreamSFXScoreParser::Parse(Score,8192);
            if(!Parsed.IsValid())
            {
                Focus(Score);Fail(Parsed.Diagnostics[0].Message);ErrorOffset+=Parsed.Diagnostics[0].SourceOffset;return false;
            }
            const double Unit=60/Arg;
            if(At+Parsed.DurationBeats*Unit>Graph.DurationSeconds+1.e-9||Graph.Timeline.Num()+2*Parsed.Notes.Num()>32768)return Fail(TEXT("Score exceeds the preview or note budget"));
            int32 Index=0;
            for(const auto& N:Parsed.Notes)
            {
                const FName Id(*FString::Printf(TEXT("s%d.%d"),Ordinal,++Index));
                FEvent On;On.To={Node,TEXT("noteOn")};On.AtSeconds=At+N.AtBeats*Unit;On.Action=EAction::NoteOn;
                On.Payload.Set<FNoteOn>(FNoteOn{Id,N.Midi,1.f});Graph.Timeline.Add(MoveTemp(On));
                FEvent Off;Off.To={Node,TEXT("noteOff")};Off.AtSeconds=At+(N.AtBeats+N.DurationBeats)*Unit;Off.Action=EAction::NoteOff;
                Off.Payload.Set<FNoteOff>(FNoteOff{Id});Graph.Timeline.Add(MoveTemp(Off));
            }
            return true;
        }
        bool Read(const FList& Tree)
        {
            if(Tree.IsEmpty())return Fail(TEXT("Expected sound <id>"));
            TMap<FString,const FStatement*> Single;
            TArray<const FStatement*> Voices,Inputs,States,Rules,Binds,Notes,Tracks,Buses;
            TMap<FString,FWrittenPhrase> Phrases;
            for(const auto& L:Tree)
            {
                Locate(L);FString Key,Arg;if(!L.Text.Split(TEXT(" "),&Key,&Arg))Key=L.Text;Arg=Trim(Arg);
                bool IsBlock=L.Block;
                if(Key==TEXT("phrase")&&IsBlock)
                {
                    if(!NameOk(Arg)||Phrases.Contains(Arg))return Fail(TEXT("Expected a unique phrase name"));
                    FDiagnostic D;auto Phrase=ReadWrittenPhrase(L.Children,D);
                    if(!Phrase){FString A,B;D.Pointer.Split(TEXT(":"),&A,&B);Origin.Line=FCString::Atoi(*A);Origin.Column=FCString::Atoi(*B);return Fail(D.Message);}
                    Phrases.Add(Arg,Phrase);continue;
                }
                if(Key==TEXT("track")&&IsBlock){Tracks.Add(&L);continue;}
                if(Key==TEXT("bus")&&IsBlock){Buses.Add(&L);continue;}
                if((Key==TEXT("score")||Key==TEXT("fire"))&&!IsBlock){Notes.Add(&L);continue;}
                if(Key==TEXT("bind")&&!IsBlock){Binds.Add(&L);continue;}
                if(Key==TEXT("voice")&&IsBlock){Voices.Add(&L);continue;}
                if(Key==TEXT("input")&&!IsBlock){Inputs.Add(&L);continue;}
                if(Key==TEXT("rule")&&IsBlock&&NameOk(Arg)&&!RoutineIds.Contains(Arg)){Rules.Add(&L);RoutineIds.Add(Arg,P.Routines.AddDefaulted());continue;}
                if(Key==TEXT("state")&&IsBlock&&NameOk(Arg)&&!StateIds.Contains(Arg)){States.Add(&L);int32 I=P.States.AddDefaulted();StateIds.Add(Arg,I);P.States[I].Name=FName(Arg);continue;}
                if(Key==TEXT("patch")&&IsBlock&&NameOk(Arg)&&!Patches.Contains(Arg)){Patches.Add(Arg,&L);continue;}
                const bool IsSection=Key==TEXT("audition")||Key==TEXT("resources")||Key==TEXT("memory")||Key==TEXT("start");
                const bool IsSetting=Key==TEXT("sound")||Key==TEXT("clock")||Key==TEXT("tail")||Key==TEXT("seed")||Key==TEXT("preview")||Key==TEXT("output");
                if((!IsSection&&!IsSetting)||IsSection!=IsBlock||Single.Contains(Key))return Fail(TEXT("Unknown or repeated declaration, expected a sound setting, input, state, rule, patch or voice"));
                Single.Add(Key,&L);
                if(IsSetting)
                {
                    if(Key==TEXT("sound"))
                    {
                        if(Arg.IsEmpty()||Arg.Len()>96)return Fail(TEXT("Expected a sound ID"));
                        for(TCHAR C:Arg)if(!(C>='a'&&C<='z')&&!FChar::IsDigit(C)&&C!='_'&&C!='.'&&C!='-')return Fail(TEXT("Expected a lowercase sound ID"));
                        Graph.Id=FName(Arg);
                    }
                    else if(Key!=TEXT("output"))
                    {
                        FString Unit;
                        if(Key==TEXT("seed"))
                        {
                            TArray<FString> Parts;Arg.ParseIntoArrayWS(Parts);
                            if (Parts.Num()==2 && Parts[1]==TEXT("mulberry32")) P.Algorithm=EProgramRandom::Mulberry32;
                            else if (Parts.Num()!=1 && !(Parts.Num()==2 && Parts[1]==TEXT("lcg32")))
                                return Fail(TEXT("Expected seed <uint32> with optional lcg32 or mulberry32"));
                            Arg=Parts[0];
                        }
                        else
                        {
                            FString N;if(!Arg.Split(TEXT(" "),&N,&Unit)||Unit!=(Key==TEXT("clock")?TEXT("Hz"):TEXT("s")))return Fail(TEXT("Expected a number followed by Hz or s"));Arg=N;
                        }
                        double N=0;if(!Number(Arg,N))return Fail(TEXT("Expected a finite number"));
                        if(Key==TEXT("clock")){if(N!=std::floor(N)||N<1||N>200)return Fail(TEXT("Expected a control rate of 1–200 Hz"));P.TickHz=int32(N);}
                        else if(Key==TEXT("tail")){if(N<0||N>30)return Fail(TEXT("Expected a voice tail in [0,30] s"));Graph.VoiceTailSeconds=N;}
                        else if(Key==TEXT("preview")){if(N<=0||N>120)return Fail(TEXT("Expected a preview duration in (0,120] s"));Graph.DurationSeconds=N;}
                        else{if(N<0||N>MAX_uint32||N!=std::floor(N))return Fail(TEXT("Expected an unsigned 32-bit seed"));Graph.Seed=uint32(N);}
                    }
                }
            }
            Locate(Tree[0]);
            if(Graph.Id.IsNone()||!Tree[0].Text.StartsWith(TEXT("sound "))||Graph.DurationSeconds<=0||!Single.Contains(TEXT("output")))return Fail(TEXT("Expected sound <id> first, a preview duration and an output"));
            int32 Slot;for(const auto* K:{TEXT("time"),TEXT("now"),TEXT("dt")})if(!Register(K,Slot))return false;
            for(const auto* L:Inputs)
            {
                Locate(*L);TArray<FString> Words;L->Text.ParseIntoArrayWS(Words);
                if(Words.Num()!=7||Words[2]!=TEXT("range")||Words[5]!=TEXT("default")||P.Inputs.Num()>=8)return Fail(TEXT("Expected input name range min max default value (up to eight inputs)"));
                const FName InputName(Words[1]);
                if (InputName.IsNone() || InputName == TEXT("State") || InputName == TEXT("Loop"))
                {
                    ErrorCode = TEXT("DSFX211");
                    Focus(Words[1]);
                    return Fail(FString::Printf(TEXT("'%s' is reserved for MetaSound; choose another input name"), *Words[1]));
                }
                auto& In=P.Inputs.AddDefaulted_GetRef();
                if(!Register(Words[1],In.Register)||!Number(Words[3],In.Min)||!Number(Words[4],In.Max)||!Number(Words[6],In.Default)||
                    In.Min>In.Max||!FMath::IsWithinInclusive(In.Default,In.Min,In.Max))return Fail(TEXT("Expected ordered finite limits and a default inside them"));
                In.Name=InputName;Graph.Inputs.Add({In.Name,In.Min,In.Max,In.Default});
            }
            if(auto** S=Single.Find(TEXT("memory")))for(const auto& L:(*S)->Children){Locate(L);if(!Assignment(L.Text,P.Memory))return false;FString K,V;L.Text.Split(TEXT("="),&K,&V);Writable.Add(Trim(K));}
            P.Random=Graph.Seed;if(auto** S=Single.Find(TEXT("resources")))if(!ReadResources(**S,Graph.Seed))return false;
            P.ScoreSeed=Graph.Seed;
            for(auto L:Voices)if(!Voice(*L))return false;
            for(auto L:Buses)
            {
                auto Bus=*L;const auto Name=Trim(Bus.Text.Mid(4));Bus.Text=TEXT("voice ")+Name;
                if(!NameOk(Name)||!Voice(Bus))return false;
                FRuleAction A;A.Kind=ERuleAction::Play;A.Index=BankIds[Name];P.Start.Actions.Add(MoveTemp(A));
            }
            for(auto L:Tracks)if(!BuildTrack(*L,Phrases))return false;
            if(!Connect())return false;
            for(int32 I=0;I<Notes.Num();++I)if(!ReadNotes(*Notes[I],I))return false;
            Graph.Timeline.StableSort([](const auto& A,const auto& B){return A.AtSeconds<B.AtSeconds;});
            for(auto L:Rules){FString Name=Trim(L->Text.Mid(5));if(!Actions(L->Children,P.Routines[RoutineIds[Name]]))return false;}
            // Bound both call depth and worst-case work before any program reaches the audio thread.
            TArray<bool> Visiting;Visiting.Init(false,P.Routines.Num());
            TFunction<bool(const FRuleList&,int32,int64&)> Budget=[&](const FRuleList& L,int32 Depth,int64& Cost)
            {
                if(Depth>16)return Fail(TEXT("Routine nesting limit exceeded"));
                for(const auto& A:L.Actions)
                {
                    ++Cost;
                    if(A.Kind==ERuleAction::Call||A.Kind==ERuleAction::Condition||A.Kind==ERuleAction::Repeat)
                    {
                        if(Visiting[A.Index])return Fail(TEXT("Recursive routine"));
                        Visiting[A.Index]=true;int64 Inner=0;
                        if(!Budget(P.Routines[A.Index],Depth+1,Inner))return false;
                        Visiting[A.Index]=false;
                        int32 Iterations=1;
                        if(A.Kind==ERuleAction::Repeat)
                        {
                            Iterations=MaxRuleRepeat;
                            if(A.Expr.Constant())
                            {
                                uint32 Unused=0;
                                const double Count=A.Expr.Eval({},Unused);
                                if(Count<0||Count>MaxRuleRepeat)return Fail(TEXT("Expected 0–128 repetitions"));
                                Iterations=FMath::CeilToInt(Count);
                            }
                        }
                        Cost+=Inner*Iterations;
                    }
                    if(Cost>8192)return Fail(TEXT("Rule work limit exceeded"));
                }
                return true;
            };
            int64 Cost=0;
            for(const auto& R:P.Routines) {Cost=0;if(!Budget(R,0,Cost))return false;}
            if(auto** S=Single.Find(TEXT("start")))if(!Actions((*S)->Children,P.Start)||!Budget(P.Start,0,Cost))return false;
            if(States.IsEmpty()){P.States.AddDefaulted_GetRef().Name=TEXT("idle");StateIds.Add(TEXT("idle"),0);}
            if(!StateIds.Contains(TEXT("idle")))return Fail(TEXT("Expected an idle state"));
            for(const auto* S:States)
            {
                auto& State=P.States[StateIds[Trim(S->Text.Mid(6))]];
                for(const auto& L:S->Children)
                {
                    Locate(L);FString Hook,Name;
                    if(!L.Text.Split(TEXT(" "),&Hook,&Name)||(Hook!=TEXT("enter")&&Hook!=TEXT("tick"))||!RoutineIds.Contains(Trim(Name)))return Fail(TEXT("Expected enter or tick followed by a declared rule"));
                    auto& List=Hook==TEXT("enter")?State.Enter:State.Tick;
                    FRuleAction A;A.Kind=ERuleAction::Call;A.Index=RoutineIds[Trim(Name)];List.Actions.Add(MoveTemp(A));
                }
            }
            if(auto** S=Single.Find(TEXT("audition")))for(const auto& L:(*S)->Children)
            {
                Locate(L);FString K,V;
                if(L.Text.StartsWith(TEXT("at ")))
                {
                    TArray<FString> W;L.Text.ParseIntoArrayWS(W);double At;
                    if(W.Num()!=3||!Number(W[1],At)||At<0||At>Graph.DurationSeconds||(P.Audition.Num()&&At<=P.Audition.Last().Key)||!StateIds.Contains(W[2]))return Fail(TEXT("Expected at <ascending seconds> <declared state>"));
                    P.Audition.Emplace(At,StateIds[W[2]]);
                }
                else if(L.Text.Split(TEXT("="),&K,&V))
                {
                    auto* In=P.Inputs.FindByPredicate([&](const auto& Val){return Val.Name==FName(Trim(K));});
                    if(!In||In->Preview.Code.Num())return Fail(TEXT("Expected one audition expression per named input"));
                    if(!Expr(Trim(V),In->Preview))return false;
                }
                else return Fail(TEXT("Expected an input assignment or at <seconds> <state>"));
            }
            TSet<FName> Bound;
            for(auto* L:Binds)
            {
                Locate(*L);FString K,V;double N;
                if(!L->Text.Mid(5).Split(TEXT("="),&K,&V)||!Number(Trim(V),N))return Fail(TEXT("Expected bind input = value"));
                const FName Name(Trim(K));const int32 I=P.Inputs.IndexOfByPredicate([&](const auto& A){return Name==A.Name;});
                if(I==INDEX_NONE||Bound.Contains(Name)||!FMath::IsWithinInclusive(N,P.Inputs[I].Min,P.Inputs[I].Max))return Fail(TEXT("Expected an unbound input and a value inside its range"));
                Bound.Add(Name);P.Inputs[I].Default=Graph.Inputs[I].Default=N;P.Inputs[I].Preview.Code.Reset();
            }
            Locate(*Single[TEXT("output")]);const FString Out=Trim(Origin.Text.Mid(7));
            if(!NodeTypes.Contains(Out)||FindNodeType({FName(NodeTypes[Out]),Versions[Out]})->Output(TEXT("audio"))!=EPortKind::Audio)return Fail(TEXT("Expected an audio output node"));
            Graph.Output={FName(Out),TEXT("audio")};return true;
        }
    };
}
bool CompileSoundProgram(const FString& Text,FGraphIR& Graph,TArray<FDiagnostic>& Errors)
{
    ProgramSyntax::FList Tree;FDiagnostic Issue;
    if(!Dsl::Parse(Text,Tree,Issue)){Errors.Add(MoveTemp(Issue));return false;}
    auto Program=MakeShared<FSoundProgram,ESPMode::ThreadSafe>();
    ProgramSyntax::FBuilder Builder{*Program,Graph};
    if(!Builder.Read(Tree))
    {
        auto Diagnostic = Dsl::Expected(Builder.Origin,
            Builder.Error.IsEmpty() ? TEXT("Invalid DSL declaration") : Builder.Error, Builder.ErrorOffset);
        Diagnostic.Code = Builder.ErrorCode;
        Errors.Add(MoveTemp(Diagnostic));
        return false;
    }
    for(const auto& State:Program->States)Graph.StateNames.Add(State.Name);
    Program->Source=Text;Graph.Program=MoveTemp(Program);return true;
}
}
