// Linux port only. Xcode compiles every file under engine/ (synchronized
// folder), so this file is empty on macOS.
#ifndef __APPLE__  // Linux port only
// Linux port: ltaudio.h on SDL3 audio + dr_mp3 (replaces sound/ltaudio.mm).

#include "ltaudio.h"
#include "ltmixer.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../third_party/dr_mp3.h"

namespace
{
    SDL_AudioStream* g_pStream = 0;
    CLTMixer*        g_pMixer  = 0;
    bool             g_bRunning = false;
    char             g_Err[256] = { 0 };
    std::vector<short> g_Buf;
    FILE*            g_pMixDump = 0;   // grown outside the callback where possible

    void Fail(const char* p) { snprintf(g_Err, sizeof(g_Err), "%s: %s", p, SDL_GetError()); }

    // Runs on SDL's audio thread: hand the device exactly what it asks for.
    void SDLCALL Feed(void*, SDL_AudioStream* s, int nAdditional, int)
    {
        const int nFrameBytes = CLTMixer::kChannels * (int)sizeof(short);
        while (nAdditional > 0)
        {
            int nFrames = nAdditional / nFrameBytes;
            if (nFrames <= 0) nFrames = 1;
            if (nFrames > 4096) nFrames = 4096;
            short* p = g_Buf.data();
            if (g_pMixer) g_pMixer->Render(p, (unsigned int)nFrames);
            else          memset(p, 0, (size_t)nFrames * nFrameBytes);
            if (g_pMixDump) fwrite(p, (size_t)nFrameBytes, (size_t)nFrames, g_pMixDump);   // LT_MIXWAV (raw s16 stereo)
            SDL_PutAudioStreamData(s, p, nFrames * nFrameBytes);
            nAdditional -= nFrames * nFrameBytes;
        }
    }
}

bool LTAudio_Running() { return g_bRunning; }
const char* LTAudio_LastError() { return g_Err[0] ? g_Err : 0; }

bool LTAudio_Start(CLTMixer* pMixer)
{
    if (g_bRunning) return true;
    if (!pMixer) { snprintf(g_Err, sizeof(g_Err), "no mixer"); return false; }
    g_Err[0] = 0;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) { Fail("SDL audio init"); return false; }

    g_Buf.assign(4096 * CLTMixer::kChannels, 0);
    if (const char* d = getenv("LT_MIXWAV")) g_pMixDump = fopen(d, "wb");
    g_pMixer = pMixer;

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = CLTMixer::kChannels;
    spec.freq = CLTMixer::kRate;
    g_pStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Feed, 0);
    if (!g_pStream) { Fail("SDL_OpenAudioDeviceStream"); g_pMixer = 0; return false; }
    if (!SDL_ResumeAudioStreamDevice(g_pStream)) { Fail("SDL_ResumeAudioStreamDevice");
        SDL_DestroyAudioStream(g_pStream); g_pStream = 0; g_pMixer = 0; return false; }

    g_bRunning = true;
    return true;
}

void LTAudio_Stop()
{
    if (!g_bRunning) return;
    // Destroying the stream stops the device thread before the mixer goes.
    SDL_DestroyAudioStream(g_pStream);
    g_pStream = 0;
    g_pMixer = 0;
    g_bRunning = false;
}

bool LTAudio_DecodeMP3(const void* pData, unsigned int nSize,
                       std::vector<short>& outSamples,
                       unsigned int& outRate, unsigned int& outChannels)
{
    if (!pData || nSize < 4) return false;
    drmp3_config cfg;
    drmp3_uint64 nFrames = 0;
    drmp3_int16* p = drmp3_open_memory_and_read_pcm_frames_s16(pData, nSize, &cfg, &nFrames, 0);
    if (!p) return false;
    if (nFrames == 0 || cfg.channels == 0) { drmp3_free(p, 0); return false; }
    outSamples.assign(p, p + (size_t)nFrames * cfg.channels);
    drmp3_free(p, 0);
    outRate = cfg.sampleRate;
    outChannels = cfg.channels;
    return true;
}
#endif  // !__APPLE__
