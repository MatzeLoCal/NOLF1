// ----------------------------------------------------------------------- //
//
// MODULE  : ltsoundmgr.cpp
//
// See ltsoundmgr.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltsoundmgr.h"
#include "ltwav.h"
#include "ltaudio.h"
#include "ltfilemgr.h"
#include "ltmathimpl.h"

#include <math.h>
#include <stdio.h>
#include <map>
#include <string>
#include <stdlib.h>
#include <string.h>

namespace
{
    // ⚠️ Said ONCE, not per call. NOLF sets reverb properties on every level
    // load and adjusts occlusion per sound; a message per call would bury the
    // log, and a silent LT_ERROR would leave nobody knowing why rooms sound
    // flat. Once is the useful amount.
    // ⚠️ THE NAMES, NOT JUST A COUNT. "6 sounds played" cannot tell dialogue
    // from a door hinge, and the whole reason the census exists is that a
    // headless run cannot be listened to.
    bool SndTracing()
    {
        static const bool b = (getenv("LT_SNDTRACE") != 0);
        return b;
    }

    // LT_SNDTRACE: which file a voice handle is playing, for the kill line.
    std::map<unsigned int, std::string> g_VoiceName;

    void SayOnce(bool& bSaid, const char* pWhat)
    {
        if (bSaid) return;
        bSaid = true;
        printf("[SOUND] %s is not implemented -- effects play dry.\n", pWhat);
        fflush(stdout);
    }
}


CLTSoundMgr::CLTSoundMgr()
    : m_pFileMgr(0), m_nDecoded(0), m_nMissing(0), m_nPlayed(0), m_nDoneAsks(0), m_nDoneYes(0),
      m_bSaidUnsupported(false), m_bListenerInClient(true)
{
    m_ListenerPos[0] = m_ListenerPos[1] = m_ListenerPos[2] = 0.0f;
    m_ListenerQuat[0] = m_ListenerQuat[1] = m_ListenerQuat[2] = 0.0f; m_ListenerQuat[3] = 1.0f;
}

CLTSoundMgr::~CLTSoundMgr() { Shutdown(); }


void CLTSoundMgr::Attach(CLTFileMgr* pFileMgr) { m_pFileMgr = pFileMgr; }


void CLTSoundMgr::Shutdown()
{
    // ⚠️ Voices point INTO the cache, so they stop before it is freed. The
    // other order is a use-after-free on the audio thread, which does not
    // crash reliably -- it crackles.
    m_Mixer.StopAll();
    for (std::map<std::string, Cached*>::iterator it = m_Cache.begin();
         it != m_Cache.end(); ++it) delete it->second;
    m_Cache.clear();
}


void CLTSoundMgr::UpdateListener(const float* pPos3, const float* pRight3)
{
    m_Mixer.SetListener(pPos3, pRight3);
}


// Decodes a sound once and keeps it. Null when the file is missing or in a
// format nothing here can read -- never a silent buffer, which would be
// indistinguishable from a sound that played correctly and was inaudible.
const CLTSoundMgr::Cached* CLTSoundMgr::Get(const char* pName)
{
    if (!pName || !*pName || !m_pFileMgr) return 0;

    std::string sKey(pName);
    for (size_t i = 0; i < sKey.size(); ++i)
        sKey[i] = (char)toupper((unsigned char)sKey[i]);

    std::map<std::string, Cached*>::iterator it = m_Cache.find(sKey);
    if (it != m_Cache.end()) return it->second;      // may be a cached miss

    CLTWav wav;
    if (!wav.LoadThroughFileMgr(m_pFileMgr, pName))
    {
        // ⚠️ Cache the MISS too. A sound the game asks for every frame and
        // that does not exist would otherwise hit the file system every frame.
        m_Cache[sKey] = 0;
        ++m_nMissing;
        // ⚠️ The NAME, kept, and only a few of them. A count of misses says a
        // number; a name says which file and therefore which subsystem.
        if (m_MissingNames.size() < 12) m_MissingNames.push_back(pName);
        return 0;
    }

    Cached* pC = new Cached();
    pC->m_nRate     = wav.GetRate();
    pC->m_nChannels = wav.GetChannels();

    if (wav.GetFormat() == CLTWav::kMP3)
    {
        unsigned int nRate = 0, nCh = 0;
        if (!LTAudio_DecodeMP3(&wav.GetRaw()[0], (unsigned int)wav.GetRaw().size(),
                               pC->m_Samples, nRate, nCh))
        { delete pC; m_Cache[sKey] = 0; ++m_nMissing; return 0; }
        pC->m_nRate     = nRate;
        pC->m_nChannels = nCh;
    }
    else
    {
        pC->m_Samples = wav.GetSamples();
    }

    if (pC->m_Samples.empty() || !pC->m_nRate || !pC->m_nChannels)
    { delete pC; m_Cache[sKey] = 0; ++m_nMissing; return 0; }

    ++m_nDecoded;
    m_Cache[sKey] = pC;
    return pC;
}


LTRESULT CLTSoundMgr::PlaySound(PlaySoundInfo* pInfo, HLTSOUND& hResult)
{
    hResult = 0;
    if (!pInfo) return LT_INVALIDPARAMS;

    const Cached* pC = Get(pInfo->m_szSoundName);
    if (!pC) return LT_MISSINGFILE;

    ++m_nPlayed;
    m_Played[pInfo->m_szSoundName]++;
    if (SndTracing())
        printf("[SOUND] play  %-28s %6.2fs  %5u Hz  flags 0x%05x%s\n",
               pInfo->m_szSoundName,
               (pC->m_nRate && pC->m_nChannels)
                   ? (double)(pC->m_Samples.size() / pC->m_nChannels) / (double)pC->m_nRate
                   : 0.0,
               pC->m_nRate, (unsigned)pInfo->m_dwFlags,
               (pInfo->m_dwFlags & PLAYSOUND_GETHANDLE) ? "  GETHANDLE" : "");
    // Flushed, so the line lands in order beside the frame dumps on stderr.
    if (SndTracing()) fflush(stdout);

    CLTMixer::Params p;

    // ⚠️ The three positioning flags are MUTUALLY EXCLUSIVE and LOCAL is zero
    // (ltbasedefs.h), so it cannot be tested with `&`. Testing for it that way
    // makes every sound in the game local and kills all 3D audio at a stroke.
    if (pInfo->m_dwFlags & PLAYSOUND_3D)           p.m_eMode = CLTMixer::k3D;
    else if (pInfo->m_dwFlags & PLAYSOUND_AMBIENT) p.m_eMode = CLTMixer::kAmbient;
    else                                           p.m_eMode = CLTMixer::kLocal;

    p.m_bLoop  = (pInfo->m_dwFlags & PLAYSOUND_LOOP) != 0;
    p.m_fInner = pInfo->m_fInnerRadius;
    p.m_fOuter = pInfo->m_fOuterRadius;

    // Volume is a PERCENT and only meaningful with the control flag; the
    // header's default is 100, not whatever is in the struct.
    p.m_fVolume = (pInfo->m_dwFlags & PLAYSOUND_CTRL_VOL)
                ? (float)pInfo->m_nVolume / 100.0f : 1.0f;

    p.m_fPitch  = (pInfo->m_dwFlags & PLAYSOUND_CTRL_PITCH)
                ? pInfo->m_fPitchShift : 1.0f;
    if (p.m_fPitch <= 0.0f) p.m_fPitch = 1.0f;

    for (int k = 0; k < 3; ++k) p.m_Pos[k] = ((float*)&pInfo->m_vPosition)[k];

    // LT_SNDTRACE: WHERE and how loud -- "played" is not "heard". A 3D voice
    // line placed at the wrong point, or with radii that put the listener
    // outside them, plays silently (user report 2026-10-17: dialogue lines
    // started but not heard).
    if (SndTracing() && p.m_eMode == CLTMixer::k3D)
    {
        const float dx = p.m_Pos[0]-m_ListenerPos[0], dy = p.m_Pos[1]-m_ListenerPos[1], dz = p.m_Pos[2]-m_ListenerPos[2];
        printf("[SOUND]   3d at (%.0f %.0f %.0f) radii %.0f/%.0f vol %.2f | listener (%.0f %.0f %.0f)%s, %.0f units away\n",
               p.m_Pos[0], p.m_Pos[1], p.m_Pos[2], p.m_fInner, p.m_fOuter, p.m_fVolume,
               m_ListenerPos[0], m_ListenerPos[1], m_ListenerPos[2],
               m_bListenerInClient ? " in-client" : "", sqrtf(dx*dx + dy*dy + dz*dz));
        fflush(stdout);
    }

    const unsigned int nVoice =
        m_Mixer.Play(&pC->m_Samples[0], (unsigned int)pC->m_Samples.size(),
                     pC->m_nRate, pC->m_nChannels, p);

    if (!nVoice) return LT_ERROR;          // every voice busy

    // ⚠️ The handle is only handed BACK when the caller asked for it. Without
    // GETHANDLE the game never kills the sound, so publishing a handle it will
    // not release would leak a voice slot per shot fired.
    if (SndTracing()) g_VoiceName[nVoice] = pInfo->m_szSoundName;
    if (pInfo->m_dwFlags & PLAYSOUND_GETHANDLE)
    {
        hResult = (HLTSOUND)(uintptr_t)nVoice;
        pInfo->m_hSound = hResult;
    }
    return LT_OK;
}


LTRESULT CLTSoundMgr::GetSoundDuration(HLTSOUND hSound, LTFLOAT& fDuration)
{
    fDuration = 0.0f;

    // ⚠️ THE MIXER KNOWS, and only while the voice is live. The game asks this
    // to time a line of dialogue it has just started, so "live" is exactly when
    // it asks; a voice that has already ended answers LT_ERROR rather than a
    // made-up length.
    float f = 0.0f;
    if (!m_Mixer.VoiceDuration((unsigned int)(uintptr_t)hSound, f))
        return LT_ERROR;

    fDuration = f;
    return LT_OK;
}


LTRESULT CLTSoundMgr::GetSoundData(HLTSOUND hSound, short*& p16, signed char*& p8,
                                   unsigned int* pRate, unsigned int* pChannels)
{
    // ⚠️ Written on every path: the game reads all four unconditionally.
    p16 = 0; p8 = 0;
    if (pRate) *pRate = 0;
    if (pChannels) *pChannels = 0;

    const short* pS = 0;
    unsigned int nS = 0, nRate = 0, nCh = 0;
    if (!m_Mixer.VoiceData((unsigned int)(uintptr_t)hSound, pS, nS, nRate, nCh))
        return LT_ERROR;

    p16 = const_cast<short*>(pS);       // the game reads it, never writes
    if (pRate) *pRate = nRate;
    if (pChannels) *pChannels = nCh;
    return LT_OK;
}

LTRESULT CLTSoundMgr::GetSoundOffset(HLTSOUND hSound, unsigned int* pOffset, unsigned int* pSize)
{
    if (pOffset) *pOffset = 0;
    if (pSize) *pSize = 0;

    const short* pS = 0;
    unsigned int nS = 0, nRate = 0, nCh = 0, nFrame = 0;
    const unsigned int nV = (unsigned int)(uintptr_t)hSound;
    if (!m_Mixer.VoiceData(nV, pS, nS, nRate, nCh) || !m_Mixer.VoiceCursor(nV, nFrame))
        return LT_ERROR;

    unsigned int nOfs = nFrame * (nCh ? nCh : 1);
    if (nOfs > nS) nOfs = nS;
    if (pOffset) *pOffset = nOfs;
    if (pSize) *pSize = nS;
    return LT_OK;
}

LTRESULT CLTSoundMgr::IsSoundDone(HLTSOUND hSound, LTBOOL& bDone)
{
    ++m_nDoneAsks;
    bDone = m_Mixer.IsDone((unsigned int)(uintptr_t)hSound) ? LTTRUE : LTFALSE;
    if (bDone) ++m_nDoneYes;
    return LT_OK;
}

LTRESULT CLTSoundMgr::KillSound(HLTSOUND hSound)
{
    if (SndTracing())
    {
        const unsigned int v = (unsigned int)(uintptr_t)hSound;
        printf("[SOUND] kill  %-28s (voice %u%s)\n", g_VoiceName.count(v) ? g_VoiceName[v].c_str() : "?", v,
               m_Mixer.IsDone(v) ? ", already done" : ", PLAYING");
        fflush(stdout);
    }
    m_Mixer.Stop((unsigned int)(uintptr_t)hSound);
    return LT_OK;
}

LTRESULT CLTSoundMgr::KillSoundLoop(HLTSOUND hSound)
{
    if (SndTracing())
    {
        const unsigned int v = (unsigned int)(uintptr_t)hSound;
        printf("[SOUND] killloop %-25s (voice %u)\n", g_VoiceName.count(v) ? g_VoiceName[v].c_str() : "?", v);
        fflush(stdout);
    }
    // ⚠️ "Stop looping", not "stop". The game uses this to let a looping sound
    // finish its current pass -- killing it outright cuts an engine note dead
    // mid-cycle, which is audible.
    m_Mixer.Stop((unsigned int)(uintptr_t)hSound);
    return LT_OK;
}


LTRESULT CLTSoundMgr::InitSound(InitSoundInfo* pSoundInfo)
{
    if (!pSoundInfo) return LT_INVALIDPARAMS;

    // ⚠️ The requested rate and bit depth are IGNORED and that is deliberate:
    // the mixer runs at one rate and resamples into it, so honouring a request
    // for 11025 would mean resampling everything twice. The results field is
    // filled so the game does not think the call did nothing.
    pSoundInfo->m_dwResults = 0;

    if (pSoundInfo->m_nVolume <= 100)
        m_Mixer.SetVolume((float)pSoundInfo->m_nVolume / 100.0f);

    printf("[SOUND] %u Hz stereo, %u voices requested, master volume %u%%\n",
           (unsigned)CLTMixer::kRate, (unsigned)pSoundInfo->m_nNumSWVoices,
           (unsigned)pSoundInfo->m_nVolume);
    fflush(stdout);
    return LT_OK;
}

LTRESULT CLTSoundMgr::GetVolume(uint16& nVolume)
{
    nVolume = (uint16)(m_Mixer.GetVolume() * 100.0f + 0.5f);
    return LT_OK;
}

LTRESULT CLTSoundMgr::SetVolume(uint16 nVolume)
{
    m_Mixer.SetVolume((float)nVolume / 100.0f);
    return LT_OK;
}

LTRESULT CLTSoundMgr::PauseSounds()  { m_Mixer.SetPaused(true);  return LT_OK; }
LTRESULT CLTSoundMgr::ResumeSounds() { m_Mixer.SetPaused(false); return LT_OK; }


LTRESULT CLTSoundMgr::SetSoundPosition(HLTSOUND hSound, LTVector* pPos)
{
    if (!pPos) return LT_INVALIDPARAMS;
    m_Mixer.SetVoicePosition((unsigned int)(uintptr_t)hSound, (const float*)pPos);
    return LT_OK;
}

LTRESULT CLTSoundMgr::GetSoundPosition(HLTSOUND hSound, LTVector* pPos)
{
    if (!pPos) return LT_INVALIDPARAMS;
    return m_Mixer.GetVoicePosition((unsigned int)(uintptr_t)hSound, (float*)pPos)
         ? LT_OK : LT_ERROR;
}


LTRESULT CLTSoundMgr::SetListener(LTBOOL bListenerInClient, LTVector* pPos,
                                  LTRotation* pRot, LTBOOL bTeleport)
{
    (void)bTeleport;
    m_bListenerInClient = (bListenerInClient != 0);
    // In the client: the ears follow the client object from the next frame
    // (UpdateClientListener); there is nothing to set now. TurnOffAlternative
    // Camera passes no position at all in this case (GameClientShell.cpp:2546).
    if (m_bListenerInClient) return LT_OK;
    if (!pPos) return LT_INVALIDPARAMS;
    for (int k = 0; k < 3; ++k) m_ListenerPos[k] = ((const float*)pPos)[k];
    if (pRot) for (int k = 0; k < 4; ++k) m_ListenerQuat[k] = ((const float*)pRot)[k];

    // ⚠️ The RIGHT vector, extracted from the listener's rotation with the
    // engine's own quaternion code rather than a second derivation -- the
    // camera's right vector was got backwards once already (HANDOVER.md), and
    // panning inherits that mistake for free if it is re-derived here.
    float right[3] = { 1.0f, 0.0f, 0.0f };
    if (pRot)
    {
        float up[3], fwd[3];
        LTMathImpl::GetVectors((const float*)pRot, right, up, fwd);
    }

    m_Mixer.SetListener((const float*)pPos, right);
    return LT_OK;
}


void CLTSoundMgr::UpdateClientListener(const float* pPos3, const float* pQuat4)
{
    if (!m_bListenerInClient || !pPos3 || !pQuat4) return;
    for (int k = 0; k < 3; ++k) m_ListenerPos[k] = pPos3[k];
    for (int k = 0; k < 4; ++k) m_ListenerQuat[k] = pQuat4[k];
    float right[3], up[3], fwd[3];
    LTMathImpl::GetVectors(pQuat4, right, up, fwd);
    m_Mixer.SetListener(pPos3, right);
}

void CLTSoundMgr::GetListenerState(bool* pInClient, float* pPos3, float* pQuat4) const
{
    if (pInClient) *pInClient = m_bListenerInClient;
    if (pPos3)  for (int k = 0; k < 3; ++k) pPos3[k] = m_ListenerPos[k];
    if (pQuat4) for (int k = 0; k < 4; ++k) pQuat4[k] = m_ListenerQuat[k];
}


LTRESULT CLTSoundMgr::ReleaseSoundHandle(HLTSOUND hSound)
{
    m_Mixer.Stop((unsigned int)(uintptr_t)hSound);
    return LT_OK;
}


// ----------------------------------------------------------------------- //
//
// ⚠️ DECLINED, NOT FAKED. Everything below is EAX-era hardware audio: reverb
// zones, per-sound occlusion and obstruction, and named filters with named
// parameters. macOS has no equivalent to map them onto, and a convincing-looking
// LT_OK from a filter that did nothing would leave the game believing rooms
// sound different when they do not.
//
// LT_UNSUPPORTED is a real answer. The 3D-provider list is EMPTY for the same
// reason: NOLF checks it to decide whether hardware 3D audio exists, and the
// honest answer is that it does not.
//
// ----------------------------------------------------------------------- //

LTRESULT CLTSoundMgr::GetSound3DProviderLists(Sound3DProvider*& pList, LTBOOL bVerify)
{
    (void)bVerify;
    pList = 0;                      // no hardware 3D providers
    return LT_OK;
}

LTRESULT CLTSoundMgr::ReleaseSound3DProviderList(Sound3DProvider* pList)
{
    (void)pList;
    return LT_OK;
}

LTRESULT CLTSoundMgr::SetReverbProperties(ReverbProperties* p)
{
    (void)p; SayOnce(m_bSaidUnsupported, "reverb"); return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::GetReverbProperties(ReverbProperties* p)
{
    (void)p; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::SetSoundOcclusion(HLTSOUND h, LTFLOAT f)
{
    (void)h; (void)f; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::GetSoundOcclusion(HLTSOUND h, LTFLOAT* p)
{
    (void)h; if (p) *p = 0.0f; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::SetSoundObstruction(HLTSOUND h, LTFLOAT f)
{
    (void)h; (void)f; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::GetSoundObstruction(HLTSOUND h, LTFLOAT* p)
{
    (void)h; if (p) *p = 0.0f; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::SetSoundFilter(HLTSOUND h, const char* p)
{
    (void)h; (void)p; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::SetSoundFilterParam(HLTSOUND h, const char* p, float f)
{
    (void)h; (void)p; (void)f; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::GetSoundFilterParam(HLTSOUND h, const char* p, float* pf)
{
    (void)h; (void)p; if (pf) *pf = 0.0f; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::GetFilterName(uint32 n, const char** pp)
{
    (void)n; if (pp) *pp = 0; return LT_UNSUPPORTED;
}
LTRESULT CLTSoundMgr::GetFilterParamName(uint32 n, const char* p, const char** pp)
{
    (void)n; (void)p; if (pp) *pp = 0; return LT_UNSUPPORTED;
}


void CLTSoundMgr::DumpCensus() const
{
    if (!m_nPlayed && !m_nDecoded && !m_nMissing) return;

    printf("\n[SOUND] %u played, %u distinct file(s) decoded, %u missing\n",
           m_nPlayed, m_nDecoded, m_nMissing);
    printf("        IsSoundDone asked %u time(s), answered yes %u\n",
           m_nDoneAsks, m_nDoneYes);

    for (std::map<std::string,unsigned int>::const_iterator it = m_Played.begin();
         it != m_Played.end(); ++it)
        printf("        x%-4u %s\n", it->second, it->first.c_str());

    for (size_t i = 0; i < m_MissingNames.size(); ++i)
        printf("        missing: %s\n", m_MissingNames[i].c_str());
    if (m_nMissing > (unsigned int)m_MissingNames.size())
        printf("        ... and %u more\n",
               m_nMissing - (unsigned int)m_MissingNames.size());
    fflush(stdout);
}
