// ----------------------------------------------------------------------- //
//
// propview.cpp -- points the camera at ONE world object and renders it
//                 through BOTH backends from the identical view.
//
// ⚠️ WHY THIS EXISTS. "Wall-mounted props look wrong" has been reported three
// times and four hypotheses have been eliminated (HANDOVER.md §"OPEN: SOME
// WALL-MOUNTED PROPS ARE STILL WRONG"). The next question is one bit: is a
// handle wrong in the SOFTWARE path too, or only under Metal? That halves the
// search space -- a placement bug lives in the object pass, a Metal-only bug
// lives in the GPU one -- and answering it needs the SAME view through both,
// which nothing here could do:
//
//   worldview          software only, and frames the whole level or one WORLD
//                      MODEL -- objects are a different list entirely
//   enginetest_metal   both backends, but hard-wired to the spawn point, which
//                      is not where any knob is
//   walk mode          both backends, but the camera is a human's hands
//
// ⚠️ IT DELIBERATELY DOES NOT COMPARE THE TWO PICTURES NUMERICALLY. That is
// enginetest_metal's job and it already reports 99.7% agreement on a corridor.
// The question here is not "do the backends agree" but "is the prop in the
// right PLACE", and only a human looking at the two images can say that -- a
// prop rotated 90 degrees about its own axis in BOTH backends agrees perfectly
// and is still wrong. This tool's output is evidence for a person, so it also
// prints the numbers behind the placement rather than only the pixels.
//
// USAGE
//   propview --dirs a:b:c --world "WORLDS\M01S01.DAT" --match Doorknob
//              [--index N] [--dist 60] [--angle 0] [--elev 10]
//              [--size 640x480] [--out DIR]
//   propview --dirs a:b:c --world "WORLDS\M02S03.DAT" --spawn [--orbit 4] [--gpu]
//
// --spawn stands at the level's GameStartPoint instead of framing a prop, and
// --orbit N turns through N views from there. ⚠️ --gpu renders each of those
// views through METAL as well and reports the agreement: enginetest_metal's
// single comparison view is M01S01's spawn corridor, and a backend feature
// that only appears on, say, window glass needs a view with windows in it.
//
// With no --index it LISTS every matching object and stops. That listing is
// half the tool: it prints the Pos, the Euler angles in degrees, the Scale and
// the MoveToFloor drop the renderer actually applied, so a placement can be
// argued about before a single pixel is drawn.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltworldrender.h"
#include "ltmetal.h"
#include "ltfilemgr.h"
#include "ltabc.h"
#include "ltmodeldraw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/time.h>
#include <algorithm>
#include <utility>

// ⚠️ Same two stubs as enginetest_metal, for the same reason: the window layer
// is linked for its NSView and its handlers call the input layer, but nothing
// here opens a window so no event can reach them. Pulling in the Win32 input
// shim would make a renderer probe depend on the input stack.
struct LTInputEvent;
void LTInput_Push(const LTInputEvent&) {}
int  LTInput_MacKeyToVK(int) { return 0; }

static const float kPi = 3.14159265358979f;

static bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
    {
        size_t j = 0;
        for (; j < needle.size(); ++j)
            if (tolower((unsigned char)hay[i+j]) != tolower((unsigned char)needle[j])) break;
        if (j == needle.size()) return true;
    }
    return false;
}

static bool WritePPM(const char* pPath, const unsigned char* pRGBA, int w, int h)
{
    FILE* f = fopen(pPath, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w*h; ++i) fwrite(&pRGBA[(size_t)i*4], 1, 3, f);
    fclose(f);
    return true;
}

// How much of the frame the object could possibly be. Not a correctness check
// -- a number that says whether the camera found anything at all, so an empty
// image is reported as empty instead of being squinted at.
static long long CountLit(const std::vector<unsigned char>& b)
{
    long long n = 0;
    for (size_t i = 0; i < b.size(); i += 4)
        if (b[i] > 24 || b[i+1] > 24 || b[i+2] > 24) ++n;
    return n;
}

// ⚠️ WHICH BRUSH IS THIS PROP MOUNTED ON. A wall prop's placement can only be
// judged against the thing it is mounted to: "the lever points toward +Z" is
// not right or wrong until you know where the door's hinge edge is. The world
// file does not link them (HANDOVER.md §"DOOR HANDLES ARE NOT PART OF THE
// DOOR"), so the association is recovered the same way CLTWorldDoors recovers
// it -- by distance to the BOX, not to its centre, because a door is a wide
// flat slab and a knob sits near one edge.
static float DistToBox(const float* p, const float* mn, const float* mx)
{
    float d2 = 0.0f;
    for (int k = 0; k < 3; ++k)
    {
        const float v = p[k] < mn[k] ? mn[k] - p[k] : (p[k] > mx[k] ? p[k] - mx[k] : 0.0f);
        d2 += v * v;
    }
    return sqrtf(d2);
}

// ⚠️ THE MEASUREMENT THIS TOOL EXISTS FOR. A picture says a wall prop "looks
// wrong"; this says by how much and in which direction, which is the
// difference between a report and a diagnosis.
//
// It rebuilds the SAME matrix the object pass builds -- Ry*Rx*Rz with the
// translation in the last column, scale on the columns -- transforms the
// model's own vertices, and prints the world box against the box of the brush
// the prop is mounted on. A door lever that runs off the edge of its door is
// then a signed overhang in units, not an impression.
//
// ⚠️ It duplicates WorldMatrix rather than calling it, because that function is
// file-static in ltworldrender.cpp. That is a real duplication risk of exactly
// the kind HANDOVER.md warns about ("one decision path, two backends"), so the
// tool asserts nothing on its own numbers -- it reports them next to the
// picture the renderer actually drew, and the two must agree by eye. If this
// ever needs to gate a test, lift WorldMatrix into a header first.
static void ObjMatrix(const float* pos, const float* eul, const float* pScale3, float* out)
{
    const float cy = cosf(eul[1]), sy = sinf(eul[1]);
    const float cx = cosf(eul[0]), sx = sinf(eul[0]);
    const float cz = cosf(eul[2]), sz = sinf(eul[2]);

    float m[9] = {  cy*cz + sy*sx*sz, -cy*sz + sy*sx*cz,  sy*cx,
                    cx*sz,             cx*cz,            -sx,
                   -sy*cz + cy*sx*sz,  sy*sz + cy*sx*cz,  cy*cx };

    LT3D::Identity(out);
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c) out[r*4 + c] = m[r*3 + c];
        out[r*4 + 3] = pos[r];
    }
    if (pScale3)
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) out[r*4 + c] *= pScale3[c];
}

static bool ModelWorldBox(CLTFileMgr* pFM, const char* pFile,
                          float* pM16, float* pMin3, float* pMax3,
                          float* pLocalMin3, float* pLocalMax3)
{
    CLTABCModel abc;
    if (!abc.LoadThroughFileMgr(pFM, pFile)) return false;

    // ⚠️ The model's declared origin offset, applied EXACTLY as the object
    // pass applies it (ltworldrender.cpp). This tool duplicates the matrix
    // build, so it has to duplicate this too or its numbers quietly describe a
    // different placement from the one being rendered beside them -- which is
    // worse than printing nothing.
    float ofs[3] = { 0.0f, 0.0f, 0.0f };
    if (abc.GetOriginOffset(0, ofs) && LTModelDraw::MirrorX()) ofs[0] = -ofs[0];
    if ((ofs[0] != 0.0f || ofs[1] != 0.0f || ofs[2] != 0.0f) &&
        (ofs[0] != 0.0f || ofs[1] != 0.0f || ofs[2] != 0.0f))
    {
        float d[3];
        for (int r = 0; r < 3; ++r)
            d[r] = pM16[r*4+0]*ofs[0] + pM16[r*4+1]*ofs[1] + pM16[r*4+2]*ofs[2];
        for (int r = 0; r < 3; ++r) pM16[r*4+3] -= d[r];
        printf("    origin offset declared by the model: (%.3f %.3f %.3f) -- applied\n",
               ofs[0], ofs[1], ofs[2]);
    }

    bool bAny = false;
    const std::vector<LTABCPiece>& pc = abc.GetPieces();
    for (size_t i = 0; i < pc.size(); ++i)
    {
        if (pc[i].m_Lods.empty()) continue;
        const std::vector<LTABCVertex>& v = pc[i].m_Lods[0].m_Verts;   // LOD 0 only

        // ⚠️ Honour LT_MODELMIRROR in the same ORDER LTModelDraw does -- the
        // mesh is mirrored in model space FIRST, then the object matrix (which
        // already carries the origin offset) is applied. Getting the order
        // wrong here would print numbers for a placement nothing renders.
        const bool bMir = LTModelDraw::MirrorX();

        for (size_t k = 0; k < v.size(); ++k)
        {
            float qm[3] = { v[k].m_Pos[0], v[k].m_Pos[1], v[k].m_Pos[2] };
            if (bMir) qm[0] = -qm[0];
            const float* q = qm;
            float w[3];
            for (int r = 0; r < 3; ++r)
                w[r] = pM16[r*4+0]*q[0] + pM16[r*4+1]*q[1] + pM16[r*4+2]*q[2] + pM16[r*4+3];
            for (int r = 0; r < 3; ++r)
            {
                if (!bAny || q[r] < pLocalMin3[r]) pLocalMin3[r] = q[r];
                if (!bAny || q[r] > pLocalMax3[r]) pLocalMax3[r] = q[r];
                if (!bAny || w[r] < pMin3[r]) pMin3[r] = w[r];
                if (!bAny || w[r] > pMax3[r]) pMax3[r] = w[r];
            }
            bAny = true;
        }
    }
    return bAny;
}

static void ReportNearbyBrushes(const CLTWorld& world, const float* pPos, float fMax)
{
    const std::vector<LTWorldModelHeader>& mdl = world.GetModels();
    // ⚠ NOT `near`: windows.h defines `near` and `far` as empty macros, so a
    // variable of that name expands to nothing and the errors point at the
    // line AFTER it. Same family as HANDOVER.md §4.3.
    std::vector<std::pair<float, size_t> > found;
    for (size_t i = 0; i < mdl.size(); ++i)
    {
        if (!mdl[i].m_bBodyParsed) continue;
        const float d = DistToBox(pPos, mdl[i].m_Min, mdl[i].m_Max);
        if (d <= fMax) found.push_back(std::make_pair(d, i));
    }
    std::sort(found.begin(), found.end());

    printf("    brushes within %.0f units (nearest first):\n", fMax);
    if (found.empty()) { printf("      (none -- the prop is not mounted on anything parsed)\n"); return; }

    for (size_t k = 0; k < found.size() && k < 6; ++k)
    {
        const LTWorldModelHeader& m = mdl[found[k].second];
        printf("      %6.1f  %-24s box (%7.1f %7.1f %7.1f)..(%7.1f %7.1f %7.1f)"
               "  size (%6.1f %6.1f %6.1f)%s\n",
               found[k].first, m.m_sName.c_str(),
               m.m_Min[0], m.m_Min[1], m.m_Min[2],
               m.m_Max[0], m.m_Max[1], m.m_Max[2],
               m.m_Max[0]-m.m_Min[0], m.m_Max[1]-m.m_Min[1], m.m_Max[2]-m.m_Min[2],
               (m.m_nInfoFlags & 2) ? "  [MOVEABLE]" : "");
    }
}

int main(int argc, char** argv)
{
    std::string sDirs, sWorld = "WORLDS\\M01S01.DAT", sMatch, sOut = ".";
    int   nIndex = -1, W = 640, H = 480, nOrbit = 0;
    bool  bSpawn = false;
    bool  bGpu   = false;
    float fLift = 0.0f;
    float fDist = 60.0f, fAngle = 0.0f, fElev = 8.0f;

    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i+1] : 0;
        if      (!strcmp(a, "--dirs")  && v) { sDirs  = v; ++i; }
        else if (!strcmp(a, "--world") && v) { sWorld = v; ++i; }
        else if (!strcmp(a, "--match") && v) { sMatch = v; ++i; }
        else if (!strcmp(a, "--out")   && v) { sOut   = v; ++i; }
        else if (!strcmp(a, "--index") && v) { nIndex = atoi(v); ++i; }
        else if (!strcmp(a, "--dist")  && v) { fDist  = (float)atof(v); ++i; }
        else if (!strcmp(a, "--angle") && v) { fAngle = (float)atof(v); ++i; }
        else if (!strcmp(a, "--elev")  && v) { fElev  = (float)atof(v); ++i; }
        else if (!strcmp(a, "--size")  && v) { sscanf(v, "%dx%d", &W, &H); ++i; }
        else if (!strcmp(a, "--orbit")  && v) { nOrbit = atoi(v); ++i; }
        else if (!strcmp(a, "--spawn"))       { bSpawn = true; }
        else if (!strcmp(a, "--gpu"))         { bGpu   = true; }
        else if (!strcmp(a, "--lift")   && v) { fLift  = (float)atof(v); ++i; }
        else { fprintf(stderr, "unknown argument %s\n", a); return 2; }
    }
    if (sDirs.empty()) { fprintf(stderr, "usage: propview --dirs a:b:c --match Doorknob [--index N]\n"); return 2; }

    std::vector<std::string> dirs;
    for (size_t p = 0; p <= sDirs.size(); )
    {
        size_t q = sDirs.find(':', p);
        if (q == std::string::npos) q = sDirs.size();
        if (q > p) dirs.push_back(sDirs.substr(p, q - p));
        p = q + 1;
    }
    std::vector<const char*> cdirs;
    for (size_t i = 0; i < dirs.size(); ++i) cdirs.push_back(dirs[i].c_str());

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&cdirs[0], (int)cdirs.size()) == 0)
    { fprintf(stderr, "no game data under %s\n", sDirs.c_str()); return 2; }

    CLTWorldScene scene;
    if (!scene.Load(&fm, sWorld.c_str()))
    { fprintf(stderr, "world %s: %s\n", sWorld.c_str(), scene.GetError()); return 2; }

    // ---- stand where the level actually starts -------------------------- //
    //
    // ⚠️ THE LEVEL'S OWN SPAWN, not a viewpoint chosen to flatter it. NOLF's
    // `GameStartPoint` carries the position AND the yaw the player faces, and
    // both are used: a still from an invented vantage point can make a broken
    // level look fine, which is precisely the failure this tool exists to
    // avoid.
    //
    // ⚠️ And it TURNS ON THE SPOT rather than orbiting a target. Standing in a
    // corridor and looking around is what a player does; orbiting a point is
    // what a modeller does, and the two show different bugs.
    if (bSpawn)
    {
        float pos[3], yaw = 0.0f;
        if (!scene.GetSpawn(pos, &yaw))
        { fprintf(stderr, "%s has no GameStartPoint\n", sWorld.c_str()); return 2; }

        // ⚠️ A SPAWN POINT CAN BE INSIDE GEOMETRY, and then every heading
        // renders one flat surface at arm's length -- which measures as "this
        // world draws black" and is a fact about the CAMERA, not the level.
        // Walk mode lifts the body out at startup for the same reason
        // (ltworldwalk.cpp); this is the still-camera equivalent, manual
        // because a probe should not quietly relocate itself without saying so.
        float eye[3] = { pos[0], pos[1] + CLTWorldScene::EyeHeight() + fLift, pos[2] };
        if (fLift != 0.0f) printf("  (eye lifted %+.0f)\n", fLift);
        printf("spawn (%.1f %.1f %.1f) yaw %.1f deg -- eye at (%.1f %.1f %.1f)\n",
               pos[0], pos[1], pos[2], yaw * 180.0f / kPi, eye[0], eye[1], eye[2]);

        // ⚠️ Warm the caches BEFORE timing anything. Without this the first
        // heading pays for decoding every texture the level uses and reads
        // 86 ms against 20 ms for its neighbours -- a number that says nothing
        // about the renderer and everything about the cache being cold.
        {
            int nTex = 0, nMdl = 0;
            struct timeval p0, p1;
            gettimeofday(&p0, 0);
            scene.PreloadAll(&nTex, &nMdl);
            gettimeofday(&p1, 0);
            const double pms = (p1.tv_sec - p0.tv_sec) * 1000.0
                             + (p1.tv_usec - p0.tv_usec) / 1000.0;
            printf("  preloaded %d texture refs and %d prop models in %.0f ms\n",
                   nTex, nMdl, pms);
        }

        const int n = nOrbit > 0 ? nOrbit : 1;
        std::vector<unsigned char> rgba((size_t)W*H*4, 0);
        std::vector<float>         depth((size_t)W*H, 0.0f);

        for (int k = 0; k < n; ++k)
        {
            // Start at the spawn's OWN facing and turn from there.
            const float a2 = yaw + (2.0f * kPi * (float)k) / (float)n;
            const float e2 = fElev * kPi / 180.0f;
            float at[3] = { eye[0] + cosf(e2) * sinf(a2),
                            eye[1] + sinf(e2),
                            eye[2] + cosf(e2) * cosf(a2) };

            std::fill(rgba.begin(), rgba.end(), (unsigned char)0);
            std::fill(depth.begin(), depth.end(), 0.0f);

            // ⚠️ Wall clock around ONE frame. Not a benchmark suite -- enough to
            // answer "is this playable", which is the only question a still
            // renderer can honestly answer about speed.
            struct timeval t0, t1;
            gettimeofday(&t0, 0);
            CLTWorldScene::Stats st = scene.Render(&rgba[0], &depth[0], W, H, eye, at, 1.4f);
            gettimeofday(&t1, 0);
            const double ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                            + (t1.tv_usec - t0.tv_usec) / 1000.0;

            char nm[1024];
            snprintf(nm, sizeof nm, "%s/spawn_%02d.ppm", sOut.c_str(), k);
            WritePPM(nm, &rgba[0], W, H);
            printf("  %3.0f deg  %6d world tris, %5d obj tris, %3d objects, %6lld lit px, %7.1f ms  -> %s\n",
                   (a2 - yaw) * 180.0f / kPi, st.m_nWorldTris, st.m_nObjTris,
                   st.m_nObjects, CountLit(rgba), ms, nm);

            // ⚠️ --gpu RENDERS THE SAME VIEW THROUGH METAL, and it exists for a
            // narrow reason: enginetest_metal's one comparison view is M01S01's
            // spawn corridor, which has 48 transparent polygons and almost no
            // glass in it. A backend feature that only shows up on windows
            // needs a view with windows, and this puts the two pictures side by
            // side anywhere the walk camera can stand.
            //
            // The number is a sanity check, not a gate -- the `metal` suite is
            // the gate. What it catches is a whole pass silently not running,
            // which is what a missing GPU texture handle looks like.
            if (bGpu)
            {
                char err[256] = { 0 };
                if (!LTMetal_SelfTest(err, sizeof err))
                {
                    printf("           Metal: UNAVAILABLE (%s) -- no comparison\n",
                           err[0] ? err : "?");
                }
                else
                {
                    std::vector<unsigned char> gpu((size_t)W*H*4, 0);
                    if (!scene.RenderMetalOffscreen(&gpu[0], W, H, eye, at, 1.4f))
                        printf("           Metal: offscreen render FAILED\n");
                    else
                    {
                        double  fSum = 0.0;
                        long long nClose = 0;
                        for (size_t q = 0; q < rgba.size(); q += 4)
                        {
                            int d = 0;
                            for (int c = 0; c < 3; ++c)
                            {
                                int e = (int)rgba[q+c] - (int)gpu[q+c];
                                if (e < 0) e = -e;
                                fSum += e;
                                if (e > d) d = e;
                            }
                            if (d <= 16) ++nClose;
                        }
                        const double fPix = (double)(W*H);
                        snprintf(nm, sizeof nm, "%s/spawn_%02d_gpu.ppm", sOut.c_str(), k);
                        WritePPM(nm, &gpu[0], W, H);
                        printf("           Metal: %6lld lit px, mean |diff| %.2f, "
                               "%.1f%% agree closely  -> %s\n",
                               CountLit(gpu), fSum / (fPix * 3.0),
                               100.0 * (double)nClose / fPix, nm);
                    }
                }
            }
        }
        return 0;
    }

    // ---- find the matching objects -------------------------------------- //
    //
    // Matched on the CLASS or the model FILENAME, because the two say different
    // things: `DoorKnob` is a class with 25 instances in M01S01, `Doorknob_01L`
    // is one model that several classes could use. Being able to ask either way
    // is what makes it possible to tell a class bug from a model bug.
    //
    // ⚠️ THE MODEL NAME COMES FROM THE RENDERER'S OWN RESOLUTION, not from the
    // object's `Filename` property. A `PropType` object carries its model in
    // Attributes\PropTypes.txt under its `Type`, so its `Filename` is empty --
    // and matching on that made this tool blind to every PropType-driven prop.
    // It reported "0 match" for `Lamp_05`, which has 84 placements. Sharing
    // CLTWorldScene::ResolveObjectModel means the tool and the renderer cannot
    // disagree about what is on screen.
    const std::vector<LTWorldObject>& obj = scene.GetWorld().GetObjects();
    std::vector<size_t> hits;
    for (size_t i = 0; i < obj.size(); ++i)
    {
        std::string sFile, sSkinIgn;
        if (!scene.ResolveObjectModel(i, sFile, sSkinIgn)) sFile.clear();
        if (ContainsNoCase(obj[i].m_sType, sMatch) || ContainsNoCase(sFile, sMatch))
            hits.push_back(i);
    }

    printf("%s: %zu objects, %zu match \"%s\"\n",
           sWorld.c_str(), obj.size(), hits.size(), sMatch.c_str());

    if (nIndex < 0 || nIndex >= (int)hits.size())
    {
        if (nIndex >= 0) fprintf(stderr, "index %d out of range\n", nIndex);
        for (size_t k = 0; k < hits.size(); ++k)
        {
            const LTWorldObject& o = obj[hits[k]];
            float pos[3] = {0,0,0}, eul[3] = {0,0,0};
            o.GetPos(pos); o.GetEuler(eul);
            const LTWorldProperty* pF  = o.Find("Filename");
            const LTWorldProperty* pV  = o.Find("Visible");
            const LTWorldProperty* pSc = o.Find("Scale");
            const LTWorldProperty* pMF = o.Find("MoveToFloor");
            printf("  [%2zu] %-16s %-22s pos (%8.1f %8.1f %8.1f)  "
                   "euler deg (%7.1f %7.1f %7.1f)  scale %.2f  drop %6.1f  %s%s%s\n",
                   k, o.m_sType.c_str(), o.GetName() ? o.GetName() : "?",
                   pos[0], pos[1], pos[2],
                   eul[0]*180.0f/kPi, eul[1]*180.0f/kPi, eul[2]*180.0f/kPi,
                   pSc ? pSc->m_Vec[0] : 1.0f,
                   scene.GetFloorDrop(hits[k]),
                   pF ? pF->m_sString.c_str() : "(no Filename)",
                   (pV && !pV->m_bBool) ? "  [INVISIBLE]" : "",
                   (pMF && pMF->m_bBool) ? "  [MoveToFloor]" : "");
        }
        printf("\nPass --index N to render one through both backends.\n");
        return 0;
    }

    // ---- aim at the chosen one ------------------------------------------ //
    const size_t iObj = hits[nIndex];
    const LTWorldObject& o = obj[iObj];
    float target[3] = {0,0,0}, eul[3] = {0,0,0};
    o.GetPos(target); o.GetEuler(eul);

    // ⚠️ The DRAWN position, not the authored one. The object pass adds the
    // MoveToFloor drop before it builds the matrix, so a camera aimed at the
    // authored Pos would frame empty air for any object that moved -- and
    // "the prop is not where I am looking" is precisely the bug under test,
    // so the probe must not be able to manufacture it.
    target[1] += scene.GetFloorDrop(iObj);

    const float a = fAngle * kPi / 180.0f;
    const float e = fElev  * kPi / 180.0f;
    float eye[3] = { target[0] + fDist * cosf(e) * sinf(a),
                     target[1] + fDist * sinf(e),
                     target[2] + fDist * cosf(e) * cosf(a) };

    const LTWorldProperty* pF = o.Find("Filename");
    printf("\n[%d] %s \"%s\"  %s\n", nIndex, o.m_sType.c_str(),
           o.GetName() ? o.GetName() : "?",
           pF ? pF->m_sString.c_str() : "(no Filename)");
    printf("    drawn at (%.1f %.1f %.1f), euler deg (%.1f %.1f %.1f), drop %.1f\n",
           target[0], target[1], target[2],
           eul[0]*180.0f/kPi, eul[1]*180.0f/kPi, eul[2]*180.0f/kPi,
           scene.GetFloorDrop(iObj));
    printf("    eye   at (%.1f %.1f %.1f), dist %.0f, angle %.0f deg, elev %.0f deg\n",
           eye[0], eye[1], eye[2], fDist, fAngle, fElev);

    ReportNearbyBrushes(scene.GetWorld(), target, 40.0f);

    // ⚠️ EVERY property, not a chosen few. The door-association work dumped a
    // knob's properties looking for a link to a door and found none; that is a
    // different question from "is there an orientation input we are ignoring",
    // and a selective dump cannot answer the second one.
    printf("    all %zu properties:\n", o.m_Props.size());
    for (size_t k = 0; k < o.m_Props.size(); ++k)
    {
        const LTWorldProperty& pr = o.m_Props[k];
        printf("      %-18s type %2u  str \"%s\"  vec (%.2f %.2f %.2f)  eul (%.4f %.4f %.4f)  real %.3f  int %d  bool %d\n",
               pr.m_sName.c_str(), pr.m_nType, pr.m_sString.c_str(),
               pr.m_Vec[0], pr.m_Vec[1], pr.m_Vec[2],
               pr.m_Euler[0], pr.m_Euler[1], pr.m_Euler[2],
               pr.m_fReal, pr.m_nInt, (int)pr.m_bBool);
    }

    // ---- where the prop's geometry actually lands ----------------------- //
    if (pF)
    {
        float scale[3] = { 1.0f, 1.0f, 1.0f };
        if (const LTWorldProperty* pSc = o.Find("Scale"))
            for (int k = 0; k < 3; ++k) if (pSc->m_Vec[k] > 0.0f) scale[k] = pSc->m_Vec[k];

        float M[16];
        ObjMatrix(target, eul, scale, M);

        float wmin[3], wmax[3], lmin[3], lmax[3];
        if (ModelWorldBox(&fm, pF->m_sString.c_str(), M, wmin, wmax, lmin, lmax))
        {
            printf("    model box, model space : (%7.1f %7.1f %7.1f)..(%7.1f %7.1f %7.1f)\n",
                   lmin[0], lmin[1], lmin[2], lmax[0], lmax[1], lmax[2]);
            printf("    model box, WORLD space : (%7.1f %7.1f %7.1f)..(%7.1f %7.1f %7.1f)\n",
                   wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2]);

            // ⚠️ THE NUMBER THAT DECIDES IT. Against the nearest MOVEABLE brush
            // -- the door -- a prop mounted ON that door must sit inside its
            // box in the two directions that span the door's face. A positive
            // overhang means geometry hanging off the edge into the frame,
            // which is what "the handle looks wrong" has been describing.
            const std::vector<LTWorldModelHeader>& mdl = scene.GetWorld().GetModels();
            int iBest = -1; float fBest = 1e30f;
            for (size_t i = 0; i < mdl.size(); ++i)
            {
                if (!mdl[i].m_bBodyParsed) continue;
                if (!(mdl[i].m_nInfoFlags & 2)) continue;              // MOVEABLE only
                const float d = DistToBox(target, mdl[i].m_Min, mdl[i].m_Max);
                if (d < fBest) { fBest = d; iBest = (int)i; }
            }
            if (iBest >= 0 && fBest <= 40.0f)
            {
                const LTWorldModelHeader& b = mdl[iBest];
                printf("    overhang past %s (positive = hanging off the edge):\n", b.m_sName.c_str());
                const char* ax = "XYZ";
                for (int k = 0; k < 3; ++k)
                    printf("      %c: low %+7.1f   high %+7.1f\n",
                           ax[k], b.m_Min[k] - wmin[k], wmax[k] - b.m_Max[k]);
            }
        }
        else printf("    (could not load %s to measure it)\n", pF->m_sString.c_str());
    }

    std::vector<unsigned char> soft((size_t)W*H*4, 0), gpu((size_t)W*H*4, 0);
    std::vector<float>         depth((size_t)W*H, 0.0f);

    CLTWorldScene::Stats st = scene.Render(&soft[0], &depth[0], W, H, eye, target, 1.4f);
    printf("    software: %d world tris, %d object tris, %d objects, %lld lit px\n",
           st.m_nWorldTris, st.m_nObjTris, st.m_nObjects, CountLit(soft));

    char nm[1024];
    snprintf(nm, sizeof nm, "%s/soft.ppm", sOut.c_str());
    if (!WritePPM(nm, &soft[0], W, H)) { fprintf(stderr, "cannot write %s\n", nm); return 2; }
    printf("    wrote %s\n", nm);

    // ⚠️ No Metal device is a SKIP, not a failure -- and it must be loud,
    // because the whole point of the run is the comparison. Half an answer
    // reported as a whole one is how the wrong subsystem gets blamed.
    char err[256] = { 0 };
    if (!LTMetal_SelfTest(err, sizeof err))
    {
        printf("    Metal: UNAVAILABLE (%s) -- software image only, the "
               "comparison did NOT happen\n", err[0] ? err : "?");
        return 0;
    }

    if (!scene.RenderMetalOffscreen(&gpu[0], W, H, eye, target, 1.4f))
    { fprintf(stderr, "    Metal: offscreen render failed\n"); return 1; }

    printf("    Metal:    %lld lit px\n", CountLit(gpu));
    snprintf(nm, sizeof nm, "%s/gpu.ppm", sOut.c_str());
    if (!WritePPM(nm, &gpu[0], W, H)) { fprintf(stderr, "cannot write %s\n", nm); return 2; }
    printf("    wrote %s\n", nm);
    return 0;
}
