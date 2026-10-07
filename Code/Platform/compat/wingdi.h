/*
** GDI subset: fonts, 24-bit DIB sections and per-glyph text output, enough
** for WW3D2's Render2DSentence glyph cache. Implemented with stb_truetype
** over the TrueType files shipped with the game (Platform/gfx/gdi_text.cpp).
*/
#pragma once
#include "windows.h"

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *LPBITMAPINFOHEADER, *PBITMAPINFOHEADER;
typedef struct tagRGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO, *LPBITMAPINFO;
#define BI_RGB 0L
#define DIB_RGB_COLORS 0

typedef struct tagTEXTMETRICA {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    LONG tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang;
    LONG tmDigitizedAspectX, tmDigitizedAspectY;
    BYTE tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} TEXTMETRIC, TEXTMETRICA, *LPTEXTMETRIC;

#define LOGPIXELSX 88
#define LOGPIXELSY 90
#define FW_DONTCARE 0
#define FW_NORMAL 400
#define FW_BOLD 700
#define ANSI_CHARSET 0
#define DEFAULT_CHARSET 1
#define SHIFTJIS_CHARSET 128
#define HANGUL_CHARSET 129
#define GB2312_CHARSET 134
#define CHINESEBIG5_CHARSET 136
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define ANTIALIASED_QUALITY 4
#define NONANTIALIASED_QUALITY 3
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
#define FF_DONTCARE (0 << 4)
#define FF_SWISS (2 << 4)
#define ETO_OPAQUE 0x0002
#define ETO_CLIPPED 0x0004
#define TRANSPARENT 1
#define OPAQUE 2
#define SWP_NOCOPYBITS 0x0100

#ifdef __cplusplus
extern "C" {
#endif
HFONT CreateFontA(int height, int width, int escapement, int orientation, int weight, DWORD italic, DWORD underline,
                  DWORD strikeout, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality,
                  DWORD pitch_and_family, LPCSTR face);
#define CreateFont CreateFontA
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *info, UINT usage, void **bits, HANDLE section, DWORD offset);
HDC CreateCompatibleDC(HDC dc);
BOOL DeleteDC(HDC dc);
COLORREF SetTextColor(HDC dc, COLORREF color);
COLORREF SetBkColor(HDC dc, COLORREF color);
int SetBkMode(HDC dc, int mode);
BOOL GetTextMetricsA(HDC dc, LPTEXTMETRIC tm);
#define GetTextMetrics GetTextMetricsA
BOOL GetTextExtentPoint32W(HDC dc, LPCWSTR text, int count, LPSIZE size);
BOOL ExtTextOutW(HDC dc, int x, int y, UINT options, const RECT *rect, LPCWSTR text, UINT count, const INT *dx);
int MulDiv(int number, int numerator, int denominator);
HWND GetDesktopWindow(void);
BOOL SetDeviceGammaRamp(HDC dc, LPVOID ramp);
BOOL GetDeviceGammaRamp(HDC dc, LPVOID ramp);
int AddFontResourceA(LPCSTR file);
#define AddFontResource AddFontResourceA
BOOL RemoveFontResourceA(LPCSTR file);
#define RemoveFontResource RemoveFontResourceA
#ifdef __cplusplus
}
#endif
