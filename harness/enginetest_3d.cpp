// ----------------------------------------------------------------------- //
//
// enginetest_3d.cpp -- the software rasteriser, on synthetic buffers with
//                      exactly known answers.
//
// ⚠️ NO GAME DATA AND NO ENGINE HEADERS, for the reason lt2d's tests give:
//   every case here has an answer that can be computed by hand, so a failure
//   says which rule is wrong rather than "the picture looks off".
//
//   The rules under test are the ones that make a rasteriser silently wrong
//   rather than obviously broken: the fill rule, perspective correction, the
//   near plane, and the depth comparison.
//
// ----------------------------------------------------------------------- //

#include "lt3d.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <set>

static int g_nPass = 0;
static int g_nFail = 0;

static void ok(const char* w)             { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)            { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w)  { b ? ok(w) : bad(w); }

static bool Near(float a, float b, float eps = 1e-5f) { return fabsf(a - b) <= eps; }

// A vertex placed directly in clip space, so the rasteriser can be tested
// without going through the matrix pipeline.
static LT3DVert V(float cx, float cy, float cz, float cw,
                  float u = 0, float v = 0,
                  float r = 1, float g = 1, float b = 1)
{
    LT3DVert o;
    o.m_Clip[0] = cx; o.m_Clip[1] = cy; o.m_Clip[2] = cz; o.m_Clip[3] = cw;
    o.m_U = u; o.m_V = v;
    o.m_R = r; o.m_G = g; o.m_B = b;
    return o;
}

struct Buf
{
    std::vector<unsigned char> rgba;
    std::vector<float>         depth;
    LT3DTarget                 t;

    Buf(int w, int h, bool bDepth = true)
    {
        rgba.assign((size_t)w * h * 4, 0);
        if (bDepth) depth.assign((size_t)w * h, 0.0f);
        t.m_pRGBA = &rgba[0];
        t.m_pDepth = bDepth ? &depth[0] : 0;
        t.m_Width = w; t.m_Height = h;
    }
    const unsigned char* At(int x, int y) const { return &rgba[((size_t)y * t.m_Width + x) * 4]; }
    int Lit() const
    {
        int n = 0;
        for (size_t i = 0; i < rgba.size(); i += 4)
            if (rgba[i] || rgba[i+1] || rgba[i+2]) ++n;
        return n;
    }
};


int main()
{
    // ================= matrices ==================================== //
    printf("\n--- matrices ---\n");
    {
        float I[16], M[16], R[16];
        LT3D::Identity(I);
        for (int i = 0; i < 16; ++i) M[i] = (float)(i + 1);
        LT3D::Multiply(M, I, R);
        check(memcmp(M, R, sizeof(M)) == 0, "M * I == M");

        // ⚠️ Multiply must survive out aliasing an input -- composing a
        // transform chain in place is the normal use and a naive
        // implementation corrupts itself halfway through.
        float A[16];
        memcpy(A, M, sizeof(M));
        LT3D::Multiply(A, I, A);
        check(memcmp(A, M, sizeof(M)) == 0, "Multiply is safe when out aliases an input");

        float p[3] = { 1.0f, 2.0f, 3.0f }, out[4];
        LT3D::Identity(I);
        I[3] = 10.0f; I[7] = 20.0f; I[11] = 30.0f;
        LT3D::TransformPoint(I, p, out);
        check(Near(out[0], 11.0f) && Near(out[1], 22.0f) && Near(out[2], 33.0f),
              "translation lands in the 4th COLUMN (row-major, column vectors)");
    }
    {
        float eye[3] = { 0, 0, 5 }, at[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
        float V4[16], o[4], origin[3] = { 0, 0, 0 };
        LT3D::LookAt(eye, at, up, V4);
        LT3D::TransformPoint(V4, origin, o);
        check(Near(o[0], 0) && Near(o[1], 0) && Near(o[2], -5.0f),
              "LookAt puts the target 5 units down -Z (right-handed)");
    }
    {
        float P[16], o[4];
        LT3D::Perspective(1.5707963f /*90 deg*/, 1.0f, 1.0f, 100.0f, P);

        float onNear[3] = { 0, 0, -1 };
        LT3D::TransformPoint(P, onNear, o);
        check(Near(o[3], 1.0f), "a point 1 unit ahead has w == 1 at near=1");

        // ⚠️ THE Y FLIP, ASSERTED. World +Y must land in the UPPER half of a
        // top-down target. Getting this wrong renders every model upside down,
        // and the tempting fix -- flipping the blit -- also inverts mouse Y.
        float upPt[3] = { 0, 1, -1 };
        LT3D::TransformPoint(P, upPt, o);
        check(o[1] < 0.0f, "world +Y maps to clip -Y, i.e. the top of the screen");
    }
    {
        // A 180-degree turn about Z as a quaternion -- the exact case the ABC
        // loader's cross-check turned on (a node matrix diag(-1,-1,1,1)).
        float q[4] = { 0, 0, 1, 0 }, p[3] = { 0, 0, 0 }, M[16];
        LT3D::FromQuatPos(q, p, M);
        check(Near(M[0], -1.0f) && Near(M[5], -1.0f) && Near(M[10], 1.0f),
              "quaternion (0,0,1,0) builds diag(-1,-1,1) -- matches ABC's node matrix");
    }

    // ================= the fill rule ================================= //
    printf("\n--- coverage and the fill rule ---\n");
    {
        const int W = 32, H = 16;
        Buf b(W, H, false);

        // A screen-aligned quad covering the target exactly, as two triangles
        // sharing the diagonal. All w == 1, so screen x = (cx*0.5+0.5)*W.
        LT3DVert tl = V(-1, -1, 0, 1), tr = V(1, -1, 0, 1);
        LT3DVert br = V(1, 1, 0, 1),   bl = V(-1, 1, 0, 1);

        int n1 = LT3D::DrawTriangle(b.t, 0, tl, tr, br, false);
        int n2 = LT3D::DrawTriangle(b.t, 0, tl, br, bl, false);

        printf("        triangle 1 wrote %d px, triangle 2 wrote %d px, target is %d\n",
               n1, n2, W * H);

        // ⚠️ THE CHECK THAT MATTERS. Every pixel exactly once: drawn twice and
        // any additive/blended surface doubles along the seam; drawn zero times
        // and every mesh has single-pixel cracks along its diagonals.
        check(n1 + n2 == W * H,
              "two triangles sharing an edge cover the target EXACTLY once");
        check(b.Lit() == W * H, "and every pixel really is lit");
    }
    {
        // NEGATIVE CONTROL: the coverage test above would also pass if the
        // rasteriser simply filled the whole target regardless of geometry.
        const int W = 32, H = 16;
        Buf b(W, H, false);
        // Left half only: screen x 0..W/2.
        int n = LT3D::DrawTriangle(b.t, 0, V(-1,-1,0,1), V(0,-1,0,1), V(0,1,0,1), false);
        n    += LT3D::DrawTriangle(b.t, 0, V(-1,-1,0,1), V(0,1,0,1), V(-1,1,0,1), false);
        printf("        half-width quad wrote %d px, expected %d\n", n, W/2 * H);
        check(n == (W/2) * H, "NEGATIVE CONTROL: a half-width quad covers exactly half");
        check(b.At(W-1, H/2)[0] == 0, "and the far side is untouched");
    }

    // ================= backface culling ============================== //
    printf("\n--- winding ---\n");
    {
        const int W = 16, H = 16;
        LT3DVert a = V(-1,-1,0,1), bb = V(1,-1,0,1), c = V(1,1,0,1);
        Buf f(W, H, false), r(W, H, false), r2(W, H, false);
        int nFwd  = LT3D::DrawTriangle(f.t,  0, a, bb, c, true);
        int nRev  = LT3D::DrawTriangle(r.t,  0, a, c, bb, true);
        int nRev2 = LT3D::DrawTriangle(r2.t, 0, a, c, bb, false);
        printf("        forward %d, reversed+cull %d, reversed+two-sided %d\n",
               nFwd, nRev, nRev2);
        check(nFwd > 0, "a front-facing triangle draws");
        check(nRev == 0, "NEGATIVE CONTROL: the same triangle wound backwards is culled");
        check(nRev2 == nFwd, "and two-sided drawing covers exactly the same pixels");
    }

    // ================= depth ========================================= //
    printf("\n--- depth ---\n");
    {
        const int W = 16, H = 16;
        Buf b(W, H, true);
        LT3D::ClearDepth(b.t);

        // Near (w=1) is red; far (w=4) is green. Draw FAR SECOND: it must lose.
        LT3DVert n0 = V(-1,-1,0,1,0,0, 1,0,0), n1 = V(1,-1,0,1,0,0, 1,0,0),
                 n2 = V(1,1,0,1,0,0, 1,0,0),   n3 = V(-1,1,0,1,0,0, 1,0,0);
        LT3D::DrawTriangle(b.t, 0, n0, n1, n2, false);
        LT3D::DrawTriangle(b.t, 0, n0, n2, n3, false);

        LT3DVert f0 = V(-4,-4,0,4,0,0, 0,1,0), f1 = V(4,-4,0,4,0,0, 0,1,0),
                 f2 = V(4,4,0,4,0,0, 0,1,0),   f3 = V(-4,4,0,4,0,0, 0,1,0);
        int nFar = LT3D::DrawTriangle(b.t, 0, f0, f1, f2, false)
                 + LT3D::DrawTriangle(b.t, 0, f0, f2, f3, false);

        const unsigned char* px = b.At(W/2, H/2);
        printf("        far surface wrote %d px; centre is (%d,%d,%d)\n",
               nFar, px[0], px[1], px[2]);
        check(nFar == 0, "a farther surface drawn second is rejected by the depth test");
        check(px[0] == 255 && px[1] == 0, "the near surface survives");

        // NEGATIVE CONTROL: with no depth buffer the later draw must win --
        // otherwise the check above proves nothing about depth, only that the
        // second draw was skipped for some other reason.
        Buf nb(W, H, false);
        LT3D::DrawTriangle(nb.t, 0, n0, n1, n2, false);
        LT3D::DrawTriangle(nb.t, 0, n0, n2, n3, false);
        int nFar2 = LT3D::DrawTriangle(nb.t, 0, f0, f1, f2, false)
                  + LT3D::DrawTriangle(nb.t, 0, f0, f2, f3, false);
        const unsigned char* px2 = nb.At(W/2, H/2);
        check(nFar2 > 0 && px2[1] == 255,
              "NEGATIVE CONTROL: without a depth buffer the later draw wins");
    }

    // ================= the near plane ================================ //
    printf("\n--- near plane ---\n");
    {
        const int W = 32, H = 32;
        Buf behind(W, H, false), across(W, H, false);

        // Entirely behind the eye (w < 0).
        int nBehind = LT3D::DrawTriangle(behind.t, 0,
                        V(-1,-1,0,-1), V(1,-1,0,-1), V(1,1,0,-1), false);
        check(nBehind == 0, "a triangle entirely behind the eye draws nothing");

        // ⚠️ STRADDLING THE EYE PLANE -- the case that makes a first rasteriser
        // look catastrophically broken. Without near clipping the w<0 vertex
        // divides to the wrong side of the screen and the triangle smears
        // across the whole viewport.
        //
        // ⚠️ THE TEST GEOMETRY HERE IS LOAD-BEARING, and two different
        // accidents made this case draw nothing against a CORRECT rasteriser:
        //
        //   1. hand-written clip coordinates with both front vertices at the
        //      same clip y -- the clipped polygon then projects onto a single
        //      screen row, zero area;
        //   2. world points that are COLLINEAR IN PROJECTION, e.g.
        //      (-1,-1,-2), (1,1,-2), (0,0,2) -- all three lie in the plane
        //      x = y through the eye, so they project to one line.
        //
        // Both are genuinely degenerate triangles that correctly cover no
        // pixels. Neither is obvious by inspection, which is exactly why the
        // vertex-count and area assertions below are stated explicitly rather
        // than trusting "it drew something".
        float P[16], Vm[16], MVP[16];
        float eye[3] = { 0, 0, 0 }, at[3] = { 0, 0, -1 }, up[3] = { 0, 1, 0 };
        LT3D::Perspective(1.5707963f, 1.0f, 1.0f, 100.0f, P);
        LT3D::LookAt(eye, at, up, Vm);
        LT3D::Multiply(P, Vm, MVP);

        float wp[3][3] = { { -1,  0, -2 },      // in front of the camera
                           {  0,  1, -2 },      // in front, a different height
                           {  1, -1,  2 } };    // BEHIND the camera
        LT3DVert cv[3];
        for (int i = 0; i < 3; ++i)
        {
            LT3D::TransformPoint(MVP, wp[i], cv[i].m_Clip);
            cv[i].m_U = cv[i].m_V = 0;
            cv[i].m_R = cv[i].m_G = cv[i].m_B = 1;
        }
        printf("        vertex w values: %.2f %.2f %.2f (third is behind the eye)\n",
               cv[0].m_Clip[3], cv[1].m_Clip[3], cv[2].m_Clip[3]);
        check(cv[2].m_Clip[3] < 0.0f, "the third vertex really is behind the eye");

        int nAcross = LT3D::DrawTriangle(across.t, 0, cv[0], cv[1], cv[2], false);
        printf("        straddling triangle wrote %d px of %d\n", nAcross, W*H);
        check(nAcross > 0, "a triangle crossing the eye plane still draws");
        check(nAcross < W * H,
              "NEGATIVE CONTROL: and it does NOT smear across the whole viewport");

        // ⚠️ AND THE INTERPOLANTS MUST STILL BE SANE -- the assertion whose
        // absence let a real bug live in this rasteriser for a whole phase.
        //
        // The clip plane used to be `w > 1e-5` rather than the projection's
        // near plane. Nothing above catches that: the triangle still draws,
        // and it still covers fewer pixels than the viewport, because the
        // bounding box is clamped. What breaks is PRECISION -- a vertex kept
        // at w = 1e-5 lands ~1e5 viewports away, and every barycentric weight
        // becomes a ratio of enormous numbers. The visible result was texture
        // coordinates sampling at random and colours extrapolating past 1.0.
        //
        // ⚠️ The invariant that catches it: give the three vertices pure RED,
        // GREEN and BLUE, and every drawn pixel must have R+G+B == 255. That
        // holds because each channel receives exactly one barycentric weight
        // and the three weights sum to one -- and it holds identically under
        // perspective-correct and affine interpolation, so one check covers
        // both paths. A weight set that has lost its precision does not sum to
        // one, and the sum drifts or saturates.
        for (int nMode = 0; nMode < 2; ++nMode)
        {
            Buf rgb(W, H, false);
            rgb.t.m_bAffineColour = (nMode == 1);

            LT3DVert tv[3];
            for (int i = 0; i < 3; ++i)
            {
                LT3D::TransformPoint(MVP, wp[i], tv[i].m_Clip);
                tv[i].m_U = tv[i].m_V = 0;
                tv[i].m_R = tv[i].m_G = tv[i].m_B = 0;
            }
            tv[0].m_R = 1.0f; tv[1].m_G = 1.0f; tv[2].m_B = 1.0f;

            int nPix = LT3D::DrawTriangle(rgb.t, 0, tv[0], tv[1], tv[2], false);
            int nBad = 0, nWorst = 0;
            for (size_t i = 0; i < rgb.rgba.size(); i += 4)
            {
                if (!rgb.rgba[i+3]) continue;
                int sum = rgb.rgba[i] + rgb.rgba[i+1] + rgb.rgba[i+2];
                int err = sum > 255 ? sum - 255 : 255 - sum;
                if (err > nWorst) nWorst = err;
                if (err > 2) ++nBad;
            }
            printf("        %-22s %6d px, worst |R+G+B-255| = %d\n",
                   nMode ? "affine colour:" : "perspective colour:", nPix, nWorst);
            check(nPix > 0 && nBad == 0,
                  nMode ? "a near-clipped triangle interpolates sanely (affine)"
                        : "a near-clipped triangle interpolates sanely (perspective)");
        }

        // ⚠️ THE CHECK THAT ACTUALLY CATCHES THE 1e-5 CLIP -- and it took two
        // attempts to find one that does. THE DEPTH BUFFER IS THE WITNESS.
        //
        // ⚠️ What did NOT work, recorded so it is not tried again: asserting
        // that pure-R/G/B vertices interpolate to pixels summing to 255. That
        // is an algebraic identity -- the three barycentric weights sum to the
        // triangle's area by construction -- so it survives complete loss of
        // precision and passes against the broken clip. It is kept above as a
        // cheap sanity check, but it discriminates NOTHING on its own.
        //
        // Depth does discriminate, because it is the quantity the bad clip
        // actually corrupts. The rasteriser stores 1/w. A vertex clipped onto
        // a near plane of 1.0 has w == 1.0, so NO pixel may exceed 1/w == 1.0.
        // Clipping at w > 1e-5 instead lets w be 1e-5, i.e. 1/w == 100000 --
        // five orders of magnitude out, and no tolerance can hide that.
        {
            Buf dep(W, H, true);
            LT3D::ClearDepth(dep.t);
            LT3D::DrawTriangle(dep.t, 0, cv[0], cv[1], cv[2], false);

            float fMax = 0.0f;
            for (size_t i = 0; i < dep.depth.size(); ++i)
                if (dep.depth[i] > fMax) fMax = dep.depth[i];

            printf("        deepest 1/w written = %.4f (near plane is 1.0, "
                   "so this must not exceed 1.0)\n", fMax);
            check(fMax > 0.0f && fMax <= 1.001f,
                  "a near-clipped vertex lands ON the near plane, not past it");
        }
    }

    // ================= perspective-correct texturing ================= //
    printf("\n--- perspective correction ---\n");
    {
        // A 256x1 texture whose red channel encodes the texel index, so the
        // colour written reports exactly which texel was sampled.
        const int TW = 256;
        std::vector<unsigned char> tex((size_t)TW * 4, 0);
        for (int i = 0; i < TW; ++i)
        {
            tex[i*4+0] = (unsigned char)i;
            tex[i*4+3] = 255;
        }
        LT3DTexture t; t.m_pRGBA = &tex[0]; t.m_Width = TW; t.m_Height = 1; t.m_bAlphaTest = false;

        const int W = 64, H = 8;
        Buf b(W, H, false);

        // Left edge at w=1 with u=0, right edge at w=3 with u=1. Screen x is
        // (cx/w * 0.5 + 0.5) * W, so cx = -w on the left and +w on the right.
        LT3DVert tl = V(-1,-1,0,1, 0,0), bl = V(-1, 1,0,1, 0,0);
        LT3DVert tr = V( 3,-3,0,3, 1,0), br = V( 3, 3,0,3, 1,0);
        LT3D::DrawTriangle(b.t, &t, tl, tr, br, false);
        LT3D::DrawTriangle(b.t, &t, tl, br, bl, false);

        // Analytic expectation, computed here and not taken from the code:
        //   u(t) = (u0/w0*(1-t) + u1/w1*t) / (1/w0*(1-t) + 1/w1*t)
        const int x = W / 2;
        float tt = ((float)x + 0.5f) / (float)W;
        float w0 = 1.0f, w1 = 3.0f, u0 = 0.0f, u1 = 1.0f;
        float num = (u0 / w0) * (1 - tt) + (u1 / w1) * tt;
        float den = (1.0f / w0) * (1 - tt) + (1.0f / w1) * tt;
        float uPersp  = num / den;
        float uAffine = u0 * (1 - tt) + u1 * tt;

        int nExpect = (int)floorf(uPersp * TW);
        int nAffine = (int)floorf(uAffine * TW);
        int nGot = b.At(x, H / 2)[0];

        printf("        at x=%d: perspective u=%.4f (texel %d), affine u=%.4f (texel %d), got %d\n",
               x, uPersp, nExpect, uAffine, nAffine, nGot);

        check(nGot == nExpect, "the sampled texel matches the analytic perspective-correct value");

        // ⚠️ Without this the check above passes on an affine rasteriser too
        // whenever the two happen to agree. They must actually differ here.
        check(nAffine != nExpect,
              "NEGATIVE CONTROL: the affine answer is different, so this case discriminates");
        check(nGot != nAffine, "and the rasteriser did NOT produce the affine answer");
    }

    // ================= texture wrap and punch-through ================ //
    printf("\n--- sampling ---\n");
    {
        const int TW = 4, TH = 4;
        std::vector<unsigned char> tex((size_t)TW * TH * 4, 0);
        for (int i = 0; i < TW * TH; ++i)
        {
            tex[i*4+0] = (unsigned char)(i * 16);
            tex[i*4+1] = 128;
            tex[i*4+2] = 200;
            tex[i*4+3] = 255;
        }
        LT3DTexture t; t.m_pRGBA = &tex[0]; t.m_Width = TW; t.m_Height = TH; t.m_bAlphaTest = true;

        const int W = 8, H = 8;
        Buf a(W, H, false), c(W, H, false);

        // u in [0,1) versus u in [2,3): wrapping must make them identical.
        LT3D::DrawTriangle(a.t, &t, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
        LT3D::DrawTriangle(c.t, &t, V(-1,-1,0,1, 2,2), V(1,-1,0,1, 3,2), V(1,1,0,1, 3,3), false);
        check(a.rgba == c.rgba, "texture coordinates wrap by whole tiles");

        // ⚠️ PUNCH-THROUGH: a fully transparent texel must not be drawn and
        // must not write depth. DXT1's 3-colour blocks produce exactly these,
        // and a fence or leaf card that wrote depth would occlude everything
        // behind it with invisible pixels.
        std::vector<unsigned char> clear((size_t)TW * TH * 4, 0);
        for (int i = 0; i < TW * TH; ++i) { clear[i*4+0] = 255; clear[i*4+3] = 0; }
        LT3DTexture ct; ct.m_pRGBA = &clear[0]; ct.m_Width = TW; ct.m_Height = TH; ct.m_bAlphaTest = true;

        Buf p(W, H, true);
        LT3D::ClearDepth(p.t);
        int nDrawn = LT3D::DrawTriangle(p.t, &ct, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
        check(nDrawn == 0, "a fully transparent texture draws no pixels");
        bool bDepthClean = true;
        for (size_t i = 0; i < p.depth.size(); ++i) if (p.depth[i] != 0.0f) bDepthClean = false;
        check(bDepthClean, "and writes no depth -- so it cannot occlude invisibly");
    }

    // ================= bilinear filtering ============================ //
    printf("\n--- bilinear filtering ---\n");
    {
        // ⚠️ THE SAFETY PROPERTY FIRST, as with the mip chain: every caller
        // that predates this flag assigns four fields and nothing else.
        LT3DTarget fresh;
        check(fresh.m_bBilinear == false,
              "a default-constructed target does NOT filter");

        // A 2x2 texture of four unmistakable colours. Magnified hugely, the
        // nearest sampler must produce exactly four flat blocks; the filter
        // must produce a gradient between them.
        const int TW = 2, TH = 2;
        unsigned char tex[2*2*4] = {
              0,   0,   0, 255,     255, 255, 255, 255,
            255, 255, 255, 255,       0,   0,   0, 255 };
        LT3DTexture t2; t2.m_pRGBA = tex; t2.m_Width = TW; t2.m_Height = TH;

        // Count how many DISTINCT red values appear across the quad. Nearest
        // can only ever emit the values that are in the texture.
        const int W = 32, H = 32;
        Buf np(W, H, false), fp(W, H, false);
        fp.t.m_bBilinear = true;

        for (int k = 0; k < 2; ++k)
        {
            Buf& b = k ? fp : np;
            LT3D::DrawTriangle(b.t, &t2, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
            LT3D::DrawTriangle(b.t, &t2, V(-1,-1,0,1, 0,0), V(1,1,0,1, 1,1), V(-1,1,0,1, 0,1), false);
        }

        std::set<int> nearestVals, filteredVals;
        for (size_t i = 0; i < np.rgba.size(); i += 4)
        {
            if (np.rgba[i+3]) nearestVals.insert(np.rgba[i]);
            if (fp.rgba[i+3]) filteredVals.insert(fp.rgba[i]);
        }
        check(nearestVals.size() <= 2,
              "nearest sampling emits only the texture's own values");
        check(filteredVals.size() > 8,
              "and filtering emits a gradient between them");

        // ⚠️ THE WRAP MUST BE SEAMLESS. The +1 neighbour of the last texel is
        // texel 0, not a clamp -- otherwise every tile shows a seam. Sampling
        // the same surface one whole tile over must give the same picture.
        Buf wa(W, H, false), wb(W, H, false);
        wa.t.m_bBilinear = true; wb.t.m_bBilinear = true;
        LT3D::DrawTriangle(wa.t, &t2, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
        LT3D::DrawTriangle(wb.t, &t2, V(-1,-1,0,1, 3,3), V(1,-1,0,1, 4,3), V(1,1,0,1, 4,4), false);
        check(wa.rgba == wb.rgba, "the filter wraps rather than clamping at the edge");

        // ⚠️ ALPHA-TESTED TEXTURES ARE NOT FILTERED -- see lt3d.h. The
        // silhouette must be bit-identical to the nearest path, or every
        // cut-out edge grows a fringe of whatever sits under the cut.
        unsigned char cut[2*2*4] = {
            255,   0,   0, 255,       0, 255,   0,   0,
              0, 255,   0,   0,     255,   0,   0, 255 };
        LT3DTexture ta; ta.m_pRGBA = cut; ta.m_Width = TW; ta.m_Height = TH;
        ta.m_bAlphaTest = true;

        Buf ca(W, H, false), cb(W, H, false);
        cb.t.m_bBilinear = true;
        LT3D::DrawTriangle(ca.t, &ta, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
        LT3D::DrawTriangle(cb.t, &ta, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
        check(ca.rgba == cb.rgba,
              "an alpha-tested texture samples identically with the filter on");

        // NEGATIVE CONTROL: the same texture WITHOUT the alpha-test flag does
        // change -- so the check above is testing the suppression, not a
        // filter that quietly does nothing.
        LT3DTexture tn = ta; tn.m_bAlphaTest = false;
        Buf cc(W, H, false), cd(W, H, false);
        cd.t.m_bBilinear = true;
        LT3D::DrawTriangle(cc.t, &tn, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
        LT3D::DrawTriangle(cd.t, &tn, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
        check(cc.rgba != cd.rgba,
              "NEGATIVE CONTROL: without the alpha flag the filter does change it");
    }

    // ================= determinism =================================== //
    printf("\n--- mip selection ---\n");
    {
        // ⚠️ THE SAFETY PROPERTY FIRST. Every existing caller builds an
        // LT3DTexture by assigning four fields and nothing else. If m_nMips
        // were stack garbage the sampler would index an array of junk
        // pointers, and this engine has paid for that family of bug five times
        // already. The constructor is what prevents it, so it is tested.
        LT3DTexture fresh;
        check(fresh.m_nMips == 0 && fresh.m_pMipRGBA[0] == 0,
              "a default-constructed texture declares NO mip chain");

        // Two levels that disagree completely: 8x8 pure red, 1x1 pure blue.
        // Which one a pixel samples is then unambiguous from its colour.
        const int TW = 8, TH = 8;
        std::vector<unsigned char> lod0((size_t)TW * TH * 4);
        for (int i = 0; i < TW * TH; ++i)
        { lod0[i*4+0] = 255; lod0[i*4+1] = 0; lod0[i*4+2] = 0; lod0[i*4+3] = 255; }

        std::vector<unsigned char> lod3(4);
        lod3[0] = 0; lod3[1] = 0; lod3[2] = 255; lod3[3] = 255;

        LT3DTexture mt;
        mt.m_pRGBA = &lod0[0]; mt.m_Width = TW; mt.m_Height = TH;
        mt.m_pMipRGBA[0] = &lod0[0]; mt.m_MipW[0] = TW; mt.m_MipH[0] = TH;
        mt.m_pMipRGBA[1] = &lod3[0]; mt.m_MipW[1] = 1;  mt.m_MipH[1] = 1;
        mt.m_nMips = 2;

        // MAGNIFIED: one texel covers many pixels, so level 0 must win.
        Buf big(64, 64, false);
        LT3D::DrawTriangle(big.t, &mt, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 1,0), V(1,1,0,1, 1,1), false);
        check(big.rgba[0*4+0] > 200 && big.rgba[0*4+2] < 60,
              "a magnified triangle samples mip 0");

        // MINIFIED: the same 0..1 range crammed into a few pixels, and the
        // texture repeated 64 times on top of that -- far past one texel per
        // pixel, so the coarse level must win.
        Buf small(4, 4, false);
        LT3D::DrawTriangle(small.t, &mt, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 64,0), V(1,1,0,1, 64,64), false);
        bool bBlue = false;
        for (size_t i = 0; i < small.rgba.size(); i += 4)
            if (small.rgba[i+2] > 200 && small.rgba[i+0] < 60) bBlue = true;
        check(bBlue, "and a heavily minified one drops to the coarse level");

        // ⚠️ THE COMPATIBILITY CLAIM, TESTED RATHER THAN ASSERTED. The frame
        // contract's reference frames predate mips, so a texture WITHOUT a
        // chain must produce the identical buffer either way.
        LT3DTexture plain;
        plain.m_pRGBA = &lod0[0]; plain.m_Width = TW; plain.m_Height = TH;

        LT3DTexture chained = mt;
        chained.m_nMips = 0;            // chain present but not declared

        Buf x(16, 16, false), y(16, 16, false);
        LT3D::DrawTriangle(x.t, &plain,   V(-1,-1,0,1, 0,0), V(1,-1,0,1, 4,0), V(1,1,0,1, 4,4), false);
        LT3D::DrawTriangle(y.t, &chained, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 4,0), V(1,1,0,1, 4,4), false);
        check(x.rgba == y.rgba,
              "a texture that declares no chain samples EXACTLY as before");
    }

    printf("\n--- affine vertex colour ---\n");
    {
        // ⚠️ The default must be PERSPECTIVE, because the frame contract's
        // reference frames were rendered that way and every existing caller
        // builds an LT3DTarget by assigning four fields and nothing else.
        LT3DTarget fresh;
        check(fresh.m_bAffineColour == false,
              "a default-constructed target shades perspective-correctly");

        // A triangle stretching far into the distance -- one vertex close, one
        // far -- is where the two disagree. On a flat-on triangle they do not.
        const int W = 48, H = 48;
        Buf persp(W, H, true), affine(W, H, true);
        affine.t.m_bAffineColour = true;

        for (int pass = 0; pass < 2; ++pass)
        {
            Buf& tgt = pass ? affine : persp;
            LT3D::ClearDepth(tgt.t);
            LT3D::ClearColor(tgt.t, 0x000000);
            LT3D::DrawTriangle(tgt.t, 0,
                V(-1, -1, 0,   1.0f, 0, 0,  1, 0, 0),
                V( 1, -1, 0,   1.0f, 0, 0,  0, 1, 0),
                V( 0,  1, 0, 200.0f, 0, 0,  0, 0, 1), false);
        }
        check(persp.rgba != affine.rgba,
              "and an oblique triangle shades differently when affine is asked for");

        // ⚠️ On a triangle with equal w they MUST agree -- otherwise the affine
        // path is not an interpolation of the same values, it is a different
        // shading model, and models would change too.
        Buf p2(W, H, true), a2(W, H, true);
        a2.t.m_bAffineColour = true;
        for (int pass = 0; pass < 2; ++pass)
        {
            Buf& tgt = pass ? a2 : p2;
            LT3D::ClearDepth(tgt.t);
            LT3D::ClearColor(tgt.t, 0x000000);
            LT3D::DrawTriangle(tgt.t, 0,
                V(-1, -1, 0, 1.0f, 0, 0,  1, 0, 0),
                V( 1, -1, 0, 1.0f, 0, 0,  0, 1, 0),
                V( 0,  1, 0, 1.0f, 0, 0,  0, 0, 1), false);
        }
        check(p2.rgba == a2.rgba,
              "and identically when every vertex shares one w");
    }

    // ================= anisotropic filtering ========================= //
    printf("\n--- anisotropic filtering ---\n");
    {
        LT3DTarget fresh;
        check(fresh.m_nMaxAniso == 1,
              "a default-constructed target takes ONE sample (no anisotropy)");

        // A checkerboard, with a real box-filtered mip chain under it. The
        // checker matters: it is the highest-frequency content a texture can
        // carry, so whatever detail survives to the screen is unambiguous.
        const int TW = 64;
        std::vector<std::vector<unsigned char> > mips;
        mips.push_back(std::vector<unsigned char>((size_t)TW*TW*4));
        for (int y = 0; y < TW; ++y)
            for (int x = 0; x < TW; ++x)
            {
                unsigned char c = ((x ^ y) & 1) ? 255 : 0;
                unsigned char* q = &mips[0][((size_t)y*TW + x)*4];
                q[0] = q[1] = q[2] = c; q[3] = 255;
            }
        for (int w = TW/2, l = 1; w >= 1; w /= 2, ++l)
        {
            mips.push_back(std::vector<unsigned char>((size_t)w*w*4));
            const std::vector<unsigned char>& pr = mips[l-1];
            int pw = w*2;
            for (int y = 0; y < w; ++y)
                for (int x = 0; x < w; ++x)
                    for (int k = 0; k < 4; ++k)
                    {
                        int a = pr[((size_t)(y*2  )*pw + x*2  )*4 + k];
                        int b = pr[((size_t)(y*2  )*pw + x*2+1)*4 + k];
                        int c = pr[((size_t)(y*2+1)*pw + x*2  )*4 + k];
                        int d = pr[((size_t)(y*2+1)*pw + x*2+1)*4 + k];
                        mips[l][((size_t)y*w + x)*4 + k] = (unsigned char)((a+b+c+d)/4);
                    }
        }

        LT3DTexture mt;
        mt.m_pRGBA = &mips[0][0]; mt.m_Width = TW; mt.m_Height = TW;
        mt.m_nMips = (int)mips.size();
        for (int l = 0, w = TW; l < mt.m_nMips; ++l, w /= 2)
        { mt.m_pMipRGBA[l] = &mips[l][0]; mt.m_MipW[l] = w; mt.m_MipH[l] = w; }

        // ⚠️ A STEEPLY RECEDING QUAD -- a floor, not a wall facing you. This is
        // the case the whole feature is about: the footprint is a long thin
        // ellipse, and choosing one mip for it means choosing for the LONG axis
        // and blurring the short one by the same factor.
        const int W = 96, H = 96;
        float P[16], Vm[16], MVP[16];
        float eye[3] = { 0, 12, 0 }, at[3] = { 0, 0, -60 }, up[3] = { 0, 1, 0 };
        LT3D::Perspective(1.2f, 1.0f, 1.0f, 4000.0f, P);
        LT3D::LookAt(eye, at, up, Vm);
        LT3D::Multiply(P, Vm, MVP);

        // ⚠️ THE DEPTH RANGE AND THE UV REPEAT ARE BOTH TUNED, and they have to
        // be: run the floor out to 1200 units with a 90x repeat and every pixel
        // lands on the LAST mip -- a 1x1 grey -- so both passes are flat and
        // the test compares nothing. The LOD has to sit INSIDE the chain for
        // the choice of level to be observable at all.
        float fp[4][3] = { { -40, 0,  -20 }, {  40, 0,  -20 },
                           {  40, 0, -260 }, { -40, 0, -260 } };
        float fuv[4][2] = { { 0, 0 }, { 3, 0 }, { 3, 10 }, { 0, 10 } };

        double fDetail[2] = { 0, 0 };
        int    nPix[2] = { 0, 0 };
        std::vector<unsigned char> keep[2];

        for (int nPass = 0; nPass < 2; ++nPass)
        {
            Buf f(W, H, true);
            LT3D::ClearDepth(f.t);
            f.t.m_bBilinear  = true;
            f.t.m_nMaxAniso  = nPass ? 4 : 1;

            LT3DVert q[4];
            for (int i = 0; i < 4; ++i)
            {
                LT3D::TransformPoint(MVP, fp[i], q[i].m_Clip);
                q[i].m_U = fuv[i][0]; q[i].m_V = fuv[i][1];
                q[i].m_R = q[i].m_G = q[i].m_B = 1.0f;
            }
            nPix[nPass]  = LT3D::DrawTriangle(f.t, &mt, q[0], q[1], q[2], false);
            nPix[nPass] += LT3D::DrawTriangle(f.t, &mt, q[0], q[2], q[3], false);

            // Detail = mean absolute horizontal difference between neighbours.
            // A blurred floor tends to a flat grey and scores near zero.
            double d = 0; int n = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 1; x < W; ++x)
                {
                    const unsigned char* a = f.At(x-1, y);
                    const unsigned char* b = f.At(x,   y);
                    if (!a[3] || !b[3]) continue;
                    d += (a[0] > b[0]) ? (a[0]-b[0]) : (b[0]-a[0]);
                    ++n;
                }
            fDetail[nPass] = n ? d / n : 0.0;
            keep[nPass] = f.rgba;
        }

        printf("        receding floor: isotropic detail %.2f, aniso4 %.2f\n",
               fDetail[0], fDetail[1]);
        check(nPix[0] > 500 && nPix[1] > 500, "the receding floor actually drew");
        check(fDetail[1] > fDetail[0] * 1.15,
              "anisotropy recovers detail an isotropic mip choice blurs away");

        // ⚠️ NEGATIVE CONTROL, AND IT IS THE STRUCTURAL ONE. On a surface facing
        // the camera the footprint is round, the major and minor axes are
        // equal, and the tap count must fall back to exactly one -- so the
        // image must be IDENTICAL, not merely similar. If this ever differs,
        // anisotropy is being applied where it was never asked for, and the
        // "off by default" guarantee is not real.
        Buf h1(W, H, false), h4(W, H, false);
        h1.t.m_bBilinear = true; h1.t.m_nMaxAniso = 1;
        h4.t.m_bBilinear = true; h4.t.m_nMaxAniso = 4;
        for (int k = 0; k < 2; ++k)
        {
            Buf& b = k ? h4 : h1;
            LT3D::DrawTriangle(b.t, &mt, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 4,0), V(1,1,0,1, 4,4), false);
            LT3D::DrawTriangle(b.t, &mt, V(-1,-1,0,1, 0,0), V(1,1,0,1, 4,4), V(-1,1,0,1, 0,4), false);
        }
        check(h1.rgba == h4.rgba,
              "NEGATIVE CONTROL: a head-on surface is bit-identical either way");
    }

    // ================= trilinear filtering =========================== //
    printf("\n--- trilinear filtering ---\n");
    {
        LT3DTarget fresh;
        check(fresh.m_bTrilinear == false,
              "a default-constructed target does NOT blend mip levels");

        // ⚠️ LEVELS THAT DISAGREE COMPLETELY -- red, green, blue -- so which
        // level a pixel took is unambiguous from its colour, and a BLEND is
        // equally unambiguous. Measuring this against real level content is
        // what failed: on an actual corridor the row-to-row differences are
        // dominated by archways and mouldings, and the mip seam is a small term
        // inside a much larger signal. A synthetic chain isolates it.
        const int TW = 32;
        std::vector<std::vector<unsigned char> > lv;
        const unsigned char kCol[3][3] = { {255,0,0}, {0,255,0}, {0,0,255} };
        for (int l = 0, w = TW; l < 3; ++l, w /= 2)
        {
            lv.push_back(std::vector<unsigned char>((size_t)w*w*4));
            for (int i = 0; i < w*w; ++i)
            {
                unsigned char* q = &lv[l][(size_t)i*4];
                q[0] = kCol[l][0]; q[1] = kCol[l][1]; q[2] = kCol[l][2]; q[3] = 255;
            }
        }
        LT3DTexture mt;
        mt.m_pRGBA = &lv[0][0]; mt.m_Width = TW; mt.m_Height = TW;
        mt.m_nMips = 3;
        for (int l = 0, w = TW; l < 3; ++l, w /= 2)
        { mt.m_pMipRGBA[l] = &lv[l][0]; mt.m_MipW[l] = w; mt.m_MipH[l] = w; }

        const int W = 64, H = 96;
        float P[16], Vm[16], MVP[16];
        float eye[3] = { 0, 10, 0 }, at[3] = { 0, 0, -60 }, up[3] = { 0, 1, 0 };
        LT3D::Perspective(1.2f, (float)W/(float)H, 1.0f, 4000.0f, P);
        LT3D::LookAt(eye, at, up, Vm);
        LT3D::Multiply(P, Vm, MVP);

        float fp[4][3] = { { -40, 0,  -12 }, {  40, 0,  -12 },
                           {  40, 0, -400 }, { -40, 0, -400 } };
        float fuv[4][2] = { { 0, 0 }, { 4, 0 }, { 4, 40 }, { 0, 40 } };

        double fStep[2] = { 0, 0 };
        int    nShades[2] = { 0, 0 };

        for (int nPass = 0; nPass < 2; ++nPass)
        {
            Buf f(W, H, true);
            LT3D::ClearDepth(f.t);
            f.t.m_bTrilinear = (nPass == 1);

            LT3DVert q[4];
            for (int i = 0; i < 4; ++i)
            {
                LT3D::TransformPoint(MVP, fp[i], q[i].m_Clip);
                q[i].m_U = fuv[i][0]; q[i].m_V = fuv[i][1];
                q[i].m_R = q[i].m_G = q[i].m_B = 1.0f;
            }
            LT3D::DrawTriangle(f.t, &mt, q[0], q[1], q[2], false);
            LT3D::DrawTriangle(f.t, &mt, q[0], q[2], q[3], false);

            // Row means of the RED channel sweep 255 -> 0 as the LOD advances.
            // Nearest selection makes that a STAIRCASE; blending makes it a ramp.
            std::vector<double> rows;
            for (int y = 0; y < H; ++y)
            {
                double t = 0; int n = 0;
                for (int x = 0; x < W; ++x)
                { const unsigned char* q2 = f.At(x, y); if (q2[3]) { t += q2[0]; ++n; } }
                if (n) rows.push_back(t / n);
            }
            double fMax = 0;
            for (size_t i = 1; i < rows.size(); ++i)
            {
                double d = rows[i] > rows[i-1] ? rows[i]-rows[i-1] : rows[i-1]-rows[i];
                if (d > fMax) fMax = d;
            }
            fStep[nPass] = fMax;

            std::set<int> shades;
            for (size_t i = 0; i < f.rgba.size(); i += 4)
                if (f.rgba[i+3]) shades.insert(f.rgba[i]);
            nShades[nPass] = (int)shades.size();
        }

        printf("        worst level-to-level step: nearest %.1f, trilinear %.1f\n",
               fStep[0], fStep[1]);
        printf("        distinct red values:       nearest %d, trilinear %d\n",
               nShades[0], nShades[1]);

        check(fStep[1] < fStep[0] * 0.5,
              "blending turns the mip seam from a step into a ramp");
        check(nShades[1] > nShades[0] * 3,
              "and the levels become a gradient instead of flat bands");

        // ⚠️ NEGATIVE CONTROL: with no mip chain there is no second level, so
        // the flag must do NOTHING -- bit-identical, not merely similar.
        LT3DTexture flat;
        flat.m_pRGBA = &lv[0][0]; flat.m_Width = TW; flat.m_Height = TW;
        Buf n0(W, H, false), n1(W, H, false);
        n1.t.m_bTrilinear = true;
        for (int k = 0; k < 2; ++k)
        {
            Buf& b = k ? n1 : n0;
            LT3D::DrawTriangle(b.t, &flat, V(-1,-1,0,1, 0,0), V(1,-1,0,1, 4,0), V(1,1,0,1, 4,4), false);
        }
        check(n0.rgba == n1.rgba,
              "NEGATIVE CONTROL: with no mip chain the flag changes nothing");
    }

    printf("\n--- the far clip plane (FarZ) ---\n");
    {
        // A full-screen quad receding from w = 50 at the left edge to w = 150
        // at the right. 1/w is linear in screen x, so w = 100 falls at 3/4 of
        // the width: with FarZ 100 exactly the left three quarters may draw.
        const int W = 64, H = 8;
        LT3DVert tl = V(-50, -50, 0, 50),  bl = V(-50, 50, 0, 50);
        LT3DVert tr = V(150, -150, 0, 150), br = V(150, 150, 0, 150);
        Buf off(W, H, true), on(W, H, true), past(W, H, true);
        on.t.m_fFarW = 100.0f;
        past.t.m_fFarW = 40.0f;
        for (int k = 0; k < 3; ++k)
        {
            Buf& b = k == 0 ? off : (k == 1 ? on : past);
            LT3D::DrawTriangle(b.t, 0, tl, tr, br, false);
            LT3D::DrawTriangle(b.t, 0, tl, br, bl, false);
        }
        check(off.Lit() == W * H,
              "NEGATIVE CONTROL: m_fFarW 0 = no far clip, the whole quad draws");
        bool bBeyond = false, bDepth = true;
        int nCols = 0;
        for (int x = 0; x < W; ++x)
        {
            const bool bLit = on.At(x, H / 2)[0] != 0;
            if (bLit) ++nCols;
            for (int y = 0; y < H; ++y)
            {
                const size_t i = (size_t)y * W + x;
                if (on.rgba[i * 4] && 1.0f / on.depth[i] > 100.0f + 1e-3f) bBeyond = true;
                if (!on.rgba[i * 4] && on.depth[i] != 0.0f) bDepth = false;
            }
        }
        check(!bBeyond, "FarZ 100: no drawn pixel lies farther than 100");
        check(nCols >= W * 3 / 4 - 1 && nCols <= W * 3 / 4 + 1,
              "FarZ 100: the near three quarters draw (w = 100 at 3/4 width)");
        check(bDepth, "a far-clipped pixel writes no depth either");
        check(past.Lit() == 0, "FarZ nearer than the whole quad: nothing draws");
    }

    printf("\n--- determinism ---\n");
    {
        // ⚠️ The whole reason this is software: the frame contract gates at
        // 0.000 mean absolute difference, which requires the same input to
        // produce the identical buffer every time.
        const int W = 40, H = 24;
        Buf a(W, H, true), b(W, H, true);
        for (int pass = 0; pass < 2; ++pass)
        {
            Buf& tgt = pass ? b : a;
            LT3D::ClearDepth(tgt.t);
            LT3D::ClearColor(tgt.t, 0x101820);
            LT3D::DrawTriangle(tgt.t, 0, V(-1,-1,0,1,0,0, 1,0,0), V(1,-1,0,1,0,0, 0,1,0), V(0.3f,1,0,2,0,0, 0,0,1), false);
            LT3D::DrawTriangle(tgt.t, 0, V(-0.8f,0.9f,0,1.5f,0,0, 1,1,0), V(0.9f,0.2f,0,1.1f,0,0, 0,1,1), V(-1,-0.4f,0,1,0,0, 1,0,1), false);
        }
        check(a.rgba == b.rgba && a.depth == b.depth,
              "two identical runs produce byte-identical colour and depth");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
