// COM runtime with no registered classes, plus BSTR and ATL advise helpers.
#include "windows.h"
#include "atlbase.h"

#include <cstdlib>
#include <cstring>
#include <cwchar>

extern "C" const IID IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
extern "C" const IID IID_IConnectionPointContainer = {0xB196B284, 0xBAB4, 0x101A, {0xB6, 0x9C, 0x00, 0xAA, 0x00, 0x34, 0x1D, 0x07}};

extern "C" HRESULT CoInitialize(LPVOID) { return S_OK; }
extern "C" HRESULT CoInitializeEx(LPVOID, DWORD) { return S_OK; }
extern "C" void CoUninitialize(void) {}
extern "C" HRESULT CoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *out) {
  if (out)
    *out = nullptr;
  return REGDB_E_CLASSNOTREG;
}
extern "C" LPVOID CoTaskMemAlloc(SIZE_T n) { return malloc(n); }
extern "C" void CoTaskMemFree(LPVOID p) { free(p); }
extern "C" void *MIDL_user_allocate(size_t n) { return malloc(n); }
extern "C" void MIDL_user_free(void *p) { free(p); }

// BSTR: length prefix (bytes) immediately before the characters.
extern "C" BSTR SysAllocStringLen(const OLECHAR *s, UINT n) {
  unsigned char *block = (unsigned char *)malloc(sizeof(UINT) + (n + 1) * sizeof(OLECHAR));
  if (!block)
    return nullptr;
  *(UINT *)block = n * (UINT)sizeof(OLECHAR);
  BSTR b = (BSTR)(block + sizeof(UINT));
  if (s)
    memcpy(b, s, n * sizeof(OLECHAR));
  else
    memset(b, 0, n * sizeof(OLECHAR));
  b[n] = 0;
  return b;
}
extern "C" BSTR SysAllocString(const OLECHAR *s) { return s ? SysAllocStringLen(s, (UINT)wcslen(s)) : nullptr; }
extern "C" void SysFreeString(BSTR s) {
  if (s)
    free((unsigned char *)s - sizeof(UINT));
}
extern "C" UINT SysStringLen(BSTR s) { return s ? *(UINT *)((unsigned char *)s - sizeof(UINT)) / sizeof(OLECHAR) : 0; }

HRESULT ATL::AtlAdvise(IUnknown *container, IUnknown *sink, const IID &iid, LPDWORD cookie) {
  if (!container)
    return E_INVALIDARG;
  IConnectionPointContainer *cpc = nullptr;
  HRESULT hr = container->QueryInterface(IID_IConnectionPointContainer, (void **)&cpc);
  if (FAILED(hr))
    return hr;
  IConnectionPoint *cp = nullptr;
  hr = cpc->FindConnectionPoint(iid, &cp);
  cpc->Release();
  if (FAILED(hr))
    return hr;
  hr = cp->Advise(sink, cookie);
  cp->Release();
  return hr;
}

HRESULT ATL::AtlUnadvise(IUnknown *container, const IID &iid, DWORD cookie) {
  if (!container)
    return E_INVALIDARG;
  IConnectionPointContainer *cpc = nullptr;
  HRESULT hr = container->QueryInterface(IID_IConnectionPointContainer, (void **)&cpc);
  if (FAILED(hr))
    return hr;
  IConnectionPoint *cp = nullptr;
  hr = cpc->FindConnectionPoint(iid, &cp);
  cpc->Release();
  if (FAILED(hr))
    return hr;
  hr = cp->Unadvise(cookie);
  cp->Release();
  return hr;
}
