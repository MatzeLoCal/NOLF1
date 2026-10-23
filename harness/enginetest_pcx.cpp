// ----------------------------------------------------------------------- //
//
// enginetest_pcx.cpp -- the PCX decoder, with negative controls.
//
// ⚠️ THE DECODER'S REAL VERIFICATION IS ELSEWHERE, AND IT IS STRONGER
//
//   Every one of the 289 shipped PCX files was decoded by CLTPcx and by PIL --
//   an independent implementation -- and compared byte for byte: **289 of 289
//   identical**. That is the check that matters, and it is recorded in
//   FORMATS.md §2a.
//
//   This file is the part of it that can live in the tree: it needs no Python,
//   runs against the real archives, and asserts the properties a future change
//   could break silently. Each check is paired with one that must FAIL.
//
// ----------------------------------------------------------------------- //

// ⚠️ Include order mirrors the game's Stdafx.h -- HANDOVER.md §4.3.
#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltpcx.h"
#include "ltfilemgr.h"

#include <stdio.h>
#include <string.h>

static int g_nPass = 0;
static int g_nFail = 0;

static void ok(const char* w)             { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)            { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w)  { b ? ok(w) : bad(w); }


int main(int argc, char** argv)
{
    const char* pDirs[8];
    int nDirs = 0;
    for (int i = 1; i < argc && nDirs < 8; ++i) pDirs[nDirs++] = argv[i];
    if (nDirs == 0) { fprintf(stderr, "usage: enginetest_pcx <game-dir> ...\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(pDirs, nDirs) == 0) { fprintf(stderr, "no game data\n"); return 2; }

    // --- the 3-plane 24-bit variant (75 of 289) ----------------------- //
    {
        CLTPcx p;
        check(p.LoadThroughFileMgr(&fm, "interface\\pause.pcx"), "INTERFACE\\PAUSE.PCX loads");
        check(p.GetWidth() == 640 && p.GetHeight() == 480,
              "PAUSE.PCX is exactly 640x480 (the screen size)");
        check(p.GetPlanes() == 3, "PAUSE.PCX is the 3-plane 24-bit variant");
        check(!p.HasPalette(), "a 3-plane file has no trailing palette");
        check(p.GetRGBASize() == 640u * 480u * 4u, "RGBA buffer is w*h*4");

        // ⚠️ NEGATIVE CONTROL: a decoder that filled the buffer with a constant
        // would pass every check above. A real 640x480 screen image must
        // contain more than a handful of distinct colours.
        const unsigned char* rgba = p.GetRGBA();
        int nDistinct = 0;
        unsigned int seen[64];
        for (unsigned int i = 0; i < 640u * 480u && nDistinct < 64; ++i)
        {
            unsigned int c = ((unsigned int)rgba[i*4] << 16) |
                             ((unsigned int)rgba[i*4+1] << 8) | rgba[i*4+2];
            bool bNew = true;
            for (int k = 0; k < nDistinct; ++k) if (seen[k] == c) { bNew = false; break; }
            if (bNew) seen[nDistinct++] = c;
        }
        check(nDistinct >= 32, "NEGATIVE CONTROL: the image is not a flat fill");
        printf("        %d distinct colours in the first sample\n", nDistinct);

        // Alpha must be opaque everywhere: transparency is a DRAW-time colour
        // key in this engine, not a load-time decision (see ltpcx.h).
        bool bAllOpaque = true;
        for (unsigned int i = 0; i < 640u * 480u; ++i)
            if (rgba[i*4+3] != 255) { bAllOpaque = false; break; }
        check(bAllOpaque, "alpha is 255 everywhere -- keying is a draw-time job");
    }

    // --- the paletted variant (214 of 289) ---------------------------- //
    {
        CLTPcx p;
        check(p.LoadThroughFileMgr(&fm, "interface\\mb_back.pcx"),
              "INTERFACE\\MB_BACK.PCX loads");
        check(p.GetPlanes() == 1, "MB_BACK.PCX is the 1-plane paletted variant");
        check(p.HasPalette(), "NEGATIVE CONTROL: the 0x0C palette was found");
        check(p.GetRGBASize() == p.GetWidth() * p.GetHeight() * 4,
              "paletted output is still RGBA w*h*4");
    }

    // --- the extensionless name the game actually passes --------------- //
    {
        // ⚠️ Layout.txt says Frame = "Interface\DlgWnd\dialogueframe" and the
        // game appends TOP/BOTTOMLEFT/... -- so the loader must add ".pcx"
        // itself. A loader that only accepts full names fails every frame piece.
        CLTPcx p;
        check(p.LoadThroughFileMgr(&fm, "Interface\\DlgWnd\\dialogueframetop"),
              "an extensionless name resolves by appending .pcx");
        check(p.GetWidth() > 0 && p.GetHeight() > 0, "and decodes to a real size");
    }

    // --- negative controls -------------------------------------------- //
    {
        CLTPcx p;
        check(!p.LoadThroughFileMgr(&fm, "interface\\no_such_image"),
              "NEGATIVE CONTROL: a missing name fails");
        check(!p.IsLoaded(), "a failed load leaves the object empty");

        // ⚠️ A non-PCX file must be REFUSED, not decoded into noise. Handing a
        // 6031-string DLL to the decoder is the clearest possible wrong input.
        check(!p.LoadThroughFileMgr(&fm, "CRES.DLL"),
              "NEGATIVE CONTROL: a non-PCX file is refused");

        check(!p.Load(0, 0), "NEGATIVE CONTROL: empty input is refused");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
