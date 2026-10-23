// ------------------------------------------------------------------------- //
//
// ltdmhost.cpp -- puts the music manager into the engine (HANDOVER 5cj):
// files through the engine's file manager, Apple's GM bank as the default
// collection, the music thread, and the mixer's pull. Kept out of ltdmmgr.cpp
// so the offline renderer (Docs/tools/dmrender) links without the engine.
//
//   LT_NOMUSIC=1       the generated ILTDirectMusicMgr stub again (says so)
//   LT_DMWAV=<file>    record the music to a WAV in real time, device or not
//                      -- the headless check that in-game music plays (says so)
//
// ------------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltdmmgr.h"
#include "ltfilemgr.h"
#include "ltaudio.h"
#include "ltmixer.h"

#include <atomic>
#include <chrono>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <vector>

namespace
{
    const char* const kGMPath = "/System/Library/Components/CoreAudio.component/Contents/Resources/gs_instruments.dls";

    bool ReadGameFile(const char* pPath, std::vector<unsigned char>& out, void*)
    {
        CLTFileMgr* pMgr = LTGetFileMgr();
        ILTStream* s = pMgr ? pMgr->OpenFile(pPath) : 0;
        if (!s) return false;
        const uint32 n = s->GetLen();
        out.resize(n);
        const bool ok = n && s->Read(&out[0], n) == LT_OK;
        s->Release();
        return ok;
    }

    void PullMusic(float* pLR, unsigned int nFrames) { LTDM_GetMusicMgr()->Pull(pLR, nFrames); }

    std::atomic<bool>   g_bDump(false);
    std::thread         g_DumpThread;

    bool MusicActive() { return LTAudio_Running() || g_bDump; }

    // LT_DMWAV: drain the ring at the device's pace, into a WAV.
    void DumpMain(std::string sPath)
    {
        FILE* f = fopen(sPath.c_str(), "wb");
        if (!f) { printf("[LTDM] LT_DMWAV: cannot write %s\n", sPath.c_str()); fflush(stdout); return; }
        const uint32_t kRate = 44100;
        unsigned char hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                                  (unsigned char)(kRate & 0xFF), (unsigned char)((kRate >> 8) & 0xFF), (unsigned char)(kRate >> 16), 0,
                                  0x10,0xB1,0x02,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
        fwrite(hdr, 1, 44, f);
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        uint64_t nOut = 0;
        float buf[1024 * 2];
        short pcm[1024 * 2];
        while (g_bDump)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const uint64_t nTarget = (uint64_t)(dt * kRate);
            while (nOut < nTarget)
            {
                const unsigned int n = (unsigned int)std::min<uint64_t>(1024, nTarget - nOut);
                for (unsigned int i = 0; i < n * 2; ++i) buf[i] = 0.0f;
                PullMusic(buf, n);
                for (unsigned int i = 0; i < n * 2; ++i)
                {
                    int v = (int)(buf[i] * 32767.0f);
                    pcm[i] = (short)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
                }
                fwrite(pcm, 4, n, f);
                nOut += n;
            }
            const uint32_t nData = (uint32_t)(nOut * 4), nRiff = nData + 36;
            fseek(f, 4, SEEK_SET);  fwrite(&nRiff, 4, 1, f);
            fseek(f, 40, SEEK_SET); fwrite(&nData, 4, 1, f);
            fseek(f, 0, SEEK_END);
            fflush(f);
        }
        fclose(f);
    }

    void StopMusicAtExit()
    {
        LTMixer_SetMusicSource(0);
        if (g_bDump) { g_bDump = false; if (g_DumpThread.joinable()) g_DumpThread.join(); }
        LTDM_GetMusicMgr()->StopThread();
    }
}

// The manager the client hands the game, or 0 for "keep the stub".
ILTDirectMusicMgr* LTDM_HostSetup()
{
    static ILTDirectMusicMgr* s_p = []() -> ILTDirectMusicMgr* {
        if (getenv("LT_NOMUSIC"))
        {
            printf("[LTDM] LT_NOMUSIC=1 -- ILTDirectMusicMgr is the generated stub, no music (old)\n");
            fflush(stdout);
            return 0;
        }
        CLTDMMusicMgr* m = LTDM_GetMusicMgr();
        LTDM_SetDefaultReader(&ReadGameFile);
        m->Performance().SetGMPath(kGMPath);
        // The synth runs at unity: a full-velocity, full-volume note on a
        // full-scale sample comes out at the sample's level less the pan law,
        // then the game's music volume (GUID_PerfMasterVolume). LT_DMGAIN=<dB>
        // trims that, for setting the level by ear (says so).
        if (const char* g = getenv("LT_DMGAIN"))
        {
            const float fDB = (float)atof(g);
            m->Performance().Synth().SetGain((float)pow(10.0, fDB / 20.0));
            printf("[LTDM] LT_DMGAIN=%.1f -- music synth gain %+.1f dB\n", fDB, fDB);
            fflush(stdout);
        }
        // Built now, so it is not torn down before the music thread stops.
        (void)CLTDMPerformance::DefaultChord();
        if (const char* w = getenv("LT_DMWAV"))
        {
            printf("[LTDM] LT_DMWAV=%s -- the music is recorded there in real time\n", w);
            fflush(stdout);
            g_bDump = true;
            g_DumpThread = std::thread(DumpMain, std::string(w));
        }
        else LTMixer_SetMusicSource(&PullMusic);
        m->StartThread(&MusicActive);
        atexit(&StopMusicAtExit);
        return m;
    }();
    return s_p;
}
