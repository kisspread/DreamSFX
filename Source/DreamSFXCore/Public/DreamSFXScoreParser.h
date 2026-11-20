#pragma once
#include "CoreMinimal.h"
#include "DreamSFXDiagnostics.h"

struct FDreamSFXScoreNote
{
 int32 Midi = 60;
 double AtBeats = 0, DurationBeats = 0;
 double Accent = 1.;
 TArray<int32> Choices; // A pitch is drawn at each attack of a pick(...) symbol.
 int32 Offset = 0, Length = 0;
};
struct FDreamSFXScoreResult
{
 TArray<FDreamSFXScoreNote> Notes;
 TArray<FDreamSFXDiagnostic> Diagnostics;
 double DurationBeats = 0; // includes trailing rests; never inferred from gates
 bool IsValid() const { return Diagnostics.IsEmpty(); }
};

/** Written notes, rests and chords with explicit duration, ties and 3:2 tuplets.
 * An integer beat clock preserves tuplet endpoints across bar lines.
 */
class DREAMSFXCORE_API FDreamSFXScoreParser
{
public:
 static FDreamSFXScoreResult Parse(const FString& Score, int32 MaxNotes = 65536);
};
