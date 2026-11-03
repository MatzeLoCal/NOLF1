// ----------------------------------------------------------------------- //
//
// enginetest_metal.cpp -- does the GPU backend draw the same picture?
//
// ⚠️ WHY THIS EXISTS AND WHAT IT CAN AND CANNOT SAY. A GPU renderer will never
// match a software rasteriser bit for bit: different fill rules on shared
// edges, different filtering, different interpolation precision. So the check
// cannot be equality, and a check that cannot be equality needs a threshold --
// which is exactly the kind of check that passes anything if nobody proves it
// discriminates.
//
// So this asserts THREE things, and the third is the one that matters:
//
//   1. the GPU frame is not blank (it drew something at all);
//   2. it agrees with the software frame within a tolerance;
//   3. NEGATIVE CONTROL -- the same comparison against a DIFFERENT view scores
//      far worse. Without that, a loose tolerance would pass two arbitrary
//      pictures and the suite would be decorative.
//
// ⚠️ Skips rather than fails when no Metal device is present. A headless build
// machine having no GPU is not a defect in the renderer.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltworldrender.h"
#include "ltmetal.h"
#include "ltfilemgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

// ⚠️ The window layer is linked for its NSView, and its event handlers call
// into the input layer -- which needs the Win32 compat shim. Stubbing these
// symbols keeps this suite about the RENDERER: it never opens a window, so no
// event can reach them. Pulling the shim in instead would make a graphics test
// depend on the input stack, which is how test suites become unmaintainable.
struct LTInputEvent;
void LTInput_Push(const LTInputEvent&) {}
int  LTInput_MacKeyToVK(int) { return 0; }
int  LTInput_MacModifierToVK(int, unsigned long, bool*, bool*) { return 0; }   // flagsChanged

// One run-time sprite, for the FLAG_ROTATEABLESPRITE check (5em).
struct OneSprite : public ILTWorldDynamicObjects
{
    LTDynamicSprite m_S;
    bool            m_bOn = true;
    unsigned int Count() const override { return 0; }
    bool         Get(unsigned int, LTDynamicModel&) const override { return false; }
    unsigned int SpriteCount() const override { return m_bOn ? 1u : 0u; }
    bool         GetSprite(unsigned int i, LTDynamicSprite& out) const override
    { if (i || !m_bOn) return false; out = m_S; return true; }
};

// One game model, for the model-chrome check (5en).
struct OneModel : public ILTWorldDynamicObjects
{
    LTDynamicModel m_M;
    unsigned int Count() const override { return 1; }
    bool         Get(unsigned int i, LTDynamicModel& out) const override
    { if (i) return false; out = m_M; return true; }
};

static int g_nPass = 0, g_nFail = 0;
static void ok(const char* w)            { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)           { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w) { b ? ok(w) : bad(w); }

// Mean absolute difference per channel, and the share of pixels that agree
// closely. Two renderings of the SAME view differ at edges and on filtered
// texels; two renderings of DIFFERENT views disagree nearly everywhere.
struct Diff { double m_fMean; double m_fCloseShare; };

static Diff Compare(const std::vector<unsigned char>& a,
                    const std::vector<unsigned char>& b)
{
    Diff d; d.m_fMean = 0; d.m_fCloseShare = 0;
    if (a.size() != b.size() || a.empty()) return d;

    double sum = 0; long long nClose = 0, nPix = 0;
    for (size_t i = 0; i < a.size(); i += 4)
    {
        int dr = abs((int)a[i]   - (int)b[i]);
        int dg = abs((int)a[i+1] - (int)b[i+1]);
        int db = abs((int)a[i+2] - (int)b[i+2]);
        sum += dr + dg + db;
        if (dr <= 40 && dg <= 40 && db <= 40) ++nClose;
        ++nPix;
    }
    d.m_fMean = sum / (double)(nPix * 3);
    d.m_fCloseShare = (double)nClose / (double)nPix;
    return d;
}

int main(int argc, char** argv)
{
    std::vector<const char*> dirs;
    for (int i = 1; i < argc; ++i) dirs.push_back(argv[i]);
    if (dirs.empty()) { fprintf(stderr, "usage: enginetest_metal <dirs...>\n"); return 2; }

    printf("\n--- Metal backend ---\n");

    char err[256] = { 0 };
    if (!LTMetal_SelfTest(err, sizeof err))
    {
        printf("        no usable Metal device (%s) -- SKIPPED\n", err[0] ? err : "?");
        printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
        return 0;
    }
    ok("the device, shaders, vertex layout and pipeline all come up");

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&dirs[0], (int)dirs.size()) == 0)
    { fprintf(stderr, "no game data\n"); return 2; }

    CLTWorldScene scene;
    check(scene.Load(&fm, "WORLDS\\M01S01.DAT"), "M01S01 loads");
    if (!scene.IsLoaded()) { printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail); return 1; }

    const int W = 320, H = 240;
    float pos[3], yaw = 0.0f;
    scene.GetSpawn(pos, &yaw);
    float eye[3] = { pos[0], pos[1] + CLTWorldScene::EyeHeight(), pos[2] };

    // ⚠️ Down the corridor, not at the spawn's own facing: a view of one flat
    // wall would agree between any two renderers that got the colour roughly
    // right, and would prove nothing about geometry.
    const float y2 = yaw + 3.14159265f * 1.5f;
    float at[3] = { eye[0] + sinf(y2), eye[1], eye[2] + cosf(y2) };

    std::vector<unsigned char> soft((size_t)W*H*4, 0), gpu((size_t)W*H*4, 0);
    std::vector<float>         depth((size_t)W*H, 0.0f);

    scene.Render(&soft[0], &depth[0], W, H, eye, at, 1.4f);

    check(scene.RenderMetalOffscreen(&gpu[0], W, H, eye, at, 1.4f),
          "the GPU backend renders offscreen and reads back");

    long long nLit = 0;
    for (size_t i = 0; i < gpu.size(); i += 4)
        if (gpu[i] > 24 || gpu[i+1] > 24 || gpu[i+2] > 24) ++nLit;
    printf("        GPU frame: %lld of %d pixels above the clear colour\n",
           nLit, W*H);
    check(nLit > (long long)(W*H) / 4, "and the GPU frame is not blank");

    // ⚠️ A dump switch, because "0.1% of pixels agree" says a picture is wrong
    // and nothing about HOW. Set LT_METAL_DUMP to a directory to write both.
    if (const char* pDir = getenv("LT_METAL_DUMP"))
    {
        char nm[512];
        for (int k = 0; k < 2; ++k)
        {
            const std::vector<unsigned char>& b = k ? gpu : soft;
            snprintf(nm, sizeof nm, "%s/%s.ppm", pDir, k ? "gpu" : "soft");
            if (FILE* f = fopen(nm, "wb"))
            {
                fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (int i = 0; i < W*H; ++i) fwrite(&b[(size_t)i*4], 1, 3, f);
                fclose(f);
            }
        }
    }

    Diff same = Compare(soft, gpu);
    printf("        same view : mean |diff| %.2f, %.1f%% of pixels agree closely\n",
           same.m_fMean, 100.0 * same.m_fCloseShare);

    // A DIFFERENT view, rendered in software, as the control.
    const float y3 = y2 + 2.2f;
    float at3[3] = { eye[0] + sinf(y3), eye[1], eye[2] + cosf(y3) };
    std::vector<unsigned char> other((size_t)W*H*4, 0);
    scene.Render(&other[0], &depth[0], W, H, eye, at3, 1.4f);
    Diff diffView = Compare(other, gpu);
    printf("        other view: mean |diff| %.2f, %.1f%% of pixels agree closely\n",
           diffView.m_fMean, 100.0 * diffView.m_fCloseShare);

    // ⚠️ 0.95, AND THE NUMBER IS EVIDENCE-BASED. The achievable figure is 99.7%
    // with a mean difference of 1.39 levels, once the GPU is told to interpolate
    // the vertex colour affinely like the software path. A threshold loose
    // enough to pass the state this backend was in an hour ago -- upside down,
    // at 64% -- would not be a guard, it would be decoration.
    check(same.m_fCloseShare > 0.95,
          "the GPU picture matches the software picture");
    check(same.m_fMean < 4.0,
          "and the mean difference is within rasterisation noise, not structural");
    // ⚠️ BY THE MEAN, NOT THE "CLOSE" SHARE (HANDOVER 5ej). The share counts a
    // pixel within 40 levels as agreeing, so it measures BRIGHTNESS as much as
    // agreement: when the world light went from x1/128 to the original's
    // x1/255 the wrong view's share rose from below 2/3 to 84.7% -- dim
    // pixels of any two views sit within 40 of each other -- while the means
    // still parted 0.28 against 18.43. A wrong view must differ ten times more.
    check(diffView.m_fMean > same.m_fMean * 10.0 && diffView.m_fMean > 4.0,
          "NEGATIVE CONTROL: and it matches the RIGHT view much better than a wrong one");

    // ---- THE WORLD'S ENVIRONMENT MAP (HANDOVER 5el) --------------------- //
    //
    // ⚠️ A SEPARATE GPU PATH: the static world leaves env-mapped polygons out
    // and the frame draws them through a two-texture pipeline. So the same
    // agreement is asked of a view full of them -- T01S02's metal pillar and
    // window frames -- and the CONTROL is the option switched off: if the env
    // term were silently missing in both renderers, they would still agree.
    {
        CLTWorldScene envScene;
        check(envScene.Load(&fm, "WORLDS\\T01S02.DAT"), "T01S02 loads");
        const float eE[3] = { 200.0f, 40.0f, -420.0f }, aE[3] = { 352.0f, 50.0f, -520.0f };
        std::vector<unsigned char> sOn((size_t)W*H*4, 0), gOn((size_t)W*H*4, 0), sOff((size_t)W*H*4, 0);
        LTWorldSetEnvMaps(true, 1.0f);
        envScene.Render(&sOn[0], &depth[0], W, H, eE, aE, 1.4f);
        check(envScene.RenderMetalOffscreen(&gOn[0], W, H, eE, aE, 1.4f),
              "the GPU renders the env-mapped view");
        LTWorldSetEnvMaps(false, 1.0f);
        envScene.Render(&sOff[0], &depth[0], W, H, eE, aE, 1.4f);
        LTWorldSetEnvMaps(true, 1.0f);

        Diff envSame = Compare(sOn, gOn), envCtl = Compare(sOff, sOn), envGpuCtl = Compare(sOff, gOn);
        printf("        env on, GPU vs software: mean |diff| %.2f, %.1f%% agree closely\n",
               envSame.m_fMean, 100.0 * envSame.m_fCloseShare);
        printf("        env off vs on (software %.2f, GPU %.2f mean |diff|)\n",
               envCtl.m_fMean, envGpuCtl.m_fMean);
        check(envSame.m_fCloseShare > 0.95 && envSame.m_fMean < 4.0,
              "the GPU's environment maps match the software's");
        check(envCtl.m_fMean > envSame.m_fMean * 3.0 && envGpuCtl.m_fMean > envSame.m_fMean * 3.0,
              "CONTROL: and both pictures differ from the same view with the option off");
    }

    // ---- AN ANIMATED `.spr` SURFACE (HANDOVER 5em) ----------------------- //
    //
    // ⚠️ THE GPU DRAWS IT THROUGH A BORROWED HANDLE: the static world's runs
    // are keyed by the sprite's alias, which takes its current frame's
    // texture handle each frame. M01S02's EXIT sign (Spr0027: 40 frames at 15
    // fps, ESign013 glowing at 0 s, ESign012 at 0.5 s). The comparison is
    // over the pixels the sign CHANGES between the two times -- 1.4% of the
    // frame, which a whole-frame mean would drown -- and the control is frame
    // 0: a GPU stuck there would match the 0 s picture instead.
    {
        CLTWorldScene sprScene;
        check(sprScene.Load(&fm, "WORLDS\\M01S02.DAT"), "M01S02 loads");
        const float eS[3] = { 1990.0f, -1344.0f, 1968.0f }, aS[3] = { 2080.0f, -1344.0f, 1968.0f };
        std::vector<unsigned char> s0((size_t)W*H*4, 0), s5((size_t)W*H*4, 0), g5((size_t)W*H*4, 0);
        LTWorldSetSpriteClock(0.0);
        sprScene.Render(&s0[0], &depth[0], W, H, eS, aS, 1.4f);
        LTWorldSetSpriteClock(0.5);
        sprScene.Render(&s5[0], &depth[0], W, H, eS, aS, 1.4f);
        check(sprScene.RenderMetalOffscreen(&g5[0], W, H, eS, aS, 1.4f),
              "the GPU renders the animated surface");
        LTWorldSetSpriteClock(0.0);

        long long nRegion = 0; double dNow = 0.0, dThen = 0.0;
        for (size_t i = 0; i < s0.size(); i += 4)
        {
            if (s0[i] == s5[i] && s0[i+1] == s5[i+1] && s0[i+2] == s5[i+2]) continue;
            ++nRegion;
            for (int k = 0; k < 3; ++k)
            {
                dNow  += abs((int)g5[i+k] - (int)s5[i+k]);
                dThen += abs((int)g5[i+k] - (int)s0[i+k]);
            }
        }
        if (nRegion) { dNow /= (double)(nRegion * 3); dThen /= (double)(nRegion * 3); }
        printf("        animated sign: %lld pixels change 0 s -> 0.5 s; GPU at 0.5 s differs %.2f from that frame, %.2f from frame 0\n",
               nRegion, dNow, dThen);
        check(nRegion > 300, "the sign's frame changes between 0 s and 0.5 s (software)");
        check(dNow * 3.0 < dThen, "CONTROL: the GPU shows the frame of the moment, not frame 0");
    }

    // ---- A ROTATABLE SPRITE LIES IN ITS OWN PLANE (HANDOVER 5em) -------- //
    //
    // ⚠️ GEOMETRY, SO THE TEST IS EDGE-ON. One sprite 120 units down M01S01's
    // corridor: facing the camera it covers a square; FLAG_ROTATEABLESPRITE
    // with its plane along the view direction it is a sliver; turned 90
    // degrees about Y it faces the camera again. The CONTROL is the same
    // sprite without the flag, which ignores its rotation and stays a square.
    {
        float fwd[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
        const float fl = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
        for (int k = 0; k < 3; ++k) fwd[k] /= fl;
        OneSprite one;
        one.m_S.m_Draw.m_sSprite  = "SFX\\Flares\\WarmSoft1.spr";
        one.m_S.m_Draw.m_fScaleX  = one.m_S.m_Draw.m_fScaleY = 0.2f;
        one.m_S.m_Draw.m_nBlendOp = 1;
        for (int k = 0; k < 3; ++k) one.m_S.m_Pos[k] = eye[k] + fwd[k] * 120.0f;
        // A quaternion whose RIGHT axis is the view direction (yaw about Y):
        // right = (cos a, 0, -sin a) for a rotation a about +Y.
        const float yawEdge = atan2f(-fwd[2], fwd[0]);
        const float yawFace = yawEdge + 3.14159265f * 0.5f;
        auto QuatY = [](float a, float* q) { q[0] = 0.0f; q[1] = sinf(a * 0.5f); q[2] = 0.0f; q[3] = cosf(a * 0.5f); };

        std::vector<unsigned char> base((size_t)W*H*4, 0), img((size_t)W*H*4, 0), gimg((size_t)W*H*4, 0);
        one.m_bOn = false;
        scene.Render(&base[0], &depth[0], W, H, eye, at, 1.4f, 0, &one);
        one.m_bOn = true;
        auto Covered = [&](const std::vector<unsigned char>& a) {
            long long n = 0;
            for (size_t i = 0; i < a.size(); i += 4)
                if (abs((int)a[i] - (int)base[i]) + abs((int)a[i+1] - (int)base[i+1]) + abs((int)a[i+2] - (int)base[i+2]) > 6) ++n;
            return n; };
        long long nFlat, nEdge, nFace, nCtl; double gMean = 0.0;
        one.m_S.m_Draw.m_bRotatable = false; QuatY(yawEdge, one.m_S.m_Draw.m_Quat);
        scene.Render(&img[0], &depth[0], W, H, eye, at, 1.4f, 0, &one);  nCtl = Covered(img);
        one.m_S.m_Draw.m_bRotatable = true;
        scene.Render(&img[0], &depth[0], W, H, eye, at, 1.4f, 0, &one);  nEdge = Covered(img);
        QuatY(yawFace, one.m_S.m_Draw.m_Quat);
        scene.Render(&img[0], &depth[0], W, H, eye, at, 1.4f, 0, &one);  nFace = Covered(img);
        nFlat = nFace;
        if (scene.RenderMetalOffscreen(&gimg[0], W, H, eye, at, 1.4f, 0, &one))
            gMean = Compare(img, gimg).m_fMean;
        printf("        sprite pixels: camera-facing %lld, rotatable edge-on %lld, rotatable facing %lld; GPU vs software %.2f\n",
               nCtl, nEdge, nFlat, gMean);
        check(nCtl > 400 && nFace > nCtl / 2, "a rotatable sprite turned to the camera covers about what a facing one does");
        check(nEdge * 20 < nFace, "and edge-on it is a sliver -- it lies in its own plane");
        check(gMean < 4.0, "and the GPU draws it where the software does");
    }

    // ---- MODEL CHROME (HANDOVER 5en) ------------------------------------ //
    //
    // ⚠️ A THIRD COMBINE, ON ITS OWN GPU ROUTE: a chrome model leaves GPU
    // skinning for the CPU path and the batcher's chrome runs (fs_env's lerp).
    // The revolver's first-person model and skin (S&W_pv, alpha 135..255,
    // mean 197 -- ~23% chrome on its metal), 70 units down the corridor,
    // scaled up 25x. Compared over the pixels chrome CHANGES; the control is
    // chrome off, which a missing term in both renderers would also pass.
    {
        float fwd[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
        const float fl = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
        for (int k = 0; k < 3; ++k) fwd[k] /= fl;
        OneModel one;
        one.m_M.sModel = "Guns\\Models_PV\\S&W_pv.abc";
        one.m_M.sSkins = "Guns\\Skins_PV\\S&W_pv.dtx";
        for (int k = 0; k < 3; ++k) { one.m_M.m_Pos[k] = eye[k] + fwd[k] * 50.0f; one.m_M.m_Scale[k] = 30.0f; }
        one.m_M.m_bChrome = true;
        std::vector<unsigned char> sOff((size_t)W*H*4, 0), sOn((size_t)W*H*4, 0),
                                   gOff((size_t)W*H*4, 0), gOn((size_t)W*H*4, 0);
        LTWorldSetChrome(false, "Tex\\Chrome.dtx", 0.0005f, 1.0f);
        scene.Render(&sOff[0], &depth[0], W, H, eye, at, 1.4f, 0, &one);
        scene.RenderMetalOffscreen(&gOff[0], W, H, eye, at, 1.4f, 0, &one);
        LTWorldSetChrome(true, "Tex\\Chrome.dtx", 0.0005f, 1.0f);
        scene.Render(&sOn[0], &depth[0], W, H, eye, at, 1.4f, 0, &one);
        check(scene.RenderMetalOffscreen(&gOn[0], W, H, eye, at, 1.4f, 0, &one),
              "the GPU renders the chrome model");
        LTWorldSetChrome(false, "Tex\\Chrome.dtx", 0.0005f, 1.0f);

        // ⚠️ THE CHROME TERM ITSELF, per renderer: on minus off. Comparing
        // the two terms cancels what both renderers share (the model's own
        // rasterisation and light) and leaves the chrome alone.
        long long nRegion = 0; double dTerm = 0.0, dSize = 0.0;
        for (size_t i = 0; i < sOff.size(); i += 4)
        {
            int ds = 0;
            for (int k = 0; k < 3; ++k) ds += abs((int)sOn[i+k] - (int)sOff[i+k]);
            if (ds <= 3) continue;
            ++nRegion;
            for (int k = 0; k < 3; ++k)
            {
                const int ts = (int)sOn[i+k] - (int)sOff[i+k], tg = (int)gOn[i+k] - (int)gOff[i+k];
                dTerm += abs(tg - ts);
                dSize += abs(ts);
            }
        }
        if (nRegion) { dTerm /= (double)(nRegion * 3); dSize /= (double)(nRegion * 3); }
        printf("        chrome: %lld pixels change with it, by %.2f on average; GPU's change differs from software's by %.2f\n",
               nRegion, dSize, dTerm);
        check(nRegion > 500, "chrome changes the model (software)");
        check(dTerm * 2.0 < dSize, "CONTROL: the GPU adds the same chrome the software does");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
