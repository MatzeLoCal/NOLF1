// ----------------------------------------------------------------------- //
//
// enginetest_surfaces.cpp -- the surface table, and the field that carries it.
//
// ⚠️ THE CLAIM UNDER TEST IS THAT DTX m_UserFlags IS NOLF'S SURFACE TYPE. That
// is a claim about MEANING, so it is checked the way this port checks meaning:
// against something the field did not come from. Surface.txt independently
// documents 30 = Wood, 10/11 = Stone, 20 = Metal, 110 = Sky -- and the WOOD\,
// STONE\, METAL\ and SKY\ texture families carry exactly those values.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltsurfaces.h"
#include "ltdtx.h"
#include "ltfilemgr.h"
#include "ltrezfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <map>
#include <string>
#include <vector>

static int g_nPass = 0, g_nFail = 0;
static void ok(const char* w)            { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)           { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w) { b ? ok(w) : bad(w); }

// The commonest m_UserFlags among textures whose path starts with this family.
static int DominantId(CLTFileMgr& fm, const char* pFamily, int* pCount)
{
    std::map<int,int> h;
    for (int t = 0; t < fm.GetNumTrees(); ++t)
    {
        const char* pPath = fm.GetTreePath(t);
        if (!pPath) continue;
        CLTRezArchive rz;
        if (!rz.Open(pPath)) continue;
        for (unsigned int i = 0; i < rz.GetNumEntries(); ++i)
        {
            std::string s(rz.GetEntryName(i));
            if (s.size() < 4) continue;
            std::string e = s.substr(s.size()-4);
            for (size_t k=0;k<e.size();k++) e[k]=(char)tolower((unsigned char)e[k]);
            if (e != ".dtx") continue;

            std::string up(s);
            for (size_t k=0;k<up.size();k++) up[k]=(char)toupper((unsigned char)up[k]);
            if (up.find(pFamily) == std::string::npos) continue;

            CLTDtx d;
            if (!d.LoadThroughFileMgr(&fm, s.c_str())) continue;
            h[d.GetUserFlags()]++;
        }
    }
    int best = -1, n = 0;
    for (std::map<int,int>::iterator it = h.begin(); it != h.end(); ++it)
        if (it->second > n) { n = it->second; best = it->first; }
    if (pCount) *pCount = n;
    return best;
}

int main(int argc, char** argv)
{
    std::vector<const char*> dirs;
    for (int i = 1; i < argc; ++i) dirs.push_back(argv[i]);
    if (dirs.empty()) { fprintf(stderr, "usage: enginetest_surfaces <dirs...>\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&dirs[0], (int)dirs.size()) == 0)
    { fprintf(stderr, "no game data\n"); return 2; }

    printf("\n--- the surface table ---\n");
    CLTSurfaces surf;
    check(surf.Load(&fm), "Attributes\\Surface.txt is read");
    printf("        %u surfaces\n", surf.Count());
    check(surf.Count() >= 30, "and yields the ~30 surfaces the file defines");

    // ⚠️ The block INDEX is not the id. [Surface12] is "Wood" and its Id is 30;
    // if this ever reads 12 as the id, every material gets another material's
    // sounds while still looking entirely plausible.
    const CLTSurfaces::Surface* pWood = surf.Find(30);
    check(pWood != 0, "id 30 exists");
    check(pWood && pWood->m_sName == "Wood",
          "and is named Wood -- the Id field, not the block number");

    const CLTSurfaces::Surface* pMetal = surf.Find(20);
    check(pMetal && pMetal->m_sName == "Metal", "id 20 is Metal");
    check(!surf.Find(12) || surf.Find(12)->m_sName != "Wood",
          "NEGATIVE CONTROL: block index 12 is NOT what Wood is keyed on");

    printf("\n--- m_UserFlags IS the surface type ---\n");
    {
        struct Case { const char* m_pFamily; int m_nExpect; const char* m_pWhat; };
        const Case kCases[4] = {
            { "\\WOOD\\",  30, "WOOD textures carry id 30, which Surface.txt calls Wood"  },
            { "\\METAL\\", 20, "METAL textures carry id 20, which it calls Metal"         },
            { "\\STONE\\", 10, "STONE textures carry id 10, which it calls Smooth Stone"  },
            { "\\SKY\\",    0, "SKY textures are dominated by 0 -- sky is drawn, not walked on" },
        };

        for (int i = 0; i < 4; ++i)
        {
            int n = 0;
            const int id = DominantId(fm, kCases[i].m_pFamily, &n);
            const CLTSurfaces::Surface* p = surf.Find(id);
            printf("        %-10s -> id %-4d %-16s (%d textures)\n",
                   kCases[i].m_pFamily, id,
                   p && !p->m_sName.empty() ? p->m_sName.c_str() : "-", n);
            check(id == kCases[i].m_nExpect, kCases[i].m_pWhat);
        }
    }

    printf("\n--- footsteps come from the table ---\n");
    {
        const char* pWoodStep  = surf.Footstep(30, true, 0);
        const char* pStoneStep = surf.Footstep(11, true, 0);
        printf("        wood  right foot: %s\n", pWoodStep);
        printf("        stone right foot: %s\n", pStoneStep);
        check(pWoodStep && *pWoodStep, "wood has a footstep sound");
        check(pStoneStep && *pStoneStep, "and so does rough stone");
        check(strcmp(pWoodStep, pStoneStep) != 0,
              "and they are DIFFERENT -- the surface actually selects the sound");

        check(strcmp(surf.Footstep(30, true, 0), surf.Footstep(30, false, 0)) != 0,
              "left and right feet differ");

        // ⚠️ An id that does not exist must fall back to surface 0, which is
        // the game's own "Unknown" -- not to silence, which would make a bad
        // lookup indistinguishable from a quiet floor.
        const char* pJunk = surf.Footstep(9999, true, 0);
        check(pJunk && *pJunk, "an unknown id falls back to a real sound, not silence");
        check(strcmp(pJunk, surf.Footstep(0, true, 0)) == 0,
              "and the fallback is surface 0, the game's own Unknown");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
