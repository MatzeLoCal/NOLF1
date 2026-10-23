// ------------------------------------------------------------------------- //
//
// ltdls.h -- a DLS (Downloadable Sounds, level 1) collection: instruments,
// regions, articulation and the wave pool (HANDOVER 5cj).
//
// NOLF's music ships 113 DLS banks (39.7 MB): every one level 1 (`art1`),
// every wave 16-bit mono PCM, mostly 22,050 Hz. The articulation in use is a
// small set -- EG1 attack/decay/sustain/release, velocity -> attack and ->
// attenuation, key -> decay, LFO frequency/delay and LFO (x mod wheel) ->
// pitch/attenuation, EG2 -> pitch -- and that is what is read. Apple's GM
// bank (CoreAudio's gs_instruments.dls) loads through the same code as the
// default set for band instruments that name no collection.
//
// Units, from the DLS 1 specification:
//   time cents   seconds = 2^(tc / (1200 * 65536))      (0x80000000 = 0 s)
//   absolute pitch (LFO)  Hz = 440 * 2^((pc / 65536 - 6900) / 1200)
//   attenuation  centibels = scale / 65536
//   sustain / pan  0.1 % = scale / 65536
//
// ------------------------------------------------------------------------- //

#ifndef __LTDLS_H__
#define __LTDLS_H__

#include <stdint.h>
#include <string>
#include <vector>

struct LTDLSArt
{
    // EG1 -- the volume envelope. Seconds; sustain 0..1.
    float   m_fAttack, m_fDecay, m_fSustain, m_fRelease;
    float   m_fVelToAttack;     // time cents scale, applied as 2^(x * vel/128 / 1200)
    float   m_fKeyToDecay;      // likewise, by key/128
    // EG2 -- here only ever routed to pitch.
    float   m_fAttack2, m_fDecay2, m_fSustain2, m_fRelease2;
    float   m_fKeyToDecay2;
    float   m_fEG2ToPitch;      // cents at full EG2
    // LFO.
    float   m_fLFOFreq, m_fLFODelay;
    float   m_fLFOToPitch, m_fLFOToAtten;       // cents, centibels
    float   m_fModToPitch, m_fModToAtten;       // the same, scaled by CC1 / 128
    // Static.
    float   m_fPan;             // -0.5 .. 0.5
    float   m_fVelToAtten;      // centibels at velocity 0 (concave), default 960
    bool    m_bVelToAttenSet;

    LTDLSArt();
    void    Apply(const unsigned char* pArt1, size_t n);   // one art1 chunk body
};

struct LTDLSWave
{
    std::vector<int16_t>    m_Samples;
    unsigned int            m_nRate;
    // The wave's own wsmp (a region's overrides it).
    bool                    m_bHasWsmp;
    int                     m_nUnity;
    int                     m_nFineTune;        // cents
    float                   m_fGain;            // linear
    bool                    m_bLoop;
    unsigned int            m_nLoopStart, m_nLoopLength;
    LTDLSWave() : m_nRate(22050), m_bHasWsmp(false), m_nUnity(60), m_nFineTune(0),
                  m_fGain(1.0f), m_bLoop(false), m_nLoopStart(0), m_nLoopLength(0) {}
};

struct LTDLSRegion
{
    uint8_t     m_nKeyLo, m_nKeyHi, m_nVelLo, m_nVelHi;
    uint16_t    m_nKeyGroup;
    unsigned int m_nWave;           // index into the collection's pool
    bool        m_bHasWsmp;         // region overrides the wave's wsmp
    int         m_nUnity, m_nFineTune;
    float       m_fGain;
    bool        m_bLoop;
    unsigned int m_nLoopStart, m_nLoopLength;
    bool        m_bHasArt;
    LTDLSArt    m_Art;
};

struct LTDLSInstrument
{
    std::string m_sName;
    uint32_t    m_nBank;            // DLS ulBank: MSB bits 8-14, LSB 0-6, bit 31 drums
    uint32_t    m_nProgram;
    LTDLSArt    m_Art;              // instrument-level articulation
    bool        m_bHasArt;
    std::vector<LTDLSRegion> m_Regions;

    // The region playing `nKey` at `nVel`, or 0.
    const LTDLSRegion* FindRegion(int nKey, int nVel) const;
};

class CLTDLSCollection
{
public:
    std::string                     m_sName;
    std::vector<LTDLSInstrument>    m_Instruments;
    std::vector<LTDLSWave>          m_Waves;

    bool    Load(const unsigned char* p, size_t n);

    // A band's patch: (MSB << 16) | (LSB << 8) | program, bit 31 = drums.
    // With bExact false, a drum kit or program matches on program alone when
    // the banks disagree (the GM fallback).
    const LTDLSInstrument* Find(uint32_t nPatch, bool bExact = true) const;
};

#endif // __LTDLS_H__
