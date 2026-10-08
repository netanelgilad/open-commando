/*
** D3D texture formats: sizes and conversion to/from RGBA8.
**
** D3D names list channels from the most significant bit down, and the data
** is little-endian, so D3DFMT_A8R8G8B8 is the byte sequence B,G,R,A.
*/
#include "d3d9_internal.h"

#include <algorithm>
#include <cstring>

namespace rebel_d3d {

FormatInfo Format_Info(D3DFORMAT fmt) {
  FormatInfo f;
  f.known = true;
  switch (fmt) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8B8G8R8:
  case D3DFMT_X8L8V8U8:
  case D3DFMT_Q8W8V8U8:
  case D3DFMT_V16U16:
  case D3DFMT_W11V11U10:
  case D3DFMT_D32:
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24X4S4:
  case D3DFMT_INDEX32: f.bytes = 4; break;
  case D3DFMT_R8G8B8: f.bytes = 3; break;
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A4R4G4B4:
  case D3DFMT_X4R4G4B4:
  case D3DFMT_A8R3G3B2:
  case D3DFMT_A8P8:
  case D3DFMT_A8L8:
  case D3DFMT_V8U8:
  case D3DFMT_L6V5U5:
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
  case D3DFMT_D15S1:
  case D3DFMT_INDEX16:
  case D3DFMT_UYVY:
  case D3DFMT_YUY2: f.bytes = 2; break;
  case D3DFMT_R3G3B2:
  case D3DFMT_A8:
  case D3DFMT_P8:
  case D3DFMT_L8:
  case D3DFMT_A4L4: f.bytes = 1; break;
  case D3DFMT_DXT1:
    f.compressed = true;
    f.bytes = 8;
    break;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    f.compressed = true;
    f.bytes = 16;
    break;
  default: f.known = false; break;
  }
  return f;
}

UINT Level_Pitch(D3DFORMAT fmt, UINT width) {
  FormatInfo f = Format_Info(fmt);
  if (f.compressed)
    return std::max(1u, (width + 3) / 4) * f.bytes;
  return width * (f.bytes ? f.bytes : 4);
}

UINT Level_Rows(D3DFORMAT fmt, UINT height) {
  return Format_Info(fmt).compressed ? std::max(1u, (height + 3) / 4) : height;
}

UINT Level_Size(D3DFORMAT fmt, UINT width, UINT height) { return Level_Pitch(fmt, width) * Level_Rows(fmt, height); }

/* --------------------------------------------------------------- DXTn */

namespace {

void Rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
  out[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
  out[2] = (uint8_t)((c & 31) * 255 / 31);
}

// Decodes the 8-byte color part of a DXT block into 16 RGBA pixels.
void Decode_Color_Block(const uint8_t *b, uint8_t px[16][4], bool dxt1_alpha) {
  uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8)), c1 = (uint16_t)(b[2] | (b[3] << 8));
  uint8_t pal[4][4];
  Rgb565(c0, pal[0]);
  Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || !dxt1_alpha) {
    for (int i = 0; i < 3; i++) {
      pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3);
      pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int i = 0; i < 3; i++) {
      pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
      pal[3][i] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
  for (int i = 0; i < 16; i++)
    memcpy(px[i], pal[(bits >> (2 * i)) & 3], 4);
}

void Decode_Dxt_Block(D3DFORMAT fmt, const uint8_t *b, uint8_t px[16][4]) {
  if (fmt == D3DFMT_DXT1) {
    Decode_Color_Block(b, px, true);
    return;
  }
  Decode_Color_Block(b + 8, px, false);
  if (fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3) {
    for (int i = 0; i < 16; i++) {
      int nib = (b[i / 2] >> ((i & 1) * 4)) & 15;
      px[i][3] = (uint8_t)(nib * 17);
    }
  } else {
    uint8_t a0 = b[0], a1 = b[1], pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
      for (int i = 1; i < 7; i++)
        pal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
    } else {
      for (int i = 1; i < 5; i++)
        pal[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
      pal[6] = 0;
      pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
      bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++)
      px[i][3] = pal[(bits >> (3 * i)) & 7];
  }
  if (fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT4) {
    // premultiplied: un-premultiply for a straight-alpha RGBA result
    for (int i = 0; i < 16; i++)
      if (px[i][3])
        for (int c = 0; c < 3; c++)
          px[i][c] = (uint8_t)std::min(255, px[i][c] * 255 / px[i][3]);
  }
}

inline uint8_t Expand(uint32_t v, int bits) { return (uint8_t)(bits >= 8 ? v : (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

} // namespace

bool Decode_To_RGBA8(D3DFORMAT fmt, const uint8_t *src, UINT pitch, UINT width, UINT height, uint8_t *rgba) {
  FormatInfo info = Format_Info(fmt);
  if (!info.known)
    return false;
  if (info.compressed) {
    UINT bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (UINT by = 0; by < bh; by++) {
      for (UINT bx = 0; bx < bw; bx++) {
        uint8_t px[16][4];
        Decode_Dxt_Block(fmt, src + by * pitch + bx * info.bytes, px);
        for (int i = 0; i < 16; i++) {
          UINT x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
          if (x < width && y < height)
            memcpy(rgba + ((size_t)y * width + x) * 4, px[i], 4);
        }
      }
    }
    return true;
  }
  for (UINT y = 0; y < height; y++) {
    const uint8_t *s = src + (size_t)y * pitch;
    uint8_t *d = rgba + (size_t)y * width * 4;
    for (UINT x = 0; x < width; x++, d += 4) {
      uint32_t v = 0;
      switch (info.bytes) {
      case 1: v = s[x]; break;
      case 2: v = (uint32_t)(s[2 * x] | (s[2 * x + 1] << 8)); break;
      case 3: v = (uint32_t)(s[3 * x] | (s[3 * x + 1] << 8) | (s[3 * x + 2] << 16)); break;
      default: v = (uint32_t)(s[4 * x] | (s[4 * x + 1] << 8) | (s[4 * x + 2] << 16) | ((uint32_t)s[4 * x + 3] << 24)); break;
      }
      uint8_t r = 0, g = 0, b = 0, a = 255;
      switch (fmt) {
      case D3DFMT_A8R8G8B8: a = (uint8_t)(v >> 24); /* fallthrough */
      case D3DFMT_X8R8G8B8:
      case D3DFMT_R8G8B8:
        r = (uint8_t)(v >> 16);
        g = (uint8_t)(v >> 8);
        b = (uint8_t)v;
        break;
      case D3DFMT_A8B8G8R8:
        a = (uint8_t)(v >> 24);
        b = (uint8_t)(v >> 16);
        g = (uint8_t)(v >> 8);
        r = (uint8_t)v;
        break;
      case D3DFMT_R5G6B5:
        r = Expand((v >> 11) & 31, 5);
        g = Expand((v >> 5) & 63, 6);
        b = Expand(v & 31, 5);
        break;
      case D3DFMT_A1R5G5B5: a = (v & 0x8000) ? 255 : 0; /* fallthrough */
      case D3DFMT_X1R5G5B5:
        r = Expand((v >> 10) & 31, 5);
        g = Expand((v >> 5) & 31, 5);
        b = Expand(v & 31, 5);
        break;
      case D3DFMT_A4R4G4B4: a = Expand((v >> 12) & 15, 4); /* fallthrough */
      case D3DFMT_X4R4G4B4:
        r = Expand((v >> 8) & 15, 4);
        g = Expand((v >> 4) & 15, 4);
        b = Expand(v & 15, 4);
        break;
      case D3DFMT_A8R3G3B2: a = (uint8_t)(v >> 8); /* fallthrough */
      case D3DFMT_R3G3B2:
        r = Expand((v >> 5) & 7, 3);
        g = Expand((v >> 2) & 7, 3);
        b = Expand(v & 3, 2);
        break;
      case D3DFMT_A8: r = g = b = 255; a = (uint8_t)v; break;
      case D3DFMT_L8:
      case D3DFMT_P8: r = g = b = (uint8_t)v; break;
      case D3DFMT_A8L8:
      case D3DFMT_A8P8:
        r = g = b = (uint8_t)v;
        a = (uint8_t)(v >> 8);
        break;
      case D3DFMT_A4L4:
        r = g = b = Expand(v & 15, 4);
        a = Expand((v >> 4) & 15, 4);
        break;
      // Signed bump formats are stored biased (value + 128) so shaders can
      // recover [-1, 1] with x * 2 - 1.
      case D3DFMT_V8U8:
        r = (uint8_t)((int8_t)(v & 0xFF) + 128);
        g = (uint8_t)((int8_t)((v >> 8) & 0xFF) + 128);
        b = 128;
        break;
      case D3DFMT_X8L8V8U8:
        r = (uint8_t)((int8_t)(v & 0xFF) + 128);
        g = (uint8_t)((int8_t)((v >> 8) & 0xFF) + 128);
        b = (uint8_t)(v >> 16);
        break;
      case D3DFMT_L6V5U5: {
        int u = (int)(v & 31);
        int vv = (int)((v >> 5) & 31);
        if (u & 16) u -= 32;
        if (vv & 16) vv -= 32;
        r = (uint8_t)(u * 8 + 128);
        g = (uint8_t)(vv * 8 + 128);
        b = Expand((v >> 10) & 63, 6);
        break;
      }
      case D3DFMT_Q8W8V8U8:
        r = (uint8_t)((int8_t)(v & 0xFF) + 128);
        g = (uint8_t)((int8_t)((v >> 8) & 0xFF) + 128);
        b = (uint8_t)((int8_t)((v >> 16) & 0xFF) + 128);
        a = (uint8_t)((int8_t)(v >> 24) + 128);
        break;
      default: return false;
      }
      d[0] = r;
      d[1] = g;
      d[2] = b;
      d[3] = a;
    }
  }
  return true;
}

bool Encode_From_RGBA8(D3DFORMAT fmt, const uint8_t *rgba, UINT width, UINT height, uint8_t *dst, UINT pitch) {
  FormatInfo info = Format_Info(fmt);
  if (!info.known || info.compressed)
    return false;
  for (UINT y = 0; y < height; y++) {
    const uint8_t *s = rgba + (size_t)y * width * 4;
    uint8_t *d = dst + (size_t)y * pitch;
    for (UINT x = 0; x < width; x++, s += 4) {
      uint32_t r = s[0], g = s[1], b = s[2], a = s[3], v;
      switch (fmt) {
      case D3DFMT_A8R8G8B8:
      case D3DFMT_X8R8G8B8:
      case D3DFMT_R8G8B8: v = (a << 24) | (r << 16) | (g << 8) | b; break;
      case D3DFMT_A8B8G8R8: v = (a << 24) | (b << 16) | (g << 8) | r; break;
      case D3DFMT_R5G6B5: v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); break;
      case D3DFMT_A1R5G5B5:
      case D3DFMT_X1R5G5B5: v = ((a >= 128) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3); break;
      case D3DFMT_A4R4G4B4:
      case D3DFMT_X4R4G4B4: v = ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4); break;
      case D3DFMT_R3G3B2: v = ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6); break;
      case D3DFMT_A8R3G3B2: v = (a << 8) | ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6); break;
      case D3DFMT_A8: v = a; break;
      case D3DFMT_L8: v = (r * 77 + g * 151 + b * 28) >> 8; break;
      case D3DFMT_A8L8: v = (a << 8) | ((r * 77 + g * 151 + b * 28) >> 8); break;
      case D3DFMT_A4L4: v = ((a >> 4) << 4) | (((r * 77 + g * 151 + b * 28) >> 8) >> 4); break;
      default: return false;
      }
      switch (info.bytes) {
      case 1: d[x] = (uint8_t)v; break;
      case 2:
        d[2 * x] = (uint8_t)v;
        d[2 * x + 1] = (uint8_t)(v >> 8);
        break;
      case 3:
        d[3 * x] = (uint8_t)v;
        d[3 * x + 1] = (uint8_t)(v >> 8);
        d[3 * x + 2] = (uint8_t)(v >> 16);
        break;
      default:
        d[4 * x] = (uint8_t)v;
        d[4 * x + 1] = (uint8_t)(v >> 8);
        d[4 * x + 2] = (uint8_t)(v >> 16);
        d[4 * x + 3] = (uint8_t)(v >> 24);
        break;
      }
    }
  }
  return true;
}

} // namespace rebel_d3d
