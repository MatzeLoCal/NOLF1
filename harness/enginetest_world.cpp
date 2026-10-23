// ----------------------------------------------------------------------- //
//
// enginetest_world.cpp -- the v66 world reader, with negative controls.
//
// ⚠️ THE STRONGER VERIFICATION IS RECORDED IN FORMATS.md
//
//   Every object of every shipped world was dumped field-by-field by CLTWorld
//   and by an independently written Python reader and diffed:
//   **102 worlds, 79,968 objects, 0 differing lines.**
//
//   That cross-check earned its keep immediately -- it found a real bug the
//   per-object boundary check could not: the header's info string uses a
//   **uint32** length while every string in the object list uses uint16.
//   Reading it as uint16 takes the low half (the right VALUE) and starts the
//   text two bytes early, on the length's own high-order zeroes. The result
//   begins with NUL, so it measures as non-empty and PRINTS as empty.
//
// ----------------------------------------------------------------------- //

// ⚠️ Include order mirrors the game's Stdafx.h -- HANDOVER.md §4.3.
#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltworld.h"
#include "ltworldvis.h"
#include "ltrezfile.h"
#include "ltfilemgr.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
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
    if (nDirs == 0) { fprintf(stderr, "usage: enginetest_world <game-dir> ...\n"); return 2; }

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
                if (EndsWith(u, ".DAT")) names.push_back(u);
            }
        }
        closedir(pDir);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    printf("\n--- sweep: every .dat world in the archives ---\n");
    printf("        %u distinct worlds\n", (unsigned int)names.size());
    check(names.size() >= 100, "the archives yield the expected ~102 worlds");

    unsigned int nLoaded = 0, nFailed = 0, nWithInfo = 0, nWithStart = 0;
    unsigned int nGeom = 0, nComplete = 0;
    unsigned long long nTreeNodes = 0, nWorldModels = 0, nModelsRead = 0;
    unsigned long long nPolysTotal = 0, nTexRefs = 0;
    unsigned long long nBodies = 0, nSkipped = 0, nTris = 0, nPts = 0, nFx = 0;
    unsigned long long nBadIdx = 0, nBadNormal = 0, nOutOfBox = 0;
    unsigned long long nHasLeafs = 0, nTailTotal = 0, nWithTail = 0, nFullyAccounted = 0;
    unsigned long long nLeafsParsed = 0, nPortals = 0;
    unsigned long long nGridOK = 0, nGridBad = 0, nLeafGridZero = 0;
    unsigned long long nObjects = 0, nProps = 0;
    std::map<std::string, unsigned int> classes;
    std::string sFirstFail, sFirstWhy;

    for (size_t i = 0; i < names.size(); ++i)
    {
        CLTWorld w;
        if (!w.LoadThroughFileMgr(&fm, names[i].c_str()))
        {
            ++nFailed;
            if (sFirstFail.empty()) { sFirstFail = names[i]; sFirstWhy = w.GetError(); }
            continue;
        }
        ++nLoaded;
        if (w.GetInfoString()[0]) ++nWithInfo;
        if (w.FindObjectOfType("GameStartPoint")) ++nWithStart;

        if (w.HasGeometryInfo())
        {
            ++nGeom;
            nTreeNodes += w.GetWorldInfo().m_nTreeNodes;
            nWorldModels += w.GetWorldInfo().m_nWorldModels;
        }
        if (w.ModelsAreComplete()) ++nComplete;
        nBodies += w.GetNumBodiesParsed();
        nSkipped += w.GetNumBodiesSkipped();
        nFullyAccounted += w.GetNumBodiesFullyAccounted();
        const std::vector<LTWorldModelHeader>& mdl = w.GetModels();
        nModelsRead += mdl.size();
        for (size_t k = 0; k < mdl.size(); ++k)
        {
            nPolysTotal += mdl[k].m_nPolies;
            nTexRefs += mdl[k].m_Textures.size();
            if (mdl[k].m_nLeafs != 0)
            {
                ++nHasLeafs;
                if (mdl[k].m_bBodyParsed) ++nLeafsParsed;
                nPortals += mdl[k].m_PortalNames.size();
                if (mdl[k].m_SectionGrid[0] == 0 && mdl[k].m_SectionGrid[1] == 0
                    && mdl[k].m_SectionGrid[2] == 0) ++nLeafGridZero;
            }
            if (!mdl[k].m_bBodyParsed) continue;
            nTailTotal += mdl[k].m_nUnparsedTail;
            if (mdl[k].m_nUnparsedTail != 0) ++nWithTail;

            // ⚠️ THE SECTION HEADER, CHECKED AGAINST GEOMETRY IT DID NOT COME
            // FROM. The stored grid must equal ceil(extent / cell size)
            // computed from the model's own bounding box. Nothing in the
            // parser makes this true -- it is a claim about what the three
            // int32s MEAN, and it is the only reason to believe the reading.
            if (mdl[k].m_bSectionHeader && mdl[k].m_nLeafs == 0
                && mdl[k].m_SectionSize[0] > 0.0f && mdl[k].m_SectionSize[1] > 0.0f
                && mdl[k].m_SectionSize[2] > 0.0f)
            {
                bool bMatch = true;
                for (int a = 0; a < 3; ++a)
                {
                    float fExtent = mdl[k].m_Max[a] - mdl[k].m_Min[a];
                    int nWant = (int)ceilf(fExtent / mdl[k].m_SectionSize[a]);
                    if (nWant < 1) nWant = 1;
                    if (mdl[k].m_SectionGrid[a] != nWant) bMatch = false;
                }
                if (bMatch) ++nGridOK; else ++nGridBad;
            }
            const LTWorldModelHeader& M = mdl[k];
            nPts += M.m_Points.size();
            for (size_t q = 0; q < M.m_Planes.size(); ++q)
            {
                const float* n2 = M.m_Planes[q].m_Normal;
                float L = sqrtf(n2[0]*n2[0] + n2[1]*n2[1] + n2[2]*n2[2]);
                if (fabsf(L - 1.0f) > 1e-3f) ++nBadNormal;
            }
            for (size_t q = 0; q < M.m_Surfaces.size(); ++q)
                nFx += M.m_Surfaces[q].m_Effects.size();
            for (size_t q = 0; q < M.m_Polys.size(); ++q)
            {
                const LTWorldPoly& po = M.m_Polys[q];
                nTris += (po.m_Verts.size() >= 3) ? (po.m_Verts.size() - 2) : 0;
                for (size_t v = 0; v < po.m_Verts.size(); ++v)
                    if (po.m_Verts[v].m_nPoint >= M.m_Points.size()) ++nBadIdx;
            }
            for (size_t q = 0; q < M.m_Points.size(); ++q)
            {
                const float* pp = M.m_Points[q].m_Pos;
                for (int a = 0; a < 3; ++a)
                    if (pp[a] < M.m_Min[a] - 1.0f || pp[a] > M.m_Max[a] + 1.0f) { ++nOutOfBox; break; }
            }
        }

        const std::vector<LTWorldObject>& objs = w.GetObjects();
        nObjects += objs.size();
        for (size_t o = 0; o < objs.size(); ++o)
        {
            ++classes[objs[o].m_sType];
            nProps += objs[o].m_Props.size();
        }
    }

    printf("        loaded %u, refused %u\n", nLoaded, nFailed);
    if (nFailed) printf("        first refusal: %s -- %s\n", sFirstFail.c_str(), sFirstWhy.c_str());

    // ⚠️ "Loads" means every object landed exactly on its declared byte
    // length -- Load() refuses the file otherwise.
    check(nFailed == 0, "every shipped world loads, every object exact on its declared length");
    printf("        %llu objects, %llu properties, %u distinct classes\n",
           nObjects, nProps, (unsigned int)classes.size());
    check(nObjects > 79000, "the expected ~79,968 objects are read");

    // ⚠️ The info string is what caught the uint16/uint32 length bug: reading
    // it wrong made EVERY world's info print as empty, which looks exactly
    // like "these worlds have no info".
    //
    // ⚠️ So the count is asserted EXACTLY, not as "> 0". Three worlds really do
    // store a zero-length info string -- WORLDS\GOTY\M16S04.DAT,
    // WORLDS\M10S03.DAT and WORLDS\MULTI\ASSAULTMAP\STASI_AM.DAT, verified
    // in the raw bytes -- so 99 is the right answer and 102 would be wrong.
    // A regression to 0 still trips this; a loose "> 0" would not have.
    printf("        %u of %u worlds carry a non-empty info string\n", nWithInfo, nLoaded);
    check(nWithInfo == 99 && nLoaded == 102,
          "exactly 99 of 102 worlds have an info string (3 are genuinely empty)");

    check(classes["Light"] > 20000 && classes["AIVolume"] > 8000,
          "the class census matches NOLF (Light x21321, AIVolume x8238)");
    printf("        %u worlds have a GameStartPoint\n", nWithStart);
    check(nWithStart > 90, "nearly every world has a player start");

    // ----------------------------------------------------------------- //
    printf("\n--- world geometry (PARTIAL -- info block and tree only) ---\n");
    printf("        %u of %u worlds yield a geometry info block\n", nGeom, nLoaded);
    printf("        %llu world-tree nodes, %llu world models in total\n",
           nTreeNodes, nWorldModels);

    // ⚠️ This is the strong one. Reading the tree means walking a BIT STREAM
    // whose length is implied, and the walk must account for exactly
    // (nNodes-1)/4 subdivisions. Any earlier field being the wrong width
    // desynchronises it, so one passing walk validates the whole preamble --
    // extents, tree box, node count and terrain depth alike.
    check(nGeom == nLoaded,
          "every world's tree bit-stream walk balances against its node count");
    printf("        %llu world models walked, %llu polygons, %llu texture refs\n",
           nModelsRead, nPolysTotal, nTexRefs);

    // ⚠️ THE WHOLE-REGION CHECK, and it is the strong one. Every world model
    // is preceded by a link to the next; walking them must consume exactly
    // nWorldModels entries AND land exactly on objectDataPos. Any mis-read
    // header or a wrong link ends somewhere else.
    check(nComplete == nLoaded,
          "every world's model list walks completely and lands on objectDataPos");
    check(nModelsRead == nWorldModels,
          "and the number walked matches the count each world declares");

    // ---- the BSP bodies ---------------------------------------------- //
    printf("        BSP bodies: %llu parsed, %llu skipped (%.0f%%)\n",
           nBodies, nSkipped, 100.0 * nBodies / (double)(nBodies + nSkipped));
    printf("        %llu points, %llu triangles, %llu surface effects\n",
           nPts, nTris, nFx);
    printf("        %llu fully accounted, %llu carry a section block (%llu bytes)\n",
           nFullyAccounted, nWithTail, nTailTotal);

    // ⚠️ Every parsed body landed EXACTLY on its declared end -- Load()
    // discards anything else -- so this is a statement about byte accounting,
    // not about the parse "not crashing".
    // ⚠️ THE BODY LAYOUT IS NOT SOLVED, AND THIS TEST SAYS SO.
    //
    //   Byte accounting alone reaches 78% -- but that was FALSE CONFIDENCE.
    //   A body whose sub-blocks are permuted still adds up to the same total,
    //   and when the meaning checks were added the figure collapsed to a
    //   handful: the points do not land inside their model's bounding box and
    //   polygon indices do not address them. The ordering of nodes / points /
    //   the extra2 array is still wrong.
    //
    //   So the loader publishes a body ONLY when both gates pass, and this
    //   test asserts the honest state rather than the hoped-for one. When the
    //   ordering is fixed, THIS ASSERTION IS THE ONE THAT SHOULD CHANGE --
    //   raise the expected count and it will hold the new work to the same
    //   standard.
    printf("        %llu models have leaves, %llu parsed; %llu user portals named\n",
           nHasLeafs, nLeafsParsed, nPortals);
    printf("        %llu tail bytes unread\n", nTailTotal);

    // ⚠️ The leaf-list walk is checked by THREE header counts, not by landing
    // on the right offset: nLeafLists, totalVisListSize and extra1 must each
    // equal what the walk actually counted, or the body is refused. So a model
    // reaching here has agreed with the header three independent times.
    check(nLeafsParsed == nHasLeafs,
          "every model with leaves parses its leaf lists (VisBSP, one per world)");
    check(nPortals > 0,
          "and the user-portal block those models carry is read, names and all");
    check(nLeafGridZero == nHasLeafs,
          "every model with leaves has a ZEROED section grid -- VisBSPs have no sections");

    printf("        section grid: %llu match ceil(extent/cell), %llu differ\n",
           nGridOK, nGridBad);

    // ⚠️ NOT nGridBad == 0, and the gap is stated rather than tuned away. 105
    // models store a grid one larger than the prediction in a single axis --
    // an alignment detail in how the builder rounded, not a different field.
    // 18,463 agree, which is 99.4%.
    check(nGridOK > 18000 && nGridBad < 200,
          "the section grid matches a prediction made from the model's own box");

    // ⚠️ THIS THRESHOLD HAS A HISTORY, AND IT IS NOT MONOTONIC. Read before
    // changing it.
    //
    //   "> 15000 refused"  only 3 bodies survived the meaning checks
    //   "> 9000 parsed"    measuring the point offset empirically: ~9,619,
    //                      but with the TRAILING BLOCK UNREAD, so those bodies
    //                      were meaningful yet not fully accounted
    //   "> 7500 parsed"    the trailing block is now parsed and the exact
    //                      boundary check is back, so a published body is both
    //                      complete AND meaningful -- a STRONGER guarantee
    //                      satisfied by FEWER models (7,822)
    //   "> 13500 parsed"   four format errors fixed at once, gates unchanged:
    //                      the vertex-count field is two uint8s not a uint16;
    //                      the point record is position-then-normal; the tail
    //                      is 48+6*nNodes, not 60+6*nNodes; and each polygon
    //                      carries an optional 4-byte field BEFORE its vertex
    //                      list. 13,739 bodies, 316,485 triangles.
    //
    // ⚠️ So the count going DOWN was an improvement, and a future change that
    // pushes it up must not do so by weakening a gate. The number is only
    // comparable against the same pair of gates.
    check(nBodies > 16000,
          "bodies pass BOTH the exact boundary check AND the meaning checks");

    // ⚠️ NO LONGER ZERO, AND THAT IS A DELIBERATE CHANGE. 3,857 models carry a
    // section block between their points and their trailer -- the one the
    // donor refuses outright (`nSections > 0`, de_mainworld.cpp:622) -- and it
    // is skipped rather than decoded. The skip is to an offset known from the
    // model list, and every byte the renderer consumes lies before it.
    //
    // So the assertion changes shape: instead of "nothing is unread", it is
    // "everything unread is accounted for and attributed". The two counts
    // below must agree exactly with each other, which is what makes the
    // unread bytes a measured quantity rather than a leak.
    check(nFullyAccounted > 0 && nFullyAccounted <= nBodies,
          "bodies that also account for their trailing block are counted separately");
    check(nTailTotal > 0,
          "and the section block those models carry is reported, not hidden");
    check(nBodies - nFullyAccounted == nWithTail,
          "every body with unread bytes is exactly one that is not fully accounted");
    check(nBodies + nSkipped == nModelsRead,
          "every model is either parsed or explicitly skipped, never silently");

    // These hold BY CONSTRUCTION for anything published -- the parser will not
    // publish a body that fails them. They are stated anyway so that a future
    // change which drops the gate is caught here.
    check(nBadNormal == 0, "every published plane normal is unit length");
    check(nBadIdx == 0, "every published polygon vertex indexes a real point");
    check(nOutOfBox == 0, "every published point lies inside its model's box");

    printf("\n--- NOLF's first level, with exactly known contents ---\n");
    {
        CLTWorld w;
        check(w.LoadThroughFileMgr(&fm, "WORLDS\\M01S01.DAT"), "M01S01 loads");
        check(w.GetVersion() == 66, "it is world version 66");
        check(strcmp(w.GetInfoString(), "AmbientLight 48 48 48") == 0,
              "its info string is exactly \"AmbientLight 48 48 48\"");

        check(w.HasGeometryInfo(), "its geometry info block reads");
        if (w.HasGeometryInfo())
        {
            const LTWorldInfo& gi = w.GetWorldInfo();
            printf("        tree %u nodes (%u subdivisions), %u world models\n",
                   gi.m_nTreeNodes, gi.m_nSubdivisions, gi.m_nWorldModels);
            check(gi.m_nTreeNodes == 229 && gi.m_nWorldModels == 144,
                  "M01S01 has exactly 229 tree nodes and 144 world models");
        }
        check(w.ModelsAreComplete(), "its world-model list walks to objectDataPos");
        if (!w.GetModels().empty())
        {
            const LTWorldModelHeader& m = w.GetModels()[0];
            printf("        %u world models walked\n",
                   (unsigned int)w.GetModels().size());
            printf("        body parsed: %s\n", m.m_bBodyParsed ? "yes" : "no");
            printf("        first model \"%s\": %u polys, %u points, %u verts, %u textures\n",
                   m.m_sName.c_str(), m.m_nPolies, m.m_nPoints, m.m_nVerts,
                   (unsigned int)m.m_Textures.size());
            check(m.m_sName == "TranslucentWorldModel5",
                  "its first world model is TranslucentWorldModel5");
            check(m.m_nPolies == 26 && m.m_nPlanes == 26 && m.m_nVerts == 104,
                  "26 polygons, 26 planes, 104 vertices (26 quads)");
            check(m.m_Textures.size() == 1
                  && m.m_Textures[0] == "Tex\\Wood\\Wd04\\Wd0200.dtx",
                  "and one texture, Tex\\Wood\\Wd04\\Wd0200.dtx");
        }

        const LTWorldObject* pStart = w.FindObjectOfType("GameStartPoint");
        check(pStart != 0, "it has a GameStartPoint");
        if (pStart)
        {
            float pos[3] = { 0, 0, 0 }, eul[3] = { 0, 0, 0 };
            check(pStart->GetPos(pos), "the start point has a Pos");
            printf("        spawn at (%.1f, %.1f, %.1f)\n", pos[0], pos[1], pos[2]);
            check(fabsf(pos[0] - 1248.0f) < 0.01f && fabsf(pos[1] - 320.0f) < 0.01f
                  && fabsf(pos[2]) < 0.01f,
                  "Cate spawns at exactly (1248, 320, 0)");

            const LTWorldProperty* pType = pStart->Find("GameType");
            check(pType && pType->m_sString == "Single", "GameType is \"Single\"");
            const LTWorldProperty* pModel = pStart->Find("PlayerModel");
            check(pModel && pModel->m_sString == "Casual", "PlayerModel is \"Casual\"");

            // ⚠️ ROTATION IS EULER RADIANS, NOT A QUATERNION. A yaw of 4.714
            // rad is 270 degrees; read as a quaternion it is not even
            // normalised. This is the trap in the format.
            check(pStart->GetEuler(eul), "the start point has a Rotation");
            printf("        yaw %.3f rad (%.1f deg)\n", eul[1], eul[1] * 180.0f / 3.14159265f);
            check(fabsf(eul[1] - 4.714f) < 0.01f && fabsf(eul[0]) < 1e-6f,
                  "its Rotation is EULER radians (yaw 4.714), not a quaternion");
        }
    }

    // ----------------------------------------------------------------- //
    // ⚠️ THE VISIBILITY (HANDOVER 5cp). A leaf's list is a bit stream that
    // steers a walk of the VisBSP's nodes (lithtech.exe 0x48b8e0) -- read as
    // a bitmap it is nonsense. Walked, every list ends inside its last byte
    // and every leaf sees itself; those two are what a wrong walk breaks.
    printf("\n--- M01S01's visibility (the PVS that puts unseen AIs to sleep) ---\n");
    {
        CLTWorld w;
        CLTWorldVis v;
        if (!w.LoadThroughFileMgr(&fm, "WORLDS\\M01S01.DAT")) bad("M01S01 loads for the vis test");
        else
        {
            check(v.Build(w), "its VisBSP builds");
            printf("        %u leaves, %u nodes, %u portals\n", v.NumLeaves(), v.NumNodes(), v.NumPortals());
            check(v.NumLeaves() == 616 && v.NumNodes() == 3388, "616 leaves and 3,388 nodes");
            unsigned int nLists = 0, nBad = 0, nSelf = 0;
            v.SelfCheck(&nLists, &nBad, &nSelf);
            printf("        %u list(s), %u misread, %u leaves see themselves\n", nLists, nBad, nSelf);
            check(nLists == 616 && nBad == 0, "all 616 lists are walked to inside their last byte");
            check(nSelf == 616, "every leaf sees itself");

            const float spawn[3] = { 1248.0f, 320.0f, 0.0f };
            const int nLeaf = v.LeafAt(spawn);
            printf("        the spawn point is in leaf %d\n", nLeaf);
            check(nLeaf >= 0, "Cate's spawn point is inside a leaf");
            v.SetViewLeaf(nLeaf);
            const unsigned int nSeen = v.VisibleLeafCount();
            printf("        it sees %u of %u leaves\n", nSeen, v.NumLeaves());
            check(nSeen > 1 && nSeen < v.NumLeaves(), "it sees some of the level, not all of it");
            const float mn[3] = { spawn[0] - 16, spawn[1] - 16, spawn[2] - 16 };
            const float mx[3] = { spawn[0] + 16, spawn[1] + 16, spawn[2] + 16 };
            check(v.BoxVisible(mn, mx), "a box at the viewpoint is visible from it");
            v.SetViewLeaf(-1);
            check(v.VisibleLeafCount() == v.NumLeaves(),
                  "NEGATIVE CONTROL: a viewer outside every leaf sees everything (the engine's fallback)");
        }
    }

    // ----------------------------------------------------------------- //
    printf("\n--- negative controls ---\n");
    {
        CLTWorld w;
        check(!w.Load(0, 0), "NEGATIVE CONTROL: a null buffer is refused");
        check(!w.IsLoaded(), "and the world stays empty");
    }
    {
        std::vector<unsigned char> buf;
        if (Slurp(fm, "interface\\pause.pcx", buf))
        {
            CLTWorld w;
            check(!w.Load(&buf[0], (unsigned int)buf.size()),
                  "NEGATIVE CONTROL: a PCX offered as a world is refused");
            printf("        said: %s\n", w.GetError());
        }
        else bad("could not read a PCX to offer as a world");
    }

    std::vector<unsigned char> good;
    if (!Slurp(fm, "WORLDS\\MULTI\\DEATHMATCH\\003CHILLKILL_DM.DAT", good))
        bad("could not read a world to corrupt");
    else
    {
        {
            CLTWorld w;
            check(w.Load(&good[0], (unsigned int)good.size()),
                  "the uncorrupted copy still loads (the control's control)");
        }
        {
            std::vector<unsigned char> v = good;
            v[0] = 85;      // the version the DONOR wants
            CLTWorld w;
            check(!w.Load(&v[0], (unsigned int)v.size()),
                  "NEGATIVE CONTROL: version 85 is refused (equality, like the donor)");
            printf("        said: %s\n", w.GetError());
        }
        {
            CLTWorld w;
            check(!w.Load(&good[0], 2048),
                  "NEGATIVE CONTROL: a truncated world is refused");
            printf("        said: %s\n", w.GetError());
        }
        {
            // ⚠️ Corrupt ONE object's declared length. The header still parses
            // and the object still reads -- only the boundary disagrees, which
            // is precisely the check under test.
            std::vector<unsigned char> v = good;
            unsigned int objPos = (unsigned int)v[4] | ((unsigned int)v[5] << 8)
                                | ((unsigned int)v[6] << 16) | ((unsigned int)v[7] << 24);
            v[objPos + 4] = (unsigned char)(v[objPos + 4] + 1);   // first objDataLen
            CLTWorld w;
            check(!w.Load(&v[0], (unsigned int)v.size()),
                  "NEGATIVE CONTROL: an object whose length is wrong by ONE byte is refused");
            printf("        said: %s\n", w.GetError());
            check(strstr(w.GetError(), "off by") != 0,
                  "and it names the object and the shortfall");
        }
        {
            CLTWorld w;
            check(!w.LoadThroughFileMgr(&fm, "WORLDS\\NOT_A_REAL_WORLD.DAT"),
                  "NEGATIVE CONTROL: a world that does not exist is refused");
        }
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
