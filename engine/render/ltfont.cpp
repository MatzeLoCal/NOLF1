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

    return true;
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

        if (bSolid)
            LT2D::BlitSolidColor(dst, m_Sheet, &src, penX, y, transColor, color, 1.0f);
        else
            LT2D::Blit(dst, m_Sheet, &src, penX, y, true, transColor, 1.0f);

        penX += gw + nExtraSpace;
    }
}
