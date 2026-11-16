// ----------------------------------------------------------------------- //
//
// MODULE  : ltfont.cpp
//
// PURPOSE : Bitmap fonts. See ltfont.h for the format and for the two details
//           that are easy to get backwards (space, and FIXED_END).
//
// ----------------------------------------------------------------------- //

#include "ltfont.h"

#include <stdio.h>
#include <string.h>


// The seven groups, in bitmap order. ⚠️ Order is load-bearing: nGroupFlags
// selects a prefix of this list, so reordering it silently remaps every
// character in every subset font.
namespace
{
    struct Group { unsigned int m_Flag; int m_First; int m_Count; };

    const Group kGroups[] =
    {
        { LTFONT_SYMBOLS_1,  33, 15 },
        { LTFONT_NUMBERS,    48, 10 },
        { LTFONT_SYMBOLS_2,  58,  7 },
        { LTFONT_UPPERCASE,  65, 26 },
        { LTFONT_SYMBOLS_3,  91,  6 },
        { LTFONT_LOWERCASE,  97, 26 },
        { LTFONT_SYMBOLS_4, 123,  4 },
    };
    const int kNumGroups = (int)(sizeof(kGroups) / sizeof(kGroups[0]));
}


CLTBitmapFont::CLTBitmapFont()
    : m_Height(0), m_AvgWidth(0)
{
    m_Sheet.m_pRGBA = 0;
    m_Sheet.m_Width = 0;
    m_Sheet.m_Height = 0;
    for (int i = 0; i < 256; ++i) m_CharToGlyph[i] = -1;
}


bool CLTBitmapFont::Build(const unsigned char* pRGBA, int nWidth, int nHeight,
                          unsigned int transColor, unsigned int nGroupFlags)
{
    m_Glyphs.clear();
    m_Pixels.clear();
    for (int i = 0; i < 256; ++i) m_CharToGlyph[i] = -1;
    m_Height = 0;
    m_AvgWidth = 0;

    if (!pRGBA || nWidth <= 0 || nHeight <= 0) return false;

    m_Pixels.assign(pRGBA, pRGBA + (size_t)nWidth * nHeight * 4);
    m_Sheet.m_pRGBA  = &m_Pixels[0];
    m_Sheet.m_Width  = nWidth;
    m_Sheet.m_Height = nHeight;
    m_Height = nHeight;

    const unsigned int key = LT2D::ColorRGB(transColor);

    // --- which columns are entirely transparent ----------------------- //

    std::vector<unsigned char> blank((size_t)nWidth, 1);
    for (int x = 0; x < nWidth; ++x)
    {
        for (int y = 0; y < nHeight; ++y)
        {
            const unsigned char* p = &m_Pixels[((size_t)y * nWidth + x) * 4];
            unsigned int rgb = ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2];
            if (rgb != key) { blank[x] = 0; break; }
        }
    }

    // --- runs of non-blank columns are the glyph boxes ---------------- //

    std::vector<Glyph> boxes;
    int x = 0;
    while (x < nWidth)
    {
        while (x < nWidth && blank[x]) ++x;
        if (x >= nWidth) break;

        int start = x;
        while (x < nWidth && !blank[x]) ++x;

        Glyph g;
        g.m_X = start;
        g.m_W = x - start;
        boxes.push_back(g);
    }

    if (boxes.empty()) return false;

    // --- ⚠️ FIXED_END decides where a cell ENDS ----------------------- //
    //
    // Without the flag the break sits at the MIDPOINT of the gap, so a cell is
    // wider than its ink. With it, the cell is the ink box exactly. See
    // ltfont.h -- getting this backwards shifts every centred string.

    const bool bFixedEnd = (nGroupFlags & LTFONT_FIXED_END) != 0;

    m_Glyphs.resize(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        if (bFixedEnd)
        {
            m_Glyphs[i] = boxes[i];
            continue;
        }

        int left  = boxes[i].m_X;
        int right = boxes[i].m_X + boxes[i].m_W;

        // Half of the gap on each side, where a gap exists.
        if (i > 0)
        {
            int prevEnd = boxes[i - 1].m_X + boxes[i - 1].m_W;
            left = prevEnd + (boxes[i].m_X - prevEnd) / 2;
        }
        if (i + 1 < boxes.size())
        {
            int nextStart = boxes[i + 1].m_X;
            right = right + (nextStart - right) / 2;
        }

        m_Glyphs[i].m_X = left;
        m_Glyphs[i].m_W = right - left;
    }

    // --- map characters to glyphs ------------------------------------- //

    int nExpected = 0;
    for (int g = 0; g < kNumGroups; ++g)
        if (nGroupFlags & kGroups[g].m_Flag) nExpected += kGroups[g].m_Count;

    if (nExpected == 0) nExpected = (int)m_Glyphs.size();   // flags said nothing

    // ⚠️ THE COUNT MUST MATCH EXACTLY -- BOTH DIRECTIONS.
    //
    // Too many means the separator colour is wrong and letters have been split
    // at internal gaps: lithfontdefs.h's own warning is that "you'll end up
    // with characters that look like 'half letters' and any characters after
    // the bad ones will be off on the ascii chart".
    //
    // Too few is the same disease with the opposite symptom, and the first
    // version of this check MISSED IT: with magenta as the key no column is
    // blank at all, the whole strip reads as ONE glyph, 1 <= 94 passed, and
    // every string would have rendered as a single smear mapped to '!'. The
    // test's negative control is what caught it.
    //
    // Refusing beats truncating in both directions, because the failure mode
    // of a wrong split is plausible-looking WRONG TEXT rather than no text.
    if ((int)m_Glyphs.size() != nExpected)
    {
        fprintf(stderr, "[LTFONT] ⚠️  %d glyphs but flags %#x account for %d --"
                        " wrong separator colour?\n",
                (int)m_Glyphs.size(), nGroupFlags, nExpected);
        m_Glyphs.clear();
        m_Pixels.clear();
        m_Sheet.m_pRGBA = 0;
        m_Sheet.m_Width = m_Sheet.m_Height = 0;
        m_Height = 0;
        return false;
    }

    int idx = 0;
    for (int g = 0; g < kNumGroups && idx < (int)m_Glyphs.size(); ++g)
    {
        if (!(nGroupFlags & kGroups[g].m_Flag)) continue;

        for (int c = 0; c < kGroups[g].m_Count && idx < (int)m_Glyphs.size(); ++c)
            m_CharToGlyph[kGroups[g].m_First + c] = idx++;
    }

    // Average width -- what a space advances by. See ltfont.h.
    long total = 0;
    for (size_t i = 0; i < m_Glyphs.size(); ++i) total += m_Glyphs[i].m_W;
    m_AvgWidth = (int)(total / (long)m_Glyphs.size());

    AddAccentedGlyphs(key);

    return true;
}


// ----------------------------------------------------------------------- //
//
// PORT: accented letters. The shipped strips are ASCII 33..126 only, so a
// translated string table (ltstringmgr.cpp, NOLF_LANG_FILE) would lose every
// accent. Each accented letter is built from its base glyph plus an accent
// drawn in the glyph's own ink colour, appended to the RIGHT of the strip (so
// pixel (0,0), which ltgui_FontKey samples, is still the key colour).
//
// The bytes are CP1252 for the Western letters; for Hungarian the four
// letters CP1252 lacks take CP1250's positions, which are CP1252's õ û Õ Û:
// ő 0xF5, ű 0xFB, Ő 0xD5, Ű 0xDB. The English table never uses those four.
//
// ----------------------------------------------------------------------- //

namespace
{
    enum Accent { kAcute, kDiaeresis, kDoubleAcute };

    struct AccentDef { unsigned char m_Char; unsigned char m_Base; Accent m_Accent; };

    const AccentDef kAccents[] =
    {
        { 0xE1, 'a', kAcute }, { 0xE9, 'e', kAcute }, { 0xED, 'i', kAcute },
        { 0xF3, 'o', kAcute }, { 0xFA, 'u', kAcute },
        { 0xE4, 'a', kDiaeresis }, { 0xF6, 'o', kDiaeresis }, { 0xFC, 'u', kDiaeresis },
        { 0xF5, 'o', kDoubleAcute }, { 0xFB, 'u', kDoubleAcute },
        { 0xC1, 'A', kAcute }, { 0xC9, 'E', kAcute }, { 0xCD, 'I', kAcute },
        { 0xD3, 'O', kAcute }, { 0xDA, 'U', kAcute },
        { 0xC4, 'A', kDiaeresis }, { 0xD6, 'O', kDiaeresis }, { 0xDC, 'U', kDiaeresis },
        { 0xD5, 'O', kDoubleAcute }, { 0xDB, 'U', kDoubleAcute },
    };

    // Typographic CP1252 characters the strip has no glyph for, drawn as
    // their plain ASCII look-alike.
    const unsigned char kAliases[][2] =
    {
        { 0x91, '\'' }, { 0x92, '\'' }, { 0x82, ',' }, { 0x93, '"' }, { 0x94, '"' },
        { 0x84, '"' }, { 0x96, '-' }, { 0x97, '-' }, { 0xAD, '-' }, { 0xB4, '\'' },
    };
}


void CLTBitmapFont::AddAccentedGlyphs(unsigned int key)
{
    const int H = m_Height;
    const int W0 = m_Sheet.m_Width;
    if (H < 6 || W0 <= 0) return;

    for (size_t i = 0; i < sizeof(kAliases) / sizeof(kAliases[0]); ++i)
        if (m_CharToGlyph[kAliases[i][0]] < 0 && m_CharToGlyph[kAliases[i][1]] >= 0)
            m_CharToGlyph[kAliases[i][0]] = m_CharToGlyph[kAliases[i][1]];

    auto pix = [](std::vector<unsigned char>& v, int w, int x, int y) -> unsigned char*
        { return &v[((size_t)y * w + x) * 4]; };
    auto rgb = [](const unsigned char* p) -> unsigned int
        { return ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2]; };

    // Which accents can be built: the base letter must be in this font.
    std::vector<int> todo;
    int addW = 0;
    for (int i = 0; i < (int)(sizeof(kAccents) / sizeof(kAccents[0])); ++i)
    {
        if (m_CharToGlyph[kAccents[i].m_Char] >= 0) continue;
        int b = m_CharToGlyph[kAccents[i].m_Base];
        if (b < 0) continue;
        todo.push_back(i);
        addW += m_Glyphs[b].m_W + 2;
    }
    if (todo.empty()) return;

    // A wider strip: the original on the left, the new glyphs after it.
    const int W = W0 + addW;
    std::vector<unsigned char> px((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
    {
        memcpy(&px[(size_t)y * W * 4], &m_Pixels[(size_t)y * W0 * 4], (size_t)W0 * 4);
        for (int x = W0; x < W; ++x)
        {
            unsigned char* d = pix(px, W, x, y);
            d[0] = (unsigned char)(key >> 16); d[1] = (unsigned char)(key >> 8);
            d[2] = (unsigned char)key; d[3] = 0;
        }
    }

    // Accent size scales with the font: a 14 px font gets 1 px strokes.
    const int t  = H >= 26 ? 2 : 1;             // stroke thickness
    const int ah = H >= 26 ? 4 : (H >= 18 ? 3 : 2);   // accent height
    const int gap = 1;                          // rows between accent and letter

    int penX = W0 + 1;
    for (int k : todo)
    {
        const AccentDef& a = kAccents[k];
        const Glyph& g = m_Glyphs[m_CharToGlyph[a.m_Base]];

        // Copy the base cell, and find its ink box and ink colour.
        int inkTop = H, inkBot = -1, inkL = g.m_W, inkR = -1;
        unsigned int ink = 0xFFFFFF; unsigned char inkA = 255; int bestLum = -1;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < g.m_W; ++x)
            {
                const unsigned char* s = &m_Pixels[((size_t)y * W0 + g.m_X + x) * 4];
                memcpy(pix(px, W, penX + x, y), s, 4);
                if (rgb(s) == key) continue;
                if (y < inkTop) inkTop = y;
                if (y > inkBot) inkBot = y;
                if (x < inkL) inkL = x;
                if (x > inkR) inkR = x;
                int lum = s[0] + s[1] + s[2];
                if (lum > bestLum) { bestLum = lum; ink = rgb(s); inkA = s[3]; }
            }
        if (inkBot < 0) { penX += g.m_W + 2; continue; }

        // í: drop the dot of the i (ink rows above the first blank row).
        if (a.m_Base == 'i')
        {
            for (int y = inkTop; y <= inkBot; ++y)
            {
                bool blankRow = true;
                for (int x = 0; x < g.m_W && blankRow; ++x)
                    if (rgb(pix(px, W, penX + x, y)) != key) blankRow = false;
                if (!blankRow) continue;
                for (int yy = inkTop; yy < y; ++yy)
                    for (int x = 0; x < g.m_W; ++x)
                    {
                        unsigned char* d = pix(px, W, penX + x, yy);
                        d[0] = (unsigned char)(key >> 16); d[1] = (unsigned char)(key >> 8);
                        d[2] = (unsigned char)key; d[3] = 0;
                    }
                inkTop = y + 1;
                while (inkTop <= inkBot)
                {
                    bool b = true;
                    for (int x = 0; x < g.m_W && b; ++x)
                        if (rgb(pix(px, W, penX + x, inkTop)) != key) b = false;
                    if (!b) break;
                    ++inkTop;
                }
                break;
            }
        }

        // Not enough room above a capital: squeeze the letter down.
        const int need = ah + gap;
        if (inkTop < need)
        {
            const int newTop = need;
            const int oldH = inkBot - inkTop + 1, newH = inkBot - newTop + 1;
            if (newH >= 3)
            {
                std::vector<unsigned char> col((size_t)oldH * 4);
                for (int x = 0; x < g.m_W; ++x)
                {
                    for (int y = 0; y < oldH; ++y) memcpy(&col[(size_t)y * 4], pix(px, W, penX + x, inkTop + y), 4);
                    for (int y = 0; y < newTop; ++y)
                    {
                        unsigned char* d = pix(px, W, penX + x, y);
                        d[0] = (unsigned char)(key >> 16); d[1] = (unsigned char)(key >> 8);
                        d[2] = (unsigned char)key; d[3] = 0;
                    }
                    for (int y = 0; y < newH; ++y)
                        memcpy(pix(px, W, penX + x, newTop + y), &col[(size_t)(y * oldH / newH) * 4], 4);
                }
                inkTop = newTop;
            }
        }

        // Draw the accent in the rows just above the ink.
        auto dot = [&](int x, int y)
        {
            if (x < 0 || x >= g.m_W || y < 0 || y >= H) return;
            unsigned char* d = pix(px, W, penX + x, y);
            d[0] = (unsigned char)(ink >> 16); d[1] = (unsigned char)(ink >> 8);
            d[2] = (unsigned char)ink; d[3] = inkA;
        };
        const int base = inkTop - gap - 1;          // lowest accent row
        const int cx = (inkL + inkR) / 2;
        auto acute = [&](int x0)
        {
            // rising to the right, one column per row
            for (int r = 0; r < ah; ++r)
                for (int tt = 0; tt < t; ++tt)
                    dot(x0 + r + tt - ah / 2, base - r);
        };
        if (a.m_Accent == kAcute)
            acute(cx);
        else if (a.m_Accent == kDoubleAcute)
        {
            int sp = (inkR - inkL) >= 6 ? 2 : 1;
            acute(cx - sp); acute(cx + sp + 1);
        }
        else
        {
            int sp = (inkR - inkL + 1) / 4 + 1;
            for (int r = 0; r < t; ++r)
                for (int c = 0; c < t; ++c)
                {
                    dot(cx - sp + c, base - r - (ah > 2 ? 1 : 0));
                    dot(cx + sp + c - (t - 1), base - r - (ah > 2 ? 1 : 0));
                }
        }

        Glyph ng; ng.m_X = penX; ng.m_W = g.m_W;
        m_CharToGlyph[a.m_Char] = (int)m_Glyphs.size();
        m_Glyphs.push_back(ng);
        penX += g.m_W + 2;
    }

    m_Pixels.swap(px);
    m_Sheet.m_pRGBA = &m_Pixels[0];
    m_Sheet.m_Width = W;
}


bool CLTBitmapFont::GetGlyph(unsigned char c, int& x, int& w) const
{
    int i = m_CharToGlyph[c];
    if (i < 0 || i >= (int)m_Glyphs.size()) return false;
    x = m_Glyphs[i].m_X;
    w = m_Glyphs[i].m_W;
    return true;
}


int CLTBitmapFont::MeasureWidth(const char* pStr, int nExtraSpace) const
{
    if (!pStr || !*pStr || m_Glyphs.empty()) return 0;

    int total = 0;
    for (const unsigned char* p = (const unsigned char*)pStr; *p; ++p)
    {
        if (*p == ' ') { total += m_AvgWidth + nExtraSpace; continue; }

        int gx, gw;
        if (GetGlyph(*p, gx, gw)) total += gw + nExtraSpace;
        // A character the font does not carry contributes nothing rather than a
        // guessed width -- an invented advance would drift the whole line.
    }

    return total;
}


void CLTBitmapFont::Draw(const LT2DSurf& dst, int x, int y, const char* pStr,
                         bool bSolid, unsigned int color,
                         unsigned int transColor, int nExtraSpace) const
{
    if (!pStr || !dst.m_pRGBA || m_Glyphs.empty()) return;

    int penX = x;

    for (const unsigned char* p = (const unsigned char*)pStr; *p; ++p)
    {
        if (*p == ' ') { penX += m_AvgWidth + nExtraSpace; continue; }

        int gx, gw;
        if (!GetGlyph(*p, gx, gw)) continue;

        LT2DRect src;
        src.left   = gx;
        src.top    = 0;
        src.right  = gx + gw;
        src.bottom = m_Height;

        if (m_bSmooth)
        {
            // Coverage in the alpha channel: blend the colour (or the glyph's
            // own white) by it.
            const unsigned int c = bSolid ? color : 0xFFFFFF;
            const int cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
            for (int yy = 0; yy < m_Height; ++yy)
            {
                const int dy = y + yy;
                if (dy < 0 || dy >= dst.m_Height) continue;
                for (int xx = 0; xx < gw; ++xx)
                {
                    const int dx = penX + xx;
                    if (dx < 0 || dx >= dst.m_Width) continue;
                    const unsigned char* sp = &m_Pixels[((size_t)yy * m_Sheet.m_Width + gx + xx) * 4];
                    const int a = sp[3];
                    if (!a) continue;
                    unsigned char* dp = dst.m_pRGBA + ((size_t)dy * dst.m_Width + dx) * 4;
                    dp[0] = (unsigned char)((cr * a + dp[0] * (255 - a)) / 255);
                    dp[1] = (unsigned char)((cg * a + dp[1] * (255 - a)) / 255);
                    dp[2] = (unsigned char)((cb * a + dp[2] * (255 - a)) / 255);
                    dp[3] = 255;
                }
            }
            penX += gw + nExtraSpace;
            continue;
        }

        if (bSolid)
            LT2D::BlitSolidColor(dst, m_Sheet, &src, penX, y, transColor, color, 1.0f);
        else
            LT2D::Blit(dst, m_Sheet, &src, penX, y, true, transColor, 1.0f);

        penX += gw + nExtraSpace;
    }
}


// ----------------------------------------------------------------------- //
//
// PORT: the smooth (system font) build. See ltfont.h.
//
// ----------------------------------------------------------------------- //

#ifdef __APPLE__
bool LTSysFont_RasterGlyph(unsigned int cp, int nPixelHeight,
                           std::vector<unsigned char>& cov, int& nWidth);   // ltsysfont.mm
#endif

namespace
{
    // CP1252 0x80-0x9F as Unicode (0 = unused)
    const unsigned short kSmooth80_9F[32] =
    {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
    };

    unsigned int SmoothCodePoint(int b)
    {
        switch (b)                      // Hungarian, at CP1250's positions
        {
            case 0xF5: return 0x0151;   // ő
            case 0xFB: return 0x0171;   // ű
            case 0xD5: return 0x0150;   // Ő
            case 0xDB: return 0x0170;   // Ű
        }
        if (b >= 0x80 && b <= 0x9F) return kSmooth80_9F[b - 0x80];
        return (unsigned int)b;
    }
}

bool CLTBitmapFont::BuildSmooth(int nHeight, unsigned int transColor)
{
#ifndef __APPLE__
    (void)nHeight; (void)transColor;
    return false;
#else
    if (nHeight < 6) return false;

    struct G { int code; int w; std::vector<unsigned char> cov; };
    std::vector<G> glyphs;
    int nSpace = nHeight / 4;
    int total = 1;                       // column 0 stays the key colour
    for (int b = 32; b < 256; ++b)
    {
        if (b >= 127 && b < 0x80) continue;
        if (b == 0xA0) continue;
        const unsigned int cp = SmoothCodePoint(b);
        if (!cp) continue;
        G g; g.code = b; g.w = 0;
        if (!LTSysFont_RasterGlyph(cp, nHeight, g.cov, g.w)) continue;
        if (b == ' ') { nSpace = g.w; continue; }
        total += g.w;
        glyphs.push_back(std::move(g));
    }
    if (glyphs.size() < 94) return false;

    const unsigned int key = LT2D::ColorRGB(transColor);
    m_Pixels.assign((size_t)total * nHeight * 4, 0);
    for (size_t i = 0; i < (size_t)total * nHeight; ++i)
    {
        m_Pixels[i * 4 + 0] = (unsigned char)(key >> 16);
        m_Pixels[i * 4 + 1] = (unsigned char)(key >> 8);
        m_Pixels[i * 4 + 2] = (unsigned char)key;
        m_Pixels[i * 4 + 3] = 0;
    }
    m_Glyphs.clear();
    for (int i = 0; i < 256; ++i) m_CharToGlyph[i] = -1;

    int x = 1;
    for (const G& g : glyphs)
    {
        for (int yy = 0; yy < nHeight; ++yy)
            for (int xx = 0; xx < g.w; ++xx)
            {
                const unsigned char a = g.cov[(size_t)yy * g.w + xx];
                if (!a) continue;
                unsigned char* p = &m_Pixels[((size_t)yy * total + x + xx) * 4];
                p[0] = p[1] = p[2] = 255; p[3] = a;
            }
        Glyph gl; gl.m_X = x; gl.m_W = g.w;
        m_CharToGlyph[g.code] = (int)m_Glyphs.size();
        m_Glyphs.push_back(gl);
        x += g.w;
    }

    m_Sheet.m_pRGBA  = &m_Pixels[0];
    m_Sheet.m_Width  = total;
    m_Sheet.m_Height = nHeight;
    m_Height   = nHeight;
    m_AvgWidth = nSpace;
    m_bSmooth  = true;
    return true;
#endif
}
