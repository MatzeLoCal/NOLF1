// ----------------------------------------------------------------------- //
//
// MODULE  : lt2d.cpp
//
// PURPOSE : The 2D compositor. See lt2d.h -- in particular the note about
//           masking bit 31 off HLTCOLOR before any colour-key comparison.
//
// ----------------------------------------------------------------------- //

#include "lt2d.h"

#include <string.h>

namespace
{
    // ⚠️ ONE CLIPPER, USED BY EVERYTHING.
    //
    // Clipping written per-operation is clipping written five times and wrong
    // in one of them, and an off-by-one in a blitter shows up as a one-pixel
    // seam that reads as an art problem. Every path below reduces to this.
    //
    // Narrows [srcX0,srcX1) x [srcY0,srcY1) and the destination origin so that
    // both the source region and the destination region are fully inside their
    // surfaces. Returns false when nothing survives.
    bool ClipBlit(const LT2DSurf& dst, const LT2DSurf& src,
                  int& sx, int& sy, int& w, int& h, int& dx, int& dy)
    {
        // Source rect off the left/top edge: trim, and move the destination by
        // the same amount so the two stay in step.
        if (sx < 0) { w += sx; dx -= sx; sx = 0; }
        if (sy < 0) { h += sy; dy -= sy; sy = 0; }

        if (sx + w > src.m_Width)  w = src.m_Width  - sx;
        if (sy + h > src.m_Height) h = src.m_Height - sy;

        // Now the same for the destination.
        if (dx < 0) { w += dx; sx -= dx; dx = 0; }
        if (dy < 0) { h += dy; sy -= dy; dy = 0; }

        if (dx + w > dst.m_Width)  w = dst.m_Width  - dx;
        if (dy + h > dst.m_Height) h = dst.m_Height - dy;

        return (w > 0 && h > 0);
    }

    inline unsigned int PixRGB(const unsigned char* p)
    {
        return ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2];
    }

    // dst = src*a + dst*(1-a), with a in 0..255.
    inline void BlendInto(unsigned char* d, unsigned char r,
                          unsigned char g, unsigned char b, unsigned int a)
    {
        unsigned int ia = 255 - a;
        d[0] = (unsigned char)((r * a + d[0] * ia) / 255);
        d[1] = (unsigned char)((g * a + d[1] * ia) / 255);
        d[2] = (unsigned char)((b * a + d[2] * ia) / 255);
        d[3] = 255;
    }

    inline unsigned int AlphaToByte(float f)
    {
        if (f <= 0.0f) return 0;
        if (f >= 1.0f) return 255;
        return (unsigned int)(f * 255.0f + 0.5f);
    }

    bool Valid(const LT2DSurf& s)
    {
        return s.m_pRGBA && s.m_Width > 0 && s.m_Height > 0;
    }

    // Resolves an optional rect against a surface's full extent, and clamps it.
    void ResolveRect(const LT2DSurf& s, const LT2DRect* p, LT2DRect& out)
    {
        if (p) out = *p;
        else { out.left = 0; out.top = 0; out.right = s.m_Width; out.bottom = s.m_Height; }

        if (out.left   < 0) out.left = 0;
        if (out.top    < 0) out.top  = 0;
        if (out.right  > s.m_Width)  out.right  = s.m_Width;
        if (out.bottom > s.m_Height) out.bottom = s.m_Height;
    }
}


// ----------------------------------------------------------------------- //

void LT2D::FillRect(const LT2DSurf& dst, const LT2DRect* pRect, unsigned int color)
{
    if (!Valid(dst)) return;

    LT2DRect r;
    ResolveRect(dst, pRect, r);
    if (r.right <= r.left || r.bottom <= r.top) return;

    const unsigned char cr = ColorR(color);
    const unsigned char cg = ColorG(color);
    const unsigned char cb = ColorB(color);

    for (int y = r.top; y < r.bottom; ++y)
    {
        unsigned char* d = dst.m_pRGBA + ((size_t)y * dst.m_Width + r.left) * 4;
        for (int x = r.left; x < r.right; ++x, d += 4)
        {
            d[0] = cr; d[1] = cg; d[2] = cb; d[3] = 255;
        }
    }
}


void LT2D::Blit(const LT2DSurf& dst, const LT2DSurf& src,
                const LT2DRect* pSrcRect, int destX, int destY,
                bool bKey, unsigned int key, float fAlpha)
{
    if (!Valid(dst) || !Valid(src)) return;

    LT2DRect s;
    ResolveRect(src, pSrcRect, s);

    int sx = s.left, sy = s.top;
    int w  = s.right - s.left, h = s.bottom - s.top;
    int dx = destX,  dy = destY;

    if (!ClipBlit(dst, src, sx, sy, w, h, dx, dy)) return;

    // ⚠️ Bit 31 masked off -- see lt2d.h. The game's key is 0x80FF00FF.
    const unsigned int keyRGB = ColorRGB(key);
    const unsigned int a = AlphaToByte(fAlpha);
    if (a == 0) return;

    for (int y = 0; y < h; ++y)
    {
        const unsigned char* p = src.m_pRGBA + ((size_t)(sy + y) * src.m_Width + sx) * 4;
        unsigned char*       d = dst.m_pRGBA + ((size_t)(dy + y) * dst.m_Width + dx) * 4;

        for (int x = 0; x < w; ++x, p += 4, d += 4)
        {
            if (bKey && PixRGB(p) == keyRGB) continue;

            if (a == 255) { d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255; }
            else          BlendInto(d, p[0], p[1], p[2], a);
        }
    }
}


void LT2D::BlitBlend(const LT2DSurf& dst, const LT2DSurf& src,
                     const LT2DRect* pSrcRect, int destX, int destY,
                     bool bKey, unsigned int key, float fAlpha,
                     int nBlend, unsigned int modulate)
{
    // ⚠️ ALPHA with a white modulate IS the old path, bit for bit. Routing it
    // back to Blit rather than reimplementing it keeps the frame contract's
    // blessed references valid: the default state must produce exactly the
    // pixels it produced before blend modes existed.
    if (nBlend == LT2D::kBlend_Alpha && ColorRGB(modulate) == 0x00FFFFFFu)
    {
        Blit(dst, src, pSrcRect, destX, destY, bKey, key, fAlpha);
        return;
    }

    if (!Valid(dst) || !Valid(src)) return;

    LT2DRect s;
    ResolveRect(src, pSrcRect, s);

    int sx = s.left, sy = s.top;
    int w  = s.right - s.left, h = s.bottom - s.top;
    int dx = destX,  dy = destY;

    if (!ClipBlit(dst, src, sx, sy, w, h, dx, dy)) return;

    const unsigned int keyRGB = ColorRGB(key);
    const unsigned int a = AlphaToByte(fAlpha);
    if (a == 0) return;

    const unsigned int mr = ColorR(modulate);
    const unsigned int mg = ColorG(modulate);
    const unsigned int mb = ColorB(modulate);

    for (int y = 0; y < h; ++y)
    {
        const unsigned char* p = src.m_pRGBA + ((size_t)(sy + y) * src.m_Width + sx) * 4;
        unsigned char*       d = dst.m_pRGBA + ((size_t)(dy + y) * dst.m_Width + dx) * 4;

        for (int x = 0; x < w; ++x, p += 4, d += 4)
        {
            if (bKey && PixRGB(p) == keyRGB) continue;

            // Source, modulated by SetOptimized2DColor.
            const unsigned int sr = ((unsigned int)p[0] * mr + 127) / 255;
            const unsigned int sg = ((unsigned int)p[1] * mg + 127) / 255;
            const unsigned int sb = ((unsigned int)p[2] * mb + 127) / 255;

            unsigned int rr, rg, rb;

            switch (nBlend)
            {
                case LT2D::kBlend_Solid:      // src
                    rr = sr; rg = sg; rb = sb;
                    break;

                case LT2D::kBlend_Add:        // src + dest
                    rr = sr + d[0]; rg = sg + d[1]; rb = sb + d[2];
                    break;

                case LT2D::kBlend_Multiply:   // src * dest
                    rr = (sr * d[0] + 127) / 255;
                    rg = (sg * d[1] + 127) / 255;
                    rb = (sb * d[2] + 127) / 255;
                    break;

                case LT2D::kBlend_Multiply2:  // src * dest * 2
                    rr = (sr * d[0] * 2 + 127) / 255;
                    rg = (sg * d[1] * 2 + 127) / 255;
                    rb = (sb * d[2] * 2 + 127) / 255;
                    break;

                case LT2D::kBlend_Mask:       // (1 - src) * dest
                    rr = ((255 - sr) * d[0] + 127) / 255;
                    rg = ((255 - sg) * d[1] + 127) / 255;
                    rb = ((255 - sb) * d[2] + 127) / 255;
                    break;

                case LT2D::kBlend_MaskAdd:    // src + (1 - src) * dest
                    rr = sr + ((255 - sr) * d[0] + 127) / 255;
                    rg = sg + ((255 - sg) * d[1] + 127) / 255;
                    rb = sb + ((255 - sb) * d[2] + 127) / 255;
                    break;

                default:                        // ALPHA, with a modulate
                    rr = sr; rg = sg; rb = sb;
                    break;
            }

            if (rr > 255) rr = 255;
            if (rg > 255) rg = 255;
            if (rb > 255) rb = 255;

            // ⚠️ The surface alpha still applies ON TOP of the blend, because
            // the game fades text in and out through SetSurfaceAlpha while a
            // blend mode is set. Two separate controls, both live at once.
            if (a == 255)
            {
                d[0] = (unsigned char)rr;
                d[1] = (unsigned char)rg;
                d[2] = (unsigned char)rb;
                d[3] = 255;
            }
            else
            {
                BlendInto(d, (unsigned char)rr, (unsigned char)rg,
                          (unsigned char)rb, a);
            }
        }
    }
}


void LT2D::BlitSolidColor(const LT2DSurf& dst, const LT2DSurf& src,
                          const LT2DRect* pSrcRect, int destX, int destY,
                          unsigned int key, unsigned int fill, float fAlpha)
{
    if (!Valid(dst) || !Valid(src)) return;

    LT2DRect s;
    ResolveRect(src, pSrcRect, s);

    int sx = s.left, sy = s.top;
    int w  = s.right - s.left, h = s.bottom - s.top;
    int dx = destX,  dy = destY;

    if (!ClipBlit(dst, src, sx, sy, w, h, dx, dy)) return;

    const unsigned int keyRGB = ColorRGB(key);
    const unsigned int a  = AlphaToByte(fAlpha);
    if (a == 0) return;

    const unsigned char fr = ColorR(fill), fg = ColorG(fill), fb = ColorB(fill);

    for (int y = 0; y < h; ++y)
    {
        const unsigned char* p = src.m_pRGBA + ((size_t)(sy + y) * src.m_Width + sx) * 4;
        unsigned char*       d = dst.m_pRGBA + ((size_t)(dy + y) * dst.m_Width + dx) * 4;

        for (int x = 0; x < w; ++x, p += 4, d += 4)
        {
            if (PixRGB(p) == keyRGB) continue;

            if (a == 255) { d[0] = fr; d[1] = fg; d[2] = fb; d[3] = 255; }
            else          BlendInto(d, fr, fg, fb, a);
        }
    }
}


// ----------------------------------------------------------------------- //
// Scaling.
//
// ⚠️ THE SOURCE STEP IS COMPUTED FROM THE UNCLIPPED DESTINATION RECT.
//
// If the destination is clipped against the screen edge and the step is then
// derived from what survived, the image SQUASHES to fit instead of being
// cropped -- a bug that looks like a layout problem and is invisible whenever
// nothing happens to overlap an edge.
// ----------------------------------------------------------------------- //

static void ScaleImpl(const LT2DSurf& dst, const LT2DRect* pDestRect,
                      const LT2DSurf& src, const LT2DRect* pSrcRect,
                      bool bKey, unsigned int key,
                      bool bSolid, unsigned int fill, float fAlpha)
{
    if (!dst.m_pRGBA || !src.m_pRGBA) return;
    if (dst.m_Width <= 0 || dst.m_Height <= 0) return;
    if (src.m_Width <= 0 || src.m_Height <= 0) return;

    LT2DRect s;
    if (pSrcRect) s = *pSrcRect;
    else { s.left = 0; s.top = 0; s.right = src.m_Width; s.bottom = src.m_Height; }

    if (s.left < 0) s.left = 0;
    if (s.top  < 0) s.top  = 0;
    if (s.right  > src.m_Width)  s.right  = src.m_Width;
    if (s.bottom > src.m_Height) s.bottom = src.m_Height;

    const int sw = s.right - s.left;
    const int sh = s.bottom - s.top;
    if (sw <= 0 || sh <= 0) return;

    LT2DRect dr;
    if (pDestRect) dr = *pDestRect;
    else { dr.left = 0; dr.top = 0; dr.right = dst.m_Width; dr.bottom = dst.m_Height; }

    const int dw = dr.right - dr.left;
    const int dh = dr.bottom - dr.top;
    if (dw <= 0 || dh <= 0) return;

    // Clip the destination AFTER remembering its full extent.
    int x0 = dr.left   < 0 ? 0 : dr.left;
    int y0 = dr.top    < 0 ? 0 : dr.top;
    int x1 = dr.right  > dst.m_Width  ? dst.m_Width  : dr.right;
    int y1 = dr.bottom > dst.m_Height ? dst.m_Height : dr.bottom;
    if (x1 <= x0 || y1 <= y0) return;

    const unsigned int keyRGB = LT2D::ColorRGB(key);
    unsigned int a = 255;
    if (fAlpha <= 0.0f) return;
    if (fAlpha < 1.0f)  a = (unsigned int)(fAlpha * 255.0f + 0.5f);

    const unsigned char fr = LT2D::ColorR(fill);
    const unsigned char fg = LT2D::ColorG(fill);
    const unsigned char fb = LT2D::ColorB(fill);

    for (int y = y0; y < y1; ++y)
    {
        // Nearest-neighbour, from the UNCLIPPED destination origin and size.
        int srcY = s.top + (int)(((long long)(y - dr.top) * sh) / dh);
        if (srcY < s.top) srcY = s.top;
        if (srcY >= s.bottom) srcY = s.bottom - 1;

        const unsigned char* pRow = src.m_pRGBA + (size_t)srcY * src.m_Width * 4;
        unsigned char*       d    = dst.m_pRGBA + ((size_t)y * dst.m_Width + x0) * 4;

        for (int x = x0; x < x1; ++x, d += 4)
        {
            int srcX = s.left + (int)(((long long)(x - dr.left) * sw) / dw);
            if (srcX < s.left) srcX = s.left;
            if (srcX >= s.right) srcX = s.right - 1;

            const unsigned char* p = pRow + (size_t)srcX * 4;

            unsigned int rgb = ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2];
            if ((bKey || bSolid) && rgb == keyRGB) continue;

            unsigned char r = bSolid ? fr : p[0];
            unsigned char g = bSolid ? fg : p[1];
            unsigned char b = bSolid ? fb : p[2];

            if (a == 255) { d[0] = r; d[1] = g; d[2] = b; d[3] = 255; }
            else
            {
                unsigned int ia = 255 - a;
                d[0] = (unsigned char)((r * a + d[0] * ia) / 255);
                d[1] = (unsigned char)((g * a + d[1] * ia) / 255);
                d[2] = (unsigned char)((b * a + d[2] * ia) / 255);
                d[3] = 255;
            }
        }
    }
}


void LT2D::Scale(const LT2DSurf& dst, const LT2DRect* pDestRect,
                 const LT2DSurf& src, const LT2DRect* pSrcRect,
                 bool bKey, unsigned int key, float fAlpha)
{
    ScaleImpl(dst, pDestRect, src, pSrcRect, bKey, key, false, 0, fAlpha);
}

void LT2D::ScaleSolidColor(const LT2DSurf& dst, const LT2DRect* pDestRect,
                           const LT2DSurf& src, const LT2DRect* pSrcRect,
                           unsigned int key, unsigned int fill, float fAlpha)
{
    ScaleImpl(dst, pDestRect, src, pSrcRect, true, key, true, fill, fAlpha);
}


// ----------------------------------------------------------------------- //

void LT2D::SetPixel(const LT2DSurf& dst, int x, int y, unsigned int color)
{
    if (!Valid(dst)) return;
    if (x < 0 || y < 0 || x >= dst.m_Width || y >= dst.m_Height) return;

    unsigned char* d = dst.m_pRGBA + ((size_t)y * dst.m_Width + x) * 4;
    d[0] = ColorR(color); d[1] = ColorG(color); d[2] = ColorB(color); d[3] = 255;
}

unsigned int LT2D::GetPixel(const LT2DSurf& src, int x, int y)
{
    if (!Valid(src)) return 0;
    if (x < 0 || y < 0 || x >= src.m_Width || y >= src.m_Height) return 0;

    const unsigned char* p = src.m_pRGBA + ((size_t)y * src.m_Width + x) * 4;
    return PixRGB(p);
}


bool LT2D::BorderSize(const LT2DSurf& src, unsigned int color, LT2DRect& out)
{
    if (!Valid(src)) return false;

    const unsigned int c = ColorRGB(color);

    int minX = src.m_Width, minY = src.m_Height, maxX = -1, maxY = -1;

    for (int y = 0; y < src.m_Height; ++y)
    {
        const unsigned char* p = src.m_pRGBA + (size_t)y * src.m_Width * 4;
        for (int x = 0; x < src.m_Width; ++x, p += 4)
        {
            if (PixRGB(p) == c) continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }

    if (maxX < 0) return false;             // every pixel was `color`

    out.left = minX; out.top = minY;
    out.right = maxX + 1; out.bottom = maxY + 1;
    return true;
}
