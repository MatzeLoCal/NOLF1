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
    check(same.m_fCloseShare > diffView.m_fCloseShare * 1.5,
          "NEGATIVE CONTROL: and it matches the RIGHT view much better than a wrong one");

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
