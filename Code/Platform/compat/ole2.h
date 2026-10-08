#pragma once
#include "ocidl.h"
/* COM runtime: there are no registered servers in the browser, so every
** CoCreateInstance fails with REGDB_E_CLASSNOTREG and callers take their
** "service unavailable" paths. */
#define CLSCTX_INPROC_SERVER 0x1
#define CLSCTX_INPROC_HANDLER 0x2
#define CLSCTX_LOCAL_SERVER 0x4
#define CLSCTX_ALL (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER)
#define REGDB_E_CLASSNOTREG ((HRESULT)0x80040154L)
#define CO_E_NOTINITIALIZED ((HRESULT)0x800401F0L)
#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED 0x0
#define SEVERITY_SUCCESS 0
#define SEVERITY_ERROR 1
#define FACILITY_ITF 4
#define FACILITY_WIN32 7
#ifdef __cplusplus
extern "C" {
#endif
HRESULT CoInitialize(LPVOID reserved);
HRESULT CoInitializeEx(LPVOID reserved, DWORD flags);
void CoUninitialize(void);
HRESULT CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID *out);
LPVOID CoTaskMemAlloc(SIZE_T n);
void CoTaskMemFree(LPVOID p);
#ifdef __cplusplus
}
#endif
