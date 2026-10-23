// ----------------------------------------------------------------------- //
//
// MODULE  : ltmixer.cpp
//
// See ltmixer.h.
//
// ----------------------------------------------------------------------- //

#include "ltmixer.h"

#include <math.h>
#include <string.h>

namespace
{
    // ⚠️ 32 is a CHOICE, not a limit of the format. NOLF plays a handful of
    // sounds at once; the cap exists so a bug that starts a voice per frame
    // degrades into dropped sounds instead of unbounded memory.
    const size_t kMaxVoices = 32;

    inline float Clamp01(float f) { return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f); }
}


CLTMixer::CLTMixer()
    : m_nNextId(1), m_fMaster(1.0f), m_bPaused(false)
{
    m_Listener[0] = m_Listener[1] = m_Listener[2] = 0.0f;
    m_Right[0] = 1.0f; m_Right[1] = 0.0f; m_Right[2] = 0.0f;
    m_Voices.reserve(kMaxVoices);
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

    Voice* pV = 0;
    for (size_t i = 0; i < m_Voices.size() && !pV; ++i)
        if (!m_Voices[i].m_bActive) pV = &m_Voices[i];

    if (!pV)
    {
        if (m_Voices.size() >= kMaxVoices) return 0;    // dropped, not grown
        m_Voices.push_back(Voice());
        pV = &m_Voices.back();
    }

    pV->m_pSamples  = pSamples;
    pV->m_nSamples  = nSamples;
    pV->m_nRate     = nRate;
    pV->m_nChannels = nChannels;
    pV->m_fCursor   = 0.0;
    pV->m_Params    = p;
    pV->m_bActive   = true;
    pV->m_nId       = m_nNextId++;
    if (m_nNextId == 0) m_nNextId = 1;      // never hand out 0: it means "none"
    return pV->m_nId;
}


void CLTMixer::Stop(unsigned int nVoice)
{
    if (Voice* pV = Find(nVoice)) pV->m_bActive = false;
}

void CLTMixer::StopAll()
{
    for (size_t i = 0; i < m_Voices.size(); ++i) m_Voices[i].m_bActive = false;
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
    if (m_bPaused) return;

    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        Voice& v = m_Voices[i];
        if (!v.m_bActive) continue;

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
}


void CLTMixer::Advance(unsigned int nFrames)
{
    // ⚠️ PAUSED MEANS PAUSED, here as in Render. A paused mixer whose cursors
    // kept moving would let the game's pause menu silently run every line of
    // dialogue to its end behind the player's back.
    if (!nFrames || m_bPaused) return;

    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        Voice& v = m_Voices[i];
        if (!v.m_bActive) continue;

        const double fStep = ((double)v.m_nRate / (double)kRate)
                           * (v.m_Params.m_fPitch > 0.0f ? v.m_Params.m_fPitch : 1.0f);

        const unsigned int nFramesIn = v.m_nSamples / v.m_nChannels;
        if (!nFramesIn) { v.m_bActive = false; continue; }

        v.m_fCursor += fStep * (double)nFrames;

        if (v.m_fCursor < (double)nFramesIn) continue;

        if (!v.m_Params.m_bLoop) { v.m_bActive = false; continue; }

        // fmod, not a subtract: a frame's worth of time can be many passes of
        // a short loop, and one subtraction would leave the cursor past the end
        // and retire a looping voice that should never retire.
        v.m_fCursor = fmod(v.m_fCursor, (double)nFramesIn);
    }
}
