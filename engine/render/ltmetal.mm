// ----------------------------------------------------------------------- //
//
// MODULE  : ltmetal.mm
//
// See ltmetal.h.
//
// ----------------------------------------------------------------------- //

#include <atomic>
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "ltmetal.h"
#include "ltwindow.h"
#include "ltcomp.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <vector>

// ----------------------------------------------------------------------- //
//
// ⚠️ THE SHADER PAIR, AND IT IS DELIBERATELY THE SAME ARITHMETIC AS lt3d.
//
// The fragment stage multiplies a texture sample by an interpolated vertex
// colour, and that colour arrives ALREADY SCALED by the caller -- the 1/128
// that turns NOLF's stored bytes into a multiplier lives in ltworldrender,
// once, for both backends. Duplicating it here would be the first step to two
// renderers that disagree about lighting.
//
// ⚠️ Alpha test is a `discard_fragment()`, not a blend, matching the software
// path's punch-through rule (lt3d.cpp): a rejected texel must write neither
// colour nor depth, or a fence occludes what is behind it with nothing.
//
// ⚠️ BLENDING IS A SEPARATE PIPELINE, not a uniform. Fixed-function blend
// state lives in the pipeline object in Metal, so a translucent surface needs
// its own MTLRenderPipelineState (source-alpha over) AND its own depth state
// (test, but DO NOT WRITE) -- exactly the pair of behaviours lt3d.cpp's
// blended write path has. The shader's only share of it is emitting the
// texture's alpha instead of a hard 1.0, which is what `blend` selects.
//
// ----------------------------------------------------------------------- //

static const char* kShaderSrc = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VIn {
    float3 pos [[attribute(0)]];
    float2 uv  [[attribute(1)]];
    float3 rgb [[attribute(2)]];
};

struct VOut {
    float4 pos [[position]];
    float2 uv;
    // The view depth (clip w) for linear fog -- what the software rasteriser
    // reads back as 1/w from its depth buffer.
    float  fogZ;
    // ⚠️ AND THE VIEW-SPACE X, Y (clip x, y times 1/P00, 1/P11), so the
    // fragment can take the RANGE, as LT2 fogs (HANDOVER 5ch). Zero when the
    // pass asks for depth fog.
    float2 fogXY;

    // ⚠️ center_no_perspective = AFFINE, and it is the same deliberate choice
    // the software rasteriser makes (LT3DTarget::m_bAffineColour). Interpolating
    // this 0..1 lighting term perspective-correctly across a corridor floor
    // loses its low bits and comes back as a stipple -- the corridor-floor
    // striping that took nine hypotheses to pin down on the CPU side.
    //
    // ⚠️ The GPU is NOT immune: measured on the same view, floor high-frequency
    // energy was 13.63 affine against 20.11 perspective-correct. Texture
    // coordinates stay perspective-correct, because they span thousands of
    // texels and the same absolute error is nothing; colour spans 0..1, where
    // it is the whole signal.
    float3 rgb [[center_no_perspective]];

    // ⚠️ THE DEFAULT ONE since 2026-09-14. `rgb` above is the affine variant,
    // kept so both rules can be compared from a single binary -- see
    // LT_AFFINECOLOUR and HANDOVER §5i.
    float3 rgbPersp;
};

struct Uniforms {
    float4x4 mvp;
    int      alphaTest;
    // ⚠️ TAKES THE PLACE OF A PAD, so the constant buffer's size and every
    // offset in it are unchanged -- mvp 0..63, alphaTest 64, alphaRef 68.
    // Adding a field instead would have silently shifted the C++ mirror of
    // this struct out of agreement with the shader, and a constant buffer
    // that disagrees does not fail, it draws something plausible.
    float    alphaRef;
    // ⚠️ TAKES THE PLACE OF THE OTHER PAD, for the same reason alphaRef took
    // the first: the buffer stays 80 bytes and every existing offset is
    // unchanged. mvp 0..63, alphaTest 64, alphaRef 68, blend 72, constAlpha 76.
    //
    // ⚠️ THREE-VALUED rather than a bool, which is why the last pad could stay
    // a pad: 0 opaque, 1 blend with the texel's alpha, 2 blend WITHOUT it.
    // The third state is for a prop whose skin has an all-zero alpha channel
    // and whose translucency is entirely the object's constant -- 98 spotlight
    // beams in the shipped worlds. Reading the texel there draws nothing.
    int      blend;
    float    constAlpha;

    // ⚠️ AND HERE THE BUFFER FINALLY GREW, from 80 bytes to 96. Both pads were
    // already spent (alphaRef, then blend), and the C++ mirror below grew in
    // the same edit -- a constant buffer whose two declarations disagree does
    // not fail, it draws something plausible.
    int      affineColour;
    // ⚠️ TAKES pad_a: 1 = this cut-out goes out as COVERAGE (alpha-to-
    // coverage under MSAA, HANDOVER 5bs) instead of being discarded at the
    // reference. The buffer stays 128 bytes.
    int      a2c;
    // ⚠️ TAKE pad_b, pad_c (HANDOVER 5ch): 1/|P00|, 1/|P11| of the pass's
    // projection for RANGE fog; 0, 0 = depth fog. The buffer stays 128 bytes.
    float    fogKX, fogKY;

    // ⚠️ GROWN AGAIN, 96 -> 128 bytes, for FOG (HANDOVER 5bo). The float4 sits
    // at offset 96, which is 16-aligned, so no hidden padding is inserted --
    // and the C++ mirror in LTMetal_DrawTris grew in the same edit.
    float4   fogColor;      // rgb 0..1 (already black/white for add/multiply)
    float    fogNear;
    float    fogFar;
    int      fogOn;
    int      pad_d;
};

vertex VOut vs_world(VIn vin [[stage_in]],
                     constant Uniforms& u [[buffer(1)]])
{
    VOut o;
    o.pos = u.mvp * float4(vin.pos, 1.0);
    o.uv  = vin.uv;
    o.rgb = vin.rgb;
    o.rgbPersp = vin.rgb;
    o.fogZ = o.pos.w;
    o.fogXY = o.pos.xy * float2(u.fogKX, u.fogKY);
    return o;
}

fragment float4 fs_world(VOut in [[stage_in]],
                         constant Uniforms& u [[buffer(1)]],
                         texture2d<float> tex [[texture(0)]],
                         sampler smp [[sampler(0)]])
{
    float4 t = tex.sample(smp, in.uv);
    // The alpha's screen-space rate, for the sharpened coverage below. Taken
    // here, in uniform control flow, before any fragment can be discarded.
    const float fw = max(fwidth(t.a), 1.0 / 1024.0);
    // ⚠️ The texture's OWN threshold. This used to be a hard 0.5 while the
    // software rasteriser had already moved to the authored `alpharef`, so
    // the two backends disagreed on 231 shipped textures -- and the software
    // path is the reference the frame contract is built on.
    // ⚠️ A CUT-OUT UNDER ALPHA-TO-COVERAGE IS NOT DISCARDED AT THE REFERENCE:
    // its alpha is sharpened around it -- (a - ref) / fwidth(a) + 0.5 -- so the
    // edge falls exactly where the test put it, but spread over the samples of
    // ONE pixel instead of jumping between pixels. That is what turns a
    // stair-stepped grille into an anti-aliased one.
    float cover = 1.0;
    if (u.alphaTest != 0)
    {
        if (u.a2c != 0)
        {
            cover = clamp((t.a - u.alphaRef) / fw + 0.5, 0.0, 1.0);
            if (cover <= 0.0) discard_fragment();
        }
        else if (t.a < u.alphaRef) discard_fragment();
    }

    // ⚠️ The blended pipeline's source factor is SourceAlpha, so this is the
    // coverage the compositor uses; the opaque pipeline has blending off and
    // ignores it, but it still WRITES to the target's alpha channel, and the
    // software rasteriser leaves that at 255 on every path. Hence the hard 1.
    //
    // ⚠️ The SAME product as lt3d.cpp's blended path: the texel's alpha, when
    // it means anything, times the object's constant.
    float a = (u.blend == 2 ? 1.0 : t.a) * u.constAlpha;
    // ⚠️ PERSPECTIVE-CORRECT BY DEFAULT since 2026-09-14. The affine variant
    // is kept because it is still what LT_AFFINECOLOUR asks for, and because
    // the two must be comparable from one binary; see lt3d.cpp.
    float3 vc = (u.affineColour != 0) ? in.rgb : in.rgbPersp;
    float3 c = t.rgb * vc;
    // ⚠️ LINEAR DEPTH FOG, the software rasteriser's rule (lt3d.cpp): before
    // the blend, toward the colour the C++ side chose for this blend mode.
    if (u.fogOn != 0 && u.fogFar != u.fogNear)
    {
        // RANGE when the pass gave the tangents (LT2's rule), else depth.
        const float fd = (u.fogKX != 0.0 || u.fogKY != 0.0)
                       ? length(float3(in.fogXY, in.fogZ)) : in.fogZ;
        float f = clamp((fd - u.fogNear) / (u.fogFar - u.fogNear), 0.0, 1.0);
        c = mix(c, u.fogColor.rgb, f);
    }
    return float4(c, u.blend != 0 ? clamp(a, 0.0, 1.0) : cover);
}

// ---- GPU SKINNING (HANDOVER 5cd) ------------------------------------------
//
// LTModelDraw::Draw's per-vertex work, moved here VERBATIM in meaning: the
// bias-weighted position sum divided by the bias sum (SkinPosition), the
// normal through R_i * R_bind_i^T (SkinNormal; the CPU folds the two into
// one 3x3 per node), the X mirror, the object's normal matrix, and the
// level's lighting -- or the fixed two-sided key light when there is none.
// The mesh (vertices, weights, per-CORNER UVs) is uploaded once per model;
// a frame sends only the node palette and these uniforms. The fragment stage
// is fs_world, unchanged.
struct SCorner { uint vert; float u; float v; uint pad; };
struct SVert   { packed_float3 pos; uint wFirst; packed_float3 nrm; uint wCount; };
struct SWeight { packed_float3 offset; float bias; uint node; uint pad0; uint pad1; uint pad2; };

struct SkinU {
    float4 nrm0, nrm1, nrm2;        // the object normal matrix, rows
    int    pose;                    // 1 = skin through the palette
    int    mirror;                  // model X mirror
    int    mirrorU;                 // texture U flip
    int    lit;                     // 1 = level lighting, 0 = key light
    int    skinNormal;              // 0 = LT_NOSKINNORMAL
    int    biasNorm;                // 0 = LT_NOBIASNORM
    int    nLights;
    int    nNodes;
    float4 ambient;
    float4 dir[8];
    float4 color[8];
};

vertex VOut vs_skin(uint vid [[vertex_id]],
                    constant Uniforms& u        [[buffer(1)]],
                    const device SCorner* cor   [[buffer(2)]],
                    const device SVert*   vtx   [[buffer(3)]],
                    const device SWeight* wts   [[buffer(4)]],
                    const device float4*  pal   [[buffer(5)]],
                    constant SkinU& s           [[buffer(6)]])
{
    SCorner c = cor[vid];
    SVert   v = vtx[c.vert];
    float3 p = float3(v.pos);
    float3 n = float3(v.nrm);

    if (s.pose != 0 && v.wCount > 0)
    {
        float3 ps = float3(0.0);
        float3 ns = float3(0.0);
        float  sum = 0.0;
        for (uint i = 0; i < v.wCount; ++i)
        {
            SWeight w = wts[v.wFirst + i];
            if (w.node >= (uint)s.nNodes) continue;
            const uint b = w.node * 6;
            float3 o = float3(w.offset);
            ps += float3(dot(pal[b+0].xyz, o) + pal[b+0].w * w.bias,
                         dot(pal[b+1].xyz, o) + pal[b+1].w * w.bias,
                         dot(pal[b+2].xyz, o) + pal[b+2].w * w.bias);
            ns += float3(dot(pal[b+3].xyz, n), dot(pal[b+4].xyz, n),
                         dot(pal[b+5].xyz, n)) * w.bias;
            sum += w.bias;
        }
        if (s.biasNorm != 0 && sum > 1e-6) ps *= (1.0 / sum);
        p = ps;
        if (s.skinNormal != 0)
        {
            float l = length(ns);
            if (l > 1e-6) n = ns / l;
        }
    }

    if (s.mirror != 0) { p.x = -p.x; n.x = -n.x; }

    float3 nw = float3(dot(s.nrm0.xyz, n), dot(s.nrm1.xyz, n), dot(s.nrm2.xyz, n));

    float3 rgb;
    if (s.lit != 0)
    {
        float nl = length(nw);
        if (nl > 1e-6) nw = nw / nl;
        float3 cc = s.ambient.rgb;
        for (int li = 0; li < s.nLights; ++li)
        {
            float dd = -dot(nw, s.dir[li].xyz);
            if (dd <= 0.0) continue;
            cc += s.color[li].rgb * dd;
        }
        rgb = min(cc, float3(1.0));
    }
    else
    {
        float d = abs(dot(nw, float3(-0.40824829, 0.81649658, 0.40824829)));
        float l = min(0.35 + 0.65 * d, 1.0);
        rgb = float3(l);
    }

    VOut o;
    o.pos = u.mvp * float4(p, 1.0);
    o.uv  = float2(s.mirrorU != 0 ? -c.u : c.u, c.v);
    o.rgb = rgb;
    o.rgbPersp = rgb;
    o.fogZ = o.pos.w;
    o.fogXY = o.pos.xy * float2(u.fogKX, u.fogKY);
    return o;
}
)METAL";

namespace
{
    id<MTLDevice>               g_Device   = nil;
    id<MTLCommandQueue>         g_Queue    = nil;
    id<MTLRenderPipelineState>  g_Pipe     = nil;
    id<MTLRenderPipelineState>  g_PipeBlend = nil;

    // ⚠️ THREE COMPOSITING MODES, THREE PIPELINE OBJECTS. Blend factors are
    // fixed function and live in the pipeline, so "additive" is not a uniform
    // this backend can switch -- it is a different object. See
    // LT3DTexture::m_nBlendOp for what the three are and which props ask for
    // them.
    id<MTLRenderPipelineState>  g_PipeAdd   = nil;
    id<MTLRenderPipelineState>  g_PipeMul   = nil;

    // ⚠️ THE SKINNING TWINS (HANDOVER 5cd): the same four blend states with
    // vs_skin, no vertex descriptor -- vs_skin reads its buffers by index.
    // 0 opaque, 1 over, 2 additive, 3 multiply; [4] the scene's A2C.
    id<MTLRenderPipelineState>  g_PipeSkin[4]      = { nil, nil, nil, nil };
    id<MTLRenderPipelineState>  g_ScenePipeSkin[5] = { nil, nil, nil, nil, nil };
    id<MTLDepthStencilState>    g_Depth    = nil;

    // ⚠️ Tests but does not WRITE. A translucent pane must still be hidden by
    // the wall in front of it, and must still let the next pane behind it
    // draw -- see lt3d.cpp's blended path, which skips its depth store.
    id<MTLDepthStencilState>    g_DepthNoWrite = nil;

    // Which pair the open encoder currently has bound; -1 after every new
    // encoder, because render state does not survive one ending.
    int                         g_nEncMode = -1;
    id<MTLSamplerState>         g_Sampler  = nil;

    // ⚠️ NEAREST, FOR ALPHA-TESTED TEXTURES ONLY, and it is not a quality
    // setting -- it is the software rasteriser's rule (LT3DTarget::m_bBilinear
    // says so in as many words). Filtering across a punch-through edge mixes
    // in the RGB of texels the alpha test is about to throw away, and in a DTX
    // those are whatever the artist left under the cut-out, so every leaf and
    // grating gets a dark fringe. Metal filtered them anyway, which cost 9.63
    // mean absolute against the software path on T01S01's foliage.
    id<MTLSamplerState>         g_SamplerAT = nil;
    CAMetalLayer*               g_Layer    = nil;
    id<MTLTexture>              g_DepthTex = nil;

    id<MTLCommandBuffer>        g_Cmd      = nil;
    id<CAMetalDrawable>         g_Drawable = nil;
    id<MTLRenderCommandEncoder> g_Enc      = nil;

    std::vector<id<MTLTexture> > g_Textures;   // handle 1..N -> index 0..N-1

    // ⚠️ A PER-FRAME VERTEX ARENA, because `setVertexBytes` is capped at 4 KB
    // and a level is three orders of magnitude past that. Vertices are appended
    // into one buffer and each draw points at its own offset; the cursor resets
    // every frame. Without this the geometry silently stops at 4 KB, which
    // draws a recognisable corner of the level and looks like a culling bug.
    id<MTLTexture> g_OffTex    = nil;      // offscreen colour, for validation

    // ⚠️ THE COLOUR TARGET FOR THIS FRAME, whichever it is. LTMetal_ClearDepth
    // has to start a second render pass, and it used [g_Drawable texture]
    // directly -- which is nil offscreen, so the second encoder bound nothing
    // and every draw after the sky silently went nowhere. The frame came back
    // as sky and only sky, which reads as a depth or matrix bug and is neither.
    id<MTLTexture> g_ColourTarget = nil;
    bool           g_bOffscreen = false;

    // ⚠️ THE COMPOSITOR'S 3D TARGET (HANDOVER 5bq): the output's size, kept
    // across frames, never read back. While a scene pass is open the depth
    // clear below must restart on THIS depth and re-apply the camera's
    // viewport -- a new encoder forgets both, and the view weapon would be
    // drawn over the whole target from the top-left corner.
    id<MTLTexture> g_SceneTex   = nil;
    id<MTLTexture> g_SceneDepth = nil;
    int            g_SceneW = 0, g_SceneH = 0;
    bool           g_bSceneVP = false;
    MTLViewport    g_SceneViewport;
    MTLScissorRect g_SceneScissor;

    // ⚠️ MULTISAMPLED (HANDOVER 5bq, LT_MSAA, default 4): the scene pass draws
    // into g_SceneMSAA and resolves into g_SceneTex, which the composite reads.
    // Every pipeline that draws in the scene pass needs the same sample count,
    // so the four world pipelines exist twice -- g_ScenePipes is the scene's
    // set (0 opaque, 1 over, 2 additive, 3 multiply), null when 1 sample.
    id<MTLTexture>             g_SceneMSAA = nil;
    int                        g_nSceneSamples = 0;     // 0 = not decided yet
    id<MTLRenderPipelineState> g_ScenePipes[4] = { nil, nil, nil, nil };
    void SceneClearDepthQuad();

    // ⚠️ THE SCENE'S SAMPLERS: 16x anisotropic (LT_ANISO, 1..16) where the
    // old path keeps 4x -- the `metal` suite measures that one against the
    // software rasteriser, and its thresholds were set at 4.
    id<MTLSamplerState>        g_SceneSampler = nil, g_SceneSamplerAT = nil;

    // The opaque scene pipeline again with alpha-to-coverage on; null at 1x.
    id<MTLRenderPipelineState> g_ScenePipeA2C = nil;

    // LT_CUTOUTS=0: the scene pass samples cut-outs nearest and discards at
    // the reference, as the old path does, and uploads skip the colour fill.
    bool SceneCutouts()
    {
        static const bool s_b = []() {
            const char* p = getenv("LT_CUTOUTS");
            const bool b = !(p && p[0] == '0');
            if (!b) { printf("[COMP] LT_CUTOUTS=0 -- cut-outs nearest + alpha test, no colour fill\n"); fflush(stdout); }
            return b; }();
        return s_b;
    }

    id<MTLBuffer>  g_Arena     = nil;
    size_t         g_ArenaUsed = 0;
    size_t         g_ArenaCap  = 0;

    bool         g_bInit  = false;
    bool         g_bFail  = false;
    char         g_Err[256] = { 0 };
    unsigned int g_nTris  = 0;
    int          g_W = 0, g_H = 0;

    void Fail(const char* p)
    {
        snprintf(g_Err, sizeof(g_Err), "%s", p ? p : "?");
        g_bFail = true;
    }
    // ⚠️ Depth attachment is recreated with the target size, not once at
    // attach: the window is resizable, and a depth buffer that keeps the old
    // size silently stops testing against the right pixels.
    bool EnsureDepth(int w, int h)
    {
        if (g_DepthTex && (int)[g_DepthTex width] == w && (int)[g_DepthTex height] == h)
            return true;

        MTLTextureDescriptor* d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:(NSUInteger)w
                                                              height:(NSUInteger)h
                                                           mipmapped:NO];
        [d setUsage:MTLTextureUsageRenderTarget];
        [d setStorageMode:MTLStorageModePrivate];
        g_DepthTex = [g_Device newTextureWithDescriptor:d];
        return g_DepthTex != nil;
    }

    // ⚠️ Shared by the on-screen and offscreen paths, so the two cannot end up
    // with different pipeline or sampler state and therefore different pictures.
    // The device must already exist.
    bool BuildPipeline()
    {
        g_Queue = [g_Device newCommandQueue];
        if (!g_Queue) { Fail("no command queue"); return false; }

        NSError* err = nil;
        NSString* src = [NSString stringWithUTF8String:kShaderSrc];
        id<MTLLibrary> lib = [g_Device newLibraryWithSource:src options:nil error:&err];
        if (!lib)
        {
            Fail(err ? [[err localizedDescription] UTF8String] : "shader compile failed");
            return false;
        }

        id<MTLFunction> vs = [lib newFunctionWithName:@"vs_world"];
        id<MTLFunction> fs = [lib newFunctionWithName:@"fs_world"];
        if (!vs || !fs) { Fail("shader entry point missing"); return false; }

        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat3;
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2;
        vd.attributes[1].offset = sizeof(float) * 3;
        vd.attributes[1].bufferIndex = 0;
        vd.attributes[2].format = MTLVertexFormatFloat3;
        vd.attributes[2].offset = sizeof(float) * 5;
        vd.attributes[2].bufferIndex = 0;
        vd.layouts[0].stride = sizeof(LTMetalVert);
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        [pd setVertexFunction:vs];
        [pd setFragmentFunction:fs];
        [pd setVertexDescriptor:vd];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        [pd setDepthAttachmentPixelFormat:MTLPixelFormatDepth32Float];

        g_Pipe = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_Pipe)
        {
            Fail(err ? [[err localizedDescription] UTF8String] : "pipeline failed");
            return false;
        }
        id<MTLFunction> vsSkin = [lib newFunctionWithName:@"vs_skin"];
        auto skinTwin = [&](int k) {
            if (!vsSkin) return;
            MTLRenderPipelineDescriptor* sd = [pd copy];
            [sd setVertexFunction:vsSkin];
            [sd setVertexDescriptor:nil];
            NSError* e2 = nil;
            g_PipeSkin[k] = [g_Device newRenderPipelineStateWithDescriptor:sd error:&e2];
            if (!g_PipeSkin[k] && e2) { printf("[METAL] skinning pipeline %d failed: %s\n", k, [[e2 localizedDescription] UTF8String]); fflush(stdout); }
        };
        skinTwin(0);

        // ⚠️ THE SAME SHADER, A DIFFERENT BLEND STATE. Source-alpha over, which
        // is `sr*a + dst*(1-a)` -- lt3d.cpp's blended write path written as
        // fixed function. The two must stay arithmetically identical: the
        // software rasteriser is the reference the frame contract is built on,
        // and the `metal` suite compares them pixel for pixel.
        //
        // ⚠️ ALPHA IS DELIBERATELY NOT BLENDED (Zero, One = leave it alone).
        // The software path writes 255 into the destination alpha on every
        // path including the blended one; letting the colour factors apply
        // here too would decay the target's alpha and diverge for nothing.
        pd.colorAttachments[0].blendingEnabled             = YES;
        pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorZero;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;

        g_PipeBlend = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_PipeBlend)
        {
            Fail(err ? [[err localizedDescription] UTF8String] : "blend pipeline failed");
            return false;
        }
        skinTwin(1);

        // ADDITIVE: `dst = dst + src*a`, the light-beam mode.
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        g_PipeAdd = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_PipeAdd)
        {
            Fail(err ? [[err localizedDescription] UTF8String] : "additive pipeline failed");
            return false;
        }
        skinTwin(2);

        // MULTIPLY: `dst = dst * src`, and alpha takes no part -- the same
        // ZERO/SRCCOLOR pair D3D uses and lt3d.cpp copies.
        pd.colorAttachments[0].sourceRGBBlendFactor      = MTLBlendFactorDestinationColor;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorZero;
        g_PipeMul = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_PipeMul)
        {
            Fail(err ? [[err localizedDescription] UTF8String] : "multiply pipeline failed");
            return false;
        }
        skinTwin(3);

        // ⚠️ LESS, AND THE FIRST VERSION OF THIS COMMENT WAS THE BUG. It said
        // "GREATER, because this engine's depth is 1/w and nearer is larger" --
        // true of the SOFTWARE rasteriser, which writes its own depth buffer,
        // and irrelevant here. The GPU writes whatever the vertex shader puts
        // in z/w, which is the projection's clip z, where nearer is SMALLER.
        //
        // ⚠️ And Metal's clip volume is 0 <= z <= w, not OpenGL's -w <= z <= w
        // (which LT3D::Perspective produces). Everything nearer than the middle
        // of the range has z < 0 and is CLIPPED AWAY -- combined with the
        // inverted test, that rendered a completely blank frame. The caller
        // remaps z into Metal's range; see kMetalZ in ltworldmetal.cpp.
        MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
        [dd setDepthCompareFunction:MTLCompareFunctionLess];
        [dd setDepthWriteEnabled:YES];
        g_Depth = [g_Device newDepthStencilStateWithDescriptor:dd];

        // ⚠️ SAME TEST, NO WRITE, and the "no write" is the entire correctness
        // of the blended pass. A pane that stored depth would occlude the room
        // it is supposed to show, and the second pane behind it would vanish.
        [dd setDepthWriteEnabled:NO];
        g_DepthNoWrite = [g_Device newDepthStencilStateWithDescriptor:dd];

        // ⚠️ Trilinear + anisotropic, in ONE object. Everything the software
        // rasteriser needed several hundred lines and three opt-in flags for is
        // sampler state here.
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        [sd setMinFilter:MTLSamplerMinMagFilterLinear];
        [sd setMagFilter:MTLSamplerMinMagFilterLinear];
        [sd setMipFilter:MTLSamplerMipFilterLinear];
        [sd setSAddressMode:MTLSamplerAddressModeRepeat];
        [sd setTAddressMode:MTLSamplerAddressModeRepeat];
        [sd setMaxAnisotropy:4];
        g_Sampler = [g_Device newSamplerStateWithDescriptor:sd];

        // ⚠️ The MIP filter stays LINEAR. The software drops only the bilinear
        // tap for an alpha-tested texture; its mip selection and its
        // anisotropic taps are unchanged, so dropping those here too would
        // trade one disagreement for another.
        [sd setMinFilter:MTLSamplerMinMagFilterNearest];
        [sd setMagFilter:MTLSamplerMinMagFilterNearest];
        g_SamplerAT = [g_Device newSamplerStateWithDescriptor:sd];
        return true;
    }
}


bool LTMetal_Available() { return g_bInit && !g_bFail; }
const char* LTMetal_LastError() { return g_Err[0] ? g_Err : 0; }
unsigned int LTMetal_FrameTriangles() { return g_nTris; }


// ⚠️ Brings the pipeline up with NO window, so the parts most likely to be
// wrong -- shader source, the vertex layout matching the shader's attributes,
// the pipeline's pixel formats -- are checkable headlessly. "It compiled" is
// not evidence that a renderer draws; this is the least evidence that is not
// free. Uses the same BuildPipeline the real paths do, so it cannot pass while
// they would fail.
bool LTMetal_SelfTest(char* pErr, int nErrBytes)
{
    if (pErr && nErrBytes > 0) pErr[0] = 0;

    @autoreleasepool
    {
        if (!g_Device)
        {
            g_Device = MTLCreateSystemDefaultDevice();
            if (!g_Device)
            {
                if (pErr) snprintf(pErr, nErrBytes, "no Metal device");
                return false;
            }
        }
        if (!g_Pipe && !BuildPipeline())
        {
            if (pErr) snprintf(pErr, nErrBytes, "%s", g_Err[0] ? g_Err : "pipeline failed");
            g_bFail = false; g_Err[0] = 0;      // a self-test must not poison the real path
            return false;
        }
    }
    return true;
}


bool LTMetal_Attach(int nWidth, int nHeight)
{
    if (g_bInit) return !g_bFail;
    g_bInit = true;
    g_W = nWidth; g_H = nHeight;

    @autoreleasepool
    {
        g_Device = MTLCreateSystemDefaultDevice();
        if (!g_Device) { Fail("no Metal device"); return false; }
        if (!BuildPipeline()) return false;

        // The layer goes on the window's existing content view.
        NSView* v = (__bridge NSView*)LTWindow_GetContentView();
        if (!v) { Fail("no content view to attach to"); return false; }

        g_Layer = [CAMetalLayer layer];
        [g_Layer setDevice:g_Device];
        [g_Layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
        [g_Layer setFramebufferOnly:YES];
        [g_Layer setDrawableSize:CGSizeMake(nWidth, nHeight)];
        [g_Layer setFrame:[v bounds]];
        [v setWantsLayer:YES];
        [v setLayer:g_Layer];
    }

    // Handle 0 means "no texture", so index 0 is never used.
    g_Textures.clear();
    g_Textures.push_back(nil);
    return true;
}


void LTMetal_Shutdown()
{
    g_Enc = nil; g_Cmd = nil; g_Drawable = nil;
    g_Textures.clear();
    g_Arena = nil; g_ArenaUsed = g_ArenaCap = 0;
    g_DepthTex = nil; g_Sampler = nil; g_Depth = nil;
    g_Pipe = nil; g_Queue = nil; g_Device = nil; g_Layer = nil;
    g_bInit = false; g_bFail = false;
}


// Fills the colour of every texel below the reference from its known
// neighbours (8-connected, wrapping -- textures tile), ring by ring, up to 16
// rings; what is still unknown after that takes the mean opaque colour, so a
// far mip or an anisotropic tap lands on something neutral. Alpha untouched.
static void BleedCutout(unsigned char* p, int w, int h, unsigned char ref)
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
                known[i] = 2;           // filled this ring; a source only from the next
                fresh.push_back(i);
            }
        if (fresh.empty()) break;
        for (size_t k = 0; k < fresh.size(); ++k) known[fresh[k]] = 1;
    }
    const unsigned char mr = (unsigned char)(sr / nKnown), mg = (unsigned char)(sg / nKnown), mb = (unsigned char)(sb / nKnown);
    for (int i = 0; i < n; ++i)
        if (!known[i]) { p[i*4] = mr; p[i*4+1] = mg; p[i*4+2] = mb; }
}

unsigned int LTMetal_CreateTexture(const unsigned char* const* ppLevels,
                                   const int* pW, const int* pH, int nLevels,
                                   bool bCutout, float fAlphaRef)
{
    if (!LTMetal_Available() || !ppLevels || nLevels <= 0) return 0;
    if (pW[0] <= 0 || pH[0] <= 0) return 0;

    // The fill, on copies -- the CPU texture is the software rasteriser's.
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
        ppLevels = &levels[0];
    }

    @autoreleasepool
    {
        MTLTextureDescriptor* d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)pW[0]
                                                              height:(NSUInteger)pH[0]
                                                           mipmapped:(nLevels > 1)];
        [d setMipmapLevelCount:(NSUInteger)nLevels];
        [d setUsage:MTLTextureUsageShaderRead];
        id<MTLTexture> t = [g_Device newTextureWithDescriptor:d];
        if (!t) return 0;

        for (int i = 0; i < nLevels; ++i)
        {
            if (!ppLevels[i] || pW[i] <= 0 || pH[i] <= 0) continue;
            MTLRegion r = MTLRegionMake2D(0, 0, (NSUInteger)pW[i], (NSUInteger)pH[i]);
            [t replaceRegion:r
                 mipmapLevel:(NSUInteger)i
                   withBytes:ppLevels[i]
                 bytesPerRow:(NSUInteger)pW[i] * 4];
        }

        g_Textures.push_back(t);
        return (unsigned int)(g_Textures.size() - 1);
    }
}


bool LTMetal_BeginOffscreen(int nWidth, int nHeight, float fR, float fG, float fB)
{
    // ⚠️ Does NOT need a layer, so it works before -- or entirely without --
    // LTMetal_Attach. The device and pipeline are brought up on demand.
    if (!g_bInit)
    {
        @autoreleasepool
        {
            g_Device = MTLCreateSystemDefaultDevice();
            if (!g_Device) { Fail("no Metal device"); g_bInit = true; return false; }
            if (!BuildPipeline()) { g_bInit = true; return false; }
            g_Textures.clear();
            g_Textures.push_back(nil);
        }
        g_bInit = true;
    }
    if (g_bFail) return false;

    g_nTris = 0;
    g_ArenaUsed = 0;
    g_bOffscreen = true;

    @autoreleasepool
    {
        if (!g_OffTex || (int)[g_OffTex width] != nWidth || (int)[g_OffTex height] != nHeight)
        {
            MTLTextureDescriptor* d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:(NSUInteger)nWidth
                                                                  height:(NSUInteger)nHeight
                                                               mipmapped:NO];
            [d setUsage:(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)];
            // ⚠️ Shared, so the CPU can read it back without a blit encoder.
            [d setStorageMode:MTLStorageModeShared];
            g_OffTex = [g_Device newTextureWithDescriptor:d];
            if (!g_OffTex) { Fail("offscreen target failed"); return false; }
        }
        if (!EnsureDepth(nWidth, nHeight)) { Fail("offscreen depth failed"); return false; }

        g_Cmd = [g_Queue commandBuffer];

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        g_ColourTarget = g_OffTex;
        rp.colorAttachments[0].texture = g_OffTex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(fR, fG, fB, 1.0);
        rp.depthAttachment.texture = g_DepthTex;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
        rp.depthAttachment.clearDepth = 1.0;

        g_Enc = [g_Cmd renderCommandEncoderWithDescriptor:rp];
        [g_Enc setRenderPipelineState:g_Pipe];
        [g_Enc setDepthStencilState:g_Depth];
        [g_Enc setFragmentSamplerState:g_Sampler atIndex:0];
        g_nEncMode = 0;
    }
    return true;
}


bool LTMetal_ReadPixels(unsigned char* pRGBA, int nWidth, int nHeight)
{
    if (!pRGBA || !g_OffTex) return false;
    if ((int)[g_OffTex width] != nWidth || (int)[g_OffTex height] != nHeight) return false;

    // Kept across frames: the game reads back every frame now (3 MB at 1024x768).
    static std::vector<unsigned char> bgra;
    bgra.resize((size_t)nWidth * nHeight * 4);
    MTLRegion r = MTLRegionMake2D(0, 0, (NSUInteger)nWidth, (NSUInteger)nHeight);
    [g_OffTex getBytes:&bgra[0]
           bytesPerRow:(NSUInteger)nWidth * 4
            fromRegion:r
           mipmapLevel:0];

    // BGRA -> RGBA. The layer format is BGRA because that is what a drawable
    // wants; the rest of this engine is RGBA.
    for (size_t i = 0; i < bgra.size(); i += 4)
    {
        pRGBA[i+0] = bgra[i+2];
        pRGBA[i+1] = bgra[i+1];
        pRGBA[i+2] = bgra[i+0];
        pRGBA[i+3] = 255;
    }
    return true;
}


bool LTMetal_BeginFrame(float fR, float fG, float fB)
{
    if (!LTMetal_Available()) return false;
    g_nTris = 0;

    g_ArenaUsed = 0;

    g_Drawable = [g_Layer nextDrawable];
    if (!g_Drawable) return false;

    const int w = (int)[[g_Drawable texture] width];
    const int h = (int)[[g_Drawable texture] height];
    if (!EnsureDepth(w, h)) { g_Drawable = nil; return false; }

    g_Cmd = [g_Queue commandBuffer];

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    g_ColourTarget = [g_Drawable texture];
    rp.colorAttachments[0].texture = g_ColourTarget;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(fR, fG, fB, 1.0);
    rp.depthAttachment.texture = g_DepthTex;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    rp.depthAttachment.clearDepth = 1.0;        // clip z: 1 is the far plane

    g_Enc = [g_Cmd renderCommandEncoderWithDescriptor:rp];
    [g_Enc setRenderPipelineState:g_Pipe];
    [g_Enc setDepthStencilState:g_Depth];
    [g_Enc setFragmentSamplerState:g_Sampler atIndex:0];
    g_nEncMode = 0;
    return true;
}


void LTMetal_ClearDepth()
{
    if (!g_Enc || !g_Cmd) return;

    // ⚠️ IN THE COMPOSITOR'S SCENE PASS THE CLEAR IS A DRAW, not a new pass: a
    // full-target triangle at the far plane with depth test ALWAYS and colour
    // writes off, inside the camera's viewport. Ending the encoder instead would
    // store and reload four samples per pixel of a Retina-size target twice a
    // frame (sky -> world, world -> view weapon) for nothing.
    if (g_bSceneVP) { SceneClearDepthQuad(); return; }

    // ⚠️ A clear mid-frame means ENDING the encoder and starting another whose
    // colour LOADS and whose depth CLEARS. Metal has no "clear now" command
    // inside a pass, and drawing the sky into a separate earlier pass is the
    // only way to get the software path's behaviour exactly.
    [g_Enc endEncoding];

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g_ColourTarget;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = g_bSceneVP ? g_SceneDepth : g_DepthTex;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    rp.depthAttachment.clearDepth = 1.0;

    g_Enc = [g_Cmd renderCommandEncoderWithDescriptor:rp];
    [g_Enc setRenderPipelineState:g_Pipe];
    [g_Enc setDepthStencilState:g_Depth];
    [g_Enc setFragmentSamplerState:g_Sampler atIndex:0];
    if (g_bSceneVP)
    {
        [g_Enc setViewport:g_SceneViewport];
        [g_Enc setScissorRect:g_SceneScissor];
    }
    g_nEncMode = 0;
}


// The fog the following draws use. See LTMetal_SetFog in ltmetal.h.
namespace { struct MetalFog { bool m_bOn; float m_fNear, m_fFar; float m_RGB[3]; float m_fKX, m_fKY; };
            MetalFog g_Fog = { false, 0.0f, 1.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f }; }

void LTMetal_SetFog(bool bOn, float fNear, float fFar, const float* pRGB255)
{
    g_Fog.m_bOn = bOn; g_Fog.m_fNear = fNear; g_Fog.m_fFar = fFar;
    for (int k = 0; k < 3; ++k) g_Fog.m_RGB[k] = pRGB255 ? pRGB255[k] : 0.0f;
}

void LTMetal_SetFogRange(float fKX, float fKY)
{
    g_Fog.m_fKX = fKX; g_Fog.m_fKY = fKY;
}

// ⚠️ ONE ENCODE FOR BOTH VERTEX SOURCES -- the per-frame arena
// (LTMetal_DrawTris) and a buffer made once (LTMetal_DrawTrisFrom, the static
// world, HANDOVER 5bu). Everything a draw decides -- uniforms, pipeline,
// sampler, fog -- is decided here once, so the two can only differ in where
// the vertices live.
// The skinned draw's extra state, or null for a plain vertex-buffer draw.
struct SkinEncode
{
    id<MTLBuffer> m_Mesh;   size_t m_nCornerOfs, m_nVertOfs, m_nWeightOfs;
    id<MTLBuffer> m_Pal;    size_t m_nPalOfs;
    const void*   m_pU;     size_t m_nUBytes;
};

static void EncodeTris(const float* pMVP, unsigned int nTexture,
                       id<MTLBuffer> vb, size_t nOffset, int nVerts,
                       bool bAlphaTest, float fAlphaRef, bool bBlend,
                       float fConstAlpha, bool bTexAlpha, int nBlendOp,
                       const SkinEncode* pSkin = 0)
{
    // ⚠️ MUST MATCH THE MSL STRUCT ABOVE FIELD FOR FIELD.
    // ⚠️ 128 bytes now (fog appended at 96), and it must match the MSL struct
    // above field for field.
    struct Uniforms { float mvp[16]; int alphaTest; float alphaRef; int blend;
                      float constAlpha; int affineColour; int a2c; float fogK[2];
                      float fogColor[4]; float fogNear; float fogFar; int fogOn; int padD; } u;
    static_assert(sizeof(Uniforms) == 128, "Uniforms must match the MSL struct (128 bytes)");

    // ⚠️ TRANSPOSED ON THE WAY IN. This engine stores matrices ROW-major and
    // multiplies row-vector style; Metal's float4x4 is column-major and the
    // shader does `mvp * v`. Handing the rows straight over draws a scene
    // mirrored about its diagonal, which looks like a camera bug.
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) u.mvp[c*4 + r] = pMVP[r*4 + c];

    u.alphaTest = bAlphaTest ? 1 : 0;

    // ⚠️ THE SAME FALLBACK RULE AS THE SOFTWARE PATH, deliberately duplicated
    // in words rather than shared through a header: anything outside (0,1] is
    // an unset or nonsense field and becomes the old half. If these two ever
    // disagree the backends diverge on exactly the textures this change
    // exists to make them agree about.
    u.alphaRef = (fAlphaRef > 0.0f && fAlphaRef <= 1.0f) ? fAlphaRef : 0.5f;
    u.blend      = bBlend ? (bTexAlpha ? 1 : 2) : 0;
    u.constAlpha = (fConstAlpha >= 0.0f && fConstAlpha <= 1.0f) ? fConstAlpha : 1.0f;

    // ⚠️ THE SAME SWITCH THE SOFTWARE RASTERISER READS, spelled out here rather
    // than shared through a header, exactly as the alpha-ref fallback is. If
    // the two ever disagree the instrument measures nothing.
    static const int nAffine = (getenv("LT_AFFINECOLOUR") != 0) ? 1 : 0;
    u.affineColour = nAffine;
    u.fogK[0] = g_Fog.m_fKX; u.fogK[1] = g_Fog.m_fKY;

    // ⚠️ FILTERED CUT-OUTS, the compositor's scene pass only (HANDOVER 5bs):
    // an opaque alpha-tested run draws through the alpha-to-coverage pipeline
    // and the trilinear/anisotropic sampler. Blended cut-outs keep the test
    // (coverage under a blend is not a thing) but are filtered too.
    const bool bCutoutsOn = SceneCutouts();
    const bool bA2C = g_bSceneVP && bCutoutsOn && bAlphaTest && !bBlend && g_ScenePipeA2C;
    u.a2c = bA2C ? 1 : 0;

    // Fog -- the current pass's (LTMetal_SetFog). Additive fogs toward black,
    // multiply toward white, as the software path does.
    u.fogOn = g_Fog.m_bOn ? 1 : 0;
    u.fogNear = g_Fog.m_fNear; u.fogFar = g_Fog.m_fFar; u.padD = 0;
    {
        const bool bAdd = bBlend && nBlendOp == 1, bMul = bBlend && nBlendOp == 2;
        for (int k = 0; k < 3; ++k)
            u.fogColor[k] = bAdd ? 0.0f : (bMul ? 1.0f : g_Fog.m_RGB[k] / 255.0f);
        u.fogColor[3] = 1.0f;
    }

    // ⚠️ SWITCHED PER RUN, not per frame, and tracked because an encoder does
    // not remember state across the mid-frame depth clear -- g_nEncMode is
    // reset to -1 nowhere and to 0 wherever an encoder is created, so a run
    // that wants the blended pair after a clear still rebinds it.
    // 0 opaque, 1 source-alpha over, 2 additive, 3 multiply.
    const int nMode = bBlend ? (1 + ((nBlendOp == 1 || nBlendOp == 2) ? nBlendOp : 0)) : 0;
    // Skinned runs are modes 8..12, so switching between the two vertex
    // stages always rebinds.
    const int nPipeMode = (bA2C ? 4 : nMode) + (pSkin ? 8 : 0);
    if (nPipeMode != g_nEncMode)
    {
        id<MTLRenderPipelineState> pipe = g_Pipe;
        if      (nMode == 1) pipe = g_PipeBlend;
        else if (nMode == 2) pipe = g_PipeAdd;
        else if (nMode == 3) pipe = g_PipeMul;
        if (g_bSceneVP && g_ScenePipes[nMode]) pipe = g_ScenePipes[nMode];
        if (bA2C) pipe = g_ScenePipeA2C;
        if (pSkin)
        {
            pipe = g_PipeSkin[nMode];
            if (g_bSceneVP && g_ScenePipeSkin[nMode]) pipe = g_ScenePipeSkin[nMode];
            if (bA2C && g_ScenePipeSkin[4]) pipe = g_ScenePipeSkin[4];
        }
        if (!pipe) return;      // a twin that failed to build draws nothing
        [g_Enc setRenderPipelineState:pipe];
        [g_Enc setDepthStencilState:(nMode ? g_DepthNoWrite : g_Depth)];
        g_nEncMode = nPipeMode;
    }

    // ⚠️ Per RUN, not per frame: one batch is alpha-tested and the next is not.
    //
    // ⚠️ LT_ATSAMPLER=0 lets Metal filter alpha-tested textures like everything
    // else. It is MEASURABLY WORSE overall -- T01S01 goes 6.23 -> 9.61 -- but
    // two worlds improve slightly, so the switch stays as the evidence.
    static const int nATS = getenv("LT_ATSAMPLER") ? atoi(getenv("LT_ATSAMPLER")) : 1;
    if (g_bSceneVP && g_SceneSampler)
        [g_Enc setFragmentSamplerState:((bAlphaTest && nATS && !bCutoutsOn) ? g_SceneSamplerAT : g_SceneSampler) atIndex:0];
    else
        [g_Enc setFragmentSamplerState:((bAlphaTest && nATS) ? g_SamplerAT : g_Sampler) atIndex:0];

    if (pSkin)
    {
        [g_Enc setVertexBuffer:pSkin->m_Mesh offset:pSkin->m_nCornerOfs atIndex:2];
        [g_Enc setVertexBuffer:pSkin->m_Mesh offset:pSkin->m_nVertOfs   atIndex:3];
        [g_Enc setVertexBuffer:pSkin->m_Mesh offset:pSkin->m_nWeightOfs atIndex:4];
        if (pSkin->m_Pal) [g_Enc setVertexBuffer:pSkin->m_Pal offset:pSkin->m_nPalOfs atIndex:5];
        else              [g_Enc setVertexBuffer:pSkin->m_Mesh offset:0 atIndex:5];   // unread: pose 0
        [g_Enc setVertexBytes:pSkin->m_pU length:pSkin->m_nUBytes atIndex:6];
    }
    else
        [g_Enc setVertexBuffer:vb offset:nOffset atIndex:0];
    [g_Enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [g_Enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [g_Enc setFragmentTexture:g_Textures[nTexture] atIndex:0];
    [g_Enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)nVerts];

    g_nTris += (unsigned int)(nVerts / 3);
}


void LTMetal_DrawTris(const float* pMVP, unsigned int nTexture,
                      const LTMetalVert* pVerts, int nVerts,
                      bool bAlphaTest, float fAlphaRef, bool bBlend,
                      float fConstAlpha, bool bTexAlpha, int nBlendOp)
{
    if (!g_Enc || !pVerts || nVerts < 3 || !pMVP) return;
    if (nTexture == 0 || nTexture >= g_Textures.size()) return;

    const size_t nBytes = sizeof(LTMetalVert) * (size_t)nVerts;

    // ⚠️ Grown, never shrunk, and only between frames -- resizing while an
    // encoder holds the old buffer would draw from freed memory.
    if (g_ArenaUsed + nBytes > g_ArenaCap)
    {
        size_t nWant = (g_ArenaUsed + nBytes) * 2;
        if (nWant < (size_t)(4 << 20)) nWant = (size_t)(4 << 20);
        id<MTLBuffer> nb = [g_Device newBufferWithLength:nWant
                                                 options:MTLResourceStorageModeShared];
        if (!nb) return;
        if (g_Arena && g_ArenaUsed)
            memcpy([nb contents], [g_Arena contents], g_ArenaUsed);
        g_Arena = nb;
        g_ArenaCap = nWant;
    }

    memcpy((unsigned char*)[g_Arena contents] + g_ArenaUsed, pVerts, nBytes);
    const size_t nOffset = g_ArenaUsed;
    g_ArenaUsed += nBytes;

    EncodeTris(pMVP, nTexture, g_Arena, nOffset, nVerts, bAlphaTest, fAlphaRef,
               bBlend, fConstAlpha, bTexAlpha, nBlendOp);
}


// ---- vertex buffers made once (HANDOVER 5bu) ------------------------------ //

namespace { std::vector<id<MTLBuffer> > g_Buffers(1, nil); }   // handle 0 = none

unsigned int LTMetal_CreateBuffer(const LTMetalVert* pVerts, int nVerts)
{
    if (!g_Device || !pVerts || nVerts <= 0) return 0;
    id<MTLBuffer> b = [g_Device newBufferWithBytes:pVerts
                                            length:sizeof(LTMetalVert) * (size_t)nVerts
                                           options:MTLResourceStorageModeShared];
    if (!b) return 0;
    for (size_t i = 1; i < g_Buffers.size(); ++i)
        if (!g_Buffers[i]) { g_Buffers[i] = b; return (unsigned int)i; }
    g_Buffers.push_back(b);
    return (unsigned int)(g_Buffers.size() - 1);
}

void LTMetal_FreeBuffer(unsigned int nBuffer)
{
    // A frame still in flight holds its own reference (ARC, retained command
    // buffer), so letting go here is safe.
    if (nBuffer > 0 && nBuffer < g_Buffers.size()) g_Buffers[nBuffer] = nil;
}

void LTMetal_DrawTrisFrom(const float* pMVP, unsigned int nTexture,
                          unsigned int nBuffer, int nFirst, int nVerts,
                          bool bAlphaTest, float fAlphaRef, bool bBlend,
                          float fConstAlpha, bool bTexAlpha, int nBlendOp)
{
    if (!g_Enc || nVerts < 3 || !pMVP || nFirst < 0) return;
    if (nTexture == 0 || nTexture >= g_Textures.size()) return;
    if (nBuffer == 0 || nBuffer >= g_Buffers.size() || !g_Buffers[nBuffer]) return;
    EncodeTris(pMVP, nTexture, g_Buffers[nBuffer], sizeof(LTMetalVert) * (size_t)nFirst,
               nVerts, bAlphaTest, fAlphaRef, bBlend, fConstAlpha, bTexAlpha, nBlendOp);
}


// ---- GPU skinning (HANDOVER 5cd) ------------------------------------------- //

namespace
{
    // One buffer per mesh: corners, then vertices, then weights, each
    // section 256-aligned (a buffer offset's alignment on every Mac GPU).
    struct SkinMesh { id<MTLBuffer> m_Buf; size_t m_nVertOfs, m_nWeightOfs; };
    std::vector<SkinMesh> g_SkinMeshes(1);          // handle 0 = none
    size_t Align256(size_t n) { return (n + 255) & ~(size_t)255; }

    // ⚠️ MUST MATCH `SkinU` IN THE SHADER, field for field (352 bytes).
    struct SkinUC
    {
        float nrm[3][4];
        int   pose, mirror, mirrorU, lit, skinNormal, biasNorm, nLights, nNodes;
        float ambient[4];
        float dir[8][4];
        float color[8][4];
    };
    static_assert(sizeof(SkinUC) == 352, "SkinUC must match the MSL SkinU");
}

unsigned int LTMetal_CreateSkinMesh(const LTMetalSkinVert* pVerts, int nVerts,
                                    const LTMetalSkinWeight* pWeights, int nWeights,
                                    const LTMetalSkinCorner* pCorners, int nCorners)
{
    static_assert(sizeof(LTMetalSkinVert) == 32 && sizeof(LTMetalSkinWeight) == 32
                  && sizeof(LTMetalSkinCorner) == 16, "skin structs must match the MSL ones");
    if (!g_Device || !pVerts || nVerts <= 0 || !pCorners || nCorners <= 0) return 0;
    const size_t nC = sizeof(LTMetalSkinCorner) * (size_t)nCorners;
    const size_t nV = sizeof(LTMetalSkinVert) * (size_t)nVerts;
    const size_t nW = sizeof(LTMetalSkinWeight) * (size_t)(nWeights > 0 ? nWeights : 1);
    const size_t oV = Align256(nC), oW = oV + Align256(nV);
    id<MTLBuffer> b = [g_Device newBufferWithLength:oW + nW options:MTLResourceStorageModeShared];
    if (!b) return 0;
    unsigned char* p = (unsigned char*)[b contents];
    memset(p, 0, oW + nW);
    memcpy(p, pCorners, nC);
    memcpy(p + oV, pVerts, nV);
    if (nWeights > 0 && pWeights) memcpy(p + oW, pWeights, sizeof(LTMetalSkinWeight) * (size_t)nWeights);
    SkinMesh m; m.m_Buf = b; m.m_nVertOfs = oV; m.m_nWeightOfs = oW;
    for (size_t i = 1; i < g_SkinMeshes.size(); ++i)
        if (!g_SkinMeshes[i].m_Buf) { g_SkinMeshes[i] = m; return (unsigned int)i; }
    g_SkinMeshes.push_back(m);
    return (unsigned int)(g_SkinMeshes.size() - 1);
}

void LTMetal_FreeSkinMesh(unsigned int nMesh)
{
    // A frame in flight keeps its own reference (retained command buffer).
    if (nMesh > 0 && nMesh < g_SkinMeshes.size()) g_SkinMeshes[nMesh].m_Buf = nil;
}

int LTMetal_PushPalette(const float* pPalette, int nNodes)
{
    if (!g_Enc || !pPalette || nNodes <= 0) return -1;
    const size_t nBytes = sizeof(float) * 24 * (size_t)nNodes;
    const size_t nStart = Align256(g_ArenaUsed);
    if (nStart + nBytes > g_ArenaCap)
    {
        size_t nWant = (nStart + nBytes) * 2;
        if (nWant < (size_t)(4 << 20)) nWant = (size_t)(4 << 20);
        id<MTLBuffer> nb = [g_Device newBufferWithLength:nWant options:MTLResourceStorageModeShared];
        if (!nb) return -1;
        if (g_Arena && g_ArenaUsed) memcpy([nb contents], [g_Arena contents], g_ArenaUsed);
        g_Arena = nb;
        g_ArenaCap = nWant;
    }
    memcpy((unsigned char*)[g_Arena contents] + nStart, pPalette, nBytes);
    g_ArenaUsed = nStart + nBytes;
    return (int)nStart;
}

void LTMetal_DrawSkinned(const float* pMVP, unsigned int nTexture, unsigned int nMesh,
                         int nFirstCorner, int nCorners, const LTMetalSkinDraw& sd,
                         bool bAlphaTest, float fAlphaRef, bool bBlend,
                         float fConstAlpha, bool bTexAlpha, int nBlendOp)
{
    if (!g_Enc || !pMVP || nCorners < 3 || nFirstCorner < 0) return;
    if (nTexture == 0 || nTexture >= g_Textures.size()) return;
    if (nMesh == 0 || nMesh >= g_SkinMeshes.size() || !g_SkinMeshes[nMesh].m_Buf) return;
    if (!g_PipeSkin[0]) return;

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
    se.m_nCornerOfs = sizeof(LTMetalSkinCorner) * (size_t)nFirstCorner;
    se.m_nVertOfs = m.m_nVertOfs;
    se.m_nWeightOfs = m.m_nWeightOfs;
    se.m_Pal = (u.pose && g_Arena) ? g_Arena : nil;
    se.m_nPalOfs = u.pose ? (size_t)sd.m_nPaletteOfs : 0;
    se.m_pU = &u; se.m_nUBytes = sizeof(u);
    EncodeTris(pMVP, nTexture, nil, 0, nCorners, bAlphaTest, fAlphaRef, bBlend,
               fConstAlpha, bTexAlpha, nBlendOp, &se);
}

bool LTMetal_SkinningAvailable() { return g_PipeSkin[0] != nil; }


void LTMetal_EndFrame()
{
    if (!g_Enc) return;
    [g_Enc endEncoding];

    if (g_bOffscreen)
    {
        // ⚠️ WAIT. There is no drawable to present, and the readback that
        // follows would otherwise race the GPU and return whatever the target
        // held before -- which on a first frame is a plausible-looking clear
        // colour, i.e. a silent wrong answer.
        [g_Cmd commit];
        [g_Cmd waitUntilCompleted];
        g_bOffscreen = false;
    }
    else
    {
        [g_Cmd presentDrawable:g_Drawable];
        [g_Cmd commit];
    }
    g_Enc = nil; g_Cmd = nil; g_Drawable = nil; g_ColourTarget = nil;
}


// ======================================================================= //
//
// THE COMPOSITOR'S GPU HALF -- see ltcomp.h and HANDOVER 5bq.
//
// ======================================================================= //

static const char* kCompShaderSrc = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct CompOut { float4 pos [[position]]; };

// One triangle that covers the whole target; no vertex buffer.
vertex CompOut vs_full(uint vid [[vertex_id]])
{
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    CompOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    return o;
}

struct CompU
{
    float4 dst;         // output rect x, y, w, h (pixels)
    float4 src;         // 2D-layer rect x, y, w, h (pixels)
    float4 layer;       // 2D-layer size w, h
    int    scene;       // the 3D target underlies dst
    int    pad0, pad1, pad2;
};

// ⚠️ out = scene * M + A, the affine trick of ltcomp.h. The layers are
// filtered BEFORE they are combined, which is exact: the result is linear in
// both, so a HUD edge over the 3D blends smoothly instead of stepping.
fragment float4 fs_comp(CompOut in [[stage_in]],
                        constant CompU& u [[buffer(0)]],
                        texture2d<float> scene [[texture(0)]],
                        texture2d<float> lb    [[texture(1)]],
                        texture2d<float> lg    [[texture(2)]],
                        sampler smp [[sampler(0)]])
{
    float2 p = in.pos.xy;
    float2 s  = u.src.xy + (p - u.dst.xy) * (u.src.zw / u.dst.zw);
    float3 a, g;
    const bool inside = !(p.x < u.dst.x || p.y < u.dst.y ||
                          p.x >= u.dst.x + u.dst.z || p.y >= u.dst.y + u.dst.w);
    if (inside)
    {
        float2 uv = s / u.layer.xy;
        a = lb.sample(smp, uv).rgb;
        g = lg.sample(smp, uv).rgb;
    }
    else
    {
        // No 3D behind (a menu) -- or the menu box over its own 3D (5ce,
        // scene 2): the bars are black.
        if (u.scene != 1) return float4(0.0, 0.0, 0.0, 1.0);

        // ⚠️ OUTSIDE THE 2D, OVER THE 3D: WHAT THE WHOLE ROW AGREES ON. Three
        // votes along the row (or column, above and below): the near edge, the
        // far edge and the centre. A full-screen fade, flash or cinematic bar is
        // the same at all three, so it carries on to the window's edge; a
        // message bar or icon touching ONE edge is outvoted by the other two,
        // so the side shows the fade behind it instead of a smear of it. No
        // two agreeing: the 3D alone.
        float2 lo = u.src.xy + 0.5, hi = u.src.xy + u.src.zw - 0.5;
        float2 e  = floor(clamp(s, lo, hi)) + 0.5;
        float2 far2, mid2;
        if (s.x < lo.x || s.x > hi.x)
        {
            far2 = float2(floor(s.x < lo.x ? hi.x : lo.x) + 0.5, e.y);
            mid2 = float2(floor(u.src.x + u.src.z * 0.5) + 0.5, e.y);
        }
        else
        {
            far2 = float2(e.x, floor(s.y < lo.y ? hi.y : lo.y) + 0.5);
            mid2 = float2(e.x, floor(u.src.y + u.src.w * 0.5) + 0.5);
        }
        const float2 il = 1.0 / u.layer.xy;
        float3 an = lb.sample(smp, e * il).rgb,    gn = lg.sample(smp, e * il).rgb;
        float3 af = lb.sample(smp, far2 * il).rgb, gf = lg.sample(smp, far2 * il).rgb;
        float3 am = lb.sample(smp, mid2 * il).rgb, gm = lg.sample(smp, mid2 * il).rgb;
        const float k = 2.5 / 255.0;
        float3 dnf = max(abs(an - af), abs(gn - gf));
        float3 dfm = max(abs(af - am), abs(gf - gm));
        float3 dnm = max(abs(an - am), abs(gn - gm));
        if      (max(dnf.r, max(dnf.g, dnf.b)) <= k) { a = an; g = gn; }
        else if (max(dfm.r, max(dfm.g, dfm.b)) <= k) { a = af; g = gf; }
        else if (max(dnm.r, max(dnm.g, dnm.b)) <= k) { a = an; g = gn; }
        else { a = float3(0.0); g = float3(128.0 / 255.0); }       // the 3D alone
    }
    float3 m = max(g - a, 0.0) * (255.0 / 128.0);
    float3 c = a;
    if (u.scene != 0)
    {
        uint2 q = uint2(p);
        if (q.x < scene.get_width() && q.y < scene.get_height())
            c += scene.read(q).rgb * m;
    }
    return float4(min(c, 1.0), 1.0);
}

// The depth clear inside a pass: the same triangle, at the far plane.
vertex CompOut vs_far(uint vid [[vertex_id]])
{
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    CompOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 1.0, 1.0);
    return o;
}

// The light scale: the pipeline's blend does dst * blendColour + this.
fragment float4 fs_scale(CompOut in [[stage_in]], constant float4& add [[buffer(0)]])
{
    return float4(add.rgb, 1.0);
}
)METAL";

namespace
{
    // ⚠️ THREE FRAMES IN FLIGHT, and what the CPU rewrites exists three times.
    // The old offscreen path waited for the GPU every frame (and paid for it);
    // the compositor never waits, so reusing the vertex arena or a layer
    // texture the next frame would rewrite memory the GPU is still reading --
    // geometry from two frames mixed, which looks like tearing.
    enum { kSlots = 3 };
    struct CompSlot
    {
        id<MTLBuffer>  m_Arena;
        size_t         m_Cap;
        id<MTLTexture> m_B, m_G;
    };
    CompSlot              g_Slots[kSlots];
    int                   g_nSlot     = -1;     // the slot this frame holds
    int                   g_nNextSlot = 0;
    dispatch_semaphore_t  g_FrameSem  = nil;
    id<MTLCommandBuffer>  g_FrameCmd  = nil;

    id<MTLRenderPipelineState> g_PipeComp  = nil;
    id<MTLRenderPipelineState> g_PipeScale = nil;
    id<MTLRenderPipelineState> g_PipeDepthClear = nil;
    id<MTLDepthStencilState>   g_DepthAlways = nil;
    id<MTLDepthStencilState>   g_DepthAlwaysWrite = nil;
    id<MTLSamplerState>        g_SampLayer = nil;
    id<MTLTexture>             g_Dummy   = nil;
    id<MTLTexture>             g_CompOff = nil;
    CAMetalLayer*              g_CompLayer = nil;

    // The device and the world pipeline, as LTMetal_BeginOffscreen brings them up.
    bool EnsureDevice()
    {
        if (g_bInit) return !g_bFail;
        @autoreleasepool
        {
            g_Device = MTLCreateSystemDefaultDevice();
            if (!g_Device) { Fail("no Metal device"); g_bInit = true; return false; }
            if (!BuildPipeline()) { g_bInit = true; return false; }
            g_Textures.clear();
            g_Textures.push_back(nil);
        }
        g_bInit = true;
        return true;
    }

    // LT_MSAA=1/2/4/8, default 4; a count the GPU cannot do falls back to the
    // nearest lower one it can, and the run says what it got.
    int SceneSamples()
    {
        if (g_nSceneSamples) return g_nSceneSamples;
        int want = 4;
        if (const char* p = getenv("LT_MSAA")) want = atoi(p);
        if (want != 1 && want != 2 && want != 4 && want != 8) want = 4;
        int n = want;
        while (n > 1 && !(g_Device && [g_Device supportsTextureSampleCount:(NSUInteger)n])) n /= 2;
        g_nSceneSamples = n;
        printf("[COMP] 3D anti-aliasing: %dx MSAA%s\n", n,
               getenv("LT_MSAA") ? " (LT_MSAA)" : "");
        fflush(stdout);
        return n;
    }

    // The four world pipelines again, at the scene's sample count -- the same
    // shader and blend rules as BuildPipeline, which must stay in step.
    bool BuildScenePipes()
    {
        const int n = SceneSamples();
        if (n <= 1) return true;
        NSError* err = nil;
        id<MTLLibrary> lib = [g_Device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSrc]
                                                    options:nil error:&err];
        if (!lib) { Fail(err ? [[err localizedDescription] UTF8String] : "scene shader failed"); return false; }
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat3; vd.attributes[0].offset = 0;                 vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = sizeof(float) * 3; vd.attributes[1].bufferIndex = 0;
        vd.attributes[2].format = MTLVertexFormatFloat3; vd.attributes[2].offset = sizeof(float) * 5; vd.attributes[2].bufferIndex = 0;
        vd.layouts[0].stride = sizeof(LTMetalVert);
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        [pd setVertexFunction:[lib newFunctionWithName:@"vs_world"]];
        [pd setFragmentFunction:[lib newFunctionWithName:@"fs_world"]];
        [pd setVertexDescriptor:vd];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        [pd setDepthAttachmentPixelFormat:MTLPixelFormatDepth32Float];
        [pd setRasterSampleCount:(NSUInteger)n];
        g_ScenePipes[0] = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        id<MTLFunction> vsSkinS = [lib newFunctionWithName:@"vs_skin"];
        auto sceneTwin = [&](int k) {
            if (!vsSkinS) return;
            MTLRenderPipelineDescriptor* sd = [pd copy];
            [sd setVertexFunction:vsSkinS];
            [sd setVertexDescriptor:nil];
            NSError* e2 = nil;
            g_ScenePipeSkin[k] = [g_Device newRenderPipelineStateWithDescriptor:sd error:&e2];
        };
        sceneTwin(0);
        [pd setAlphaToCoverageEnabled:YES];
        g_ScenePipeA2C = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        sceneTwin(4);
        [pd setAlphaToCoverageEnabled:NO];
        if (!g_ScenePipeA2C) { Fail(err ? [[err localizedDescription] UTF8String] : "alpha-to-coverage pipeline failed"); return false; }

        pd.colorAttachments[0].blendingEnabled             = YES;
        pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorZero;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        g_ScenePipes[1] = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        sceneTwin(1);
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        g_ScenePipes[2] = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        sceneTwin(2);
        pd.colorAttachments[0].sourceRGBBlendFactor      = MTLBlendFactorDestinationColor;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorZero;
        g_ScenePipes[3] = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        sceneTwin(3);
        for (int i = 0; i < 4; ++i)
            if (!g_ScenePipes[i]) { Fail(err ? [[err localizedDescription] UTF8String] : "scene pipeline failed"); return false; }
        return true;
    }

    bool BuildCompPipelines()
    {
        if (g_PipeComp) return true;
        if (!EnsureDevice()) return false;
        NSError* err = nil;
        id<MTLLibrary> lib = [g_Device newLibraryWithSource:[NSString stringWithUTF8String:kCompShaderSrc]
                                                    options:nil error:&err];
        if (!lib) { Fail(err ? [[err localizedDescription] UTF8String] : "compositor shader failed"); return false; }

        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        [pd setVertexFunction:[lib newFunctionWithName:@"vs_full"]];
        [pd setFragmentFunction:[lib newFunctionWithName:@"fs_comp"]];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_PipeComp = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_PipeComp) { Fail(err ? [[err localizedDescription] UTF8String] : "compositor pipeline failed"); return false; }

        // ⚠️ IN THE SCENE PASS, so it must declare the pass's depth format
        // even though it never tests: a pipeline whose attachments disagree
        // with the encoder's is a validation error, not a no-op.
        [pd setFragmentFunction:[lib newFunctionWithName:@"fs_scale"]];
        [pd setDepthAttachmentPixelFormat:MTLPixelFormatDepth32Float];
        [pd setRasterSampleCount:(NSUInteger)SceneSamples()];
        pd.colorAttachments[0].blendingEnabled             = YES;
        pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorBlendColor;
        pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorZero;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        g_PipeScale = [g_Device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_PipeScale) { Fail(err ? [[err localizedDescription] UTF8String] : "light-scale pipeline failed"); return false; }

        MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
        [dd setDepthCompareFunction:MTLCompareFunctionAlways];
        [dd setDepthWriteEnabled:NO];
        g_DepthAlways = [g_Device newDepthStencilStateWithDescriptor:dd];
        [dd setDepthWriteEnabled:YES];
        g_DepthAlwaysWrite = [g_Device newDepthStencilStateWithDescriptor:dd];

        // The in-pass depth clear: no fragment stage, no colour written.
        {
            MTLRenderPipelineDescriptor* cd = [[MTLRenderPipelineDescriptor alloc] init];
            [cd setVertexFunction:[lib newFunctionWithName:@"vs_far"]];
            cd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            cd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
            [cd setDepthAttachmentPixelFormat:MTLPixelFormatDepth32Float];
            [cd setRasterSampleCount:(NSUInteger)SceneSamples()];
            g_PipeDepthClear = [g_Device newRenderPipelineStateWithDescriptor:cd error:&err];
            if (!g_PipeDepthClear) { Fail(err ? [[err localizedDescription] UTF8String] : "depth-clear pipeline failed"); return false; }
        }
        if (!BuildScenePipes()) return false;

        {
            int nAniso = 16;
            if (const char* pA = getenv("LT_ANISO")) nAniso = atoi(pA);
            if (nAniso < 1) nAniso = 1;
            if (nAniso > 16) nAniso = 16;
            MTLSamplerDescriptor* ad = [[MTLSamplerDescriptor alloc] init];
            [ad setMinFilter:MTLSamplerMinMagFilterLinear];
            [ad setMagFilter:MTLSamplerMinMagFilterLinear];
            [ad setMipFilter:MTLSamplerMipFilterLinear];
            [ad setSAddressMode:MTLSamplerAddressModeRepeat];
            [ad setTAddressMode:MTLSamplerAddressModeRepeat];
            [ad setMaxAnisotropy:(NSUInteger)nAniso];
            g_SceneSampler = [g_Device newSamplerStateWithDescriptor:ad];
            [ad setMinFilter:MTLSamplerMinMagFilterNearest];
            [ad setMagFilter:MTLSamplerMinMagFilterNearest];
            g_SceneSamplerAT = [g_Device newSamplerStateWithDescriptor:ad];
            printf("[COMP] texture filtering: trilinear, %dx anisotropic%s\n", nAniso,
                   getenv("LT_ANISO") ? " (LT_ANISO)" : "");
            fflush(stdout);
        }

        // ⚠️ LINEAR, CLAMPED: the 2D is scaled up by a non-integer factor
        // (768 lines onto 1664), and nearest would make every glyph a
        // different width. Clamp, or the right edge filters in the left.
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        [sd setMinFilter:MTLSamplerMinMagFilterLinear];
        [sd setMagFilter:MTLSamplerMinMagFilterLinear];
        [sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
        [sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
        g_SampLayer = [g_Device newSamplerStateWithDescriptor:sd];

        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:1 height:1 mipmapped:NO];
        [td setUsage:MTLTextureUsageShaderRead];
        g_Dummy = [g_Device newTextureWithDescriptor:td];
        const unsigned char black[4] = { 0, 0, 0, 255 };
        [g_Dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:black bytesPerRow:4];
        return true;
    }

    // Written by the completion handlers, on Metal's thread.
    std::atomic<long long> g_nGpuUs(0), g_nGpuFrames(0), g_nGpuErrors(0);

    // Takes a frame slot (waits while three frames are in flight) and opens the
    // frame's command buffer, once per host frame.
    bool CompAcquire()
    {
        if (g_nSlot >= 0) return true;
        if (!g_FrameSem) g_FrameSem = dispatch_semaphore_create(kSlots);
        dispatch_semaphore_wait(g_FrameSem, DISPATCH_TIME_FOREVER);
        g_nSlot = g_nNextSlot;
        g_nNextSlot = (g_nNextSlot + 1) % kSlots;
        g_Arena = g_Slots[g_nSlot].m_Arena;
        g_ArenaCap = g_Slots[g_nSlot].m_Cap;
        g_ArenaUsed = 0;
        g_FrameCmd = [g_Queue commandBuffer];
        if (!g_FrameCmd) { dispatch_semaphore_signal(g_FrameSem); g_nSlot = -1; return false; }
        return true;
    }

    // Commits the frame and hands its slot back when the GPU is done with it.
    void CompCommit(bool bWait)
    {
        if (g_nSlot < 0) return;
        if (g_Enc) { [g_Enc endEncoding]; g_Enc = nil; }
        g_Slots[g_nSlot].m_Arena = g_Arena;
        g_Slots[g_nSlot].m_Cap   = g_ArenaCap;
        dispatch_semaphore_t sem = g_FrameSem;
        // ⚠️ AND SAYS WHEN THE GPU FAILED IT, which this handler used to
        // ignore: a command buffer that errors (a fault, a watchdog reset)
        // looked exactly like a slow GPU from the CPU side. Each error is
        // printed (the first 20); the GPU's own time feeds LT_PASSTIME.
        [g_FrameCmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            if ([cb status] == MTLCommandBufferStatusError)
            {
                const long long n = ++g_nGpuErrors;
                if (n <= 20)
                {
                    NSError* e = [cb error];
                    printf("[METAL] frame command buffer FAILED (#%lld): %s\n", n,
                           e ? [[e description] UTF8String] : "no error object");
                    fflush(stdout);
                }
            }
            const double t = ([cb GPUEndTime] - [cb GPUStartTime]) * 1000000.0;
            if (t > 0.0 && t < 60.0e6) { g_nGpuUs += (long long)t; ++g_nGpuFrames; }
            dispatch_semaphore_signal(sem);
        }];
        [g_FrameCmd commit];
        if (bWait) [g_FrameCmd waitUntilCompleted];
        g_FrameCmd = nil;
        g_Cmd = nil;
        g_nSlot = -1;
        // The arena belongs to the slot now; nothing outside a frame appends.
        g_Arena = nil; g_ArenaCap = 0; g_ArenaUsed = 0;
    }

    id<MTLTexture> LayerTex(id<MTLTexture> t, int w, int h)
    {
        if (t && (int)[t width] == w && (int)[t height] == h) return t;
        MTLTextureDescriptor* d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)w height:(NSUInteger)h
                                                           mipmapped:NO];
        [d setUsage:MTLTextureUsageShaderRead];
        [d setStorageMode:MTLStorageModeShared];
        return [g_Device newTextureWithDescriptor:d];
    }
}


bool LTMetal_CompAttach()
{
    if (!EnsureDevice()) return false;
    NSView* v = (__bridge NSView*)LTWindow_GetContentView();
    if (!v) { Fail("no content view for the compositor"); return false; }

    @autoreleasepool
    {
        g_CompLayer = [CAMetalLayer layer];
        [g_CompLayer setDevice:g_Device];
        [g_CompLayer setPixelFormat:MTLPixelFormatBGRA8Unorm];
        [g_CompLayer setFramebufferOnly:YES];
        // ⚠️ THE BACKING SCALE, or a Retina window gets a drawable of its
        // POINT size and the whole compositor renders at half resolution --
        // the very thing it exists to stop.
        CGFloat sc = [[v window] backingScaleFactor];
        if (sc <= 0) sc = 1;
        [g_CompLayer setContentsScale:sc];
        [g_CompLayer setFrame:[v bounds]];
        [g_CompLayer setAutoresizingMask:(kCALayerWidthSizable | kCALayerHeightSizable)];
        [g_CompLayer setMaximumDrawableCount:3];
        NSRect b = [v convertRectToBacking:[v bounds]];
        [g_CompLayer setDrawableSize:CGSizeMake(b.size.width, fabs(b.size.height))];
        // ⚠️ LAYER FIRST, THEN wantsLayer: the order that makes a layer-HOSTING
        // view, whose drawRect: is never called again -- the CoreGraphics
        // present must not paint over the GPU's frame.
        [v setLayer:g_CompLayer];
        [v setWantsLayer:YES];
    }
    return true;
}


bool LTMetal_CompOutputSize(int* pW, int* pH)
{
    if (!g_CompLayer) return false;
    NSView* v = (__bridge NSView*)LTWindow_GetContentView();
    if (!v) return false;
    NSRect b = [v convertRectToBacking:[v bounds]];
    const int w = (int)lround(b.size.width), h = (int)lround(fabs(b.size.height));
    if (w <= 0 || h <= 0) return false;
    *pW = w; *pH = h;
    return true;
}


bool LTMetal_BeginScene(int nOutW, int nOutH, int vx, int vy, int vw, int vh, bool bClear)
{
    if (nOutW <= 0 || nOutH <= 0 || vw <= 0 || vh <= 0) return false;
    if (!EnsureDevice() || !BuildCompPipelines() || !CompAcquire()) return false;

    @autoreleasepool
    {
        if (!g_SceneTex || g_SceneW != nOutW || g_SceneH != nOutH)
        {
            MTLTextureDescriptor* d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:(NSUInteger)nOutW
                                                                  height:(NSUInteger)nOutH
                                                               mipmapped:NO];
            [d setUsage:(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)];
            [d setStorageMode:MTLStorageModePrivate];
            g_SceneTex = [g_Device newTextureWithDescriptor:d];
            MTLTextureDescriptor* dd =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                   width:(NSUInteger)nOutW
                                                                  height:(NSUInteger)nOutH
                                                               mipmapped:NO];
            [dd setUsage:MTLTextureUsageRenderTarget];
            [dd setStorageMode:MTLStorageModePrivate];
            g_SceneDepth = [g_Device newTextureWithDescriptor:dd];
            if (!g_SceneTex || !g_SceneDepth) { Fail("compositor scene target failed"); return false; }
            g_SceneMSAA = nil;
            const int ns = SceneSamples();
            if (ns > 1)
            {
                MTLTextureDescriptor* md =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                       width:(NSUInteger)nOutW
                                                                      height:(NSUInteger)nOutH
                                                                   mipmapped:NO];
                [md setTextureType:MTLTextureType2DMultisample];
                [md setSampleCount:(NSUInteger)ns];
                [md setUsage:MTLTextureUsageRenderTarget];
                [md setStorageMode:MTLStorageModePrivate];
                g_SceneMSAA = [g_Device newTextureWithDescriptor:md];
                // The depth must have the same sample count as the colour.
                [dd setTextureType:MTLTextureType2DMultisample];
                [dd setSampleCount:(NSUInteger)ns];
                g_SceneDepth = [g_Device newTextureWithDescriptor:dd];
                if (!g_SceneMSAA || !g_SceneDepth) { Fail("compositor MSAA target failed"); return false; }
            }
            g_SceneW = nOutW; g_SceneH = nOutH;
            bClear = true;          // a new texture holds nothing defined
        }

        if (vx < 0) { vw += vx; vx = 0; }
        if (vy < 0) { vh += vy; vy = 0; }
        if (vx + vw > nOutW) vw = nOutW - vx;
        if (vy + vh > nOutH) vh = nOutH - vy;
        if (vw <= 0 || vh <= 0) return false;

        g_nTris = 0;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        // ⚠️ STORE *AND* RESOLVE: a second camera in the same frame loads the
        // samples the first one left, and only a stored MSAA target has them.
        if (g_SceneMSAA)
        {
            rp.colorAttachments[0].texture = g_SceneMSAA;
            rp.colorAttachments[0].resolveTexture = g_SceneTex;
            rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        }
        else
        {
            rp.colorAttachments[0].texture = g_SceneTex;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        }
        rp.colorAttachments[0].loadAction = bClear ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.055, 0.067, 0.086, 1.0);
        rp.depthAttachment.texture = g_SceneDepth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
        rp.depthAttachment.clearDepth = 1.0;

        g_Cmd = g_FrameCmd;
        g_ColourTarget = g_SceneTex;
        g_Enc = [g_Cmd renderCommandEncoderWithDescriptor:rp];
        [g_Enc setRenderPipelineState:(g_ScenePipes[0] ? g_ScenePipes[0] : g_Pipe)];
        [g_Enc setDepthStencilState:g_Depth];
        [g_Enc setFragmentSamplerState:(g_SceneSampler ? g_SceneSampler : g_Sampler) atIndex:0];
        g_nEncMode = 0;

        g_SceneViewport.originX = vx; g_SceneViewport.originY = vy;
        g_SceneViewport.width = vw;   g_SceneViewport.height = vh;
        g_SceneViewport.znear = 0.0;  g_SceneViewport.zfar = 1.0;
        g_SceneScissor.x = (NSUInteger)vx; g_SceneScissor.y = (NSUInteger)vy;
        g_SceneScissor.width = (NSUInteger)vw; g_SceneScissor.height = (NSUInteger)vh;
        g_bSceneVP = true;
        [g_Enc setViewport:g_SceneViewport];
        [g_Enc setScissorRect:g_SceneScissor];
    }
    return true;
}


void LTMetal_SceneLightScale(const float* pScale3, const float* pAdd3)
{
    if (!g_Enc || !g_bSceneVP || !pScale3 || !pAdd3) return;
    const bool bScale = fabsf(pScale3[0]-1.0f) > 1e-3f || fabsf(pScale3[1]-1.0f) > 1e-3f || fabsf(pScale3[2]-1.0f) > 1e-3f;
    const bool bAdd   = pAdd3[0] > 1e-3f || pAdd3[1] > 1e-3f || pAdd3[2] > 1e-3f;
    if (!bScale && !bAdd) return;
    if (!BuildCompPipelines()) return;

    [g_Enc setRenderPipelineState:g_PipeScale];
    [g_Enc setDepthStencilState:g_DepthAlways];
    [g_Enc setBlendColorRed:pScale3[0] green:pScale3[1] blue:pScale3[2] alpha:1.0f];
    float add[4] = { pAdd3[0], pAdd3[1], pAdd3[2], 0.0f };
    [g_Enc setFragmentBytes:add length:sizeof(add) atIndex:0];
    [g_Enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    g_nEncMode = -1;        // the next world draw rebinds its own pipeline
}


void LTMetal_EndScene()
{
    if (g_Enc) [g_Enc endEncoding];
    g_Enc = nil;
    g_Cmd = nil;
    g_ColourTarget = nil;
    g_bSceneVP = false;
}


bool LTMetal_Composite(const unsigned char* pB, const unsigned char* pG,
                       int nLayerW, int nLayerH, const LTCompMap& map,
                       int nOutW, int nOutH, bool bPresent, unsigned char* pRead)
{
    if (!pB || !pG || nLayerW <= 0 || nLayerH <= 0 || nOutW <= 0 || nOutH <= 0) return false;
    if (!BuildCompPipelines() || !CompAcquire()) return false;

    @autoreleasepool
    {
        CompSlot& sl = g_Slots[g_nSlot];
        sl.m_B = LayerTex(sl.m_B, nLayerW, nLayerH);
        sl.m_G = LayerTex(sl.m_G, nLayerW, nLayerH);
        if (!sl.m_B || !sl.m_G) { CompCommit(false); return false; }
        const MTLRegion r = MTLRegionMake2D(0, 0, (NSUInteger)nLayerW, (NSUInteger)nLayerH);
        [sl.m_B replaceRegion:r mipmapLevel:0 withBytes:pB bytesPerRow:(NSUInteger)nLayerW * 4];
        [sl.m_G replaceRegion:r mipmapLevel:0 withBytes:pG bytesPerRow:(NSUInteger)nLayerW * 4];

        id<CAMetalDrawable> drawable = nil;
        id<MTLTexture> target = nil;
        if (bPresent && g_CompLayer)
        {
            // ⚠️ FOLLOWS THE WINDOW every frame: a resize or the switch to
            // fullscreen changes the backing size, and a drawable that keeps
            // the old one is stretched by the window server.
            const CGSize want = CGSizeMake(nOutW, nOutH);
            if (!CGSizeEqualToSize([g_CompLayer drawableSize], want)) [g_CompLayer setDrawableSize:want];
            NSView* v = (__bridge NSView*)LTWindow_GetContentView();
            if (v)
            {
                const CGFloat sc = [[v window] backingScaleFactor];
                if (sc > 0 && [g_CompLayer contentsScale] != sc) [g_CompLayer setContentsScale:sc];
            }
            drawable = [g_CompLayer nextDrawable];
            if (!drawable) { CompCommit(false); return false; }
            target = [drawable texture];
        }
        else
        {
            if (!g_CompOff || (int)[g_CompOff width] != nOutW || (int)[g_CompOff height] != nOutH)
            {
                MTLTextureDescriptor* d =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                       width:(NSUInteger)nOutW
                                                                      height:(NSUInteger)nOutH
                                                                   mipmapped:NO];
                [d setUsage:(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)];
                [d setStorageMode:MTLStorageModeShared];
                g_CompOff = [g_Device newTextureWithDescriptor:d];
                if (!g_CompOff) { CompCommit(false); return false; }
            }
            target = g_CompOff;
        }

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLRenderCommandEncoder> enc = [g_FrameCmd renderCommandEncoderWithDescriptor:rp];

        struct CompU { float dst[4]; float src[4]; float layer[4]; int scene; int pad[3]; } u;
        static_assert(sizeof(CompU) == 64, "CompU must match the MSL struct (64 bytes)");
        u.dst[0] = (float)map.m_DstX; u.dst[1] = (float)map.m_DstY;
        u.dst[2] = (float)map.m_DstW; u.dst[3] = (float)map.m_DstH;
        u.src[0] = map.m_SrcX; u.src[1] = map.m_SrcY; u.src[2] = map.m_SrcW; u.src[3] = map.m_SrcH;
        u.layer[0] = (float)nLayerW; u.layer[1] = (float)nLayerH; u.layer[2] = u.layer[3] = 0.0f;
        u.scene = (map.m_bScene && g_SceneTex) ? 1 : ((map.m_bMenuScene && g_SceneTex) ? 2 : 0);
        u.pad[0] = u.pad[1] = u.pad[2] = 0;

        [enc setRenderPipelineState:g_PipeComp];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [enc setFragmentTexture:(u.scene ? g_SceneTex : g_Dummy) atIndex:0];
        [enc setFragmentTexture:sl.m_B atIndex:1];
        [enc setFragmentTexture:sl.m_G atIndex:2];
        [enc setFragmentSamplerState:g_SampLayer atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];

        if (drawable) [g_FrameCmd presentDrawable:drawable];
        CompCommit(pRead != 0);

        if (pRead && target == g_CompOff)
        {
            std::vector<unsigned char> bgra((size_t)nOutW * nOutH * 4);
            [g_CompOff getBytes:&bgra[0] bytesPerRow:(NSUInteger)nOutW * 4
                     fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)nOutW, (NSUInteger)nOutH)
                    mipmapLevel:0];
            for (size_t i = 0; i < bgra.size(); i += 4)
            {
                pRead[i+0] = bgra[i+2]; pRead[i+1] = bgra[i+1];
                pRead[i+2] = bgra[i+0]; pRead[i+3] = 255;
            }
        }
    }
    return true;
}


bool LTMetal_TakeGpuTime(double* pMs, int* pnFrames, int* pnErrors)
{
    const long long us = g_nGpuUs.exchange(0), n = g_nGpuFrames.exchange(0);
    if (pMs) *pMs = (double)us / 1000.0;
    if (pnFrames) *pnFrames = (int)n;
    if (pnErrors) *pnErrors = (int)g_nGpuErrors.load();
    return n > 0;
}


void LTMetal_CompFrameEnd()
{
    if (g_nSlot >= 0) CompCommit(false);
}


void LTMetal_CompShutdown()
{
    if (g_nSlot >= 0) CompCommit(false);
    // ⚠️ DRAIN, THEN GIVE THE COUNT BACK. A dispatch semaphore released while
    // its value is below the one it was made with is a libdispatch crash, and
    // a completion handler firing after the globals are gone is another.
    if (g_FrameSem)
    {
        for (int i = 0; i < kSlots; ++i) dispatch_semaphore_wait(g_FrameSem, DISPATCH_TIME_FOREVER);
        for (int i = 0; i < kSlots; ++i) dispatch_semaphore_signal(g_FrameSem);
    }
    @autoreleasepool
    {
        if (g_CompLayer)
        {
            NSView* v = (__bridge NSView*)LTWindow_GetContentView();
            if (v && [v layer] == g_CompLayer) { [v setLayer:nil]; [v setWantsLayer:NO]; }
            g_CompLayer = nil;
        }
        for (int i = 0; i < kSlots; ++i) { g_Slots[i].m_Arena = nil; g_Slots[i].m_B = nil; g_Slots[i].m_G = nil; g_Slots[i].m_Cap = 0; }
        g_SceneTex = nil; g_SceneDepth = nil; g_SceneW = g_SceneH = 0;
        g_CompOff = nil;
    }
}


// See LTMetal_ClearDepth: the depth clear as a draw inside the scene pass.
namespace
{
    void SceneClearDepthQuad()
    {
        if (!g_Enc || !g_PipeDepthClear) return;
        [g_Enc setRenderPipelineState:g_PipeDepthClear];
        [g_Enc setDepthStencilState:g_DepthAlwaysWrite];
        [g_Enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [g_Enc setDepthStencilState:g_Depth];
        g_nEncMode = -1;        // the next world draw rebinds its pipeline
    }
}
