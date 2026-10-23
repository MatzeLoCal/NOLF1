// ----------------------------------------------------------------------- //
//
// enginetest_2d.cpp -- the 2D compositor, on synthetic buffers.
//
// WHY THIS ONE NEEDS NO GAME DATA
//
//   lt2d is deliberately free of engine dependencies, so every case here is a
//   handful of pixels with an EXACTLY known answer. That matters more than it
//   sounds: a blitter is the kind of code where "it looks right" is the only
//   check most projects ever apply, and a one-pixel seam or an off-by-one clip
//   survives that indefinitely.
//
//   Each check is paired with one that must FAIL.
//
// ----------------------------------------------------------------------- //

#include "lt2d.h"

#include <stdio.h>
#include <string.h>
#include <vector>

static int g_nPass = 0;
static int g_nFail = 0;

static void ok(const char* w)            { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)           { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w) { b ? ok(w) : bad(w); }

#define RGB(r,g,b)  (((unsigned int)(r)<<16)|((unsigned int)(g)<<8)|(unsigned int)(b))
#define MAGENTA_T   (RGB(255,0,255) | 0x80000000u)   // SETRGB_T(255,0,255)

struct Buf
{
    std::vector<unsigned char> px;
    LT2DSurf s;
    Buf(int w, int h, unsigned int fill = 0)
    {
        px.assign((size_t)w * h * 4, 0);
        s.m_pRGBA = &px[0]; s.m_Width = w; s.m_Height = h;
        LT2D::FillRect(s, 0, fill);
    }
    unsigned int At(int x, int y) const { return LT2D::GetPixel(s, x, y); }
    int CountOf(unsigned int c) const
    {
        int n = 0;
        for (int y = 0; y < s.m_Height; ++y)
            for (int x = 0; x < s.m_Width; ++x)
                if (LT2D::GetPixel(s, x, y) == c) ++n;
        return n;
    }
};


int main()
{
    // --- FillRect ------------------------------------------------------ //
    {
        Buf d(10, 10, RGB(0,0,0));
        LT2DRect r = { 2, 3, 5, 7 };                 // 3 wide, 4 tall
        LT2D::FillRect(d.s, &r, RGB(10,20,30));

        check(d.At(2,3) == RGB(10,20,30), "FillRect writes the top-left corner");
        check(d.At(4,6) == RGB(10,20,30), "FillRect writes the bottom-right corner");
        // ⚠️ [left,top,right,bottom) -- right/bottom are EXCLUSIVE. An inclusive
        // implementation passes both checks above and paints one row too many.
        check(d.At(5,3) == RGB(0,0,0), "NEGATIVE CONTROL: right edge is exclusive");
        check(d.At(2,7) == RGB(0,0,0), "NEGATIVE CONTROL: bottom edge is exclusive");
        check(d.CountOf(RGB(10,20,30)) == 12, "exactly 3x4 = 12 pixels filled");
    }

    // --- clipping ------------------------------------------------------ //
    {
        Buf d(8, 8, RGB(0,0,0));
        Buf s(4, 4, RGB(1,2,3));

        LT2D::Blit(d.s, s.s, 0, -2, -2, false, 0, 1.0f);   // off the top-left
        check(d.At(0,0) == RGB(1,2,3), "a blit clipped at the top-left still lands");
        check(d.CountOf(RGB(1,2,3)) == 4, "and contributes exactly the 2x2 that survives");

        Buf d2(8, 8, RGB(0,0,0));
        LT2D::Blit(d2.s, s.s, 0, 6, 6, false, 0, 1.0f);    // off the bottom-right
        check(d2.CountOf(RGB(1,2,3)) == 4, "clipping at the bottom-right is symmetric");

        Buf d3(8, 8, RGB(0,0,0));
        LT2D::Blit(d3.s, s.s, 0, 100, 100, false, 0, 1.0f);
        check(d3.CountOf(RGB(1,2,3)) == 0,
              "NEGATIVE CONTROL: a fully off-surface blit draws nothing");
    }

    // --- the colour key ------------------------------------------------ //
    {
        Buf d(4, 4, RGB(0,0,0));
        Buf s(4, 4, RGB(255,0,255));                 // all magenta
        LT2D::SetPixel(s.s, 1, 1, RGB(9,9,9));       // one real pixel

        // ⚠️ THE KEY CARRIES BIT 31. Passing MAGENTA_T unmasked is exactly what
        // the game does; a comparison that forgets to mask it matches nothing
        // and the whole magenta background gets drawn.
        LT2D::Blit(d.s, s.s, 0, 0, 0, true, MAGENTA_T, 1.0f);

        check(d.At(1,1) == RGB(9,9,9), "the non-keyed pixel is drawn");
        check(d.CountOf(RGB(255,0,255)) == 0,
              "NEGATIVE CONTROL: no keyed pixel was drawn (bit 31 was masked)");

        // And the control in the other direction: without keying, it all lands.
        Buf d2(4, 4, RGB(0,0,0));
        LT2D::Blit(d2.s, s.s, 0, 0, 0, false, MAGENTA_T, 1.0f);
        check(d2.CountOf(RGB(255,0,255)) == 15,
              "NEGATIVE CONTROL: without keying the magenta IS drawn");
    }

    // --- alpha --------------------------------------------------------- //
    {
        Buf d(2, 2, RGB(0,0,0));
        Buf s(2, 2, RGB(255,255,255));

        LT2D::Blit(d.s, s.s, 0, 0, 0, false, 0, 0.5f);
        // 255*128 + 0*127 / 255 = 128
        check(d.At(0,0) == RGB(128,128,128), "50% alpha blends to exactly 128");

        Buf d2(2, 2, RGB(0,0,0));
        LT2D::Blit(d2.s, s.s, 0, 0, 0, false, 0, 0.0f);
        check(d2.At(0,0) == RGB(0,0,0),
              "NEGATIVE CONTROL: zero alpha draws nothing at all");
    }

    // --- solid colour -------------------------------------------------- //
    {
        Buf d(4, 4, RGB(0,0,0));
        Buf s(4, 4, RGB(255,0,255));
        LT2D::SetPixel(s.s, 2, 2, RGB(7,7,7));

        LT2D::BlitSolidColor(d.s, s.s, 0, 0, 0, MAGENTA_T, RGB(1,2,3), 1.0f);
        check(d.At(2,2) == RGB(1,2,3),
              "DrawSurfaceSolidColor replaces the pixel with the fill colour");
        check(d.CountOf(RGB(1,2,3)) == 1, "and only where the source was not keyed");
    }

    // --- scaling ------------------------------------------------------- //
    {
        Buf s(2, 2, 0);
        LT2D::SetPixel(s.s, 0, 0, RGB(1,1,1));
        LT2D::SetPixel(s.s, 1, 0, RGB(2,2,2));
        LT2D::SetPixel(s.s, 0, 1, RGB(3,3,3));
        LT2D::SetPixel(s.s, 1, 1, RGB(4,4,4));

        Buf d(4, 4, RGB(0,0,0));
        LT2DRect dr = { 0, 0, 4, 4 };
        LT2D::Scale(d.s, &dr, s.s, 0, false, 0, 1.0f);

        check(d.At(0,0) == RGB(1,1,1) && d.At(3,3) == RGB(4,4,4),
              "a 2x2 doubled to 4x4 puts the corners right");
        check(d.CountOf(RGB(1,1,1)) == 4 && d.CountOf(RGB(4,4,4)) == 4,
              "each source pixel becomes exactly 2x2 -- nearest, not filtered");

        // ⚠️ THE ONE THAT CATCHES THE SQUASH BUG. The destination runs off the
        // right edge, so it must be CROPPED, not resized to fit. If the source
        // step were derived from the clipped width, the left half would show
        // the whole image instead of the left half of it.
        Buf d2(4, 4, RGB(0,0,0));
        LT2DRect dr2 = { 0, 0, 8, 4 };            // twice as wide as the surface
        LT2D::Scale(d2.s, &dr2, s.s, 0, false, 0, 1.0f);
        check(d2.At(3,0) == RGB(1,1,1),
              "NEGATIVE CONTROL: a clipped scale CROPS, it does not squash");
    }

    // --- border size --------------------------------------------------- //
    {
        Buf s(8, 8, RGB(0,0,0));
        LT2D::SetPixel(s.s, 2, 3, RGB(5,5,5));
        LT2D::SetPixel(s.s, 5, 6, RGB(5,5,5));

        LT2DRect r;
        check(LT2D::BorderSize(s.s, RGB(0,0,0), r), "BorderSize finds content");
        check(r.left == 2 && r.top == 3 && r.right == 6 && r.bottom == 7,
              "and reports the exclusive bounding box exactly");

        Buf blank(4, 4, RGB(0,0,0));
        check(!LT2D::BorderSize(blank.s, RGB(0,0,0), r),
              "NEGATIVE CONTROL: an all-background surface reports no content");
    }

    // --- degenerate input ---------------------------------------------- //
    {
        LT2DSurf nul = { 0, 0, 0 };
        Buf d(4, 4, RGB(0,0,0));
        LT2D::Blit(d.s, nul, 0, 0, 0, false, 0, 1.0f);
        LT2D::FillRect(nul, 0, RGB(1,1,1));
        LT2D::Scale(d.s, 0, nul, 0, false, 0, 1.0f);
        check(d.CountOf(RGB(0,0,0)) == 16, "null surfaces are ignored, not crashed on");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
