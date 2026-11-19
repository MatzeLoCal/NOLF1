// ----------------------------------------------------------------------- //
//
// MODULE  : ltsysfont_ft.cpp
//
// PURPOSE : PORT (Linux): one glyph of a system TrueType font as an
//           anti-aliased coverage bitmap, for the smooth fonts
//           (ltfont.cpp, CLTBitmapFont::BuildSmooth). The Linux twin of
//           engine/platform/ltsysfont.mm; FreeType does the rasterising.
//
//           Font: NOLF_FONT=<path to .ttf/.otf>, else the first of a short
//           list of common bold sans fonts (DejaVu, Noto, Liberation) that is
//           installed (DejaVu Sans Condensed Bold
//           first). Every one of them carries the Hungarian ő ű Ő Ű.
//
// ----------------------------------------------------------------------- //

#ifndef __APPLE__  // Linux port only

#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace
{
    FT_Library g_Lib  = 0;
    FT_Face    g_Face = 0;
    bool       g_bTried = false;
    int        g_nSize = 0;                // the pixel height last set up
    int        g_nBaseline = 0;            // from the top of the line

    bool OpenFace()
    {
        if (g_bTried) return g_Face != 0;
        g_bTried = true;
        if (FT_Init_FreeType(&g_Lib)) { g_Lib = 0; return false; }

        const char* kPaths[] =
        {
            getenv("NOLF_FONT"),
            // Condensed first: the bitmap fonts are narrow, and the menus
            // were laid out for their widths ("Vissza" beside the help box).
            "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSansCondensed-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansCondensed-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
        };
        for (const char* p : kPaths)
        {
            if (!p || !*p) continue;
            if (FT_New_Face(g_Lib, p, 0, &g_Face) == 0)
            {
                printf("[LTFONT] smooth font: %s\n", p);
                fflush(stdout);
                return true;
            }
            g_Face = 0;
        }
        // ⚠️ Loud: without it every font stays the original bitmap.
        printf("[LTFONT] ⚠️  no TrueType font found (NOLF_FONT=...); bitmap fonts kept\n");
        fflush(stdout);
        return false;
    }

    bool SetSize(int nPixelHeight)
    {
        if (g_nSize == nPixelHeight) return true;
        // Line height ~ 1.2 x the em, as the macOS version measures it.
        const int nEm = (int)floorf((float)nPixelHeight / 1.22f + 0.5f);
        if (FT_Set_Pixel_Sizes(g_Face, 0, (FT_UInt)(nEm > 1 ? nEm : 1))) return false;
        const int asc  = (int)(g_Face->size->metrics.ascender  >> 6);
        const int desc = (int)(g_Face->size->metrics.descender >> 6);   // negative
        // The ascender-to-descender box centred in the line.
        g_nBaseline = (nPixelHeight - (asc - desc)) / 2 + asc;
        g_nSize = nPixelHeight;
        return true;
    }
}

// Renders code point `cp` so that a line is `nPixelHeight` pixels tall. Fills
// `cov` (nWidth x nPixelHeight, one byte of coverage per pixel) and returns
// the advance in nWidth. False for a code point the font cannot draw.
bool LTSysFont_RasterGlyph(unsigned int cp, int nPixelHeight,
                           std::vector<unsigned char>& cov, int& nWidth)
{
    if (nPixelHeight < 6 || !OpenFace() || !SetSize(nPixelHeight)) return false;
    if (cp != ' ' && FT_Get_Char_Index(g_Face, cp) == 0) return false;
    if (FT_Load_Char(g_Face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return false;

    const FT_GlyphSlot g = g_Face->glyph;
    int w = (int)((g->advance.x + 63) >> 6);
    if (w <= 0) return false;
    // No extra column of air (the macOS version adds one): the advance
    // already carries the side bearings, and the game adds its own letter
    // spacing on top.

    cov.assign((size_t)w * nPixelHeight, 0);
    const FT_Bitmap& bm = g->bitmap;
    for (int yy = 0; yy < (int)bm.rows; ++yy)
    {
        const int dy = g_nBaseline - g->bitmap_top + yy;
        if (dy < 0 || dy >= nPixelHeight) continue;
        const unsigned char* src = bm.buffer + yy * bm.pitch;
        for (int xx = 0; xx < (int)bm.width; ++xx)
        {
            const int dx = g->bitmap_left + xx;
            if (dx < 0 || dx >= w) continue;
            unsigned char& d = cov[(size_t)dy * w + dx];
            if (src[xx] > d) d = src[xx];
        }
    }
    nWidth = w;
    return true;
}

#endif  // !__APPLE__
