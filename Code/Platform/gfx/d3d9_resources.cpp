/*
** D3D9 resources on WebGL2: textures, surfaces, vertex and index buffers.
*/
#include "d3d9_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace rebel_d3d {

// Texture unit reserved for uploads so draw bindings aren't disturbed.
static const GLenum UploadUnit = GL_TEXTURE0 + 15;

/* ============================================================== Surface */

Surface::Surface(Device *device, Kind kind, UINT width, UINT height, D3DFORMAT fmt, D3DPOOL pool, DWORD usage)
    : Resource(device), SurfaceKind(kind), Width(width), Height(height), Format(fmt), Pool(pool), Usage(usage) {
  if (kind == Kind::Offscreen || kind == Kind::RenderTarget || kind == Kind::BackBuffer)
    Data.assign(Level_Size(fmt, width, height), 0);
  if (kind == Kind::DepthStencil) {
    glGenRenderbuffers(1, &Renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, Renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)width, (GLsizei)height);
  } else if (kind == Kind::RenderTarget || kind == Kind::BackBuffer) {
    glGenRenderbuffers(1, &Renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, Renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)width, (GLsizei)height);
  }
}

Surface::~Surface() {
  if (Fbo)
    gls::Forget_Framebuffer(Fbo);
    glDeleteFramebuffers(1, &Fbo);
  if (Renderbuffer)
    glDeleteRenderbuffers(1, &Renderbuffer);
}

HRESULT Surface::QueryInterface(REFIID, void **out) {
  *out = static_cast<IDirect3DSurface9 *>(this);
  AddRef();
  return S_OK;
}

// Texture-level surfaces share the texture's lifetime: they forward refs.
ULONG Surface::AddRef() {
  if (Owner)
    return Owner->AddRef();
  return Ref_Add();
}

ULONG Surface::Release() {
  if (Owner)
    return Owner->Release();
  return Ref_Release();
}

HRESULT Surface::GetDevice(IDirect3DDevice9 **device) {
  *device = OwnerDevice;
  OwnerDevice->AddRef();
  return D3D_OK;
}

DWORD Surface::SetPriority(DWORD p) {
  DWORD old = Priority;
  Priority = p;
  return old;
}

HRESULT Surface::GetDesc(D3DSURFACE_DESC *desc) {
  desc->Format = Format;
  desc->Type = D3DRTYPE_SURFACE;
  desc->Usage = Usage;
  desc->Pool = Pool;
  desc->MultiSampleType = D3DMULTISAMPLE_NONE;
  desc->MultiSampleQuality = 0;
  desc->Width = Width;
  desc->Height = Height;
  return D3D_OK;
}

uint8_t *Surface::Bits() {
  if (Owner)
    return Owner->Levels[Level].bits.data();
  return Data.data();
}

void Surface::Mark_Dirty() {
  if (Owner) {
    Owner->Levels[Level].dirty = true;
    Owner->AnyDirty = true;
  }
}

GLuint Surface::Framebuffer() {
  if (Fbo)
    return Fbo;
  glGenFramebuffers(1, &Fbo);
  gls::Bind_Framebuffer(GL_FRAMEBUFFER, Fbo);
  if (SurfaceKind == Kind::TextureLevel && Owner) {
    Owner->Upload();
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Owner->Gl, (GLint)Level);
  } else if (SurfaceKind == Kind::DepthStencil) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, Renderbuffer);
  } else if (Renderbuffer) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, Renderbuffer);
  }
  return Fbo;
}

void Surface::Read_Back() {
  bool gpu = (SurfaceKind == Kind::TextureLevel && Owner && Owner->GpuNewer) || SurfaceKind == Kind::RenderTarget ||
             SurfaceKind == Kind::BackBuffer;
  if (!gpu || Format_Info(Format).compressed)
    return;
  std::vector<uint8_t> rgba((size_t)Width * Height * 4);
  gls::Bind_Framebuffer(GL_FRAMEBUFFER, Framebuffer());
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, (GLsizei)Width, (GLsizei)Height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  Encode_From_RGBA8(Format, rgba.data(), Width, Height, Bits(), Pitch());
  if (Owner)
    Owner->GpuNewer = false;
}

HRESULT Surface::LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (SurfaceKind == Kind::DepthStencil)
    return D3DERR_INVALIDCALL;
  if (!(flags & D3DLOCK_DISCARD))
    Read_Back();
  UINT x = rect ? (UINT)rect->left : 0, y = rect ? (UINT)rect->top : 0;
  FormatInfo info = Format_Info(Format);
  locked->Pitch = (INT)Pitch();
  if (info.compressed)
    locked->pBits = Bits() + (y / 4) * Pitch() + (x / 4) * info.bytes;
  else
    locked->pBits = Bits() + y * Pitch() + x * info.bytes;
  Locked = true;
  if (!(flags & D3DLOCK_READONLY))
    Mark_Dirty();
  return D3D_OK;
}

HRESULT Surface::UnlockRect() {
  Locked = false;
  return D3D_OK;
}

/* ============================================================== Texture */

Texture::Texture(Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
    : Resource(device), Width(width), Height(height), Format(fmt), Pool(pool), Usage(usage) {
  UINT max_levels = 1;
  for (UINT w = width, h = height; w > 1 || h > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2))
    max_levels++;
  if (levels == 0 || levels > max_levels)
    levels = max_levels;
  if (usage & D3DUSAGE_RENDERTARGET)
    levels = 1;
  UINT w = width, h = height;
  for (UINT i = 0; i < levels; i++) {
    LevelData l;
    l.width = w;
    l.height = h;
    l.bits.assign(Level_Size(fmt, w, h), 0);
    Levels.push_back(std::move(l));
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  glGenTextures(1, &Gl);
}

Texture::~Texture() {
  for (LevelData &l : Levels)
    if (l.surface) {
      l.surface->Owner = nullptr;
      delete l.surface;
    }
  if (Gl)
    gls::Forget_Texture(Gl);
    glDeleteTextures(1, &Gl);
}

HRESULT Texture::QueryInterface(REFIID, void **out) {
  *out = static_cast<IDirect3DTexture9 *>(this);
  AddRef();
  return S_OK;
}
ULONG Texture::AddRef() { return Ref_Add(); }
ULONG Texture::Release() { return Ref_Release(); }

HRESULT Texture::GetDevice(IDirect3DDevice9 **device) {
  *device = OwnerDevice;
  OwnerDevice->AddRef();
  return D3D_OK;
}

DWORD Texture::SetPriority(DWORD p) {
  DWORD old = Priority;
  Priority = p;
  return old;
}

DWORD Texture::SetLOD(DWORD lod) {
  DWORD old = Lod;
  Lod = std::min<DWORD>(lod, (DWORD)Levels.size() - 1);
  return old;
}

HRESULT Texture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) {
  if (level >= Levels.size())
    return D3DERR_INVALIDCALL;
  desc->Format = Format;
  desc->Type = D3DRTYPE_SURFACE;
  desc->Usage = Usage;
  desc->Pool = Pool;
  desc->MultiSampleType = D3DMULTISAMPLE_NONE;
  desc->MultiSampleQuality = 0;
  desc->Width = Levels[level].width;
  desc->Height = Levels[level].height;
  return D3D_OK;
}

HRESULT Texture::GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface) {
  if (level >= Levels.size())
    return D3DERR_INVALIDCALL;
  LevelData &l = Levels[level];
  if (!l.surface) {
    l.surface = new Surface(OwnerDevice, Surface::Kind::TextureLevel, l.width, l.height, Format, Pool, Usage);
    l.surface->Owner = this;
    l.surface->Level = level;
  }
  *surface = l.surface;
  AddRef();
  return D3D_OK;
}

HRESULT Texture::LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  IDirect3DSurface9 *s = nullptr;
  if (FAILED(GetSurfaceLevel(level, &s)))
    return D3DERR_INVALIDCALL;
  HRESULT hr = s->LockRect(locked, rect, flags);
  s->Release();
  return hr;
}

HRESULT Texture::UnlockRect(UINT level) {
  if (level >= Levels.size())
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}

void Texture::Upload() {
  if (!AnyDirty)
    return;
  AnyDirty = false;
  Stat_Texture_Uploads++;
  for (const LevelData &l : Levels)
    if (l.dirty)
      Stat_Texture_Bytes += l.bits.size();
  // A spare unit, so draw bindings stay put; the state cache tracks it.
  gls::Active_Texture(UploadUnit);
  gls::Bind_Texture_2D(Gl);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  if (Is_Render_Target()) {
    if (Levels[0].dirty) {
      // Render targets start (and are re-specified) as RGBA8 GPU storage;
      // CPU contents are uploaded so locked writes still show up.
      std::vector<uint8_t> rgba((size_t)Width * Height * 4);
      Decode_To_RGBA8(Format, Levels[0].bits.data(), Level_Pitch(Format, Width), Width, Height, rgba.data());
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)Width, (GLsizei)Height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   rgba.data());
      Levels[0].dirty = false;
    }
  } else {
    const bool s3tc = OwnerDevice->Has_S3TC();
    for (UINT i = 0; i < Levels.size(); i++) {
      LevelData &l = Levels[i];
      if (!l.dirty)
        continue;
      l.dirty = false;
      GLenum compressed = 0;
      if (Format == D3DFMT_DXT1)
        compressed = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
      else if (Format == D3DFMT_DXT2 || Format == D3DFMT_DXT3)
        compressed = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
      else if (Format == D3DFMT_DXT4 || Format == D3DFMT_DXT5)
        compressed = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
      // WebGL S3TC needs block-aligned sizes except for the tiny mip tail.
      bool block_ok = (l.width % 4 == 0 || l.width < 4) && (l.height % 4 == 0 || l.height < 4);
      if (compressed && s3tc && block_ok && Format != D3DFMT_DXT2 && Format != D3DFMT_DXT4) {
        glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)i, compressed, (GLsizei)l.width, (GLsizei)l.height, 0,
                               (GLsizei)l.bits.size(), l.bits.data());
      } else if (Format == D3DFMT_R5G6B5) {
        glTexImage2D(GL_TEXTURE_2D, (GLint)i, GL_RGB565, (GLsizei)l.width, (GLsizei)l.height, 0, GL_RGB,
                     GL_UNSIGNED_SHORT_5_6_5, l.bits.data());
      } else {
        std::vector<uint8_t> rgba((size_t)l.width * l.height * 4);
        if (!Decode_To_RGBA8(Format, l.bits.data(), Level_Pitch(Format, l.width), l.width, l.height, rgba.data()))
          std::fill(rgba.begin(), rgba.end(), 255);
        glTexImage2D(GL_TEXTURE_2D, (GLint)i, GL_RGBA8, (GLsizei)l.width, (GLsizei)l.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba.data());
      }
    }
  }
}

/* ======================================================== VertexBuffer */

VertexBuffer::VertexBuffer(Device *device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
    : Resource(device), Usage(usage), Fvf(fvf), Pool(pool) {
  Data.assign(length, 0);
  glGenBuffers(1, &Gl);
  gls::Bind_Buffer(GL_ARRAY_BUFFER, Gl);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)length, nullptr,
               (usage & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
}

VertexBuffer::~VertexBuffer() {
  if (Gl)
    gls::Forget_Buffer(Gl);
    glDeleteBuffers(1, &Gl);
}

HRESULT VertexBuffer::QueryInterface(REFIID, void **out) {
  *out = static_cast<IDirect3DVertexBuffer9 *>(this);
  AddRef();
  return S_OK;
}
ULONG VertexBuffer::AddRef() { return Ref_Add(); }
ULONG VertexBuffer::Release() { return Ref_Release(); }
HRESULT VertexBuffer::GetDevice(IDirect3DDevice9 **device) {
  *device = OwnerDevice;
  OwnerDevice->AddRef();
  return D3D_OK;
}
DWORD VertexBuffer::SetPriority(DWORD p) {
  DWORD old = Priority;
  Priority = p;
  return old;
}

HRESULT VertexBuffer::Lock(UINT offset, UINT size, void **data, DWORD flags) {
  if (offset > Data.size())
    return D3DERR_INVALIDCALL;
  if (size == 0)
    size = (UINT)Data.size() - offset;
  *data = Data.data() + offset;
  if (!(flags & D3DLOCK_READONLY)) {
    if (DirtyEnd <= DirtyBegin) {
      DirtyBegin = offset;
      DirtyEnd = offset + size;
    } else {
      DirtyBegin = std::min(DirtyBegin, offset);
      DirtyEnd = std::max(DirtyEnd, offset + size);
    }
  }
  return D3D_OK;
}

HRESULT VertexBuffer::Unlock() { return D3D_OK; }

HRESULT VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *desc) {
  desc->Format = D3DFMT_UNKNOWN;
  desc->Type = D3DRTYPE_VERTEXBUFFER;
  desc->Usage = Usage;
  desc->Pool = Pool;
  desc->Size = (UINT)Data.size();
  desc->FVF = Fvf;
  return D3D_OK;
}

void VertexBuffer::Upload() {
  if (Usage & D3DUSAGE_DYNAMIC) { // drawn from the CPU copy, see Device::Stream_Range
    DirtyBegin = DirtyEnd = 0;
    return;
  }
  if (DirtyEnd <= DirtyBegin)
    return;
  gls::Bind_Buffer(GL_ARRAY_BUFFER, Gl);
  glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)DirtyBegin, (GLsizeiptr)(DirtyEnd - DirtyBegin), Data.data() + DirtyBegin);
  Stat_Buffer_Bytes += DirtyEnd - DirtyBegin;
  Stat_Buffer_Uploads++;
  DirtyBegin = DirtyEnd = 0;
}

/* ========================================================= IndexBuffer */

IndexBuffer::IndexBuffer(Device *device, UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
    : Resource(device), Usage(usage), Format(fmt), Pool(pool) {
  Data.assign(length, 0);
  glGenBuffers(1, &Gl);
}

IndexBuffer::~IndexBuffer() {
  if (Gl)
    gls::Forget_Buffer(Gl);
    glDeleteBuffers(1, &Gl);
}

HRESULT IndexBuffer::QueryInterface(REFIID, void **out) {
  *out = static_cast<IDirect3DIndexBuffer9 *>(this);
  AddRef();
  return S_OK;
}
ULONG IndexBuffer::AddRef() { return Ref_Add(); }
ULONG IndexBuffer::Release() { return Ref_Release(); }
HRESULT IndexBuffer::GetDevice(IDirect3DDevice9 **device) {
  *device = OwnerDevice;
  OwnerDevice->AddRef();
  return D3D_OK;
}
DWORD IndexBuffer::SetPriority(DWORD p) {
  DWORD old = Priority;
  Priority = p;
  return old;
}

HRESULT IndexBuffer::Lock(UINT offset, UINT size, void **data, DWORD flags) {
  if (offset > Data.size())
    return D3DERR_INVALIDCALL;
  if (size == 0)
    size = (UINT)Data.size() - offset;
  *data = Data.data() + offset;
  if (!(flags & D3DLOCK_READONLY)) {
    if (DirtyEnd <= DirtyBegin) {
      DirtyBegin = offset;
      DirtyEnd = offset + size;
    } else {
      DirtyBegin = std::min(DirtyBegin, offset);
      DirtyEnd = std::max(DirtyEnd, offset + size);
    }
  }
  return D3D_OK;
}

HRESULT IndexBuffer::Unlock() { return D3D_OK; }

HRESULT IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *desc) {
  desc->Format = Format;
  desc->Type = D3DRTYPE_INDEXBUFFER;
  desc->Usage = Usage;
  desc->Pool = Pool;
  desc->Size = (UINT)Data.size();
  return D3D_OK;
}

// The element binding belongs to the bound VAO; the device rebinds before draws.
void IndexBuffer::Upload() {
  if (Usage & D3DUSAGE_DYNAMIC) { // drawn from the CPU copy, see Device::DrawIndexedPrimitive
    DirtyBegin = DirtyEnd = 0;
    return;
  }
  gls::Bind_Buffer(GL_ELEMENT_ARRAY_BUFFER, Gl);
  if (GlSize != Data.size()) {
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)Data.size(), Data.data(),
                 (Usage & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    GlSize = (UINT)Data.size();
    DirtyBegin = DirtyEnd = 0;
    return;
  }
  if (DirtyEnd <= DirtyBegin)
    return;
  glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)DirtyBegin, (GLsizeiptr)(DirtyEnd - DirtyBegin),
                  Data.data() + DirtyBegin);
  Stat_Buffer_Bytes += DirtyEnd - DirtyBegin;
  Stat_Buffer_Uploads++;
  DirtyBegin = DirtyEnd = 0;
}

} // namespace rebel_d3d
