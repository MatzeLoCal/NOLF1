// ----------------------------------------------------------------------- //
//
// MODULE  : ltmixer.cpp
//
// See ltmixer.h.
//
// ----------------------------------------------------------------------- //

#include "ltmixer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

namespace
{
    // ⚠️ SLOTS, NOT VOICES (5et). Every sound that lives holds a slot; only
    // the best m_nChannelsMax in earshot are mixed (Assign). The old 32 were
    // both at once, so a level's silent ambient loops -- 29 of 32 in M14S01,
    // thousands of units away at gain 0 -- held every voice and the player's
    // own gun was refused. 256 is a CHOICE: the cap still turns a bug that
    // starts a sound per frame into refused sounds, not unbounded memory. The
    // vector is reserved once and never grows past it, because the device
    // thread walks it. LT_OLDVOICES=1 = the old 32, first come first served.
    bool OldVoices()
    {
        static const bool b = []() {
            const bool bOld = (getenv("LT_OLDVOICES") != 0);
            if (bOld) { printf("[SOUND] LT_OLDVOICES=1 -- 32 voices, first come first served, silent loops hold theirs (old)\n"); fflush(stdout); }
            return bOld; }();
        return b;
    }
    const size_t kMaxSlots = 256;
    size_t MaxSlots() { return OldVoices() ? 32 : kMaxSlots; }

    inline float Clamp01(float f) { return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f); }
}


CLTMixer::CLTMixer()
    : m_nNextId(1), m_fMaster(1.0f), m_bPaused(false)
{
    m_Listener[0] = m_Listener[1] = m_Listener[2] = 0.0f;
    m_Right[0] = 1.0f; m_Right[1] = 0.0f; m_Right[2] = 0.0f;
    m_Voices.reserve(kMaxSlots);
}


// ⚠️ THE ROLLOFF, AND IT IS A STATED CHOICE RATHER THAN A RECOVERED FACT.
// PlaySoundInfo gives an inner radius ("maximum volume inside this") and an
// outer one ("no sound outside this") and says nothing about the curve between
// them. This uses INVERSE-DISTANCE between the two, normalised so it is exactly
// 1 at the inner radius and 0 at the outer -- which is how distance actually
// behaves and is far closer than a straight line, but it is not claimed to be
// what the original engine did. If the real curve is ever recovered, this is
// the one place it changes.
float CLTMixer::DistanceGain(float fDistance, float fInner, float fOuter)
{
    if (fOuter <= 0.0f)        return 1.0f;        // no radii: not attenuated
    if (fDistance <= fInner)   return 1.0f;
    if (fDistance >= fOuter)   return 0.0f;
    if (fOuter <= fInner)      return 1.0f;

    // inverse distance, then rescaled so g(inner)=1 and g(outer)=0
    const float fRef = (fInner > 0.0f) ? fInner : 1.0f;
    const float g    = fRef / fDistance;
    const float gOut = fRef / fOuter;
    return Clamp01((g - gOut) / (1.0f - gOut));
}


void CLTMixer::SetListener(const float* pPos3, const float* pRight3)
{
    if (pPos3)   for (int k = 0; k < 3; ++k) m_Listener[k] = pPos3[k];
    if (pRight3)
    {
        float l = sqrtf(pRight3[0]*pRight3[0] + pRight3[1]*pRight3[1] + pRight3[2]*pRight3[2]);
        if (l > 1e-6f) for (int k = 0; k < 3; ++k) m_Right[k] = pRight3[k] / l;
    }
    // The ears moved: who is in earshot, and who ranks, changes with them.
    // Once a frame at least (the host updates the listener every frame), as
    // the donor's CSoundMgr::Update re-sorts every frame (5et).
    Assign();
}


void CLTMixer::SetVolume(float f) { m_fMaster = Clamp01(f); }


CLTMixer::Voice* CLTMixer::Find(unsigned int nId)
{
    for (size_t i = 0; i < m_Voices.size(); ++i)
        if (m_Voices[i].m_nId == nId && m_Voices[i].m_bActive) return &m_Voices[i];
    return 0;
}

const CLTMixer::Voice* CLTMixer::Find(unsigned int nId) const
{
    for (size_t i = 0; i < m_Voices.size(); ++i)
        if (m_Voices[i].m_nId == nId && m_Voices[i].m_bActive) return &m_Voices[i];
    return 0;
}


unsigned int CLTMixer::Play(const short* pSamples, unsigned int nSamples,
                            unsigned int nRate, unsigned int nChannels,
                            const Params& p)
{
    if (!pSamples || !nSamples || !nRate || !nChannels) return 0;

    // A free slot the device thread cannot still be reading: retired by the
    // renderer itself, or freed here before the last completed render pass.
    // (Not for LT_OLDVOICES, which reused any free slot at once.)
    const unsigned int nGen = m_nRenderGen;
    const bool bGuard = !OldVoices();
    Voice* pV = 0;
    for (size_t i = 0; i < m_Voices.size() && !pV; ++i)
        if (!m_Voices[i].m_bActive && (!bGuard || nGen > m_Voices[i].m_nFreedGen)) pV = &m_Voices[i];

    if (!pV)
    {
        if (m_Voices.size() >= MaxSlots()) { ++m_nDropped; return 0; }    // refused, not grown
        m_Voices.push_back(Voice());
        pV = &m_Voices.back();
    }

    pV->m_pSamples  = pSamples;
    pV->m_nSamples  = nSamples;
    pV->m_nRate     = nRate;
    pV->m_nChannels = nChannels;
    pV->m_fCursor   = 0.0;
    pV->m_Params    = p;
    pV->m_bAudible  = OldVoices();      // Assign decides, before any render can mix it
    pV->m_nSerial   = ++m_nSerial;
    pV->m_bActive   = true;
    pV->m_nId       = m_nNextId++;
    if (m_nNextId == 0) m_nNextId = 1;      // never hand out 0: it means "none"
    const unsigned int nNow = ActiveVoices();
    if (nNow > m_nPeak) m_nPeak = nNow;
    const unsigned int nId = pV->m_nId;
    Assign();
    return nId;
}


void CLTMixer::Assign()
{
    if (OldVoices()) return;

    struct Rank { int i; bool bLocal, bEar, bHeard; float fScore; unsigned int nSerial; };
    static std::vector<Rank> s_R;           // main thread only
    s_R.clear();
    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        const Voice& v = m_Voices[i];
        if (!v.m_bActive) continue;
        Rank r;
        r.i = (int)i;
        r.bLocal = (v.m_Params.m_eMode == kLocal);
        const float fVol = Clamp01(v.m_Params.m_fVolume);
        float fDist = 0.0f;
        if (!r.bLocal)
        {
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = v.m_Params.m_Pos[k] - m_Listener[k];
            fDist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        }
        // The donor's earshot (soundinstance.cpp PreUpdatePositionalSound):
        // within 1.5 x the outer radius and not at volume 0. A local sound is
        // always in earshot; no radius means not attenuated, so in earshot.
        r.bEar = (fVol > 0.0f) &&
                 (r.bLocal || v.m_Params.m_fOuter <= 0.0f || fDist <= 1.5f * v.m_Params.m_fOuter);
        // ...and its rank: the game's priority, plus volume / distance in 0..1.
        r.fScore = (float)v.m_Params.m_nPriority + (fDist > 1.0f ? fVol / fDist : fVol);
        r.bHeard = v.m_bAudible;
        r.nSerial = v.m_nSerial;
        s_R.push_back(r);
    }
    std::sort(s_R.begin(), s_R.end(), [](const Rank& a, const Rank& b) {
        if (a.bLocal != b.bLocal) return a.bLocal;          // local sounds first (CompareSoundInstances)
        if (a.fScore != b.fScore) return a.fScore > b.fScore;
        if (a.bHeard != b.bHeard) return a.bHeard;          // playing beats not yet playing
        return a.nSerial > b.nSerial;                       // then the newest
    });
    unsigned int n = 0;
    for (size_t k = 0; k < s_R.size(); ++k)
    {
        const bool bWant = s_R[k].bEar && n < m_nChannelsMax;
        if (bWant) ++n;
        m_Voices[(size_t)s_R[k].i].m_bAudible = bWant;
        m_Voices[(size_t)s_R[k].i].m_bEar     = s_R[k].bEar;
    }
}


void CLTMixer::Snapshot(std::vector<VoiceInfo>& out) const
{
    out.clear();
    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        const Voice& v = m_Voices[i];
        if (!v.m_bActive) continue;
        float fL = 0.0f, fR = 0.0f;
        GainsFor(v, fL, fR);
        VoiceInfo vi;
        vi.m_pTag = v.m_Params.m_pTag; vi.m_eMode = v.m_Params.m_eMode;
        vi.m_nPriority = v.m_Params.m_nPriority; vi.m_bLoop = v.m_Params.m_bLoop;
        vi.m_bHeard = v.m_bAudible;
        vi.m_fGain = fL > fR ? fL : fR;
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = v.m_Params.m_Pos[k] - m_Listener[k];
        vi.m_fDist = (v.m_Params.m_eMode == kLocal) ? 0.0f : sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        out.push_back(vi);
    }
}


void CLTMixer::Stop(unsigned int nVoice)
{
    if (Voice* pV = Find(nVoice)) { pV->m_nFreedGen = m_nRenderGen; pV->m_bActive = false; }
}

void CLTMixer::StopAll()
{
    for (size_t i = 0; i < m_Voices.size(); ++i)
        if (m_Voices[i].m_bActive) { m_Voices[i].m_nFreedGen = m_nRenderGen; m_Voices[i].m_bActive = false; }
}

bool CLTMixer::IsDone(unsigned int nVoice) const { return Find(nVoice) == 0; }

bool CLTMixer::VoiceDuration(unsigned int nVoice, float& fSeconds) const
{
    fSeconds = 0.0f;
    const Voice* pV = Find(nVoice);
    if (!pV || !pV->m_nRate || !pV->m_nChannels) return false;

    const float fPitch = (pV->m_Params.m_fPitch > 0.0f) ? pV->m_Params.m_fPitch : 1.0f;
    fSeconds = (float)(pV->m_nSamples / pV->m_nChannels)
             / ((float)pV->m_nRate * fPitch);
    return true;
}

bool CLTMixer::VoiceData(unsigned int nVoice, const short*& pSamples,
                         unsigned int& nSamples, unsigned int& nRate,
                         unsigned int& nChannels) const
{
    const Voice* pV = Find(nVoice);
    if (!pV || !pV->m_pSamples) return false;
    pSamples  = pV->m_pSamples;
    nSamples  = pV->m_nSamples;
    nRate     = pV->m_nRate;
    nChannels = pV->m_nChannels;
    return true;
}

bool CLTMixer::VoiceCursor(unsigned int nVoice, unsigned int& nFrame) const
{
    const Voice* pV = Find(nVoice);
    if (!pV) return false;
    nFrame = (pV->m_fCursor > 0.0) ? (unsigned int)pV->m_fCursor : 0u;
    return true;
}

void CLTMixer::SetVoicePosition(unsigned int nVoice, const float* pPos3)
{
    if (!pPos3) return;
    if (Voice* pV = Find(nVoice))
        for (int k = 0; k < 3; ++k) pV->m_Params.m_Pos[k] = pPos3[k];
}

bool CLTMixer::GetVoicePosition(unsigned int nVoice, float* pPos3) const
{
    const Voice* pV = Find(nVoice);
    if (!pV || !pPos3) return false;
    for (int k = 0; k < 3; ++k) pPos3[k] = pV->m_Params.m_Pos[k];
    return true;
}

unsigned int CLTMixer::ActiveVoices() const
{
    unsigned int n = 0;
    for (size_t i = 0; i < m_Voices.size(); ++i) if (m_Voices[i].m_bActive) ++n;
    return n;
}


void CLTMixer::GainsFor(const Voice& v, float& fL, float& fR) const
{
    float g = Clamp01(v.m_Params.m_fVolume) * m_fMaster;

    if (v.m_Params.m_eMode == kLocal)
    {
        fL = fR = g;                    // inside the head: centred, unattenuated
        return;
    }

    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = v.m_Params.m_Pos[k] - m_Listener[k];
    const float fDist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);

    g *= DistanceGain(fDist, v.m_Params.m_fInner, v.m_Params.m_fOuter);

    if (v.m_Params.m_eMode == kAmbient || fDist < 1e-4f)
    {
        fL = fR = g;                    // ambient: distance but no direction
        return;
    }

    // ⚠️ CONSTANT POWER, not a linear crossfade. A linear pan dips by 3 dB in
    // the middle, so a sound crossing in front of the player gets quieter as it
    // passes -- audible as a hole, and easy to mistake for a distance bug.
    const float fDot  = (d[0]*m_Right[0] + d[1]*m_Right[1] + d[2]*m_Right[2]) / fDist;
    const float fPan  = Clamp01((fDot + 1.0f) * 0.5f);   // 0 = hard left
    const float kHalfPi = 1.57079633f;

    fL = g * cosf(fPan * kHalfPi);
    fR = g * sinf(fPan * kHalfPi);
}


static LTMixerMusicFn volatile g_pfnMusic = 0;
void LTMixer_SetMusicSource(LTMixerMusicFn fn) { g_pfnMusic = fn; }

void CLTMixer::Render(short* pOut, unsigned int nFrames)
{
    if (!pOut || !nFrames) return;
    memset(pOut, 0, sizeof(short) * (size_t)nFrames * kChannels);

    // The music first, in stack-sized pieces (no allocation on this thread).
    if (LTMixerMusicFn fn = g_pfnMusic)
    {
        float buf[512 * 2];
        for (unsigned int f0 = 0; f0 < nFrames; f0 += 512)
        {
            const unsigned int n = (nFrames - f0 < 512) ? nFrames - f0 : 512;
            memset(buf, 0, sizeof(float) * n * 2);
            fn(buf, n);
            for (unsigned int i = 0; i < n * 2; ++i)
            {
                int v = (int)(buf[i] * 32767.0f);
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                pOut[(size_t)f0 * 2 + i] = (short)v;
            }
        }
    }
    if (m_bPaused) { m_nRenderGen = m_nRenderGen + 1; return; }

    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        Voice& v = m_Voices[i];
        if (!v.m_bActive) continue;

        // Running but not heard (no channel, 5et): time passes, nothing mixed.
        if (!v.m_bAudible) { AdvanceVoice(v, nFrames); continue; }

        float fL = 0.0f, fR = 0.0f;
        GainsFor(v, fL, fR);

        // ⚠️ Advance by the RATE RATIO, so a 22050 Hz sound plays at its
        // recorded pitch in a 44100 Hz mix. Two thirds of this game's audio is
        // not at the mix rate, so stepping one sample per frame would play most
        // of it an octave out.
        const double fStep = ((double)v.m_nRate / (double)kRate)
                           * (v.m_Params.m_fPitch > 0.0f ? v.m_Params.m_fPitch : 1.0f);

        const unsigned int nFramesIn = v.m_nSamples / v.m_nChannels;

        for (unsigned int f = 0; f < nFrames; ++f)
        {
            if (v.m_fCursor >= (double)nFramesIn)
            {
                if (!v.m_Params.m_bLoop) { v.m_bActive = false; break; }
                v.m_fCursor -= (double)nFramesIn;
            }

            const unsigned int nAt = (unsigned int)v.m_fCursor;
            if (nAt >= nFramesIn) { v.m_bActive = false; break; }

            // Mono sources are the overwhelming majority (4,796 of 4,798);
            // a stereo source is folded by taking its own two channels.
            int s0, s1;
            if (v.m_nChannels >= 2)
            {
                s0 = v.m_pSamples[(size_t)nAt * v.m_nChannels + 0];
                s1 = v.m_pSamples[(size_t)nAt * v.m_nChannels + 1];
            }
            else
            {
                s0 = s1 = v.m_pSamples[nAt];
            }

            int l = pOut[(size_t)f * 2 + 0] + (int)(s0 * fL);
            int r = pOut[(size_t)f * 2 + 1] + (int)(s1 * fR);

            // ⚠️ Clamp, never wrap. A wrapped mix does not sound like a loud
            // mix, it sounds like tearing static, and it is what an int16 sum
            // does on its own.
            if (l >  32767) l =  32767;
            if (l < -32768) l = -32768;
            if (r >  32767) r =  32767;
            if (r < -32768) r = -32768;

            pOut[(size_t)f * 2 + 0] = (short)l;
            pOut[(size_t)f * 2 + 1] = (short)r;

            v.m_fCursor += fStep;
        }
    }
    m_nRenderGen = m_nRenderGen + 1;     // this pass is done (see m_nFreedGen)
}


void CLTMixer::Advance(unsigned int nFrames)
{
    // ⚠️ PAUSED MEANS PAUSED, here as in Render. A paused mixer whose cursors
    // kept moving would let the game's pause menu silently run every line of
    // dialogue to its end behind the player's back.
    if (!nFrames || m_bPaused) return;

    for (size_t i = 0; i < m_Voices.size(); ++i)
        if (m_Voices[i].m_bActive) AdvanceVoice(m_Voices[i], nFrames);
    m_nRenderGen = m_nRenderGen + 1;
}


void CLTMixer::AdvanceVoice(Voice& v, unsigned int nFrames)
{
    const double fStep = ((double)v.m_nRate / (double)kRate)
                       * (v.m_Params.m_fPitch > 0.0f ? v.m_Params.m_fPitch : 1.0f);

    const unsigned int nFramesIn = v.m_nSamples / v.m_nChannels;
    if (!nFramesIn) { v.m_bActive = false; return; }

    v.m_fCursor += fStep * (double)nFrames;

    if (v.m_fCursor < (double)nFramesIn) return;

    if (!v.m_Params.m_bLoop) { v.m_bActive = false; return; }

    // fmod, not a subtract: a frame's worth of time can be many passes of
    // a short loop, and one subtraction would leave the cursor past the end
    // and retire a looping voice that should never retire.
    v.m_fCursor = fmod(v.m_fCursor, (double)nFramesIn);
}
