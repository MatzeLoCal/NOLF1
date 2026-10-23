// ----------------------------------------------------------------------- //
//
// MODULE  : lt2d.h
//
// PURPOSE : The 2D compositor -- the blits behind ILTClient's surface API.
//
//           This is the first part of the engine that actually DRAWS. Up to
//           now every draw call was a declared no-op (ltnullrender.h category
//           2); these are the real thing.
//
// ⚠️ DELIBERATELY FREE OF ENGINE DEPENDENCIES
//
//   Nothing here includes lithtech.h, the file manager, or the record. It
//   operates on {pixels, width, height} and plain integers, which means it can
//   be unit-tested on synthetic buffers with exact expected values -- no game
//   data, no archives, no host. The whole point of doing the compositor before
//   a window is that it is checkable in isolation; wiring it to engine types
//   here would have thrown that away.
//
// PIXEL FORMAT
//
//   RGBA8888, tightly packed, top-down. Same as CLTPcx produces, so a decoded
//   bitmap is already in the blitter's format and no conversion sits between
//   loading an image and drawing it.
//
// ⚠️ HLTCOLOR IS 0x00RRGGBB, WITH BIT 31 AS A FLAG
//
//   ltbasedefs.h:109 -- `SETRGB(r,g,b)` packs `(r<<16)|(g<<8)|b`, and
//   `SETRGB_T` additionally sets `COLOR_TRANSPARENCY_MASK` (0x80000000). The
//   game's transparent colour is `SETRGB_T(255,0,255)` = 0x80FF00FF
//   (GameClientShell.cpp:1081).
//
//   So a colour-key comparison MUST mask bit 31 off first. Comparing the raw
//   uint32 against a pixel's packed RGB never matches, and the symptom is not a
//   crash -- it is every transparent blit drawing its magenta background, which
//   looks like an art bug rather than a code one.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_2D_H__
#define __LT_2D_H__

// A mutable view over a pixel buffer. Does not own the memory.
struct LT2DSurf
{
    unsigned char*  m_pRGBA;
    int             m_Width;
    int             m_Height;
};

// An integer rectangle, matching LTRect's [left,top,right,bottom) convention.
struct LT2DRect
{
    int left, top, right, bottom;
};

namespace LT2D
{
    // 0x00RRGGBB, bit 31 masked off.
    inline unsigned int ColorRGB(unsigned int c) { return c & 0x00FFFFFFu; }

    inline unsigned char ColorR(unsigned int c) { return (unsigned char)((c >> 16) & 0xFF); }
    inline unsigned char ColorG(unsigned int c) { return (unsigned char)((c >>  8) & 0xFF); }
    inline unsigned char ColorB(unsigned int c) { return (unsigned char)( c        & 0xFF); }

    // Fills pRect (or the whole surface if pRect is null) with a solid colour.
    void FillRect(const LT2DSurf& dst, const LT2DRect* pRect, unsigned int color);

    // Copies pSrcRect (or all of src) to (destX, destY).
    //
    //   bKey     -- skip source pixels whose RGB equals `key`
    //   fAlpha   -- 0..1, blended per pixel; 1.0 takes an opaque fast path
    //
    // Everything is clipped against both surfaces; coordinates outside either
    // one are legal and simply contribute nothing.
    void Blit(const LT2DSurf& dst, const LT2DSurf& src,
              const LT2DRect* pSrcRect, int destX, int destY,
              bool bKey, unsigned int key, float fAlpha);

    // ⚠️ THE OPTIMIZED-2D BLEND MODES, AND WHY TEXT WAS INVISIBLE WITHOUT THEM.
    //
    // `ILTClient::SetOptimized2DBlend` and `SetOptimized2DColor` were stubs, so
    // every 2D blit was an opaque copy. NOLF draws a line of mission text in
    // TWO passes over the same glyph surface (MissionText.cpp:128):
    //
    //     SetOptimized2DBlend(LTSURFACEBLEND_MASK)   -- punch the shadow
    //     DrawSurfaceToSurface(... x+1, y+1)
    //     SetOptimized2DBlend(LTSURFACEBLEND_ADD)    -- add the glyphs
    //     DrawSurfaceToSurface(... x,   y)
    //
    // Ignoring both modes turns that into two opaque blits of a surface whose
    // background is BLACK -- so the text box paints a black rectangle over the
    // scene and the second pass paints it again. On a letterboxed cinematic,
    // black on black: the subtitles are drawn and cannot be seen.
    //
    // ⚠️ MIRRORED, NOT INCLUDED. This header is deliberately free of engine
    // dependencies (see the note at the top) so the compositor stays unit-
    // testable on synthetic buffers, and pulling in ltbasedefs.h for one enum
    // would throw that away.
    //
    // ⚠️ THE COPY IS LOCKED TO THE ORIGINAL BY static_assert IN THE CALLER --
    // ltnullrender.cpp includes both and checks every value. A mirrored enum
    // kept honest by a comment is a silent renumbering waiting to happen; this
    // one cannot drift without failing the build.
    enum EBlend
    {
        kBlend_Alpha = 0,   // lerp(src, dest, alpha)
        kBlend_Solid,       // src
        kBlend_Add,         // src + dest
        kBlend_Multiply,    // src * dest
        kBlend_Multiply2,   // src * dest * 2
        kBlend_Mask,        // (1 - src) * dest
        kBlend_MaskAdd      // src + (1 - src) * dest
    };

    // `nBlend` is an EBlend. `modulate` is SetOptimized2DColor, multiplied into
    // the source (0x00FFFFFF is a no-op).
    void BlitBlend(const LT2DSurf& dst, const LT2DSurf& src,
                   const LT2DRect* pSrcRect, int destX, int destY,
                   bool bKey, unsigned int key, float fAlpha,
                   int nBlend, unsigned int modulate);

    // As Blit, but every non-keyed source pixel is written as `fill`.
    // This is DrawSurfaceSolidColor -- how the UI tints an icon.
    void BlitSolidColor(const LT2DSurf& dst, const LT2DSurf& src,
                        const LT2DRect* pSrcRect, int destX, int destY,
                        unsigned int key, unsigned int fill, float fAlpha);

    // Nearest-neighbour scale of pSrcRect into pDestRect.
    //
    // ⚠️ NEAREST, NOT BILINEAR, and that is a decision rather than laziness.
    // The original ran this through D3D, so pixel-exact agreement with it is
    // not achievable on any path; what IS achievable is agreement with our own
    // archived reference frames, and nearest-neighbour is exactly reproducible
    // while a filtered path invites rounding drift between builds. The frame
    // contract is only worth having if the renderer is deterministic.
    void Scale(const LT2DSurf& dst, const LT2DRect* pDestRect,
               const LT2DSurf& src, const LT2DRect* pSrcRect,
               bool bKey, unsigned int key, float fAlpha);

    void ScaleSolidColor(const LT2DSurf& dst, const LT2DRect* pDestRect,
                         const LT2DSurf& src, const LT2DRect* pSrcRect,
                         unsigned int key, unsigned int fill, float fAlpha);

    // Single pixels. Out-of-range coordinates are ignored / read as 0.
    void         SetPixel(const LT2DSurf& dst, int x, int y, unsigned int color);
    unsigned int GetPixel(const LT2DSurf& src, int x, int y);

    // The bounding box of everything that is NOT `color`, which is what
    // ILTClient::GetBorderSize reports. Returns false if the whole surface is
    // that colour.
    bool BorderSize(const LT2DSurf& src, unsigned int color, LT2DRect& out);
}

#endif // __LT_2D_H__
