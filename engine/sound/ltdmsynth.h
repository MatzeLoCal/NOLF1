// ------------------------------------------------------------------------- //
//
// ltdmsynth.h -- the DLS synthesizer under the music (HANDOVER 5cj).
//
// What DirectMusic's software synth did for NOLF: PChannels hold a DLS
// instrument with a volume, pan and transposition (from a band); notes pick
// a region by key, play its wave resampled to pitch, shaped by the region's
// articulation (EG1 volume envelope linear in dB, EG2 to pitch, LFO vibrato
// and tremolo, velocity curve); a reverb sits on the output.
//
// ⚠️ EVERY CALL IS MADE WITH THE PERFORMANCE'S LOCK HELD -- the game thread
// and the audio thread both reach the synth through CLTDMPerformance.
//
// Notes carry a TAG (the segment state that started them) so a NoteOff from
// a segment that was cut off cannot release a note a newer one started.
//
// ------------------------------------------------------------------------- //

#ifndef __LTDMSYNTH_H__
#define __LTDMSYNTH_H__

#include "ltdls.h"

#include <map>
#include <vector>

class CLTDMSynth
{
public:
    explicit CLTDMSynth(unsigned int nRate = 44100);

    // A band instrument. pInst null = keep the instrument, update the rest.
    // nVolume / nPan < 0 = leave as is.
    void    SetChannel(uint32_t nPChannel, const LTDLSInstrument* pInst,
                       const CLTDLSCollection* pColl, int nVolume, int nPan,
                       uint32_t nPriority);
    void    NoteOn(uint32_t nPChannel, int nNote, int nVelocity, unsigned int nTag);
    void    NoteOff(uint32_t nPChannel, int nNote, unsigned int nTag);
    void    Controller(uint32_t nPChannel, int nCC, int nValue);
    void    PitchBend(uint32_t nPChannel, int nValue14);        // 0..16383, 8192 centre
    void    AllNotesOff(bool bHard);    // hard = silence now, else release
    void    ReleaseTag(unsigned int nTag);  // release every note a segment state started
    void    ResetChannels();            // forget instruments and controllers

    void    SetMaxVoices(int n)          { m_nMaxVoices = n < 8 ? 8 : (n > 256 ? 256 : n); }
    void    SetGain(float f)             { m_fGain = f; }
    void    SetReverb(bool bOn, float fInGainDB, float fMixDB, float fTimeMs, float fHFRatio);

    // Adds nFrames of stereo float to pLR (interleaved), at the synth's rate.
    void    Render(float* pLR, unsigned int nFrames);

    unsigned int ActiveVoices() const;
    unsigned int Rate() const { return m_nRate; }

private:
    struct Channel
    {
        const LTDLSInstrument*  m_pInst;
        const CLTDLSCollection* m_pColl;
        int     m_nVolume, m_nExpression, m_nPan, m_nMod;
        int     m_nBend;                // -8192..8191
        float   m_fBendRange;           // semitones
        bool    m_bSustain;
        uint32_t m_nPriority;
        Channel() : m_pInst(0), m_pColl(0), m_nVolume(100), m_nExpression(127),
                    m_nPan(64), m_nMod(0), m_nBend(0), m_fBendRange(2.0f),
                    m_bSustain(false), m_nPriority(0) {}
    };

    struct Voice
    {
        bool                m_bActive;
        uint32_t            m_nPChannel;
        unsigned int        m_nTag;
        int                 m_nNote, m_nVelocity;
        uint16_t            m_nKeyGroup;
        const LTDLSWave*    m_pWave;
        const LTDLSArt*     m_pArt;
        double              m_fPos, m_fStep;    // frames, frames per output frame at unity pitch
        bool                m_bLoop;
        double              m_fLoopStart, m_fLoopEnd;
        float               m_fBaseGain;        // region/wave gain x velocity
        float               m_fPanArt;
        // EG1 (amplitude): stage 0 attack, 1 decay, 2 sustain, 3 release, 4 done
        int                 m_nStage;
        float               m_fEnv;             // attack: linear 0..1; later: dB (<= 0)
        float               m_fAttackRate;      // per frame
        float               m_fDecayRate;       // dB per frame
        float               m_fSustainDB;
        float               m_fReleaseRate;     // dB per frame
        // EG2 (pitch): linear 0..1
        int                 m_nStage2;
        float               m_fEnv2, m_fAttack2Rate, m_fDecay2Rate, m_fRelease2Rate, m_fSustain2;
        // LFO
        double              m_fLFOPhase;
        float               m_fLFODelayFrames;
        bool                m_bKeyDown, m_bHeld;
        unsigned int        m_nAge;
    };

    void        StartRelease(Voice& v);
    float       VoiceGain(const Voice& v) const;
    Voice*      Allocate(uint32_t nPriority);

    unsigned int                    m_nRate;
    std::map<uint32_t, Channel>     m_Channels;
    std::vector<Voice>              m_Voices;
    int                             m_nMaxVoices;
    unsigned int                    m_nAgeCounter;
    float                           m_fGain;

    // ---- reverb (Freeverb's comb/allpass network) ---------------------- //
    bool                            m_bReverb;
    float                           m_fRevIn, m_fRevWet, m_fRevFeedback, m_fRevDamp;
    struct Comb    { std::vector<float> m_Buf; size_t m_nAt; float m_fStore; };
    struct Allpass { std::vector<float> m_Buf; size_t m_nAt; };
    Comb                            m_Comb[2][8];
    Allpass                         m_All[2][4];
    std::vector<float>              m_Scratch;
};

#endif // __LTDMSYNTH_H__
