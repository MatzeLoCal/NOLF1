// ------------------------------------------------------------------------- //
//
// ltdmperf.h -- the music performance: DirectMusic's job under LithTech's
// music manager (HANDOVER 5cj).
//
// It owns the clock (music time in ticks, advanced by Render at the current
// tempo), the SEGMENT STATES -- primaries, secondaries, motifs -- and the
// synth. A segment is turned into note, controller, tempo and band EVENTS
// when it is played, in full (NOLF's segments are a few measures; the two
// that repeat ten times come to a few thousand events), and the events are
// dispatched as the clock reaches them. A new primary CUTS the one before it
// at its start tick; notes already sounding ring out (LithTech plays with
// DMUS_SEGF_NOINVALIDATE).
//
// The STYLE ENGINE is DirectMusic's style track as far as NOLF's data uses
// it (census in HANDOVER 5cj: 50,468 notes, play modes FIXED and ALWAYSPLAY
// only, no inversions, embellishments normal and motif only):
//   * per measure, the groove level from the command track picks a pattern;
//   * per pattern instance, each part reference picks a variation -- from
//     the ones whose chord-function flags fit the chord (0x20000000 alone is
//     a disabled variation), random or sequential, shared by lock id;
//   * notes go from music values to MIDI through the chord in force (the
//     segment's chord track, else the default chord) and the part's play
//     mode; the mute track silences or remaps PChannels; curves become
//     controllers.
//
// ⚠️ ONE LOCK guards everything, and the music manager uses the SAME lock:
// the game thread (manager -> performance) and the music thread (Render ->
// notification -> manager) would otherwise take two locks in opposite orders.
// It is recursive because notifications run inside Render and play segments.
//
// ------------------------------------------------------------------------- //

#ifndef __LTDMPERF_H__
#define __LTDMPERF_H__

#include "ltdmfiles.h"
#include "ltdls.h"
#include "ltdmsynth.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

class CLTDMPerformance
{
public:
    // Reads a music file ("Music\Ambush\" + name) into bytes.
    typedef bool (*ReadFn)(const char* pPath, std::vector<unsigned char>& out, void* pUser);
    // A segment state started / is about to end / ended. Called from Render
    // with the lock held -- DirectMusic's SEGSTART / SEGALMOSTEND / SEGEND.
    enum Notify { kNotifyStart, kNotifyAlmostEnd, kNotifyEnd };
    typedef void (*NotifyFn)(void* pUser, Notify eKind, unsigned int nStateId);

    // DMUS_SEGF_* boundaries. kDefault = the segment's own (segh resolution).
    enum Enact { kImmediate, kGrid, kBeat, kMeasure, kSegmentEnd, kDefault };

    explicit CLTDMPerformance(unsigned int nRate = 44100);
    ~CLTDMPerformance();

    std::recursive_mutex& Lock() { return m_Lock; }

    void    SetReader(ReadFn fn, void* pUser)   { m_pfnRead = fn; m_pReadUser = pUser; }
    void    SetDirectory(const std::string& s)  { m_sDir = s; }
    void    SetNotify(NotifyFn fn, void* pUser) { m_pfnNotify = fn; m_pNotifyUser = pUser; }
    // The GM set for band instruments that name no collection and match no
    // downloaded one (Apple's gs_instruments.dls). "" = none.
    void    SetGMPath(const std::string& s)     { m_sGMPath = s; }

    // ---- assets (loaded once, by file name, case-insensitive) ---------- //
    // Every load also loads what the object needs to play: a segment its
    // styles and its bands' DLS collections, a style its bands' collections.
    const LTDMSegment*      GetSegment(const char* pFile);
    const LTDMStyle*        GetStyle(const char* pFile);
    const CLTDLSCollection* GetDLS(const char* pFile);          // "" = the GM set
    void    PrepareBand(const LTDMBand& b);
    // Drops every asset and state (TermLevel).
    void    Reset();

    // ---- playback ------------------------------------------------------ //
    // Returns the new state's id (0 = nothing played).
    unsigned int PlayPrimary(const LTDMSegment* pSeg, Enact e);
    unsigned int PlaySecondary(const LTDMSegment* pSeg, Enact e);
    unsigned int PlayMotif(const LTDMStyle* pStyle, const LTDMPattern* pMotif, Enact e);
    // A band, now -- LithTech's LoadBand plays each control-file band once.
    void    PlayBandNow(const LTDMBand& b);
    void    StopState(unsigned int nId, Enact e);
    void    StopAll(Enact e);
    bool    IsPlaying(unsigned int nId) const;
    void    SetPaused(bool b);
    void    SetVolumeDB(float fDB)          { m_fTargetGain = DBToGain(fDB); }
    CLTDMSynth& Synth()                     { return m_Synth; }

    // Adds nFrames of interleaved stereo float to pLR, at the synth's rate.
    void    Render(float* pLR, unsigned int nFrames);

    // ---- DirectMusic's MusicToMIDI, as NOLF's data needs it ------------ //
    static int  MusicToMIDI(uint16_t nValue, uint8_t nPlayMode, const LTDMSubChord& c);
    static const LTDMChord& DefaultChord();
    // Notes whose chord position was past the top of the chord (diagnostic).
    static unsigned int s_nOverChord;

    // ---- diagnostics --------------------------------------------------- //
    double  Tick() const                { return m_fTick; }
    double  Tempo() const               { return m_fTempo; }
    size_t  PendingEvents() const       { return m_Events.size(); }
    struct Stats
    {
        unsigned int m_nNotes;          // NoteOns dispatched
        unsigned int m_nNoInstrument;   // ... on a PChannel with no instrument
        unsigned int m_nNoRegion;       // ... outside every region of its instrument
        unsigned int m_nMuted;          // dropped by the mute track
        unsigned int m_nBandMisses;     // band instruments that resolved to nothing
        unsigned int m_nGMHits;         // ... resolved through the GM set
        unsigned int m_nGMDefaults;     // PChannels no band set: the GM power-on instrument
        unsigned int m_nMinNote, m_nMaxNote;
    };
    const Stats& GetStats() const       { return m_Stats; }
    // Per-PChannel note ranges vs the instrument's region range (dmrender).
    struct ChannelRange { int m_nLo, m_nHi, m_nRegLo, m_nRegHi; unsigned int m_nNotes, m_nMissed; std::string m_sInst; };
    const std::map<uint32_t, ChannelRange>& Ranges() const { return m_Ranges; }

private:
    struct Event
    {
        double          m_fTick;
        int             m_nOrder;       // at one tick: tempo, band, off, cc, on, stop, end
        unsigned int    m_nSeq;
        enum Kind { kTempo, kBand, kNoteOff, kCC, kBend, kNoteOn, kStopTag, kEnd } m_eKind;
        unsigned int    m_nTag;         // the segment state
        uint32_t        m_nPChannel;
        int             m_nA, m_nB;     // note/velocity, cc/value, bend
        double          m_fValue;       // tempo; for a NoteOff, its NoteOn's tick
        const LTDMBand* m_pBand;
    };
    struct Later
    {
        bool operator()(const Event& a, const Event& b) const
        {
            if (a.m_fTick != b.m_fTick) return a.m_fTick > b.m_fTick;
            if (a.m_nOrder != b.m_nOrder) return a.m_nOrder > b.m_nOrder;
            return a.m_nSeq > b.m_nSeq;
        }
    };

    struct State
    {
        unsigned int        m_nId;
        const LTDMSegment*  m_pSeg;     // null for a motif
        bool                m_bPrimary;
        double              m_fStart, m_fEnd;
        LTDMTimeSig         m_TimeSig;  // for boundaries inside it
        bool                m_bStartSent, m_bAlmostSent;
    };

    // One played instance of a pattern: the variation per part reference.
    struct Instance
    {
        const LTDMStyle*    m_pStyle;
        const LTDMPattern*  m_pPattern;
        double              m_fStart;   // segment ticks
        uint32_t            m_nGroup;   // the style track's group bits
        std::vector<int>    m_Var;
    };

    static float DBToGain(float fDB);
    double      Boundary(Enact e, const LTDMSegment* pSeg) const;
    const State* SoundingPrimary() const;
    const State* LatestPrimary() const;
    void        Push(Event e);
    void        CutState(unsigned int nId, double fTick, bool bRelease);
    void        Generate(const State& s);
    void        GenerateStyleSpan(const State& s, int nFrom, int nTo, double fOffset, uint32_t nGroup);
    void        GenerateInstanceMeasure(const State& s, const Instance& in, int nMeasure,
                                        double fMeasureStart, int nSegTime, double fOffset, int nLimit);
    void        ChooseVariations(Instance& in, const LTDMSubChord& chord);
    const LTDMSubChord& ChordAt(const LTDMSegment* pSeg, int nSegTime, uint8_t nLevel, uint32_t nGroup) const;
    const LTDMSubChord& ChordAtTick(double fTick, uint8_t nLevel) const;
    bool        MuteAt(const LTDMSegment* pSeg, int nSegTime, uint32_t& nPChannel, uint32_t nGroup) const;
    void        Dispatch(const Event& e);
    void        ApplyBand(const LTDMBand& b);
    bool        Resolve(const LTDMBandInstrument& bi, const LTDLSInstrument*& pInst,
                        const CLTDLSCollection*& pColl);
    bool        ReadFile(const std::string& sName, std::vector<unsigned char>& out);
    uint32_t    Rand();
    void        Note(const State& s, double fTick, uint32_t nPChannel, int nMidi, int nVel, double fDur);

    std::recursive_mutex                m_Lock;
    CLTDMSynth                          m_Synth;
    unsigned int                        m_nRate;
    ReadFn                              m_pfnRead;
    void*                               m_pReadUser;
    std::string                         m_sDir;
    std::string                         m_sGMPath;
    NotifyFn                            m_pfnNotify;
    void*                               m_pNotifyUser;

    std::map<std::string, LTDMSegment*>      m_Segments;
    std::map<std::string, LTDMStyle*>        m_Styles;
    std::map<std::string, CLTDLSCollection*> m_DLS;
    // Patches downloaded by a band that named its collection: what a band
    // instrument naming none plays (DirectMusic's port holds instruments
    // by patch, whoever downloaded them).
    std::map<uint32_t, std::pair<const LTDLSInstrument*, const CLTDLSCollection*> > m_Downloaded;
    std::map<uint32_t, int>             m_Transpose;    // per PChannel, from bands
    std::map<uint64_t, int>             m_Sounding;     // (tag, pch, raw note) -> played note
    std::map<uint32_t, const LTDLSInstrument*> m_ChannelInst;   // diagnostics

    std::vector<Event>                  m_Events;       // a heap, earliest on top
    unsigned int                        m_nSeq;
    std::vector<State>                  m_States;
    unsigned int                        m_nNextId;
    double                              m_fTick;
    double                              m_fTempo;
    bool                                m_bPaused;
    float                               m_fGain, m_fTargetGain;
    uint32_t                            m_nRand;
    std::map<const LTDMStylePart*, int> m_SeqVar;       // sequential variation counters
    Stats                               m_Stats;
    std::map<uint32_t, ChannelRange>    m_Ranges;
};

#endif // __LTDMPERF_H__
