// IMM (no IME in the browser) and the locale helpers WWUi uses.
#include "windows.h"

#include <cwctype>

extern "C" HIMC ImmCreateContext(void) { return nullptr; }
extern "C" BOOL ImmDestroyContext(HIMC) { return TRUE; }
extern "C" BOOL ImmGetOpenStatus(HIMC) { return FALSE; }
extern "C" BOOL ImmSetOpenStatus(HIMC, BOOL) { return FALSE; }
extern "C" BOOL ImmGetConversionStatus(HIMC, LPDWORD c, LPDWORD s) {
  if (c)
    *c = 0;
  if (s)
    *s = 0;
  return FALSE;
}
extern "C" LONG ImmGetCompositionStringA(HIMC, DWORD, LPVOID, DWORD) { return 0; }
extern "C" LONG ImmGetCompositionStringW(HIMC, DWORD, LPVOID, DWORD) { return 0; }
extern "C" DWORD ImmGetCandidateListA(HIMC, DWORD, LPCANDIDATELIST, DWORD) { return 0; }
extern "C" DWORD ImmGetCandidateListW(HIMC, DWORD, LPCANDIDATELIST, DWORD) { return 0; }
extern "C" BOOL ImmGetCompositionFontA(HIMC, LPLOGFONTA) { return FALSE; }
extern "C" UINT ImmGetDescriptionA(HKL, LPSTR buf, UINT len) {
  if (buf && len)
    buf[0] = 0;
  return 0;
}
extern "C" UINT ImmGetDescriptionW(HKL, LPWSTR buf, UINT len) {
  if (buf && len)
    buf[0] = 0;
  return 0;
}
extern "C" DWORD ImmGetGuideLineA(HIMC, DWORD, LPSTR, DWORD) { return 0; }
extern "C" DWORD ImmGetGuideLineW(HIMC, DWORD, LPWSTR, DWORD) { return 0; }
extern "C" DWORD ImmGetProperty(HKL, DWORD) { return 0; }
extern "C" BOOL ImmNotifyIME(HIMC, DWORD, DWORD, DWORD) { return FALSE; }

extern "C" int CompareStringW(LCID, DWORD flags, LPCWSTR a, int alen, LPCWSTR b, int blen) {
  if (alen < 0)
    alen = (int)wcslen(a);
  if (blen < 0)
    blen = (int)wcslen(b);
  int n = alen < blen ? alen : blen;
  for (int i = 0; i < n; i++) {
    wchar_t x = a[i], y = b[i];
    if (flags & NORM_IGNORECASE) {
      x = (wchar_t)towlower(x);
      y = (wchar_t)towlower(y);
    }
    if (x != y)
      return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
  }
  return alen == blen ? CSTR_EQUAL : (alen < blen ? CSTR_LESS_THAN : CSTR_GREATER_THAN);
}

extern "C" int GetLocaleInfoA(LCID, DWORD type, LPSTR buf, int len) {
  const char *v = type == LOCALE_IDEFAULTANSICODEPAGE ? "1252" : "";
  int need = (int)strlen(v) + 1;
  if (!buf || len == 0)
    return need;
  snprintf(buf, len, "%s", v);
  return need;
}
