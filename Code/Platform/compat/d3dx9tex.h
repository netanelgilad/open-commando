/*
** The D3DX helper subset WW3D2 uses (texture creation/loading/filtering,
** FVF size, error strings, two math types). Implemented in Platform/gfx.
*/
#pragma once
#include "d3d9.h"

#define D3DX_DEFAULT ((UINT)-1)
#define D3DX_FILTER_NONE (1 << 0)
#define D3DX_FILTER_POINT (2 << 0)
#define D3DX_FILTER_LINEAR (3 << 0)
#define D3DX_FILTER_TRIANGLE (4 << 0)
#define D3DX_FILTER_BOX (5 << 0)

typedef struct _D3DXIMAGE_INFO {
    UINT Width, Height, Depth, MipLevels;
    D3DFORMAT Format;
    D3DRESOURCETYPE ResourceType;
    DWORD ImageFileFormat;
} D3DXIMAGE_INFO;

typedef struct tagPALETTEENTRY { BYTE peRed, peGreen, peBlue, peFlags; } PALETTEENTRY;

#ifdef __cplusplus
struct D3DXVECTOR3 : public D3DVECTOR {
    D3DXVECTOR3() {}
    D3DXVECTOR3(float fx, float fy, float fz) { x = fx; y = fy; z = fz; }
};
struct D3DXVECTOR4 {
    float x, y, z, w;
    D3DXVECTOR4() {}
    D3DXVECTOR4(float fx, float fy, float fz, float fw) : x(fx), y(fy), z(fz), w(fw) {}
};
struct D3DXMATRIX : public D3DMATRIX {
    D3DXMATRIX() {}
    D3DXMATRIX(const D3DMATRIX &m) : D3DMATRIX(m) {}
    float &operator()(UINT r, UINT c) { return m[r][c]; }
    float operator()(UINT r, UINT c) const { return m[r][c]; }
};

extern "C" {
#endif
HRESULT D3DXCreateTexture(IDirect3DDevice9 *device, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                          D3DPOOL pool, IDirect3DTexture9 **tex);
HRESULT D3DXCreateTextureFromFileExA(IDirect3DDevice9 *device, const char *file, UINT w, UINT h, UINT levels,
                                     DWORD usage, D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mip_filter,
                                     D3DCOLOR color_key, D3DXIMAGE_INFO *info, PALETTEENTRY *palette,
                                     IDirect3DTexture9 **tex);
HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface9 *dst, const PALETTEENTRY *dst_pal, const RECT *dst_rect,
                                   IDirect3DSurface9 *src, const PALETTEENTRY *src_pal, const RECT *src_rect,
                                   DWORD filter, D3DCOLOR color_key);
HRESULT D3DXFilterTexture(IDirect3DBaseTexture9 *tex, const PALETTEENTRY *pal, UINT src_level, DWORD filter);
UINT D3DXGetFVFVertexSize(DWORD fvf);
const char *D3DXGetErrorStringA(HRESULT hr);
#define D3DXGetErrorString D3DXGetErrorStringA
#ifdef __cplusplus
}
#endif
