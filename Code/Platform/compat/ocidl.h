#pragma once
#include "oaidl.h"
#ifdef __cplusplus
struct IConnectionPoint;
struct IEnumConnections;
struct IEnumConnectionPoints;
struct IConnectionPointContainer : public IUnknown {
    virtual HRESULT EnumConnectionPoints(IEnumConnectionPoints **e) = 0;
    virtual HRESULT FindConnectionPoint(REFIID riid, IConnectionPoint **cp) = 0;
};
struct IConnectionPoint : public IUnknown {
    virtual HRESULT GetConnectionInterface(IID *iid) = 0;
    virtual HRESULT GetConnectionPointContainer(IConnectionPointContainer **c) = 0;
    virtual HRESULT Advise(IUnknown *sink, DWORD *cookie) = 0;
    virtual HRESULT Unadvise(DWORD cookie) = 0;
    virtual HRESULT EnumConnections(IEnumConnections **e) = 0;
};
#endif
