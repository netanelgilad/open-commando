/*
** Direct3D fixed-function pipeline as generated GLSL ES 3.00.
**
** One program per distinct FfpKey (vertex format + the render, light and
** texture-stage state that changes code). Matrices are uploaded in D3D's
** memory layout, which GLSL reads as the transpose, so D3D's row-vector
** `v * M` becomes `M * v` here.
*/
#include "d3d9_internal.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>

namespace rebel_d3d {

std::string FfpKey::Serialize() const {
  return std::string(reinterpret_cast<const char *>(this), sizeof(*this));
}

FvfLayout Fvf_Layout(DWORD fvf) {
  FvfLayout l;
  UINT off = 0;
  switch (fvf & D3DFVF_POSITION_MASK) {
  case D3DFVF_XYZRHW:
    l.rhw = true;
    l.position_size = 4;
    off = 16;
    break;
  case D3DFVF_XYZW:
    l.position_size = 4;
    off = 16;
    break;
  case D3DFVF_XYZ:
    l.position_size = 3;
    off = 12;
    break;
  case D3DFVF_XYZB1:
  case D3DFVF_XYZB2:
  case D3DFVF_XYZB3:
  case D3DFVF_XYZB4:
  case D3DFVF_XYZB5:
    l.position_size = 3;
    l.weights = (int)(((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZB1) / 2 + 1);
    l.last_beta_ubyte4 = (fvf & D3DFVF_LASTBETA_UBYTE4) != 0;
    l.blend = 12;
    off = 12 + 4 * l.weights;
    break;
  default: break;
  }
  if (fvf & D3DFVF_NORMAL) {
    l.normal = (int)off;
    off += 12;
  }
  if (fvf & D3DFVF_PSIZE) {
    l.psize = (int)off;
    off += 4;
  }
  if (fvf & D3DFVF_DIFFUSE) {
    l.diffuse = (int)off;
    off += 4;
  }
  if (fvf & D3DFVF_SPECULAR) {
    l.specular = (int)off;
    off += 4;
  }
  l.tex_count = (int)((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
  if (l.tex_count > 8)
    l.tex_count = 8;
  for (int i = 0; i < l.tex_count; i++) {
    int fmt = (int)((fvf >> (16 + 2 * i)) & 3);
    int size = fmt == D3DFVF_TEXTUREFORMAT1 ? 1 : fmt == D3DFVF_TEXTUREFORMAT3 ? 3 : fmt == D3DFVF_TEXTUREFORMAT4 ? 4 : 2;
    l.tex_offset[i] = (int)off;
    l.tex_size[i] = size;
    off += 4 * size;
  }
  l.stride = off;
  return l;
}

namespace {

std::unordered_map<std::string, std::unique_ptr<FfpProgram>> Cache;

const char *Arg_Expr(int arg, int stage, bool alpha_context, std::string &tmp) {
  int sel = arg & D3DTA_SELECTMASK;
  const char *base;
  switch (sel) {
  case D3DTA_DIFFUSE: base = "v_diffuse"; break;
  case D3DTA_CURRENT: base = stage == 0 ? "v_diffuse" : "cur"; break;
  case D3DTA_TEXTURE: base = nullptr; break;
  case D3DTA_TFACTOR: base = "u_tfactor"; break;
  case D3DTA_SPECULAR: base = "v_specular"; break;
  case D3DTA_TEMP: base = "tmp"; break;
  default: base = "u_tfactor"; break; // CONSTANT: per-stage constants are unused by the engine
  }
  std::string b = base ? std::string(base) : "t" + std::to_string(stage);
  if (arg & D3DTA_ALPHAREPLICATE)
    b = "vec4(" + b + ".a)";
  if (arg & D3DTA_COMPLEMENT)
    b = "(vec4(1.0) - " + b + ")";
  (void)alpha_context;
  tmp = b;
  return tmp.c_str();
}

// Expression for a texture op applied to vec4 operands a0/a1/a2. `swz` picks
// the channels (".rgb" or ".a").
std::string Op_Expr(int op, const std::string &a0, const std::string &a1, const std::string &a2, const std::string &swz,
                    int stage) {
  auto c = [&](const std::string &x) { return "(" + x + ")" + swz; };
  std::string t = "t" + std::to_string(stage);
  switch (op) {
  case D3DTOP_SELECTARG1: return c(a1);
  case D3DTOP_SELECTARG2: return c(a2);
  case D3DTOP_MODULATE: return c(a1) + " * " + c(a2);
  case D3DTOP_MODULATE2X: return "2.0 * " + c(a1) + " * " + c(a2);
  case D3DTOP_MODULATE4X: return "4.0 * " + c(a1) + " * " + c(a2);
  case D3DTOP_ADD: return c(a1) + " + " + c(a2);
  case D3DTOP_ADDSIGNED: return c(a1) + " + " + c(a2) + " - 0.5";
  case D3DTOP_ADDSIGNED2X: return "2.0 * (" + c(a1) + " + " + c(a2) + " - 0.5)";
  case D3DTOP_SUBTRACT: return c(a1) + " - " + c(a2);
  case D3DTOP_ADDSMOOTH: return c(a1) + " + " + c(a2) + " - " + c(a1) + " * " + c(a2);
  case D3DTOP_BLENDDIFFUSEALPHA: return "mix(" + c(a2) + ", " + c(a1) + ", v_diffuse.a)";
  case D3DTOP_BLENDTEXTUREALPHA: return "mix(" + c(a2) + ", " + c(a1) + ", " + t + ".a)";
  case D3DTOP_BLENDFACTORALPHA: return "mix(" + c(a2) + ", " + c(a1) + ", u_tfactor.a)";
  case D3DTOP_BLENDTEXTUREALPHAPM: return c(a1) + " + " + c(a2) + " * (1.0 - " + t + ".a)";
  case D3DTOP_BLENDCURRENTALPHA: return "mix(" + c(a2) + ", " + c(a1) + ", cur.a)";
  case D3DTOP_PREMODULATE: return c(a1);
  case D3DTOP_MODULATEALPHA_ADDCOLOR:
    return swz == ".a" ? c(a1) : "(" + a1 + ").rgb + (" + a1 + ").a * (" + a2 + ").rgb";
  case D3DTOP_MODULATECOLOR_ADDALPHA:
    return swz == ".a" ? c(a1) : "(" + a1 + ").rgb * (" + a2 + ").rgb + vec3((" + a1 + ").a)";
  case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
    return swz == ".a" ? c(a1) : "(1.0 - (" + a1 + ").a) * (" + a2 + ").rgb + (" + a1 + ").rgb";
  case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
    return swz == ".a" ? c(a1) : "(vec3(1.0) - (" + a1 + ").rgb) * (" + a2 + ").rgb + vec3((" + a1 + ").a)";
  case D3DTOP_DOTPRODUCT3: {
    std::string d = "4.0 * dot((" + a1 + ").rgb - 0.5, (" + a2 + ").rgb - 0.5)";
    return swz == ".a" ? d : "vec3(" + d + ")";
  }
  case D3DTOP_MULTIPLYADD: return c(a0) + " + " + c(a1) + " * " + c(a2);
  case D3DTOP_LERP: return "mix(" + c(a2) + ", " + c(a1) + ", " + c(a0) + ")";
  case D3DTOP_BUMPENVMAP:
  case D3DTOP_BUMPENVMAPLUMINANCE: return swz == ".a" ? "cur.a" : "cur.rgb";
  default: return c(a1);
  }
}

std::string Vertex_Source(const FfpKey &k) {
  FvfLayout l = Fvf_Layout(k.fvf);
  std::ostringstream s;
  s << "#version 300 es\nprecision highp float;\n";
  s << "in vec4 a_position;\n";
  if (l.weights)
    s << "in vec4 a_weights;\n";
  if (l.normal >= 0)
    s << "in vec3 a_normal;\n";
  if (l.diffuse >= 0)
    s << "in vec4 a_diffuse;\n";
  if (l.specular >= 0)
    s << "in vec4 a_specular;\n";
  for (int i = 0; i < l.tex_count; i++)
    s << "in vec4 a_texcoord" << i << ";\n";
  s << "uniform mat4 u_world[4];\nuniform mat4 u_view;\nuniform mat4 u_proj;\nuniform mat3 u_normal_matrix;\n";
  s << "uniform vec4 u_viewport;\nuniform float u_point_size;\n";
  s << "uniform vec4 u_mat_diffuse, u_mat_ambient, u_mat_specular, u_mat_emissive;\nuniform float u_mat_power;\n";
  s << "uniform vec4 u_ambient;\n";
  for (int i = 0; i < 8; i++)
    if (k.light_types[i])
      s << "uniform vec4 u_light_diffuse" << i << ", u_light_specular" << i << ", u_light_ambient" << i << ";\n"
        << "uniform vec3 u_light_position" << i << ", u_light_direction" << i << ";\n"
        << "uniform vec4 u_light_atten" << i << ";\nuniform vec3 u_light_spot" << i << ";\n";
  s << "uniform vec4 u_fog_params;\n"; // start, end, density, unused
  for (int i = 0; i < k.num_stages; i++)
    if (k.stages[i].ttff)
      s << "uniform mat4 u_tex_matrix" << i << ";\n";
  const char *fl = k.flat ? "flat " : "";
  s << fl << "out vec4 v_diffuse;\n" << fl << "out vec4 v_specular;\nout float v_fog;\n";
  for (int i = 0; i < k.num_stages; i++)
    s << "out vec4 v_tc" << i << ";\n";

  s << "void main() {\n";
  s << "  vec4 diffuse_in = " << (l.diffuse >= 0 ? "a_diffuse.zyxw" : "vec4(1.0)") << ";\n";
  s << "  vec4 specular_in = " << (l.specular >= 0 ? "a_specular.zyxw" : "vec4(0.0)") << ";\n";
  s << "  vec3 view_pos = vec3(0.0);\n  vec3 view_normal = vec3(0.0, 0.0, -1.0);\n";
  if (l.rhw) {
    s << "  float w = a_position.w > 0.0 ? 1.0 / a_position.w : 1.0;\n";
    s << "  vec2 ndc = ((a_position.xy + 0.5) - u_viewport.xy) / u_viewport.zw * 2.0 - 1.0;\n";
    s << "  gl_Position = vec4(ndc * w, (a_position.z * 2.0 - 1.0) * w, w);\n";
    s << "  view_pos = vec3(0.0, 0.0, w);\n";
  } else {
    if (k.blend_weights > 1) {
      int n = k.blend_weights;
      s << "  float wsum = 0.0;\n  vec4 world_pos = vec4(0.0);\n  vec3 world_n = vec3(0.0);\n";
      for (int i = 0; i < n - 1; i++)
        s << "  { float wi = a_weights[" << i << "]; wsum += wi; world_pos += wi * (u_world[" << i
          << "] * vec4(a_position.xyz, 1.0));"
          << (l.normal >= 0 ? (" world_n += wi * (mat3(u_world[" + std::to_string(i) + "]) * a_normal);") : "") << " }\n";
      s << "  { float wi = 1.0 - wsum; world_pos += wi * (u_world[" << n - 1 << "] * vec4(a_position.xyz, 1.0));"
        << (l.normal >= 0 ? (" world_n += wi * (mat3(u_world[" + std::to_string(n - 1) + "]) * a_normal);") : "")
        << " }\n";
      s << "  vec4 vp = u_view * world_pos;\n";
      if (l.normal >= 0)
        s << "  view_normal = mat3(u_view) * world_n;\n";
    } else {
      s << "  vec4 vp = u_view * (u_world[0] * vec4(a_position.xyz, " << (l.position_size == 4 ? "a_position.w" : "1.0")
        << "));\n";
      if (l.normal >= 0)
        s << "  view_normal = u_normal_matrix * a_normal;\n";
    }
    s << "  view_pos = vp.xyz;\n";
    if (l.normal >= 0 && (k.normalize || k.lighting))
      s << "  view_normal = normalize(view_normal);\n";
    s << "  vec4 clip = u_proj * vp;\n";
    // D3D -> GL: flip y, depth [0,w] -> [-w,w], D3D9 half-pixel offset
    s << "  clip.y = -clip.y;\n  clip.z = 2.0 * clip.z - clip.w;\n";
    s << "  clip.xy += vec2(1.0, 1.0) / u_viewport.zw * clip.w;\n";
    s << "  gl_Position = clip;\n";
  }
  s << "  gl_PointSize = u_point_size;\n";

  // Lighting
  if (k.lighting && !l.rhw) {
    auto src = [&](int which, const char *mat) -> std::string {
      if (!k.color_vertex)
        return mat;
      if (which == D3DMCS_COLOR1 && l.diffuse >= 0)
        return "diffuse_in";
      if (which == D3DMCS_COLOR2 && l.specular >= 0)
        return "specular_in";
      return mat;
    };
    s << "  vec4 m_diffuse = " << src(k.diffuse_src, "u_mat_diffuse") << ";\n";
    s << "  vec4 m_ambient = " << src(k.ambient_src, "u_mat_ambient") << ";\n";
    s << "  vec4 m_specular = " << src(k.specular_src, "u_mat_specular") << ";\n";
    s << "  vec4 m_emissive = " << src(k.emissive_src, "u_mat_emissive") << ";\n";
    s << "  vec3 col = m_emissive.rgb + m_ambient.rgb * u_ambient.rgb;\n  vec3 spec = vec3(0.0);\n";
    s << "  vec3 eye = " << (k.local_viewer ? "normalize(-view_pos)" : "vec3(0.0, 0.0, -1.0)") << ";\n";
    for (int i = 0; i < 8; i++) {
      int t = k.light_types[i];
      if (!t)
        continue;
      std::string n = std::to_string(i);
      s << "  {\n";
      if (t == D3DLIGHT_DIRECTIONAL) {
        s << "    vec3 L = -u_light_direction" << n << ";\n    float att = 1.0;\n";
      } else {
        s << "    vec3 d = u_light_position" << n << " - view_pos;\n    float dist = length(d);\n";
        s << "    vec3 L = d / max(dist, 1e-6);\n";
        s << "    float att = dist > u_light_atten" << n << ".w ? 0.0 : 1.0 / max(u_light_atten" << n << ".x + u_light_atten"
          << n << ".y * dist + u_light_atten" << n << ".z * dist * dist, 1e-6);\n";
        if (t == D3DLIGHT_SPOT) {
          // u_light_spot: cos(theta/2), cos(phi/2), falloff
          s << "    float rho = dot(-L, u_light_direction" << n << ");\n";
          s << "    float spot = rho > u_light_spot" << n << ".x ? 1.0 : (rho <= u_light_spot" << n << ".y ? 0.0 : pow(max((rho - u_light_spot"
            << n << ".y) / max(u_light_spot" << n << ".x - u_light_spot" << n << ".y, 1e-6), 0.0), u_light_spot" << n << ".z));\n";
          s << "    att *= spot;\n";
        }
      }
      s << "    float ndl = max(dot(view_normal, L), 0.0);\n";
      s << "    col += att * (m_ambient.rgb * u_light_ambient" << n << ".rgb + m_diffuse.rgb * u_light_diffuse" << n
        << ".rgb * ndl);\n";
      if (k.specular)
        s << "    if (ndl > 0.0) { vec3 H = normalize(L + eye); spec += att * m_specular.rgb * u_light_specular" << n
          << ".rgb * pow(max(dot(view_normal, H), 0.0), max(u_mat_power, 1e-4)); }\n";
      s << "  }\n";
    }
    s << "  v_diffuse = vec4(clamp(col, 0.0, 1.0), m_diffuse.a);\n";
    s << "  v_specular = vec4(clamp(spec, 0.0, 1.0), specular_in.a);\n";
  } else {
    s << "  v_diffuse = diffuse_in;\n  v_specular = specular_in;\n";
  }

  // Fog
  if (k.fog) {
    if (l.rhw && k.fog == 1) {
      s << "  v_fog = specular_in.a;\n"; // pre-transformed vertices carry fog in specular alpha
    } else {
      s << "  float fd = " << (k.range_fog ? "length(view_pos)" : "abs(view_pos.z)") << ";\n";
      if (k.fog_mode == D3DFOG_LINEAR)
        s << "  v_fog = clamp((u_fog_params.y - fd) / max(u_fog_params.y - u_fog_params.x, 1e-6), 0.0, 1.0);\n";
      else if (k.fog_mode == D3DFOG_EXP)
        s << "  v_fog = clamp(exp(-u_fog_params.z * fd), 0.0, 1.0);\n";
      else
        s << "  v_fog = clamp(exp(-(u_fog_params.z * fd) * (u_fog_params.z * fd)), 0.0, 1.0);\n";
    }
  } else {
    s << "  v_fog = 1.0;\n";
  }

  // Texture coordinates
  for (int i = 0; i < k.num_stages; i++) {
    const FfpKey::Stage &st = k.stages[i];
    std::string tc;
    int count = 2;
    switch (st.tci) {
    case 1: tc = "vec4(view_normal, 1.0)"; count = 3; break;
    case 2: tc = "vec4(view_pos, 1.0)"; count = 3; break;
    case 3: tc = "vec4(reflect(normalize(view_pos), view_normal), 1.0)"; count = 3; break;
    default: {
      int ci = st.coord_index;
      if (ci < l.tex_count) {
        count = l.tex_size[ci];
        std::string a = "a_texcoord" + std::to_string(ci);
        // D3D pads missing components so a 2D coord picks up the matrix's 3rd row
        if (count == 1)
          tc = "vec4(" + a + ".x, 1.0, 0.0, 0.0)";
        else if (count == 2)
          tc = "vec4(" + a + ".xy, 1.0, 0.0)";
        else if (count == 3)
          tc = "vec4(" + a + ".xyz, 1.0)";
        else
          tc = a;
      } else {
        tc = "vec4(0.0, 0.0, 0.0, 1.0)";
      }
    }
    }
    if (st.ttff)
      s << "  v_tc" << i << " = u_tex_matrix" << i << " * " << tc << ";\n";
    else
      s << "  v_tc" << i << " = " << tc << ";\n";
  }
  s << "}\n";
  return s.str();
}

std::string Fragment_Source(const FfpKey &k) {
  std::ostringstream s;
  s << "#version 300 es\nprecision highp float;\n";
  const char *fl = k.flat ? "flat " : "";
  s << fl << "in vec4 v_diffuse;\n" << fl << "in vec4 v_specular;\nin float v_fog;\n";
  for (int i = 0; i < k.num_stages; i++)
    s << "in vec4 v_tc" << i << ";\nuniform sampler2D u_sampler" << i << ";\nuniform mat2 u_bump_mat" << i
      << ";\nuniform vec2 u_bump_lum" << i << ";\n";
  s << "uniform vec4 u_tfactor;\nuniform vec4 u_fog_color;\nuniform float u_alpha_ref;\nuniform bool u_specular_add;\n";
  s << "out vec4 frag;\n";
  s << "void main() {\n  vec4 cur = v_diffuse;\n  vec4 tmp = vec4(0.0);\n";
  for (int i = 0; i < k.num_stages; i++) {
    const FfpKey::Stage &st = k.stages[i];
    std::string n = std::to_string(i);
    // Projected texture transforms divide by the last used component.
    std::string coord = "v_tc" + n + ".xy";
    if (st.ttff & D3DTTFF_PROJECTED) {
      int cnt = st.ttff & 0xFF;
      const char *last = cnt == 2 ? "y" : cnt == 3 ? "z" : "w";
      coord = "(v_tc" + n + ".xy / v_tc" + n + "." + last + ")";
    }
    if (i > 0 && (k.stages[i - 1].color_op == D3DTOP_BUMPENVMAP || k.stages[i - 1].color_op == D3DTOP_BUMPENVMAPLUMINANCE))
      coord = "(" + coord + " + u_bump_mat" + std::to_string(i - 1) + " * (t" + std::to_string(i - 1) + ".rg * 2.0 - 1.0))";
    if (st.has_texture)
      s << "  vec4 t" << n << " = texture(u_sampler" << n << ", " << coord << ");\n";
    else
      s << "  vec4 t" << n << " = vec4(1.0);\n";
    std::string a0c, a1c, a2c, a0a, a1a, a2a;
    std::string c0 = Arg_Expr(st.color_arg0, i, false, a0c), c1 = Arg_Expr(st.color_arg1, i, false, a1c),
                c2 = Arg_Expr(st.color_arg2, i, false, a2c);
    std::string al0 = Arg_Expr(st.alpha_arg0, i, true, a0a), al1 = Arg_Expr(st.alpha_arg1, i, true, a1a),
                al2 = Arg_Expr(st.alpha_arg2, i, true, a2a);
    std::string target = st.result_arg == D3DTA_TEMP ? "tmp" : "cur";
    s << "  {\n    vec4 r = " << target << ";\n";
    s << "    r.rgb = clamp(" << Op_Expr(st.color_op, c0, c1, c2, ".rgb", i) << ", 0.0, 1.0);\n";
    int aop = st.alpha_op == D3DTOP_DISABLE ? D3DTOP_SELECTARG1 : st.alpha_op;
    if (st.alpha_op == D3DTOP_DISABLE)
      s << "    r.a = " << (i == 0 ? "v_diffuse.a" : "cur.a") << ";\n";
    else
      s << "    r.a = clamp(" << Op_Expr(aop, al0, al1, al2, ".a", i) << ", 0.0, 1.0);\n";
    if (st.color_op == D3DTOP_BUMPENVMAPLUMINANCE)
      s << "    r.rgb *= clamp(t" << n << ".b * u_bump_lum" << n << ".x + u_bump_lum" << n << ".y, 0.0, 1.0);\n";
    s << "    " << target << " = r;\n  }\n";
  }
  s << "  vec4 color = cur;\n";
  if (k.specular)
    s << "  color.rgb = clamp(color.rgb + v_specular.rgb, 0.0, 1.0);\n";
  if (k.fog)
    s << "  color.rgb = mix(u_fog_color.rgb, color.rgb, v_fog);\n";
  if (k.alpha_test) {
    const char *cmp = "true";
    switch (k.alpha_test) {
    case D3DCMP_NEVER: cmp = "false"; break;
    case D3DCMP_LESS: cmp = "color.a < u_alpha_ref"; break;
    case D3DCMP_EQUAL: cmp = "abs(color.a - u_alpha_ref) < 0.5 / 255.0"; break;
    case D3DCMP_LESSEQUAL: cmp = "color.a <= u_alpha_ref"; break;
    case D3DCMP_GREATER: cmp = "color.a > u_alpha_ref"; break;
    case D3DCMP_NOTEQUAL: cmp = "abs(color.a - u_alpha_ref) >= 0.5 / 255.0"; break;
    case D3DCMP_GREATEREQUAL: cmp = "color.a >= u_alpha_ref"; break;
    default: cmp = "true"; break;
    }
    s << "  if (!(" << cmp << ")) discard;\n";
  }
  s << "  frag = color;\n}\n";
  return s.str();
}

GLuint Compile(GLenum type, const std::string &src) {
  GLuint sh = glCreateShader(type);
  const char *p = src.c_str();
  glShaderSource(sh, 1, &p, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    fprintf(stderr, "[d3d9] shader compile failed:\n%s\n--- source ---\n%s\n", log, src.c_str());
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

} // namespace

FfpProgram *Ffp_Program_For(const FfpKey &key) {
  std::string id = key.Serialize();
  auto it = Cache.find(id);
  if (it != Cache.end())
    return it->second.get();

  GLuint vs = Compile(GL_VERTEX_SHADER, Vertex_Source(key));
  GLuint fs = Compile(GL_FRAGMENT_SHADER, Fragment_Source(key));
  if (!vs || !fs) {
    Cache[id] = nullptr;
    return nullptr;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
    fprintf(stderr, "[d3d9] program link failed: %s\n", log);
    glDeleteProgram(prog);
    Cache[id] = nullptr;
    return nullptr;
  }
  auto p = std::make_unique<FfpProgram>();
  p->program = prog;
  auto U = [&](const std::string &name) { return glGetUniformLocation(prog, name.c_str()); };
  for (int i = 0; i < 4; i++)
    p->u_world[i] = U("u_world[" + std::to_string(i) + "]");
  p->u_view = U("u_view");
  p->u_proj = U("u_proj");
  p->u_normal_matrix = U("u_normal_matrix");
  p->u_viewport = U("u_viewport");
  p->u_point_size = U("u_point_size");
  p->u_mat_diffuse = U("u_mat_diffuse");
  p->u_mat_ambient = U("u_mat_ambient");
  p->u_mat_specular = U("u_mat_specular");
  p->u_mat_emissive = U("u_mat_emissive");
  p->u_mat_power = U("u_mat_power");
  p->u_ambient = U("u_ambient");
  for (int i = 0; i < 8; i++) {
    std::string n = std::to_string(i);
    p->u_light_diffuse[i] = U("u_light_diffuse" + n);
    p->u_light_specular[i] = U("u_light_specular" + n);
    p->u_light_ambient[i] = U("u_light_ambient" + n);
    p->u_light_position[i] = U("u_light_position" + n);
    p->u_light_direction[i] = U("u_light_direction" + n);
    p->u_light_atten[i] = U("u_light_atten" + n);
    p->u_light_spot[i] = U("u_light_spot" + n);
    p->u_tex_matrix[i] = U("u_tex_matrix" + n);
    p->u_bump_mat[i] = U("u_bump_mat" + n);
    p->u_bump_lum[i] = U("u_bump_lum" + n);
    p->u_sampler[i] = U("u_sampler" + n);
    p->a_texcoord[i] = glGetAttribLocation(prog, ("a_texcoord" + n).c_str());
  }
  p->u_fog_color = U("u_fog_color");
  p->u_fog_params = U("u_fog_params");
  p->u_tfactor = U("u_tfactor");
  p->u_alpha_ref = U("u_alpha_ref");
  p->a_position = glGetAttribLocation(prog, "a_position");
  p->a_weights = glGetAttribLocation(prog, "a_weights");
  p->a_normal = glGetAttribLocation(prog, "a_normal");
  p->a_diffuse = glGetAttribLocation(prog, "a_diffuse");
  p->a_specular = glGetAttribLocation(prog, "a_specular");
  glUseProgram(prog);
  for (int i = 0; i < 8; i++)
    if (p->u_sampler[i] >= 0)
      glUniform1i(p->u_sampler[i], i);
  FfpProgram *raw = p.get();
  Cache[id] = std::move(p);
  return raw;
}

void Ffp_Clear_Cache() {
  for (auto &e : Cache)
    if (e.second)
      glDeleteProgram(e.second->program);
  Cache.clear();
}

} // namespace rebel_d3d
