/*
** Rebel portable prelude: force-included into every translation unit when
** REBEL_PORTABLE is set. Supplies the MSVC CRT names the 2002 codebase uses.
** Target is wasm32 (ILP32), so int/long/pointer sizes match Win32.
*/
#pragma once

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <cctype>
#include <cstdarg>
#include <new>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <stdarg.h>
#endif
#include <strings.h>
#include <unistd.h>
#include <limits.h>

#ifndef _MAX_PATH
#define _MAX_PATH 260
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256

#define __cdecl
#define __stdcall
#define __fastcall
#define _cdecl
#define _stdcall
#define __forceinline inline __attribute__((always_inline))
#define __debugbreak() Rebel_Debug_Break()
#define __declspec(x)

/* SAL annotations are documentation-only. */
#define _In_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#define _Inout_opt_
#define _In_z_
#define _Printf_format_string_

/* __int8/16/32/64 are builtin keywords under -fms-extensions */

#define stricmp strcasecmp
#define _stricmp strcasecmp
#define strcmpi strcasecmp
#define _strcmpi strcasecmp
#define strnicmp strncasecmp
#define _strnicmp strncasecmp
#define _vsnprintf vsnprintf
#define _snprintf snprintf
#define _vsnwprintf vswprintf
#define _snwprintf swprintf
#define _strdup strdup
#define _wcsdup wcsdup
#define _wcsicmp wcscasecmp
#define wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define wcsnicmp wcsncasecmp
#define _access access
#define _unlink unlink
#define _getcwd getcwd
#define _chdir chdir
#define _isnan isnan
#define _finite isfinite
#define _hypot hypot
#define _alloca __builtin_alloca
#define _close close
#define _read read
#define _write write
#define _lseek lseek
#define _fileno fileno

#ifdef __cplusplus
extern "C" {
#endif
char *strupr(char *s);
char *strlwr(char *s);
wchar_t *wcsupr(wchar_t *s);
wchar_t *wcslwr(wchar_t *s);
char *strrev(char *s);
char *itoa(int value, char *buf, int radix);
char *ltoa(long value, char *buf, int radix);
char *ultoa(unsigned long value, char *buf, int radix);
char *_i64toa(long long value, char *buf, int radix);
void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext);
void _makepath(char *path, const char *drive, const char *dir, const char *fname, const char *ext);
char *_fullpath(char *abs, const char *rel, size_t max);
/* Debugger breaks log a stack trace and continue ("Ignore") instead of
** trapping: a wasm trap would end the program on every assertion. */
void Rebel_Debug_Break(void);
int _wtoi(const wchar_t *s);
long _wtol(const wchar_t *s);
/* Multibyte helpers: the ANSI code page here is single-byte (1252). */
unsigned char *_mbsinc(const unsigned char *s);
size_t _mbslen(const unsigned char *s);
size_t _mbsnccnt(const unsigned char *s, size_t n);
#ifdef __cplusplus
}
#endif
#define _strupr strupr
#define _strlwr strlwr
#define _wcsupr wcsupr
#define _wcslwr wcslwr
#define _strrev strrev
#define _itoa itoa
#define _ltoa ltoa
#define _ultoa ultoa
