#pragma once
#include "rpcndr.h"
/* Automation types: BSTR as a length-prefixed wide string, VARIANT minimal. */
typedef wchar_t OLECHAR;
typedef OLECHAR *LPOLESTR;
typedef const OLECHAR *LPCOLESTR;
typedef OLECHAR *BSTR;
typedef short VARIANT_BOOL;
#define VARIANT_TRUE ((VARIANT_BOOL)-1)
#define VARIANT_FALSE ((VARIANT_BOOL)0)
typedef unsigned short VARTYPE;
enum { VT_EMPTY = 0, VT_NULL = 1, VT_I2 = 2, VT_I4 = 3, VT_BSTR = 8, VT_DISPATCH = 9, VT_BOOL = 11, VT_UNKNOWN = 13 };
typedef struct tagVARIANT {
    VARTYPE vt;
    WORD wReserved1, wReserved2, wReserved3;
    union {
        LONG lVal;
        SHORT iVal;
        VARIANT_BOOL boolVal;
        BSTR bstrVal;
        void *byref;
    };
} VARIANT, VARIANTARG;
typedef LONG DISPID;
typedef struct tagDISPPARAMS {
    VARIANTARG *rgvarg;
    DISPID *rgdispidNamedArgs;
    UINT cArgs, cNamedArgs;
} DISPPARAMS;
#ifdef __cplusplus
extern "C" {
#endif
BSTR SysAllocString(const OLECHAR *s);
BSTR SysAllocStringLen(const OLECHAR *s, UINT n);
void SysFreeString(BSTR s);
UINT SysStringLen(BSTR s);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
struct ITypeInfo;
struct IDispatch : public IUnknown {
    virtual HRESULT GetTypeInfoCount(UINT *count) = 0;
    virtual HRESULT GetTypeInfo(UINT index, LCID lcid, ITypeInfo **info) = 0;
    virtual HRESULT GetIDsOfNames(REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids) = 0;
    virtual HRESULT Invoke(DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
                           void *excep, UINT *arg_err) = 0;
};
typedef IDispatch *LPDISPATCH;
#endif
