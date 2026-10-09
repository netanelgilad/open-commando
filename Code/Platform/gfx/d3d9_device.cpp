/*
** IDirect3D9 / IDirect3DDevice9 on WebGL2 (fixed-function only).
** See d3d9_internal.h for the coordinate conventions.
*/
#include "d3d9_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

extern "C" void Rebel_Frame_Yield(void);
extern "C" unsigned Rebel_Stat_Sleep_Count; // win32_core.cpp
extern "C" double Rebel_Stat_Sleep_Ms;
extern "C" double Rebel_Stat_Audio_Ms;
DWORD Rebel_Last_Frame_Yield = 0;

namespace rebel_d3d {

unsigned long long Stat_Buffer_Bytes = 0, Stat_Texture_Bytes = 0;
unsigned Stat_Buffer_Uploads = 0, Stat_Texture_Uploads = 0;

HRESULT Copy_Surface(Surface *dst, const RECT *dst_rect, Surface *src, const RECT *src_rect, bool linear);

namespace {

inline float F(DWORD v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline DWORD D(float f) {
  DWORD v;
  memcpy(&v, &f, 4);
  return v;
}

void Color_To_Vec4(D3DCOLOR c, float out[4]) {
  out[0] = ((c >> 16) & 0xFF) / 255.0f;
  out[1] = ((c >> 8) & 0xFF) / 255.0f;
  out[2] = (c & 0xFF) / 255.0f;
  out[3] = ((c >> 24) & 0xFF) / 255.0f;
}

D3DMATRIX Identity() {
  D3DMATRIX m = {};
  m._11 = m._22 = m._33 = m._44 = 1.0f;
  return m;
}

D3DMATRIX Multiply(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX r;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
  return r;
}

// Column-major mat3 for GLSL that maps a column normal n to (n_row * inverse(M)^T)^T.
void Normal_Matrix(const D3DMATRIX &m, float out[9]) {
  float a = m._11, b = m._12, c = m._13, d = m._21, e = m._22, f = m._23, g = m._31, h = m._32, i = m._33;
  float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-12f)
    det = 1.0f;
  float inv[3][3] = {{(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det},
                     {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det},
                     {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}};
  // GLSL wants N = inv (math); column-major storage: out[col*3+row] = inv[row][col]
  for (int col = 0; col < 3; col++)
    for (int row = 0; row < 3; row++)
      out[col * 3 + row] = inv[row][col];
}

void Transform_Point(const D3DVECTOR &p, const D3DMATRIX &m, float out[3]) {
  out[0] = p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41;
  out[1] = p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42;
  out[2] = p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43;
}

void Transform_Dir(const D3DVECTOR &p, const D3DMATRIX &m, float out[3]) {
  out[0] = p.x * m._11 + p.y * m._21 + p.z * m._31;
  out[1] = p.x * m._12 + p.y * m._22 + p.z * m._32;
  out[2] = p.x * m._13 + p.y * m._23 + p.z * m._33;
  float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  if (len > 1e-12f)
    for (int k = 0; k < 3; k++)
      out[k] /= len;
}

GLenum Blend_Factor(DWORD b) {
  switch (b) {
  case D3DBLEND_ZERO: return GL_ZERO;
  case D3DBLEND_ONE: return GL_ONE;
  case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
  case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
  case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
  case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
  case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
  case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
  case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
  case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
  case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
  default: return GL_ONE;
  }
}

GLenum Compare_Func(DWORD c) {
  switch (c) {
  case D3DCMP_NEVER: return GL_NEVER;
  case D3DCMP_LESS: return GL_LESS;
  case D3DCMP_EQUAL: return GL_EQUAL;
  case D3DCMP_LESSEQUAL: return GL_LEQUAL;
  case D3DCMP_GREATER: return GL_GREATER;
  case D3DCMP_NOTEQUAL: return GL_NOTEQUAL;
  case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
  default: return GL_ALWAYS;
  }
}

GLenum Stencil_Op(DWORD o) {
  switch (o) {
  case D3DSTENCILOP_ZERO: return GL_ZERO;
  case D3DSTENCILOP_REPLACE: return GL_REPLACE;
  case D3DSTENCILOP_INCRSAT: return GL_INCR;
  case D3DSTENCILOP_DECRSAT: return GL_DECR;
  case D3DSTENCILOP_INVERT: return GL_INVERT;
  case D3DSTENCILOP_INCR: return GL_INCR_WRAP;
  case D3DSTENCILOP_DECR: return GL_DECR_WRAP;
  default: return GL_KEEP;
  }
}

GLenum Address_Mode(DWORD a) {
  switch (a) {
  case D3DTADDRESS_MIRROR:
  case D3DTADDRESS_MIRRORONCE: return GL_MIRRORED_REPEAT;
  case D3DTADDRESS_CLAMP:
  case D3DTADDRESS_BORDER: return GL_CLAMP_TO_EDGE; // no border color in WebGL
  default: return GL_REPEAT;
  }
}

GLenum Prim_Mode(D3DPRIMITIVETYPE t, UINT prims, GLsizei &count) {
  switch (t) {
  case D3DPT_POINTLIST: count = (GLsizei)prims; return GL_POINTS;
  case D3DPT_LINELIST: count = (GLsizei)(prims * 2); return GL_LINES;
  case D3DPT_LINESTRIP: count = (GLsizei)(prims + 1); return GL_LINE_STRIP;
  case D3DPT_TRIANGLESTRIP: count = (GLsizei)(prims + 2); return GL_TRIANGLE_STRIP;
  case D3DPT_TRIANGLEFAN: count = (GLsizei)(prims + 2); return GL_TRIANGLE_FAN;
  default: count = (GLsizei)(prims * 3); return GL_TRIANGLES;
  }
}

#ifdef __EMSCRIPTEN__
// Applies a D3D gamma ramp to the canvas as an SVG component-transfer table.
EM_JS(void, Rebel_JS_Set_Gamma, (const uint16_t *ramp, int identity), {
  var canvas = Module.canvas || document.getElementById('canvas');
  if (!canvas) return;
  if (identity) { canvas.style.filter = ''; return; }
  var ns = 'http://www.w3.org/2000/svg';
  var svg = document.getElementById('rebel-gamma-svg');
  if (!svg) {
    svg = document.createElementNS(ns, 'svg');
    svg.id = 'rebel-gamma-svg';
    svg.setAttribute('width', '0');
    svg.setAttribute('height', '0');
    svg.style.position = 'absolute';
    svg.innerHTML = '<filter id="rebel-gamma" color-interpolation-filters="sRGB"><feComponentTransfer>' +
                    '<feFuncR type="table"/><feFuncG type="table"/><feFuncB type="table"/></feComponentTransfer></filter>';
    document.body.appendChild(svg);
  }
  var funcs = svg.querySelectorAll('feFuncR, feFuncG, feFuncB');
  for (var c = 0; c < 3; c++) {
    var vals = [];
    for (var i = 0; i < 256; i += 15) vals.push((HEAPU16[(ramp >> 1) + c * 256 + i] / 65535).toFixed(4));
    vals.push((HEAPU16[(ramp >> 1) + c * 256 + 255] / 65535).toFixed(4));
    funcs[c].setAttribute('tableValues', vals.join(' '));
  }
  canvas.style.filter = 'url(#rebel-gamma)';
});
#endif

} // namespace

/* ================================================================ Device */

Device::Device(IDirect3D9 *d3d, HWND window, D3DPRESENT_PARAMETERS *pp) : D3D(d3d), Window(window), Params(*pp) {
  D3D->AddRef();
#ifdef __EMSCRIPTEN__
  EmscriptenWebGLContextAttributes attr;
  emscripten_webgl_init_context_attributes(&attr);
  attr.majorVersion = 2;
  attr.minorVersion = 0;
  attr.alpha = false;
  attr.depth = false;
  attr.stencil = false;
  attr.antialias = false;
  attr.premultipliedAlpha = false;
  attr.preserveDrawingBuffer = false;
  attr.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &attr);
  if (ctx <= 0) {
    fprintf(stderr, "[d3d9] WebGL2 context creation failed (%d)\n", (int)ctx);
    return;
  }
  Context = (int)ctx;
  emscripten_webgl_make_context_current(ctx);
  S3tc = emscripten_webgl_enable_extension(ctx, "WEBGL_compressed_texture_s3tc");
  if (emscripten_webgl_enable_extension(ctx, "EXT_texture_filter_anisotropic"))
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &MaxAniso);
#endif
  if (Params.BackBufferWidth == 0)
    Params.BackBufferWidth = 800;
  if (Params.BackBufferHeight == 0)
    Params.BackBufferHeight = 600;
  if (Params.BackBufferFormat == D3DFMT_UNKNOWN)
    Params.BackBufferFormat = D3DFMT_X8R8G8B8;
  *pp = Params;

  gls::Invalidate();
  glGenVertexArrays(1, &Vao);
  gls::Bind_Vertex_Array(Vao);
  glGenBuffers(1, &ScratchVbo);
  glGenBuffers(1, &ScratchIbo);
  glFrontFace(GL_CCW);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  for (int i = 0; i < 4; i++)
    World[i] = Identity();
  View = Projection = Identity();
  for (D3DMATRIX &t : TexMatrix)
    t = Identity();
  for (int c = 0; c < 3; c++)
    for (int i = 0; i < 256; i++)
      Gamma[c][i] = (WORD)(i * 257);

  if (!Create_Back_Buffer())
    return;
  Set_Render_State_Defaults();
  Ready = true;
  fprintf(stderr, "[d3d9] WebGL2 device %ux%u, S3TC %s, anisotropy %.0f\n", Params.BackBufferWidth,
          Params.BackBufferHeight, S3tc ? "yes" : "no (software DXT decode)", MaxAniso);
}

Device::~Device() {
  for (Texture *&t : Textures)
    if (t) {
      t->Release();
      t = nullptr;
    }
  if (Stream)
    Stream->Release();
  if (Indices)
    Indices->Release();
  if (CurrentRT)
    CurrentRT->Release();
  if (CurrentDepth)
    CurrentDepth->Release();
  Destroy_Back_Buffer();
  Ffp_Clear_Cache();
  if (BlitProgram)
    glDeleteProgram(BlitProgram);
  gls::Forget_Buffer(ScratchVbo);
  gls::Forget_Buffer(ScratchIbo);
  glDeleteBuffers(1, &ScratchVbo);
  glDeleteBuffers(1, &ScratchIbo);
  glDeleteVertexArrays(1, &Vao);
#ifdef __EMSCRIPTEN__
  if (Context > 0)
    emscripten_webgl_destroy_context((EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)Context);
#endif
  D3D->Release();
}

bool Device::Create_Back_Buffer() {
#ifdef __EMSCRIPTEN__
  emscripten_set_canvas_element_size("#canvas", (int)Params.BackBufferWidth, (int)Params.BackBufferHeight);
#endif
  BackBuffer = new Surface(this, Surface::Kind::BackBuffer, Params.BackBufferWidth, Params.BackBufferHeight,
                           Params.BackBufferFormat, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET);
  if (Params.EnableAutoDepthStencil)
    AutoDepth = new Surface(this, Surface::Kind::DepthStencil, Params.BackBufferWidth, Params.BackBufferHeight,
                            Params.AutoDepthStencilFormat, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL);
  if (CurrentRT)
    CurrentRT->Release();
  CurrentRT = BackBuffer;
  CurrentRT->AddRef();
  if (CurrentDepth)
    CurrentDepth->Release();
  CurrentDepth = AutoDepth;
  if (CurrentDepth)
    CurrentDepth->AddRef();
  Viewport = {0, 0, Params.BackBufferWidth, Params.BackBufferHeight, 0.0f, 1.0f};
  return true;
}

void Device::Destroy_Back_Buffer() {
  if (BackBuffer)
    BackBuffer->Release();
  if (AutoDepth)
    AutoDepth->Release();
  BackBuffer = AutoDepth = nullptr;
}

void Device::Set_Render_State_Defaults() {
  DWORD *rs = RenderStates;
  rs[D3DRS_ZENABLE] = Params.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
  rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
  rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  rs[D3DRS_ZWRITEENABLE] = TRUE;
  rs[D3DRS_LASTPIXEL] = TRUE;
  rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  rs[D3DRS_CULLMODE] = D3DCULL_CCW;
  rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_FOGSTART] = D(0.0f);
  rs[D3DRS_FOGEND] = D(1.0f);
  rs[D3DRS_FOGDENSITY] = D(1.0f);
  rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
  rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
  rs[D3DRS_CLIPPING] = TRUE;
  rs[D3DRS_LIGHTING] = TRUE;
  rs[D3DRS_COLORVERTEX] = TRUE;
  rs[D3DRS_LOCALVIEWER] = TRUE;
  rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  rs[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  rs[D3DRS_POINTSIZE] = D(1.0f);
  rs[D3DRS_POINTSIZE_MIN] = D(1.0f);
  rs[D3DRS_POINTSCALE_A] = D(1.0f);
  rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
  rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
  rs[D3DRS_POINTSIZE_MAX] = D(64.0f);
  rs[D3DRS_COLORWRITEENABLE] = 0xF;
  rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  for (int s = 0; s < 8; s++) {
    DWORD *ts = StageStates[s];
    ts[D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    ts[D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    ts[D3DTSS_COLORARG1] = ts[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    ts[D3DTSS_COLORARG2] = ts[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    ts[D3DTSS_COLORARG0] = ts[D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
    ts[D3DTSS_RESULTARG] = D3DTA_CURRENT;
    ts[D3DTSS_TEXCOORDINDEX] = (DWORD)s;
    DWORD *ss = SamplerStates[s];
    ss[D3DSAMP_ADDRESSU] = ss[D3DSAMP_ADDRESSV] = ss[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    ss[D3DSAMP_MAGFILTER] = ss[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    ss[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    ss[D3DSAMP_MAXANISOTROPY] = 1;
  }
}

HRESULT Device::QueryInterface(REFIID, void **out) {
  *out = static_cast<IDirect3DDevice9 *>(this);
  AddRef();
  return S_OK;
}

ULONG Device::Release() {
  ULONG r = --Refs;
  if (r == 0)
    delete this;
  return r;
}

HRESULT Device::GetDeviceCaps(D3DCAPS9 *caps) { return D3D->GetDeviceCaps(0, D3DDEVTYPE_HAL, caps); }

HRESULT Device::GetDisplayMode(UINT, D3DDISPLAYMODE *mode) {
  mode->Width = Params.BackBufferWidth;
  mode->Height = Params.BackBufferHeight;
  mode->RefreshRate = 60;
  mode->Format = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

HRESULT Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **chain) {
  *chain = nullptr;
  return D3DERR_NOTAVAILABLE; // single canvas
}

HRESULT Device::Reset(D3DPRESENT_PARAMETERS *pp) {
  Params = *pp;
  if (Params.BackBufferWidth == 0)
    Params.BackBufferWidth = 800;
  if (Params.BackBufferHeight == 0)
    Params.BackBufferHeight = 600;
  if (Params.BackBufferFormat == D3DFMT_UNKNOWN)
    Params.BackBufferFormat = D3DFMT_X8R8G8B8;
  *pp = Params;
  Destroy_Back_Buffer();
  Create_Back_Buffer();
  Set_Render_State_Defaults();
  return D3D_OK;
}

HRESULT Device::Present(const RECT *, const RECT *, HWND, const void *) {
  // Back buffer (top row first) -> canvas (GL bottom row first): flipped blit.
  gls::Bind_Framebuffer(GL_READ_FRAMEBUFFER, BackBuffer->Framebuffer());
  gls::Bind_Framebuffer(GL_DRAW_FRAMEBUFFER, 0);
  GLint w = (GLint)Params.BackBufferWidth, h = (GLint)Params.BackBufferHeight;
  gls::Enable(GL_SCISSOR_TEST, false);
  glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  // The next draw or clear binds its own framebuffer.
  FrameCount++;
#ifdef __EMSCRIPTEN__
  // Frame timing: "work" is game time between frame yields, "wait" the time
  // the browser took to hand the frame back (vsync, GPU, other tasks).
  static double work_sum = 0, work_max = 0, wait_sum = 0, wait_max = 0, yield_end = 0;
  static unsigned frames = 0;
  double before = emscripten_get_now();
  if (yield_end > 0) {
    double work = before - yield_end;
    work_sum += work;
    work_max = work > work_max ? work : work_max;
  }
#endif
  Rebel_Last_Frame_Yield = GetTickCount();
  Rebel_Frame_Yield();
  Rebel_Last_Frame_Yield = GetTickCount();
#ifdef __EMSCRIPTEN__
  double now = emscripten_get_now();
  double wait = now - before;
  wait_sum += wait;
  wait_max = wait > wait_max ? wait : wait_max;
  yield_end = now;
  frames++;
  if (now - LastReport > 5000.0) {
    fprintf(stderr,
            "[d3d9] frame %u: %.1f fps, %u draws, %u prims, %u clears, %u failed programs; work %.1f ms avg (max %.0f), "
            "wait %.1f ms avg (max %.0f, audio %.1f ms avg), %u sleeps %.0f ms; uploads: %u buffers %.1f MB, %u textures %.1f MB\n",
            FrameCount, frames * 1000.0 / (now - LastReport), DrawCalls, Primitives, Clears, FailedPrograms,
            work_sum / frames, work_max, wait_sum / frames, wait_max, Rebel_Stat_Audio_Ms / frames, Rebel_Stat_Sleep_Count, Rebel_Stat_Sleep_Ms,
            Stat_Buffer_Uploads, Stat_Buffer_Bytes / 1048576.0, Stat_Texture_Uploads, Stat_Texture_Bytes / 1048576.0);
    Stat_Buffer_Bytes = Stat_Texture_Bytes = 0;
    Stat_Buffer_Uploads = Stat_Texture_Uploads = 0;
    DrawCalls = Primitives = Clears = FailedPrograms = 0;
    work_sum = work_max = wait_sum = wait_max = 0;
    frames = 0;
    Rebel_Stat_Sleep_Count = 0;
    Rebel_Stat_Sleep_Ms = 0;
    Rebel_Stat_Audio_Ms = 0;
    LastReport = now;
  }
#endif
  return D3D_OK;
}

HRESULT Device::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **surface) {
  *surface = BackBuffer;
  BackBuffer->AddRef();
  return D3D_OK;
}

void Device::SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *ramp) {
  bool identity = true;
  for (int i = 0; i < 256; i++) {
    Gamma[0][i] = ramp->red[i];
    Gamma[1][i] = ramp->green[i];
    Gamma[2][i] = ramp->blue[i];
    for (int c = 0; c < 3; c++)
      if (std::abs((int)Gamma[c][i] - i * 257) > 257)
        identity = false;
  }
  GammaIdentity = identity;
#ifdef __EMSCRIPTEN__
  Rebel_JS_Set_Gamma(&Gamma[0][0], identity ? 1 : 0);
#endif
}

HRESULT Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                              IDirect3DTexture9 **tex, HANDLE *) {
  if (!Format_Info(fmt).known || w == 0 || h == 0) {
    *tex = nullptr;
    return D3DERR_INVALIDCALL;
  }
  *tex = new Texture(this, w, h, levels, usage, fmt, pool);
  return D3D_OK;
}

HRESULT Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **vb,
                                   HANDLE *) {
  *vb = new VertexBuffer(this, length, usage, fvf, pool);
  return D3D_OK;
}

HRESULT Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **ib,
                                  HANDLE *) {
  *ib = new IndexBuffer(this, length, usage, fmt, pool);
  return D3D_OK;
}

HRESULT Device::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                   IDirect3DSurface9 **surface, HANDLE *) {
  *surface = new Surface(this, Surface::Kind::RenderTarget, w, h, fmt, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET);
  return D3D_OK;
}

HRESULT Device::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                          IDirect3DSurface9 **surface, HANDLE *) {
  *surface = new Surface(this, Surface::Kind::DepthStencil, w, h, fmt, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL);
  return D3D_OK;
}

HRESULT Device::UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) {
  Texture *s = static_cast<Texture *>(static_cast<IDirect3DTexture9 *>(src));
  Texture *d = static_cast<Texture *>(static_cast<IDirect3DTexture9 *>(dst));
  if (!s || !d || s->Format != d->Format)
    return D3DERR_INVALIDCALL;
  size_t n = std::min(s->Levels.size(), d->Levels.size());
  for (size_t i = 0; i < n; i++) {
    if (s->Levels[i].bits.size() != d->Levels[i].bits.size())
      return D3DERR_INVALIDCALL;
    d->Levels[i].bits = s->Levels[i].bits;
    d->Levels[i].dirty = true;
  }
  d->AnyDirty = true;
  return D3D_OK;
}

HRESULT Device::GetFrontBufferData(UINT, IDirect3DSurface9 *dst) {
  return Copy_Surface(static_cast<Surface *>(dst), nullptr, BackBuffer, nullptr, false);
}

HRESULT Device::StretchRect(IDirect3DSurface9 *src, const RECT *src_rect, IDirect3DSurface9 *dst, const RECT *dst_rect,
                            D3DTEXTUREFILTERTYPE filter) {
  Surface *s = static_cast<Surface *>(src), *d = static_cast<Surface *>(dst);
  auto gpu = [](Surface *x) {
    return x->SurfaceKind == Surface::Kind::BackBuffer || x->SurfaceKind == Surface::Kind::RenderTarget ||
           (x->SurfaceKind == Surface::Kind::TextureLevel && x->Owner && x->Owner->Is_Render_Target());
  };
  if (gpu(s) && gpu(d)) {
    RECT sr = src_rect ? *src_rect : RECT{0, 0, (LONG)s->Width, (LONG)s->Height};
    RECT dr = dst_rect ? *dst_rect : RECT{0, 0, (LONG)d->Width, (LONG)d->Height};
    GLuint sf = s->Framebuffer(), df = d->Framebuffer();
    gls::Bind_Framebuffer(GL_READ_FRAMEBUFFER, sf);
    gls::Bind_Framebuffer(GL_DRAW_FRAMEBUFFER, df);
    gls::Enable(GL_SCISSOR_TEST, false);
    glBlitFramebuffer(sr.left, sr.top, sr.right, sr.bottom, dr.left, dr.top, dr.right, dr.bottom, GL_COLOR_BUFFER_BIT,
                      filter == D3DTEXF_POINT || filter == D3DTEXF_NONE ? GL_NEAREST : GL_LINEAR);
    if (d->Owner)
      d->Owner->GpuNewer = true;
    return D3D_OK;
  }
  return Copy_Surface(d, dst_rect, s, src_rect, filter == D3DTEXF_LINEAR);
}

HRESULT Device::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **surface,
                                            HANDLE *) {
  *surface = new Surface(this, Surface::Kind::Offscreen, w, h, fmt, pool, 0);
  return D3D_OK;
}

HRESULT Device::SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) {
  if (index != 0)
    return D3DERR_INVALIDCALL;
  Surface *s = surface ? static_cast<Surface *>(surface) : BackBuffer;
  s->AddRef();
  if (CurrentRT)
    CurrentRT->Release();
  CurrentRT = s;
  // D3D9 resets the viewport to the full new target
  Viewport = {0, 0, s->Width, s->Height, 0.0f, 1.0f};
  return D3D_OK;
}

HRESULT Device::GetRenderTarget(DWORD index, IDirect3DSurface9 **surface) {
  if (index != 0 || !CurrentRT) {
    *surface = nullptr;
    return D3DERR_NOTFOUND;
  }
  *surface = CurrentRT;
  CurrentRT->AddRef();
  return D3D_OK;
}

HRESULT Device::SetDepthStencilSurface(IDirect3DSurface9 *surface) {
  Surface *s = static_cast<Surface *>(surface);
  if (s)
    s->AddRef();
  if (CurrentDepth)
    CurrentDepth->Release();
  CurrentDepth = s;
  return D3D_OK;
}

HRESULT Device::GetDepthStencilSurface(IDirect3DSurface9 **surface) {
  *surface = CurrentDepth;
  if (!CurrentDepth)
    return D3DERR_NOTFOUND;
  CurrentDepth->AddRef();
  return D3D_OK;
}

// One framebuffer per color surface; the current depth surface is attached
// on demand (GLES3 allows attachments of different sizes).
void Device::Bind_Framebuffer_For(Surface *color) {
  GLuint fbo = color->Framebuffer();
  gls::Bind_Framebuffer(GL_FRAMEBUFFER, fbo);
  // Track the attachment ourselves: querying GL here stalls the pipeline.
  GLuint want = CurrentDepth ? CurrentDepth->Renderbuffer : 0;
  if (color->AttachedDepth != want) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, want);
    color->AttachedDepth = want;
  }
  if (color->Owner)
    color->Owner->GpuNewer = true;
}

HRESULT Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
  Bind_Framebuffer_For(CurrentRT);
  GLbitfield mask = 0;
  if (flags & D3DCLEAR_TARGET) {
    float c[4];
    Color_To_Vec4(color, c);
    glClearColor(c[0], c[1], c[2], c[3]);
    gls::Color_Mask(true, true, true, true);
    mask |= GL_COLOR_BUFFER_BIT;
  }
  if ((flags & D3DCLEAR_ZBUFFER) && CurrentDepth) {
    glClearDepthf(z);
    gls::Depth_Mask(true);
    mask |= GL_DEPTH_BUFFER_BIT;
  }
  if ((flags & D3DCLEAR_STENCIL) && CurrentDepth) {
    glClearStencil((GLint)stencil);
    gls::Stencil_Mask(0xFF);
    mask |= GL_STENCIL_BUFFER_BIT;
  }
  if (!mask)
    return D3D_OK;
  Clears++;
  gls::Enable(GL_SCISSOR_TEST, true);
  if (count && rects) {
    for (DWORD i = 0; i < count; i++) {
      gls::Scissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
      glClear(mask);
    }
  } else {
    gls::Scissor((GLint)Viewport.X, (GLint)Viewport.Y, (GLsizei)Viewport.Width, (GLsizei)Viewport.Height);
    glClear(mask);
  }
  gls::Enable(GL_SCISSOR_TEST, false);
  return D3D_OK;
}

HRESULT Device::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *m) {
  DWORD s = (DWORD)state;
  if (s >= 256 && s < 260)
    World[s - 256] = *m;
  else if (s == D3DTS_VIEW)
    View = *m;
  else if (s == D3DTS_PROJECTION)
    Projection = *m;
  else if (s >= D3DTS_TEXTURE0 && s <= D3DTS_TEXTURE7)
    TexMatrix[s - D3DTS_TEXTURE0] = *m;
  return D3D_OK;
}

HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) {
  DWORD s = (DWORD)state;
  if (s >= 256 && s < 260)
    *m = World[s - 256];
  else if (s == D3DTS_VIEW)
    *m = View;
  else if (s == D3DTS_PROJECTION)
    *m = Projection;
  else if (s >= D3DTS_TEXTURE0 && s <= D3DTS_TEXTURE7)
    *m = TexMatrix[s - D3DTS_TEXTURE0];
  else
    *m = Identity();
  return D3D_OK;
}

HRESULT Device::SetViewport(const D3DVIEWPORT9 *vp) {
  Viewport = *vp;
  return D3D_OK;
}
HRESULT Device::GetViewport(D3DVIEWPORT9 *vp) {
  *vp = Viewport;
  return D3D_OK;
}
HRESULT Device::SetMaterial(const D3DMATERIAL9 *mat) {
  Material = *mat;
  return D3D_OK;
}
HRESULT Device::GetMaterial(D3DMATERIAL9 *mat) {
  *mat = Material;
  return D3D_OK;
}
HRESULT Device::SetLight(DWORD index, const D3DLIGHT9 *light) {
  if (index >= 8)
    return D3D_OK; // the engine never uses more than 4
  Lights[index] = *light;
  return D3D_OK;
}
HRESULT Device::GetLight(DWORD index, D3DLIGHT9 *light) {
  if (index >= 8)
    return D3DERR_INVALIDCALL;
  *light = Lights[index];
  return D3D_OK;
}
HRESULT Device::LightEnable(DWORD index, BOOL enable) {
  if (index < 8)
    LightOn[index] = enable != 0;
  return D3D_OK;
}
HRESULT Device::SetClipPlane(DWORD, const float *) { return D3D_OK; } // user clip planes: unsupported

HRESULT Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
  if ((DWORD)state < REBEL_D3D_RENDERSTATE_COUNT)
    RenderStates[state] = value;
  return D3D_OK;
}
HRESULT Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) {
  *value = (DWORD)state < REBEL_D3D_RENDERSTATE_COUNT ? RenderStates[state] : 0;
  return D3D_OK;
}

HRESULT Device::GetTexture(DWORD stage, IDirect3DBaseTexture9 **tex) {
  *tex = stage < 8 ? Textures[stage] : nullptr;
  if (*tex)
    (*tex)->AddRef();
  return D3D_OK;
}

HRESULT Device::SetTexture(DWORD stage, IDirect3DBaseTexture9 *tex) {
  if (stage >= 8)
    return D3D_OK;
  Texture *t = tex ? static_cast<Texture *>(static_cast<IDirect3DTexture9 *>(tex)) : nullptr;
  if (t)
    t->AddRef();
  if (Textures[stage])
    Textures[stage]->Release();
  Textures[stage] = t;
  return D3D_OK;
}

HRESULT Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) {
  *value = (stage < 8 && (DWORD)type < REBEL_D3D_TSS_COUNT) ? StageStates[stage][type] : 0;
  return D3D_OK;
}

HRESULT Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (stage >= 8 || (DWORD)type >= REBEL_D3D_TSS_COUNT)
    return D3D_OK;
  StageStates[stage][type] = value;
  // DX8-era sampler states set through the stage API
  switch (type) {
  case D3DTSS_ADDRESSU: SamplerStates[stage][D3DSAMP_ADDRESSU] = value; break;
  case D3DTSS_ADDRESSV: SamplerStates[stage][D3DSAMP_ADDRESSV] = value; break;
  case D3DTSS_ADDRESSW: SamplerStates[stage][D3DSAMP_ADDRESSW] = value; break;
  case D3DTSS_MAGFILTER: SamplerStates[stage][D3DSAMP_MAGFILTER] = value; break;
  case D3DTSS_MINFILTER: SamplerStates[stage][D3DSAMP_MINFILTER] = value; break;
  case D3DTSS_MIPFILTER: SamplerStates[stage][D3DSAMP_MIPFILTER] = value; break;
  case D3DTSS_MAXMIPLEVEL: SamplerStates[stage][D3DSAMP_MAXMIPLEVEL] = value; break;
  case D3DTSS_MAXANISOTROPY: SamplerStates[stage][D3DSAMP_MAXANISOTROPY] = value; break;
  default: break;
  }
  return D3D_OK;
}

HRESULT Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) {
  *value = (sampler < 8 && (DWORD)type < REBEL_D3D_SAMP_COUNT) ? SamplerStates[sampler][type] : 0;
  return D3D_OK;
}

HRESULT Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  if (sampler < 8 && (DWORD)type < REBEL_D3D_SAMP_COUNT)
    SamplerStates[sampler][type] = value;
  return D3D_OK;
}

HRESULT Device::ValidateDevice(DWORD *passes) {
  if (passes)
    *passes = 1;
  return D3D_OK;
}

HRESULT Device::SetFVF(DWORD fvf) {
  Fvf = fvf;
  return D3D_OK;
}
HRESULT Device::GetFVF(DWORD *fvf) {
  *fvf = Fvf;
  return D3D_OK;
}

// DX8 style: a small value is an FVF; NULL means "use SetFVF".
HRESULT Device::SetVertexShader(void *shader) {
  if (shader && (ULONG_PTR)shader < 0x10000)
    Fvf = (DWORD)(ULONG_PTR)shader;
  return D3D_OK;
}

HRESULT Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) {
  if (stream != 0)
    return D3D_OK;
  VertexBuffer *v = static_cast<VertexBuffer *>(vb);
  if (v)
    v->AddRef();
  if (Stream)
    Stream->Release();
  Stream = v;
  StreamOffset = offset;
  StreamStride = stride;
  return D3D_OK;
}

HRESULT Device::SetIndices(IDirect3DIndexBuffer9 *ib) {
  IndexBuffer *i = static_cast<IndexBuffer *>(ib);
  if (i)
    i->AddRef();
  if (Indices)
    Indices->Release();
  Indices = i;
  return D3D_OK;
}

/* ------------------------------------------------------------- drawing */

FfpKey Device::Build_Key(const FvfLayout &l) const {
  FfpKey k;
  const DWORD *rs = RenderStates;
  k.fvf = Fvf;
  k.lighting = (rs[D3DRS_LIGHTING] && !l.rhw) ? 1 : 0;
  k.specular = rs[D3DRS_SPECULARENABLE] ? 1 : 0;
  if (k.lighting) {
    k.local_viewer = rs[D3DRS_LOCALVIEWER] ? 1 : 0;
    k.color_vertex = rs[D3DRS_COLORVERTEX] ? 1 : 0;
    k.diffuse_src = (uint8_t)rs[D3DRS_DIFFUSEMATERIALSOURCE];
    k.ambient_src = (uint8_t)rs[D3DRS_AMBIENTMATERIALSOURCE];
    k.specular_src = (uint8_t)rs[D3DRS_SPECULARMATERIALSOURCE];
    k.emissive_src = (uint8_t)rs[D3DRS_EMISSIVEMATERIALSOURCE];
    for (int i = 0; i < 8; i++)
      k.light_types[i] = LightOn[i] ? (uint8_t)Lights[i].Type : 0;
  }
  k.normalize = rs[D3DRS_NORMALIZENORMALS] ? 1 : 0;
  if (rs[D3DRS_FOGENABLE]) {
    if (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE) {
      k.fog = 2;
      k.fog_mode = (uint8_t)rs[D3DRS_FOGTABLEMODE];
    } else if (rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE) {
      k.fog = 1;
      k.fog_mode = (uint8_t)rs[D3DRS_FOGVERTEXMODE];
    } else if (l.rhw || l.specular >= 0) {
      k.fog = 1; // application-supplied fog in specular alpha
      k.fog_mode = D3DFOG_NONE;
    }
    k.range_fog = rs[D3DRS_RANGEFOGENABLE] ? 1 : 0;
  }
  k.alpha_test = rs[D3DRS_ALPHATESTENABLE] ? (uint8_t)rs[D3DRS_ALPHAFUNC] : 0;
  k.flat = rs[D3DRS_SHADEMODE] == D3DSHADE_FLAT ? 1 : 0;
  k.blend_weights = 1;
  DWORD vb = rs[D3DRS_VERTEXBLEND];
  if (!l.rhw && vb >= D3DVBF_1WEIGHTS && vb <= D3DVBF_3WEIGHTS && (int)vb <= l.weights)
    k.blend_weights = (uint8_t)(vb + 1);
  for (int s = 0; s < 8; s++) {
    const DWORD *ts = StageStates[s];
    if (ts[D3DTSS_COLOROP] == D3DTOP_DISABLE)
      break;
    FfpKey::Stage &st = k.stages[s];
    st.color_op = (uint8_t)ts[D3DTSS_COLOROP];
    st.color_arg0 = (uint8_t)ts[D3DTSS_COLORARG0];
    st.color_arg1 = (uint8_t)ts[D3DTSS_COLORARG1];
    st.color_arg2 = (uint8_t)ts[D3DTSS_COLORARG2];
    st.alpha_op = (uint8_t)ts[D3DTSS_ALPHAOP];
    st.alpha_arg0 = (uint8_t)ts[D3DTSS_ALPHAARG0];
    st.alpha_arg1 = (uint8_t)ts[D3DTSS_ALPHAARG1];
    st.alpha_arg2 = (uint8_t)ts[D3DTSS_ALPHAARG2];
    st.result_arg = (uint8_t)ts[D3DTSS_RESULTARG];
    st.coord_index = (uint8_t)(ts[D3DTSS_TEXCOORDINDEX] & 0xFF);
    switch (ts[D3DTSS_TEXCOORDINDEX] & 0xFFFF0000) {
    case D3DTSS_TCI_CAMERASPACENORMAL: st.tci = 1; break;
    case D3DTSS_TCI_CAMERASPACEPOSITION: st.tci = 2; break;
    case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: st.tci = 3; break;
    default: st.tci = 0; break;
    }
    DWORD tt = ts[D3DTSS_TEXTURETRANSFORMFLAGS];
    st.ttff = (uint8_t)((tt & 0xFF) | ((tt & D3DTTFF_PROJECTED) ? 0x80 : 0));
    if (st.ttff & 0x80)
      st.ttff = (uint8_t)((st.ttff & 0x7F) | D3DTTFF_PROJECTED); // keep the flag in the byte
    st.has_texture = Textures[s] ? 1 : 0;
    k.num_stages = (uint8_t)(s + 1);
  }
  return k;
}

void Device::Apply_Pipeline_State() {
  const DWORD *rs = RenderStates;
  Bind_Framebuffer_For(CurrentRT);
  gls::Viewport((GLint)Viewport.X, (GLint)Viewport.Y, (GLsizei)Viewport.Width, (GLsizei)Viewport.Height);
  gls::Depth_Range(Viewport.MinZ, Viewport.MaxZ);

  if (rs[D3DRS_ALPHABLENDENABLE]) {
    gls::Enable(GL_BLEND, true);
    GLenum src = Blend_Factor(rs[D3DRS_SRCBLEND]), dst = Blend_Factor(rs[D3DRS_DESTBLEND]);
    if (rs[D3DRS_SRCBLEND] == D3DBLEND_BOTHSRCALPHA) {
      src = GL_SRC_ALPHA;
      dst = GL_ONE_MINUS_SRC_ALPHA;
    } else if (rs[D3DRS_SRCBLEND] == D3DBLEND_BOTHINVSRCALPHA) {
      src = GL_ONE_MINUS_SRC_ALPHA;
      dst = GL_SRC_ALPHA;
    }
    gls::Blend_Func(src, dst);
    switch (rs[D3DRS_BLENDOP]) {
    case D3DBLENDOP_SUBTRACT: gls::Blend_Equation(GL_FUNC_SUBTRACT); break;
    case D3DBLENDOP_REVSUBTRACT: gls::Blend_Equation(GL_FUNC_REVERSE_SUBTRACT); break;
    case D3DBLENDOP_MIN: gls::Blend_Equation(GL_MIN); break;
    case D3DBLENDOP_MAX: gls::Blend_Equation(GL_MAX); break;
    default: gls::Blend_Equation(GL_FUNC_ADD); break;
    }
  } else {
    gls::Enable(GL_BLEND, false);
  }

  if (rs[D3DRS_ZENABLE] != D3DZB_FALSE && CurrentDepth) {
    gls::Enable(GL_DEPTH_TEST, true);
    gls::Depth_Func(Compare_Func(rs[D3DRS_ZFUNC]));
  } else {
    gls::Enable(GL_DEPTH_TEST, false);
  }
  gls::Depth_Mask(rs[D3DRS_ZWRITEENABLE] && CurrentDepth);

  if (rs[D3DRS_STENCILENABLE] && CurrentDepth) {
    gls::Enable(GL_STENCIL_TEST, true);
    gls::Stencil_Func(Compare_Func(rs[D3DRS_STENCILFUNC]), (GLint)rs[D3DRS_STENCILREF], rs[D3DRS_STENCILMASK]);
    gls::Stencil_Op(Stencil_Op(rs[D3DRS_STENCILFAIL]), Stencil_Op(rs[D3DRS_STENCILZFAIL]), Stencil_Op(rs[D3DRS_STENCILPASS]));
    gls::Stencil_Mask(rs[D3DRS_STENCILWRITEMASK]);
  } else {
    gls::Enable(GL_STENCIL_TEST, false);
  }

  switch (rs[D3DRS_CULLMODE]) {
  case D3DCULL_CW:
    gls::Enable(GL_CULL_FACE, true);
    gls::Cull_Face(GL_FRONT);
    break;
  case D3DCULL_CCW:
    gls::Enable(GL_CULL_FACE, true);
    gls::Cull_Face(GL_BACK);
    break;
  default: gls::Enable(GL_CULL_FACE, false); break;
  }

  DWORD cw = rs[D3DRS_COLORWRITEENABLE];
  gls::Color_Mask((cw & D3DCOLORWRITEENABLE_RED) != 0, (cw & D3DCOLORWRITEENABLE_GREEN) != 0,
              (cw & D3DCOLORWRITEENABLE_BLUE) != 0, (cw & D3DCOLORWRITEENABLE_ALPHA) != 0);

  // Depth bias: D3D9 float bias (depth units) or the DX8 integer ZBIAS.
  float bias = F(rs[D3DRS_DEPTHBIAS]);
  float slope = F(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
  float units = -bias * 16777215.0f - (float)rs[D3DRS_ZBIAS];
  if (units != 0.0f || slope != 0.0f) {
    gls::Enable(GL_POLYGON_OFFSET_FILL, true);
    gls::Polygon_Offset(-slope, units);
  } else {
    gls::Enable(GL_POLYGON_OFFSET_FILL, false);
  }
}

void Device::Apply_Samplers(FfpProgram *) {
  for (int s = 0; s < 8; s++) {
    Texture *t = Textures[s];
    if (!t) {
      gls::Bind_Texture_Unit(s, 0);
      continue;
    }
    t->Upload();
    gls::Bind_Texture_Unit(s, t->Gl);
    // Parameter calls apply to the active unit's texture: select it first.
    auto unit = [&] { gls::Active_Texture(GL_TEXTURE0 + s); };
    const DWORD *ss = SamplerStates[s];
    bool mips = t->Levels.size() > 1 && ss[D3DSAMP_MIPFILTER] != D3DTEXF_NONE;
    bool min_linear = ss[D3DSAMP_MINFILTER] != D3DTEXF_POINT && ss[D3DSAMP_MINFILTER] != D3DTEXF_NONE;
    bool mip_linear = ss[D3DSAMP_MIPFILTER] == D3DTEXF_LINEAR;
    GLint min = mips ? (min_linear ? (mip_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                   : (mip_linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST))
                     : (min_linear ? GL_LINEAR : GL_NEAREST);
    GLint mag = (ss[D3DSAMP_MAGFILTER] == D3DTEXF_POINT || ss[D3DSAMP_MAGFILTER] == D3DTEXF_NONE) ? GL_NEAREST : GL_LINEAR;
    GLint ws = (GLint)Address_Mode(ss[D3DSAMP_ADDRESSU]), wt = (GLint)Address_Mode(ss[D3DSAMP_ADDRESSV]);
    float aniso = (ss[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC && MaxAniso > 1.0f)
                      ? std::min((float)std::max<DWORD>(ss[D3DSAMP_MAXANISOTROPY], 1), MaxAniso)
                      : 1.0f;
    int max_level = (int)t->Levels.size() - 1;
    if (t->Applied.min != min)
      unit(), glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->Applied.min = min);
    if (t->Applied.mag != mag)
      unit(), glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->Applied.mag = mag);
    if (t->Applied.wrap_s != ws)
      unit(), glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t->Applied.wrap_s = ws);
    if (t->Applied.wrap_t != wt)
      unit(), glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t->Applied.wrap_t = wt);
    if (t->Applied.max_level != max_level) {
      unit();
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->Applied.max_level = max_level);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL,
                      (GLint)std::min<DWORD>(std::max(ss[D3DSAMP_MAXMIPLEVEL], t->Lod), (DWORD)max_level));
    }
    if (MaxAniso > 1.0f && t->Applied.aniso != aniso) {
      unit();
      glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
      t->Applied.aniso = aniso;
    }
  }
}

// Uniform uploads skip values the program already has (per location).
static bool Uniform_Unchanged(FfpProgram *p, GLint loc, const float *v, int n) {
  if (!p->uniform_cache)
    p->uniform_cache = new std::unordered_map<GLint, std::array<float, 16>>();
  auto it = p->uniform_cache->find(loc);
  if (it != p->uniform_cache->end() && memcmp(it->second.data(), v, n * sizeof(float)) == 0)
    return true;
  std::array<float, 16> &slot = (*p->uniform_cache)[loc];
  memcpy(slot.data(), v, n * sizeof(float));
  return false;
}
static void UM(FfpProgram *p, GLint loc, int n, const float *m) {
  if (loc < 0 || Uniform_Unchanged(p, loc, m, n))
    return;
  if (n == 16)
    glUniformMatrix4fv(loc, 1, GL_FALSE, m);
  else if (n == 9)
    glUniformMatrix3fv(loc, 1, GL_FALSE, m);
  else
    glUniformMatrix2fv(loc, 1, GL_FALSE, m);
}
static void UV(FfpProgram *p, GLint loc, int n, const float *v) {
  if (loc < 0 || Uniform_Unchanged(p, loc, v, n))
    return;
  if (n == 4)
    glUniform4fv(loc, 1, v);
  else
    glUniform3fv(loc, 1, v);
}
static void UFn(FfpProgram *p, GLint loc, const float *v, int n) {
  if (loc < 0 || Uniform_Unchanged(p, loc, v, n))
    return;
  switch (n) {
  case 1: glUniform1f(loc, v[0]); break;
  case 2: glUniform2f(loc, v[0], v[1]); break;
  case 3: glUniform3f(loc, v[0], v[1], v[2]); break;
  default: glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
  }
}
static void UF1(FfpProgram *p, GLint loc, float x) { UFn(p, loc, &x, 1); }
static void UF2(FfpProgram *p, GLint loc, float x, float y) { float v[2] = {x, y}; UFn(p, loc, v, 2); }
static void UF3(FfpProgram *p, GLint loc, float x, float y, float z) { float v[3] = {x, y, z}; UFn(p, loc, v, 3); }
static void UF4(FfpProgram *p, GLint loc, float x, float y, float z, float w) {
  float v[4] = {x, y, z, w};
  UFn(p, loc, v, 4);
}

void Device::Upload_Uniforms(FfpProgram *p, const FvfLayout &) {
  const DWORD *rs = RenderStates;
  for (int i = 0; i < 4; i++)
    if (p->u_world[i] >= 0)
      UM(p, p->u_world[i], 16, &World[i].m[0][0]);
  if (p->u_view >= 0)
    UM(p, p->u_view, 16, &View.m[0][0]);
  if (p->u_proj >= 0)
    UM(p, p->u_proj, 16, &Projection.m[0][0]);
  if (p->u_normal_matrix >= 0) {
    float nm[9];
    Normal_Matrix(Multiply(World[0], View), nm);
    UM(p, p->u_normal_matrix, 9, nm);
  }
  if (p->u_viewport >= 0)
    UF4(p, p->u_viewport, (float)Viewport.X, (float)Viewport.Y, (float)std::max<DWORD>(Viewport.Width, 1),
                (float)std::max<DWORD>(Viewport.Height, 1));
  if (p->u_point_size >= 0)
    UF1(p, p->u_point_size, F(rs[D3DRS_POINTSIZE]));
  if (p->u_mat_diffuse >= 0)
    UV(p, p->u_mat_diffuse, 4, &Material.Diffuse.r);
  if (p->u_mat_ambient >= 0)
    UV(p, p->u_mat_ambient, 4, &Material.Ambient.r);
  if (p->u_mat_specular >= 0)
    UV(p, p->u_mat_specular, 4, &Material.Specular.r);
  if (p->u_mat_emissive >= 0)
    UV(p, p->u_mat_emissive, 4, &Material.Emissive.r);
  if (p->u_mat_power >= 0)
    UF1(p, p->u_mat_power, Material.Power);
  if (p->u_ambient >= 0) {
    float c[4];
    Color_To_Vec4(rs[D3DRS_AMBIENT], c);
    UV(p, p->u_ambient, 4, c);
  }
  for (int i = 0; i < 8; i++) {
    if (p->u_light_diffuse[i] < 0)
      continue;
    const D3DLIGHT9 &L = Lights[i];
    UV(p, p->u_light_diffuse[i], 4, &L.Diffuse.r);
    UV(p, p->u_light_specular[i], 4, &L.Specular.r);
    UV(p, p->u_light_ambient[i], 4, &L.Ambient.r);
    float pos[3], dir[3];
    Transform_Point(L.Position, View, pos);
    Transform_Dir(L.Direction, View, dir);
    UV(p, p->u_light_position[i], 3, pos);
    UV(p, p->u_light_direction[i], 3, dir);
    UF4(p, p->u_light_atten[i], L.Attenuation0, L.Attenuation1, L.Attenuation2, L.Range > 0 ? L.Range : 1e30f);
    UF3(p, p->u_light_spot[i], std::cos(L.Theta * 0.5f), std::cos(L.Phi * 0.5f), L.Falloff);
  }
  if (p->u_fog_color >= 0) {
    float c[4];
    Color_To_Vec4(rs[D3DRS_FOGCOLOR], c);
    UV(p, p->u_fog_color, 4, c);
  }
  if (p->u_fog_params >= 0)
    UF4(p, p->u_fog_params, F(rs[D3DRS_FOGSTART]), F(rs[D3DRS_FOGEND]), F(rs[D3DRS_FOGDENSITY]), 0.0f);
  if (p->u_tfactor >= 0) {
    float c[4];
    Color_To_Vec4(rs[D3DRS_TEXTUREFACTOR], c);
    UV(p, p->u_tfactor, 4, c);
  }
  if (p->u_alpha_ref >= 0)
    UF1(p, p->u_alpha_ref, (rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f);
  for (int s = 0; s < 8; s++) {
    if (p->u_tex_matrix[s] >= 0)
      UM(p, p->u_tex_matrix[s], 16, &TexMatrix[s].m[0][0]);
    if (p->u_bump_mat[s] >= 0) {
      const DWORD *ts = StageStates[s];
      float m[4] = {F(ts[D3DTSS_BUMPENVMAT00]), F(ts[D3DTSS_BUMPENVMAT01]), F(ts[D3DTSS_BUMPENVMAT10]),
                    F(ts[D3DTSS_BUMPENVMAT11])};
      UM(p, p->u_bump_mat[s], 4, m);
    }
    if (p->u_bump_lum[s] >= 0)
      UF2(p, p->u_bump_lum[s], F(StageStates[s][D3DTSS_BUMPENVLSCALE]), F(StageStates[s][D3DTSS_BUMPENVLOFFSET]));
  }
}

bool Device::Prepare_Draw(UINT base_bytes, GLuint vbo, UINT stride, DWORD fvf) {
  (void)fvf;
  FvfLayout l = Fvf_Layout(Fvf);
  if (!stride)
    stride = l.stride;
  FfpKey key = Build_Key(l);
  FfpProgram *p = Ffp_Program_For(key);
  if (!p) {
    FailedPrograms++;
    return false;
  }
  DrawCalls++;
  gls::Use_Program(p->program);
  Apply_Pipeline_State();
  Apply_Samplers(p);
  Upload_Uniforms(p, l);

  gls::Bind_Vertex_Array(Vao);
  gls::Bind_Buffer(GL_ARRAY_BUFFER, vbo);
  uint32_t enabled = 0;
  auto attr = [&](GLint loc, int size, GLenum type, bool norm, int offset) {
    if (loc < 0 || offset < 0)
      return;
    enabled |= 1u << loc;
    gls::Attrib_Pointer((GLuint)loc, size, type, norm, (GLsizei)stride, (uintptr_t)(base_bytes + (UINT)offset));
  };
  attr(p->a_position, l.position_size, GL_FLOAT, false, l.position);
  if (l.weights)
    attr(p->a_weights, std::min(l.weights, 4), GL_FLOAT, false, l.blend);
  attr(p->a_normal, 3, GL_FLOAT, false, l.normal);
  attr(p->a_diffuse, 4, GL_UNSIGNED_BYTE, true, l.diffuse);
  attr(p->a_specular, 4, GL_UNSIGNED_BYTE, true, l.specular);
  for (int i = 0; i < l.tex_count; i++)
    attr(p->a_texcoord[i], l.tex_size[i], GL_FLOAT, false, l.tex_offset[i]);
  gls::Attrib_Arrays(enabled);
  return true;
}

// Dynamic buffers (D3DUSAGE_DYNAMIC: WW3D's shared 5000-vertex / 5000-index
// streaming buffers for text, UI, particles...) are never updated in place
// on the GPU. The game rewrites parts of them between draws, and a partial
// update of a buffer that queued draws still read makes ANGLE copy the whole
// buffer. Instead each draw uploads just the range it uses into a scratch
// buffer, replacing its storage (glBufferData), which never has to wait.
UINT Device::Stream_Range(UINT first_byte, UINT bytes) {
  const std::vector<uint8_t> &d = Stream->Data;
  if (first_byte > d.size())
    first_byte = (UINT)d.size();
  bytes = std::min<UINT>(bytes, (UINT)d.size() - first_byte);
  gls::Bind_Buffer(GL_ARRAY_BUFFER, ScratchVbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)std::max<UINT>(bytes, 4), d.data() + first_byte, GL_STREAM_DRAW);
  Stat_Buffer_Bytes += bytes;
  Stat_Buffer_Uploads++;
  return bytes;
}

HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) {
  if (!Stream)
    return D3DERR_INVALIDCALL;
  UINT stride = StreamStride ? StreamStride : Fvf_Layout(Fvf).stride;
  GLsizei n;
  GLenum mode = Prim_Mode(type, count, n);
  if (Stream->Usage & D3DUSAGE_DYNAMIC) {
    Stream_Range(StreamOffset + start * stride, (UINT)n * stride);
    if (!Prepare_Draw(0, ScratchVbo, stride, Fvf))
      return D3D_OK;
    Primitives += (unsigned)n;
    glDrawArrays(mode, 0, n);
    return D3D_OK;
  }
  Stream->Upload();
  if (!Prepare_Draw(StreamOffset, Stream->Gl, stride, Fvf))
    return D3D_OK;
  Primitives += (unsigned)n;
  glDrawArrays(mode, (GLint)start, n);
  return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base_vertex, UINT min_index, UINT num_vertices,
                                     UINT start_index, UINT prim_count) {
  if (!Stream || !Indices)
    return D3DERR_INVALIDCALL;
  UINT stride = StreamStride ? StreamStride : Fvf_Layout(Fvf).stride;
  // WebGL has no base-vertex draws: offset the attribute pointers instead.
  INT base = (INT)StreamOffset + base_vertex * (INT)stride;
  if (base < 0)
    return D3DERR_INVALIDCALL;
  GLsizei n;
  GLenum mode = Prim_Mode(type, prim_count, n);
  bool i32 = Indices->Format == D3DFMT_INDEX32;
  UINT isize = i32 ? 4u : 2u;
  bool ok;
  if (Stream->Usage & D3DUSAGE_DYNAMIC) {
    // Vertices from the index space's vertex 0 (base) up to the last used one.
    Stream_Range((UINT)base, (min_index + num_vertices) * stride);
    ok = Prepare_Draw(0, ScratchVbo, stride, Fvf);
  } else {
    Stream->Upload();
    ok = Prepare_Draw((UINT)base, Stream->Gl, stride, Fvf);
  }
  if (!ok)
    return D3D_OK;
  Primitives += (unsigned)n;
  if (Indices->Usage & D3DUSAGE_DYNAMIC) {
    const std::vector<uint8_t> &d = Indices->Data;
    size_t first = std::min<size_t>((size_t)start_index * isize, d.size());
    size_t bytes = std::min<size_t>((size_t)n * isize, d.size() - first);
    gls::Bind_Buffer(GL_ELEMENT_ARRAY_BUFFER, ScratchIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)std::max<size_t>(bytes, 4), d.data() + first, GL_STREAM_DRAW);
    Stat_Buffer_Bytes += bytes;
    Stat_Buffer_Uploads++;
    glDrawElements(mode, (GLsizei)(bytes / isize), i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, nullptr);
    return D3D_OK;
  }
  Indices->Upload(); // binds the element buffer into the VAO
  glDrawElements(mode, n, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void *)(uintptr_t)(start_index * isize));
  return D3D_OK;
}

HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT prim_count, const void *data, UINT stride) {
  GLsizei n;
  GLenum mode = Prim_Mode(type, prim_count, n);
  gls::Bind_Buffer(GL_ARRAY_BUFFER, ScratchVbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * stride), data, GL_STREAM_DRAW);
  Stat_Buffer_Bytes += (size_t)n * stride;
  Stat_Buffer_Uploads++;
  if (!Prepare_Draw(0, ScratchVbo, stride, Fvf))
    return D3D_OK;
  Primitives += (unsigned)n;
  glDrawArrays(mode, 0, n);
  // D3D9 resets stream 0 after a UP draw
  if (Stream) {
    Stream->Release();
    Stream = nullptr;
  }
  return D3D_OK;
}

GLuint Device::Blit_Program() { return BlitProgram; }

/* ============================================================== IDirect3D9 */

namespace {

struct Mode {
  UINT w, h;
};
const Mode Modes[] = {{640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},  {1280, 800},  {1280, 960},
                      {1280, 1024}, {1366, 768},  {1440, 900},  {1600, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080},
                      {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
const UINT ModeCount = sizeof(Modes) / sizeof(Modes[0]);

bool Display_Format(D3DFORMAT f) { return f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 || f == D3DFMT_R5G6B5; }

class Direct3D final : public IDirect3D9 {
public:
  HRESULT QueryInterface(REFIID, void **out) override {
    *out = static_cast<IDirect3D9 *>(this);
    AddRef();
    return S_OK;
  }
  ULONG AddRef() override { return ++Refs; }
  ULONG Release() override {
    ULONG r = --Refs;
    if (r == 0)
      delete this;
    return r;
  }
  UINT GetAdapterCount() override { return 1; }
  HRESULT GetAdapterIdentifier(UINT adapter, DWORD, D3DADAPTER_IDENTIFIER9 *id) override {
    if (adapter != 0)
      return D3DERR_INVALIDCALL;
    memset(id, 0, sizeof(*id));
    snprintf(id->Driver, sizeof(id->Driver), "rebel-webgl2");
    snprintf(id->Description, sizeof(id->Description), "WebGL 2.0 (Rebel D3D9 fixed-function)");
    snprintf(id->DeviceName, sizeof(id->DeviceName), "\\\\.\\DISPLAY1");
    id->DriverVersion.QuadPart = 0x0006000E00000001LL;
    id->WHQLLevel = 1;
    return D3D_OK;
  }
  UINT GetAdapterModeCount(UINT, D3DFORMAT fmt) override { return Display_Format(fmt) ? ModeCount : 0; }
  HRESULT EnumAdapterModes(UINT, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE *out) override {
    if (!Display_Format(fmt) || mode >= ModeCount)
      return D3DERR_INVALIDCALL;
    out->Width = Modes[mode].w;
    out->Height = Modes[mode].h;
    out->RefreshRate = 60;
    out->Format = fmt;
    return D3D_OK;
  }
  HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE *mode) override {
    int w = 1920, h = 1080;
#ifdef __EMSCRIPTEN__
    w = EM_ASM_INT({ return (window.screen && screen.width) ? screen.width * (window.devicePixelRatio || 1) : 1920; });
    h = EM_ASM_INT({ return (window.screen && screen.height) ? screen.height * (window.devicePixelRatio || 1) : 1080; });
#endif
    mode->Width = (UINT)w;
    mode->Height = (UINT)h;
    mode->RefreshRate = 60;
    mode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
  }
  HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD usage, D3DRESOURCETYPE, D3DFORMAT fmt) override {
    if (usage & D3DUSAGE_DEPTHSTENCIL)
      return (fmt == D3DFMT_D24S8 || fmt == D3DFMT_D24X8 || fmt == D3DFMT_D16 || fmt == D3DFMT_D32 ||
              fmt == D3DFMT_D24X4S4 || fmt == D3DFMT_D15S1)
                 ? D3D_OK
                 : D3DERR_NOTAVAILABLE;
    if (usage & D3DUSAGE_RENDERTARGET)
      return (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_R5G6B5) ? D3D_OK : D3DERR_NOTAVAILABLE;
    switch (fmt) {
    case D3DFMT_P8:
    case D3DFMT_A8P8:
    case D3DFMT_UYVY:
    case D3DFMT_YUY2:
    case D3DFMT_W11V11U10:
    case D3DFMT_V16U16: return D3DERR_NOTAVAILABLE;
    default: return Format_Info(fmt).known ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
  }
  HRESULT CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
  HRESULT GetDeviceCaps(UINT, D3DDEVTYPE type, D3DCAPS9 *caps) override {
    memset(caps, 0, sizeof(*caps));
    caps->DeviceType = type;
    caps->AdapterOrdinal = 0;
    caps->Caps2 = D3DCAPS2_FULLSCREENGAMMA;
    caps->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
    caps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    caps->RasterCaps = D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_ZBIAS;
    caps->TextureFilterCaps = D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFLINEAR |
                              D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    caps->TextureOpCaps = 0x03FFFFFF; // every op through LERP
    caps->MaxTextureBlendStages = 8;
    caps->MaxSimultaneousTextures = 8;
    caps->MaxActiveLights = 8;
    caps->MaxUserClipPlanes = 0;
    caps->MaxVertexBlendMatrices = 4;
    caps->MaxTextureWidth = caps->MaxTextureHeight = 4096;
    caps->MaxTextureAspectRatio = 4096;
    caps->MaxAnisotropy = 16;
    caps->MaxPointSize = 64.0f;
    caps->MaxPrimitiveCount = 0xFFFFF;
    caps->MaxVertexIndex = 0xFFFFFF;
    caps->MaxStreams = 1;
    caps->MaxStreamStride = 255;
    caps->VertexShaderVersion = 0; // fixed-function only: keep the engine on its FFP paths
    caps->PixelShaderVersion = 0;
    return D3D_OK;
  }
  HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND focus, DWORD, D3DPRESENT_PARAMETERS *pp,
                       IDirect3DDevice9 **device) override {
    Device *d = new Device(this, pp->hDeviceWindow ? pp->hDeviceWindow : focus, pp);
    if (!d->Initialized()) {
      d->Release();
      *device = nullptr;
      return D3DERR_NOTAVAILABLE;
    }
    *device = d;
    return D3D_OK;
  }

private:
  ULONG Refs = 1;
};

} // namespace
} // namespace rebel_d3d

extern "C" IDirect3D9 *Direct3DCreate9(UINT) { return new rebel_d3d::Direct3D(); }
