/*
** Shadow of the WebGL state the D3D9 layer sets, so unchanged state costs
** nothing. Every WebGL call is validated, serialized and decoded by the GPU
** process (several microseconds each); re-applying the full pipeline per
** draw was ~74 calls per draw and saturated Chrome's GPU process.
**
** All state the device touches must go through here (or Invalidate() after
** raw calls). One context, one vertex array object: attribute and element
** buffer state is tracked for that VAO only.
*/
#pragma once

#include <GLES3/gl3.h>
#include <cstdint>

namespace rebel_d3d {
namespace gls {

void Invalidate(); // forget everything (context created/restored)

// Capabilities: GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE,
// GL_POLYGON_OFFSET_FILL, GL_SCISSOR_TEST.
void Enable(GLenum cap, bool on);
void Blend_Func(GLenum src, GLenum dst);
void Blend_Equation(GLenum mode);
void Depth_Func(GLenum func);
void Depth_Mask(bool on);
void Color_Mask(bool r, bool g, bool b, bool a);
void Cull_Face(GLenum mode);
void Polygon_Offset(float factor, float units);
void Stencil_Func(GLenum func, GLint ref, GLuint mask);
void Stencil_Op(GLenum fail, GLenum zfail, GLenum zpass);
void Stencil_Mask(GLuint mask);
void Viewport(GLint x, GLint y, GLsizei w, GLsizei h);
void Depth_Range(float n, float f);
void Scissor(GLint x, GLint y, GLsizei w, GLsizei h);

void Use_Program(GLuint program);
void Bind_Vertex_Array(GLuint vao);
void Bind_Buffer(GLenum target, GLuint buffer); // GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER
void Active_Texture(GLenum unit);               // GL_TEXTURE0 + n
void Bind_Texture_2D(GLuint texture);           // on the active unit
void Bind_Texture_Unit(int unit, GLuint texture); // switches units only if the binding changes
void Bind_Framebuffer(GLenum target, GLuint fbo); // GL_FRAMEBUFFER sets read and draw

// Vertex attributes of the device VAO: enables exactly the arrays in `mask`.
void Attrib_Arrays(uint32_t mask);
void Attrib_Pointer(GLuint index, GLint size, GLenum type, bool normalized, GLsizei stride, uintptr_t offset);

// Objects about to be deleted: drop any cached binding of them.
void Forget_Buffer(GLuint buffer);
void Forget_Texture(GLuint texture);
void Forget_Framebuffer(GLuint fbo);
void Forget_Program(GLuint program);

} // namespace gls
} // namespace rebel_d3d
