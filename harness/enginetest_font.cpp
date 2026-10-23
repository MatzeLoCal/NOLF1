// ----------------------------------------------------------------------- //
//
// enginetest_font.cpp -- bitmap fonts, against the real shipped bitmaps.
//
// Each check is paired with one that must FAIL. The important ones here are
// the character MAPPING (a font that splits at the wrong columns still yields
// glyphs, just the wrong ones) and the FIXED_END width rule.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltfont.h"
#include "ltpcx.h"
#include "ltfilemgr.h"

#include <stdio.h>
#include <string.h>

static int g_nPass = 0;
static int g_nFail = 0;
static void ok(const char* w)            { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)           { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w) { b ? ok(w) : bad(w); }

#define BLACK   0x000000u
#define MAGENTA 0xFF00FFu

static bool LoadFont(CLTFileMgr& fm, CLTBitmapFont& f, const char* pName,
                     unsigned int key, unsigned int flags)
{
    CLTPcx pcx;
    if (!pcx.LoadThroughFileMgr(&fm, pName)) return false;
    return f.Build(pcx.GetRGBA(), (int)pcx.GetWidth(), (int)pcx.GetHeight(), key, flags);
}

int main(int argc, char** argv)
{
    const char* pDirs[8];
    int nDirs = 0;
    for (int i = 1; i < argc && nDirs < 8; ++i) pDirs[nDirs++] = argv[i];
    if (nDirs == 0) { fprintf(stderr, "usage: enginetest_font <game-dir> ...\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(pDirs, nDirs) == 0) { fprintf(stderr, "no game data\n"); return 2; }

    // --- a full 94-glyph font ------------------------------------------ //
    {
        CLTBitmapFont f;
        check(LoadFont(fm, f, "interface\\fonts\\font_med_0.pcx", BLACK, LTFONT_ALL),
              "FONT_MED_0.PCX builds");
        check(f.GetNumGlyphs() == 94,
              "exactly 94 glyphs -- LTF_NUM_TOTAL_CHARS");
        printf("        %d glyphs, height %d, avg width %d\n",
               f.GetNumGlyphs(), f.GetHeight(), f.GetAvgWidth());

        int x, w;
        check(f.GetGlyph('A', x, w), "'A' is present");
        check(f.GetGlyph('z', x, w), "'z' is present");
        check(f.GetGlyph('!', x, w), "'!' is present (first glyph, ASCII 33)");
        check(f.GetGlyph('~', x, w), "'~' is present (last glyph, ASCII 126)");

        // ⚠️ NEGATIVE CONTROL: space has no glyph. If it reported one, the
        // separator scan has found ink in what should be blank -- i.e. the
        // whole strip is split wrong and every later character is off.
        check(!f.GetGlyph(' ', x, w),
              "NEGATIVE CONTROL: space has NO glyph (it is the separator)");
        check(!f.GetGlyph((unsigned char)200, x, w),
              "NEGATIVE CONTROL: a character outside 33..126 is absent");

        // ⚠️ THE MAPPING CHECK. 'A' is glyph 32 = 15 symbols + 10 digits + 7
        // symbols. A font split at the wrong columns still produces 94 glyphs
        // in some other arrangement, and only an ORDER check catches that.
        int xa, wa, x0, w0, xz, wz;
        f.GetGlyph('A', xa, wa); f.GetGlyph('0', x0, w0); f.GetGlyph('z', xz, wz);
        check(x0 < xa && xa < xz,
              "glyphs are in ASCII order across the strip: '0' < 'A' < 'z'");

        // Measurement must be monotonic and space must cost something.
        int wHello  = f.MeasureWidth("Hello");
        int wHelloW = f.MeasureWidth("Hello World");
        check(wHello > 0, "a string measures non-zero");
        check(wHelloW > wHello, "a longer string measures wider");
        check(f.MeasureWidth(" ") == f.GetAvgWidth(),
              "a space advances by the average width");
        check(f.MeasureWidth("") == 0, "an empty string measures zero");
        check(f.MeasureWidth("AA", 2) == f.MeasureWidth("AA") + 4,
              "extra letter spacing is applied per character");
    }

    // --- a SUBSET font -------------------------------------------------- //
    {
        // NUMBERFONTFORE has 32 glyphs = symbols1(15) + numbers(10) +
        // symbols2(7), i.e. flags 0x07. This is the check that proves the
        // groups are contiguous and start-aligned.
        CLTBitmapFont f;
        unsigned int flags = LTFONT_SYMBOLS_1 | LTFONT_NUMBERS | LTFONT_SYMBOLS_2;
        check(LoadFont(fm, f, "interface\\fonts\\numberfontfore.pcx", BLACK, flags),
              "NUMBERFONTFORE.PCX builds as a subset font");
        check(f.GetNumGlyphs() == 32, "32 glyphs = 15 + 10 + 7");

        int x, w;
        check(f.GetGlyph('7', x, w), "a digit is present in the subset");
        // ⚠️ NEGATIVE CONTROL: letters are NOT in this bitmap. If 'A' resolved,
        // the group mapping is ignoring the flags and every digit would be
        // reading the wrong cell.
        check(!f.GetGlyph('A', x, w),
              "NEGATIVE CONTROL: 'A' is absent -- the flags are honoured");
    }

    // --- FIXED_END changes widths --------------------------------------- //
    {
        CLTBitmapFont loose, fixed;
        LoadFont(fm, loose, "interface\\fonts\\font_med_0.pcx", BLACK, LTFONT_ALL);
        LoadFont(fm, fixed, "interface\\fonts\\font_med_0.pcx", BLACK,
                 LTFONT_ALL | LTFONT_FIXED_END);

        int wl = loose.MeasureWidth("Hello World");
        int wf = fixed.MeasureWidth("Hello World");
        printf("        \"Hello World\": default %dpx, FIXED_END %dpx\n", wl, wf);

        // ⚠️ Without FIXED_END the cell runs to the MIDPOINT of the gap, so it
        // must be WIDER. If these matched, the flag would be doing nothing and
        // every centred string would sit slightly off.
        check(wl > wf, "NEGATIVE CONTROL: FIXED_END really does narrow the cells");
        check(fixed.GetNumGlyphs() == loose.GetNumGlyphs(),
              "and it does not change how many glyphs there are");
    }

    // --- the wrong separator colour must be REFUSED --------------------- //
    {
        // ⚠️ THE ONE THAT MATTERS MOST. With magenta as the key, no column is
        // blank, the whole strip reads as ONE glyph, and a lenient
        // implementation would happily map it to '!' and render garbage for
        // every string. lithfontdefs.h warns about exactly this failure.
        CLTBitmapFont f;
        check(!LoadFont(fm, f, "interface\\fonts\\font_med_0.pcx", MAGENTA, LTFONT_ALL),
              "NEGATIVE CONTROL: the wrong separator colour is refused");
        check(!f.IsBuilt(), "and leaves the font unbuilt");
    }

    // --- drawing -------------------------------------------------------- //
    {
        CLTBitmapFont f;
        LoadFont(fm, f, "interface\\fonts\\font_med_0.pcx", BLACK, LTFONT_ALL);

        std::vector<unsigned char> px((size_t)200 * 40 * 4, 0);
        LT2DSurf dst = { &px[0], 200, 40 };

        f.Draw(dst, 4, 4, "Hi!", false, 0, BLACK);

        int lit = 0;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] || px[i+1] || px[i+2]) ++lit;
        check(lit > 0, "drawing text lights pixels");
        printf("        \"Hi!\" lit %d pixels\n", lit);

        // ⚠️ NEGATIVE CONTROL: nothing outside the measured extent.
        int wide = f.MeasureWidth("Hi!");
        bool bClean = true;
        for (int y = 0; y < 40; ++y)
            for (int x = 4 + wide; x < 200; ++x)
            {
                const unsigned char* p = &px[((size_t)y * 200 + x) * 4];
                if (p[0] || p[1] || p[2]) bClean = false;
            }
        check(bClean, "NEGATIVE CONTROL: nothing is drawn past the measured width");

        // Solid mode must recolour.
        std::fill(px.begin(), px.end(), 0);
        f.Draw(dst, 4, 4, "Hi!", true, 0x00FF00, BLACK);
        bool bAllGreen = true, bAny = false;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] || px[i+1] || px[i+2])
            {
                bAny = true;
                if (px[i] != 0x00 || px[i+1] != 0xFF || px[i+2] != 0x00) bAllGreen = false;
            }
        check(bAny && bAllGreen, "solid mode paints every lit pixel the fill colour");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
