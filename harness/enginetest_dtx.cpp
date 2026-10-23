// ----------------------------------------------------------------------- //
//
// enginetest_dtx.cpp -- the DTX texture reader, with negative controls.
//
// ⚠️ THE STRONGER VERIFICATION IS RECORDED ELSEWHERE, AS WITH PCX
//
//   Every S3TC mip level in the shipped set was decoded by CLTDtx and by
//   **Pillow's** independent BCn decoder and compared byte for byte:
//   **10,512 mip levels, 0 differing.** And the whole reader -- including the
//   uncompressed and palettised formats Pillow cannot check -- was digested
//   field-by-field against a separately written Python reader across all
//   **4160 files with zero differences**. FORMATS.md §2.
//
//   This file is the part that lives in the tree: no Python, real archives,
//   and it sweeps rather than samples.
//
// ----------------------------------------------------------------------- //

// ⚠️ Include order mirrors the game's Stdafx.h -- HANDOVER.md §4.3.
#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltdtx.h"
#include "ltrezfile.h"
#include "ltfilemgr.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

static int g_nPass = 0;
static int g_nFail = 0;

static void ok(const char* w)             { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)            { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w)  { b ? ok(w) : bad(w); }

static bool EndsWith(const std::string& s, const char* p)
{
    size_t n = strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

static bool Slurp(CLTFileMgr& fm, const char* pName, std::vector<unsigned char>& out)
{
    out.clear();
    ILTStream* pStream = fm.OpenFile(pName);
    if (!pStream) return false;
    unsigned long nLen = 0;
    pStream->GetLen(&nLen);
    out.resize((size_t)nLen);
    bool bOK = (nLen > 0) && (pStream->Read(&out[0], nLen) == LT_OK);
    pStream->Release();
    if (!bOK) out.clear();
    return bOK;
}


int main(int argc, char** argv)
{
    const char* pDirs[8];
    int nDirs = 0;
    for (int i = 1; i < argc && nDirs < 8; ++i) pDirs[nDirs++] = argv[i];
    if (nDirs == 0) { fprintf(stderr, "usage: enginetest_dtx <game-dir> ...\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(pDirs, nDirs) == 0) { fprintf(stderr, "no game data\n"); return 2; }

    std::vector<std::string> names;
    for (int d = 0; d < nDirs; ++d)
    {
        DIR* pDir = opendir(pDirs[d]);
        if (!pDir) continue;
        struct dirent* pEnt;
        while ((pEnt = readdir(pDir)) != 0)
        {
            std::string l(pEnt->d_name);
            std::transform(l.begin(), l.end(), l.begin(), ::tolower);
            if (!EndsWith(l, ".rez")) continue;
            std::string sPath = std::string(pDirs[d]) + "/" + pEnt->d_name;
            CLTRezArchive rez;
            if (!rez.Open(sPath.c_str())) continue;
            for (unsigned int i = 0; i < rez.GetNumEntries(); ++i)
            {
                std::string u(rez.GetEntryName(i));
                std::transform(u.begin(), u.end(), u.begin(), ::toupper);
                if (EndsWith(u, ".DTX")) names.push_back(u);
            }
        }
        closedir(pDir);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    printf("\n--- sweep: every .dtx in the archives ---\n");
    printf("        %u distinct texture names\n", (unsigned int)names.size());
    check(names.size() >= 4100, "the archives yield the expected ~4160 textures");

    unsigned int nLoaded = 0, nFailed = 0;
    unsigned int nDXT1 = 0, nDXT3 = 0, nDXT5 = 0, n32 = 0, n32P = 0;
    unsigned int nDecodeFail = 0, nBadSize = 0, nSectioned = 0;
    unsigned long long nTexels = 0;
    std::string sFirstFail, sFirstWhy;
    std::vector<unsigned char> rgba;

    for (size_t i = 0; i < names.size(); ++i)
    {
        CLTDtx t;
        if (!t.LoadThroughFileMgr(&fm, names[i].c_str()))
        {
            ++nFailed;
            if (sFirstFail.empty()) { sFirstFail = names[i]; sFirstWhy = t.GetError(); }
            continue;
        }
        ++nLoaded;

        switch (t.GetFormat())
        {
            case CLTDtx::kDXT1:    ++nDXT1; break;
            case CLTDtx::kDXT3:    ++nDXT3; break;
            case CLTDtx::kDXT5:    ++nDXT5; break;
            case CLTDtx::kBPP_32:  ++n32;   break;
            case CLTDtx::kBPP_32P: ++n32P;  break;
            default: break;
        }
        if (t.GetNumSections()) ++nSectioned;

        // Decode every mip of every texture -- 4160 files, not a sample.
        for (unsigned int m = 0; m < t.GetNumMipmaps(); ++m)
        {
            const CLTDtx::Mip* mp = t.GetMip(m);
            if (!t.DecodeMip(m, rgba)) { ++nDecodeFail; continue; }
            if (rgba.size() != (size_t)mp->m_nWidth * mp->m_nHeight * 4) ++nBadSize;
            nTexels += (unsigned long long)mp->m_nWidth * mp->m_nHeight;
        }
    }

    printf("        loaded %u, refused %u\n", nLoaded, nFailed);
    if (nFailed) printf("        first refusal: %s -- %s\n", sFirstFail.c_str(), sFirstWhy.c_str());

    // ⚠️ Load() ends by asserting the header predicts the file's exact length,
    // so "loads" means "every byte is accounted for", not "parsed something".
    check(nFailed == 0, "every shipped texture loads, with its size predicted exactly");
    check(nDecodeFail == 0 && nBadSize == 0, "every mip of every texture decodes to w*h*4");
    printf("        %llu texels decoded\n", nTexels);

    printf("        DXT1 %u, DXT3 %u, DXT5 %u, BPP_32 %u, BPP_32P %u\n",
           nDXT1, nDXT3, nDXT5, n32, n32P);

    // ⚠️ THE CENSUS, ASSERTED. An earlier survey of this format concluded
    // "all 4166 are BPP_32, no compression" -- it read m_Extra from offset 28
    // instead of 24, and the numbers looked plausible and uniform. Stating the
    // expected split turns that class of error into a test failure.
    check(nDXT1 + nDXT3 + nDXT5 > 2600, "2657-ish textures really are S3TC-compressed");
    check(n32 > 1400, "and ~1500 are uncompressed BPP_32");

    // ⚠️ BPPIdent 0 must fold to BPP_32, not BPP_8P. 605 files say 0; if they
    // were read literally this count would collapse.
    check(n32P == 1 && nSectioned == 1,
          "exactly one BPP_32P texture, and it is the only one with a section");

    // ----------------------------------------------------------------- //
    printf("\n--- textures with exactly known properties ---\n");
    {
        CLTDtx t;
        check(t.LoadThroughFileMgr(&fm, "TEX\\GLASS\\GL01\\GL0007.DTX"),
              "the one BPP_32P texture loads");
        check(t.GetFormat() == CLTDtx::kBPP_32P, "it is BPP_32P");
        check(t.GetWidth() == 128 && t.GetHeight() == 128 && t.GetNumMipmaps() == 3,
              "128x128 with 3 mips");
        check(t.GetNumSections() == 1 &&
              strcmp(t.GetSectionType(0), "PALLETE32") == 0,
              "and it carries a PALLETE32 section");

        // ⚠️ Its indices must actually address the palette: a decode that
        // ignored the palette would still produce w*h*4 bytes.
        check(t.DecodeMip(0, rgba), "its base mip decodes");
        int nDistinct = 0;
        unsigned int seen[64];
        for (size_t i = 0; i < rgba.size() && nDistinct < 64; i += 4)
        {
            unsigned int c = ((unsigned int)rgba[i] << 16) |
                             ((unsigned int)rgba[i+1] << 8) | rgba[i+2];
            bool bNew = true;
            for (int k = 0; k < nDistinct; ++k) if (seen[k] == c) { bNew = false; break; }
            if (bNew) seen[nDistinct++] = c;
        }
        check(nDistinct >= 16, "NEGATIVE CONTROL: the palettised image is not a flat fill");
        printf("        %d distinct colours in the sample\n", nDistinct);
    }

    // ⚠️ The channel-order check, stated as a property rather than eyeballed.
    // Stored BGRA; a reader that passed the bytes through unswapped would make
    // a sky texture red-dominant.
    // ⚠️ AGGREGATED OVER EVERY MATCHING TEXTURE, NOT THE FIRST ONE.
    // The first SKY texture in name order is TEX\SKYPAN4.DTX, which is a flat
    // 128/128/128 panel -- mean R and mean B come out *exactly* equal, so it
    // carries no hue evidence at all and the check fails on a correct decoder.
    // Sampling is the recurring mistake in this project (HANDOVER.md §5:
    // "sweep, don't sample"); the fact was established across all 46 SKY
    // textures, so the test is stated the same way.
    {
        CLTDtx t;
        unsigned int nSky = 0, nWood = 0;
        double fSkyR = 0, fSkyB = 0, fWoodR = 0, fWoodB = 0, fSkyN = 0, fWoodN = 0;
        for (size_t i = 0; i < names.size(); ++i)
        {
            bool bSky  = names[i].find("SKY")  != std::string::npos;
            bool bWood = names[i].find("WOOD") != std::string::npos;
            if (!bSky && !bWood) continue;
            if (!t.LoadThroughFileMgr(&fm, names[i].c_str())) continue;
            if (t.GetFormat() != CLTDtx::kBPP_32) continue;
            if (!t.DecodeMip(0, rgba)) continue;
            double r = 0, b = 0;
            for (size_t k = 0; k < rgba.size(); k += 4) { r += rgba[k]; b += rgba[k+2]; }
            if (bSky)  { fSkyR += r;  fSkyB += b;  fSkyN += rgba.size()/4;  ++nSky; }
            else       { fWoodR += r; fWoodB += b; fWoodN += rgba.size()/4; ++nWood; }
        }
        printf("        SKY  x%u: mean R %.1f, mean B %.1f\n",
               nSky, fSkyR / fSkyN, fSkyB / fSkyN);
        printf("        WOOD x%u: mean R %.1f, mean B %.1f\n",
               nWood, fWoodR / fWoodN, fWoodB / fWoodN);

        check(nSky > 20 && fSkyB > fSkyR,
              "SKY textures decode blue-dominant -- BGRA is unswapped correctly");

        // ⚠️ The opposite-direction control. Blue-dominant skies alone would
        // also pass if the decoder swapped nothing and the data happened to be
        // RGBA; wood being RED-dominant in the same run is what rules that out.
        check(nWood > 5 && fWoodR > fWoodB,
              "NEGATIVE CONTROL: WOOD textures decode red-dominant in the same run");
    }

    // ----------------------------------------------------------------- //
    printf("\n--- negative controls ---\n");
    {
        CLTDtx t;
        check(!t.Load(0, 0), "NEGATIVE CONTROL: a null buffer is refused");
        check(!t.IsLoaded(), "and the texture stays empty");
    }
    {
        std::vector<unsigned char> buf;
        if (Slurp(fm, "interface\\pause.pcx", buf))
        {
            CLTDtx t;
            check(!t.Load(&buf[0], (unsigned int)buf.size()),
                  "NEGATIVE CONTROL: a PCX offered as a texture is refused");
            printf("        said: %s\n", t.GetError());
        }
        else bad("could not read a PCX to offer as a texture");
    }

    std::vector<unsigned char> good;
    if (!Slurp(fm, "TEX\\GLASS\\GL01\\GL0007.DTX", good)) bad("could not read a texture to corrupt");
    else
    {
        {
            CLTDtx t;
            check(t.Load(&good[0], (unsigned int)good.size()),
                  "the uncorrupted copy still loads (the control's control)");
        }
        {
            CLTDtx t;
            check(!t.Load(&good[0], (unsigned int)good.size() - 64),
                  "NEGATIVE CONTROL: a truncated texture is refused, not part-decoded");
            printf("        said: %s\n", t.GetError());
        }
        {
            // ⚠️ Claim a format the data is not. The bytes stay valid; only
            // m_Extra[2] changes, from 7 (BPP_32P) to 4 (DXT1). The mip chain
            // then has a completely different size, so the file's total length
            // stops matching -- which is precisely the check that caught the
            // original wrong census of this format.
            std::vector<unsigned char> v = good;
            v[26] = 4;
            CLTDtx t;
            check(!t.Load(&v[0], (unsigned int)v.size()),
                  "NEGATIVE CONTROL: a texture relabelled to another format is refused");
            printf("        said: %s\n", t.GetError());
        }
        {
            std::vector<unsigned char> v = good;
            v[26] = 99;
            CLTDtx t;
            check(!t.Load(&v[0], (unsigned int)v.size()),
                  "NEGATIVE CONTROL: an unknown BPPIdent is refused, not guessed");
            printf("        said: %s\n", t.GetError());
        }
        {
            // Version is an equality test, exactly as the donor's is.
            std::vector<unsigned char> v = good;
            v[4] = 0xFA;                       // -6 instead of -5
            CLTDtx t;
            check(!t.Load(&v[0], (unsigned int)v.size()),
                  "NEGATIVE CONTROL: a wrong DTX version is refused");
            printf("        said: %s\n", t.GetError());
        }
        {
            CLTDtx t;
            check(!t.LoadThroughFileMgr(&fm, "TEX\\NOT_A_REAL_TEXTURE.DTX"),
                  "NEGATIVE CONTROL: a texture that does not exist is refused");
        }
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
