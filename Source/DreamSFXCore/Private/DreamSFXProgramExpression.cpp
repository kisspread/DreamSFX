#include "DreamSFXSoundProgram.h"
#include <cmath>

namespace DreamSFX::V2
{
    namespace
    {
        enum EOp : uint8 { Literal, Variable, Neg, Add, Sub, Mul, Div, Power, Less, Greater, Equal, LessEq, GreaterEq, And,
            Min, Max, Clamp, Sin, Sqrt, Wrap, Choose, Rand, Pick, Floor };
        struct FSymbol { const TCHAR* Text; EOp Op; int32 Rank; };
        constexpr FSymbol Binary[] = {{TEXT("&&"),And,1},{TEXT("=="),Equal,2},{TEXT("<="),LessEq,3},{TEXT(">="),GreaterEq,3},
            {TEXT("<"),Less,3},{TEXT(">"),Greater,3},{TEXT("+"),Add,4},{TEXT("-"),Sub,4},{TEXT("*"),Mul,5},{TEXT("/"),Div,5},{TEXT("^"),Power,7}};
        constexpr FSymbol Functions[] = {{TEXT("min"),Min,2},{TEXT("max"),Max,2},{TEXT("clamp"),Clamp,3},{TEXT("if"),Choose,3},
            {TEXT("sin"),Sin,1},{TEXT("sqrt"),Sqrt,1},{TEXT("floor"),Floor,1},{TEXT("wrap"),Wrap,2},{TEXT("rand"),Rand,2},{TEXT("pow"),Power,2}};
        struct FParser
        {
            const TCHAR* At;
            const TMap<FString,int32>& Names;
            const TMap<FString,double>& Constants;
            TArray<FProgramExpr::FOp>& Code;
            FString& Error;
            const TCHAR* Fault=nullptr;
            void Space() { while (FChar::IsWhitespace(*At)) ++At; }
            bool Take(TCHAR Ch) { Space(); if (*At != Ch) return false; ++At; return true; }
            bool Fail(const FString& Why) { Error = Why; return false; }
            bool Expression(int32 Rank, int32 Depth)
            {
                if (Depth > 36 || Code.Num() > 192) return Fail(TEXT("Expression exceeds its nesting or instruction limit"));
                Space();
                if (Take('-')) { if (!Expression(6, Depth+1)) return false; Code.Add({Neg,0}); }
                else if (Take('(')) { if (!Expression(1,Depth+1) || !Take(')')) return Fail(TEXT("Missing closing parenthesis")); }
                else if (FChar::IsDigit(*At) || *At == '.')
                {
                    const TCHAR* Begin=At;
                    bool Digits=false;
                    while(FChar::IsDigit(*At)){++At;Digits=true;}
                    if(*At=='.'){++At;while(FChar::IsDigit(*At)){++At;Digits=true;}}
                    if(!Digits)return Fail(TEXT("Expected digits in numeric literal"));
                    if(*At=='e'||*At=='E')
                    {
                        ++At;if(*At=='+'||*At=='-')++At;
                        if(!FChar::IsDigit(*At))return Fail(TEXT("Missing exponent digits"));
                        while(FChar::IsDigit(*At))++At;
                    }
                    const double V=FCString::Atod(*FString(At-Begin,Begin));
                    if(!FMath::IsFinite(V))return Fail(TEXT("Invalid numeric literal"));
                    Code.Add({Literal,V});
                }
                else
                {
                    const TCHAR* Begin = At;
                    while (FChar::IsAlnum(*At) || *At == '_') ++At;
                    const FString Name(At-Begin,Begin);
                    if (Name.IsEmpty()) return Fail(TEXT("Expected a number or name"));
                    if (Take('('))
                    {
                        const FSymbol* Fn = nullptr;
                        for (const auto& S : Functions) if (Name == S.Text) { Fn=&S; break; }
                        if(Name==TEXT("pick"))
                        {
                            int32 Count=0;
                            do {if(++Count>32||!Expression(1,Depth+1))return Fail(TEXT("Expected up to 31 lookup values"));}while(Take(','));
                            if(Count<2||!Take(')'))return Fail(TEXT("Expected pick(index, value, ...)"));
                            Code.Add({Pick,double(Count)});
                        }
                        else
                        {
                        if (!Fn) {Fault=Begin;return Fail(TEXT("Expected a supported function, found ")+Name);}
                        for (int32 I=0; I<Fn->Rank; ++I)
                            {if(I&&!Take(','))return Fail(TEXT("Expected a comma in ")+Name);if(!Expression(1,Depth+1))return false;}
                        if (!Take(')')) return Fail(TEXT("Invalid argument count for ")+Name);
                        Code.Add({Fn->Op,0});
                        }
                    }
                    else if (const double* V=Constants.Find(Name)) Code.Add({Literal,*V});
                    else if (const int32* Register=Names.Find(Name)) Code.Add({Variable,double(*Register)});
                    else {Fault=Begin;return Fail(TEXT("Expected a declared input or register, found ")+Name);}
                }
                while (true)
                {
                    Space();
                    const FSymbol* Found=nullptr;
                    for (const auto& S:Binary)
                        if (FCString::Strncmp(At,S.Text,FCString::Strlen(S.Text))==0) { Found=&S; break; }
                    if (!Found || Found->Rank < Rank) break;
                    At += FCString::Strlen(Found->Text);
                    if (!Expression(Found->Rank+(Found->Op==Power?0:1),Depth+1)) return false;
                    Code.Add({Found->Op,0});
                }
                return true;
            }
        };
    }
    bool FProgramExpr::Parse(const FString& Text,const TMap<FString,int32>& Names,const TMap<FString,double>& Constants,FString& Error,int32* Offset)
    {
        FParser Parser{*Text,Names,Constants,Code,Error};
        if (!Parser.Expression(1,0)) {if(Offset)*Offset=int32((Parser.Fault?Parser.Fault:Parser.At)-*Text);return false;}
        Parser.Space();
        if (*Parser.At) { if(Offset)*Offset=int32(Parser.At-*Text);Error=TEXT("Expected an operator before ")+FString(Parser.At); return false; }
        return true;
    }
    bool FProgramExpr::Constant() const
    {
        return !Code.ContainsByPredicate([](const FOp& I){return I.Op==Variable || I.Op==Rand;});
    }
    int32 FProgramExpr::CaptureReads(TArray<int32>& Indices,int32 GlobalCount) const
    {
        int32 Draws=0;
        for(const auto& I:Code)
        {
            if(I.Op==Variable&&I.Operand<GlobalCount)Indices.AddUnique(int32(I.Operand));
            Draws+=I.Op==Rand;
        }
        return Draws;
    }
    uint32 AdvanceProgramRandom(uint32 Seed,uint32 Count,EProgramRandom Algorithm)
    {
        if (Algorithm == EProgramRandom::Mulberry32) return Seed + 0x6D2B79F5u * Count;
        uint32 Multiplier=1664525u,Increment=1013904223u;
        while(Count)
        {
            if(Count&1)Seed=Multiplier*Seed+Increment;
            Increment*=Multiplier+1;Multiplier*=Multiplier;Count>>=1;
        }
        return Seed;
    }
    double FProgramExpr::Eval(TConstArrayView<double> Values,uint32& State,bool* Valid,EProgramRandom Algorithm) const
    {
        bool Ok=true;
        double Stack[256];
        int32 N=0;
        for (const auto& I:Code)
        {
            if (I.Op==Literal || I.Op==Variable) { Stack[N++]=I.Op==Literal?I.Operand:Values[int32(I.Operand)]; continue; }
            if(I.Op==Pick)
            {
                const int32 First=N-int32(I.Operand);const double Index=Stack[First];
                if(Index<0||Index>=I.Operand-1||Index!=std::floor(Index)){Ok=false;Stack[First]=0;}
                else Stack[First]=Stack[First+1+int32(Index)];
                N=First+1;continue;
            }
            double& A=Stack[N-1];
            switch (I.Op)
            {
            case Neg: A=-A; continue;
            case Sin: A=std::sin(A); continue;
            case Floor: A=std::floor(A); continue;
            case Sqrt: Ok&=A>=0.;A=std::sqrt(FMath::Max(A,0.)); continue;
            default: break;
            }
            const double Right=Stack[--N]; double& Left=Stack[N-1];
            switch (I.Op)
            {
            case Add: Left+=Right; break; case Sub: Left-=Right; break;
            case Mul: Left*=Right; break; case Div: Ok&=Right!=0.;Left=Right!=0?Left/Right:0; break;
            case Power: Left=std::pow(Left,Right); break;
            case Greater: Left=Left>Right; break; case Less: Left=Left<Right; break;
            case Equal: Left=Left==Right; break; case GreaterEq: Left=Left>=Right; break; case LessEq: Left=Left<=Right; break;
            case And: Left=Left!=0 && Right!=0; break;
            case Min: Left=FMath::Min(Left,Right); break; case Max: Left=FMath::Max(Left,Right); break;
            case Wrap: Ok&=Right>0;Left=Right>0?std::fmod(std::fmod(Left,Right)+Right,Right):0; break;
            case Rand: Ok&=Right>=Left;Left+=(Right-Left)*(double(DrawRandomBits(State,Algorithm))/4294967296.); break;
            case Clamp: {const double Lo=Stack[--N]; Ok&=Lo<=Right;Stack[N-1]=FMath::Clamp(Stack[N-1],Lo,Right);break;}
            case Choose:{const double Yes=Stack[--N]; Stack[N-1]=Stack[N-1]!=0?Yes:Right;break;}
            default: checkNoEntry(); break;
            }
        }
        Ok=Ok&&N==1&&FMath::IsFinite(Stack[0]);if(Valid)*Valid=Ok;
        return Ok?Stack[0]:0.;
    }
}
