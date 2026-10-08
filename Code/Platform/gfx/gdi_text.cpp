/*
** GDI text subset on stb_truetype.
**
** Render2DSentenceClass builds its glyph cache by drawing one character at a
** time, white on black, into a 24-bit DIB section, then reads the pixels
** back. This file provides exactly that: fonts resolved by face name from
** the .ttf files next to the game (Arial MT, Regatta Condensed LET), memory
** DCs, DIB sections, text metrics, extents and glyph output.
**
** Synthetic bold widens coverage by one pixel, as GDI's emboldening does.
** Italic is not synthesized (tracked in docs/WEB_PORT_GAPS.md).
*/
#include "windows.h"
#include "wingdi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace {

struct FontFile {
  std::string path;
  std::vector<unsigned char> data;
  stbtt_fontinfo info;
};

enum class GdiKind : unsigned { Font = 0x46475244, Bitmap = 0x42475244, Dc = 0x44475244 };

struct GdiObject {
  GdiKind kind;
  explicit GdiObject(GdiKind k) : kind(k) {}
  virtual ~GdiObject() {}
};

struct Font : GdiObject {
  FontFile *file = nullptr;
  float scale = 1.0f;
  int ascent = 0, descent = 0, em_px = 0;
  bool bold = false, italic = false;
  Font() : GdiObject(GdiKind::Font) {}
};

struct Bitmap : GdiObject {
  int width = 0, height = 0, pitch = 0;
  bool top_down = true;
  std::vector<unsigned char> bits;
  Bitmap() : GdiObject(GdiKind::Bitmap) {}
  unsigned char *Row(int y) { return bits.data() + (size_t)(top_down ? y : height - 1 - y) * pitch; }
};

struct Dc : GdiObject {
  Font *font = nullptr;
  Bitmap *bitmap = nullptr;
  COLORREF text = RGB(0, 0, 0);
  COLORREF bk = RGB(255, 255, 255);
  int bk_mode = OPAQUE;
  Dc() : GdiObject(GdiKind::Dc) {}
};

std::mutex Lock;
// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::vector<std::unique_ptr<FontFile>> &Fonts_Instance() {
  static std::vector<std::unique_ptr<FontFile>> *instance = new std::vector<std::unique_ptr<FontFile>>();
  return *instance;
}
#define Fonts Fonts_Instance()
bool FontsScanned = false;
Dc ScreenDc;

template <class T> T *As(void *h, GdiKind kind) {
  if (!h || (ULONG_PTR)h < 0x10000)
    return nullptr;
  GdiObject *o = static_cast<GdiObject *>(h);
  return o->kind == kind ? static_cast<T *>(o) : nullptr;
}

void Load_Font_File(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return;
  auto ff = std::make_unique<FontFile>();
  ff->path = path;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  ff->data.resize(n > 0 ? (size_t)n : 0);
  size_t got = fread(ff->data.data(), 1, ff->data.size(), f);
  fclose(f);
  if (got != ff->data.size() || !stbtt_InitFont(&ff->info, ff->data.data(), stbtt_GetFontOffsetForIndex(ff->data.data(), 0)))
    return;
  Fonts.push_back(std::move(ff));
}

void Scan_Dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  while (dirent *e = readdir(d)) {
    size_t n = strlen(e->d_name);
    if (n > 4 && strcasecmp(e->d_name + n - 4, ".ttf") == 0)
      Load_Font_File(std::string(dir) + "/" + e->d_name);
  }
  closedir(d);
}

void Scan_Fonts() {
  if (FontsScanned)
    return;
  FontsScanned = true;
  Scan_Dir(".");
  Scan_Dir("data");
  if (const char *extra = getenv("REBEL_FONT_DIR"))
    Scan_Dir(extra);
  if (Fonts.empty())
    fprintf(stderr, "[gdi] no .ttf fonts found next to the game; text will be blank\n");
}

// Compares a font's family name (name ID 1) to the requested face.
bool Family_Matches(const FontFile &ff, const char *face) {
  static const int platforms[][3] = {{3, 1, 0x409}, {1, 0, 0}};
  for (auto &p : platforms) {
    int len = 0;
    const char *raw = stbtt_GetFontNameString(&ff.info, &len, p[0], p[1], p[2], 1);
    if (!raw)
      continue;
    std::string name;
    if (p[0] == 3)
      for (int i = 0; i + 1 < len; i += 2)
        name.push_back(raw[i] ? '?' : raw[i + 1]);
    else
      name.assign(raw, len);
    if (strcasecmp(name.c_str(), face) == 0)
      return true;
  }
  return false;
}

FontFile *Find_Font(const char *face) {
  Scan_Fonts();
  if (face && *face)
    for (auto &ff : Fonts)
      if (Family_Matches(*ff, face))
        return ff.get();
  for (auto &ff : Fonts) // prefer Arial MT as the stand-in
    if (Family_Matches(*ff, "Arial MT"))
      return ff.get();
  return Fonts.empty() ? nullptr : Fonts.front().get();
}

int Advance(Font *f, wchar_t ch) {
  if (!f || !f->file)
    return 0;
  int adv = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(&f->file->info, (int)ch, &adv, &lsb);
  return (int)std::lround(adv * f->scale) + (f->bold ? 1 : 0);
}

} // namespace

extern "C" HFONT CreateFontA(int height, int, int, int, int weight, DWORD italic, DWORD, DWORD, DWORD, DWORD, DWORD,
                             DWORD, DWORD, LPCSTR face) {
  std::lock_guard<std::mutex> g(Lock);
  Font *f = new Font();
  f->file = Find_Font(face);
  f->bold = weight >= FW_BOLD;
  f->italic = italic != 0;
  if (f->file) {
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&f->file->info, &asc, &desc, &gap);
    int px = std::abs(height ? height : 12);
    // GDI: negative height = em size, positive = cell height.
    f->scale = height < 0 ? stbtt_ScaleForMappingEmToPixels(&f->file->info, (float)px)
                          : stbtt_ScaleForPixelHeight(&f->file->info, (float)px);
    f->ascent = (int)std::ceil(asc * f->scale);
    f->descent = (int)std::ceil(-desc * f->scale);
    f->em_px = height < 0 ? px : px - 0;
  }
  return (HFONT)f;
}

extern "C" HBITMAP CreateDIBSection(HDC, const BITMAPINFO *info, UINT, void **bits, HANDLE, DWORD) {
  const BITMAPINFOHEADER &h = info->bmiHeader;
  if (h.biBitCount != 24 && h.biBitCount != 32) {
    fprintf(stderr, "[gdi] CreateDIBSection: %d bpp not supported\n", h.biBitCount);
    return nullptr;
  }
  Bitmap *b = new Bitmap();
  b->width = h.biWidth;
  b->height = std::abs(h.biHeight);
  b->top_down = h.biHeight < 0;
  b->pitch = ((b->width * (h.biBitCount / 8)) + 3) & ~3;
  b->bits.assign((size_t)b->pitch * b->height, 0);
  if (bits)
    *bits = b->bits.data();
  return (HBITMAP)b;
}

extern "C" HDC CreateCompatibleDC(HDC) { return (HDC) new Dc(); }

extern "C" BOOL DeleteDC(HDC dc) {
  Dc *d = As<Dc>(dc, GdiKind::Dc);
  if (!d || d == &ScreenDc)
    return FALSE;
  delete d;
  return TRUE;
}

extern "C" HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj) {
  Dc *d = As<Dc>(dc, GdiKind::Dc);
  if (!d)
    return obj;
  if (Font *f = As<Font>(obj, GdiKind::Font)) {
    Font *old = d->font;
    d->font = f;
    return (HGDIOBJ)old;
  }
  if (Bitmap *b = As<Bitmap>(obj, GdiKind::Bitmap)) {
    Bitmap *old = d->bitmap;
    d->bitmap = b;
    return (HGDIOBJ)old;
  }
  return nullptr; // restoring a NULL "old" object
}

extern "C" BOOL DeleteObject(HGDIOBJ obj) {
  if (Font *f = As<Font>(obj, GdiKind::Font)) {
    delete f;
    return TRUE;
  }
  if (Bitmap *b = As<Bitmap>(obj, GdiKind::Bitmap)) {
    delete b;
    return TRUE;
  }
  return TRUE; // stock objects
}

extern "C" HDC GetDC(HWND) { return (HDC)&ScreenDc; }
extern "C" int ReleaseDC(HWND, HDC) { return 1; }
extern "C" HWND GetDesktopWindow(void) { return (HWND)0x2000; }

extern "C" int Rebel_Screen_Width(void);
extern "C" int Rebel_Screen_Height(void);

extern "C" int GetDeviceCaps(HDC, int idx) {
  switch (idx) {
  case HORZRES: return Rebel_Screen_Width();
  case VERTRES: return Rebel_Screen_Height();
  case BITSPIXEL: return 32;
  case VREFRESH: return 60;
  case LOGPIXELSX:
  case LOGPIXELSY: return 96;
  default: return 0;
  }
}

extern "C" COLORREF SetTextColor(HDC dc, COLORREF c) {
  Dc *d = As<Dc>(dc, GdiKind::Dc);
  if (!d)
    return 0;
  COLORREF old = d->text;
  d->text = c;
  return old;
}

extern "C" COLORREF SetBkColor(HDC dc, COLORREF c) {
  Dc *d = As<Dc>(dc, GdiKind::Dc);
  if (!d)
    return 0;
  COLORREF old = d->bk;
  d->bk = c;
  return old;
}

extern "C" int SetBkMode(HDC dc, int mode) {
  Dc *d = As<Dc>(dc, GdiKind::Dc);
  if (!d)
    return 0;
  int old = d->bk_mode;
  d->bk_mode = mode;
  return old;
}

extern "C" BOOL GetTextMetricsA(HDC dc, LPTEXTMETRIC tm) {
  Dc *d = As<Dc>(dc, GdiKind::Dc);
  memset(tm, 0, sizeof(*tm));
  if (!d || !d->font)
    return FALSE;
  Font *f = d->font;
  tm->tmAscent = f->ascent;
  tm->tmDescent = f->descent;
  tm->tmHeight = f->ascent + f->descent;
  tm->tmInternalLeading = std::max(0L, tm->tmHeight - (LONG)f->em_px);
  tm->tmAveCharWidth = Advance(f, L'x');
  tm->tmMaxCharWidth = Advance(f, L'W');
  tm->tmWeight = f->bold ? FW_BOLD : FW_NORMAL;
  tm->tmItalic = f->italic;
  tm->tmFirstChar = 32;
  tm->tmLastChar = 255;
  tm->tmDefaultChar = '?';
  tm->tmBreakChar = ' ';
  return TRUE;
}

extern "C" BOOL GetTextExtentPoint32W(HDC dc, LPCWSTR text, int count, LPSIZE size) {
  Dc *d = As<Dc>(dc, GdiKind::Dc);
  size->cx = size->cy = 0;
  if (!d || !d->font)
    return FALSE;
  for (int i = 0; i < count; i++)
    size->cx += Advance(d->font, text[i]);
  size->cy = d->font->ascent + d->font->descent;
  return TRUE;
}

extern "C" BOOL ExtTextOutW(HDC dc, int x, int y, UINT options, const RECT *rect, LPCWSTR text, UINT count,
                            const INT *dx) {
  Dc *d = As<Dc>(dc, GdiKind::Dc);
  if (!d || !d->bitmap)
    return FALSE;
  Bitmap *b = d->bitmap;
  const int bpp = (b->pitch / std::max(b->width, 1)) >= 4 ? 4 : 3;
  auto put = [&](int px, int py, COLORREF c, float a) {
    if (px < 0 || py < 0 || px >= b->width || py >= b->height)
      return;
    unsigned char *p = b->Row(py) + px * bpp;
    // DIB pixels are BGR.
    p[0] = (unsigned char)std::lround(p[0] * (1 - a) + GetBValue(c) * a);
    p[1] = (unsigned char)std::lround(p[1] * (1 - a) + GetGValue(c) * a);
    p[2] = (unsigned char)std::lround(p[2] * (1 - a) + GetRValue(c) * a);
  };
  if ((options & ETO_OPAQUE) && rect)
    for (int py = rect->top; py < rect->bottom; py++)
      for (int px = rect->left; px < rect->right; px++)
        put(px, py, d->bk, 1.0f);
  Font *f = d->font;
  if (!f || !f->file)
    return TRUE;
  int pen = x;
  const int baseline = y + f->ascent;
  for (UINT i = 0; i < count; i++) {
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char *cov =
        stbtt_GetCodepointBitmap(&f->file->info, f->scale, f->scale, (int)text[i], &w, &h, &xoff, &yoff);
    if (cov) {
      for (int gy = 0; gy < h; gy++) {
        for (int gx = 0; gx < w + (f->bold ? 1 : 0); gx++) {
          int c0 = gx < w ? cov[gy * w + gx] : 0;
          int c1 = (f->bold && gx > 0) ? cov[gy * w + gx - 1] : 0;
          int c = std::max(c0, c1);
          if (!c)
            continue;
          int px = pen + xoff + gx, py = baseline + yoff + gy;
          if ((options & ETO_CLIPPED) && rect &&
              (px < rect->left || px >= rect->right || py < rect->top || py >= rect->bottom))
            continue;
          put(px, py, d->text, c / 255.0f);
        }
      }
      stbtt_FreeBitmap(cov, nullptr);
    }
    pen += dx ? dx[i] : Advance(f, text[i]);
  }
  return TRUE;
}

extern "C" int MulDiv(int number, int numerator, int denominator) {
  if (denominator == 0)
    return -1;
  long long r = (long long)number * numerator;
  // round half away from zero, like Win32
  long long q = (r + ((r >= 0) == (denominator > 0) ? denominator / 2 : -denominator / 2)) / denominator;
  return (int)q;
}

extern "C" BOOL SetDeviceGammaRamp(HDC, LPVOID) { return TRUE; } // gamma applied in the present shader
extern "C" BOOL GetDeviceGammaRamp(HDC, LPVOID) { return FALSE; }
extern "C" int AddFontResourceA(LPCSTR file) {
  std::lock_guard<std::mutex> g(Lock);
  Scan_Fonts();
  Load_Font_File(file);
  return 1;
}
extern "C" BOOL RemoveFontResourceA(LPCSTR) { return TRUE; }
