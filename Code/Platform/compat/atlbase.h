#pragma once
/* The slice of ATL the WWOnline layer uses: CComPtr and connection-point
** advise helpers. */
#include "ole2.h"

#ifdef __cplusplus
#define ATLAPI HRESULT
#define ATLAPI_(x) x

extern "C" const IID IID_IUnknown;
extern "C" const IID IID_IConnectionPointContainer;

template <class T> class CComPtr {
public:
  T *p;
  CComPtr() : p(nullptr) {}
  CComPtr(T *lp) : p(lp) {
    if (p)
      p->AddRef();
  }
  CComPtr(const CComPtr &o) : p(o.p) {
    if (p)
      p->AddRef();
  }
  ~CComPtr() {
    if (p)
      p->Release();
  }
  void Release() {
    T *t = p;
    if (t) {
      p = nullptr;
      t->Release();
    }
  }
  operator T *() const { return p; }
  T &operator*() const { return *p; }
  T **operator&() { return &p; }
  T *operator->() const { return p; }
  bool operator!() const { return p == nullptr; }
  bool operator==(T *o) const { return p == o; }
  bool operator!=(T *o) const { return p != o; }
  T *operator=(T *lp) {
    if (lp)
      lp->AddRef();
    if (p)
      p->Release();
    p = lp;
    return p;
  }
  T *operator=(const CComPtr &o) { return *this = o.p; }
  void Attach(T *p2) {
    if (p)
      p->Release();
    p = p2;
  }
  T *Detach() {
    T *t = p;
    p = nullptr;
    return t;
  }
  HRESULT CopyTo(T **out) {
    if (!out)
      return E_POINTER;
    *out = p;
    if (p)
      p->AddRef();
    return S_OK;
  }
  HRESULT CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer = nullptr, DWORD ctx = CLSCTX_ALL);
  HRESULT Advise(IUnknown *sink, const IID &iid, LPDWORD cookie);
};

template <class T> HRESULT CComPtr<T>::CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx) {
  return ::CoCreateInstance(clsid, outer, ctx, IID_IUnknown, (void **)&p);
}

namespace ATL {
ATLAPI AtlAdvise(IUnknown *container, IUnknown *sink, const IID &iid, LPDWORD cookie);
ATLAPI AtlUnadvise(IUnknown *container, const IID &iid, DWORD cookie);
} // namespace ATL
using ATL::AtlAdvise;
using ATL::AtlUnadvise;

template <class T> HRESULT CComPtr<T>::Advise(IUnknown *sink, const IID &iid, LPDWORD cookie) {
  return ATL::AtlAdvise(p, sink, iid, cookie);
}
#endif
