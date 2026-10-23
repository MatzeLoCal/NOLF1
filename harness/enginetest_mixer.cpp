// ----------------------------------------------------------------------- //
//
// enginetest_mixer.cpp -- the sound mixer, without a sound card.
//
// ⚠️ THIS SUITE IS THE REASON THE MIXER IS NOT INSIDE A CoreAudio CALLBACK.
// Every property below -- does a sound to the left come out of the left
// speaker, does a 22 kHz sound play at the right pitch in a 44 kHz mix, does a
// sound past its outer radius fall silent -- is a thing people normally verify
// by listening, which nobody else can repeat and which cannot fail a build.
//
// ----------------------------------------------------------------------- //

#include "ltmixer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

static int g_nPass = 0, g_nFail = 0;
static void ok(const char* w)            { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)           { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w) { b ? ok(w) : bad(w); }

// Peak absolute level of one channel, 0 = left.
static int Peak(const std::vector<short>& buf, int nChannel)
{
    int m = 0;
    for (size_t i = (size_t)nChannel; i < buf.size(); i += 2)
    {
        int v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > m) m = v;
    }
    return m;
}

// Zero crossings, which is a proxy for frequency and therefore for pitch.
static int Crossings(const std::vector<short>& buf, int nChannel)
{
    int n = 0; int prev = 0; bool bFirst = true;
    for (size_t i = (size_t)nChannel; i < buf.size(); i += 2)
    {
        int v = buf[i];
        if (!bFirst && ((prev < 0 && v >= 0) || (prev >= 0 && v < 0))) ++n;
        prev = v; bFirst = false;
    }
    return n;
}

int main()
{
    // A one-second sine at 440 Hz, sampled at 22050 -- the rate two thirds of
    // NOLF's audio uses, and deliberately NOT the mix rate.
    const unsigned int kSrcRate = 22050;
    std::vector<short> tone(kSrcRate);
    for (unsigned int i = 0; i < kSrcRate; ++i)
        tone[i] = (short)(20000.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / kSrcRate));

    printf("\n--- distance rolloff ---\n");
    {
        check(CLTMixer::DistanceGain(0.0f, 100.0f, 1000.0f) == 1.0f,
              "at the listener, a sound is at full volume");
        check(CLTMixer::DistanceGain(100.0f, 100.0f, 1000.0f) == 1.0f,
              "and still full volume exactly at the inner radius");
        check(CLTMixer::DistanceGain(1000.0f, 100.0f, 1000.0f) == 0.0f,
              "exactly silent at the outer radius");
        check(CLTMixer::DistanceGain(5000.0f, 100.0f, 1000.0f) == 0.0f,
              "and beyond it");

        const float a = CLTMixer::DistanceGain(200.0f, 100.0f, 1000.0f);
        const float b = CLTMixer::DistanceGain(600.0f, 100.0f, 1000.0f);
        printf("        gain at 200 units %.3f, at 600 units %.3f\n", a, b);
        check(a < 1.0f && a > 0.0f && b > 0.0f && b < a,
              "and falls monotonically in between, never reaching either end early");

        // ⚠️ NEGATIVE CONTROL for the whole idea: with no radii set, a sound
        // must NOT be attenuated. PLAYSOUND_LOCAL sounds carry no radius, and
        // silently rolling them off would mute every UI click in the game.
        check(CLTMixer::DistanceGain(9999.0f, 0.0f, 0.0f) == 1.0f,
              "NEGATIVE CONTROL: with no radii, distance does nothing at all");
    }

    printf("\n--- panning ---\n");
    {
        const float kEye[3]   = { 0, 0, 0 };
        const float kRight[3] = { 1, 0, 0 };

        int nL = 0, nR = 0;
        for (int side = 0; side < 2; ++side)
        {
            CLTMixer mx;
            mx.SetListener(kEye, kRight);

            CLTMixer::Params p;
            p.m_eMode  = CLTMixer::k3D;
            p.m_fInner = 10.0f;
            p.m_fOuter = 10000.0f;
            p.m_Pos[0] = side ? 100.0f : -100.0f;      // right, then left

            check(mx.Play(&tone[0], (unsigned)tone.size(), kSrcRate, 1, p) != 0,
                  side ? "a voice starts to the right" : "a voice starts to the left");

            std::vector<short> out(4096 * 2, 0);
            mx.Render(&out[0], 4096);
            if (side) nR = Peak(out, 1) - Peak(out, 0);
            else      nL = Peak(out, 0) - Peak(out, 1);
        }
        printf("        left-side sound: L exceeds R by %d;  right-side: R exceeds L by %d\n", nL, nR);
        check(nL > 1000, "a sound on the left is louder in the LEFT channel");
        check(nR > 1000, "and a sound on the right is louder in the RIGHT channel");
    }

    printf("\n--- resampling ---\n");
    {
        // ⚠️ THE CHECK THAT CATCHES THE MOST LIKELY BUG. Stepping one source
        // sample per output frame is the obvious implementation and it plays a
        // 22 kHz sound an octave low in a 44 kHz mix -- which is two thirds of
        // this game's audio. Zero crossings measure pitch directly.
        CLTMixer mx;
        CLTMixer::Params p;                 // local: no distance, no panning
        mx.Play(&tone[0], (unsigned)tone.size(), kSrcRate, 1, p);

        const unsigned int kFrames = CLTMixer::kRate / 4;    // a quarter second
        std::vector<short> out(kFrames * 2, 0);
        mx.Render(&out[0], kFrames);

        const int nX = Crossings(out, 0);
        const int nExpect = 440 / 2;        // 440 Hz for 0.25 s = 220 crossings
        printf("        440 Hz at 22050 into a 44100 mix: %d zero crossings, expected ~%d\n",
               nX, nExpect);
        check(abs(nX - nExpect) < 20,
              "a 22 kHz source keeps its pitch in a 44 kHz mix");
    }

    printf("\n--- voices ---\n");
    {
        CLTMixer mx;
        CLTMixer::Params p;
        unsigned int h = mx.Play(&tone[0], (unsigned)tone.size(), kSrcRate, 1, p);
        check(h != 0 && !mx.IsDone(h), "a started voice reports itself playing");
        check(mx.ActiveVoices() == 1, "and is counted once");

        mx.Stop(h);
        check(mx.IsDone(h), "a stopped voice reports itself done");
        check(mx.ActiveVoices() == 0, "and is no longer counted");

        // Run a short sound to its end and confirm it retires itself.
        std::vector<short> blip(64, 12000);
        CLTMixer mx2;
        unsigned int h2 = mx2.Play(&blip[0], (unsigned)blip.size(), CLTMixer::kRate, 1, p);
        std::vector<short> out(512 * 2, 0);
        mx2.Render(&out[0], 512);
        check(mx2.IsDone(h2), "a one-shot voice ends on its own and frees its slot");

        // ...and a looping one does not.
        CLTMixer mx3;
        CLTMixer::Params lp; lp.m_bLoop = true;
        unsigned int h3 = mx3.Play(&blip[0], (unsigned)blip.size(), CLTMixer::kRate, 1, lp);
        mx3.Render(&out[0], 512);
        check(!mx3.IsDone(h3),
              "NEGATIVE CONTROL: a LOOPING voice survives the same render");
    }

    printf("\n--- master volume and pause ---\n");
    {
        CLTMixer mx;
        CLTMixer::Params p;
        std::vector<short> out(2048 * 2, 0);

        mx.Play(&tone[0], (unsigned)tone.size(), kSrcRate, 1, p);
        mx.Render(&out[0], 2048);
        const int nFull = Peak(out, 0);

        CLTMixer mx2;
        mx2.SetVolume(0.25f);
        mx2.Play(&tone[0], (unsigned)tone.size(), kSrcRate, 1, p);
        mx2.Render(&out[0], 2048);
        const int nQuarter = Peak(out, 0);

        printf("        peak at full volume %d, at 0.25 %d\n", nFull, nQuarter);
        check(nFull > 4 * nQuarter - 400 && nFull < 4 * nQuarter + 400,
              "master volume scales the mix proportionally");

        CLTMixer mx3;
        mx3.Play(&tone[0], (unsigned)tone.size(), kSrcRate, 1, p);
        mx3.SetPaused(true);
        mx3.Render(&out[0], 2048);
        check(Peak(out, 0) == 0 && Peak(out, 1) == 0,
              "and a paused mixer writes silence, not the previous buffer");
    }

    printf("\n--- clipping ---\n");
    {
        // ⚠️ Sixteen loud voices at once must clamp, not wrap. A wrapped int16
        // sum does not sound loud, it sounds like tearing static.
        CLTMixer mx;
        CLTMixer::Params p;
        std::vector<short> loud(1024, 30000);
        for (int i = 0; i < 16; ++i)
            mx.Play(&loud[0], (unsigned)loud.size(), CLTMixer::kRate, 1, p);

        std::vector<short> out(512 * 2, 0);
        mx.Render(&out[0], 512);

        bool bWrapped = false;
        for (size_t i = 0; i < out.size(); ++i) if (out[i] < 0) bWrapped = true;
        check(!bWrapped, "sixteen loud voices clamp instead of wrapping to negative");
        check(Peak(out, 0) == 32767, "and sit exactly at the rail");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
