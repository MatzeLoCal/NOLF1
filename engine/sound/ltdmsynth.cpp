// ------------------------------------------------------------------------- //
//
// ltdmsynth.cpp -- see ltdmsynth.h.
//
// ⚠️ THE ENVELOPE IS IN DECIBELS after the attack, as DLS defines it: decay
// and release times are the time to fall the full 96 dB, so a note reaching
// a sustain of 80 % (-19.2 dB) takes 20 % of the decay time to get there,
// and the sustain level is a fraction of that 96 dB range, not of the
// amplitude. Read as amplitude, every sustained instrument came out far too
// loud against the ones that decay.
//
// ------------------------------------------------------------------------- //

#include "ltdmsynth.h"

#include <math.h>
#include <string.h>

namespace
{
    const float kPi = 3.14159265358979f;
    const float kMinRelease = 0.008f;       // seconds: no click on a zero release
    const float kFloorDB = -96.0f;

    inline float DBToGain(float dB) { return (float)pow(10.0, dB / 20.0); }
}

CLTDMSynth::CLTDMSynth(unsigned int nRate)
    : m_nRate(nRate ? nRate : 44100), m_nMaxVoices(64), m_nAgeCounter(0), m_fGain(1.0f),
      m_bReverb(false), m_fRevIn(0.015f), m_fRevWet(0.0f), m_fRevFeedback(0.84f), m_fRevDamp(0.4f)
{
    m_Voices.resize(256);
    for (size_t i = 0; i < m_Voices.size(); ++i) m_Voices[i].m_bActive = false;

    // Freeverb's tunings at 44.1 kHz, scaled to the rate; +23 on the right.
    static const int kComb[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static const int kAll[4]  = { 556, 441, 341, 225 };
    const double s = m_nRate / 44100.0;
    for (int c = 0; c < 2; ++c)
    {
        for (int i = 0; i < 8; ++i)
        {
            m_Comb[c][i].m_Buf.assign((size_t)((kComb[i] + c * 23) * s) + 1, 0.0f);
            m_Comb[c][i].m_nAt = 0; m_Comb[c][i].m_fStore = 0.0f;
        }
        for (int i = 0; i < 4; ++i)
        {
            m_All[c][i].m_Buf.assign((size_t)((kAll[i] + c * 23) * s) + 1, 0.0f);
            m_All[c][i].m_nAt = 0;
        }
    }
}

void CLTDMSynth::SetReverb(bool bOn, float fInGainDB, float fMixDB, float fTimeMs, float fHFRatio)
{
    m_bReverb = bOn;
    m_fRevIn  = 0.015f * DBToGain(fInGainDB);
    // DSFXWavesReverb's mix: 0 dB = all wet. Freeverb's own wet scale is 3.
    m_fRevWet = 3.0f * DBToGain(fMixDB);
    // Feedback from the decay time: -60 dB after fTimeMs over the mean comb delay.
    const double fMean = 1378.0 * m_nRate / 44100.0;
    const double fT = (fTimeMs > 10.0f ? fTimeMs : 1000.0f) / 1000.0;
    m_fRevFeedback = (float)pow(10.0, -3.0 * fMean / (fT * m_nRate));
    if (m_fRevFeedback > 0.98f) m_fRevFeedback = 0.98f;
    // The high-frequency ratio: < 1 = treble dies faster -> more damping.
    float r = fHFRatio < 0.001f ? 0.001f : (fHFRatio > 1.0f ? 1.0f : fHFRatio);
    m_fRevDamp = 0.15f + 0.55f * (1.0f - r);
}

void CLTDMSynth::SetChannel(uint32_t nPChannel, const LTDLSInstrument* pInst,
                            const CLTDLSCollection* pColl, int nVolume, int nPan,
                            uint32_t nPriority)
{
    Channel& c = m_Channels[nPChannel];
    if (pInst) { c.m_pInst = pInst; c.m_pColl = pColl; }
    if (nVolume >= 0) c.m_nVolume = nVolume;
    if (nPan >= 0) c.m_nPan = nPan;
    if (nPriority) c.m_nPriority = nPriority;
}

void CLTDMSynth::ResetChannels()
{
    m_Channels.clear();
}

CLTDMSynth::Voice* CLTDMSynth::Allocate(uint32_t nPriority)
{
    int nActive = 0;
    Voice* pFree = 0;
    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        if (m_Voices[i].m_bActive) ++nActive;
        else if (!pFree) pFree = &m_Voices[i];
    }
    if (pFree && nActive < m_nMaxVoices) return pFree;

    // Steal: a released voice first (quietest), else the oldest of the
    // lowest priority.
    Voice* pBest = 0;
    float fBest = 1e9f;
    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        Voice& v = m_Voices[i];
        if (!v.m_bActive || v.m_nStage != 3) continue;
        const float g = v.m_fEnv;
        if (g < fBest) { fBest = g; pBest = &v; }
    }
    if (!pBest)
    {
        uint32_t nLow = 0xFFFFFFFFu; unsigned int nOld = 0xFFFFFFFFu;
        for (size_t i = 0; i < m_Voices.size(); ++i)
        {
            Voice& v = m_Voices[i];
            if (!v.m_bActive) continue;
            const Channel& c = m_Channels[v.m_nPChannel];
            if (c.m_nPriority < nLow || (c.m_nPriority == nLow && v.m_nAge < nOld))
            { nLow = c.m_nPriority; nOld = v.m_nAge; pBest = &v; }
        }
        if (pBest && nLow > nPriority && nPriority) return 0;    // all more important
    }
    return pBest;
}

void CLTDMSynth::NoteOn(uint32_t nPChannel, int nNote, int nVelocity, unsigned int nTag)
{
    if (nNote < 0 || nNote > 127) return;
    if (nVelocity <= 0) { NoteOff(nPChannel, nNote, nTag); return; }
    Channel& c = m_Channels[nPChannel];
    if (!c.m_pInst || !c.m_pColl) return;
    const LTDLSRegion* r = c.m_pInst->FindRegion(nNote, nVelocity);
    if (!r || r->m_nWave >= c.m_pColl->m_Waves.size()) return;
    const LTDLSWave& w = c.m_pColl->m_Waves[r->m_nWave];
    if (w.m_Samples.size() < 2) return;

    // An exclusive class (hi-hat open/closed): cut the others in the group.
    if (r->m_nKeyGroup)
        for (size_t i = 0; i < m_Voices.size(); ++i)
        {
            Voice& o = m_Voices[i];
            if (o.m_bActive && o.m_nPChannel == nPChannel && o.m_nKeyGroup == r->m_nKeyGroup)
            {
                StartRelease(o);
                o.m_fReleaseRate = 96.0f / (0.005f * m_nRate);
            }
        }

    Voice* v = Allocate(c.m_nPriority);
    if (!v) return;
    const LTDLSArt& a = r->m_bHasArt ? r->m_Art : c.m_pInst->m_Art;

    // The region's wsmp overrides the wave's.
    const bool bR = r->m_bHasWsmp;
    const int   nUnity = bR ? r->m_nUnity : w.m_nUnity;
    const int   nFine  = bR ? r->m_nFineTune : w.m_nFineTune;
    const float fGain  = bR ? r->m_fGain : w.m_fGain;
    const bool  bLoop  = bR ? r->m_bLoop : w.m_bLoop;
    const unsigned int nLS = bR ? r->m_nLoopStart : w.m_nLoopStart;
    const unsigned int nLL = bR ? r->m_nLoopLength : w.m_nLoopLength;

    v->m_bActive = true;
    v->m_nPChannel = nPChannel;
    v->m_nTag = nTag;
    v->m_nNote = nNote;
    v->m_nVelocity = nVelocity;
    v->m_nKeyGroup = r->m_nKeyGroup;
    v->m_pWave = &w;
    v->m_pArt = &a;
    v->m_fPos = 0.0;
    // Unity pitch as a frame step; pitch bend, LFO and EG2 are applied per block.
    v->m_fStep = pow(2.0, ((nNote - nUnity) * 100.0 + nFine) / 1200.0) * (double)w.m_nRate / m_nRate;
    v->m_bLoop = bLoop && nLS + nLL <= w.m_Samples.size() && nLL > 1;
    v->m_fLoopStart = nLS;
    v->m_fLoopEnd = (double)nLS + nLL;

    // The concave velocity curve: -40 log10(vel/127) dB, capped at the art's range.
    float fVelCB = -400.0f * (float)log10(nVelocity / 127.0);
    if (fVelCB > a.m_fVelToAtten) fVelCB = a.m_fVelToAtten;
    v->m_fBaseGain = fGain * (float)pow(10.0, -fVelCB / 200.0);
    v->m_fPanArt = a.m_fPan;

    // EG1.
    const double fAttack = a.m_fAttack * pow(2.0, a.m_fVelToAttack * (nVelocity / 128.0) / 1200.0);
    const double fDecay  = a.m_fDecay  * pow(2.0, a.m_fKeyToDecay  * (nNote / 128.0) / 1200.0);
    v->m_nStage = 0;
    v->m_fEnv = 0.0f;
    v->m_fAttackRate = fAttack > 0.0005 ? (float)(1.0 / (fAttack * m_nRate)) : 1.0f;
    v->m_fSustainDB = a.m_fSustain >= 1.0f ? 0.0f : kFloorDB * (1.0f - (a.m_fSustain < 0.0f ? 0.0f : a.m_fSustain));
    v->m_fDecayRate = fDecay > 0.0005 ? (float)(96.0 / (fDecay * m_nRate)) : 96.0f;
    const float fRel = a.m_fRelease > kMinRelease ? a.m_fRelease : kMinRelease;
    v->m_fReleaseRate = 96.0f / (fRel * m_nRate);
    // EG2 -- linear, 0..1.
    const double fDecay2 = a.m_fDecay2 * pow(2.0, a.m_fKeyToDecay2 * (nNote / 128.0) / 1200.0);
    v->m_nStage2 = 0;
    v->m_fEnv2 = 0.0f;
    v->m_fAttack2Rate  = a.m_fAttack2 > 0.0005f ? 1.0f / (a.m_fAttack2 * m_nRate) : 1.0f;
    v->m_fDecay2Rate   = fDecay2 > 0.0005 ? (float)(1.0 / (fDecay2 * m_nRate)) : 1.0f;
    v->m_fRelease2Rate = a.m_fRelease2 > 0.0005f ? 1.0f / (a.m_fRelease2 * m_nRate) : 1.0f;
    v->m_fSustain2     = a.m_fSustain2;
    // LFO.
    v->m_fLFOPhase = 0.0;
    v->m_fLFODelayFrames = a.m_fLFODelay * m_nRate;
    v->m_bKeyDown = true;
    v->m_bHeld = false;
    v->m_nAge = ++m_nAgeCounter;
}

void CLTDMSynth::StartRelease(Voice& v)
{
    if (v.m_nStage == 0)
    {
        // Leaving the attack: from linear amplitude into the dB domain.
        v.m_fEnv = v.m_fEnv > 0.00002f ? 20.0f * (float)log10(v.m_fEnv) : kFloorDB;
    }
    v.m_nStage = 3;
    v.m_nStage2 = 3;
    v.m_bKeyDown = false;
}

void CLTDMSynth::NoteOff(uint32_t nPChannel, int nNote, unsigned int nTag)
{
    const Channel& c = m_Channels[nPChannel];
    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        Voice& v = m_Voices[i];
        if (!v.m_bActive || !v.m_bKeyDown || v.m_nPChannel != nPChannel
            || v.m_nNote != nNote || v.m_nTag != nTag) continue;
        if (c.m_bSustain) { v.m_bHeld = true; v.m_bKeyDown = false; continue; }
        StartRelease(v);
        break;          // one NoteOff ends one note
    }
}

void CLTDMSynth::Controller(uint32_t nPChannel, int nCC, int nValue)
{
    Channel& c = m_Channels[nPChannel];
    nValue = nValue < 0 ? 0 : (nValue > 127 ? 127 : nValue);
    switch (nCC)
    {
        case 1:  c.m_nMod = nValue; break;
        case 7:  c.m_nVolume = nValue; break;
        case 10: c.m_nPan = nValue; break;
        case 11: c.m_nExpression = nValue; break;
        case 64:
            c.m_bSustain = nValue >= 64;
            if (!c.m_bSustain)
                for (size_t i = 0; i < m_Voices.size(); ++i)
                {
                    Voice& v = m_Voices[i];
                    if (v.m_bActive && v.m_bHeld && v.m_nPChannel == nPChannel)
                    { v.m_bHeld = false; StartRelease(v); }
                }
            break;
        case 121:   // reset all controllers
            c.m_nMod = 0; c.m_nExpression = 127; c.m_nBend = 0; c.m_bSustain = false;
            break;
        case 123:   // all notes off
            for (size_t i = 0; i < m_Voices.size(); ++i)
                if (m_Voices[i].m_bActive && m_Voices[i].m_nPChannel == nPChannel) StartRelease(m_Voices[i]);
            break;
        default: break;
    }
}

void CLTDMSynth::PitchBend(uint32_t nPChannel, int nValue14)
{
    m_Channels[nPChannel].m_nBend = nValue14 - 8192;
}

void CLTDMSynth::AllNotesOff(bool bHard)
{
    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        Voice& v = m_Voices[i];
        if (!v.m_bActive) continue;
        if (bHard) v.m_bActive = false;
        else StartRelease(v);
    }
}

void CLTDMSynth::ReleaseTag(unsigned int nTag)
{
    for (size_t i = 0; i < m_Voices.size(); ++i)
    {
        Voice& v = m_Voices[i];
        if (v.m_bActive && v.m_nTag == nTag && v.m_nStage != 3) { v.m_bHeld = false; StartRelease(v); }
    }
}

unsigned int CLTDMSynth::ActiveVoices() const
{
    unsigned int n = 0;
    for (size_t i = 0; i < m_Voices.size(); ++i) if (m_Voices[i].m_bActive) ++n;
    return n;
}

void CLTDMSynth::Render(float* pLR, unsigned int nFrames)
{
    if (!nFrames) return;
    if (m_Scratch.size() < (size_t)nFrames * 2) m_Scratch.resize((size_t)nFrames * 2);
    float* pDry = &m_Scratch[0];
    memset(pDry, 0, sizeof(float) * nFrames * 2);

    const unsigned int kBlock = 16;
    for (size_t vi = 0; vi < m_Voices.size(); ++vi)
    {
        Voice& v = m_Voices[vi];
        if (!v.m_bActive) continue;
        const Channel& c = m_Channels[v.m_nPChannel];
        const LTDLSArt& a = *v.m_pArt;
        const int16_t* s = &v.m_pWave->m_Samples[0];
        const double nLast = (double)v.m_pWave->m_Samples.size() - 1.0;

        // Per-voice static gain and pan.
        const float fVol = (c.m_nVolume / 127.0f) * (c.m_nVolume / 127.0f)
                         * (c.m_nExpression / 127.0f) * (c.m_nExpression / 127.0f);
        float fPan = c.m_nPan / 127.0f + v.m_fPanArt;
        fPan = fPan < 0.0f ? 0.0f : (fPan > 1.0f ? 1.0f : fPan);
        const float fL = (float)cos(fPan * kPi * 0.5f), fR = (float)sin(fPan * kPi * 0.5f);
        const float fStatic = v.m_fBaseGain * fVol * m_fGain;

        for (unsigned int f0 = 0; f0 < nFrames && v.m_bActive; f0 += kBlock)
        {
            const unsigned int nB = (nFrames - f0 < kBlock) ? nFrames - f0 : kBlock;

            // ---- per block: LFO, EG2, pitch -------------------------- //
            float fLFO = 0.0f;
            if (v.m_fLFODelayFrames > 0.0f) v.m_fLFODelayFrames -= nB;
            else
            {
                fLFO = (float)sin(2.0 * kPi * v.m_fLFOPhase);
                v.m_fLFOPhase += a.m_fLFOFreq * nB / (double)m_nRate;
                if (v.m_fLFOPhase > 1.0) v.m_fLFOPhase -= floor(v.m_fLFOPhase);
            }
            // EG2, a block at a time (pitch only, so coarse is fine).
            switch (v.m_nStage2)
            {
                case 0: v.m_fEnv2 += v.m_fAttack2Rate * nB; if (v.m_fEnv2 >= 1.0f) { v.m_fEnv2 = 1.0f; v.m_nStage2 = 1; } break;
                case 1: v.m_fEnv2 -= v.m_fDecay2Rate * nB; if (v.m_fEnv2 <= v.m_fSustain2) { v.m_fEnv2 = v.m_fSustain2; v.m_nStage2 = 2; } break;
                case 3: v.m_fEnv2 -= v.m_fRelease2Rate * nB; if (v.m_fEnv2 < 0.0f) v.m_fEnv2 = 0.0f; break;
                default: break;
            }
            const float fMod = c.m_nMod / 128.0f;
            const float fCents = c.m_nBend * c.m_fBendRange * 100.0f / 8192.0f
                               + v.m_fEnv2 * a.m_fEG2ToPitch
                               + fLFO * (a.m_fLFOToPitch + fMod * a.m_fModToPitch);
            const double fStep = v.m_fStep * (fCents != 0.0f ? pow(2.0, fCents / 1200.0) : 1.0);
            const float fTremCB = fLFO * (a.m_fLFOToAtten + fMod * a.m_fModToAtten);
            const float fTrem = fTremCB != 0.0f ? (float)pow(10.0, -fTremCB / 200.0) : 1.0f;
            const float fG = fStatic * fTrem;

            for (unsigned int k = 0; k < nB; ++k)
            {
                // ---- EG1, per frame ---------------------------------- //
                float fAmp;
                switch (v.m_nStage)
                {
                    case 0:
                        v.m_fEnv += v.m_fAttackRate;
                        if (v.m_fEnv >= 1.0f) { v.m_fEnv = 0.0f; v.m_nStage = 1; fAmp = 1.0f; }
                        else fAmp = v.m_fEnv;
                        break;
                    case 1:
                        v.m_fEnv -= v.m_fDecayRate;
                        if (v.m_fEnv <= v.m_fSustainDB) { v.m_fEnv = v.m_fSustainDB; v.m_nStage = 2; }
                        fAmp = DBToGain(v.m_fEnv);
                        break;
                    case 2:
                        fAmp = DBToGain(v.m_fEnv);
                        break;
                    default:
                        v.m_fEnv -= v.m_fReleaseRate;
                        if (v.m_fEnv <= kFloorDB) { v.m_bActive = false; fAmp = 0.0f; }
                        else fAmp = DBToGain(v.m_fEnv);
                        break;
                }
                if (!v.m_bActive) break;

                // ---- the sample -------------------------------------- //
                const size_t i0 = (size_t)v.m_fPos;
                const float fr = (float)(v.m_fPos - (double)i0);
                size_t i1 = i0 + 1;
                if (v.m_bLoop && (double)i1 >= v.m_fLoopEnd) i1 = (size_t)v.m_fLoopStart;
                const float x = (s[i0] + (s[i1 < v.m_pWave->m_Samples.size() ? i1 : i0] - s[i0]) * fr) * (1.0f / 32768.0f);
                const float y = x * fAmp * fG;
                pDry[(f0 + k) * 2]     += y * fL;
                pDry[(f0 + k) * 2 + 1] += y * fR;

                v.m_fPos += fStep;
                if (v.m_bLoop)
                {
                    while (v.m_fPos >= v.m_fLoopEnd) v.m_fPos -= (v.m_fLoopEnd - v.m_fLoopStart);
                }
                else if (v.m_fPos >= nLast) { v.m_bActive = false; break; }
            }
        }
    }

    // ---- reverb, then out -------------------------------------------- //
    for (unsigned int f = 0; f < nFrames; ++f)
    {
        float l = pDry[f * 2], r = pDry[f * 2 + 1];
        if (m_bReverb)
        {
            const float in = (l + r) * m_fRevIn;
            float out[2];
            for (int ch = 0; ch < 2; ++ch)
            {
                float acc = 0.0f;
                for (int i = 0; i < 8; ++i)
                {
                    Comb& cb = m_Comb[ch][i];
                    const float y = cb.m_Buf[cb.m_nAt];
                    cb.m_fStore = y * (1.0f - m_fRevDamp) + cb.m_fStore * m_fRevDamp;
                    cb.m_Buf[cb.m_nAt] = in + cb.m_fStore * m_fRevFeedback;
                    if (++cb.m_nAt >= cb.m_Buf.size()) cb.m_nAt = 0;
                    acc += y;
                }
                for (int i = 0; i < 4; ++i)
                {
                    Allpass& ap = m_All[ch][i];
                    const float b = ap.m_Buf[ap.m_nAt];
                    const float o = -acc + b;
                    ap.m_Buf[ap.m_nAt] = acc + b * 0.5f;
                    if (++ap.m_nAt >= ap.m_Buf.size()) ap.m_nAt = 0;
                    acc = o;
                }
                out[ch] = acc;
            }
            l += out[0] * m_fRevWet;
            r += out[1] * m_fRevWet;
        }
        pLR[f * 2]     += l;
        pLR[f * 2 + 1] += r;
    }
}
