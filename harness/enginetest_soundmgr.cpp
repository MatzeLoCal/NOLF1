// ----------------------------------------------------------------------- //
//
// enginetest_soundmgr.cpp -- the join: PLAYSOUND_ flags onto mixer voices.
//
// ⚠️ THIS LAYER LOOKS TRIVIAL AND IS WHERE THE SUBTLE BUG LIVES. PLAYSOUND_LOCAL
// is ZERO, so `flags & PLAYSOUND_LOCAL` is false for every sound in the game --
// test it that way and every 3D sound silently becomes local, which is not a
// crash, not a silence, just a world with no directional audio.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltsoundmgr.h"
#include "ltfilemgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

static int g_nPass = 0, g_nFail = 0;
static void ok(const char* w)            { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)           { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w) { b ? ok(w) : bad(w); }

static int Peak(const std::vector<short>& b, int ch)
{
    int m = 0;
    for (size_t i = (size_t)ch; i < b.size(); i += 2)
    { int v = b[i] < 0 ? -b[i] : b[i]; if (v > m) m = v; }
    return m;
}

static void Fill(PlaySoundInfo& si, const char* pName, uint32 dwFlags)
{
    memset(&si, 0, sizeof(si));
    si.m_dwFlags = dwFlags;
    snprintf(si.m_szSoundName, sizeof(si.m_szSoundName), "%s", pName);
    si.m_nVolume = 100;
    si.m_fPitchShift = 1.0f;
}

int main(int argc, char** argv)
{
    std::vector<const char*> dirs;
    for (int i = 1; i < argc; ++i) dirs.push_back(argv[i]);
    if (dirs.empty()) { fprintf(stderr, "usage: enginetest_soundmgr <dirs...>\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&dirs[0], (int)dirs.size()) == 0)
    { fprintf(stderr, "no game data\n"); return 2; }

    const char* kPCM = "CHARS\\SND\\JUMP2.WAV";        // PCM 22050 mono
    const char* kMP3 = "CHARS\\SND\\ENGLISH\\SEARCH9.WAV";

    printf("\n--- playing a real sound ---\n");
    {
        CLTSoundMgr sm;
        sm.Attach(&fm);

        PlaySoundInfo si; Fill(si, kPCM, PLAYSOUND_GETHANDLE);
        HLTSOUND h = 0;
        check(sm.PlaySound(&si, h) == LT_OK, "a shipped PCM sound plays");
        check(h != 0, "and GETHANDLE hands back a handle");
        check(sm.GetMixer().ActiveVoices() == 1, "with exactly one voice running");

        LTBOOL bDone = LTTRUE;
        check(sm.IsSoundDone(h, bDone) == LT_OK && !bDone, "which reports itself not done");
        check(sm.KillSound(h) == LT_OK, "killing it succeeds");
        check(sm.GetMixer().ActiveVoices() == 0, "and the voice is gone");

        // ⚠️ Without GETHANDLE the game never releases the sound, so it must
        // NOT be given a handle -- publishing one would leak a voice per shot.
        PlaySoundInfo si2; Fill(si2, kPCM, 0);
        HLTSOUND h2 = (HLTSOUND)(uintptr_t)12345;
        check(sm.PlaySound(&si2, h2) == LT_OK && h2 == 0,
              "NEGATIVE CONTROL: without GETHANDLE no handle is published");
    }

    printf("\n--- the MP3 majority ---\n");
    {
        CLTSoundMgr sm; sm.Attach(&fm);
        PlaySoundInfo si; Fill(si, kMP3, PLAYSOUND_GETHANDLE);
        HLTSOUND h = 0;
        check(sm.PlaySound(&si, h) == LT_OK,
              "an MP3-in-WAV sound plays through the same path");

        std::vector<short> out(2048 * 2, 0);
        sm.GetMixer().Render(&out[0], 2048);
        check(Peak(out, 0) > 0 || Peak(out, 1) > 0,
              "and produces actual samples, not silence");
    }

    printf("\n--- flags map to modes ---\n");
    {
        CLTSoundMgr sm; sm.Attach(&fm);
        const float kEye[3] = { 0, 0, 0 }, kRight[3] = { 1, 0, 0 };
        sm.UpdateListener(kEye, kRight);

        // A 3D sound far to the left must be louder on the left.
        PlaySoundInfo si; Fill(si, kPCM, PLAYSOUND_3D | PLAYSOUND_GETHANDLE);
        si.m_fInnerRadius = 10.0f;
        si.m_fOuterRadius = 10000.0f;
        ((float*)&si.m_vPosition)[0] = -500.0f;
        HLTSOUND h = 0;
        sm.PlaySound(&si, h);

        std::vector<short> out(2048 * 2, 0);
        sm.GetMixer().Render(&out[0], 2048);
        const int l = Peak(out, 0), r = Peak(out, 1);
        printf("        PLAYSOUND_3D at x=-500: L %d, R %d\n", l, r);

        // ⚠️ A RATIO, NOT AN ABSOLUTE MARGIN, and the first version of this
        // check got that wrong. It asked for `l > r + 500`, which quietly
        // depends on the ROLLOFF: with an inner radius of 10 and the sound 500
        // units away the gain is about 0.019, so a correctly and fully panned
        // sound arrives at L=196, R=0 and fails a 500-level margin. The panning
        // was right; the assertion was measuring loudness and calling it pan.
        check(l > 0 && r * 4 < l,
              "PLAYSOUND_3D pans -- a sound on the left is louder left");

        // ⚠️ THE ONE THAT CATCHES THE FLAG BUG. The same position with no 3D
        // flag is LOCAL, and local means centred -- so the channels must match.
        // If PLAYSOUND_LOCAL were tested with `&`, this case and the one above
        // would both come out local and both checks would pass on a broken map.
        CLTSoundMgr sm2; sm2.Attach(&fm);
        sm2.UpdateListener(kEye, kRight);
        PlaySoundInfo si2; Fill(si2, kPCM, PLAYSOUND_GETHANDLE);
        ((float*)&si2.m_vPosition)[0] = -500.0f;
        HLTSOUND h2 = 0;
        sm2.PlaySound(&si2, h2);

        std::vector<short> out2(2048 * 2, 0);
        sm2.GetMixer().Render(&out2[0], 2048);
        const int l2 = Peak(out2, 0), r2 = Peak(out2, 1);
        printf("        no 3D flag, same position: L %d, R %d\n", l2, r2);
        check(l2 == r2 && l2 > 0,
              "and a LOCAL sound at the same place is dead centre, not panned");
    }

    printf("\n--- volume, pause, position ---\n");
    {
        CLTSoundMgr sm; sm.Attach(&fm);
        check(sm.SetVolume(50) == LT_OK, "SetVolume succeeds");
        uint16 v = 0;
        check(sm.GetVolume(v) == LT_OK && v == 50, "and GetVolume reads it back");

        PlaySoundInfo si; Fill(si, kPCM, PLAYSOUND_3D | PLAYSOUND_GETHANDLE);
        si.m_fOuterRadius = 1000.0f;
        HLTSOUND h = 0; sm.PlaySound(&si, h);

        LTVector p; ((float*)&p)[0] = 42.0f; ((float*)&p)[1] = 0; ((float*)&p)[2] = 0;
        check(sm.SetSoundPosition(h, &p) == LT_OK, "a playing sound can be moved");
        LTVector q; memset(&q, 0, sizeof(q));
        check(sm.GetSoundPosition(h, &q) == LT_OK && ((float*)&q)[0] == 42.0f,
              "and reports back where it was moved to");

        check(sm.PauseSounds() == LT_OK && sm.GetMixer().GetPaused(), "PauseSounds pauses");
        check(sm.ResumeSounds() == LT_OK && !sm.GetMixer().GetPaused(), "ResumeSounds resumes");
    }

    printf("\n--- the caching layer ---\n");
    {
        CLTSoundMgr sm; sm.Attach(&fm);
        PlaySoundInfo si; Fill(si, kPCM, 0);
        HLTSOUND h = 0;
        for (int i = 0; i < 8; ++i) sm.PlaySound(&si, h);
        printf("        8 plays of one sound decoded it %u time(s)\n", sm.Decoded());
        check(sm.Decoded() == 1, "a sound played eight times is decoded ONCE");
    }

    printf("\n--- what is declined, and what is refused ---\n");
    {
        CLTSoundMgr sm; sm.Attach(&fm);

        PlaySoundInfo si; Fill(si, "CHARS\\SND\\NO_SUCH_SOUND.WAV", PLAYSOUND_GETHANDLE);
        HLTSOUND h = 0;
        check(sm.PlaySound(&si, h) == LT_MISSINGFILE,
              "a missing sound is LT_MISSINGFILE, not a silent success");

        // ⚠️ Declined, not faked -- see ltsoundmgr.cpp. A convincing LT_OK from
        // a filter that did nothing would leave the game believing rooms sound
        // different when they do not.
        ReverbProperties rp; memset(&rp, 0, sizeof(rp));
        check(sm.SetReverbProperties(&rp) == LT_UNSUPPORTED,
              "reverb reports LT_UNSUPPORTED rather than a convincing LT_OK");
        check(sm.SetSoundOcclusion(h, 1.0f) == LT_UNSUPPORTED, "so does occlusion");
        check(sm.SetSoundFilter(h, "flange") == LT_UNSUPPORTED, "so do the EAX filters");

        Sound3DProvider* pList = (Sound3DProvider*)1;
        check(sm.GetSound3DProviderLists(pList, LTFALSE) == LT_OK && pList == 0,
              "and the hardware 3D provider list is empty, which is the honest answer");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
