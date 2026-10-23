// ----------------------------------------------------------------------- //
//
// enginetest_walk.cpp -- the world scene and the walk camera.
//
// ⚠️ THIS EXISTS BECAUSE WALK MODE CANNOT BE TESTED BY RUNNING IT. The host's
// scripted-key path (LT_TEST_KEYS) pushes a key DOWN and UP in the same tick,
// on purpose, so that a game tracking key state does not believe a key is held
// for the rest of the run. Movement needs the opposite -- a key that stays
// down across frames -- so no scripted run can demonstrate walking, and
// "I looked at the window and it moved" is not a check anyone else can repeat.
//
// So the camera is driven through its own public API instead, and the thing
// asserted is that the PICTURE CHANGES when a movement key is held and does
// not when it is released.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltworldrender.h"
#include "ltworldwalk.h"
#include "ltworldcollide.h"
#include "ltworlddoors.h"
#include "ltfilemgr.h"
#include "lt2d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

static int g_nPass = 0;
static int g_nFail = 0;
static void ok(const char* w)            { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)           { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w) { b ? ok(w) : bad(w); }


// ⚠️ WHICH WAY DID THE PICTURE MOVE? Returns the horizontal shift, in pixels,
// that best maps image `a` onto image `b`: negative means the content slid
// LEFT. Compared as per-column brightness profiles, which is what a horizontal
// pan actually translates -- and cheap enough to run on every frame pair.
static int BestShift(const unsigned char* a, const unsigned char* b, int W, int H)
{
    std::vector<double> pa(W, 0.0), pb(W, 0.0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const unsigned char* qa = a + ((size_t)y * W + x) * 4;
            const unsigned char* qb = b + ((size_t)y * W + x) * 4;
            pa[x] += qa[0] + qa[1] + qa[2];
            pb[x] += qb[0] + qb[1] + qb[2];
        }

    double ma = 0, mb = 0;
    for (int x = 0; x < W; ++x) { ma += pa[x]; mb += pb[x]; }
    ma /= W; mb /= W;
    for (int x = 0; x < W; ++x) { pa[x] -= ma; pb[x] -= mb; }

    // ⚠️ A DELIBERATELY NARROW SEARCH. A corridor of evenly spaced doors is
    // periodic, so a wide window lets the correlation find a confident peak one
    // whole period out -- in the wrong direction. Keeping the window well under
    // the feature spacing means a wrong answer shows up as a weak or zero shift
    // rather than a plausible backwards one.
    const int kMax = W / 8;
    int nBest = 0; double fBest = -1e300;
    for (int d = -kMax; d <= kMax; ++d)
    {
        double num = 0; int n = 0;
        for (int x = 0; x < W; ++x)
        {
            int xb = x + d;
            if (xb < 0 || xb >= W) continue;
            num += pa[x] * pb[xb];
            ++n;
        }
        if (n < W / 2) continue;
        num /= n;
        if (num > fBest) { fBest = num; nBest = d; }
    }
    return nBest;
}

int main(int argc, char** argv)
{
    std::vector<const char*> dirs;
    for (int i = 1; i < argc; ++i) dirs.push_back(argv[i]);
    if (dirs.empty()) { fprintf(stderr, "usage: enginetest_walk <dirs...>\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&dirs[0], (int)dirs.size()) == 0)
    { fprintf(stderr, "no game data\n"); return 2; }

    printf("\n--- the world scene ---\n");

    CLTWorldScene scene;
    check(scene.Load(&fm, "WORLDS\\M01S01.DAT"), "M01S01 loads as a scene");
    if (!scene.IsLoaded()) { printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail); return 1; }

    // ⚠️ The spawn is the world's, not ours. enginetest_world independently
    // asserts M01S01's GameStartPoint is at (1248, 320, 0); if these two ever
    // disagree, one of them is reading the object list wrongly.
    float pos[3] = { 0, 0, 0 }, yaw = 0.0f;
    check(scene.GetSpawn(pos, &yaw), "it has a spawn point");
    check(fabsf(pos[0] - 1248.0f) < 0.5f &&
          fabsf(pos[1] -  320.0f) < 0.5f &&
          fabsf(pos[2] -    0.0f) < 0.5f,
          "and it is exactly where the world says Cate starts");

    const int W = 160, H = 120;
    std::vector<unsigned char> rgbaA((size_t)W * H * 4, 0);
    std::vector<unsigned char> rgbaB((size_t)W * H * 4, 0);
    std::vector<float>         depth((size_t)W * H, 0.0f);

    float eye[3] = { pos[0], pos[1] + CLTWorldScene::EyeHeight(), pos[2] };
    float at[3]  = { eye[0] + sinf(yaw), eye[1], eye[2] + cosf(yaw) };

    CLTWorldScene::Stats st = scene.Render(&rgbaA[0], &depth[0], W, H, eye, at, 1.4f);
    printf("        %d world tris, %d object tris, %d objects, %d pixels\n",
           st.m_nWorldTris, st.m_nObjTris, st.m_nObjects, st.m_nPixels);

    // ⚠️ "It rendered" and "it drew nothing" must not look the same -- the
    // whole point of counting pixels rather than trusting the call returned.
    check(st.m_nWorldTris > 1000, "rendering it produces thousands of world triangles");
    check(st.m_nPixels > (W * H) / 4, "and covers a real part of the frame");
    check(st.m_nObjects > 0, "and places the level's prop models");

    printf("\n--- collision ---\n");
    {
        CLTWorldCollide col;
        col.Build(scene.GetWorld());
        printf("        %u triangles in %u cells\n", col.Triangles(), col.Cells());
        check(col.Triangles() > 1000, "the level yields thousands of solid triangles");

        const float R = 24.0f;

        // ⚠️ FALLING IS THE TEST THAT MATTERS. Drop a sphere from well above the
        // spawn with a delta far longer than the level is tall: if the resolver
        // works it lands, and if it does not it ends up at the bottom of the
        // requested move having sailed through the floor.
        float p[3] = { pos[0], pos[1] + 200.0f, pos[2] };
        float d[3] = { 0.0f, -4000.0f, 0.0f };
        bool bGround = false;
        col.MoveSphere(p, d, R, &bGround);

        check(bGround, "a sphere dropped at the spawn lands on something");
        check(p[1] > pos[1] - 200.0f,
              "and stops near the floor instead of falling through it");

        // ⚠️ The substep guarantee, stated as a test: the same move made in one
        // enormous delta must land in the same place as many small ones. Without
        // substepping the long move tunnels and these disagree wildly.
        float a[3] = { pos[0], pos[1] + 200.0f, pos[2] };
        float one[3] = { 0.0f, -4000.0f, 0.0f };
        col.MoveSphere(a, one, R, &bGround);

        float b[3] = { pos[0], pos[1] + 200.0f, pos[2] };
        for (int i = 0; i < 400; ++i)
        { float sm[3] = { 0.0f, -10.0f, 0.0f }; col.MoveSphere(b, sm, R, &bGround); }

        check(fabsf(a[1] - b[1]) < 2.0f,
              "one long fall and four hundred short ones agree -- no tunnelling");

        // A sphere resting on the floor must report itself overlapping nothing.
        check(!col.Overlaps(a, R * 0.9f),
              "and the landed sphere is not embedded in the floor");
    }

    // ================= MoveToFloor =================================== //
    printf("\n--- MoveToFloor ---\n");
    {
        const std::vector<LTWorldObject>& ob = scene.GetWorld().GetObjects();

        int nFlagged = 0, nAdjusted = 0, nLifted = 0, nUnflaggedMoved = 0, nSunk = 0;
        float fWorst = 0.0f;

        for (size_t i = 0; i < ob.size(); ++i)
        {
            // ⚠️ THE SCENE'S OWN RULE, not the world file's property (HANDOVER
            // 5bt): a PropType takes MoveToFloor from its type and its model
            // from Attributes\PropTypes.txt -- it has no Filename at all, so
            // reading the file here would call every PropType "unflagged".
            std::string sModel, sSkin;
            const bool bDrawable = scene.ResolveObjectModel(i, sModel, sSkin);
            const bool bFlagged = bDrawable && scene.ObjectMoveToFloor(i);
            const float d = scene.GetFloorDrop(i);

            if (!bFlagged) { if (d != 0.0f) ++nUnflaggedMoved; continue; }

            ++nFlagged;
            if (d == 0.0f) continue;
            ++nAdjusted;
            if (d > 0.0f) ++nLifted;
            if (-d > fWorst) fWorst = -d;

            // ⚠️ THE ANTI-SINK CHECK. The failure mode of dropping an object is
            // not that it stays up, it is that it ends up INSIDE the floor --
            // which looks like a fix from one angle and a bug from another. The
            // surface is re-found independently here rather than reusing the
            // number that placed it.
            float pos[3];
            if (!ob[i].GetPos(pos)) continue;
            const float fNewY = pos[1] + d;
            float fFloor = 0.0f;
            if (scene.FloorUnder(pos[0], pos[2], pos[1], 10000.0f, &fFloor))
                if (fNewY < fFloor - 0.5f) ++nSunk;
        }

        printf("        %d objects flagged, %d adjusted, worst drop %.1f units\n",
               nFlagged, nAdjusted, fWorst);

        check(nFlagged > 0, "M01S01 has objects that ask to be moved to the floor");
        check(nAdjusted > 0, "and some of them are actually moved");

        // ⚠️ The game only ever moves DOWN (`if (fDist > vDims.y)` in
        // ServerUtilities.cpp). An implementation that also lifted would look
        // plausible and would push objects up through tables.
        check(nLifted == 0, "nothing is ever lifted -- the game only moves down");

        check(nSunk == 0, "and nothing ends up below the surface it landed on");

        // NEGATIVE CONTROL: the flag has to be what selects them.
        check(nUnflaggedMoved == 0,
              "NEGATIVE CONTROL: objects without the flag are not moved at all");
    }

    // ================= props that ride on doors ====================== //
    // ================= front-to-back ordering ======================== //
    printf("\n--- front-to-back ordering ---\n");
    {
        const int SW = 256, SH = 192;
        std::vector<unsigned char> uA((size_t)SW*SH*4), uB((size_t)SW*SH*4);
        std::vector<float>         uD((size_t)SW*SH);

        float e2[3] = { pos[0], pos[1] + CLTWorldScene::EyeHeight(), pos[2] };
        float a2[3] = { e2[0] + sinf(yaw), e2[1], e2[2] + cosf(yaw) };

        scene.SetFrontToBack(false);
        CLTWorldScene::Stats s0 = scene.Render(&uA[0], &uD[0], SW, SH, e2, a2, 1.4f);

        scene.SetFrontToBack(true);
        CLTWorldScene::Stats s1 = scene.Render(&uB[0], &uD[0], SW, SH, e2, a2, 1.4f);

        printf("        pixels shaded: unsorted %d, front-to-back %d (%.0f%% fewer)\n",
               s0.m_nPixels, s1.m_nPixels,
               100.0 * (s0.m_nPixels - s1.m_nPixels) / (double)s0.m_nPixels);

        // ⚠️ THE TWO HALVES ARE BOTH NECESSARY, and either alone is worthless.
        //
        // Identical output alone would pass a "sort" that did nothing at all.
        // Fewer shaded pixels alone would pass one that dropped geometry --
        // which is exactly what the abandoned frustum culling did, and it
        // announced itself by changing 13 of 24 views.
        //
        // Together they say: the same picture, reached with less work. That is
        // what makes reordering an optimisation rather than a trade.
        check(uA == uB,
              "reordering does not change ONE pixel of the result");
        check(s1.m_nPixels < s0.m_nPixels,
              "and it shades measurably fewer pixels to get there");

        scene.SetFrontToBack(true);
    }


    printf("\n--- door handles ---\n");
    {
        CLTWorldDoors doors;
        doors.Build(scene.GetWorld());

        const std::vector<LTWorldObject>& obj = scene.GetWorld().GetObjects();

        int nKnob = 0, nAttached = 0, nMovedBefore = 0;
        for (size_t i = 0; i < obj.size(); ++i)
        {
            if (obj[i].m_sType != "DoorKnob") continue;
            ++nKnob;
            const char* pName = obj[i].GetName();
            float M[16];
            if (!pName || !doors.GetTransform(pName, M)) continue;
            ++nAttached;

            // Identity while everything is shut.
            float p[3];
            if (!obj[i].GetPos(p)) continue;
            float o[3];
            for (int r = 0; r < 3; ++r)
                o[r] = M[r*4+0]*p[0] + M[r*4+1]*p[1] + M[r*4+2]*p[2] + M[r*4+3];
            float d = sqrtf((o[0]-p[0])*(o[0]-p[0]) + (o[1]-p[1])*(o[1]-p[1])
                          + (o[2]-p[2])*(o[2]-p[2]));
            if (d > 0.5f) ++nMovedBefore;
        }

        printf("        %d DoorKnob objects, %d attached to a door\n", nKnob, nAttached);
        check(nKnob > 0, "M01S01 has DoorKnob objects at all");
        check(nAttached > 0, "and some of them are attached to a door");

        // ⚠️ NEGATIVE CONTROL FOR THE BOUND. The knobs in this level fall into
        // two groups with nothing between them: ten sit 0-2 units from a door
        // brush, the other fifteen are 154 units away or more, on frames and
        // fixed panels. If EVERY knob attached, the search would be grabbing
        // whatever is nearest rather than what it is touching, and opening a
        // door would drag handles across the room.
        check(nAttached < nKnob,
              "NEGATIVE CONTROL: knobs not touching a door are NOT attached");

        check(nMovedBefore == 0,
              "and while every door is shut, no handle is displaced");

        // ---- now open them ---------------------------------------------- //
        for (unsigned int d = 0; d < doors.Count(); ++d)
        {
            float c[3] = { 0, 0, 0 };
            const std::vector<LTWorldModelHeader>& mdl = scene.GetWorld().GetModels();
            for (size_t k = 0; k < mdl.size(); ++k)
            {
                if (mdl[k].m_sName != doors.Name(d) || !mdl[k].m_bBodyParsed) continue;
                float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
                for (size_t q = 0; q < mdl[k].m_Points.size(); ++q)
                    for (int a = 0; a < 3; ++a)
                    {
                        float v = mdl[k].m_Points[q].m_Pos[a];
                        if (v < mn[a]) mn[a] = v;
                        if (v > mx[a]) mx[a] = v;
                    }
                for (int a = 0; a < 3; ++a) c[a] = (mn[a] + mx[a]) * 0.5f;
                break;
            }
            bool bLocked = false;
            doors.Activate(c, 400.0f, &bLocked);
        }
        for (int i = 0; i < 400; ++i) doors.Update(0.05f);

        int nCarried = 0; float fFurthest = 0.0f;
        for (size_t i = 0; i < obj.size(); ++i)
        {
            if (obj[i].m_sType != "DoorKnob") continue;
            const char* pName = obj[i].GetName();
            float M[16], p[3];
            if (!pName || !doors.GetTransform(pName, M)) continue;
            if (!obj[i].GetPos(p)) continue;
            float o[3];
            for (int r = 0; r < 3; ++r)
                o[r] = M[r*4+0]*p[0] + M[r*4+1]*p[1] + M[r*4+2]*p[2] + M[r*4+3];
            float d = sqrtf((o[0]-p[0])*(o[0]-p[0]) + (o[1]-p[1])*(o[1]-p[1])
                          + (o[2]-p[2])*(o[2]-p[2]));
            if (d > 1.0f) { ++nCarried; if (d > fFurthest) fFurthest = d; }
        }
        printf("        with the doors open, %d handles are carried, furthest %.1f units\n",
               nCarried, fFurthest);
        check(nCarried > 0 && fFurthest > 10.0f,
              "opening a door CARRIES its handle instead of leaving it in mid-air");
    }

    printf("\n--- the walk camera ---\n");

    // ⚠️ Init reads LT_WORLD, so the test sets it rather than reaching into
    // private state. That is the same door the host uses.
    setenv("LT_WORLD", "WORLDS\\M01S01.DAT", 1);
    check(LTWorldWalk_Init(&fm), "walk mode initialises from LT_WORLD");
    check(LTWorldWalk_Active(), "and reports itself active");

    LT2DSurf surf;
    surf.m_pRGBA = &rgbaA[0]; surf.m_Width = W; surf.m_Height = H;

    // ⚠️ LET IT FALL FIRST. With collision the body is under gravity, so the
    // frames after Init are NOT stationary -- it is settling onto the floor.
    // An earlier version of this test asserted stillness immediately and
    // failed the moment gravity was added, which was the test being wrong
    // rather than the code.
    for (int i = 0; i < 120; ++i) LTWorldWalk_Frame(surf, 1.0f / 60.0f);
    memcpy(&rgbaB[0], &rgbaA[0], rgbaA.size());

    // ---- a key that is NOT held must not move the camera ----------------- //
    LTWorldWalk_Frame(surf, 1.0f / 60.0f);
    check(memcmp(&rgbaA[0], &rgbaB[0], rgbaA.size()) == 0,
          "once landed, with no key held, the view is identical frame to frame");

    // ---- holding W must move it ------------------------------------------ //
    LTWorldWalk_OnKey('W', true);
    for (int i = 0; i < 20; ++i) LTWorldWalk_Frame(surf, 1.0f / 60.0f);
    check(memcmp(&rgbaA[0], &rgbaB[0], rgbaA.size()) != 0,
          "holding W changes the view -- the camera moved");

    // ---- releasing it must stop --------------------------------------- //
    LTWorldWalk_OnKey('W', false);
    for (int i = 0; i < 300; ++i) LTWorldWalk_Frame(surf, 1.0f / 60.0f);

    // ⚠️ Measure how many of the last frames differ, not just one pair. A body
    // that JITTERS -- pushed out of one surface into another and back -- would
    // pass a single comparison by luck and fail the next run.
    int nMoving = 0;
    for (int i = 0; i < 20; ++i)
    {
        memcpy(&rgbaB[0], &rgbaA[0], rgbaA.size());
        LTWorldWalk_Frame(surf, 1.0f / 60.0f);
        if (memcmp(&rgbaA[0], &rgbaB[0], rgbaA.size()) != 0) ++nMoving;
    }
    printf("        %d of 20 settled frames still changed\n", nMoving);
    check(nMoving == 0, "releasing it stops the camera dead -- and it stays stopped");

    // ---- turning must change the view too ------------------------------- //
    LTWorldWalk_OnKey(VK_LEFT, true);
    for (int i = 0; i < 10; ++i) LTWorldWalk_Frame(surf, 1.0f / 60.0f);
    LTWorldWalk_OnKey(VK_LEFT, false);
    check(memcmp(&rgbaA[0], &rgbaB[0], rgbaA.size()) != 0,
          "and holding Left turns it");

    // ---- ...AND IT MUST TURN THE WAY THE KEY SAYS ----------------------- //
    //
    // ⚠️ EVERY CHECK ABOVE PASSES A CAMERA THAT MOVES BACKWARDS. They assert
    // that the picture CHANGED, which a sign error changes just as thoroughly
    // as correct code. Both the turn keys and the strafe keys were inverted for
    // a whole phase underneath a green suite, and a player found it in seconds.
    //
    // The property is end-to-end and needs no knowledge of the maths: turning
    // or strafing RIGHT must slide the picture LEFT. `right` is `f x up`, which
    // is the vector screen +X maps to (lt3d.cpp, LookAt row 0), so moving along
    // it moves the world the other way.
    //
    // ⚠️ Noclip for the strafe half, deliberately: in a corridor a correct
    // strafe can be stopped dead by a wall, and "did not move" would then be
    // indistinguishable from "moved the wrong way".
    {
        LTWorldWalk_OnKey('F', true);
        LTWorldWalk_Frame(surf, 1.0f / 60.0f);
        LTWorldWalk_OnKey('F', false);

        struct Case { int nKey; const char* pWhat; int nSign; };
        const Case kCases[4] = {
            { VK_RIGHT, "turning Right",   -1 },
            { VK_LEFT,  "turning Left",    +1 },
            { 'D',      "strafing Right",  -1 },
            { 'A',      "strafing Left",   +1 },
        };

        for (int c = 0; c < 4; ++c)
        {
            LTWorldWalk_Frame(surf, 1.0f / 60.0f);
            memcpy(&rgbaB[0], &rgbaA[0], rgbaA.size());          // before

            // ⚠️ TWO FRAMES, ~10 UNITS, AND THE SMALLNESS IS THE POINT. At
            // eight frames this test FAILED on a camera that is provably
            // correct: 40 units of strafe in a corridor barely wider than that
            // puts the eye against the far wall, so the picture is not a
            // translation of the one before it, and the correlation locks onto
            // the NEXT repeated door panel instead -- reporting a confident
            // shift the wrong way.
            //
            // ⚠️ This was nearly recorded as a bug in the camera. What settled
            // it was refusing to trust one instrument: the same +s displacement
            // in a standalone probe at a different spot moved the picture LEFT,
            // the view matrix says row 0 is `s`, and the logged position delta
            // was exactly +40 along `s`. Three agreeing answers against one
            // disagreeing measurement means the MEASUREMENT is what is wrong.
            LTWorldWalk_OnKey(kCases[c].nKey, true);
            for (int i = 0; i < 2; ++i) LTWorldWalk_Frame(surf, 1.0f / 60.0f);
            LTWorldWalk_OnKey(kCases[c].nKey, false);
            LTWorldWalk_Frame(surf, 1.0f / 60.0f);               // after, in rgbaA

            int nShift = BestShift(&rgbaB[0], &rgbaA[0], W, H);
            printf("        %-16s picture moved %+d px\n", kCases[c].pWhat, nShift);

            char szWhat[128];
            snprintf(szWhat, sizeof szWhat, "%s slides the picture %s",
                     kCases[c].pWhat, kCases[c].nSign < 0 ? "LEFT" : "RIGHT");
            check(nShift * kCases[c].nSign > 0, szWhat);
        }

        LTWorldWalk_OnKey('F', true);       // back out of noclip
        LTWorldWalk_Frame(surf, 1.0f / 60.0f);
        LTWorldWalk_OnKey('F', false);
    }

    // ================= mouse look ==================================== //
    printf("\n--- mouse look ---\n");
    {
        LTWorldWalk_SetMouseLook(true);

        // ⚠️ Same property as the keys, and it has to be checked separately:
        // mouse look applies its own sign to g_Yaw, so it can be inverted while
        // the arrow keys are correct. Moving the mouse RIGHT must slide the
        // picture LEFT, exactly as turning right does.
        struct MCase { int nDX; int nDY; const char* pWhat; int nSign; };
        const MCase kM[2] = {
            { +40, 0, "mouse right", -1 },
            { -40, 0, "mouse left",  +1 },
        };

        for (int c = 0; c < 2; ++c)
        {
            LTWorldWalk_Frame(surf, 1.0f / 60.0f);
            memcpy(&rgbaB[0], &rgbaA[0], rgbaA.size());

            LTWorldWalk_OnMouse(kM[c].nDX, kM[c].nDY);
            LTWorldWalk_Frame(surf, 1.0f / 60.0f);

            int nShift = BestShift(&rgbaB[0], &rgbaA[0], W, H);
            printf("        %-14s picture moved %+d px\n", kM[c].pWhat, nShift);

            char szW[128];
            snprintf(szW, sizeof szW, "%s slides the picture %s",
                     kM[c].pWhat, kM[c].nSign < 0 ? "LEFT" : "RIGHT");
            check(nShift * kM[c].nSign > 0, szW);
        }

        // ⚠️ NEGATIVE CONTROL: with mouse look released, motion must be IGNORED.
        // Otherwise Tab would hide the cursor without actually letting go, and
        // the camera would keep spinning while you tried to reach the close box.
        LTWorldWalk_SetMouseLook(false);
        LTWorldWalk_Frame(surf, 1.0f / 60.0f);
        memcpy(&rgbaB[0], &rgbaA[0], rgbaA.size());
        LTWorldWalk_OnMouse(200, 0);
        LTWorldWalk_Frame(surf, 1.0f / 60.0f);
        check(rgbaA == rgbaB,
              "NEGATIVE CONTROL: with mouse look released the camera ignores it");

        LTWorldWalk_SetMouseLook(true);
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
