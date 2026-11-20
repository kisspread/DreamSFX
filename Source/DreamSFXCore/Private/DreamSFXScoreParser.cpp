#include "DreamSFXScoreParser.h"

namespace DreamSFXScorePrivate
{
 bool Pitch(const FString& Text, int32& Midi)
 {
  if(Text.Len()<2) return false;
  int32 Semitone=0;
  switch(FChar::ToUpper(Text[0]))
  {
   case 'C': Semitone=0; break; case 'D': Semitone=2; break;
   case 'E': Semitone=4; break; case 'F': Semitone=5; break;
   case 'G': Semitone=7; break; case 'A': Semitone=9; break;
   case 'B': Semitone=11; break; default: return false;
  }
  int32 I=1;
  if(Text[I]=='#'||Text[I]=='b') { Semitone+=Text[I]=='#'?1:-1; ++I; }
  bool Negative=false;
  if(I<Text.Len()&&Text[I]=='-') { Negative=true; ++I; }
  if(I>=Text.Len()) return false;
  int32 Octave=0;
  for(;I<Text.Len();++I)
  {
   if(Text[I]<'0'||Text[I]>'9') return false;
   Octave=Octave*10+Text[I]-'0';
   if(Octave>10) return false;
  }
  if(Negative) Octave=-Octave;
  Midi=(Octave+1)*12+Semitone;
  return Midi>=0&&Midi<=127;
 }
}

namespace
{
    constexpr int32 PulsesPerBeat = 960;

    struct FNotationReader
    {
        const FString& Text;
        int32 Limit;
        FDreamSFXScoreResult Result;
        int32 At = 0;
        int64 Pulse = 0;
        TArray<int32> TiedNotes;

        bool Fail(const TCHAR* Message, int32 Begin)
        {
            FDreamSFXDiagnostic Error;
            Error.Code = TEXT("DSFX2002");
            Error.Message = Message;
            Error.SourceOffset = Begin;
            Error.SourceLength = FMath::Max(1, At - Begin);
            Result.Diagnostics.Add(MoveTemp(Error));
            Result.Notes.Reset();
            return false;
        }
        TCHAR Peek() const { return At < Text.Len() ? Text[At] : 0; }
        bool Eat(TCHAR Ch) { if (Peek() != Ch) return false; ++At; return true; }
        void Space() { while (FChar::IsWhitespace(Peek())) ++At; }
        bool Starts(const TCHAR* Word) const { return FCString::Strncmp(*Text + At, Word, FCString::Strlen(Word)) == 0; }

        bool ReadPitch(int32& Midi)
        {
            const int32 Begin = At;
            if (FChar::IsAlpha(Peek())) ++At;
            if (Peek() == '#' || Peek() == 'b') ++At;
            Eat('-');
            while (FChar::IsDigit(Peek())) ++At;
            return DreamSFXScorePrivate::Pitch(Text.Mid(Begin, At-Begin),Midi) || Fail(TEXT("Expected a pitch such as C4 or Bb3"),Begin);
        }
        bool Symbol(bool Triplet)
        {
            const int32 Begin = At;
            TArray<int32> Pitches, Choices;
            bool Rest = false;
            if (Peek() == 'R' || Peek() == 'r') { ++At; Rest=true; }
            else if (Eat('['))
            {
                Space();
                while (Peek() && Peek()!=']')
                {
                    int32 Midi;
                    if (!ReadPitch(Midi)) return false;
                    if (Pitches.Contains(Midi)) return Fail(TEXT("Repeated chord pitch"),Begin);
                    Pitches.Add(Midi);
                    Space();
                }
                if (Pitches.IsEmpty() || !Eat(']')) return Fail(TEXT("Expected a nonempty chord ending in ]"),Begin);
            }
            else if (Starts(TEXT("pick(")))
            {
                At+=5;
                do { Space(); int32 Midi; if(!ReadPitch(Midi))return false; Choices.Add(Midi); Space(); } while(Eat(','));
                if(Choices.Num()<2||Choices.Num()>32||!Eat(')'))return Fail(TEXT("Expected pick(pitch, pitch, ...) with 2–32 choices"),Begin);
                Pitches.Add(Choices[0]);
            }
            else { int32 Midi; if(!ReadPitch(Midi))return false; Pitches.Add(Midi); }
            if(!Eat('/'))return Fail(TEXT("Expected an explicit note duration such as /8"),At);
            int32 Den = 0;
            while(FChar::IsDigit(Peek())){Den=Den*10+Text[At++]-'0';if(Den>128)return Fail(TEXT("Duration denominator exceeds 128"),Begin);}
            if(Den<1 || (Den&(Den-1)))return Fail(TEXT("Duration denominator must be a power of two"),Begin);
            int32 Length=4*PulsesPerBeat/Den;
            if(Eat('.'))Length+=Length/2;
            if(Triplet)Length=Length*2/3;
            double Accent=1.;
            const bool HasAccent=Eat('@');
            if(HasAccent)
            {
                const int32 Start=At;while(FChar::IsDigit(Peek())||Peek()=='.')++At;
                const FString Number=Text.Mid(Start,At-Start);
                if(!Number.IsNumeric()||!LexTryParseString(Accent,*Number)||Accent<0||Accent>1)return Fail(TEXT("Expected an accent from 0 to 1"),Start);
            }
            const bool Tie=Eat('~');
            if(Peek()&&!FChar::IsWhitespace(Peek())&&Peek()!='|'&&Peek()!=')')return Fail(TEXT("Unexpected note suffix"),At);
            TArray<int32> Current;
            if(!TiedNotes.IsEmpty())
            {
                if(Rest||!Choices.IsEmpty()||HasAccent||Pitches.Num()!=TiedNotes.Num())return Fail(TEXT("Tie needs the same pitches without a new attack"),Begin);
                for(const int32 Index:TiedNotes)
                {
                    auto& Note=Result.Notes[Index];
                    if(!Pitches.Contains(Note.Midi))return Fail(TEXT("Tie continuation changes pitch"),Begin);
                    Note.DurationBeats+=double(Length)/PulsesPerBeat;
                    Note.Length=At-Note.Offset;
                }
                Current=MoveTemp(TiedNotes);
            }
            else
            {
                if(Pitches.Num()>Limit-Result.Notes.Num())return Fail(TEXT("Score exceeds the written note budget"),Begin);
                for(const int32 Midi:Pitches)
                {
                    const int32 Index=Result.Notes.AddDefaulted();auto& Note=Result.Notes[Index];
                    Note.Midi=Midi;Note.AtBeats=double(Pulse)/PulsesPerBeat;Note.DurationBeats=double(Length)/PulsesPerBeat;
                    Note.Offset=Begin;Note.Length=At-Begin;Note.Accent=Accent;Note.Choices=Choices;
                    Current.Add(Index);
                }
            }
            if(Tie&&(Rest||!Choices.IsEmpty()))return Fail(TEXT("Only a definite note or chord can be tied"),Begin);
            if(Tie)TiedNotes=MoveTemp(Current);else TiedNotes.Reset();
            Pulse+=Length;return true;
        }
        bool Read()
        {
            if(Text.Len()>1024*1024||Limit<0)return Fail(TEXT("Score exceeds its size budget"),0);
            while(true)
            {
                Space();if(Eat('|'))continue;
                if(!Peek())break;
                if(Starts(TEXT("3:2(")))
                {
                    const int32 Begin=At;At+=4;
                    for(int32 I=0;I<3;++I){Space();if(!Symbol(true))return false;}
                    Space();if(!Eat(')'))return Fail(TEXT("Expected exactly three symbols in a 3:2 group"),Begin);
                }
                else if(!Symbol(false))return false;
            }
            if(!TiedNotes.IsEmpty())return Fail(TEXT("Missing continuation after ~"),At);
            Result.DurationBeats=double(Pulse)/PulsesPerBeat;return true;
        }
    };
}
FDreamSFXScoreResult FDreamSFXScoreParser::Parse(const FString& Score,int32 MaxNotes)
{
    FNotationReader Reader{Score,MaxNotes};
    Reader.Read();
    return MoveTemp(Reader.Result);
}
