/*
** Internal declarations for the Direct3D 9 (fixed-function) implementation
** on WebGL2 / OpenGL ES 3.0.
**
** Conventions
** - Everything renders into offscreen framebuffers whose row 0 is the top
**   of the image, like D3D. Clip-space y is negated and depth remapped from
**   D3D's [0,w] to GL's [-w,w] in every generated vertex shader, so viewport,
**   scissor, render-to-texture and readback all use D3D coordinates. Present
**   copies the back buffer to the canvas flipped (and applies the gamma ramp).
** - With that mapping a triangle that is clockwise on the D3D screen is
**   counter-clockwise in GL window space: glFrontFace(GL_CCW).
** - Every resource keeps a CPU copy in its D3D format. Lock returns pointers
**   into it; Unlock marks it dirty; it is uploaded before the next draw.
*/
#pragma once

#include "d3d9.h"

#include <GLES3/gl3.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rebel_d3d {

class Device;

/* -------------------------------------------------------------- formats */

struct FormatInfo {
  bool known = false;
  bool compressed = false; // 4x4 block format (DXTn)
  int bytes = 0;           // bytes per pixel, or per 4x4 block when compressed
};
FormatInfo Format_Info(D3DFORMAT fmt);
UINT Level_Pitch(D3DFORMAT fmt, UINT width);
UINT Level_Rows(D3DFORMAT fmt, UINT height);
UINT Level_Size(D3DFORMAT fmt, UINT width, UINT height);

// Converts a rectangle of `fmt` pixels to tightly packed RGBA8 (R,G,B,A bytes).
bool Decode_To_RGBA8(D3DFORMAT fmt, const uint8_t *src, UINT pitch, UINT width, UINT height, uint8_t *rgba);
// Converts tightly packed RGBA8 into `fmt` (uncompressed formats only).
bool Encode_From_RGBA8(D3DFORMAT fmt, const uint8_t *rgba, UINT width, UINT height, uint8_t *dst, UINT pitch);

/* ------------------------------------------------------------ resources */

class Resource {
public:
  explicit Resource(Device *device) : OwnerDevice(device) {}
  virtual ~Resource() {}
  ULONG Ref_Add() { return ++Refs; }
  ULONG Ref_Release() {
    ULONG r = --Refs;
    if (r == 0)
      delete this;
    return r;
  }
  Device *OwnerDevice;
  ULONG Refs = 1;
  DWORD Priority = 0;
};

class Texture;

class Surface final : public IDirect3DSurface9, public Resource {
public:
  enum class Kind { TextureLevel, Offscreen, RenderTarget, DepthStencil, BackBuffer };

  Surface(Device *device, Kind kind, UINT width, UINT height, D3DFORMAT fmt, D3DPOOL pool, DWORD usage);
  ~Surface() override;

  // IUnknown
  HRESULT QueryInterface(REFIID riid, void **out) override;
  ULONG AddRef() override;
  ULONG Release() override;
  // IDirect3DResource9
  HRESULT GetDevice(IDirect3DDevice9 **device) override;
  DWORD SetPriority(DWORD p) override;
  DWORD GetPriority() override { return Priority; }
  void PreLoad() override {}
  D3DRESOURCETYPE GetType() override { return D3DRTYPE_SURFACE; }
  // IDirect3DSurface9
  HRESULT GetDesc(D3DSURFACE_DESC *desc) override;
  HRESULT LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
  HRESULT UnlockRect() override;

  // Pixels of a texture level live in the texture; others are owned here.
  uint8_t *Bits();
  UINT Pitch() const { return Level_Pitch(Format, Width); }
  void Mark_Dirty();
  // GPU-side surfaces (render targets, back buffer): copy GL pixels into Bits().
  void Read_Back();
  // Framebuffer holding this surface as color (render targets) or depth.
  GLuint Framebuffer();

  Kind SurfaceKind;
  UINT Width, Height;
  D3DFORMAT Format;
  D3DPOOL Pool;
  DWORD Usage;
  Texture *Owner = nullptr; // for TextureLevel
  UINT Level = 0;
  std::vector<uint8_t> Data; // CPU pixels when not a texture level
  GLuint Renderbuffer = 0;   // depth-stencil, or color for standalone RTs
  GLuint Fbo = 0;
  GLuint AttachedDepth = 0; // depth renderbuffer currently attached to Fbo
  bool Locked = false;
};

class Texture final : public IDirect3DTexture9, public Resource {
public:
  Texture(Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool);
  ~Texture() override;

  HRESULT QueryInterface(REFIID riid, void **out) override;
  ULONG AddRef() override;
  ULONG Release() override;
  HRESULT GetDevice(IDirect3DDevice9 **device) override;
  DWORD SetPriority(DWORD p) override;
  DWORD GetPriority() override { return Priority; }
  void PreLoad() override { Upload(); }
  D3DRESOURCETYPE GetType() override { return D3DRTYPE_TEXTURE; }
  DWORD SetLOD(DWORD lod) override;
  DWORD GetLOD() override { return Lod; }
  DWORD GetLevelCount() override { return (DWORD)Levels.size(); }
  HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) override;
  HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface) override;
  HRESULT LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
  HRESULT UnlockRect(UINT level) override;

  struct LevelData {
    UINT width, height;
    std::vector<uint8_t> bits;
    bool dirty = true;
    Surface *surface = nullptr; // created on demand, holds a ref to us
  };

  // Ensures the GL texture holds the current CPU pixels.
  void Upload();
  bool Is_Render_Target() const { return (Usage & D3DUSAGE_RENDERTARGET) != 0; }

  UINT Width, Height;
  D3DFORMAT Format;
  D3DPOOL Pool;
  DWORD Usage;
  DWORD Lod = 0;
  std::vector<LevelData> Levels;
  GLuint Gl = 0;
  bool AnyDirty = true;
  bool GpuNewer = false; // rendered to: GL holds the newest pixels
  // Sampler state last applied to the GL texture object
  struct {
    GLint min = -1, mag = -1, wrap_s = -1, wrap_t = -1;
    float aniso = -1;
    int max_level = -1;
  } Applied;
};

class VertexBuffer final : public IDirect3DVertexBuffer9, public Resource {
public:
  VertexBuffer(Device *device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool);
  ~VertexBuffer() override;
  HRESULT QueryInterface(REFIID riid, void **out) override;
  ULONG AddRef() override;
  ULONG Release() override;
  HRESULT GetDevice(IDirect3DDevice9 **device) override;
  DWORD SetPriority(DWORD p) override;
  DWORD GetPriority() override { return Priority; }
  void PreLoad() override { Upload(); }
  D3DRESOURCETYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
  HRESULT Lock(UINT offset, UINT size, void **data, DWORD flags) override;
  HRESULT Unlock() override;
  HRESULT GetDesc(D3DVERTEXBUFFER_DESC *desc) override;

  void Upload();
  std::vector<uint8_t> Data;
  UINT DirtyBegin = 0, DirtyEnd = 0;
  DWORD Usage, Fvf;
  D3DPOOL Pool;
  GLuint Gl = 0;
};

class IndexBuffer final : public IDirect3DIndexBuffer9, public Resource {
public:
  IndexBuffer(Device *device, UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool);
  ~IndexBuffer() override;
  HRESULT QueryInterface(REFIID riid, void **out) override;
  ULONG AddRef() override;
  ULONG Release() override;
  HRESULT GetDevice(IDirect3DDevice9 **device) override;
  DWORD SetPriority(DWORD p) override;
  DWORD GetPriority() override { return Priority; }
  void PreLoad() override { Upload(); }
  D3DRESOURCETYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }
  HRESULT Lock(UINT offset, UINT size, void **data, DWORD flags) override;
  HRESULT Unlock() override;
  HRESULT GetDesc(D3DINDEXBUFFER_DESC *desc) override;

  void Upload();
  std::vector<uint8_t> Data;
  UINT DirtyBegin = 0, DirtyEnd = 0;
  UINT GlSize = 0; // bytes allocated in the GL buffer
  DWORD Usage;
  D3DFORMAT Format;
  D3DPOOL Pool;
  GLuint Gl = 0;
};

/* ------------------------------------------------- fixed-function shaders */

// Everything that changes the generated GLSL. Uniform-only state (matrices,
// colors, light parameters) is not part of the key.
struct FfpKey {
  DWORD fvf = 0;
  uint8_t lighting = 0, specular = 0, local_viewer = 0, normalize = 0, color_vertex = 0;
  uint8_t diffuse_src = 0, ambient_src = 0, specular_src = 0, emissive_src = 0;
  uint8_t light_types[8] = {}; // 0 = off, else D3DLIGHTTYPE
  uint8_t fog = 0;             // 0 off, 1 vertex, 2 table
  uint8_t fog_mode = 0;        // D3DFOGMODE
  uint8_t range_fog = 0;
  uint8_t alpha_test = 0;      // D3DCMPFUNC when enabled, else 0
  uint8_t flat = 0;
  uint8_t blend_weights = 0;   // world matrices used for vertex blending (1 = none)
  struct Stage {
    uint8_t color_op, color_arg0, color_arg1, color_arg2;
    uint8_t alpha_op, alpha_arg0, alpha_arg1, alpha_arg2;
    uint8_t result_arg, coord_index, tci, ttff, has_texture;
  } stages[8] = {};
  uint8_t num_stages = 0;

  std::string Serialize() const;
};

struct FfpProgram {
  GLuint program = 0;
  // uniform locations
  GLint u_world[4], u_view, u_proj, u_normal_matrix, u_viewport;
  GLint u_mat_diffuse, u_mat_ambient, u_mat_specular, u_mat_emissive, u_mat_power, u_ambient;
  GLint u_light_diffuse[8], u_light_specular[8], u_light_ambient[8], u_light_position[8], u_light_direction[8];
  GLint u_light_atten[8], u_light_spot[8];
  GLint u_fog_color, u_fog_params, u_tfactor, u_alpha_ref;
  GLint u_tex_matrix[8], u_bump_mat[8], u_bump_lum[8], u_sampler[8];
  GLint u_point_size;
  // attribute locations: position, blend weights, normal, diffuse, specular, texcoords 0..7
  GLint a_position, a_weights, a_normal, a_diffuse, a_specular, a_texcoord[8];
};

// Builds (and caches) the GLSL program for a key. Returns nullptr on failure.
FfpProgram *Ffp_Program_For(const FfpKey &key);
void Ffp_Clear_Cache();

/* ---------------------------------------------------------------- device */

struct FvfLayout {
  UINT stride = 0;
  int position_size = 0; // 3 for XYZ(B), 4 for XYZRHW
  bool rhw = false;
  int weights = 0; // floats of blend weights
  bool last_beta_ubyte4 = false;
  int normal = -1, diffuse = -1, specular = -1, psize = -1, position = 0, blend = -1;
  int tex_count = 0;
  int tex_offset[8] = {}, tex_size[8] = {};
};
FvfLayout Fvf_Layout(DWORD fvf);

class Device final : public IDirect3DDevice9 {
public:
  Device(IDirect3D9 *d3d, HWND window, D3DPRESENT_PARAMETERS *pp);
  ~Device();
  bool Initialized() const { return Ready; }

  // IUnknown
  HRESULT QueryInterface(REFIID riid, void **out) override;
  ULONG AddRef() override { return ++Refs; }
  ULONG Release() override;

  // IDirect3DDevice9
  HRESULT TestCooperativeLevel() override { return D3D_OK; }
  UINT GetAvailableTextureMem() override { return 512u << 20; }
  HRESULT EvictManagedResources() override { return D3D_OK; }
  HRESULT GetDeviceCaps(D3DCAPS9 *caps) override;
  HRESULT GetDisplayMode(UINT swapchain, D3DDISPLAYMODE *mode) override;
  HRESULT CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain9 **chain) override;
  HRESULT Reset(D3DPRESENT_PARAMETERS *pp) override;
  HRESULT Present(const RECT *src, const RECT *dst, HWND wnd, const void *dirty) override;
  HRESULT GetBackBuffer(UINT swapchain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface) override;
  void SetGammaRamp(UINT swapchain, DWORD flags, const D3DGAMMARAMP *ramp) override;
  HRESULT CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **tex,
                        HANDLE *shared) override;
  HRESULT CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **vb,
                             HANDLE *shared) override;
  HRESULT CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **ib,
                            HANDLE *shared) override;
  HRESULT CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL lockable,
                             IDirect3DSurface9 **surface, HANDLE *shared) override;
  HRESULT CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL discard,
                                    IDirect3DSurface9 **surface, HANDLE *shared) override;
  HRESULT UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) override;
  HRESULT GetFrontBufferData(UINT swapchain, IDirect3DSurface9 *dst) override;
  HRESULT StretchRect(IDirect3DSurface9 *src, const RECT *src_rect, IDirect3DSurface9 *dst, const RECT *dst_rect,
                      D3DTEXTUREFILTERTYPE filter) override;
  HRESULT CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **surface,
                                      HANDLE *shared) override;
  HRESULT SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) override;
  HRESULT GetRenderTarget(DWORD index, IDirect3DSurface9 **surface) override;
  HRESULT SetDepthStencilSurface(IDirect3DSurface9 *surface) override;
  HRESULT GetDepthStencilSurface(IDirect3DSurface9 **surface) override;
  HRESULT BeginScene() override { return D3D_OK; }
  HRESULT EndScene() override { return D3D_OK; }
  HRESULT Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
  HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *m) override;
  HRESULT GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) override;
  HRESULT SetViewport(const D3DVIEWPORT9 *vp) override;
  HRESULT GetViewport(D3DVIEWPORT9 *vp) override;
  HRESULT SetMaterial(const D3DMATERIAL9 *mat) override;
  HRESULT GetMaterial(D3DMATERIAL9 *mat) override;
  HRESULT SetLight(DWORD index, const D3DLIGHT9 *light) override;
  HRESULT GetLight(DWORD index, D3DLIGHT9 *light) override;
  HRESULT LightEnable(DWORD index, BOOL enable) override;
  HRESULT SetClipPlane(DWORD index, const float *plane) override;
  HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
  HRESULT GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) override;
  HRESULT GetTexture(DWORD stage, IDirect3DBaseTexture9 **tex) override;
  HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture9 *tex) override;
  HRESULT GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) override;
  HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
  HRESULT GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) override;
  HRESULT SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
  HRESULT ValidateDevice(DWORD *passes) override;
  HRESULT SetSoftwareVertexProcessing(BOOL) override { return D3D_OK; }
  HRESULT SetNPatchMode(float) override { return D3D_OK; }
  HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override;
  HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base_vertex, UINT min_index, UINT num_vertices,
                               UINT start_index, UINT prim_count) override;
  HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT prim_count, const void *data, UINT stride) override;
  HRESULT SetFVF(DWORD fvf) override;
  HRESULT GetFVF(DWORD *fvf) override;
  HRESULT SetVertexShader(void *shader) override;
  HRESULT SetPixelShader(void *) override { return D3D_OK; }
  HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) override;
  HRESULT SetIndices(IDirect3DIndexBuffer9 *ib) override;

  // internal
  bool Has_S3TC() const { return S3tc; }
  void Bind_Framebuffer_For(Surface *color);
  GLuint Blit_Program();
  Surface *Back_Buffer() { return BackBuffer; }

private:
  bool Create_Back_Buffer();
  void Destroy_Back_Buffer();
  bool Prepare_Draw(UINT vertex_base_bytes, GLuint vbo, UINT stride, DWORD fvf);
  void Apply_Pipeline_State();
  void Apply_Samplers(FfpProgram *prog);
  void Upload_Uniforms(FfpProgram *prog, const FvfLayout &layout);
  FfpKey Build_Key(const FvfLayout &layout) const;
  void Set_Render_State_Defaults();

  ULONG Refs = 1;
  bool Ready = false;
  bool S3tc = false;
  float MaxAniso = 1.0f;
  IDirect3D9 *D3D;
  HWND Window;
  D3DPRESENT_PARAMETERS Params;
  int Context = 0; // EMSCRIPTEN_WEBGL_CONTEXT_HANDLE

  Surface *BackBuffer = nullptr;
  Surface *AutoDepth = nullptr;
  Surface *CurrentRT = nullptr;
  Surface *CurrentDepth = nullptr;

  DWORD RenderStates[REBEL_D3D_RENDERSTATE_COUNT] = {};
  DWORD StageStates[8][REBEL_D3D_TSS_COUNT] = {};
  DWORD SamplerStates[8][REBEL_D3D_SAMP_COUNT] = {};
  D3DMATRIX World[4], View, Projection, TexMatrix[8];
  D3DMATERIAL9 Material = {};
  D3DLIGHT9 Lights[8] = {};
  bool LightOn[8] = {};
  D3DVIEWPORT9 Viewport = {};
  Texture *Textures[8] = {};
  VertexBuffer *Stream = nullptr;
  UINT StreamOffset = 0, StreamStride = 0;
  IndexBuffer *Indices = nullptr;
  DWORD Fvf = 0;
  WORD Gamma[3][256];
  bool GammaIdentity = true;

  // Diagnostics: logged periodically from Present.
  unsigned FrameCount = 0, DrawCalls = 0, Clears = 0, Primitives = 0, FailedPrograms = 0;
  double LastReport = 0;

  GLuint Vao = 0;
  GLuint ScratchVbo = 0, ScratchIbo = 0;
  GLuint BlitProgram = 0, GammaTexture = 0;
};

} // namespace rebel_d3d
