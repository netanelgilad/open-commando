/*
** D3DX helpers WW3D2 uses: texture creation and file loading (DDS, TGA,
** BMP), surface-to-surface copies with format conversion and scaling, mip
** generation, FVF size and error strings.
*/
#include "d3d9_internal.h"
#include "d3dx9tex.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rebel_d3d {

namespace {

bool Is_Gpu(Surface *s) {
  return s->SurfaceKind == Surface::Kind::BackBuffer || s->SurfaceKind == Surface::Kind::RenderTarget ||
         (s->SurfaceKind == Surface::Kind::TextureLevel && s->Owner && s->Owner->Is_Render_Target());
}

// Resamples an RGBA8 image. Box filter when shrinking with `linear`,
// bilinear when enlarging, nearest otherwise.
void Resample(const uint8_t *src, UINT sw, UINT sh, uint8_t *dst, UINT dw, UINT dh, bool linear) {
  for (UINT y = 0; y < dh; y++) {
    for (UINT x = 0; x < dw; x++) {
      uint8_t *d = dst + ((size_t)y * dw + x) * 4;
      if (!linear) {
        UINT sx = std::min(sw - 1, (UINT)((x + 0.5) * sw / dw));
        UINT sy = std::min(sh - 1, (UINT)((y + 0.5) * sh / dh));
        memcpy(d, src + ((size_t)sy * sw + sx) * 4, 4);
        continue;
      }
      double fx0 = (double)x * sw / dw, fx1 = (double)(x + 1) * sw / dw;
      double fy0 = (double)y * sh / dh, fy1 = (double)(y + 1) * sh / dh;
      if (fx1 - fx0 >= 1.0 || fy1 - fy0 >= 1.0) {
        UINT x0 = (UINT)fx0, x1 = std::max(x0 + 1, std::min(sw, (UINT)(fx1 + 0.999)));
        UINT y0 = (UINT)fy0, y1 = std::max(y0 + 1, std::min(sh, (UINT)(fy1 + 0.999)));
        unsigned sum[4] = {}, n = 0;
        for (UINT yy = y0; yy < y1; yy++)
          for (UINT xx = x0; xx < x1; xx++, n++)
            for (int c = 0; c < 4; c++)
              sum[c] += src[((size_t)yy * sw + xx) * 4 + c];
        for (int c = 0; c < 4; c++)
          d[c] = (uint8_t)((sum[c] + n / 2) / n);
      } else {
        double cx = (x + 0.5) * sw / dw - 0.5, cy = (y + 0.5) * sh / dh - 0.5;
        int ix = (int)std::floor(cx), iy = (int)std::floor(cy);
        double tx = cx - ix, ty = cy - iy;
        auto px = [&](int xx, int yy, int c) {
          xx = std::clamp(xx, 0, (int)sw - 1);
          yy = std::clamp(yy, 0, (int)sh - 1);
          return (double)src[((size_t)yy * sw + xx) * 4 + c];
        };
        for (int c = 0; c < 4; c++) {
          double v = px(ix, iy, c) * (1 - tx) * (1 - ty) + px(ix + 1, iy, c) * tx * (1 - ty) +
                     px(ix, iy + 1, c) * (1 - tx) * ty + px(ix + 1, iy + 1, c) * tx * ty;
          d[c] = (uint8_t)std::clamp((int)(v + 0.5), 0, 255);
        }
      }
    }
  }
}

} // namespace

HRESULT Copy_Surface(Surface *dst, const RECT *dst_rect, Surface *src, const RECT *src_rect, bool linear) {
  if (!dst || !src || dst->SurfaceKind == Surface::Kind::DepthStencil || src->SurfaceKind == Surface::Kind::DepthStencil)
    return D3DERR_INVALIDCALL;
  RECT sr = src_rect ? *src_rect : RECT{0, 0, (LONG)src->Width, (LONG)src->Height};
  RECT dr = dst_rect ? *dst_rect : RECT{0, 0, (LONG)dst->Width, (LONG)dst->Height};
  UINT sw = (UINT)(sr.right - sr.left), sh = (UINT)(sr.bottom - sr.top);
  UINT dw = (UINT)(dr.right - dr.left), dh = (UINT)(dr.bottom - dr.top);
  if (!sw || !sh || !dw || !dh)
    return D3D_OK;
  if (Is_Gpu(src))
    src->Read_Back();

  FormatInfo sf = Format_Info(src->Format), df = Format_Info(dst->Format);
  // Raw copy: same format and size (also the only way into DXT targets).
  if (src->Format == dst->Format && sw == dw && sh == dh) {
    UINT rows = sf.compressed ? (sh + 3) / 4 : sh;
    UINT row_bytes = sf.compressed ? ((sw + 3) / 4) * sf.bytes : sw * sf.bytes;
    UINT sx = sf.compressed ? (UINT)sr.left / 4 * sf.bytes : (UINT)sr.left * sf.bytes;
    UINT sy = sf.compressed ? (UINT)sr.top / 4 : (UINT)sr.top;
    UINT dx = df.compressed ? (UINT)dr.left / 4 * df.bytes : (UINT)dr.left * df.bytes;
    UINT dy = df.compressed ? (UINT)dr.top / 4 : (UINT)dr.top;
    for (UINT r = 0; r < rows; r++)
      memcpy(dst->Bits() + (dy + r) * dst->Pitch() + dx, src->Bits() + (sy + r) * src->Pitch() + sx, row_bytes);
    dst->Mark_Dirty();
    return D3D_OK;
  }
  if (df.compressed)
    return D3DERR_NOTAVAILABLE; // re-compression to DXT isn't needed by the engine

  // Decode the whole source (DXT blocks need aligned decode), crop, scale, encode.
  std::vector<uint8_t> full((size_t)src->Width * src->Height * 4);
  if (!Decode_To_RGBA8(src->Format, src->Bits(), src->Pitch(), src->Width, src->Height, full.data()))
    return D3DERR_NOTAVAILABLE;
  std::vector<uint8_t> crop((size_t)sw * sh * 4);
  for (UINT y = 0; y < sh; y++)
    memcpy(crop.data() + (size_t)y * sw * 4, full.data() + ((size_t)(sr.top + y) * src->Width + sr.left) * 4, sw * 4);
  std::vector<uint8_t> scaled;
  const uint8_t *pixels = crop.data();
  if (sw != dw || sh != dh) {
    scaled.resize((size_t)dw * dh * 4);
    Resample(crop.data(), sw, sh, scaled.data(), dw, dh, linear);
    pixels = scaled.data();
  }
  if (!Encode_From_RGBA8(dst->Format, pixels, dw, dh, dst->Bits() + dr.top * dst->Pitch() + dr.left * df.bytes,
                         dst->Pitch()))
    return D3DERR_NOTAVAILABLE;
  dst->Mark_Dirty();
  return D3D_OK;
}

namespace {

/* ------------------------------------------------------------ image files */

struct Image {
  UINT width = 0, height = 0;
  D3DFORMAT format = D3DFMT_A8R8G8B8;
  std::vector<std::vector<uint8_t>> levels; // in `format`
};

uint32_t Rd32(const uint8_t *p) { return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t Rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool Load_Dds(const std::vector<uint8_t> &f, Image &img) {
  if (f.size() < 128 || memcmp(f.data(), "DDS ", 4) != 0)
    return false;
  const uint8_t *h = f.data() + 4;
  img.height = Rd32(h + 8);
  img.width = Rd32(h + 12);
  UINT mips = std::max(1u, Rd32(h + 24));
  const uint8_t *pf = h + 72;
  uint32_t flags = Rd32(pf + 4), fourcc = Rd32(pf + 8), bits = Rd32(pf + 12);
  uint32_t rmask = Rd32(pf + 16), amask = Rd32(pf + 28);
  if (flags & 0x4) { // DDPF_FOURCC
    img.format = (D3DFORMAT)fourcc;
    if (!Format_Info(img.format).compressed)
      return false;
  } else if (bits == 32) {
    img.format = amask ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
  } else if (bits == 24) {
    img.format = D3DFMT_R8G8B8;
  } else if (bits == 16) {
    img.format = rmask == 0xF800 ? D3DFMT_R5G6B5 : rmask == 0x7C00 ? (amask ? D3DFMT_A1R5G5B5 : D3DFMT_X1R5G5B5)
                                                                   : D3DFMT_A4R4G4B4;
  } else {
    return false;
  }
  size_t off = 128;
  UINT w = img.width, h2 = img.height;
  for (UINT i = 0; i < mips; i++) {
    size_t size = Level_Size(img.format, w, h2);
    if (off + size > f.size())
      break;
    img.levels.emplace_back(f.begin() + off, f.begin() + off + size);
    off += size;
    w = std::max(1u, w / 2);
    h2 = std::max(1u, h2 / 2);
  }
  return !img.levels.empty();
}

bool Load_Tga(const std::vector<uint8_t> &f, Image &img) {
  if (f.size() < 18)
    return false;
  int id_len = f[0], cmap = f[1], type = f[2], bpp = f[16], desc = f[17];
  img.width = Rd16(&f[12]);
  img.height = Rd16(&f[14]);
  if (cmap || !img.width || !img.height || (bpp != 8 && bpp != 24 && bpp != 32) ||
      (type != 2 && type != 3 && type != 10 && type != 11))
    return false;
  size_t pos = 18 + id_len;
  int bytes = bpp / 8;
  size_t count = (size_t)img.width * img.height;
  std::vector<uint8_t> raw(count * bytes);
  if (type == 2 || type == 3) {
    if (pos + raw.size() > f.size())
      return false;
    memcpy(raw.data(), &f[pos], raw.size());
  } else {
    size_t px = 0;
    while (px < count && pos < f.size()) {
      int hdr = f[pos++];
      int n = (hdr & 127) + 1;
      if (hdr & 128) {
        if (pos + bytes > f.size())
          return false;
        for (int i = 0; i < n && px < count; i++, px++)
          memcpy(&raw[px * bytes], &f[pos], bytes);
        pos += bytes;
      } else {
        for (int i = 0; i < n && px < count; i++, px++) {
          if (pos + bytes > f.size())
            return false;
          memcpy(&raw[px * bytes], &f[pos], bytes);
          pos += bytes;
        }
      }
    }
  }
  // To A8R8G8B8 (B,G,R,A bytes), top row first.
  img.format = D3DFMT_A8R8G8B8;
  std::vector<uint8_t> out(count * 4);
  bool top_down = (desc & 0x20) != 0;
  for (UINT y = 0; y < img.height; y++) {
    UINT sy = top_down ? y : img.height - 1 - y;
    for (UINT x = 0; x < img.width; x++) {
      const uint8_t *s = &raw[((size_t)sy * img.width + x) * bytes];
      uint8_t *d = &out[((size_t)y * img.width + x) * 4];
      if (bytes == 1) {
        d[0] = d[1] = d[2] = s[0];
        d[3] = 255;
      } else {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = bytes == 4 ? s[3] : 255;
      }
    }
  }
  img.levels.push_back(std::move(out));
  return true;
}

bool Load_Bmp(const std::vector<uint8_t> &f, Image &img) {
  if (f.size() < 54 || f[0] != 'B' || f[1] != 'M')
    return false;
  uint32_t data = Rd32(&f[10]);
  int32_t w = (int32_t)Rd32(&f[18]), h = (int32_t)Rd32(&f[22]);
  int bpp = Rd16(&f[28]);
  if (Rd32(&f[30]) != 0 || (bpp != 24 && bpp != 32) || w <= 0 || h == 0)
    return false;
  bool top_down = h < 0;
  img.width = (UINT)w;
  img.height = (UINT)std::abs(h);
  img.format = D3DFMT_A8R8G8B8;
  size_t pitch = ((size_t)w * (bpp / 8) + 3) & ~(size_t)3;
  std::vector<uint8_t> out((size_t)img.width * img.height * 4);
  for (UINT y = 0; y < img.height; y++) {
    UINT sy = top_down ? y : img.height - 1 - y;
    size_t row = data + sy * pitch;
    if (row + (size_t)w * (bpp / 8) > f.size())
      return false;
    for (UINT x = 0; x < img.width; x++) {
      const uint8_t *s = &f[row + (size_t)x * (bpp / 8)];
      uint8_t *d = &out[((size_t)y * img.width + x) * 4];
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = bpp == 32 ? s[3] : 255;
    }
  }
  img.levels.push_back(std::move(out));
  return true;
}

void Generate_Mips(Texture *t, UINT from, bool linear) {
  if (Format_Info(t->Format).compressed)
    return;
  for (UINT i = from + 1; i < t->Levels.size(); i++) {
    Texture::LevelData &prev = t->Levels[i - 1], &cur = t->Levels[i];
    std::vector<uint8_t> a((size_t)prev.width * prev.height * 4), b((size_t)cur.width * cur.height * 4);
    Decode_To_RGBA8(t->Format, prev.bits.data(), Level_Pitch(t->Format, prev.width), prev.width, prev.height, a.data());
    Resample(a.data(), prev.width, prev.height, b.data(), cur.width, cur.height, linear);
    Encode_From_RGBA8(t->Format, b.data(), cur.width, cur.height, cur.bits.data(), Level_Pitch(t->Format, cur.width));
    cur.dirty = true;
  }
  t->AnyDirty = true;
}

} // namespace
} // namespace rebel_d3d

using namespace rebel_d3d;

extern "C" HRESULT D3DXCreateTexture(IDirect3DDevice9 *device, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                     D3DPOOL pool, IDirect3DTexture9 **tex) {
  if (w == D3DX_DEFAULT || w == 0)
    w = 256;
  if (h == D3DX_DEFAULT || h == 0)
    h = 256;
  if (levels == D3DX_DEFAULT)
    levels = 0;
  if (!Format_Info(fmt).known || fmt == D3DFMT_P8 || fmt == D3DFMT_A8P8)
    fmt = D3DFMT_A8R8G8B8;
  return device->CreateTexture(w, h, levels, usage, fmt, pool, tex, nullptr);
}

extern "C" HRESULT D3DXCreateTextureFromFileExA(IDirect3DDevice9 *device, const char *file, UINT, UINT, UINT levels,
                                                DWORD usage, D3DFORMAT, D3DPOOL pool, DWORD, DWORD, D3DCOLOR,
                                                D3DXIMAGE_INFO *info, PALETTEENTRY *, IDirect3DTexture9 **tex) {
  *tex = nullptr;
  FILE *f = fopen(file, "rb");
  if (!f)
    return D3DERR_NOTAVAILABLE;
  std::vector<uint8_t> data;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  data.resize(n > 0 ? (size_t)n : 0);
  size_t got = fread(data.data(), 1, data.size(), f);
  fclose(f);
  data.resize(got);
  Image img;
  if (!Load_Dds(data, img) && !Load_Bmp(data, img) && !Load_Tga(data, img))
    return D3DERR_NOTAVAILABLE;
  if (levels == D3DX_DEFAULT)
    levels = 0;
  if (img.levels.size() > 1 && levels == 0)
    levels = (UINT)img.levels.size();
  if (FAILED(device->CreateTexture(img.width, img.height, levels, usage, img.format, pool, tex, nullptr)))
    return D3DERR_NOTAVAILABLE;
  Texture *t = static_cast<Texture *>(*tex);
  for (size_t i = 0; i < img.levels.size() && i < t->Levels.size(); i++)
    if (img.levels[i].size() == t->Levels[i].bits.size())
      t->Levels[i].bits = img.levels[i];
  if (img.levels.size() < t->Levels.size())
    Generate_Mips(t, (UINT)img.levels.size() - 1, true);
  for (auto &l : t->Levels)
    l.dirty = true;
  t->AnyDirty = true;
  if (info) {
    info->Width = img.width;
    info->Height = img.height;
    info->Depth = 1;
    info->MipLevels = (UINT)img.levels.size();
    info->Format = img.format;
    info->ResourceType = D3DRTYPE_TEXTURE;
    info->ImageFileFormat = 0;
  }
  return D3D_OK;
}

extern "C" HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface9 *dst, const PALETTEENTRY *, const RECT *dst_rect,
                                              IDirect3DSurface9 *src, const PALETTEENTRY *, const RECT *src_rect,
                                              DWORD filter, D3DCOLOR) {
  bool linear = (filter & 0xFF) != D3DX_FILTER_NONE && (filter & 0xFF) != D3DX_FILTER_POINT;
  return Copy_Surface(static_cast<Surface *>(dst), dst_rect, static_cast<Surface *>(src), src_rect, linear);
}

extern "C" HRESULT D3DXFilterTexture(IDirect3DBaseTexture9 *tex, const PALETTEENTRY *, UINT src_level, DWORD filter) {
  Texture *t = static_cast<Texture *>(static_cast<IDirect3DTexture9 *>(tex));
  if (!t)
    return D3DERR_INVALIDCALL;
  if (src_level == D3DX_DEFAULT)
    src_level = 0;
  Generate_Mips(t, src_level, (filter & 0xFF) != D3DX_FILTER_POINT && (filter & 0xFF) != D3DX_FILTER_NONE);
  return D3D_OK;
}

extern "C" UINT D3DXGetFVFVertexSize(DWORD fvf) { return Fvf_Layout(fvf).stride; }

extern "C" const char *D3DXGetErrorStringA(HRESULT hr) {
  static char buf[64];
  switch (hr) {
  case D3D_OK: return "D3D_OK";
  case D3DERR_INVALIDCALL: return "D3DERR_INVALIDCALL";
  case D3DERR_NOTAVAILABLE: return "D3DERR_NOTAVAILABLE";
  case D3DERR_DEVICELOST: return "D3DERR_DEVICELOST";
  case D3DERR_OUTOFVIDEOMEMORY: return "D3DERR_OUTOFVIDEOMEMORY";
  default: snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", (unsigned long)hr); return buf;
  }
}
