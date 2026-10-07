/*
** Input Method Manager (Asian-language text entry). The browser has no IMM;
** every query reports "no IME" so WWUi's IME UI stays dormant. Values are
** the documented Win32 ones.
*/
#pragma once
#include "windows.h"

typedef struct tagCANDIDATELIST {
    DWORD dwSize, dwStyle, dwCount, dwSelection, dwPageStart, dwPageSize;
    DWORD dwOffset[1];
} CANDIDATELIST, *LPCANDIDATELIST;

typedef struct tagLOGFONTA {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    CHAR lfFaceName[32];
} LOGFONT, LOGFONTA, *LPLOGFONT, *LPLOGFONTA;

#define GCS_COMPREADSTR 0x0001
#define GCS_COMPREADATTR 0x0002
#define GCS_COMPREADCLAUSE 0x0004
#define GCS_COMPSTR 0x0008
#define GCS_COMPATTR 0x0010
#define GCS_COMPCLAUSE 0x0020
#define GCS_CURSORPOS 0x0080
#define GCS_DELTASTART 0x0100
#define GCS_RESULTREADSTR 0x0200
#define GCS_RESULTREADCLAUSE 0x0400
#define GCS_RESULTSTR 0x0800
#define GCS_RESULTCLAUSE 0x1000
#define CS_INSERTCHAR 0x2000
#define CS_NOMOVECARET 0x4000

#define IMN_CLOSESTATUSWINDOW 0x0001
#define IMN_OPENSTATUSWINDOW 0x0002
#define IMN_CHANGECANDIDATE 0x0003
#define IMN_CLOSECANDIDATE 0x0004
#define IMN_OPENCANDIDATE 0x0005
#define IMN_SETCONVERSIONMODE 0x0006
#define IMN_SETSENTENCEMODE 0x0007
#define IMN_SETOPENSTATUS 0x0008
#define IMN_SETCANDIDATEPOS 0x0009
#define IMN_SETCOMPOSITIONFONT 0x000A
#define IMN_SETCOMPOSITIONWINDOW 0x000B
#define IMN_SETSTATUSWINDOWPOS 0x000C
#define IMN_GUIDELINE 0x000D

#define NI_OPENCANDIDATE 0x0010
#define NI_CLOSECANDIDATE 0x0011
#define NI_SELECTCANDIDATESTR 0x0012
#define NI_CHANGECANDIDATELIST 0x0013
#define NI_SETCANDIDATE_PAGESTART 0x0016
#define NI_SETCANDIDATE_PAGESIZE 0x0017

#define IGP_PROPERTY 0x00000004
#define IME_PROP_AT_CARET 0x00010000
#define IME_PROP_SPECIAL_UI 0x00020000
#define IME_PROP_UNICODE 0x00080000
#define IME_PROP_COMPLETE_ON_UNSELECT 0x00100000
#define IME_PROP_CANDLIST_START_FROM_1 0x00040000
#define ATTR_INPUT 0x00
#define ATTR_TARGET_CONVERTED 0x01
#define ATTR_CONVERTED 0x02
#define ATTR_TARGET_NOTCONVERTED 0x03
#define ATTR_INPUT_ERROR 0x04

#define IME_CMODE_NATIVE 0x0001
#define IME_CMODE_KATAKANA 0x0002
#define IME_CMODE_FULLSHAPE 0x0008
#define IME_CMODE_ROMAN 0x0010
#define IME_CMODE_CHARCODE 0x0020
#define IME_CMODE_HANJACONVERT 0x0040
#define IME_CMODE_SOFTKBD 0x0080
#define IME_CMODE_NOCONVERSION 0x0100
#define IME_CMODE_EUDC 0x0200
#define IME_CMODE_SYMBOL 0x0400
#define IME_CMODE_FIXED 0x0800
#define IME_SMODE_NONE 0x0000
#define IME_SMODE_PLAURALCLAUSE 0x0001
#define IME_SMODE_SINGLECONVERT 0x0002
#define IME_SMODE_AUTOMATIC 0x0004
#define IME_SMODE_PHRASEPREDICT 0x0008
#define IME_SMODE_CONVERSATION 0x0010

#define ISC_SHOWUIALL 0xC000000F
#define GL_LEVEL_NOGUIDELINE 0x0000
#define GGL_LEVEL 1
#define GGL_INDEX 2
#define GGL_STRING 3
#define IME_CAND_UNKNOWN 0x0000
#define IME_CAND_READ 0x0001
#define IME_CAND_CODE 0x0002
#define IME_CAND_MEANING 0x0003
#define IME_CAND_RADICAL 0x0004
#define IME_CAND_STROKE 0x0005
#define CPS_COMPLETE 0x0001
#define CPS_CANCEL 0x0004

#ifdef __cplusplus
extern "C" {
#endif
HIMC ImmCreateContext(void);
BOOL ImmDestroyContext(HIMC imc);
BOOL ImmGetOpenStatus(HIMC imc);
BOOL ImmSetOpenStatus(HIMC imc, BOOL open);
BOOL ImmGetConversionStatus(HIMC imc, LPDWORD conversion, LPDWORD sentence);
LONG ImmGetCompositionStringA(HIMC imc, DWORD index, LPVOID buf, DWORD len);
LONG ImmGetCompositionStringW(HIMC imc, DWORD index, LPVOID buf, DWORD len);
#define ImmGetCompositionString ImmGetCompositionStringA
DWORD ImmGetCandidateListA(HIMC imc, DWORD index, LPCANDIDATELIST list, DWORD len);
DWORD ImmGetCandidateListW(HIMC imc, DWORD index, LPCANDIDATELIST list, DWORD len);
#define ImmGetCandidateList ImmGetCandidateListA
BOOL ImmGetCompositionFontA(HIMC imc, LPLOGFONTA font);
#define ImmGetCompositionFont ImmGetCompositionFontA
UINT ImmGetDescriptionA(HKL kl, LPSTR buf, UINT len);
UINT ImmGetDescriptionW(HKL kl, LPWSTR buf, UINT len);
#define ImmGetDescription ImmGetDescriptionA
DWORD ImmGetGuideLineA(HIMC imc, DWORD index, LPSTR buf, DWORD len);
DWORD ImmGetGuideLineW(HIMC imc, DWORD index, LPWSTR buf, DWORD len);
#define ImmGetGuideLine ImmGetGuideLineA
DWORD ImmGetProperty(HKL kl, DWORD index);
BOOL ImmNotifyIME(HIMC imc, DWORD action, DWORD index, DWORD value);
#ifdef __cplusplus
}
#endif
