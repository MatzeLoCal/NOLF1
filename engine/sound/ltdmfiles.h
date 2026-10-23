// ------------------------------------------------------------------------- //
//
// ltdmfiles.h -- DirectMusic file formats: segments (.sgt), styles (.sty),
// bands and chords, read into plain structures (HANDOVER 5cj).
//
// NOLF 1's music is DirectMusic (DX7) STYLE music: a segment carries a
// command track (groove levels), a tempo track, a style reference, a band
// and sometimes a chord, time-signature or mute track -- the notes live in
// the style's patterns. Layouts were confirmed against the shipped files
// (struct sizes are read from the chunks wherever the format stores them):
//
//   segh 24      DWORD repeats; MUSIC_TIME length, playStart, loopStart,
//                loopEnd; DWORD resolution
//   cmnd         DWORD size(12) + { MUSIC_TIME t; WORD measure; BYTE beat,
//                command, grooveLevel, grooveRange, repeatMode }
//   tetr         DWORD size(16) + { MUSIC_TIME t; (pad) double bpm }
//   tims         DWORD size(8)  + { MUSIC_TIME t; BYTE beatsPerMeasure,
//                beat; WORD gridsPerBeat }
//   mute         DWORD size(12) + { MUSIC_TIME t; DWORD pchannel, map }
//   bins 40      DWORD patch, assignPatch, noteRanges[4], pchannel, flags;
//                BYTE pan, volume; short transpose; DWORD priority
//   styh 12      timesig(4) + double tempo (packed)
//   prth 154     timesig(4); DWORD variationChoices[32]; GUID part;
//                WORD measures; BYTE playMode, invertUpper, invertLower
//   note         DWORD size(22) + { MUSIC_TIME gridStart; DWORD variation;
//                MUSIC_TIME duration; short timeOffset; WORD musicValue;
//                BYTE velocity, timeRange, durRange, velRange, inversion,
//                playMode }
//   ptnh 10      timesig(4); BYTE grooveBottom, grooveTop; WORD
//                embellishment, measures
//   prfc 22      GUID part; WORD logicalPart (= the PChannel in DX7);
//                BYTE lockId, subChordLevel, priority, randomVariation
//
// MUSIC_TIME is DirectMusic's tick: 768 per quarter note (DMUS_PPQ).
//
// ------------------------------------------------------------------------- //

#ifndef __LTDMFILES_H__
#define __LTDMFILES_H__

#include <string>
#include <vector>
#include <stdint.h>

enum { kLTDMPPQ = 768 };

struct LTDMTimeSig
{
    uint8_t     m_nBeatsPerMeasure;     // 7 in 7/4
    uint8_t     m_nBeat;                // 4 = quarter note
    uint16_t    m_nGridsPerBeat;
    LTDMTimeSig() : m_nBeatsPerMeasure(4), m_nBeat(4), m_nGridsPerBeat(4) {}
    int         BeatTicks() const    { return kLTDMPPQ * 4 / (m_nBeat ? m_nBeat : 4); }
    int         MeasureTicks() const { return BeatTicks() * (m_nBeatsPerMeasure ? m_nBeatsPerMeasure : 4); }
    int         GridTicks() const    { return BeatTicks() / (m_nGridsPerBeat ? m_nGridsPerBeat : 1); }
    // DirectMusic's grid-to-time: whole measures, then beats, then grids.
    int         GridToTicks(int nGrid) const;
};

// A DMRF reference: the name and/or file of another DirectMusic object.
struct LTDMRef
{
    std::string m_sName;
    std::string m_sFile;
};

struct LTDMBandInstrument
{
    uint32_t    m_nPatch;               // (MSB << 16) | (LSB << 8) | program; bit 31 drums
    uint32_t    m_nPChannel;
    uint32_t    m_nFlags;               // DMUS_IO_INST_*
    uint8_t     m_nPan;                 // 0..127
    uint8_t     m_nVolume;              // 0..127
    int16_t     m_nTranspose;
    uint32_t    m_nPriority;
    std::string m_sDLSFile;             // "" = the default GM set
};

struct LTDMBand
{
    std::string m_sName;
    int         m_nTime;                // in a band track
    std::vector<LTDMBandInstrument> m_Instruments;
};

struct LTDMSubChord
{
    uint32_t    m_nChordPattern;
    uint32_t    m_nScalePattern;
    uint32_t    m_nInversionPoints;
    uint32_t    m_nLevels;
    uint8_t     m_nChordRoot;
    uint8_t     m_nScaleRoot;
};

struct LTDMChord
{
    int         m_nTime;
    uint32_t    m_nGroup;               // the track's group bits (trkh)
    std::string m_sName;
    std::vector<LTDMSubChord> m_Sub;
};

struct LTDMStyleNote
{
    int         m_nGridStart;
    uint32_t    m_nVariation;           // bit v = plays in variation v
    int         m_nDuration;
    int16_t     m_nTimeOffset;
    uint16_t    m_nMusicValue;
    uint8_t     m_nVelocity, m_nTimeRange, m_nDurRange, m_nVelRange;
    uint8_t     m_nInversion, m_nPlayMode;
};

struct LTDMStyleCurve
{
    int         m_nGridStart;
    uint32_t    m_nVariation;
    int         m_nDuration;
    int         m_nResetDuration;
    int16_t     m_nTimeOffset;
    int16_t     m_nStart, m_nEnd, m_nReset;
    uint8_t     m_nEventType;           // 3 pitch bend, 4 CC, 5 channel pressure
    uint8_t     m_nShape;               // 0 linear, 1 instant, 2 exp, 3 log, 4 sine
    uint8_t     m_nCC;
    uint8_t     m_nFlags;               // 1 = reset
};

struct LTDMStylePart
{
    LTDMTimeSig m_TimeSig;
    uint32_t    m_VariationChoices[32];
    uint8_t     m_Guid[16];
    uint16_t    m_nMeasures;
    uint8_t     m_nPlayMode;
    std::vector<LTDMStyleNote>  m_Notes;
    std::vector<LTDMStyleCurve> m_Curves;
};

struct LTDMPartRef
{
    uint8_t     m_Guid[16];
    int         m_nPart;                // index into LTDMStyle::m_Parts, -1 = none
    uint32_t    m_nPChannel;            // the logical part id
    uint8_t     m_nLockId, m_nSubChordLevel, m_nPriority, m_nRandomVariation;
};

struct LTDMPattern
{
    std::string m_sName;
    LTDMTimeSig m_TimeSig;
    uint8_t     m_nGrooveBottom, m_nGrooveTop;
    uint16_t    m_nEmbellishment;       // 0 normal, 1 fill, 2 break, 4 intro, 8 end, 16 motif
    uint16_t    m_nMeasures;
    std::vector<LTDMPartRef> m_PartRefs;
};

struct LTDMStyle
{
    std::string m_sName;
    LTDMTimeSig m_TimeSig;
    double      m_fTempo;
    std::vector<LTDMStylePart> m_Parts;
    std::vector<LTDMPattern>   m_Patterns;      // motifs included (m_nEmbellishment & 16)
    std::vector<LTDMBand>      m_Bands;

    bool        Load(const unsigned char* p, size_t n);
};

struct LTDMCommand
{
    int         m_nTime;
    uint16_t    m_nMeasure;
    uint8_t     m_nBeat, m_nCommand, m_nGrooveLevel, m_nGrooveRange, m_nRepeatMode;
    uint32_t    m_nGroup;
};

struct LTDMTempo    { int m_nTime; double m_fBPM; };
struct LTDMTimeSigItem { int m_nTime; LTDMTimeSig m_TimeSig; uint32_t m_nGroup; };
struct LTDMMute     { int m_nTime; uint32_t m_nPChannel, m_nMap; uint32_t m_nGroup; };
struct LTDMStyleRef { int m_nTime; LTDMRef m_Ref; uint32_t m_nGroup; };

// One note of a sequence track (rare in NOLF: one segment has one).
struct LTDMSeqNote  { int m_nTime; int m_nDuration; uint32_t m_nPChannel; uint8_t m_nStatus, m_nData1, m_nData2; };

struct LTDMSegment
{
    std::string m_sName;
    uint32_t    m_nRepeats;
    int         m_nLength;
    int         m_nPlayStart, m_nLoopStart, m_nLoopEnd;
    uint32_t    m_nResolution;          // DMUS_SEGF_* default boundary
    std::vector<LTDMCommand>     m_Commands;
    std::vector<LTDMTempo>       m_Tempos;
    std::vector<LTDMTimeSigItem> m_TimeSigs;
    std::vector<LTDMMute>        m_Mutes;
    std::vector<LTDMStyleRef>    m_Styles;
    std::vector<LTDMBand>        m_Bands;
    std::vector<LTDMChord>       m_Chords;
    uint32_t    m_nChordKey;            // crdh: root << 24 | scale; 0 = none
    std::vector<LTDMSeqNote>     m_Seq;

    bool        Load(const unsigned char* p, size_t n);
};

#endif // __LTDMFILES_H__
