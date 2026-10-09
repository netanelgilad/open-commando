#include "gl_state.h"

#include <cstring>

namespace rebel_d3d {
namespace gls {
namespace {

const int Units = 16;
const int Attribs = 16;

struct AttribPointer {
  GLuint buffer;
  GLint size;
  GLenum type;
  bool normalized;
  GLsizei stride;
  uintptr_t offset;
  bool valid;
};

// "Unknown" is encoded as a value no real call uses, so the first call
// always goes through.
const GLenum Unknown = 0xFFFFFFFFu;

struct State {
  int caps[6]; // -1 unknown, 0 off, 1 on
  GLenum blend_src, blend_dst, blend_eq, depth_func, cull;
  int depth_mask, color_mask;
  float poly_factor, poly_units;
  bool poly_valid;
  GLenum stencil_func, stencil_fail, stencil_zfail, stencil_zpass;
  GLint stencil_ref;
  GLuint stencil_func_mask, stencil_write_mask;
  bool stencil_func_valid, stencil_op_valid, stencil_mask_valid;
  GLint vp[4];
  bool vp_valid;
  float depth_near, depth_far;
  bool depth_range_valid;
  GLint sc[4];
  bool sc_valid;
  GLuint program, vao, array_buffer, element_buffer, read_fbo, draw_fbo;
  bool program_valid, vao_valid, array_valid, element_valid, read_valid, draw_valid;
  GLenum active_unit;
  GLuint texture[Units];
  bool texture_valid[Units];
  uint32_t attrib_mask;
  bool attrib_mask_valid;
  AttribPointer attrib[Attribs];
} S;

int Cap_Index(GLenum cap) {
  switch (cap) {
  case GL_BLEND: return 0;
  case GL_DEPTH_TEST: return 1;
  case GL_STENCIL_TEST: return 2;
  case GL_CULL_FACE: return 3;
  case GL_POLYGON_OFFSET_FILL: return 4;
  case GL_SCISSOR_TEST: return 5;
  default: return -1;
  }
}

} // namespace

void Invalidate() {
  memset(&S, 0, sizeof(S));
  for (int &c : S.caps)
    c = -1;
  S.blend_src = S.blend_dst = S.blend_eq = S.depth_func = S.cull = Unknown;
  S.depth_mask = S.color_mask = -1;
  S.active_unit = Unknown;
}

void Enable(GLenum cap, bool on) {
  int i = Cap_Index(cap);
  if (i >= 0 && S.caps[i] == (int)on)
    return;
  if (on)
    glEnable(cap);
  else
    glDisable(cap);
  if (i >= 0)
    S.caps[i] = on;
}

void Blend_Func(GLenum src, GLenum dst) {
  if (S.blend_src == src && S.blend_dst == dst)
    return;
  glBlendFunc(src, dst);
  S.blend_src = src;
  S.blend_dst = dst;
}

void Blend_Equation(GLenum mode) {
  if (S.blend_eq == mode)
    return;
  glBlendEquation(mode);
  S.blend_eq = mode;
}

void Depth_Func(GLenum func) {
  if (S.depth_func == func)
    return;
  glDepthFunc(func);
  S.depth_func = func;
}

void Depth_Mask(bool on) {
  if (S.depth_mask == (int)on)
    return;
  glDepthMask(on ? GL_TRUE : GL_FALSE);
  S.depth_mask = on;
}

void Color_Mask(bool r, bool g, bool b, bool a) {
  int m = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
  if (S.color_mask == m)
    return;
  glColorMask(r, g, b, a);
  S.color_mask = m;
}

void Cull_Face(GLenum mode) {
  if (S.cull == mode)
    return;
  glCullFace(mode);
  S.cull = mode;
}

void Polygon_Offset(float factor, float units) {
  if (S.poly_valid && S.poly_factor == factor && S.poly_units == units)
    return;
  glPolygonOffset(factor, units);
  S.poly_factor = factor;
  S.poly_units = units;
  S.poly_valid = true;
}

void Stencil_Func(GLenum func, GLint ref, GLuint mask) {
  if (S.stencil_func_valid && S.stencil_func == func && S.stencil_ref == ref && S.stencil_func_mask == mask)
    return;
  glStencilFunc(func, ref, mask);
  S.stencil_func = func;
  S.stencil_ref = ref;
  S.stencil_func_mask = mask;
  S.stencil_func_valid = true;
}

void Stencil_Op(GLenum fail, GLenum zfail, GLenum zpass) {
  if (S.stencil_op_valid && S.stencil_fail == fail && S.stencil_zfail == zfail && S.stencil_zpass == zpass)
    return;
  glStencilOp(fail, zfail, zpass);
  S.stencil_fail = fail;
  S.stencil_zfail = zfail;
  S.stencil_zpass = zpass;
  S.stencil_op_valid = true;
}

void Stencil_Mask(GLuint mask) {
  if (S.stencil_mask_valid && S.stencil_write_mask == mask)
    return;
  glStencilMask(mask);
  S.stencil_write_mask = mask;
  S.stencil_mask_valid = true;
}

void Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (S.vp_valid && S.vp[0] == x && S.vp[1] == y && S.vp[2] == w && S.vp[3] == h)
    return;
  glViewport(x, y, w, h);
  S.vp[0] = x;
  S.vp[1] = y;
  S.vp[2] = w;
  S.vp[3] = h;
  S.vp_valid = true;
}

void Depth_Range(float n, float f) {
  if (S.depth_range_valid && S.depth_near == n && S.depth_far == f)
    return;
  glDepthRangef(n, f);
  S.depth_near = n;
  S.depth_far = f;
  S.depth_range_valid = true;
}

void Scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (S.sc_valid && S.sc[0] == x && S.sc[1] == y && S.sc[2] == w && S.sc[3] == h)
    return;
  glScissor(x, y, w, h);
  S.sc[0] = x;
  S.sc[1] = y;
  S.sc[2] = w;
  S.sc[3] = h;
  S.sc_valid = true;
}

void Use_Program(GLuint program) {
  if (S.program_valid && S.program == program)
    return;
  glUseProgram(program);
  S.program = program;
  S.program_valid = true;
}

void Bind_Vertex_Array(GLuint vao) {
  if (S.vao_valid && S.vao == vao)
    return;
  glBindVertexArray(vao);
  S.vao = vao;
  S.vao_valid = true;
  // Element and attribute state belong to the VAO.
  S.element_valid = false;
  S.attrib_mask_valid = false;
  for (AttribPointer &a : S.attrib)
    a.valid = false;
}

void Bind_Buffer(GLenum target, GLuint buffer) {
  if (target == GL_ARRAY_BUFFER) {
    if (S.array_valid && S.array_buffer == buffer)
      return;
    glBindBuffer(target, buffer);
    S.array_buffer = buffer;
    S.array_valid = true;
  } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
    if (S.element_valid && S.element_buffer == buffer)
      return;
    glBindBuffer(target, buffer);
    S.element_buffer = buffer;
    S.element_valid = true;
  } else {
    glBindBuffer(target, buffer);
  }
}

void Active_Texture(GLenum unit) {
  if (S.active_unit == unit)
    return;
  glActiveTexture(unit);
  S.active_unit = unit;
}

void Bind_Texture_2D(GLuint texture) {
  int u = S.active_unit == Unknown ? -1 : (int)(S.active_unit - GL_TEXTURE0);
  if (u >= 0 && u < Units && S.texture_valid[u] && S.texture[u] == texture)
    return;
  glBindTexture(GL_TEXTURE_2D, texture);
  if (u >= 0 && u < Units) {
    S.texture[u] = texture;
    S.texture_valid[u] = true;
  }
}

void Bind_Texture_Unit(int unit, GLuint texture) {
  if (unit >= 0 && unit < Units && S.texture_valid[unit] && S.texture[unit] == texture)
    return;
  Active_Texture(GL_TEXTURE0 + unit);
  Bind_Texture_2D(texture);
}

void Bind_Framebuffer(GLenum target, GLuint fbo) {
  bool read = target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER;
  bool draw = target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER;
  if ((!read || (S.read_valid && S.read_fbo == fbo)) && (!draw || (S.draw_valid && S.draw_fbo == fbo)))
    return;
  if (read && draw && !(S.read_valid && S.read_fbo == fbo))
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  else if (read && draw)
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  else
    glBindFramebuffer(target, fbo);
  if (read) {
    S.read_fbo = fbo;
    S.read_valid = true;
  }
  if (draw) {
    S.draw_fbo = fbo;
    S.draw_valid = true;
  }
}

void Attrib_Arrays(uint32_t mask) {
  uint32_t known = S.attrib_mask_valid ? S.attrib_mask : ~mask;
  uint32_t diff = known ^ mask;
  if (!S.attrib_mask_valid)
    diff = 0xFFFFu;
  for (GLuint i = 0; i < Attribs; i++) {
    if (!(diff & (1u << i)))
      continue;
    if (mask & (1u << i))
      glEnableVertexAttribArray(i);
    else
      glDisableVertexAttribArray(i);
  }
  S.attrib_mask = mask;
  S.attrib_mask_valid = true;
}

void Attrib_Pointer(GLuint index, GLint size, GLenum type, bool normalized, GLsizei stride, uintptr_t offset) {
  if (index >= Attribs) {
    glVertexAttribPointer(index, size, type, normalized ? GL_TRUE : GL_FALSE, stride, (const void *)offset);
    return;
  }
  AttribPointer &a = S.attrib[index];
  GLuint buffer = S.array_valid ? S.array_buffer : 0xFFFFFFFFu;
  if (a.valid && S.array_valid && a.buffer == buffer && a.size == size && a.type == type && a.normalized == normalized &&
      a.stride == stride && a.offset == offset)
    return;
  glVertexAttribPointer(index, size, type, normalized ? GL_TRUE : GL_FALSE, stride, (const void *)offset);
  a = {buffer, size, type, normalized, stride, offset, S.array_valid};
}

void Forget_Buffer(GLuint buffer) {
  if (S.array_buffer == buffer)
    S.array_valid = false;
  if (S.element_buffer == buffer)
    S.element_valid = false;
  for (AttribPointer &a : S.attrib)
    if (a.buffer == buffer)
      a.valid = false;
}

void Forget_Texture(GLuint texture) {
  for (int u = 0; u < Units; u++)
    if (S.texture[u] == texture)
      S.texture_valid[u] = false;
}

void Forget_Framebuffer(GLuint fbo) {
  if (S.read_fbo == fbo)
    S.read_valid = false;
  if (S.draw_fbo == fbo)
    S.draw_valid = false;
}

void Forget_Program(GLuint program) {
  if (S.program == program)
    S.program_valid = false;
}

} // namespace gls
} // namespace rebel_d3d
