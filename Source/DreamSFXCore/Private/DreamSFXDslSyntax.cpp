#include "DreamSFXDslSyntax.h"
namespace DreamSFX::V2::Dsl
{
    FDiagnostic Expected(const FStatement& At,const FString& Message,int32 Offset)
    {
        return {TEXT("DSFX210"),FString::Printf(TEXT("%d:%d"),At.Line,At.Column+Offset),Message};
    }
    namespace
    {
        enum class EToken { Statement, Open, Close };
        struct FToken { EToken Kind;FStatement Source; };
        bool Lex(const FString& Text,TArray<FToken>& Tokens,FDiagnostic& Error)
        {
            int32 Line=1,Start=0,Origin=0,StartLine=1,StartColumn=1;
            TArray<TCHAR,TInlineAllocator<32>> Brackets;
            auto Emit=[&](int32 End)
            {
                while(Start<End && FChar::IsWhitespace(Text[Start])) {++Start;++StartColumn;}
                FString S=Text.Mid(Start,End-Start).TrimEnd();
                if(!S.IsEmpty())Tokens.Add({EToken::Statement,{MoveTemp(S),StartLine,StartColumn}});
            };
            for(int32 I=0;I<Text.Len();++I)
            {
                const TCHAR C=Text[I];
                if(C=='('||C=='[')
                {
                    if(Brackets.Num()>64){Error=Expected({{},Line,I-Origin+1},TEXT("Expected fewer than 65 enclosing delimiters"));return false;}
                    Brackets.Push(C=='('?')':']');
                }
                else if(C==')'||C==']')
                {
                    if(!Brackets.Num()||Brackets.Pop()!=C){Error=Expected({{},Line,I-Origin+1},TEXT("Unexpected closing delimiter"));return false;}
                }
                const bool Comment=C=='#'&&(I==0||FChar::IsWhitespace(Text[I-1]));
                if(Comment||C=='\n'||C==';'||C=='{'||C=='}')
                {
                    if(Brackets.Num())
                    {
                        Error=Expected({{},Line,I-Origin+1},FString::Printf(TEXT("Expected '%c' before the end of this expression"),Brackets.Last()));return false;
                    }
                    Emit(I);
                    if(Comment){while(I<Text.Len()&&Text[I]!='\n')++I;}
                    if(C=='{'||C=='}')Tokens.Add({C=='{'?EToken::Open:EToken::Close,{{},Line,I-Origin+1}});
                    if(I<Text.Len()&&Text[I]=='\n'){++Line;Origin=I+1;}
                    Start=I+1;StartColumn=Start-Origin+1;StartLine=Line;
                }
            }
            if(Brackets.Num()){Error=Expected({{},Line,Text.Len()-Origin+1},TEXT("Expected closing delimiter"));return false;}
            if(Start<Text.Len())Emit(Text.Len());
            return true;
        }
    }
    bool Parse(const FString& Text,TArray<FStatement>& Tree,FDiagnostic& Error)
    {
        TArray<FToken> Tokens;
        if(!Lex(Text,Tokens,Error))return false;
        TArray<TArray<FStatement>*> Stack{&Tree};
        EToken Previous=EToken::Close;
        for(auto& T:Tokens)
        {
            if(T.Kind==EToken::Statement)Stack.Last()->Add(MoveTemp(T.Source));
            else if(T.Kind==EToken::Open)
            {
                if(Previous!=EToken::Statement||Stack.Num()>8){Error=Expected(T.Source,TEXT("Expected a declaration before '{'"));return false;}
                auto& Parent=Stack.Last()->Last();Parent.Block=true;Stack.Add(&Parent.Children);
            }
            else
            {
                if(Stack.Num()==1){Error=Expected(T.Source,TEXT("Unexpected '}'"));return false;}
                Stack.Pop();
            }
            Previous=T.Kind;
        }
        if(Stack.Num()!=1){Error=Expected(Tree.Last(),TEXT("Expected '}' to close this declaration"));return false;}
        return true;
    }
}
