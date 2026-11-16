// Linux port only. Xcode compiles every file under engine/ (synchronized
// folder), so this file is empty on macOS.
#ifndef __APPLE__  // Linux port only
// ----------------------------------------------------------------------- //
//
// MODULE  : ltgl.cpp  (Linux port)
//
// PURPOSE : ltmetal.h implemented on OpenGL 4.6 core -- the same API, the same
//           shaders' arithmetic (translated from ltmetal.mm's MSL), so
//           ltworldmetal / ltcomp / the host do not know which GPU API runs.
//
// ⚠️ CONVENTIONS ARE METAL'S, ON PURPOSE. glClipControl(GL_UPPER_LEFT,
//    GL_ZERO_TO_ONE) gives clip z 0..w (ltworldmetal's ToMetalClip output) and
//    a framebuffer whose row 0 is the TOP -- so viewports, scissors, texture
//    rows, readbacks and the mirror's screen lookup are all top-down exactly
//    as ltmetal.mm has them. Only the final present into the window's default
//    framebuffer (bottom-up) flips, in one place.
//
// ⚠️ ONE CONTEXT, ONE THREAD: the window's (ltglwin.h). Headless, a hidden
//    window's.
//
// ----------------------------------------------------------------------- //

#include "ltmetal.h"
#include "lt3d.h"
#include "ltcomp.h"
#include "ltwindow.h"
#include "ltglwin.h"

#include <GL/glcorearb.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <SDL3/SDL_video.h>

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif

// ---- GL entry points, loaded through SDL ---------------------------------- //

#define LTGL_FUNCS(X) \
    X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
    X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLCLEARPROC, glClear) X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARDEPTHFPROC, glClearDepthf) \
    X(PFNGLCOLORMASKPROC, glColorMask) X(PFNGLDEPTHMASKPROC, glDepthMask) X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) X(PFNGLBLENDCOLORPROC, glBlendColor) \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation) \
    X(PFNGLCULLFACEPROC, glCullFace) X(PFNGLFRONTFACEPROC, glFrontFace) X(PFNGLCLIPCONTROLPROC, glClipControl) \
    X(PFNGLPROVOKINGVERTEXPROC, glProvokingVertex) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers) X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf) X(PFNGLBINDSAMPLERPROC, glBindSampler) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBFORMATPROC, glVertexAttribFormat) X(PFNGLVERTEXATTRIBBINDINGPROC, glVertexAttribBinding) \
    X(PFNGLBINDVERTEXBUFFERPROC, glBindVertexBuffer) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM4FVPROC, glUniform4fv) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLFINISHPROC, glFinish) X(PFNGLCOPYBUFFERSUBDATAPROC, glCopyBufferSubData) X(PFNGLGETERRORPROC, glGetError) \
    X(PFNGLGENQUERIESPROC, glGenQueries) X(PFNGLQUERYCOUNTERPROC, glQueryCounter) \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v) X(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv)

#define X(T, n) static T p_##n = 0;
LTGL_FUNCS(X)
#undef X

static bool LoadGL()
{
    static int s = -1;
    if (s >= 0) return s != 0;
    bool ok = true;
#define X(T, n) p_##n = (T)LTGLWin_GetProc(#n); if (!p_##n) { fprintf(stderr, "[GL] missing %s\n", #n); ok = false; }
    LTGL_FUNCS(X)
#undef X
    s = ok ? 1 : 0;
    return ok;
}

// Short names inside this file.
#define X(T, n) static T& n = p_##n;
LTGL_FUNCS(X)
#undef X


// ----------------------------------------------------------------------- //
// THE SHADERS -- ltmetal.mm's, in GLSL. Same arithmetic, line for line.
// ----------------------------------------------------------------------- //

static const char* kCommon = R"GLSL(
#version 450 core
layout(std140, binding = 1) uniform U {
    mat4  mvp;
    int   alphaTest;
    float alphaRef;
    int   blend;
    float constAlpha;
    int   affineColour;
    int   a2c;
    vec2  fogK;
    vec4  fogColor;
    float fogNear;
    float fogFar;
    int   fogOn;
    int   screen;
} u;
)GLSL";

static const char* kWorldVS = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aRGB;
layout(location = 3) in vec2 aLUV;
out vec2 vUV; out float vFogZ; out vec2 vFogXY;
noperspective out vec3 vRGBa; out vec3 vRGBp;
out vec4 vClip; out vec3 vOPos; flat out float vNoLight; out vec2 vLUV;
void main()
{
    vec4 p = u.mvp * vec4(aPos, 1.0);
    gl_Position = p;
    vUV = aUV;
    vRGBa = abs(aRGB); vRGBp = abs(aRGB);
    vFogZ = p.w;
    vFogXY = p.xy * u.fogK;
    vClip = p;
    vOPos = aPos;
    vNoLight = (aRGB.r < 0.0 || aRGB.g < 0.0 || aRGB.b < 0.0) ? 1.0 : 0.0;
    vLUV = aLUV;
}
)GLSL";

static const char* kSkinVS = R"GLSL(
struct SCorner { uint vert; float u; float v; uint pad; };
struct SVert   { float px, py, pz; uint wFirst; float nx, ny, nz; uint wCount; };
struct SWeight { float ox, oy, oz, bias; uint node, p0, p1, p2; };
layout(std430, binding = 2) readonly buffer BC { SCorner cor[]; };
layout(std430, binding = 3) readonly buffer BV { SVert vtx[]; };
layout(std430, binding = 4) readonly buffer BW { SWeight wts[]; };
layout(std430, binding = 5) readonly buffer BP { vec4 pal[]; };
layout(std140, binding = 6) uniform SkinU {
    vec4 nrm0, nrm1, nrm2;
    int pose, mirror, mirrorU, lit, skinNormal, biasNorm, nLights, nNodes;
    vec4 ambient;
    vec4 dir[8];
    vec4 color[8];
} s;
out vec2 vUV; out float vFogZ; out vec2 vFogXY;
noperspective out vec3 vRGBa; out vec3 vRGBp;
out vec4 vClip; out vec3 vOPos; flat out float vNoLight; out vec2 vLUV;
void main()
{
    SCorner c = cor[gl_VertexID];
    SVert   v = vtx[c.vert];
    vec3 p = vec3(v.px, v.py, v.pz);
    vec3 n = vec3(v.nx, v.ny, v.nz);
    if (s.pose != 0 && v.wCount > 0u)
    {
        vec3 ps = vec3(0.0), ns = vec3(0.0);
        float sum = 0.0;
        for (uint i = 0u; i < v.wCount; ++i)
        {
            SWeight w = wts[v.wFirst + i];
            if (w.node >= uint(s.nNodes)) continue;
            uint b = w.node * 6u;
            vec3 o = vec3(w.ox, w.oy, w.oz);
            ps += vec3(dot(pal[b+0u].xyz, o) + pal[b+0u].w * w.bias,
                       dot(pal[b+1u].xyz, o) + pal[b+1u].w * w.bias,
                       dot(pal[b+2u].xyz, o) + pal[b+2u].w * w.bias);
            ns += vec3(dot(pal[b+3u].xyz, n), dot(pal[b+4u].xyz, n), dot(pal[b+5u].xyz, n)) * w.bias;
            sum += w.bias;
        }
        if (s.biasNorm != 0 && sum > 1e-6) ps *= (1.0 / sum);
        p = ps;
        if (s.skinNormal != 0) { float l = length(ns); if (l > 1e-6) n = ns / l; }
    }
    if (s.mirror != 0) { p.x = -p.x; n.x = -n.x; }
    vec3 nw = vec3(dot(s.nrm0.xyz, n), dot(s.nrm1.xyz, n), dot(s.nrm2.xyz, n));
    vec3 rgb;
    if (s.lit != 0)
    {
        float nl = length(nw);
        if (nl > 1e-6) nw = nw / nl;
        vec3 cc = s.ambient.rgb;
        for (int li = 0; li < s.nLights; ++li)
        {
            float dd = -dot(nw, s.dir[li].xyz);
            if (dd <= 0.0) continue;
            cc += s.color[li].rgb * dd;
        }
        rgb = min(cc, vec3(1.0));
    }
    else
    {
        float d = abs(dot(nw, vec3(-0.40824829, 0.81649658, 0.40824829)));
        rgb = vec3(min(0.35 + 0.65 * d, 1.0));
    }
    vec4 cp = u.mvp * vec4(p, 1.0);
    gl_Position = cp;
    vUV = vec2(s.mirrorU != 0 ? -c.u : c.u, c.v);
    vRGBa = rgb; vRGBp = rgb;
    vFogZ = cp.w;
    vFogXY = cp.xy * u.fogK;
    vClip = cp;
    vOPos = p;
    vNoLight = 1.0;
    vLUV = vec2(-1.0);
}
)GLSL";

static const char* kDynL = R"GLSL(
layout(std140, binding = 3) uniform DynL {
    int n; float sat; float scale; int pad;
    vec4 eye;
    vec4 pos[32];
    vec4 col[32];
} dl;
vec3 DynLightAt(vec3 p, vec3 nrm)
{
    vec3 acc = vec3(0.0);
    float fEye = dot(nrm, dl.eye.xyz - p);
    for (int i = 0; i < dl.n; ++i)
    {
        vec3 d = p - dl.pos[i].xyz;
        float x = 1.0 - dot(d, d) * dl.pos[i].w;
        if (x <= 0.0) continue;
        if (dl.col[i].w != 0.0 && dot(nrm, dl.pos[i].xyz - p) * fEye < 0.0) continue;
        acc += dl.col[i].rgb * min(1.0, x * dl.sat);
    }
    return dl.scale * min(acc, vec3(1.0));
}
layout(binding = 2) uniform sampler2D lmap;
uniform float lmScale;
)GLSL";

static const char* kWorldFS = R"GLSL(
in vec2 vUV; in float vFogZ; in vec2 vFogXY;
noperspective in vec3 vRGBa; in vec3 vRGBp;
in vec4 vClip; in vec3 vOPos; flat in float vNoLight; in vec2 vLUV;
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 oColor;
void main()
{
    vec3 nrm = cross(dFdx(vOPos), dFdy(vOPos));
    if (u.screen != 0)
    {
        vec2 ndc = vClip.xy / vClip.w;
        oColor = vec4(texture(tex, vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)).rgb, 1.0);
        return;
    }
    vec4 t = texture(tex, vUV);
    float fw = max(fwidth(t.a), 1.0 / 1024.0);
    float cover = 1.0;
    if (u.alphaTest != 0)
    {
        if (u.a2c != 0)
        {
            cover = clamp((t.a - u.alphaRef) / fw + 0.5, 0.0, 1.0);
            if (cover <= 0.0) discard;
        }
        else if (t.a < u.alphaRef) discard;
    }
    float a = (u.blend == 2 ? 1.0 : t.a) * u.constAlpha;
    vec3 vc = (u.affineColour != 0) ? vRGBa : vRGBp;
    if (vLUV.x >= 0.0) vc *= texture(lmap, vLUV).rgb * lmScale;
    if (dl.n > 0 && vNoLight == 0.0) vc += DynLightAt(vOPos, nrm);
    vec3 c = t.rgb * vc;
    if (u.fogOn != 0 && u.fogFar != u.fogNear)
    {
        float fd = (u.fogK.x != 0.0 || u.fogK.y != 0.0) ? length(vec3(vFogXY, vFogZ)) : vFogZ;
        float f = clamp((fd - u.fogNear) / (u.fogFar - u.fogNear), 0.0, 1.0);
        c = mix(c, u.fogColor.rgb, f);
    }
    oColor = vec4(c, u.blend != 0 ? clamp(a, 0.0, 1.0) : cover);
}
)GLSL";

static const char* kEnvVS = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aRGB;
layout(location = 3) in vec2 aUV2;
layout(location = 4) in vec2 aLUV;
out vec2 vUV; out vec2 vUV2; out float vFogZ; out vec2 vFogXY;
noperspective out vec3 vRGBa; out vec3 vRGBp;
out vec3 vOPos; flat out float vNoLight; out vec2 vLUV;
void main()
{
    vec4 p = u.mvp * vec4(aPos, 1.0);
    gl_Position = p;
    vUV = aUV; vUV2 = aUV2;
    vRGBa = abs(aRGB); vRGBp = abs(aRGB);
    vFogZ = p.w;
    vFogXY = p.xy * u.fogK;
    vOPos = aPos;
    vNoLight = (aRGB.r < 0.0 || aRGB.g < 0.0 || aRGB.b < 0.0) ? 1.0 : 0.0;
    vLUV = aLUV;
}
)GLSL";

static const char* kEnvFS = R"GLSL(
in vec2 vUV; in vec2 vUV2; in float vFogZ; in vec2 vFogXY;
noperspective in vec3 vRGBa; in vec3 vRGBp;
in vec3 vOPos; flat in float vNoLight; in vec2 vLUV;
layout(binding = 0) uniform sampler2D tex;
layout(binding = 1) uniform sampler2D env;
uniform int baseAlpha;      // bit 0: base alpha counts; bit 1: chrome (5en)
layout(location = 0) out vec4 oColor;
void main()
{
    vec3 nrm = cross(dFdx(vOPos), dFdy(vOPos));
    vec4 t = texture(tex, vUV);
    if (u.alphaTest != 0 && t.a < u.alphaRef) discard;
    float a = ((baseAlpha & 1) != 0) ? t.a : 1.0;
    vec3 e = texture(env, vUV2).rgb;
    vec3 b = ((baseAlpha & 2) != 0) ? mix(e, t.rgb, a) : min(t.rgb + a * e, vec3(1.0));
    vec3 vc = (u.affineColour != 0) ? vRGBa : vRGBp;
    if (vLUV.x >= 0.0) vc *= texture(lmap, vLUV).rgb * lmScale;
    if (dl.n > 0 && vNoLight == 0.0) vc += DynLightAt(vOPos, nrm);
    vec3 c = b * vc;
    if (u.fogOn != 0 && u.fogFar != u.fogNear)
    {
        float fd = (u.fogK.x != 0.0 || u.fogK.y != 0.0) ? length(vec3(vFogXY, vFogZ)) : vFogZ;
        float f = clamp((fd - u.fogNear) / (u.fogFar - u.fogNear), 0.0, 1.0);
        c = mix(c, u.fogColor.rgb, f);
    }
    oColor = vec4(c, 1.0);
}
)GLSL";

// One triangle covering the target; no vertex buffer.
static const char* kFullVS = R"GLSL(
#version 450 core
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
)GLSL";

static const char* kCompFS = R"GLSL(
#version 450 core
uniform vec4 dst;       // output rect x, y, w, h (pixels, top-down)
uniform vec4 src;       // 2D-layer rect
uniform vec4 layer;     // 2D-layer size; z = output height, w = 1: flip (window)
uniform int  scene;
layout(binding = 0) uniform sampler2D sceneTex;
layout(binding = 1) uniform sampler2D lb;
layout(binding = 2) uniform sampler2D lg;
layout(location = 0) out vec4 oColor;
void main()
{
    vec2 p = gl_FragCoord.xy;
    if (layer.w != 0.0) p.y = layer.z - p.y;         // the window is bottom-up
    vec2 s = src.xy + (p - dst.xy) * (src.zw / dst.zw);
    vec3 a, g;
    bool inside = !(p.x < dst.x || p.y < dst.y || p.x >= dst.x + dst.z || p.y >= dst.y + dst.w);
    if (inside)
    {
        vec2 uv = s / layer.xy;
        a = texture(lb, uv).rgb;
        g = texture(lg, uv).rgb;
    }
    else
    {
        if (scene != 1) { oColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
        vec2 lo = src.xy + 0.5, hi = src.xy + src.zw - 0.5;
        vec2 e  = floor(clamp(s, lo, hi)) + 0.5;
        vec2 far2, mid2;
        if (s.x < lo.x || s.x > hi.x)
        {
            far2 = vec2(floor(s.x < lo.x ? hi.x : lo.x) + 0.5, e.y);
            mid2 = vec2(floor(src.x + src.z * 0.5) + 0.5, e.y);
        }
        else
        {
            far2 = vec2(e.x, floor(s.y < lo.y ? hi.y : lo.y) + 0.5);
            mid2 = vec2(e.x, floor(src.y + src.w * 0.5) + 0.5);
        }
        vec2 il = 1.0 / layer.xy;
        vec3 an = texture(lb, e * il).rgb,    gn = texture(lg, e * il).rgb;
        vec3 af = texture(lb, far2 * il).rgb, gf = texture(lg, far2 * il).rgb;
        vec3 am = texture(lb, mid2 * il).rgb, gm = texture(lg, mid2 * il).rgb;
        float k = 2.5 / 255.0;
        vec3 dnf = max(abs(an - af), abs(gn - gf));
        vec3 dfm = max(abs(af - am), abs(gf - gm));
        vec3 dnm = max(abs(an - am), abs(gn - gm));
        if      (max(dnf.r, max(dnf.g, dnf.b)) <= k) { a = an; g = gn; }
        else if (max(dfm.r, max(dfm.g, dfm.b)) <= k) { a = af; g = gf; }
        else if (max(dnm.r, max(dnm.g, dnm.b)) <= k) { a = an; g = gn; }
        else { a = vec3(0.0); g = vec3(128.0 / 255.0); }
    }
    vec3 m = max(g - a, 0.0) * (255.0 / 128.0);
    vec3 c = a;
    if (scene != 0)
    {
        ivec2 q = ivec2(p);
        ivec2 sz = textureSize(sceneTex, 0);
        if (q.x < sz.x && q.y < sz.y && q.x >= 0 && q.y >= 0) c += texelFetch(sceneTex, q, 0).rgb * m;
    }
    oColor = vec4(min(c, vec3(1.0)), 1.0);
}
)GLSL";

// The light scale: blending does dst * constant + this.
static const char* kScaleFS = R"GLSL(
#version 450 core
uniform vec4 addc;
layout(location = 0) out vec4 oColor;
void main() { oColor = vec4(addc.rgb, 1.0); }
)GLSL";

// The software path's present: a top-down RGBA frame stretched over the window.
static const char* kBlitFS = R"GLSL(
#version 450 core
uniform vec4 outSize;   // w, h
layout(binding = 0) uniform sampler2D img;
layout(location = 0) out vec4 oColor;
void main()
{
    vec2 uv = vec2(gl_FragCoord.x / outSize.x, 1.0 - gl_FragCoord.y / outSize.y);
    oColor = vec4(texture(img, uv).rgb, 1.0);
}
)GLSL";


// ----------------------------------------------------------------------- //

namespace
{
    // Mirrors of the GLSL blocks -- std140, byte for byte (ltmetal.mm's too).
    struct Uniforms { float mvp[16]; int alphaTest; float alphaRef; int blend;
                      float constAlpha; int affineColour; int a2c; float fogK[2];
                      float fogColor[4]; float fogNear; float fogFar; int fogOn; int screen; };
    static_assert(sizeof(Uniforms) == 128, "Uniforms must match the GLSL block");

    struct GLDynL { int n; float sat; float scale; int pad; float eye[4]; float pos[32][4]; float col[32][4]; };
    static_assert(sizeof(GLDynL) == 32 + 32 * 16 * 2, "GLDynL must match the GLSL block");

    struct SkinUC
    {
        float nrm[3][4];
        int   pose, mirror, mirrorU, lit, skinNormal, biasNorm, nLights, nNodes;
        float ambient[4];
        float dir[8][4];
        float color[8][4];
    };
    static_assert(sizeof(SkinUC) == 352, "SkinUC must match the GLSL block");

    bool  g_bInit = false, g_bFail = false, g_bPipe = false;
    char  g_Err[256] = { 0 };
    unsigned int g_nTris = 0;

    void Fail(const char* p) { snprintf(g_Err, sizeof(g_Err), "%s", p ? p : "?"); g_bFail = true; }

    GLuint g_ProgWorld = 0, g_ProgEnv = 0, g_ProgSkin = 0, g_ProgComp = 0, g_ProgScale = 0, g_ProgBlit = 0;
    GLint  g_locLMWorld = -1, g_locLMEnv = -1, g_locLMSkin = -1, g_locBaseAlpha = -1;
    GLint  g_locDst = -1, g_locSrc = -1, g_locLayer = -1, g_locScene = -1, g_locAdd = -1, g_locBlitSize = -1;
    GLuint g_VaoWorld = 0, g_VaoEnv = 0, g_VaoEmpty = 0;
    GLuint g_Sampler = 0, g_SamplerAT = 0, g_SceneSampler = 0, g_SceneSamplerAT = 0, g_LMSampler = 0, g_LayerSampler = 0;
    GLuint g_LMWhite = 0, g_Dummy = 0;
    GLint  g_nUBOAlign = 256, g_nSSBOAlign = 256;

    std::vector<GLuint>        g_Textures;    // handle -> GL name; 0 = none
    std::vector<unsigned char> g_TexScreen;   // 1 = a mirror's reflection
    std::vector<GLuint>        g_Buffers(1, 0);

    // ---- the per-frame stream: vertices, uniforms, palettes ------------- //
    GLuint g_Stream = 0;
    int    g_nDynLBoundStream = 0;     // 0 = the light block must be stored again
    size_t g_StreamCap = 0, g_StreamUsed = 0;

    size_t AlignUp(size_t n, size_t a) { return (n + a - 1) / a * a; }

    void StreamFrameStart()
    {
        if (!g_Stream) { glGenBuffers(1, &g_Stream); g_StreamCap = 0; }
        if (!g_StreamCap) g_StreamCap = (size_t)8 << 20;
        glBindBuffer(GL_ARRAY_BUFFER, g_Stream);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_StreamCap, 0, GL_STREAM_DRAW);   // orphan
        g_StreamUsed = 0;
        g_nDynLBoundStream = 0;     // the stored light block went with the old storage
    }

    // Appends, returns the offset. A full stream is replaced by a bigger one;
    // draws already issued keep the old storage (GL holds it until they ran).
    size_t StreamPut(const void* p, size_t n, size_t align)
    {
        if (!g_Stream) StreamFrameStart();
        size_t at = AlignUp(g_StreamUsed, align);
        if (at + n > g_StreamCap)
        {
            // ⚠️ GROWN WITH ITS CONTENTS: a palette pushed earlier this frame is
            // drawn from by offset after this, so the offsets must stay valid.
            size_t want = (at + n) * 2;
            if (want < g_StreamCap * 2) want = g_StreamCap * 2;
            GLuint nb = 0;
            glGenBuffers(1, &nb);
            glBindBuffer(GL_COPY_WRITE_BUFFER, nb);
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)want, 0, GL_STREAM_DRAW);
            if (g_StreamUsed)
            {
                glBindBuffer(GL_COPY_READ_BUFFER, g_Stream);
                glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (GLsizeiptr)g_StreamUsed);
            }
            glDeleteBuffers(1, &g_Stream);
            g_Stream = nb;
            g_StreamCap = want;
        }
        glBindBuffer(GL_ARRAY_BUFFER, g_Stream);
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)at, (GLsizeiptr)n, p);
        g_StreamUsed = at + n;
        return at;
    }

    // ---- targets -------------------------------------------------------- //
    struct Target
    {
        GLuint fbo = 0, colTex = 0, colRB = 0, depthRB = 0;
        int w = 0, h = 0, samples = 1;
    };

    void FreeTarget(Target& t)
    {
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
        if (t.colTex) glDeleteTextures(1, &t.colTex);
        if (t.colRB) glDeleteRenderbuffers(1, &t.colRB);
        if (t.depthRB) glDeleteRenderbuffers(1, &t.depthRB);
        t = Target();
    }

    // A colour texture (samples 1) or a multisampled colour renderbuffer, and
    // a Depth32F renderbuffer of the same sample count. bDepth false: colour only.
    bool MakeTarget(Target& t, int w, int h, int samples, bool bDepth)
    {
        if (t.fbo && t.w == w && t.h == h && t.samples == samples) return true;
        FreeTarget(t);
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        if (samples <= 1)
        {
            glGenTextures(1, &t.colTex);
            glBindTexture(GL_TEXTURE_2D, t.colTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.colTex, 0);
        }
        else
        {
            glGenRenderbuffers(1, &t.colRB);
            glBindRenderbuffer(GL_RENDERBUFFER, t.colRB);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.colRB);
        }
        if (bDepth)
        {
            glGenRenderbuffers(1, &t.depthRB);
            glBindRenderbuffer(GL_RENDERBUFFER, t.depthRB);
            if (samples <= 1) glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h);
            else glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT32F, w, h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depthRB);
        }
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE)
        {
            fprintf(stderr, "[GL] framebuffer %dx%d x%d incomplete (0x%x)\n", w, h, samples, st);
            FreeTarget(t);
            return false;
        }
        t.w = w; t.h = h; t.samples = samples;
        return true;
    }

    Target g_Off;                    // offscreen / walk-mode frame
    Target g_SceneMS, g_SceneRes;    // the compositor's 3D: MSAA + resolved (sampled)
    Target g_CompOff;                // the composite, for a readback
    enum { kReflSlots = 2 };
    Target g_Refl[kReflSlots];
    unsigned int g_ReflHandle[kReflSlots] = { 0, 0 };

    GLuint g_LayerB = 0, g_LayerG = 0, g_BlitTex = 0;
    int    g_LayerW = 0, g_LayerH = 0, g_BlitW = 0, g_BlitH = 0;

    // ---- pass state ------------------------------------------------------ //
    bool  g_bInPass = false;         // a Begin* is open: draws go somewhere
    bool  g_bOffscreen = false;      // BeginOffscreen (vs BeginFrame)
    bool  g_bWalkFrame = false;      // BeginFrame: present on EndFrame
    bool  g_bSceneVP = false;        // the compositor's scene pass
    int   g_VP[4] = { 0, 0, 0, 0 };  // its viewport (top-down)
    GLuint g_SceneFBO = 0;           // the FBO the scene pass draws into
    bool  g_bReflecting = false, g_bAfterMain = false, g_bSavedSceneVP = false;
    GLuint g_SavedFBO = 0;
    int   g_SavedW = 0, g_SavedH = 0;
    int   g_CurW = 0, g_CurH = 0;    // the bound target's size
    bool  g_bFrameOpen = false;      // the compositor's host frame started
    int   g_nSceneSamples = 0;

    // ---- cached GL state ------------------------------------------------ //
    int g_nWorldCull = 0;

    struct Fog { bool m_bOn; float m_fNear, m_fFar; float m_RGB[3]; float m_fKX, m_fKY; };
    Fog g_Fog = { false, 0.0f, 1.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f };

    GLDynL g_DynL = { 0, 1.0f, 2.0f, 0, { 0, 0, 0, 0 }, {}, {} };
    unsigned int g_nDynLSerial = 1, g_nDynLBound = 0;
    size_t g_nDynLOfs = 0;
    unsigned int g_nLightMap = 0;
    float g_fLMScale = 1.0f;

    bool SceneCutouts()
    {
        static const bool s_b = []() {
            const char* p = getenv("LT_CUTOUTS");
            const bool b = !(p && p[0] == '0');
            if (!b) { printf("[COMP] LT_CUTOUTS=0 -- cut-outs nearest + alpha test, no colour fill\n"); fflush(stdout); }
            return b; }();
        return s_b;
    }

    GLuint Compile(GLenum type, const char* a, const char* b, const char* c, const char* name)
    {
        const char* src[3] = { a, b ? b : "", c ? c : "" };
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 3, src, 0);
        glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[4096]; log[0] = 0;
            glGetShaderInfoLog(sh, sizeof(log), 0, log);
            fprintf(stderr, "[GL] shader %s failed:\n%s\n", name, log);
            snprintf(g_Err, sizeof(g_Err), "shader %s failed", name);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    }

    GLuint Link(GLuint vs, GLuint fs, const char* name)
    {
        if (!vs || !fs) return 0;
        GLuint p = glCreateProgram();
        glAttachShader(p, vs); glAttachShader(p, fs);
        glLinkProgram(p);
        GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[4096]; log[0] = 0;
            glGetProgramInfoLog(p, sizeof(log), 0, log);
            fprintf(stderr, "[GL] program %s failed:\n%s\n", name, log);
            snprintf(g_Err, sizeof(g_Err), "program %s failed", name);
            return 0;
        }
        return p;
    }

    GLuint MakeSampler(GLint minF, GLint magF, GLint wrap, float aniso)
    {
        GLuint s = 0;
        glGenSamplers(1, &s);
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minF);
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, magF);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap);
        if (aniso > 1.0f) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY, aniso);
        return s;
    }

    GLuint Tex1x1(unsigned char r, unsigned char g, unsigned char b)
    {
        GLuint t = 0;
        const unsigned char px[4] = { r, g, b, 255 };
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        return t;
    }

    bool BuildPipeline()
    {
        if (g_bPipe) return true;
        if (!LoadGL()) { Fail("OpenGL entry points missing"); return false; }

        const char* ver = (const char*)glGetString(GL_VERSION);
        const char* ren = (const char*)glGetString(GL_RENDERER);
        printf("[GL] %s -- %s\n", ren ? ren : "?", ver ? ver : "?");
        fflush(stdout);

        GLuint vsW = Compile(GL_VERTEX_SHADER, kCommon, kWorldVS, 0, "vs_world");
        GLuint vsS = Compile(GL_VERTEX_SHADER, kCommon, kSkinVS, 0, "vs_skin");
        GLuint vsE = Compile(GL_VERTEX_SHADER, kCommon, kEnvVS, 0, "vs_env");
        GLuint fsW = Compile(GL_FRAGMENT_SHADER, kCommon, kDynL, kWorldFS, "fs_world");
        GLuint fsE = Compile(GL_FRAGMENT_SHADER, kCommon, kDynL, kEnvFS, "fs_env");
        GLuint vsF = Compile(GL_VERTEX_SHADER, kFullVS, 0, 0, "vs_full");
        GLuint fsC = Compile(GL_FRAGMENT_SHADER, kCompFS, 0, 0, "fs_comp");
        GLuint fsK = Compile(GL_FRAGMENT_SHADER, kScaleFS, 0, 0, "fs_scale");
        GLuint fsB = Compile(GL_FRAGMENT_SHADER, kBlitFS, 0, 0, "fs_blit");
        g_ProgWorld = Link(vsW, fsW, "world");
        g_ProgSkin  = Link(vsS, fsW, "skin");
        g_ProgEnv   = Link(vsE, fsE, "env");
        g_ProgComp  = Link(vsF, fsC, "comp");
        g_ProgScale = Link(vsF, fsK, "scale");
        g_ProgBlit  = Link(vsF, fsB, "blit");
        if (!g_ProgWorld || !g_ProgComp || !g_ProgScale || !g_ProgBlit)
        { if (!g_Err[0]) snprintf(g_Err, sizeof(g_Err), "shader build failed"); g_bFail = true; return false; }
        if (!g_ProgSkin) printf("[GL] skinning program failed -- models skinned on the CPU\n");
        if (!g_ProgEnv)  printf("[GL] env-map program failed -- env surfaces drawn plain\n");

        g_locLMWorld = glGetUniformLocation(g_ProgWorld, "lmScale");
        g_locLMSkin  = g_ProgSkin ? glGetUniformLocation(g_ProgSkin, "lmScale") : -1;
        g_locLMEnv   = g_ProgEnv ? glGetUniformLocation(g_ProgEnv, "lmScale") : -1;
        g_locBaseAlpha = g_ProgEnv ? glGetUniformLocation(g_ProgEnv, "baseAlpha") : -1;
        g_locDst = glGetUniformLocation(g_ProgComp, "dst");
        g_locSrc = glGetUniformLocation(g_ProgComp, "src");
        g_locLayer = glGetUniformLocation(g_ProgComp, "layer");
        g_locScene = glGetUniformLocation(g_ProgComp, "scene");
        g_locAdd = glGetUniformLocation(g_ProgScale, "addc");
        g_locBlitSize = glGetUniformLocation(g_ProgBlit, "outSize");

        // Vertex layouts: LTMetalVert (40 bytes), LTMetalEnvVert (48).
        glGenVertexArrays(1, &g_VaoWorld);
        glBindVertexArray(g_VaoWorld);
        const GLuint offW[4] = { 0, 12, 20, 32 }; const GLint szW[4] = { 3, 2, 3, 2 };
        for (int i = 0; i < 4; ++i)
        { glEnableVertexAttribArray(i); glVertexAttribFormat(i, szW[i], GL_FLOAT, GL_FALSE, offW[i]); glVertexAttribBinding(i, 0); }
        glGenVertexArrays(1, &g_VaoEnv);
        glBindVertexArray(g_VaoEnv);
        const GLuint offE[5] = { 0, 12, 20, 32, 40 }; const GLint szE[5] = { 3, 2, 3, 2, 2 };
        for (int i = 0; i < 5; ++i)
        { glEnableVertexAttribArray(i); glVertexAttribFormat(i, szE[i], GL_FLOAT, GL_FALSE, offE[i]); glVertexAttribBinding(i, 0); }
        glGenVertexArrays(1, &g_VaoEmpty);
        glBindVertexArray(0);
        static_assert(sizeof(LTMetalVert) == 40 && sizeof(LTMetalEnvVert) == 48, "vertex layouts");

        g_Sampler   = MakeSampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT, 4.0f);
        g_SamplerAT = MakeSampler(GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST, GL_REPEAT, 4.0f);
        {
            int nAniso = 16;
            if (const char* pA = getenv("LT_ANISO")) nAniso = atoi(pA);
            if (nAniso < 1) nAniso = 1;
            if (nAniso > 16) nAniso = 16;
            g_SceneSampler   = MakeSampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT, (float)nAniso);
            g_SceneSamplerAT = MakeSampler(GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST, GL_REPEAT, (float)nAniso);
            printf("[COMP] texture filtering: trilinear, %dx anisotropic%s\n", nAniso, getenv("LT_ANISO") ? " (LT_ANISO)" : "");
        }
        g_LMSampler    = MakeSampler(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, 1.0f);
        g_LayerSampler = MakeSampler(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, 1.0f);
        g_LMWhite = Tex1x1(255, 255, 255);
        g_Dummy   = Tex1x1(0, 0, 0);

        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g_nUBOAlign);
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &g_nSSBOAlign);
        if (g_nUBOAlign < 16) g_nUBOAlign = 16;
        if (g_nSSBOAlign < 16) g_nSSBOAlign = 16;

        // ⚠️ METAL'S CONVENTIONS (see the top of the file), for every pass.
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);     // [[flat]] takes the first vertex in MSL
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        g_Textures.assign(1, 0); g_TexScreen.assign(1, 0);
        g_ReflHandle[0] = g_ReflHandle[1] = 0;
        g_bPipe = true;
        return true;
    }

    bool EnsureDevice()
    {
        if (g_bInit) return !g_bFail && LTGLWin_EnsureContext(true);
        g_bInit = true;
        if (!LTGLWin_EnsureContext(true)) { Fail("no OpenGL 4.6 context"); return false; }
        return BuildPipeline();
    }

    int SceneSamples()
    {
        if (g_nSceneSamples) return g_nSceneSamples;
        int want = 4;
        if (const char* p = getenv("LT_MSAA")) want = atoi(p);
        if (want != 1 && want != 2 && want != 4 && want != 8) want = 4;
        GLint mx = 1; glGetIntegerv(GL_MAX_SAMPLES, &mx);
        int n = want;
        while (n > 1 && n > mx) n /= 2;
        g_nSceneSamples = n;
        printf("[COMP] 3D anti-aliasing: %dx MSAA%s\n", n, getenv("LT_MSAA") ? " (LT_MSAA)" : "");
        fflush(stdout);
        return n;
    }

    // Binds a target for drawing, with the whole of it as viewport, no scissor.
    void BindTarget(GLuint fbo, int w, int h)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        g_CurW = w; g_CurH = h;
    }

    void ApplySceneVP()
    {
        glViewport(g_VP[0], g_VP[1], g_VP[2], g_VP[3]);
        glScissor(g_VP[0], g_VP[1], g_VP[2], g_VP[3]);
        glEnable(GL_SCISSOR_TEST);
    }

    void ClearColourDepth(float r, float g, float b, bool bColour)
    {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(r, g, b, 1.0f);
        glClearDepthf(1.0f);
        glClear((bColour ? GL_COLOR_BUFFER_BIT : 0) | GL_DEPTH_BUFFER_BIT);
    }

    // ---- the draw ---------------------------------------------------------- //
    void ApplyCull()
    {
        // 1 = cull back, front clockwise as Metal states it. Measured on
        // T01S01 (walk mode, both ways): with the upper-left origin that is
        // GL_CW here -- the other way culls the ground and every wall facing
        // the eye. LT_GLCULLFLIP=1 swaps, for checking.
        static const bool bFlip = getenv("LT_GLCULLFLIP") != 0;
        if (!g_nWorldCull) { glDisable(GL_CULL_FACE); return; }
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        const bool bCW = (g_nWorldCull == 1) != bFlip;
        glFrontFace(bCW ? GL_CW : GL_CCW);
    }

    // 0 opaque, 1 source-alpha over, 2 additive, 3 multiply.
    void ApplyBlend(int nMode)
    {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (nMode == 0) { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
        else
        {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            if (nMode == 1)      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            else if (nMode == 2) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            else                 glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
            glDepthMask(GL_FALSE);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
    }

    void BindDynAndLM(GLint locLM)
    {
        if (g_nDynLBound != g_nDynLSerial || !g_nDynLBoundStream)
        {
            // Only the header and the lights in use matter, but the block is
            // bound whole: a short range is undefined behaviour in GL too.
            g_nDynLOfs = StreamPut(&g_DynL, sizeof(g_DynL), (size_t)g_nUBOAlign);
            g_nDynLBound = g_nDynLSerial;
            g_nDynLBoundStream = 1;
        }
        glBindBufferRange(GL_UNIFORM_BUFFER, 3, g_Stream, (GLintptr)g_nDynLOfs, sizeof(g_DynL));
        GLuint lm = (g_nLightMap && g_nLightMap < g_Textures.size()) ? g_Textures[g_nLightMap] : 0;
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, lm ? lm : g_LMWhite);
        glBindSampler(2, g_LMSampler);
        if (locLM >= 0) glUniform1f(locLM, g_fLMScale);
    }

    void FillUniforms(Uniforms& u, const float* pMVP, unsigned int nTexture, bool bAlphaTest, float fAlphaRef,
                      bool bBlend, float fConstAlpha, bool bTexAlpha, int nBlendOp, bool bA2C)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) u.mvp[c*4 + r] = pMVP[r*4 + c];    // row-major -> GLSL column-major
        u.alphaTest = bAlphaTest ? 1 : 0;
        u.alphaRef = (fAlphaRef > 0.0f && fAlphaRef <= 1.0f) ? fAlphaRef : 0.5f;
        u.blend      = bBlend ? (bTexAlpha ? 1 : 2) : 0;
        u.constAlpha = (fConstAlpha >= 0.0f && fConstAlpha <= 1.0f) ? fConstAlpha : 1.0f;
        static const int nAffine = (getenv("LT_AFFINECOLOUR") != 0) ? 1 : 0;
        u.affineColour = nAffine;
        u.a2c = bA2C ? 1 : 0;
        u.fogK[0] = g_Fog.m_fKX; u.fogK[1] = g_Fog.m_fKY;
        u.fogOn = g_Fog.m_bOn ? 1 : 0;
        u.fogNear = g_Fog.m_fNear; u.fogFar = g_Fog.m_fFar;
        u.screen = (nTexture < g_TexScreen.size() && g_TexScreen[nTexture]) ? 1 : 0;
        const bool bAdd = bBlend && nBlendOp == 1, bMul = bBlend && nBlendOp == 2;
        for (int k = 0; k < 3; ++k)
            u.fogColor[k] = bAdd ? 0.0f : (bMul ? 1.0f : g_Fog.m_RGB[k] / 255.0f);
        u.fogColor[3] = 1.0f;
    }

    void BindSampler0(bool bAlphaTest, bool bEnvToo)
    {
        static const int nATS = getenv("LT_ATSAMPLER") ? atoi(getenv("LT_ATSAMPLER")) : 1;
        GLuint s;
        if (g_bSceneVP) s = (bAlphaTest && nATS && !SceneCutouts()) ? g_SceneSamplerAT : g_SceneSampler;
        else            s = (bAlphaTest && nATS) ? g_SamplerAT : g_Sampler;
        glBindSampler(0, s);
        if (bEnvToo) glBindSampler(1, s);
    }

    struct SkinEncode { GLuint m_Mesh; size_t m_nVertOfs, m_nWeightOfs, m_nCornerBytes, m_nVertBytes, m_nWeightBytes;
                        size_t m_nPalOfs, m_nPalBytes; const SkinUC* m_pU; int m_nFirst; };

    void EncodeTris(const float* pMVP, unsigned int nTexture,
                    GLuint vb, size_t nOffset, int nVerts,
                    bool bAlphaTest, float fAlphaRef, bool bBlend,
                    float fConstAlpha, bool bTexAlpha, int nBlendOp,
                    const SkinEncode* pSkin = 0, GLuint ib = 0, size_t nIdxOfs = 0)
    {
        if (!g_bInPass) return;
        const bool bA2C = g_bSceneVP && SceneCutouts() && bAlphaTest && !bBlend && g_nSceneSamples > 1;
        Uniforms u;
        FillUniforms(u, pMVP, nTexture, bAlphaTest, fAlphaRef, bBlend, fConstAlpha, bTexAlpha, nBlendOp, bA2C);
        const int nMode = bBlend ? (1 + ((nBlendOp == 1 || nBlendOp == 2) ? nBlendOp : 0)) : 0;

        const GLuint prog = pSkin ? g_ProgSkin : g_ProgWorld;
        if (!prog) return;
        glUseProgram(prog);
        ApplyBlend(nMode);
        if (bA2C) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE); else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        ApplyCull();
        BindSampler0(bAlphaTest, false);

        const size_t uo = StreamPut(&u, sizeof(u), (size_t)g_nUBOAlign);
        glBindBufferRange(GL_UNIFORM_BUFFER, 1, g_Stream, (GLintptr)uo, sizeof(u));
        BindDynAndLM(pSkin ? g_locLMSkin : g_locLMWorld);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_Textures[nTexture]);

        if (pSkin)
        {
            const size_t so = StreamPut(pSkin->m_pU, sizeof(SkinUC), (size_t)g_nUBOAlign);
            glBindBufferRange(GL_UNIFORM_BUFFER, 6, g_Stream, (GLintptr)so, sizeof(SkinUC));
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, pSkin->m_Mesh, 0, (GLsizeiptr)pSkin->m_nCornerBytes);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, pSkin->m_Mesh, (GLintptr)pSkin->m_nVertOfs, (GLsizeiptr)pSkin->m_nVertBytes);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 4, pSkin->m_Mesh, (GLintptr)pSkin->m_nWeightOfs, (GLsizeiptr)pSkin->m_nWeightBytes);
            if (pSkin->m_nPalBytes)
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 5, g_Stream, (GLintptr)pSkin->m_nPalOfs, (GLsizeiptr)pSkin->m_nPalBytes);
            else
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 5, pSkin->m_Mesh, 0, 16);   // unread: pose 0
            glBindVertexArray(g_VaoEmpty);
            glDrawArrays(GL_TRIANGLES, pSkin->m_nFirst, nVerts);
        }
        else
        {
            glBindVertexArray(g_VaoWorld);
            glBindVertexBuffer(0, vb, (GLintptr)nOffset, sizeof(LTMetalVert));
            if (ib)
            {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
                glDrawElements(GL_TRIANGLES, nVerts, GL_UNSIGNED_INT, (const void*)nIdxOfs);
            }
            else glDrawArrays(GL_TRIANGLES, 0, nVerts);
        }
        g_nTris += (unsigned int)(nVerts / 3);
    }

    // Fills the colour of every texel below the reference from its known
    // neighbours (ltmetal.mm's BleedCutout, unchanged).
    void BleedCutout(unsigned char* p, int w, int h, unsigned char ref)
    {
        const int n = w * h;
        std::vector<unsigned char> known((size_t)n, 0);
        long long sr = 0, sg = 0, sb = 0; int nKnown = 0;
        for (int i = 0; i < n; ++i)
            if (p[i*4+3] >= ref) { known[i] = 1; ++nKnown; sr += p[i*4]; sg += p[i*4+1]; sb += p[i*4+2]; }
        if (nKnown == 0 || nKnown == n) return;
        std::vector<int> fresh;
        for (int ring = 0; ring < 16; ++ring)
        {
            fresh.clear();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const int i = y * w + x;
                    if (known[i]) continue;
                    int r = 0, g = 0, b = 0, c = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            if (!dx && !dy) continue;
                            const int j = ((y + dy + h) % h) * w + ((x + dx + w) % w);
                            if (known[j] != 1) continue;
                            r += p[j*4]; g += p[j*4+1]; b += p[j*4+2]; ++c;
                        }
                    if (!c) continue;
                    p[i*4] = (unsigned char)(r / c); p[i*4+1] = (unsigned char)(g / c); p[i*4+2] = (unsigned char)(b / c);
                    known[i] = 2;
                    fresh.push_back(i);
                }
            if (fresh.empty()) break;
            for (size_t k = 0; k < fresh.size(); ++k) known[fresh[k]] = 1;
        }
        const unsigned char mr = (unsigned char)(sr / nKnown), mg = (unsigned char)(sg / nKnown), mb = (unsigned char)(sb / nKnown);
        for (int i = 0; i < n; ++i)
            if (!known[i]) { p[i*4] = mr; p[i*4+1] = mg; p[i*4+2] = mb; }
    }

    struct SkinMesh { GLuint m_Buf = 0; size_t m_nVertOfs = 0, m_nWeightOfs = 0, m_nC = 0, m_nV = 0, m_nW = 0; };
    std::vector<SkinMesh> g_SkinMeshes(1);

    void UploadLayer(GLuint& t, int& curW, int& curH, int w, int h, const unsigned char* p, bool bRealloc)
    {
        if (!t) glGenTextures(1, &t);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (bRealloc)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            curW = w; curH = h;
        }
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p);
    }

    // A full-target triangle with everything 3D switched off.
    void FullscreenState()
    {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindVertexArray(g_VaoEmpty);
    }
}


// ======================================================================= //
// ltmetal.h
// ======================================================================= //

bool LTMetal_Available() { return g_bInit && !g_bFail; }
const char* LTMetal_LastError() { return g_Err[0] ? g_Err : 0; }
unsigned int LTMetal_FrameTriangles() { return g_nTris; }

bool LTMetal_SelfTest(char* pErr, int nErrBytes)
{
    if (pErr && nErrBytes > 0) pErr[0] = 0;
    if (!LTGLWin_EnsureContext(true)) { if (pErr) snprintf(pErr, nErrBytes, "no OpenGL 4.6 context"); return false; }
    if (!BuildPipeline())
    {
        if (pErr) snprintf(pErr, nErrBytes, "%s", g_Err[0] ? g_Err : "pipeline failed");
        g_bFail = false; g_Err[0] = 0;
        return false;
    }
    return true;
}

bool LTMetal_Attach(int nWidth, int nHeight)
{
    (void)nWidth; (void)nHeight;
    if (!LTGLWin_HasWindow()) { g_bInit = true; Fail("no window to attach to"); return false; }
    return EnsureDevice();
}

void LTMetal_Shutdown()
{
    g_bInPass = false;
    g_bInit = false; g_bFail = false;
}

unsigned int LTMetal_CreateTexture(const unsigned char* const* ppLevels,
                                   const int* pW, const int* pH, int nLevels,
                                   bool bCutout, float fAlphaRef)
{
    if (!LTMetal_Available() || !ppLevels || nLevels <= 0) return 0;
    if (pW[0] <= 0 || pH[0] <= 0 || !ppLevels[0]) return 0;

    std::vector<std::vector<unsigned char> > copies;
    std::vector<const unsigned char*> levels(ppLevels, ppLevels + nLevels);
    if (bCutout && SceneCutouts())
    {
        const float ref01 = (fAlphaRef > 0.0f && fAlphaRef <= 1.0f) ? fAlphaRef : 0.5f;
        const unsigned char ref = (unsigned char)(ref01 * 255.0f + 0.5f);
        copies.resize((size_t)nLevels);
        for (int i = 0; i < nLevels; ++i)
        {
            if (!ppLevels[i] || pW[i] <= 0 || pH[i] <= 0) continue;
            copies[i].assign(ppLevels[i], ppLevels[i] + (size_t)pW[i] * pH[i] * 4);
            BleedCutout(&copies[i][0], pW[i], pH[i], ref);
            levels[i] = &copies[i][0];
        }
    }

    GLuint t = 0;
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    int nMax = 0;
    for (int i = 0; i < nLevels; ++i)
    {
        if (!levels[i] || pW[i] <= 0 || pH[i] <= 0) break;      // a gap ends the chain (completeness)
        glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, pW[i], pH[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, levels[i]);
        nMax = i;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, nMax);
    g_Textures.push_back(t);
    g_TexScreen.resize(g_Textures.size(), 0);
    return (unsigned int)(g_Textures.size() - 1);
}

void LTMetal_UpdateTextureRegion(unsigned int nTexture, int x, int y, int w, int h,
                                 const unsigned char* pRGBA, int nStride)
{
    // GL orders this against every earlier draw reading the texture by itself.
    if (!g_bInit || !pRGBA || w <= 0 || h <= 0 || nTexture == 0 || nTexture >= g_Textures.size()) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_Textures[nTexture]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, nStride / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pRGBA);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

bool LTMetal_BeginOffscreen(int nWidth, int nHeight, float fR, float fG, float fB)
{
    if (!EnsureDevice()) return false;
    if (!MakeTarget(g_Off, nWidth, nHeight, 1, true)) { Fail("offscreen target failed"); return false; }
    g_nTris = 0;
    StreamFrameStart();
    g_bOffscreen = true; g_bWalkFrame = false; g_bSceneVP = false;
    BindTarget(g_Off.fbo, nWidth, nHeight);
    ClearColourDepth(fR, fG, fB, true);
    g_bInPass = true;
    return true;
}

bool LTMetal_ReadPixels(unsigned char* pRGBA, int nWidth, int nHeight)
{
    if (!pRGBA || !g_Off.fbo || g_Off.w != nWidth || g_Off.h != nHeight) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_Off.fbo);
    glReadPixels(0, 0, nWidth, nHeight, GL_RGBA, GL_UNSIGNED_BYTE, pRGBA);   // row 0 = top (upper-left origin)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    const size_t n = (size_t)nWidth * nHeight * 4;
    for (size_t i = 3; i < n; i += 4) pRGBA[i] = 255;
    return true;
}

bool LTMetal_BeginFrame(float fR, float fG, float fB)
{
    // Walk mode: drawn into a target of the window's size, presented on EndFrame.
    int w = 0, h = 0;
    if (!LTMetal_Available() || !LTGLWin_DrawableSize(&w, &h)) return false;
    if (!LTMetal_BeginOffscreen(w, h, fR, fG, fB)) return false;
    g_bOffscreen = false; g_bWalkFrame = true;
    return true;
}

void LTMetal_ClearDepth()
{
    if (!g_bInPass) return;
    // In the scene pass the scissor holds the camera's viewport, so the clear
    // is confined to it exactly as ltmetal.mm's far-plane quad is.
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
}

bool LTMetal_BeginReflection(int nSlot, int nWidth, int nHeight)
{
    if (!g_bInPass || nSlot < 0 || nSlot >= kReflSlots || nWidth <= 0 || nHeight <= 0) return false;
    Target& t = g_Refl[nSlot];
    const GLuint old = t.colTex;
    if (!MakeTarget(t, nWidth, nHeight, 1, true)) return false;
    if (g_ReflHandle[nSlot] && g_ReflHandle[nSlot] < g_Textures.size() && g_Textures[g_ReflHandle[nSlot]] == old)
        g_Textures[g_ReflHandle[nSlot]] = t.colTex;
    if (!g_ReflHandle[nSlot] || g_ReflHandle[nSlot] >= g_Textures.size() || g_Textures[g_ReflHandle[nSlot]] != t.colTex)
    {
        g_Textures.push_back(t.colTex);
        g_ReflHandle[nSlot] = (unsigned int)(g_Textures.size() - 1);
    }
    if (g_TexScreen.size() < g_Textures.size()) g_TexScreen.resize(g_Textures.size(), 0);
    g_TexScreen[g_ReflHandle[nSlot]] = 1;

    if (!g_bAfterMain)
    {
        g_bSavedSceneVP = g_bSceneVP;
        GLint fbo = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
        g_SavedFBO = (GLuint)fbo; g_SavedW = g_CurW; g_SavedH = g_CurH;
        g_bAfterMain = true;
    }
    BindTarget(t.fbo, nWidth, nHeight);
    ClearColourDepth(0.055f, 0.067f, 0.086f, true);
    g_bSceneVP = false;
    g_bReflecting = true;
    return true;
}

void LTMetal_EndReflection() { g_bReflecting = false; }

void LTMetal_ResumeAfterReflections()
{
    if (!g_bAfterMain) return;
    g_bAfterMain = false;
    g_bSceneVP = g_bSavedSceneVP;
    BindTarget(g_SavedFBO, g_SavedW, g_SavedH);
    ClearColourDepth(0, 0, 0, false);       // colour kept, depth cleared (whole target)
    if (g_bSceneVP) ApplySceneVP();
}

unsigned int LTMetal_ReflectionTexture(int nSlot)
{
    return (nSlot >= 0 && nSlot < kReflSlots) ? g_ReflHandle[nSlot] : 0;
}

void LTMetal_SetFog(bool bOn, float fNear, float fFar, const float* pRGB255)
{
    g_Fog.m_bOn = bOn; g_Fog.m_fNear = fNear; g_Fog.m_fFar = fFar;
    for (int k = 0; k < 3; ++k) g_Fog.m_RGB[k] = pRGB255 ? pRGB255[k] : 0.0f;
}

void LTMetal_SetFogRange(float fKX, float fKY) { g_Fog.m_fKX = fKX; g_Fog.m_fKY = fKY; }

void LTMetal_SetWorldCull(int nMode) { g_nWorldCull = (nMode == 1 || nMode == 2) ? nMode : 0; }

void LTMetal_SetLightMap(unsigned int nTexture, float fScale) { g_nLightMap = nTexture; g_fLMScale = fScale; }

void LTMetal_SetDynLights(const LT3DDynLights* p)
{
    if (!p || p->m_n <= 0)
    {
        if (g_DynL.n != 0) { g_DynL.n = 0; ++g_nDynLSerial; }
        return;
    }
    ++g_nDynLSerial;
    const int n = p->m_n < 32 ? p->m_n : 32;
    g_DynL.n = n; g_DynL.sat = p->m_fSat; g_DynL.scale = p->m_fScale; g_DynL.pad = 0;
    for (int k = 0; k < 3; ++k) g_DynL.eye[k] = p->m_Eye[k];
    g_DynL.eye[3] = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < 3; ++k) { g_DynL.pos[i][k] = p->m_Pos[i][k]; g_DynL.col[i][k] = p->m_Col[i][k]; }
        g_DynL.pos[i][3] = p->m_InvR2[i];
        g_DynL.col[i][3] = p->m_bBackCull[i] ? 1.0f : 0.0f;
    }
}

void LTMetal_DrawTris(const float* pMVP, unsigned int nTexture,
                      const LTMetalVert* pVerts, int nVerts,
                      bool bAlphaTest, float fAlphaRef, bool bBlend,
                      float fConstAlpha, bool bTexAlpha, int nBlendOp)
{
    if (!g_bInPass || !pVerts || nVerts < 3 || !pMVP) return;
    if (nTexture == 0 || nTexture >= g_Textures.size()) return;
    const size_t at = StreamPut(pVerts, sizeof(LTMetalVert) * (size_t)nVerts, 16);
    EncodeTris(pMVP, nTexture, g_Stream, at, nVerts, bAlphaTest, fAlphaRef, bBlend, fConstAlpha, bTexAlpha, nBlendOp);
}

void LTMetal_DrawEnvTris(const float* pMVP, unsigned int nTexture, unsigned int nEnvTexture,
                         const LTMetalEnvVert* pVerts, int nVerts,
                         bool bAlphaTest, float fAlphaRef, bool bBaseAlpha, bool bChrome)
{
    if (!g_bInPass || !pVerts || nVerts < 3 || !pMVP) return;
    if (nTexture == 0 || nTexture >= g_Textures.size()) return;
    if (!g_ProgEnv || nEnvTexture == 0 || nEnvTexture >= g_Textures.size())
    {
        std::vector<LTMetalVert> plain((size_t)nVerts);
        for (int i = 0; i < nVerts; ++i)
        {
            memcpy(plain[(size_t)i].m_Pos, pVerts[i].m_Pos, sizeof(float) * 3);
            memcpy(plain[(size_t)i].m_UV,  pVerts[i].m_UV,  sizeof(float) * 2);
            memcpy(plain[(size_t)i].m_RGB, pVerts[i].m_RGB, sizeof(float) * 3);
            memcpy(plain[(size_t)i].m_LM,  pVerts[i].m_LM,  sizeof(float) * 2);
        }
        LTMetal_DrawTris(pMVP, nTexture, &plain[0], nVerts, bAlphaTest, fAlphaRef);
        return;
    }
    const size_t at = StreamPut(pVerts, sizeof(LTMetalEnvVert) * (size_t)nVerts, 16);
    Uniforms u;
    FillUniforms(u, pMVP, nTexture, bAlphaTest, fAlphaRef, false, 1.0f, true, 0, false);
    u.screen = 0;

    glUseProgram(g_ProgEnv);
    ApplyBlend(0);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    ApplyCull();
    BindSampler0(bAlphaTest, true);
    const size_t uo = StreamPut(&u, sizeof(u), (size_t)g_nUBOAlign);
    glBindBufferRange(GL_UNIFORM_BUFFER, 1, g_Stream, (GLintptr)uo, sizeof(u));
    BindDynAndLM(g_locLMEnv);
    glUniform1i(g_locBaseAlpha, (bBaseAlpha ? 1 : 0) | (bChrome ? 2 : 0));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_Textures[nEnvTexture]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_Textures[nTexture]);
    glBindVertexArray(g_VaoEnv);
    glBindVertexBuffer(0, g_Stream, (GLintptr)at, sizeof(LTMetalEnvVert));
    glDrawArrays(GL_TRIANGLES, 0, nVerts);
    g_nTris += (unsigned int)(nVerts / 3);
}

static unsigned int PutBuffer(GLuint b)
{
    for (size_t i = 1; i < g_Buffers.size(); ++i)
        if (!g_Buffers[i]) { g_Buffers[i] = b; return (unsigned int)i; }
    g_Buffers.push_back(b);
    return (unsigned int)(g_Buffers.size() - 1);
}

unsigned int LTMetal_CreateBuffer(const LTMetalVert* pVerts, int nVerts)
{
    if (!g_bInit || g_bFail || !pVerts || nVerts <= 0) return 0;
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(LTMetalVert) * (size_t)nVerts), pVerts, GL_STATIC_DRAW);
    return PutBuffer(b);
}

// ⚠️ NO GL CALLS ONCE THE CONTEXT IS GONE. The world scene is a static whose
// destructor runs from exit(), after LTWindow_Close destroyed the context --
// glDeleteBuffers then jumped into an unloaded driver (SIGSEGV on quit with a
// level loaded). The driver frees everything with the context anyway.
static bool GLAlive() { return SDL_GL_GetCurrentContext() != 0; }

void LTMetal_FreeBuffer(unsigned int nBuffer)
{
    if (!GLAlive()) return;
    if (nBuffer > 0 && nBuffer < g_Buffers.size() && g_Buffers[nBuffer])
    { glDeleteBuffers(1, &g_Buffers[nBuffer]); g_Buffers[nBuffer] = 0; }
}

void LTMetal_DrawTrisFrom(const float* pMVP, unsigned int nTexture,
                          unsigned int nBuffer, int nFirst, int nVerts,
                          bool bAlphaTest, float fAlphaRef, bool bBlend,
                          float fConstAlpha, bool bTexAlpha, int nBlendOp)
{
    if (!g_bInPass || nVerts < 3 || !pMVP || nFirst < 0) return;
    if (nTexture == 0 || nTexture >= g_Textures.size()) return;
    if (nBuffer == 0 || nBuffer >= g_Buffers.size() || !g_Buffers[nBuffer]) return;
    EncodeTris(pMVP, nTexture, g_Buffers[nBuffer], sizeof(LTMetalVert) * (size_t)nFirst,
               nVerts, bAlphaTest, fAlphaRef, bBlend, fConstAlpha, bTexAlpha, nBlendOp);
}

unsigned int LTMetal_CreateIndexBuffer(const unsigned int* pIdx, int nIdx)
{
    if (!g_bInit || g_bFail || !pIdx || nIdx <= 0) return 0;
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, b);       // any target to create it; drawn as ELEMENT_ARRAY
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(unsigned int) * (size_t)nIdx), pIdx, GL_STATIC_DRAW);
    return PutBuffer(b);
}

void LTMetal_DrawTrisFromIndexed(const float* pMVP, unsigned int nTexture,
                                 unsigned int nBuffer, unsigned int nIndexBuf,
                                 int nFirst, int nCount,
                                 bool bAlphaTest, float fAlphaRef, bool bBlend,
                                 float fConstAlpha, bool bTexAlpha, int nBlendOp)
{
    if (nIndexBuf == 0)
    {
        LTMetal_DrawTrisFrom(pMVP, nTexture, nBuffer, nFirst, nCount, bAlphaTest, fAlphaRef,
                             bBlend, fConstAlpha, bTexAlpha, nBlendOp);
        return;
    }
    if (!g_bInPass || nCount < 3 || !pMVP || nFirst < 0) return;
    if (nTexture == 0 || nTexture >= g_Textures.size()) return;
    if (nBuffer == 0 || nBuffer >= g_Buffers.size() || !g_Buffers[nBuffer]) return;
    if (nIndexBuf >= g_Buffers.size() || !g_Buffers[nIndexBuf]) return;
    EncodeTris(pMVP, nTexture, g_Buffers[nBuffer], 0, nCount, bAlphaTest, fAlphaRef,
               bBlend, fConstAlpha, bTexAlpha, nBlendOp, 0,
               g_Buffers[nIndexBuf], sizeof(unsigned int) * (size_t)nFirst);
}

unsigned int LTMetal_CreateSkinMesh(const LTMetalSkinVert* pVerts, int nVerts,
                                    const LTMetalSkinWeight* pWeights, int nWeights,
                                    const LTMetalSkinCorner* pCorners, int nCorners)
{
    static_assert(sizeof(LTMetalSkinVert) == 32 && sizeof(LTMetalSkinWeight) == 32
                  && sizeof(LTMetalSkinCorner) == 16, "skin structs must match the GLSL ones");
    if (!g_bInit || g_bFail || !g_ProgSkin || !pVerts || nVerts <= 0 || !pCorners || nCorners <= 0) return 0;
    const size_t al = (size_t)(g_nSSBOAlign > 256 ? g_nSSBOAlign : 256);
    const size_t nC = sizeof(LTMetalSkinCorner) * (size_t)nCorners;
    const size_t nV = sizeof(LTMetalSkinVert) * (size_t)nVerts;
    const size_t nW = sizeof(LTMetalSkinWeight) * (size_t)(nWeights > 0 ? nWeights : 1);
    const size_t oV = AlignUp(nC, al), oW = oV + AlignUp(nV, al);
    std::vector<unsigned char> buf(oW + nW, 0);
    memcpy(&buf[0], pCorners, nC);
    memcpy(&buf[oV], pVerts, nV);
    if (nWeights > 0 && pWeights) memcpy(&buf[oW], pWeights, sizeof(LTMetalSkinWeight) * (size_t)nWeights);
    SkinMesh m;
    glGenBuffers(1, &m.m_Buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m.m_Buf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)buf.size(), &buf[0], GL_STATIC_DRAW);
    m.m_nVertOfs = oV; m.m_nWeightOfs = oW; m.m_nC = nC; m.m_nV = nV; m.m_nW = nW;
    for (size_t i = 1; i < g_SkinMeshes.size(); ++i)
        if (!g_SkinMeshes[i].m_Buf) { g_SkinMeshes[i] = m; return (unsigned int)i; }
    g_SkinMeshes.push_back(m);
    return (unsigned int)(g_SkinMeshes.size() - 1);
}

void LTMetal_FreeSkinMesh(unsigned int nMesh)
{
    if (!GLAlive()) return;
    if (nMesh > 0 && nMesh < g_SkinMeshes.size() && g_SkinMeshes[nMesh].m_Buf)
    { glDeleteBuffers(1, &g_SkinMeshes[nMesh].m_Buf); g_SkinMeshes[nMesh] = SkinMesh(); }
}

// The palette's offset in this frame's stream; its size travels with it.
namespace { size_t g_nLastPalOfs = 0, g_nLastPalBytes = 0; }

int LTMetal_PushPalette(const float* pPalette, int nNodes)
{
    if (!g_bInPass || !pPalette || nNodes <= 0) return -1;
    const size_t nBytes = sizeof(float) * 24 * (size_t)nNodes;
    const size_t al = (size_t)(g_nSSBOAlign > 256 ? g_nSSBOAlign : 256);
    const size_t at = StreamPut(pPalette, nBytes, al);
    g_nLastPalOfs = at; g_nLastPalBytes = nBytes;
    return (int)at;
}

void LTMetal_DrawSkinned(const float* pMVP, unsigned int nTexture, unsigned int nMesh,
                         int nFirstCorner, int nCorners, const LTMetalSkinDraw& sd,
                         bool bAlphaTest, float fAlphaRef, bool bBlend,
                         float fConstAlpha, bool bTexAlpha, int nBlendOp)
{
    if (!g_bInPass || !pMVP || nCorners < 3 || nFirstCorner < 0) return;
    if (nTexture == 0 || nTexture >= g_Textures.size()) return;
    if (nMesh == 0 || nMesh >= g_SkinMeshes.size() || !g_SkinMeshes[nMesh].m_Buf) return;
    if (!g_ProgSkin) return;

    SkinUC u;
    memset(&u, 0, sizeof(u));
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) u.nrm[r][c] = sd.m_Normal[r*3 + c];
    u.pose = (sd.m_nPaletteOfs >= 0 && sd.m_nNodes > 0) ? 1 : 0;
    u.mirror = sd.m_bMirror ? 1 : 0;   u.mirrorU = sd.m_bMirrorU ? 1 : 0;
    u.lit = sd.m_bLit ? 1 : 0;         u.skinNormal = sd.m_bSkinNormal ? 1 : 0;
    u.biasNorm = sd.m_bBiasNorm ? 1 : 0;
    u.nLights = sd.m_nLights < 0 ? 0 : (sd.m_nLights > 8 ? 8 : sd.m_nLights);
    u.nNodes = sd.m_nNodes;
    for (int k = 0; k < 3; ++k) u.ambient[k] = sd.m_Ambient[k];
    for (int l = 0; l < u.nLights; ++l)
        for (int k = 0; k < 3; ++k) { u.dir[l][k] = sd.m_Dir[l][k]; u.color[l][k] = sd.m_Color[l][k]; }

    const SkinMesh& m = g_SkinMeshes[nMesh];
    SkinEncode se;
    se.m_Mesh = m.m_Buf;
    se.m_nVertOfs = m.m_nVertOfs; se.m_nWeightOfs = m.m_nWeightOfs;
    se.m_nCornerBytes = m.m_nC; se.m_nVertBytes = m.m_nV; se.m_nWeightBytes = m.m_nW;
    se.m_nPalOfs = u.pose ? (size_t)sd.m_nPaletteOfs : 0;
    // The palette's size: the one pushed at this offset (nodes x 24 floats).
    se.m_nPalBytes = u.pose ? (((size_t)sd.m_nPaletteOfs == g_nLastPalOfs) ? g_nLastPalBytes
                                                                            : sizeof(float) * 24 * (size_t)sd.m_nNodes) : 0;
    se.m_pU = &u;
    se.m_nFirst = nFirstCorner;
    EncodeTris(pMVP, nTexture, 0, 0, nCorners, bAlphaTest, fAlphaRef, bBlend,
               fConstAlpha, bTexAlpha, nBlendOp, &se);
}

bool LTMetal_SkinningAvailable() { return g_bPipe && g_ProgSkin != 0; }


// LT_GLSHOT=<dir>: every LT_GLSHOT_EVERY-th presented frame (default 300)
// written as <dir>/glshot_<n>.ppm -- what the window really shows.
static void GLShot(int nOutW, int nOutH)
{
    static const char* s_pShot = getenv("LT_GLSHOT");
    static const int s_nEvery = getenv("LT_GLSHOT_EVERY") ? atoi(getenv("LT_GLSHOT_EVERY")) : 300;
    static int s_nFrame = 0;
    if (!s_pShot || s_nEvery <= 0 || (++s_nFrame % s_nEvery) != 0) return;
    std::vector<unsigned char> px((size_t)nOutW * nOutH * 3);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, nOutW, nOutH, GL_RGB, GL_UNSIGNED_BYTE, &px[0]);
    char path[1024];
    snprintf(path, sizeof(path), "%s/glshot_%05d.ppm", s_pShot, s_nFrame);
    if (FILE* f = fopen(path, "wb"))
    {
        fprintf(f, "P6\n%d %d\n255\n", nOutW, nOutH);
        for (int y = nOutH - 1; y >= 0; --y) fwrite(&px[(size_t)y * nOutW * 3], 1, (size_t)nOutW * 3, f);
        fclose(f);
    }
}

void LTMetal_EndFrame()
{
    if (!g_bInPass) return;
    g_bInPass = false;
    if (g_bWalkFrame)
    {
        int w = 0, h = 0;
        if (LTGLWin_DrawableSize(&w, &h))
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_Off.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glDisable(GL_SCISSOR_TEST);
            // The target is top-down, the window bottom-up: flip on the way.
            glBlitFramebuffer(0, 0, g_Off.w, g_Off.h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            GLShot(w, h);
            LTGLWin_Swap();
        }
        g_bWalkFrame = false;
    }
    g_bOffscreen = false;
}


// ======================================================================= //
// THE COMPOSITOR'S GPU HALF -- see ltcomp.h
// ======================================================================= //

bool LTMetal_CompAttach()
{
    if (!LTGLWin_HasWindow()) { Fail("no window for the compositor"); return false; }
    return EnsureDevice();
}

bool LTMetal_CompOutputSize(int* pW, int* pH)
{
    return LTGLWin_DrawableSize(pW, pH);
}

static void CompAcquire()
{
    if (g_bFrameOpen) return;
    g_bFrameOpen = true;
    StreamFrameStart();
}

bool LTMetal_BeginScene(int nOutW, int nOutH, int vx, int vy, int vw, int vh, bool bClear)
{
    if (nOutW <= 0 || nOutH <= 0 || vw <= 0 || vh <= 0) return false;
    if (!EnsureDevice()) return false;
    CompAcquire();
    const int ns = SceneSamples();

    if (g_SceneRes.w != nOutW || g_SceneRes.h != nOutH || !g_SceneRes.fbo)
    {
        if (!MakeTarget(g_SceneRes, nOutW, nOutH, 1, ns <= 1)) { Fail("compositor scene target failed"); return false; }
        if (ns > 1 && !MakeTarget(g_SceneMS, nOutW, nOutH, ns, true)) { Fail("compositor MSAA target failed"); return false; }
        bClear = true;
    }
    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > nOutW) vw = nOutW - vx;
    if (vy + vh > nOutH) vh = nOutH - vy;
    if (vw <= 0 || vh <= 0) return false;

    g_nTris = 0;
    g_SceneFBO = (ns > 1) ? g_SceneMS.fbo : g_SceneRes.fbo;
    BindTarget(g_SceneFBO, nOutW, nOutH);
    ClearColourDepth(0.055f, 0.067f, 0.086f, bClear);
    g_VP[0] = vx; g_VP[1] = vy; g_VP[2] = vw; g_VP[3] = vh;
    g_bSceneVP = true;
    ApplySceneVP();
    if (ns > 1) glEnable(GL_MULTISAMPLE);
    g_bInPass = true;
    return true;
}

void LTMetal_SceneLightScale(const float* pScale3, const float* pAdd3)
{
    if (!g_bInPass || !g_bSceneVP || !pScale3 || !pAdd3) return;
    const bool bScale = fabsf(pScale3[0]-1.0f) > 1e-3f || fabsf(pScale3[1]-1.0f) > 1e-3f || fabsf(pScale3[2]-1.0f) > 1e-3f;
    const bool bAdd   = pAdd3[0] > 1e-3f || pAdd3[1] > 1e-3f || pAdd3[2] > 1e-3f;
    if (!bScale && !bAdd) return;
    FullscreenState();
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_CONSTANT_COLOR, GL_ZERO, GL_ONE);
    glBlendColor(pScale3[0], pScale3[1], pScale3[2], 1.0f);
    glUseProgram(g_ProgScale);
    const float add[4] = { pAdd3[0], pAdd3[1], pAdd3[2], 0.0f };
    glUniform4fv(g_locAdd, 1, add);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void LTMetal_EndScene()
{
    if (!g_bInPass) return;
    g_bInPass = false;
    g_bSceneVP = false;
    glDisable(GL_SCISSOR_TEST);
    if (g_SceneMS.fbo && g_SceneFBO == g_SceneMS.fbo)
    {
        // ⚠️ RESOLVE, AND KEEP THE SAMPLES: a second camera this frame loads them.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_SceneMS.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_SceneRes.fbo);
        glBlitFramebuffer(0, 0, g_SceneMS.w, g_SceneMS.h, 0, 0, g_SceneRes.w, g_SceneRes.h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool LTMetal_Composite(const unsigned char* pB, const unsigned char* pG,
                       int nLayerW, int nLayerH, const LTCompMap& map,
                       int nOutW, int nOutH, bool bPresent, unsigned char* pRead)
{
    if (!pB || !pG || nLayerW <= 0 || nLayerH <= 0 || nOutW <= 0 || nOutH <= 0) return false;
    if (!EnsureDevice()) return false;
    CompAcquire();

    const bool bRealloc = (g_LayerW != nLayerW || g_LayerH != nLayerH || !g_LayerB);
    int wB = g_LayerW, hB = g_LayerH;
    UploadLayer(g_LayerB, wB, hB, nLayerW, nLayerH, pB, bRealloc);
    UploadLayer(g_LayerG, g_LayerW, g_LayerH, nLayerW, nLayerH, pG, bRealloc);

    const bool bWindow = bPresent && LTGLWin_HasWindow();
    if (bWindow)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);     // the window's own orientation; the shader flips
    }
    else
    {
        if (!MakeTarget(g_CompOff, nOutW, nOutH, 1, false)) { g_bFrameOpen = false; return false; }
        glBindFramebuffer(GL_FRAMEBUFFER, g_CompOff.fbo);
    }
    glViewport(0, 0, nOutW, nOutH);
    glDisable(GL_SCISSOR_TEST);
    FullscreenState();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    const int scene = (map.m_bScene && g_SceneRes.colTex) ? 1 : ((map.m_bMenuScene && g_SceneRes.colTex) ? 2 : 0);
    glUseProgram(g_ProgComp);
    const float dst[4] = { (float)map.m_DstX, (float)map.m_DstY, (float)map.m_DstW, (float)map.m_DstH };
    const float src[4] = { map.m_SrcX, map.m_SrcY, map.m_SrcW, map.m_SrcH };
    const float lay[4] = { (float)nLayerW, (float)nLayerH, (float)nOutH, bWindow ? 1.0f : 0.0f };
    glUniform4fv(g_locDst, 1, dst);
    glUniform4fv(g_locSrc, 1, src);
    glUniform4fv(g_locLayer, 1, lay);
    glUniform1i(g_locScene, scene);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, scene ? g_SceneRes.colTex : g_Dummy);
    glBindSampler(0, g_LayerSampler);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_LayerB);
    glBindSampler(1, g_LayerSampler);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, g_LayerG);
    glBindSampler(2, g_LayerSampler);
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (bWindow)
    {
        GLShot(nOutW, nOutH);
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        LTGLWin_Swap();
    }
    else if (pRead)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_CompOff.fbo);
        glReadPixels(0, 0, nOutW, nOutH, GL_RGBA, GL_UNSIGNED_BYTE, pRead);
        const size_t n = (size_t)nOutW * nOutH * 4;
        for (size_t i = 3; i < n; i += 4) pRead[i] = 255;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_bFrameOpen = false;           // the host frame ends with its composite
    return true;
}

bool LTMetal_TakeGpuTime(double* pMs, int* pnFrames, int* pnErrors)
{
    if (pMs) *pMs = 0.0;
    if (pnFrames) *pnFrames = 0;
    if (pnErrors) *pnErrors = 0;
    return false;
}

void LTMetal_CompFrameEnd() { g_bFrameOpen = false; }

void LTMetal_CompShutdown()
{
    if (!g_bPipe) return;
    if (!LTGLWin_EnsureContext(false)) return;
    glFinish();
    FreeTarget(g_SceneMS); FreeTarget(g_SceneRes); FreeTarget(g_CompOff);
}


// ======================================================================= //
// The software path's present (ltglwin.h)
// ======================================================================= //

bool LTGL_BlitRGBA(const unsigned char* pRGBA, int nWidth, int nHeight, int nOutW, int nOutH)
{
    if (!pRGBA || nWidth <= 0 || nHeight <= 0 || nOutW <= 0 || nOutH <= 0) return false;
    if (!g_bPipe)
    {
        // The software renderer still needs the blit program, but must not
        // mark the GPU backend as up (LTMetal_Available stays false).
        if (!BuildPipeline()) { g_bFail = false; return false; }
    }
    const bool bRealloc = (g_BlitW != nWidth || g_BlitH != nHeight || !g_BlitTex);
    UploadLayer(g_BlitTex, g_BlitW, g_BlitH, nWidth, nHeight, pRGBA, bRealloc);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glViewport(0, 0, nOutW, nOutH);
    glDisable(GL_SCISSOR_TEST);
    FullscreenState();
    glUseProgram(g_ProgBlit);
    const float sz[4] = { (float)nOutW, (float)nOutH, 0.0f, 0.0f };
    glUniform4fv(g_locBlitSize, 1, sz);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_BlitTex);
    static GLuint s_Samp = 0;
    if (!s_Samp)
    {
        static const bool bNearest = getenv("LT_BLITNEAREST") != 0;
        s_Samp = MakeSampler(bNearest ? GL_NEAREST : GL_LINEAR, bNearest ? GL_NEAREST : GL_LINEAR, GL_CLAMP_TO_EDGE, 1.0f);
    }
    glBindSampler(0, s_Samp);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    return true;
}
#endif  // !__APPLE__
