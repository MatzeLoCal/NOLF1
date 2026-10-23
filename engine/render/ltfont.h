// ----------------------------------------------------------------------- //
//
// MODULE  : ltfont.h
//
// PURPOSE : NOLF 1's bitmap fonts -- the last thing between the engine and a
//           readable menu.
//
// ⚠️ BITMAP FONTS, NOT SYSTEM FONTS
//
//   CLTGUIFont has two paths and the SDK offers ILTClient::CreateFont, which
//   takes a typeface NAME and would mean CoreText on macOS. NOLF does not use
//   it for English:
//
//       if (IsEnglish())  ... SetupFont(...)          <- bitmap
//       else              ... InitEngineFont(...)     <- system font
//       -- CInterfaceResMgr::InitFonts, InterfaceResMgr.cpp:344
//
//   and SetupFont fills in `lithFont.szFontBitmap = g_szFontName`
//   (InterfaceResMgr.cpp:761). The game ships 12 font bitmaps in
//   `INTERFACE\FONTS\*.PCX`, so the English path needs **no platform text
//   engine at all** -- which keeps the whole renderer free of platform
//   dependencies for one more step.
//
// THE FORMAT (measured -- FORMATS.md §2b)
//
//   One PCX strip of glyphs separated by FULLY TRANSPARENT COLUMNS. The
//   transparent colour is chosen by the caller: black for blended fonts,
//   magenta for chroma-keyed ones (InterfaceResMgr.cpp:765-771).
//
//   Confirmed against the shipped files:
//     FONT_MED_0.PCX    850x20   94 glyphs
//     FONT_SMALL_0.PCX  595x14   94 glyphs
//     FONT_TITLE.PCX   1148x32   94 glyphs
//     NUMBERFONTFORE   435x31    32 glyphs   <- a subset, see nGroupFlags
//
//   94 = LTF_NUM_TOTAL_CHARS, and it is ASCII 33..126 in order, arranged in
//   seven contiguous groups that nGroupFlags selects from the START:
//
//     0x01 SYMBOLS_1  15   ASCII  33.. 47      0x10 SYMBOLS_3   6   91.. 96
//     0x02 NUMBERS    10          48.. 57      0x20 LOWERCASE  26   97..122
//     0x04 SYMBOLS_2   7          58.. 64      0x40 SYMBOLS_4   4  123..126
//     0x08 UPPERCASE  26          65.. 90
//
//   NUMBERFONTFORE's 32 glyphs are exactly 15+10+7, i.e. flags 0x07 -- which is
//   what confirms the groups are contiguous and start-aligned rather than
//   indexed some other way.
//
// ⚠️ SPACE IS NOT IN THE BITMAP
//
//   ASCII 32 has no glyph -- it would be an all-transparent column, which is
//   precisely the separator. The original carries `m_nAvgWidth` ("Average
//   character width", lithbitmapfont.h:56) and space is the only character that
//   could need it, so that is what a space advances by here.
//
// ⚠️ LTF_INCLUDE_FIXED_END CHANGES WHERE GLYPHS END
//
//   From lithfontdefs.h:73 -- with the flag, "the first transparent space after
//   a letter is where the width breaks"; without it, it "takes an average of
//   the transparent space between two letters and places the break there".
//   So the default is NOT the tight glyph box: cells meet at the MIDPOINT of
//   the gap, which makes text measurably wider. Getting this backwards shifts
//   every centred string.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_FONT_H__
#define __LT_FONT_H__

#include <vector>

#include "lt2d.h"

// Group flags, from lithfontdefs.h:65.
#define LTFONT_SYMBOLS_1    0x01
#define LTFONT_NUMBERS      0x02
#define LTFONT_SYMBOLS_2    0x04
#define LTFONT_UPPERCASE    0x08
#define LTFONT_SYMBOLS_3    0x10
#define LTFONT_LOWERCASE    0x20
#define LTFONT_SYMBOLS_4    0x40
#define LTFONT_FIXED_END    0x80
#define LTFONT_ALL          0x7F

class CLTBitmapFont
{
public:

    CLTBitmapFont();

    // Splits an already-decoded RGBA image into glyphs. Returns false if the
    // strip yields no glyphs, or more than the flags account for.
    bool            Build(const unsigned char* pRGBA, int nWidth, int nHeight,
                          unsigned int transColor, unsigned int nGroupFlags);

    bool            IsBuilt() const { return !m_Glyphs.empty(); }

    int             GetHeight() const   { return m_Height; }
    int             GetAvgWidth() const { return m_AvgWidth; }
    int             GetNumGlyphs() const { return (int)m_Glyphs.size(); }

    // Source x and width of one character's cell, or false if the font does not
    // carry it. ⚠️ Space returns false -- it has no glyph; use GetAvgWidth.
    bool            GetGlyph(unsigned char c, int& x, int& w) const;

    // Pixel width of a string, honouring extra letter spacing.
    int             MeasureWidth(const char* pStr, int nExtraSpace = 0) const;

    // Draws at (x, y) into dst, keying out transColor. When bSolid, every
    // non-keyed pixel is written as `color` instead of the glyph's own -- which
    // is how the UI tints text.
    void            Draw(const LT2DSurf& dst, int x, int y, const char* pStr,
                         bool bSolid, unsigned int color,
                         unsigned int transColor, int nExtraSpace = 0) const;

    // The decoded strip, so a caller can blit from it directly.
    const LT2DSurf& GetSheet() const { return m_Sheet; }

private:

    struct Glyph { int m_X; int m_W; };

    std::vector<unsigned char>  m_Pixels;       // owns the strip
    LT2DSurf                    m_Sheet;
    std::vector<Glyph>          m_Glyphs;       // in bitmap order
    int                         m_CharToGlyph[256];
    int                         m_Height;
    int                         m_AvgWidth;
};

#endif // __LT_FONT_H__
