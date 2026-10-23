// ----------------------------------------------------------------------- //
//
// MODULE  : lt3d.h
//
// PURPOSE : The 3D rasteriser -- textured, depth-buffered triangles.
//
//           This is the second part of the engine that actually draws. lt2d
//           composites surfaces; this puts geometry on one.
//
// ⚠️ WHY THIS IS SOFTWARE AND NOT METAL, WHICH IS A CONTRACT DECISION
//
//   `PORT_PLAN.md` §4.7 says "Metal belongs with the 3D renderer", and that is
//   still true -- but not FIRST. The frame contract is this port's main safety
//   net: a known-good frame is archived and every later run is diffed against
//   it at **0.000 mean absolute difference** (`harness/framediff.py`). That
//   only works if rendering is bit-for-bit reproducible.
//
//   A GPU is not. Rasterisation fill rules, interpolation precision and
//   texture-sample rounding all vary by driver and device, so a Metal frame
//   cannot be gated at 0.000 against anything but itself on one machine. Making
//   Metal the reference path would mean giving up the pixel contract exactly
//   when the renderer -- the most defect-prone subsystem in any port -- starts
//   growing.
//
//   So: this rasteriser is the REFERENCE implementation and the contract path.
//   Metal comes later as a FAST path, validated against these frames rather
//   than replacing them. At 640x480 the software path is also simply fast
//   enough to be useful, which is the same reasoning that made the window
//   CoreGraphics rather than Metal.
//
// ⚠️ DELIBERATELY FREE OF ENGINE DEPENDENCIES, for the reason lt2d.h gives
//
//   Nothing here includes lithtech.h, the model loader, or the texture loader.
//   It operates on plain floats, indices and {pixels, width, height}, so every
//   case is unit-testable on synthetic buffers with exact expected values. The
//   ABC and DTX loaders are wired to it from OUTSIDE, not from within.
//
// CONVENTIONS, stated because getting them implicit is how a renderer rots
//
//   * Matrices are **row-major**, `m[row*4 + col]`, and transform **column
//     vectors**: `v' = M * v`. Compose parent-then-child as `M = Parent *
//     Child`.
//   * Clip space is OpenGL-style: visible when `-w <= x,y,z <= w`.
//   * Screen space is top-down to match lt2d and the decoded textures, so the
//     projection flips Y once, here, rather than every caller remembering to.
//   * Depth is stored as **1/w**, so nearer means LARGER. That is not a
//     preference: interpolating 1/w is linear in screen space and interpolating
//     z is not, so this is the value that is correct to lerp anyway.
//   * Texture coordinates are interpolated **perspective-correctly** (u/w, v/w
//     alongside 1/w). Affine mapping is visibly wrong on the large floor and
//     wall polygons a NOLF level is made of.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_3D_H__
#define __LT_3D_H__

// A render target: colour, depth, and dimensions. Does not own the memory.
//
// ⚠️ m_pDepth may be null, which disables depth testing AND depth writes. That
// is a real mode, not a degenerate one -- it is what a background or a fully
// sorted draw wants -- so it is stated rather than left to a null check.
struct LT3DTarget
{
    unsigned char*  m_pRGBA;        // w*h*4, top-down, same as lt2d
    float*          m_pDepth;       // w*h 1/w values, or null
    int             m_Width;
    int             m_Height;

    // ---- affine vertex colour ------------------------------------------ //
    //
    // ⚠️ Interpolates the per-vertex COLOUR in screen space instead of
    // perspective-correctly. Set it for world geometry; leave it false -- the
    // default -- for models.
    //
    // WHY IT EXISTS: a corridor floor seen edge-on came out as a harsh white
    // stipple. It was not the texture, the mip choice, the depth test, the
    // surface flags or the lighting scale -- all were eliminated by rendering
    // with each disabled in turn. Substituting a CONSTANT vertex colour, with
    // the texture still applied, produced a perfectly clean corridor. So the
    // fault was the colour term alone.
    //
    // ⚠️ And it was not the DATA. Where two polygons share a point, their
    // colours for it disagree by a mean of 0.1 out of 255 across 12,800
    // comparisons -- the baked lighting is smooth. What is fragile is
    // recovering it: colour is carried as c/w and multiplied back by w, and on
    // a triangle stretching from a few units away to the far end of a corridor
    // that product loses its low bits. Texture coordinates survive it because
    // they span thousands of texels; colour spans 0..1, so the same absolute
    // error is the whole signal.
    //
    // Gouraud shading does not need perspective correction -- affine is what
    // software rasterisers of this era did -- so the fix is to stop dividing.
    //
    // ⚠️ Opt-in because the frame contract's reference frames were rendered
    // the old way, and models look identical either way at menu distances.
    bool            m_bAffineColour;

    // ---- bilinear texture filtering ------------------------------------- //
    //
    // ⚠️ Four texels and a weighted blend instead of one nearest texel. Set it
    // for world geometry; leave it false -- the default -- for the 2D and menu
    // paths, whose art is authored at exactly one texel per pixel and only
    // gets blurrier for being filtered.
    //
    // WHY IT EXISTS: the plaster walls of M01S01 came out visibly speckled.
    // That was not the lighting and not the mip choice -- it is what nearest
    // sampling does when a wall is magnified, and the mip chain cannot help,
    // because mips fix MINIFICATION and this is the other end.
    //
    // ⚠️ IT IS SUPPRESSED FOR ALPHA-TESTED TEXTURES, deliberately. Blending
    // across a punch-through edge mixes in the RGB of texels that are meant to
    // be invisible, and in a DTX those texels are not a helpful colour -- they
    // are whatever the artist left under the cut-out. Filtering them produces
    // a dark or coloured fringe around every leaf and grating, which is a
    // worse artefact than the blockiness it removes. Nearest sampling keeps
    // the silhouette EXACTLY as the alpha test already decided it.
    //
    // ⚠️ Opt-in for the same reason as m_bAffineColour: the frame contract's
    // reference frames were rendered with nearest sampling, and every textured
    // pixel would change.
    bool            m_bBilinear;

    // ---- anisotropic filtering ------------------------------------------ //
    //
    // ⚠️ Maximum samples along the LONG axis of the pixel's texture footprint.
    // 1 -- the default -- is plain isotropic mip selection.
    //
    // WHY IT EXISTS: a pixel's footprint in texture space is an ELLIPSE, and on
    // a wall seen edge-on it is a very long thin one -- the texture races past
    // along the wall while barely moving across it. Choosing one mip for that
    // footprint means choosing for its LONG axis, which then blurs the short
    // axis by the same factor. Corridor walls came out smeared, and it is worth
    // being clear that this is NOT the bilinear filter and NOT a mistake in the
    // LOD arithmetic: a single square sample cannot cover a long thin area.
    //
    // The fix is to take several samples ALONG the long axis and pick the level
    // for the SHORT one. Cost is up to this many texture samples per pixel, so
    // it is bounded rather than exact -- 4 recovers most of the sharpness a
    // corridor loses.
    //
    // ⚠️ Only meaningful with a mip chain; without one there is no level to
    // choose and this is ignored. Opt-in for the same reason as the two flags
    // above -- and at 1 the sampler evaluates the identical expression it
    // always did, not merely an equivalent one.
    int             m_nMaxAniso;

    // ---- trilinear filtering -------------------------------------------- //
    //
    // ⚠️ Blend the TWO mip levels either side of the computed LOD instead of
    // snapping to the nearest one. Off by default.
    //
    // WHY IT EXISTS: nearest-level selection puts a hard seam wherever the
    // chosen level changes -- across a floor receding from the camera those
    // seams are visible bands, and they SWIM as you walk, because the boundary
    // is pinned to distance rather than to the surface. The existing comment in
    // the sampler called that trade deliberate and said "until it has a reason
    // otherwise"; walking around a corridor is the reason.
    //
    // Cost is a second texture sample per tap, so it multiplies with
    // m_nMaxAniso rather than adding to it.
    //
    // ⚠️ Only meaningful with a mip chain, and only between levels: where the
    // LOD is already at 0 or at the last level there is nothing to blend with
    // and the sampler takes the single-level path.
    bool            m_bTrilinear;

    // ---- the PROBE buffers -- an INSTRUMENT, not a rendering feature ---- //
    //
    // ⚠️ WHY THEY EXIST: "which triangle painted this pixel, and what did it
    // fetch from the texture to do it?" A flat patch on a lit, textured model
    // has two ingredients -- the per-vertex light and the sampled texel -- and
    // nothing in a finished PPM can tell them apart, because the pixel is
    // their PRODUCT. LT_LIGHTONLY answers half of that question by removing
    // the texture; these answer the other half without removing anything.
    //
    // Both are null by default and the rasteriser touches neither then, so the
    // contract path is byte-for-byte unchanged. When non-null they are written
    // at exactly the moment a pixel wins the depth test:
    //
    //   m_pProbeID   w*h        LT3D::g_nProbeID, whatever the caller last set
    //   m_pProbeTex  w*h*4      the SAMPLED TEXEL, before the light multiply
    //   m_pProbeUV   w*h*2      the interpolated u,v the sampler was handed
    //   m_pProbeMip  w*h        the MIP LEVEL the sampler chose, 255 = no chain
    //
    // ⚠️ m_pProbeUV is the one that separates "the triangle carries the wrong
    // coordinates" from "the coordinates were interpolated wrongly": the UVs
    // in the file are per face-corner and never change with a pose, so a patch
    // that appears only when posed cannot be the data.
    //
    // ⚠️ Written only on the opaque path. A blended fragment does not own its
    // pixel -- several of them can contribute to one -- so stamping an id
    // there would name a winner that does not exist.
    unsigned*       m_pProbeID;
    unsigned char*  m_pProbeTex;
    float*          m_pProbeUV;
    unsigned char*  m_pProbeMip;

    // ---- linear depth fog (HANDOVER 5bo) --------------------------------- //
    //
    // The donor's D3D table fog, per pixel: depth z = 1/w (what the depth
    // buffer holds), f = clamp((z - near) / (far - near)), colour -> lerp
    // toward the fog colour by f. ADDITIVE fragments fog toward BLACK (the
    // donor sets D3DRS_FOGCOLOR 0 for them, d3d_rendershader_gouraud.cpp) so a
    // glow fades out instead of greying; MULTIPLY fragments toward WHITE, the
    // colour that changes nothing. Off by default: the frame contract's
    // reference frames were rendered without it.
    bool            m_bFog;
    float           m_fFogNear, m_fFogFar;
    float           m_FogRGB[3];        // 0..255

    // ⚠️ RANGE, NOT DEPTH (HANDOVER 5ch). LT2's own renderer fogs by the
    // straight-line DISTANCE from the eye -- d3d.ren's vertex-fog routine
    // (0x100234e0) takes sqrt(dx^2+dy^2+dz^2), and vertex fog is its default
    // (`TableFog` 0). Per pixel here: d = depth * sqrt(1 + tx^2 + ty^2), with
    // tx = (x + 0.5 - W/2) * KX and ty likewise -- the view ray's tangents.
    // KX = KY = 0 is the old depth fog (the donor's table fog).
    float           m_fFogKX, m_fFogKY;

    // ---- the far clip plane: FarZ (HANDOVER 5bp) ------------------------- //
    //
    // The donor builds its frustum with NEARZ..g_CV_FarZ (common_draw.cpp:466)
    // and D3D drops everything past the far plane. This rasteriser clips the
    // near plane only, so the far one is a per-pixel test on the same eye
    // distance the depth buffer holds: a pixel whose w exceeds m_fFarW is not
    // drawn (no colour, no depth). For this projection w IS the view depth, so
    // the test lands exactly where D3D's z/w > 1 clip does. 0 = no far clip
    // (the default: menus, the sky box, the view weapon).
    float           m_fFarW;

    // ⚠️ A CONSTRUCTOR, for the reason LT3DTexture has one: every existing
    // caller assigns the four fields above and nothing else, and an
    // uninitialised flag here would change how the game's own frames shade.
    LT3DTarget()
        : m_pRGBA(0), m_pDepth(0), m_Width(0), m_Height(0),
          m_bAffineColour(false), m_bBilinear(false), m_nMaxAniso(1),
          m_bTrilinear(false), m_pProbeID(0), m_pProbeTex(0), m_pProbeUV(0),
          m_pProbeMip(0), m_bFog(false), m_fFogNear(0.0f), m_fFogFar(1.0f),
          m_fFogKX(0.0f), m_fFogKY(0.0f), m_fFarW(0.0f)
    { m_FogRGB[0] = m_FogRGB[1] = m_FogRGB[2] = 0.0f; }
};

// A vertex after the model/view/projection transform, still in CLIP space.
// The rasteriser does the perspective divide itself, because it has to clip
// against the near plane first and a divided vertex cannot be clipped.
struct LT3DVert
{
    float m_Clip[4];        // x, y, z, w
    float m_U, m_V;         // texture coordinates, 0..1
    float m_R, m_G, m_B;    // per-vertex modulation, 0..1
};

// A texture to sample. Nearest-neighbour, wrapping.
//
// ⚠️ Wrapping is by masking, so dimensions must be powers of two -- which
// every shipped DTX is (measured: 4160 of 4160, largest 512x512).
struct LT3DTexture
{
    const unsigned char*    m_pRGBA;    // w*h*4
    int                     m_Width;
    int                     m_Height;

    // ⚠️ WHETHER THE ALPHA CHANNEL MEANS ANYTHING, AND IT USUALLY DOES NOT.
    //
    //   Measured across the shipped DTX set: **816 of 1502 BPP_32 textures
    //   have alpha 0 in EVERY texel**, and another 317 are mostly below 128.
    //   Alpha in an uncompressed DTX is not an opacity channel -- LithTech
    //   decides translucency per SURFACE, from object and render-style flags,
    //   not from the texture's bytes.
    //
    //   So alpha-testing every texture makes more than half of all
    //   uncompressed-textured geometry vanish. That is not a subtle error, but
    //   it looks like a broken transform rather than a broken blend, and it
    //   cost exactly one confusing render here (a pair of sunglasses drawing
    //   92 triangles and 0 pixels) before it was measured.
    //
    //   The compressed formats are different and this defaults accordingly:
    //   DXT1's 3-colour block encodes genuine punch-through, and DXT3/DXT5 are
    //   *chosen* by the artist precisely when alpha is wanted.
    //   CLTDtx::AlphaIsMeaningful() is the mapping.
    bool                    m_bAlphaTest;

    // ⚠️ THE THRESHOLD, AND IT IS AUTHORED PER TEXTURE -- NOT 0.5.
    //
    //   A DTX states its own alpha reference in its command string
    //   (`alpharef 96` on the wall lantern). 231 shipped textures carry one and
    //   EVERY ONE is below 128 -- min 10, max 96, mean 94.8 -- so the
    //   hard-coded half this used to be punched out every texel the artist
    //   meant to keep, deleting the lantern's whole cage and leaving it looking
    //   like a model drawn in pieces.
    //
    //   0..1, and 0.5 remains the default for the ~3900 textures that state
    //   nothing. ⚠️ Set it from CLTDtx::GetAlphaRef() / 255.0f wherever a
    //   texture is resolved; a caller that forgets gets the old behaviour,
    //   which is why the two world/menu paths are both wired rather than one.
    float                   m_fAlphaRef;

    // ⚠️ EVERY TEXEL IS BELOW m_fAlphaRef -- the texture shows nothing at all.
    //
    //   Alpha-testing such a texture erases whatever it is on, so the guard in
    //   ltworldrender refuses to test it and it draws OPAQUE instead. For a
    //   prop that is right (816 of 1502 BPP_32 textures carry meaningless
    //   all-zero alpha and are solid objects). For a world surface the LEVEL
    //   says which it is, via SURF_TRANSPARENT -- and a surface that is
    //   flagged transparent AND carries a wholly transparent texture is meant
    //   to show nothing. `tex\invisible.dtx` is exactly that, on 88 surfaces.
    //
    //   ⚠️ Drawing it opaque puts a solid panel where the level intends a
    //   hole, which is far more visible than the reverse.
    bool                    m_bFullyTransparent;

    // ⚠️ BLEND INSTEAD OF PUNCH -- `dst = src*a + dst*(1-a)`, and NO DEPTH
    // WRITE.
    //
    //   Alpha testing answers a yes/no question, and glass is not a yes/no
    //   surface: `tex\glass\gl01\gl0006` is 76.2% transparent across 1056
    //   surfaces, so punching it removes three quarters of every pane and
    //   leaves a speckled residue, while drawing it opaque puts a solid panel
    //   in a window frame. Neither is a window.
    //
    // ⚠️ NOT WRITING DEPTH IS THE WHOLE CORRECTNESS ARGUMENT. A blended
    // fragment does not hide what is behind it, so recording it as the nearest
    // surface would make the geometry BEHIND the glass fail its depth test and
    // vanish -- a window that erases the room through it. It still TESTS
    // depth, so glass behind a wall is correctly hidden.
    //
    // ⚠️ Because it writes no depth, a blended surface must be drawn AFTER all
    // opaque geometry and back-to-front among itself. The order is the
    // caller's job; this flag only says how the pixel is combined.
    bool                    m_bBlend;

    // ⚠️ A CONSTANT MULTIPLIER ON THE BLEND WEIGHT, AND FOR MODELS IT IS THE
    // WHOLE SIGNAL.
    //
    //   A world brush gets its translucency from the texture; a MODEL gets it
    //   from the object -- `SetObjectColor(r, g, b, alpha)` in Prop.cpp, whose
    //   alpha comes from `Alpha` in Attributes\PropTypes.txt. 98 spotlight-beam
    //   props in the shipped worlds are authored at 0.25 and 0.50, and their
    //   skin's alpha channel is 100% ZERO -- so a blend that read only the
    //   texture would draw nothing at all where the level wants a light beam.
    //
    //   1.0 (the default) leaves world brushes exactly as they were.
    float                   m_fConstAlpha;

    // ⚠️ WHETHER THE TEXEL'S OWN ALPHA JOINS THE BLEND.
    //
    //   False for the beams above and for `Tree_03t` (100% opaque alpha): the
    //   channel carries no information and the object's constant is the only
    //   statement there is. True everywhere the alpha actually varies, which
    //   is what makes a wine glass a wine glass.
    bool                    m_bTexAlpha;

    // ⚠️ HOW THE SOURCE IS COMBINED -- and glass is only one of three answers.
    //
    //   0  source-alpha over   `dst = src*a + dst*(1-a)`   window glass
    //   1  ADDITIVE            `dst = dst + src*a`         light beams
    //   2  MULTIPLY            `dst = dst * src`           the motorcycle
    //
    //   NOLF states this per OBJECT, not per texture: `Additive` and
    //   `Multiply` beside `Alpha` in Attributes\PropTypes.txt, which
    //   PropType.cpp turns into FLAG2_ADDITIVE / FLAG2_MULTIPLY. 102 shipped
    //   light-beam props are additive, and their skin is nearly BLACK -- read
    //   as ordinary translucency they darken the room instead of lighting it,
    //   which is exactly what this port drew before the mode existed.
    //
    //   ⚠️ Mode 2 ignores alpha, as D3D's ZERO/SRCCOLOR does.
    int                     m_nBlendOp;

    // ---- optional mip chain -------------------------------------------- //
    //
    // ⚠️ OPT-IN BY CONSTRUCTION, AND THAT IS THE POINT. With m_nMips 0 or 1 the
    // sampler takes exactly the path it always took -- mip 0, no LOD computed,
    // not one extra arithmetic operation. The frame contract's two reference
    // frames were produced before mips existed, and a sampler that changed its
    // answer for every textured pixel would break them for no reason: models
    // ship one decoded level and do not need this.
    //
    // Only a caller that FILLS the chain gets selection. That is the whole
    // compatibility argument, and it is structural rather than a promise.
    enum { kMaxMips = 14 };
    const unsigned char*    m_pMipRGBA[kMaxMips];
    int                     m_MipW[kMaxMips];
    int                     m_MipH[kMaxMips];
    int                     m_nMips;

    // ⚠️ A CONSTRUCTOR, not an aggregate. Every existing caller builds one of
    // these by assigning the four fields above and nothing else; leaving
    // m_nMips as stack garbage would make the sampler index a mip array full
    // of junk pointers. This engine has already paid for that family of bug
    // five times over -- see HANDOVER.md on uninitialised out-parameters.
    LT3DTexture()
        : m_pRGBA(0), m_Width(0), m_Height(0), m_bAlphaTest(false),
          m_fAlphaRef(0.5f), m_bFullyTransparent(false), m_bBlend(false),
          m_fConstAlpha(1.0f), m_bTexAlpha(true), m_nBlendOp(0),
          m_nMips(0)
    {
        for (int i = 0; i < kMaxMips; ++i)
        {
            m_pMipRGBA[i] = 0;
            m_MipW[i] = m_MipH[i] = 0;
        }
    }
};

namespace LT3D
{
    // ⚠️ WHAT THE PROBE STAMPS. The caller sets this before each DrawTriangle
    // and the rasteriser copies it into LT3DTarget::m_pProbeID at every pixel
    // that wins. A global rather than an argument because DrawTriangle's
    // signature is on the contract path and every caller in the tree passes a
    // const target; an id nobody reads costs one integer store per triangle.
    extern unsigned g_nProbeID;

    // ⚠️ WHEN TRUE, LTModelDraw LEAVES g_nProbeID ALONE. A game frame holds
    // dozens of models and the probe's id has only room for piece and face,
    // so the caller that wants ONE model's pixels holds the id at 0 for
    // everything else -- which also stamps 0 wherever something else wins the
    // pixel, so occlusion is recorded correctly.
    extern bool     g_bProbeHold;

    // ---- matrices ---------------------------------------------------- //

    void    Identity(float* m);
    void    Multiply(const float* a, const float* b, float* out);   // out = a * b
    void    TransformPoint(const float* m, const float* p3, float* out4);

    // Right-handed look-at. up need not be perpendicular to the view vector.
    void    LookAt(const float* eye3, const float* at3, const float* up3, float* out);

    // fovY in RADIANS. Produces the Y flip described above.
    void    Perspective(float fFovY, float fAspect, float fNear, float fFar, float* out);

    // A rotation built from a quaternion (x, y, z, w) and a translation --
    // which is exactly how ABC stores every node transform and keyframe.
    void    FromQuatPos(const float* quat4, const float* pos3, float* out);

    // ---- target ------------------------------------------------------ //

    void    ClearColor(const LT3DTarget& t, unsigned int nRGB);
    void    ClearDepth(const LT3DTarget& t);

    // ---- drawing ----------------------------------------------------- //

    // Rasterises one triangle given in CLIP space. Handles near-plane clipping
    // (which can split it into two), backface culling, the depth test, and
    // perspective-correct texturing.
    //
    // ⚠️ Winding is counter-clockwise-front in SCREEN space after the Y flip.
    // bCull=false draws both faces, which is what NOLF's own two-sided
    // surfaces need.
    //
    // Returns the number of pixels actually written -- so a caller can tell
    // "drew nothing because it was off-screen" from "drew nothing because the
    // pipeline is broken", which is the distinction that cost this port a day
    // on the 2D side.
    int     DrawTriangle(const LT3DTarget& target, const LT3DTexture* pTex,
                         const LT3DVert& a, const LT3DVert& b, const LT3DVert& c,
                         bool bCull = true);
}

#endif // __LT_3D_H__
