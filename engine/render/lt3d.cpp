// ----------------------------------------------------------------------- //
//
// MODULE  : lt3d.cpp
//
// PURPOSE : Textured, depth-buffered triangle rasteriser. See lt3d.h for the
//           conventions and for why this is software rather than Metal.
//
// ----------------------------------------------------------------------- //

#include "lt3d.h"

#include <cmath>
#include <cstring>
#include <stdio.h>
#include <cstdio>
#include <cstdlib>

namespace
{
    // ⚠️ THE ALPHA-TEST OFF SWITCH, AND IT IS AN INSTRUMENT RATHER THAN AN
    // OPTION. Punch-through alpha is decided by a HEURISTIC
    // (CLTDtx::AlphaDiscriminates -- "does this texture's alpha have both
    // transparent and opaque texels"), and a heuristic that is wrong does not
    // fail loudly: it silently deletes geometry, which looks exactly like a
    // model that was never drawn or was drawn in pieces.
    //
    // ⚠️ So there has to be a way to ask "is the geometry there at all?"
    // separately from "is it being punched out?" -- two hypotheses that are
    // indistinguishable from a screenshot and take one run to separate with
    // this. LT_NOALPHATEST=1 draws every texel opaque.
    bool NoAlphaTest()
    {
        static const bool b = (getenv("LT_NOALPHATEST") != 0);
        return b;
    }

    // ⚠️ A LAST-RESORT GUARD, NOT THE CLIP PLANE. The clip below is against the
    // projection's own near plane (`z + w > 0`); this only catches a caller
    // that hands in a matrix with no near plane at all, where w could still
    // reach zero and divide.
    //
    // ⚠️ THIS CONSTANT USED TO BE THE CLIP PLANE, AND THAT WAS THE BUG.
    // Clipping at w > 1e-5 "works" in the sense that nothing divides by zero,
    // and it is catastrophically wrong anyway: a vertex kept at w = 1e-5 has
    // 1/w = 100000, so its screen coordinate lands ~1e5 viewports away. The
    // triangle is still rasterised -- the bounding box is clamped to the
    // screen -- but every barycentric weight is now a ratio of enormous
    // numbers, and single precision has nothing left. What comes out is
    // texture coordinates that sample essentially at random (a hard-edged
    // crosshatch of noise) and a vertex colour that extrapolates past 1.0
    // (bright white wedges). Both appear ONLY when the camera is close enough
    // to something to clip it -- i.e. while walking -- which is what made it
    // look like a motion artefact rather than a clipping one.
    const float kNearEpsilon = 1e-5f;

    inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

    LT3DVert LerpVert(const LT3DVert& a, const LT3DVert& b, float t)
    {
        LT3DVert v;
        for (int i = 0; i < 4; ++i) v.m_Clip[i] = Lerp(a.m_Clip[i], b.m_Clip[i], t);
        v.m_U = Lerp(a.m_U, b.m_U, t);
        v.m_V = Lerp(a.m_V, b.m_V, t);
        v.m_R = Lerp(a.m_R, b.m_R, t);
        v.m_G = Lerp(a.m_G, b.m_G, t);
        v.m_B = Lerp(a.m_B, b.m_B, t);
        return v;
    }

    // A vertex once divided through: screen x/y, plus the values that must be
    // interpolated linearly in screen space.
    struct Screen
    {
        float x, y;
        float invW;         // 1/w   -- the depth value
        float uOverW, vOverW;
        float rOverW, gOverW, bOverW;
        float r, g, b;              // the raw colour, for affine shading
    };

    Screen ToScreen(const LT3DVert& v, int w, int h)
    {
        Screen s;
        float invW = 1.0f / v.m_Clip[3];
        s.x = (v.m_Clip[0] * invW * 0.5f + 0.5f) * (float)w;
        s.y = (v.m_Clip[1] * invW * 0.5f + 0.5f) * (float)h;
        s.invW   = invW;
        s.uOverW = v.m_U * invW;
        s.vOverW = v.m_V * invW;
        s.rOverW = v.m_R * invW;
        s.gOverW = v.m_G * invW;
        s.bOverW = v.m_B * invW;
        s.r = v.m_R; s.g = v.m_G; s.b = v.m_B;
        return s;
    }

    // One texture sample from one mip level, into a 0..1 RGBA quadruple.
    //
    // ⚠️ Factored out because anisotropic filtering takes SEVERAL of these per
    // pixel along the same axis, and having the nearest and bilinear cases
    // written once each is what keeps the two paths from drifting apart.
    inline void SampleLevel(const unsigned char* pLevel, int tw, int th,
                            float u, float v, bool bBilinear, float* o4)
    {
        const float kInv = 1.0f / 255.0f;

        if (bBilinear)
        {
            // Texel CENTRES are at +0.5, so the sample sits half a texel back.
            // Getting this wrong shifts the whole texture by half a texel and
            // looks like a UV bug.
            float fu = u * (float)tw - 0.5f;
            float fv = v * (float)th - 0.5f;
            float f0 = std::floor(fu), g0 = std::floor(fv);
            float ax = fu - f0, ay = fv - g0;

            // Wrap by masking, as the nearest path does. The +1 neighbour wraps
            // too, so the filter is seamless across the repeat rather than
            // clamping at the edge.
            int x0 = ((int)f0) & (tw - 1);
            int y0 = ((int)g0) & (th - 1);
            int x1 = (x0 + 1) & (tw - 1);
            int y1 = (y0 + 1) & (th - 1);

            const unsigned char* p00 = pLevel + ((size_t)y0 * tw + x0) * 4;
            const unsigned char* p10 = pLevel + ((size_t)y0 * tw + x1) * 4;
            const unsigned char* p01 = pLevel + ((size_t)y1 * tw + x0) * 4;
            const unsigned char* p11 = pLevel + ((size_t)y1 * tw + x1) * 4;

            float w00 = (1.0f - ax) * (1.0f - ay);
            float w10 = ax          * (1.0f - ay);
            float w01 = (1.0f - ax) * ay;
            float w11 = ax          * ay;

            for (int k = 0; k < 4; ++k)
                o4[k] = (p00[k]*w00 + p10[k]*w10 + p01[k]*w01 + p11[k]*w11) * kInv;
        }
        else
        {
            int tx = (int)std::floor(u * (float)tw);
            int ty = (int)std::floor(v * (float)th);
            tx &= (tw - 1);
            ty &= (th - 1);
            const unsigned char* px = pLevel + ((size_t)ty * tw + tx) * 4;
            for (int k = 0; k < 4; ++k) o4[k] = px[k] * kInv;
        }
    }

    // One sample from a LEVEL PAIR: level A, optionally blended toward level B.
    //
    // ⚠️ With pB null this is exactly SampleLevel -- the same call, not an
    // equivalent one -- so the nearest-level path keeps its previous result
    // when trilinear is off.
    inline void SampleLodPair(const unsigned char* pA, int aw, int ah,
                              const unsigned char* pB, int bw, int bh,
                              float fBlend, float u, float v,
                              bool bBilinear, float* o4)
    {
        SampleLevel(pA, aw, ah, u, v, bBilinear, o4);
        if (!pB) return;

        float t4[4];
        SampleLevel(pB, bw, bh, u, v, bBilinear, t4);
        for (int k = 0; k < 4; ++k) o4[k] += (t4[k] - o4[k]) * fBlend;
    }

    inline unsigned char ToByte(float f)
    {
        int i = (int)(f * 255.0f + 0.5f);
        if (i < 0) i = 0;
        if (i > 255) i = 255;
        return (unsigned char)i;
    }

    // Signed area x2 of the screen-space triangle. Sign gives the winding.
    inline float Area2(const Screen& a, const Screen& b, const Screen& c)
    {
        return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    }

    int RasteriseScreenTri(const LT3DTarget& t, const LT3DTexture* pTex,
                           const Screen& a, const Screen& b, const Screen& c,
                           bool bCull)
    {
        float fArea = Area2(a, b, c);

        // Degenerate triangles have no interior; dividing by this area would
        // produce infinities that then fail every comparison silently.
        if (fArea == 0.0f) return 0;

        const Screen* p0 = &a;
        const Screen* p1 = &b;
        const Screen* p2 = &c;

        if (fArea < 0.0f)
        {
            if (bCull) return 0;
            // Two-sided: swap two vertices so the edge functions below can
            // assume one winding, rather than carrying a sign through all of
            // them.
            p1 = &c; p2 = &b;
            fArea = -fArea;
        }

        float fInvArea = 1.0f / fArea;

        // Bounding box, clamped to the target and to the pixel centres that
        // can actually be covered.
        int minX = (int)std::floor(p0->x < p1->x ? (p0->x < p2->x ? p0->x : p2->x)
                                                 : (p1->x < p2->x ? p1->x : p2->x));
        int maxX = (int)std::ceil (p0->x > p1->x ? (p0->x > p2->x ? p0->x : p2->x)
                                                 : (p1->x > p2->x ? p1->x : p2->x));
        int minY = (int)std::floor(p0->y < p1->y ? (p0->y < p2->y ? p0->y : p2->y)
                                                 : (p1->y < p2->y ? p1->y : p2->y));
        int maxY = (int)std::ceil (p0->y > p1->y ? (p0->y > p2->y ? p0->y : p2->y)
                                                 : (p1->y > p2->y ? p1->y : p2->y));

        if (minX < 0) minX = 0;
        if (minY < 0) minY = 0;
        if (maxX > t.m_Width)  maxX = t.m_Width;
        if (maxY > t.m_Height) maxY = t.m_Height;
        if (minX >= maxX || minY >= maxY) return 0;

        // ⚠️ THE TOP-LEFT FILL RULE, and it is not cosmetic. Two triangles
        // sharing an edge must cover each shared pixel EXACTLY ONCE: draw it
        // twice and every alpha-blended or additive surface is doubled along
        // the seam; draw it zero times and every mesh has single-pixel cracks
        // along its diagonals. A NOLF model is thousands of shared edges.
        //
        // An edge is "top or left" when it goes right, or is horizontal and
        // goes left. Those edges own their boundary pixels; the others do not.
        struct Edge
        {
            float A, B, C;          // A*x + B*y + C, positive inside
            bool  bTopLeft;
        };

        Edge e[3];
        const Screen* v[3] = { p0, p1, p2 };
        for (int i = 0; i < 3; ++i)
        {
            const Screen& s = *v[i];
            const Screen& d = *v[(i + 1) % 3];
            e[i].A = s.y - d.y;
            e[i].B = d.x - s.x;
            e[i].C = s.x * d.y - s.y * d.x;
            float dx = d.x - s.x, dy = d.y - s.y;
            e[i].bTopLeft = (dy == 0.0f) ? (dx < 0.0f) : (dy < 0.0f);
        }

        // ---- mip selection setup ---------------------------------------- //
        //
        // ⚠️ Only when the caller supplied a chain. Everything below is skipped
        // otherwise, so the no-mip path is bit-for-bit what it always was.
        //
        // The texture coordinate at a pixel is a RATIO of two linear functions,
        // u = N(x,y) / D(x,y), because the perspective divide happens after
        // interpolating u/w and 1/w. So its screen derivative is the quotient
        // rule rather than a constant:
        //
        //     du/dx = (dN/dx - u * dD/dx) / D        (and D = 1/w, so / D is * w)
        //
        // ⚠️ BOTH derivatives are needed, not just d/dx. A floor seen at a
        // grazing angle barely changes u along a scanline while changing it
        // enormously down the screen -- taking only d/dx there would report a
        // fine LOD and alias exactly where aliasing is worst, which is the
        // corridor floor that prompted this.
        const bool bMip = (pTex && pTex->m_nMips > 1 && pTex->m_pMipRGBA[0] != 0);
        float dNu_dx = 0, dNv_dx = 0, dD_dx = 0;
        float dNu_dy = 0, dNv_dy = 0, dD_dy = 0;
        if (bMip)
        {
            const float dl0dx = e[1].A * fInvArea;
            const float dl1dx = e[2].A * fInvArea;
            const float dl2dx = e[0].A * fInvArea;
            const float dl0dy = e[1].B * fInvArea;
            const float dl1dy = e[2].B * fInvArea;
            const float dl2dy = e[0].B * fInvArea;

            dNu_dx = dl0dx * p0->uOverW + dl1dx * p1->uOverW + dl2dx * p2->uOverW;
            dNv_dx = dl0dx * p0->vOverW + dl1dx * p1->vOverW + dl2dx * p2->vOverW;
            dD_dx  = dl0dx * p0->invW   + dl1dx * p1->invW   + dl2dx * p2->invW;

            dNu_dy = dl0dy * p0->uOverW + dl1dy * p1->uOverW + dl2dy * p2->uOverW;
            dNv_dy = dl0dy * p0->vOverW + dl1dy * p1->vOverW + dl2dy * p2->vOverW;
            dD_dy  = dl0dy * p0->invW   + dl1dy * p1->invW   + dl2dy * p2->invW;
        }

        int nWritten = 0;

        for (int y = minY; y < maxY; ++y)
        {
            float fy = (float)y + 0.5f;
            for (int x = minX; x < maxX; ++x)
            {
                float fx = (float)x + 0.5f;

                float w0 = e[1].A * fx + e[1].B * fy + e[1].C;   // opposite v0
                float w1 = e[2].A * fx + e[2].B * fy + e[2].C;   // opposite v1
                float w2 = e[0].A * fx + e[0].B * fy + e[0].C;   // opposite v2

                bool bIn =
                    (w0 > 0.0f || (w0 == 0.0f && e[1].bTopLeft)) &&
                    (w1 > 0.0f || (w1 == 0.0f && e[2].bTopLeft)) &&
                    (w2 > 0.0f || (w2 == 0.0f && e[0].bTopLeft));
                if (!bIn) continue;

                float l0 = w0 * fInvArea;
                float l1 = w1 * fInvArea;
                float l2 = w2 * fInvArea;

                float invW = l0 * p0->invW + l1 * p1->invW + l2 * p2->invW;
                if (invW <= 0.0f) continue;
                // Past FarZ: clipped, as D3D's far plane clips it (lt3d.h).
                if (t.m_fFarW > 0.0f && invW * t.m_fFarW < 1.0f) continue;

                int nIdx = y * t.m_Width + x;

                // Depth: larger 1/w is nearer. Strictly-greater keeps the
                // FIRST of two coplanar surfaces, which is what stops z-fighting
                // from flickering with draw order.
                if (t.m_pDepth)
                {
                    if (invW <= t.m_pDepth[nIdx]) continue;
                }

                // ⚠️ A MIRROR: the reflected scene at this very pixel, as
                // rendered (lit, fogged) by its own pass -- see
                // LT3DTexture::m_pScreenRGBA. Depth is written, so what stands
                // in front of the mirror still hides it.
                if (pTex && pTex->m_pScreenRGBA)
                {
                    if (x < pTex->m_nScreenW && y < pTex->m_nScreenH)
                    {
                        const unsigned char* s = pTex->m_pScreenRGBA
                                               + ((size_t)y * pTex->m_nScreenW + x) * 4;
                        unsigned char* d = t.m_pRGBA + (size_t)nIdx * 4;
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                    }
                    if (t.m_pDepth) t.m_pDepth[nIdx] = invW;
                    ++nWritten;
                    continue;
                }

                // Perspective-correct recovery: interpolate u/w, then divide.
                float w = 1.0f / invW;
                float u = (l0 * p0->uOverW + l1 * p1->uOverW + l2 * p2->uOverW) * w;
                float vv = (l0 * p0->vOverW + l1 * p1->vOverW + l2 * p2->vOverW) * w;
                // ⚠️ Colour is interpolated AFFINELY when the target asks --
                // see LT3DTarget::m_bAffineColour for the corridor floor that
                // forced this. Perspective-correct recovery of a 0..1 quantity
                // across a very oblique triangle loses its low bits.
                float r, g, b;
                if (t.m_bAffineColour)
                {
                    r = l0 * p0->r + l1 * p1->r + l2 * p2->r;
                    g = l0 * p0->g + l1 * p1->g + l2 * p2->g;
                    b = l0 * p0->b + l1 * p1->b + l2 * p2->b;
                }
                else
                {
                    r = (l0 * p0->rOverW + l1 * p1->rOverW + l2 * p2->rOverW) * w;
                    g = (l0 * p0->gOverW + l1 * p1->gOverW + l2 * p2->gOverW) * w;
                    b = (l0 * p0->bOverW + l1 * p1->bOverW + l2 * p2->bOverW) * w;
                }

                float sr = r, sg = g, sb = b;
                float sa = 1.0f;

                // The sampled texel on its own, kept for the probe. 1.0 is
                // what the untextured path effectively multiplies by.
                float fTexR = 1.0f, fTexG = 1.0f, fTexB = 1.0f;
                // And which mip level was sampled, for the same probe.
                // 255 = no chain supplied, so mip 0 by construction.
                unsigned char nProbeMip = 255;

                bool bAlphaTest = false;
                const bool bBlend = (pTex && pTex->m_bBlend);
                if (pTex && pTex->m_pRGBA && pTex->m_Width > 0 && pTex->m_Height > 0)
                {
                    bAlphaTest = pTex->m_bAlphaTest && !NoAlphaTest();

                    // Wrap by masking. Powers of two only -- see lt3d.h.
                    int tw = pTex->m_Width, th = pTex->m_Height;
                    const unsigned char* pLevel = pTex->m_pRGBA;

                    // How many samples to take, and along which direction. Both
                    // stay at "one, centred" unless the mip path sets them.
                    int   nTaps = 1;
                    float duMaj = 0.0f, dvMaj = 0.0f;

                    // The second mip level, for trilinear. Null means "one
                    // level", which is every case when m_bTrilinear is false.
                    const unsigned char* pLevel2 = 0;
                    int   tw2 = 0, th2 = 0;
                    float fBlend = 0.0f;

                    if (bMip)
                    {
                        // Quotient rule; D is 1/w, so dividing by D is * w.
                        float dudx = (dNu_dx - u  * dD_dx) * w;
                        float dvdx = (dNv_dx - vv * dD_dx) * w;
                        float dudy = (dNu_dy - u  * dD_dy) * w;
                        float dvdy = (dNv_dy - vv * dD_dy) * w;

                        float fw = (float)tw, fh = (float)th;
                        float ax = dudx * fw, bx = dvdx * fh;
                        float ay = dudy * fw, by = dvdy * fh;
                        float r2x = ax * ax + bx * bx;
                        float r2y = ay * ay + by * by;

                        // ⚠️ THE FOOTPRINT IS AN ELLIPSE, NOT A CIRCLE, and on a
                        // wall seen edge-on it is a very long thin one: the
                        // texture races past along the wall while barely moving
                        // across it. Taking `max` here -- which is what this did
                        // -- picks a mip sized for the LONG axis and then blurs
                        // the short axis by the same factor. That is the
                        // smeared corridor wall: it is not the bilinear filter
                        // and not the LOD arithmetic, it is asking one square
                        // sample to cover a long thin area.
                        float r2maj = r2x, r2min = r2y;
                        duMaj = dudx; dvMaj = dvdx;
                        if (r2y > r2x)
                        {
                            r2maj = r2y; r2min = r2x;
                            duMaj = dudy; dvMaj = dvdy;
                        }

                        // Spread N samples along the major axis and choose the
                        // level for the MINOR one, so detail across the wall
                        // survives while detail along it is still filtered.
                        int nMax = t.m_nMaxAniso;
                        if (nMax > 1 && r2min > 0.0f)
                        {
                            int n = (int)(std::sqrt(r2maj / r2min) + 0.5f);
                            if (n < 1)    n = 1;
                            if (n > nMax) n = nMax;
                            nTaps = n;
                        }

                        // ⚠️ WITH nTaps == 1 THIS IS THE OLD EXPRESSION, BYTE FOR
                        // BYTE -- rho2 is r2maj and nothing else is evaluated.
                        // That is what makes the default off rather than merely
                        // subtle: no sqrt, no second branch, no drift.
                        float rho2 = r2maj;
                        if (nTaps > 1)
                        {
                            rho2 = r2maj / (float)(nTaps * nTaps);
                            if (rho2 < r2min) rho2 = r2min;
                        }

                        // lod = log2(sqrt(rho2)) = 0.5*log2(rho2). Nearest
                        // level, not trilinear: one sample, visible banding at
                        // the transitions, and no aliasing -- the trade this
                        // rasteriser wants until it has a reason otherwise.
                        int nLod = 0;
                        if (rho2 > 1.0f)
                        {
                            // ⚠️ The nearest-level expression is left EXACTLY as
                            // it was, evaluated the same way, so switching
                            // trilinear off returns the previous image rather
                            // than a close one.
                            if (!t.m_bTrilinear)
                            {
                                nLod = (int)(0.5f * std::log(rho2) * 1.4426950408889634f + 0.5f);
                                if (nLod >= pTex->m_nMips) nLod = pTex->m_nMips - 1;
                                if (nLod < 0) nLod = 0;
                            }
                            else
                            {
                                // Trilinear wants the level BELOW the LOD and
                                // the fraction into the next one -- floor, not
                                // round, or the blend would run backwards for
                                // half of every level.
                                float fLod = 0.5f * std::log(rho2) * 1.4426950408889634f;
                                int nLo = (int)fLod;
                                if (nLo < 0) { nLo = 0; fLod = 0.0f; }
                                if (nLo >= pTex->m_nMips - 1)
                                {
                                    // Past the end of the chain there is nothing
                                    // to blend toward.
                                    nLo = pTex->m_nMips - 1;
                                    if (nLo < 0) nLo = 0;
                                    nLod = nLo;
                                }
                                else
                                {
                                    nLod   = nLo;
                                    fBlend = fLod - (float)nLo;
                                    if (fBlend > 0.0f && pTex->m_pMipRGBA[nLo + 1])
                                    {
                                        pLevel2 = pTex->m_pMipRGBA[nLo + 1];
                                        tw2 = pTex->m_MipW[nLo + 1];
                                        th2 = pTex->m_MipH[nLo + 1];
                                    }
                                }
                            }
                        }
                        nProbeMip = (unsigned char)nLod;
                        if (pTex->m_pMipRGBA[nLod])
                        {
                            pLevel = pTex->m_pMipRGBA[nLod];
                            tw = pTex->m_MipW[nLod];
                            th = pTex->m_MipH[nLod];
                        }
                    }

                    // ⚠️ Bilinear is not used for alpha-tested textures -- see
                    // m_bBilinear in lt3d.h. Filtering across a punch-through
                    // edge fringes it.
                    const bool bFilter = t.m_bBilinear && !bAlphaTest;

                    float acc[4];
                    if (nTaps == 1)
                    {
                        // ⚠️ The single-tap case is kept as a straight call with
                        // the ORIGINAL coordinates -- no offset arithmetic to
                        // round differently -- so switching anisotropy off
                        // returns the exact previous image.
                        SampleLodPair(pLevel, tw, th, pLevel2, tw2, th2,
                                      fBlend, u, vv, bFilter, acc);
                    }
                    else
                    {
                        acc[0] = acc[1] = acc[2] = acc[3] = 0.0f;
                        for (int nT = 0; nT < nTaps; ++nT)
                        {
                            // Taps sit at the centres of nTaps equal slices of
                            // the major axis, spanning one pixel's worth of
                            // derivative and centred on the pixel.
                            float fOff = ((float)nT + 0.5f) / (float)nTaps - 0.5f;
                            float s4[4];
                            SampleLodPair(pLevel, tw, th, pLevel2, tw2, th2, fBlend,
                                          u  + fOff * duMaj,
                                          vv + fOff * dvMaj, bFilter, s4);
                            for (int k = 0; k < 4; ++k) acc[k] += s4[k];
                        }
                        const float fInv = 1.0f / (float)nTaps;
                        for (int k = 0; k < 4; ++k) acc[k] *= fInv;
                    }

                    sr *= acc[0];
                    sg *= acc[1];
                    sb *= acc[2];
                    sa  = acc[3];

                    fTexR = acc[0]; fTexG = acc[1]; fTexB = acc[2];
                }

                // ⚠️ Punch-through, not blending, and ONLY when the texture
                // says its alpha is meaningful (see LT3DTexture::m_bAlphaTest
                // -- most uncompressed DTX alpha is zero and means nothing).
                // A discarded texel must write neither colour NOR depth,
                // otherwise a fence or leaf card occludes everything behind it
                // with invisible pixels. Partial alpha is NOT handled here --
                // it belongs to the blended path a few lines below, which the
                // caller reaches by setting LT3DTexture::m_bBlend and drawing
                // those surfaces last (ltworldrender.cpp, pass 4).
                // ⚠️ The texture's OWN threshold, not a constant -- see
                // LT3DTexture::m_fAlphaRef. A zero here would mean "keep
                // everything", which is not what an unset field should do, so
                // anything outside (0,1] falls back to the old half.
                {
                    float fRef = pTex ? pTex->m_fAlphaRef : 0.5f;
                    if (!(fRef > 0.0f && fRef <= 1.0f)) fRef = 0.5f;
                    if (bAlphaTest && sa < fRef) continue;
                }

                // ⚠️ FOG, before the store and before any blend -- D3D fogs
                // the fragment, and the blend then composites the fogged
                // colour. See LT3DTarget::m_bFog.
                if (t.m_bFog && invW > 0.0f && t.m_fFogFar != t.m_fFogNear)
                {
                    float fDist = 1.0f / invW;
                    if (t.m_fFogKX != 0.0f || t.m_fFogKY != 0.0f)
                    {
                        const float tx = ((float)x + 0.5f - 0.5f * (float)t.m_Width)  * t.m_fFogKX;
                        const float ty = ((float)y + 0.5f - 0.5f * (float)t.m_Height) * t.m_fFogKY;
                        fDist *= sqrtf(1.0f + tx * tx + ty * ty);
                    }
                    float f = (fDist - t.m_fFogNear) / (t.m_fFogFar - t.m_fFogNear);
                    if (f > 0.0f)
                    {
                        if (f > 1.0f) f = 1.0f;
                        // ⚠️ 0..1 here -- sr/sg/sb are normalised (ToByte
                        // scales them), the target's colour is 0..255.
                        const float k255 = 1.0f / 255.0f;
                        float fr = t.m_FogRGB[0] * k255, fg = t.m_FogRGB[1] * k255, fb = t.m_FogRGB[2] * k255;
                        if (bBlend && pTex && pTex->m_nBlendOp == 1) fr = fg = fb = 0.0f;
                        else if (bBlend && pTex && pTex->m_nBlendOp == 2) fr = fg = fb = 1.0f;
                        sr += (fr - sr) * f;
                        sg += (fg - sg) * f;
                        sb += (fb - sb) * f;
                    }
                }

                unsigned char* d = t.m_pRGBA + (size_t)nIdx * 4;

                // ⚠️ THE BLENDED PATH -- see LT3DTexture::m_bBlend. It writes
                // NO DEPTH, so what is behind the glass still draws; the
                // caller is responsible for drawing these after the opaque
                // geometry and back-to-front among themselves.
                if (bBlend)
                {
                    const float inv = 1.0f / 255.0f;

                    // ⚠️ MULTIPLY IGNORES ALPHA, matching D3D's ZERO/SRCCOLOR.
                    // It is the mode the motorcycle prop asks for.
                    if (pTex->m_nBlendOp == 2)
                    {
                        d[0] = ToByte(sr * d[0] * inv);
                        d[1] = ToByte(sg * d[1] * inv);
                        d[2] = ToByte(sb * d[2] * inv);
                        d[3] = 255;
                        ++nWritten;
                        continue;
                    }

                    // ⚠️ TWO SOURCES OF ALPHA, MULTIPLIED. The texel's own is
                    // what makes glass glass; the constant is what makes a
                    // spotlight beam a beam, and for that prop the texel
                    // channel is all zero and carries nothing. See
                    // LT3DTexture::m_fConstAlpha / m_bTexAlpha.
                    float a = (pTex->m_bTexAlpha ? sa : 1.0f) * pTex->m_fConstAlpha;
                    if (a <= 0.0f) continue;        // nothing to contribute
                    if (a > 1.0f) a = 1.0f;

                    if (pTex->m_nBlendOp == 1)
                    {
                        // ADDITIVE: the source is LIGHT, so it only ever
                        // brightens and the destination keeps its own value.
                        d[0] = ToByte(sr * a + d[0] * inv);
                        d[1] = ToByte(sg * a + d[1] * inv);
                        d[2] = ToByte(sb * a + d[2] * inv);
                        d[3] = 255;
                        ++nWritten;
                        continue;
                    }

                    const float ia  = 1.0f - a;
                    d[0] = ToByte(sr * a + d[0] * inv * ia);
                    d[1] = ToByte(sg * a + d[1] * inv * ia);
                    d[2] = ToByte(sb * a + d[2] * inv * ia);
                    d[3] = 255;
                    ++nWritten;
                    continue;
                }

                d[0] = ToByte(sr);
                d[1] = ToByte(sg);
                d[2] = ToByte(sb);
                d[3] = 255;

                // ⚠️ THE PROBE -- see LT3DTarget::m_pProbeID. Stamped here and
                // nowhere else, because this is the only place a fragment
                // becomes the pixel's owner. The texel is recorded BEFORE the
                // light multiply, which is the whole point: the finished pixel
                // is texel x light and cannot be taken apart afterwards.
                if (t.m_pProbeID) t.m_pProbeID[nIdx] = LT3D::g_nProbeID;
                if (t.m_pProbeTex)
                {
                    unsigned char* q = t.m_pProbeTex + (size_t)nIdx * 4;
                    // With no texture the sampler leaves acc untouched, so the
                    // honest record is "fully lit white" -- exactly what the
                    // untextured path multiplies the light by.
                    q[0] = ToByte(fTexR); q[1] = ToByte(fTexG);
                    q[2] = ToByte(fTexB); q[3] = ToByte(sa);
                }
                if (t.m_pProbeUV)
                {
                    t.m_pProbeUV[(size_t)nIdx * 2 + 0] = u;
                    t.m_pProbeUV[(size_t)nIdx * 2 + 1] = vv;
                }
                if (t.m_pProbeMip) t.m_pProbeMip[nIdx] = nProbeMip;

                if (t.m_pDepth) t.m_pDepth[nIdx] = invW;
                ++nWritten;
            }
        }

        return nWritten;
    }
}


namespace LT3D
{

// See lt3d.h. Zero means "the caller is not probing"; the buffers being null
// is what actually switches it off.
unsigned g_nProbeID = 0;
bool     g_bProbeHold = false;

void Identity(float* m)
{
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void Multiply(const float* a, const float* b, float* out)
{
    float t[16];
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            t[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c]
                     + a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
        }
    }
    std::memcpy(out, t, sizeof(t));     // safe when out aliases a or b
}

void TransformPoint(const float* m, const float* p, float* out)
{
    for (int r = 0; r < 4; ++r)
        out[r] = m[r*4+0]*p[0] + m[r*4+1]*p[1] + m[r*4+2]*p[2] + m[r*4+3];
}

void LookAt(const float* eye, const float* at, const float* up, float* out)
{
    float f[3] = { at[0]-eye[0], at[1]-eye[1], at[2]-eye[2] };
    float fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (fl > 0.0f) { f[0]/=fl; f[1]/=fl; f[2]/=fl; }

    float s[3] = { f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0] };
    float sl = std::sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
    if (sl > 0.0f) { s[0]/=sl; s[1]/=sl; s[2]/=sl; }

    float u[3] = { s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0] };

    Identity(out);
    out[0]=s[0];  out[1]=s[1];  out[2]=s[2];
    out[4]=u[0];  out[5]=u[1];  out[6]=u[2];
    out[8]=-f[0]; out[9]=-f[1]; out[10]=-f[2];
    out[3]  = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
    out[7]  = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    out[11] =  (f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]);
}

void Perspective(float fFovY, float fAspect, float fNear, float fFar, float* out)
{
    float t = 1.0f / std::tan(fFovY * 0.5f);
    std::memset(out, 0, sizeof(float) * 16);
    out[0]  = t / fAspect;
    // ⚠️ The Y flip lives HERE, once. lt2d's surfaces and the decoded textures
    // are both top-down, so without this every model renders upside down --
    // and the tempting fix, flipping the blit, silently inverts mouse Y as
    // well. HANDOVER.md §5 records that exact mistake being made on the 2D
    // side; this is the same trap, pre-empted.
    out[5]  = -t;
    out[10] = (fFar + fNear) / (fNear - fFar);
    out[11] = (2.0f * fFar * fNear) / (fNear - fFar);
    out[14] = -1.0f;
}

void FromQuatPos(const float* q, const float* p, float* out)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;

    // ⚠️ s = 2/|q|^2, NOT 2 -- the lineage's own conversion (quat_ConvertTo
    // Matrix), which yields a proper rotation for ANY non-zero quaternion.
    // ABC keyframes are not exactly unit: |q| runs 0.99771..1.00177 across
    // NOLF's character animations, and with a bare 2 every such key scaled
    // and skewed its bone -- 0.08 units of bone length on Cate, found by the
    // ROTATIONONLY bone-length oracle in enginetest_abc. For a unit quaternion
    // this is the old expression exactly. LT_NOQUATNORM=1 restores the bare 2.
    static const bool s_bNoNorm = (getenv("LT_NOQUATNORM") != 0);
    static const bool s_bSaid = (s_bNoNorm ? (printf("[MATH] LT_NOQUATNORM=1 -- quaternions "
                                                     "converted as if unit length\n"), true) : true);
    (void)s_bSaid;
    const float n2 = xx + yy + zz + w*w;
    const float s = (s_bNoNorm || !(n2 > 0.0f)) ? 2.0f : 2.0f / n2;

    Identity(out);
    out[0] = 1.0f - s*(yy+zz);  out[1] = s*(xy-wz);         out[2] = s*(xz+wy);
    out[4] = s*(xy+wz);         out[5] = 1.0f - s*(xx+zz);  out[6] = s*(yz-wx);
    out[8] = s*(xz-wy);         out[9] = s*(yz+wx);         out[10]= 1.0f - s*(xx+yy);
    out[3] = p[0]; out[7] = p[1]; out[11] = p[2];
}

void ClearColor(const LT3DTarget& t, unsigned int nRGB)
{
    unsigned char r = (unsigned char)((nRGB >> 16) & 0xFF);
    unsigned char g = (unsigned char)((nRGB >> 8) & 0xFF);
    unsigned char b = (unsigned char)(nRGB & 0xFF);
    int n = t.m_Width * t.m_Height;
    for (int i = 0; i < n; ++i)
    {
        unsigned char* d = t.m_pRGBA + (size_t)i * 4;
        d[0] = r; d[1] = g; d[2] = b; d[3] = 255;
    }
}

void ClearDepth(const LT3DTarget& t)
{
    if (!t.m_pDepth) return;
    // 0 is "infinitely far", since depth is 1/w and nearer is larger.
    std::memset(t.m_pDepth, 0, sizeof(float) * (size_t)(t.m_Width * t.m_Height));
}

int DrawTriangle(const LT3DTarget& target, const LT3DTexture* pTex,
                 const LT3DVert& a, const LT3DVert& b, const LT3DVert& c,
                 bool bCull)
{
    if (!target.m_pRGBA || target.m_Width <= 0 || target.m_Height <= 0)
        return 0;

    // ---- near-plane clipping ---------------------------------------- //
    //
    // ⚠️ This must happen BEFORE the perspective divide, and it is not
    // optional. A vertex with w <= 0 is behind the eye; dividing by it mirrors
    // the vertex to the wrong side of the screen, so the triangle is drawn
    // inside-out across the whole viewport. It is the single most common way a
    // first rasteriser looks catastrophically broken while every other part is
    // correct.
    // ⚠️ CLIP AGAINST THE PROJECTION'S OWN NEAR PLANE, which the matrix already
    // carries -- no caller has to pass it and no two callers can disagree. For
    // this Perspective(), a view-space point at distance fNear projects to
    // z = -w, so the plane is exactly `z + w = 0` and "in front" is `z + w > 0`.
    // A vertex clipped onto it comes back with w == fNear (4.0 for the world,
    // 1.0 for the menu scene), which is perfectly conditioned -- that is the
    // whole difference from the 1e-5 version above.
    const LT3DVert* in[3] = { &a, &b, &c };

    LT3DVert kept[4];
    int nKept = 0;

    for (int i = 0; i < 3; ++i)
    {
        const LT3DVert& cur = *in[i];
        const LT3DVert& nxt = *in[(i + 1) % 3];
        const float dCur = cur.m_Clip[2] + cur.m_Clip[3];
        const float dNxt = nxt.m_Clip[2] + nxt.m_Clip[3];
        bool bCurIn = dCur > 0.0f;
        bool bNxtIn = dNxt > 0.0f;

        if (bCurIn) kept[nKept++] = cur;

        if (bCurIn != bNxtIn)
        {
            float t = dCur / (dCur - dNxt);
            kept[nKept++] = LerpVert(cur, nxt, t);
        }
        if (nKept >= 4) break;
    }

    if (nKept < 3) return 0;

    // ⚠️ The guard, for a matrix with no near plane: after the clip above, w is
    // fNear or more for a real projection, but a caller passing an identity or
    // orthographic matrix is not clipped by it at all and could still divide by
    // zero. Refusing the triangle is right -- it has no screen position.
    for (int i = 0; i < nKept; ++i)
        if (kept[i].m_Clip[3] <= kNearEpsilon) return 0;

    // Clipping a triangle against one plane yields a triangle or a quad; the
    // quad is emitted as a fan, which is exact for a convex polygon.
    Screen s[4];
    for (int i = 0; i < nKept; ++i)
        s[i] = ToScreen(kept[i], target.m_Width, target.m_Height);

    int nWritten = RasteriseScreenTri(target, pTex, s[0], s[1], s[2], bCull);
    if (nKept == 4)
        nWritten += RasteriseScreenTri(target, pTex, s[0], s[2], s[3], bCull);

    return nWritten;
}

} // namespace LT3D
