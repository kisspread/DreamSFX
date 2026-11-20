#pragma once

#include "CoreMinimal.h"

/**
 * Compile diagnostics for DreamSFX sources.
 *
 * Every error carries a stable code, a JSON pointer or score character range so
 * an author (human or AI) can locate the problem without reading the parser.
 */
enum class EDreamSFXDiagnosticSeverity : uint8
{
	Error,
	Warning
};

struct FDreamSFXDiagnostic
{
	FString Code;
	EDreamSFXDiagnosticSeverity Severity = EDreamSFXDiagnosticSeverity::Error;
	FString Message;
	/** JSON pointer such as "/tracks/0/score"; empty when not applicable. */
	FString Pointer;
	/** Character offset in the decoded score string, not raw JSON byte offsets. */
	int32 SourceOffset = INDEX_NONE;
	int32 SourceLength = 0;

	FString ToDisplayString() const
	{
		const TCHAR* SeverityText = Severity == EDreamSFXDiagnosticSeverity::Error ? TEXT("error") : TEXT("warning");
		if (!Pointer.IsEmpty())
		{
			return FString::Printf(TEXT("%s %s at %s: %s"), SeverityText, *Code, *Pointer, *Message);
		}
		if (SourceOffset != INDEX_NONE)
		{
			return FString::Printf(TEXT("%s %s at offset %d: %s"), SeverityText, *Code, SourceOffset, *Message);
		}
		return FString::Printf(TEXT("%s %s: %s"), SeverityText, *Code, *Message);
	}
};

/**
 * Stable diagnostic codes. Keep these append-only; renaming breaks existing
 * tooling and agent prompts.
 */
namespace DreamSFXDiagnostics
{
	inline const TCHAR* InvalidJson = TEXT("DSFX1001");
	inline const TCHAR* MissingField = TEXT("DSFX1002");
	inline const TCHAR* UnknownField = TEXT("DSFX1003");
	inline const TCHAR* WrongType = TEXT("DSFX1004");
	inline const TCHAR* UnsupportedSchema = TEXT("DSFX1005");
	inline const TCHAR* UnknownKind = TEXT("DSFX1006");
	inline const TCHAR* DuplicateId = TEXT("DSFX1007");

	inline const TCHAR* InvalidNoteName = TEXT("DSFX2001");
	inline const TCHAR* InvalidDuration = TEXT("DSFX2002");
	inline const TCHAR* InvalidMeasure = TEXT("DSFX2003");
	inline const TCHAR* UnbalancedChord = TEXT("DSFX2004");
	inline const TCHAR* NegativeTime = TEXT("DSFX2005");
	inline const TCHAR* MissingTempo = TEXT("DSFX2006");
	inline const TCHAR* InvalidTempo = TEXT("DSFX2007");
	inline const TCHAR* ScoreForSecondsTimebase = TEXT("DSFX2008");
	inline const TCHAR* MissingTime = TEXT("DSFX2009");
	inline const TCHAR* NoteOutOfRange = TEXT("DSFX2010");

	inline const TCHAR* UnknownReference = TEXT("DSFX3001");
	inline const TCHAR* MissingInstrument = TEXT("DSFX3002");
	inline const TCHAR* ScoreAndEventsExclusive = TEXT("DSFX3003");
	inline const TCHAR* UnsupportedControl = TEXT("DSFX3004");

	inline const TCHAR* BudgetExceeded = TEXT("DSFX4001");
	inline const TCHAR* InvalidNumber = TEXT("DSFX4002");
	inline const TCHAR* PathEscape = TEXT("DSFX4003");
}
