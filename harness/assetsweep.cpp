// ----------------------------------------------------------------------- //
//
// assetsweep.cpp -- does every level's every prop actually resolve?
//
// ⚠️ WHY THIS EXISTS. The foliage bug (HANDOVER.md) was invisible for weeks
// because the props under suspicion carried no cut-outs, so nothing pointed at
// the one that did. The general form of that failure is: a level looks
// plausible while some asset silently falls back, and nobody orbits the one
// object that would show it.
//
// So this asks, for EVERY object in EVERY shipped world, the questions whose
// answers a picture would only give one prop at a time:
//
//   * does the .abc load at all?
//   * does every skin in its `Skin` list load?
//   * does the model ask for MORE texture slots than the level supplies?
//     (a piece whose texture index is past the end draws untextured -- flat
//     shaded -- which is exactly what the broken foliage looked like)
//
// ⚠️ It reports rather than asserts. A missing asset is not necessarily a port
// bug: the shipped data really does contain objects the game never draws. The
// output is a worklist to orbit, not a gate.
//
// USAGE
//   assetsweep <dir> [<dir>...]           every world
//   assetsweep <dirs...> --world NAME     just one
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltworld.h"
#include "ltproptypes.h"
#include "ltabc.h"
#include "ltdtx.h"
#include "ltrezfile.h"
#include "ltfilemgr.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

static std::string Upper(const std::string& s)
{ std::string o(s); std::transform(o.begin(), o.end(), o.begin(), ::toupper); return o; }

static bool Ends(const std::string& s, const char* e)
{ size_t n = strlen(e); return s.size() >= n && Upper(s).compare(s.size()-n, n, e) == 0; }

// The same split the object pass uses: on ';', trimmed.
static void SplitSkins(const std::string& s, std::vector<std::string>& out)
{
    size_t a = 0;
    while (a <= s.size())
    {
        size_t b = s.find(';', a);
        if (b == std::string::npos) b = s.size();
        std::string one = s.substr(a, b - a);
        a = b + 1;
        size_t f = one.find_first_not_of(" \t");
        if (f == std::string::npos) continue;
        size_t l = one.find_last_not_of(" \t");
        out.push_back(one.substr(f, l - f + 1));
        if (b == s.size()) break;
    }
}

int main(int argc, char** argv)
{
    std::vector<const char*> dirs;
    const char* pOnly = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--world") && i + 1 < argc) { pOnly = argv[++i]; }
        else dirs.push_back(argv[i]);
    }
    if (dirs.empty()) { fprintf(stderr, "usage: assetsweep <dirs...> [--world NAME]\n"); return 2; }

    CLTFileMgr fm;
    if (!fm.InitFromGameDirs(&dirs[0], (int)dirs.size())) { fprintf(stderr, "no game data\n"); return 2; }

    // ⚠️ The same authority the renderer uses. Without it this sweep measures a
    // program nobody runs.
    CLTPropTypes propTypes;
    if (!propTypes.Load(&fm)) printf("  !! Attributes\\PropTypes.txt did not load\n");
    else printf("  PropTypes.txt: %u types\n", propTypes.Count());

    // ---- every world the archives hold ---------------------------------- //
    std::vector<std::string> worlds;
    if (pOnly) worlds.push_back(pOnly);
    else
    {
        for (size_t d = 0; d < dirs.size(); ++d)
        {
            DIR* pD = opendir(dirs[d]); if (!pD) continue;
            struct dirent* e;
            while ((e = readdir(pD)) != 0)
            {
                std::string s(e->d_name);
                if (!Ends(s, ".REZ")) continue;
                std::string path = std::string(dirs[d]) + "/" + e->d_name;
                CLTRezArchive r;
                if (!r.Open(path.c_str())) continue;
                for (unsigned i = 0; i < r.GetNumEntries(); ++i)
                {
                    std::string n(r.GetEntryName(i));
                    if (Ends(n, ".DAT")) worlds.push_back(Upper(n));
                }
            }
            closedir(pD);
        }
        std::sort(worlds.begin(), worlds.end());
        worlds.erase(std::unique(worlds.begin(), worlds.end()), worlds.end());
    }

    if (getenv("LT_LIST_WORLDS"))
    {
        for (size_t k = 0; k < worlds.size(); ++k) printf("%s\n", worlds[k].c_str());
        return 0;
    }
    printf("sweeping %zu world(s)\n\n", worlds.size());

    // Caches -- one shipped model is placed in many levels.
    std::map<std::string,int> modelOk;      // 1 loaded, 0 failed
    std::map<std::string,int> texOk;
    std::map<std::string,int> modelSlots;   // highest texture index + 1

    long long nObj = 0, nDrawable = 0; int nModelOverridden = 0;
    std::map<std::string,int> rescued;
    std::vector<std::string> skyNone, skyMissing;
    std::map<std::string,int> worldTexOk;
    std::set<std::string> badWorldTex;
    long long nWorldTexRefs = 0, nWorldTexBad = 0;
    std::map<std::string,int> badWorldTexRefs;
    std::set<std::string> badModels, badTex;
    // model -> "needs N slots but a level gave M"
    std::map<std::string,std::string> shortSkins;
    int nShortInstances = 0, nWorldsFailed = 0;

    for (size_t w = 0; w < worlds.size(); ++w)
    {
        CLTWorld world;
        if (!world.LoadThroughFileMgr(&fm, worlds[w].c_str()))
        { printf("  !! %s did not load: %s\n", worlds[w].c_str(), world.GetError()); ++nWorldsFailed; continue; }

        const std::vector<LTWorldObject>& obj = world.GetObjects();

        // ---- do the ARCHITECTURE's textures resolve? -------------------- //
        //
        // ⚠️ The object-skin check above says nothing about the WORLD. Each
        // world model names its own textures, and one that fails to load
        // renders those surfaces untextured -- flat shaded walls, which look
        // like a lighting problem rather than a missing file.
        {
            const std::vector<LTWorldModelHeader>& mdl = world.GetModels();
            for (size_t i = 0; i < mdl.size(); ++i)
                for (size_t k = 0; k < mdl[i].m_Textures.size(); ++k)
                {
                    const std::string u = Upper(mdl[i].m_Textures[k]);
                    if (u.empty()) continue;
                    ++nWorldTexRefs;
                    if (worldTexOk.find(u) == worldTexOk.end())
                    {
                        CLTDtx t;
                        const bool ok = t.LoadThroughFileMgr(&fm, mdl[i].m_Textures[k].c_str());
                        worldTexOk[u] = ok ? 1 : 0;
                        if (!ok) badWorldTex.insert(u);
                    }
                    if (!worldTexOk[u]) { ++nWorldTexBad; ++badWorldTexRefs[u]; }
                }
        }

        // ---- does this world's SKY resolve? ----------------------------- //
        //
        // ⚠️ Two independent ways to lose it, and both render as BLACK where
        // the sky should be -- which reads as "the level is dark", not as a
        // missing asset:
        //   * no `SkyPointer` object at all;
        //   * its `SkyObjectName` names a world model that does not exist.
        // The renderer resolves it exactly this way (ltworldrender.cpp).
        {
            std::string sSky;
            for (size_t i = 0; i < obj.size() && sSky.empty(); ++i)
            {
                if (obj[i].m_sType != "SkyPointer") continue;
                const LTWorldProperty* p = obj[i].Find("SkyObjectName");
                if (p && !p->m_sString.empty()) sSky = p->m_sString;
            }
            if (sSky.empty()) skyNone.push_back(worlds[w]);
            else
            {
                bool bFound = false;
                const std::vector<LTWorldModelHeader>& mdl = world.GetModels();
                for (size_t i = 0; i < mdl.size() && !bFound; ++i)
                    if (mdl[i].m_sName == sSky) bFound = true;
                if (!bFound) skyMissing.push_back(worlds[w] + "  -> \"" + sSky + "\"");
            }
        }
        for (size_t i = 0; i < obj.size(); ++i)
        {
            ++nObj;
            // Resolve exactly as CLTWorldScene::ResolveObjectModel does.
            std::string sFile, sSkinList;
            bool bFromPropType = false;
            if (obj[i].m_sType == "PropType")
            {
                const LTWorldProperty* pT = obj[i].Find("Type");
                if (pT && !pT->m_sString.empty())
                    if (const CLTPropTypes::Entry* pE = propTypes.Find(pT->m_sString))
                        if (!pE->m_sFilename.empty())
                        { sFile = pE->m_sFilename; sSkinList = pE->m_sSkin; bFromPropType = true; }
            }
            const LTWorldProperty* pF = obj[i].Find("Filename");
            const LTWorldProperty* pS = obj[i].Find("Skin");
            if (!bFromPropType)
            {
                if (!pF || !Ends(pF->m_sString, ".ABC")) continue;
                sFile = pF->m_sString;
                sSkinList = pS ? pS->m_sString : std::string();
            }
            if (!Ends(sFile, ".ABC")) continue;

            // ⚠️ Objects the OLD code would have skipped entirely: a PropType
            // whose world Filename is absent or not a model, rescued by the
            // attribute file. These were simply missing from the level.
            if (bFromPropType && (!pF || !Ends(pF->m_sString, ".ABC")))
                ++rescued[worlds[w]];
            if (bFromPropType && pF && Upper(pF->m_sString) != Upper(sFile)) ++nModelOverridden;
            ++nDrawable;

            const std::string sModel = Upper(sFile);
            if (modelOk.find(sModel) == modelOk.end())
            {
                CLTABCModel m;
                const bool ok = m.LoadThroughFileMgr(&fm, sFile.c_str());
                modelOk[sModel] = ok ? 1 : 0;
                int slots = 0;
                if (ok)
                {
                    const std::vector<LTABCPiece>& pc = m.GetPieces();
                    for (size_t k = 0; k < pc.size(); ++k)
                        if ((int)pc[k].m_nTexture + 1 > slots) slots = pc[k].m_nTexture + 1;
                }
                modelSlots[sModel] = slots;
                if (!ok) badModels.insert(sModel);
            }
            if (!modelOk[sModel]) continue;

            std::vector<std::string> skins;
            SplitSkins(sSkinList, skins);

            int nGood = 0;
            for (size_t k = 0; k < skins.size(); ++k)
            {
                const std::string u = Upper(skins[k]);
                if (texOk.find(u) == texOk.end())
                {
                    CLTDtx t;
                    const bool ok = t.LoadThroughFileMgr(&fm, skins[k].c_str());
                    texOk[u] = ok ? 1 : 0;
                    if (!ok) badTex.insert(u);
                }
                if (texOk[u]) ++nGood;
            }

            const int need = modelSlots[sModel];
            if (need > nGood)
            {
                ++nShortInstances;
                char buf[256];
                snprintf(buf, sizeof buf, "needs %d texture slot(s), the level supplies %d",
                         need, nGood);
                shortSkins[sModel] = buf;

                // ⚠️ Name the actual placement. "some model somewhere is
                // short" is not something anyone can go and look at; a world,
                // a class and an object name is.
                if (nShortInstances <= 40)
                {
                    const LTWorldProperty* pV = obj[i].Find("Visible");
                    printf("  short: %-28s %-14s %-16s skin=\"%s\"%s\n",
                           worlds[w].c_str(), obj[i].m_sType.c_str(),
                           obj[i].GetName() ? obj[i].GetName() : "?",
                           pS ? pS->m_sString.c_str() : "(no Skin property)",
                           (pV && !pV->m_bBool) ? "  [INVISIBLE]" : "");
                }
            }
        }
    }

    printf("=== objects ===\n");
    printf("  %lld objects, %lld of them drawable models\n", nObj, nDrawable);
    printf("  worlds that failed to load: %d\n", nWorldsFailed);
    printf("  PropType objects whose MODEL the attribute file overrode: %d\n\n", nModelOverridden);

    {
        std::vector<std::pair<int,std::string> > v;
        int tot = 0;
        for (std::map<std::string,int>::const_iterator it = rescued.begin();
             it != rescued.end(); ++it) { v.push_back(std::make_pair(it->second, it->first)); tot += it->second; }
        std::sort(v.rbegin(), v.rend());
        printf("=== objects RESCUED by the attribute file (previously not drawn at all) ===\n");
        printf("  %d across %zu worlds; worst offenders:\n", tot, v.size());
        for (size_t k = 0; k < v.size() && k < 14; ++k)
            printf("   %5d  %s\n", v[k].first, v[k].second.c_str());
        printf("\n");
    }

    printf("=== world (architecture) textures ===\n");
    printf("  %lld references, %lld unresolved, %zu distinct names missing\n",
           nWorldTexRefs, nWorldTexBad, badWorldTex.size());
    {
        // ⚠️ Split by KIND, because the two are different problems. A `.spr` is
        // an animated sprite -- a format this port does not read at all -- and
        // a missing `.dtx` is a file the game does not ship.
        long long nSpr = 0, nDtx = 0, nOther = 0;
        std::vector<std::pair<int,std::string> > v;
        for (std::map<std::string,int>::const_iterator it = badWorldTexRefs.begin();
             it != badWorldTexRefs.end(); ++it)
        {
            v.push_back(std::make_pair(it->second, it->first));
            if      (Ends(it->first, ".SPR")) nSpr   += it->second;
            else if (Ends(it->first, ".DTX")) nDtx   += it->second;
            else                              nOther += it->second;
        }
        std::sort(v.rbegin(), v.rend());
        printf("      by kind: %lld sprite (.spr) refs, %lld missing .dtx refs, %lld other\n",
               nSpr, nDtx, nOther);
        for (size_t k = 0; k < v.size(); ++k)
            printf("      %5d  %s\n", v[k].first, v[k].second.c_str());
    }
    printf("\n");

    printf("=== sky ===\n");
    printf("  %zu world(s) have no SkyPointer at all\n", skyNone.size());
    for (size_t k = 0; k < skyNone.size() && k < 10; ++k)
        printf("      %s\n", skyNone[k].c_str());
    printf("  %zu world(s) name a sky model that does not exist\n", skyMissing.size());
    for (size_t k = 0; k < skyMissing.size() && k < 10; ++k)
        printf("      %s\n", skyMissing[k].c_str());
    printf("\n");

    printf("=== models that did not load (%zu) ===\n", badModels.size());
    for (std::set<std::string>::const_iterator it = badModels.begin(); it != badModels.end(); ++it)
        printf("  %s\n", it->c_str());

    printf("\n=== skins that did not load (%zu) ===\n", badTex.size());
    for (std::set<std::string>::const_iterator it = badTex.begin(); it != badTex.end(); ++it)
        printf("  %s\n", it->c_str());

    printf("\n=== models drawn with too few skins (%zu distinct, %d placements) ===\n",
           shortSkins.size(), nShortInstances);
    printf("    ⚠️ these render some pieces UNTEXTURED -- flat shaded, which is\n"
           "       exactly what the broken foliage looked like\n");
    for (std::map<std::string,std::string>::const_iterator it = shortSkins.begin();
         it != shortSkins.end(); ++it)
        printf("  %-46s %s\n", it->first.c_str(), it->second.c_str());

    return 0;
}
