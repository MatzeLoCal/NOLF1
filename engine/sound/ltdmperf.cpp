// ------------------------------------------------------------------------- //
//
// ltdmperf.cpp -- the music performance (HANDOVER 5cj). See ltdmperf.h.
//
// ------------------------------------------------------------------------- //

#include "ltdmperf.h"

#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    // DMUS_PLAYMODE_*.
    enum { kPMKeyRoot = 1, kPMChordRoot = 2, kPMScale = 4, kPMChord = 8, kPMNone = 16 };
    // DMUS_IO_INST_*.
    enum { kInstPatch = 0x1, kInstBankSelect = 0x2, kInstPan = 0x20, kInstVolume = 0x40,
           kInstTranspose = 0x80, kInstPriority = 0x800, kInstDefaultGM = 0x1000 };
    // DMUS_SEGF_* (a segment's default resolution).
    enum { kSegfGrid = 0x800, kSegfBeat = 0x1000, kSegfMeasure = 0x2000, kSegfSegEnd = 0x800000 };
    // DMUS_CURVET_* / DMUS_CURVES_*.
    enum { kCurvePB = 3, kCurveCC = 4 };
    enum { kShapeLinear = 0, kShapeInstant = 1, kShapeExp = 2, kShapeLog = 3, kShapeSine = 4 };
    // A disabled variation carries only DMUS_VARIATIONF_DMUS_MODE.
    const uint32_t kVarFunctionMask = 0x1FFFFFFFu;

    std::string Lower(const std::string& s)
    {
        std::string r(s);
        for (size_t i = 0; i < r.size(); ++i) r[i] = (char)tolower((unsigned char)r[i]);
        return r;
    }

    // The chord function the variation flags are matched against
    // (DMUS_VARIATIONF_*): the degree of the chord root in the scale and the
    // chord's quality, root kind and type.
    uint32_t ChordFunction(const LTDMSubChord& c)
    {
        int deg[12], n = 0;
        for (int i = 0; i < 12; ++i)
            if (c.m_nScalePattern & (1u << i)) deg[n++] = (c.m_nScaleRoot + i) % 12;
        const int root = c.m_nChordRoot % 12;
        int d = -1;
        uint32_t f = 0;
        for (int k = 0; k < n && d < 0; ++k) if (deg[k] == root) { d = k; f |= 0x200000; }
        for (int k = 0; k < n && d < 0; ++k) if (deg[k] == (root + 1) % 12) { d = k; f |= 0x400000; }
        for (int k = 0; k < n && d < 0; ++k) if (deg[k] == (root + 11) % 12) { d = k; f |= 0x800000; }
        if (d < 0) d = 0;
        if (d > 6) d = 6;
        const uint32_t p = c.m_nChordPattern;
        bool has[12];
        int nTones = 0;
        for (int i = 0; i < 12; ++i)
        {
            has[i] = (p & (1u << i)) || (p & (1u << (i + 12)));
            if (has[i]) ++nTones;
        }
        const int base = (has[4] && has[7]) ? 0 : ((has[3] && has[7]) ? 7 : 14);
        f |= 1u << (base + d);
        if (nTones <= 3) f |= 0x1000000;
        else if (nTones == 4 && (has[9] || has[10] || has[11])) f |= 0x2000000;
        else f |= 0x4000000;
        return f;
    }

    double ShapeAt(int nShape, double x)
    {
        switch (nShape)
        {
            case kShapeInstant: return 1.0;
            case kShapeExp:     return x * x;
            case kShapeLog:     return sqrt(x);
            case kShapeSine:    return 0.5 - 0.5 * cos(3.14159265358979 * x);
            default:            return x;
        }
    }
}

// --------------------------------------------------------------------------- //

float CLTDMPerformance::DBToGain(float fDB)
{
    return fDB <= -100.0f ? 0.0f : (float)pow(10.0, fDB / 20.0);
}

CLTDMPerformance::CLTDMPerformance(unsigned int nRate)
    : m_Synth(nRate), m_nRate(nRate ? nRate : 44100), m_pfnRead(0), m_pReadUser(0),
      m_pfnNotify(0), m_pNotifyUser(0), m_nSeq(0), m_nNextId(1), m_fTick(0.0),
      m_fTempo(120.0), m_bPaused(false), m_fGain(1.0f), m_fTargetGain(1.0f),
      m_nRand(0x4C544D55u)
{
    memset(&m_Stats, 0, sizeof(m_Stats));
    m_Stats.m_nMinNote = 127;
}

CLTDMPerformance::~CLTDMPerformance()
{
    Reset();
}

void CLTDMPerformance::Reset()
{
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    m_Synth.AllNotesOff(true);
    m_Synth.ResetChannels();
    m_Events.clear();
    m_States.clear();
    m_Sounding.clear();
    m_Transpose.clear();
    m_ChannelInst.clear();
    m_Downloaded.clear();
    m_SeqVar.clear();
    for (std::map<std::string, LTDMSegment*>::iterator i = m_Segments.begin(); i != m_Segments.end(); ++i) delete i->second;
    for (std::map<std::string, LTDMStyle*>::iterator i = m_Styles.begin(); i != m_Styles.end(); ++i) delete i->second;
    for (std::map<std::string, CLTDLSCollection*>::iterator i = m_DLS.begin(); i != m_DLS.end(); ++i) delete i->second;
    m_Segments.clear();
    m_Styles.clear();
    m_DLS.clear();
}

uint32_t CLTDMPerformance::Rand()
{
    uint32_t x = m_nRand;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return m_nRand = x;
}

// ---- assets ---------------------------------------------------------------- //

bool CLTDMPerformance::ReadFile(const std::string& sName, std::vector<unsigned char>& out)
{
    if (!m_pfnRead) return false;
    std::string sPath = sName;
    if (sName.find('\\') == std::string::npos && sName.find('/') == std::string::npos && !m_sDir.empty())
    {
        sPath = m_sDir;
        const char c = sPath[sPath.size() - 1];
        if (c != '\\' && c != '/') sPath += '\\';
        sPath += sName;
    }
    return m_pfnRead(sPath.c_str(), out, m_pReadUser);
}

const LTDMSegment* CLTDMPerformance::GetSegment(const char* pFile)
{
    if (!pFile || !*pFile) return 0;
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    const std::string k = Lower(pFile);
    std::map<std::string, LTDMSegment*>::iterator it = m_Segments.find(k);
    if (it != m_Segments.end()) return it->second;

    std::vector<unsigned char> b;
    LTDMSegment* p = 0;
    if (ReadFile(pFile, b) && !b.empty())
    {
        p = new LTDMSegment;
        if (!p->Load(&b[0], b.size())) { delete p; p = 0; }
    }
    m_Segments[k] = p;      // a miss is remembered too
    if (!p) { fprintf(stderr, "[LTDM] segment not loaded: %s\n", pFile); return 0; }

    for (size_t i = 0; i < p->m_Styles.size(); ++i)
    {
        const LTDMRef& r = p->m_Styles[i].m_Ref;
        GetStyle(!r.m_sFile.empty() ? r.m_sFile.c_str() : (r.m_sName + ".sty").c_str());
    }
    for (size_t i = 0; i < p->m_Bands.size(); ++i) PrepareBand(p->m_Bands[i]);
    return p;
}

const LTDMStyle* CLTDMPerformance::GetStyle(const char* pFile)
{
    if (!pFile || !*pFile) return 0;
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    const std::string k = Lower(pFile);
    std::map<std::string, LTDMStyle*>::iterator it = m_Styles.find(k);
    if (it != m_Styles.end()) return it->second;

    std::vector<unsigned char> b;
    LTDMStyle* p = 0;
    if (ReadFile(pFile, b) && !b.empty())
    {
        p = new LTDMStyle;
        if (!p->Load(&b[0], b.size())) { delete p; p = 0; }
    }
    m_Styles[k] = p;
    if (!p) { fprintf(stderr, "[LTDM] style not loaded: %s\n", pFile); return 0; }
    for (size_t i = 0; i < p->m_Bands.size(); ++i) PrepareBand(p->m_Bands[i]);
    return p;
}

const CLTDLSCollection* CLTDMPerformance::GetDLS(const char* pFile)
{
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    const std::string k = Lower(pFile ? pFile : "");
    std::map<std::string, CLTDLSCollection*>::iterator it = m_DLS.find(k);
    if (it != m_DLS.end()) return it->second;

    std::vector<unsigned char> b;
    bool bRead = false;
    if (k.empty())
    {
        if (!m_sGMPath.empty())
            if (FILE* f = fopen(m_sGMPath.c_str(), "rb"))
            {
                fseek(f, 0, SEEK_END);
                const long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (n > 0) { b.resize((size_t)n); bRead = fread(&b[0], 1, (size_t)n, f) == (size_t)n; }
                fclose(f);
            }
    }
    else bRead = ReadFile(pFile, b);

    CLTDLSCollection* p = 0;
    if (bRead && !b.empty())
    {
        p = new CLTDLSCollection;
        if (!p->Load(&b[0], b.size())) { delete p; p = 0; }
    }
    m_DLS[k] = p;
    if (!p) fprintf(stderr, "[LTDM] DLS not loaded: %s\n", k.empty() ? m_sGMPath.c_str() : pFile);
    return p;
}

void CLTDMPerformance::PrepareBand(const LTDMBand& b)
{
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    for (size_t i = 0; i < b.m_Instruments.size(); ++i)
    {
        const LTDMBandInstrument& bi = b.m_Instruments[i];
        if (!(bi.m_nFlags & kInstPatch)) continue;
        if (!bi.m_sDLSFile.empty() && !(bi.m_nFlags & kInstDefaultGM))
        {
            // Downloading: the patch now plays this instrument for anyone.
            const LTDLSInstrument* pi = 0;
            const CLTDLSCollection* pc = 0;
            if (GetDLS(bi.m_sDLSFile.c_str())) Resolve(bi, pi, pc);
        }
        else if (!m_sGMPath.empty()) GetDLS("");
    }
}

bool CLTDMPerformance::Resolve(const LTDMBandInstrument& bi, const LTDLSInstrument*& pInst,
                               const CLTDLSCollection*& pColl)
{
    pInst = 0; pColl = 0;
    uint32_t nPatch = bi.m_nPatch;
    if (!(bi.m_nFlags & kInstBankSelect)) nPatch &= 0x8000007Fu;

    // A named collection: no loading here (this runs on the music thread);
    // PrepareBand loaded it.
    if (!bi.m_sDLSFile.empty() && !(bi.m_nFlags & kInstDefaultGM))
    {
        std::map<std::string, CLTDLSCollection*>::iterator it = m_DLS.find(Lower(bi.m_sDLSFile));
        if (it != m_DLS.end() && it->second)
        {
            const LTDLSInstrument* p = it->second->Find(nPatch, true);
            if (!p) p = it->second->Find(nPatch, false);
            if (p)
            {
                pInst = p; pColl = it->second;
                m_Downloaded[nPatch] = std::make_pair(p, (const CLTDLSCollection*)it->second);
                return true;
            }
        }
    }
    // Downloaded by another band.
    std::map<uint32_t, std::pair<const LTDLSInstrument*, const CLTDLSCollection*> >::iterator d = m_Downloaded.find(nPatch);
    if (d != m_Downloaded.end()) { pInst = d->second.first; pColl = d->second.second; return true; }
    // Any loaded collection that has the patch exactly.
    for (std::map<std::string, CLTDLSCollection*>::iterator it = m_DLS.begin(); it != m_DLS.end(); ++it)
    {
        if (it->first.empty() || !it->second) continue;
        if (const LTDLSInstrument* p = it->second->Find(nPatch, true))
        { pInst = p; pColl = it->second; return true; }
    }
    // The GM set.
    std::map<std::string, CLTDLSCollection*>::iterator gm = m_DLS.find("");
    if (gm != m_DLS.end() && gm->second)
        if (const LTDLSInstrument* p = gm->second->Find(nPatch, false))
        { pInst = p; pColl = gm->second; ++m_Stats.m_nGMHits; return true; }
    return false;
}

// ---- music values ---------------------------------------------------------- //

const LTDMChord& CLTDMPerformance::DefaultChord()
{
    // ⚠️ ASSUMED, not measured: C major 7 over a C major scale, root in the
    // second octave like the chords NOLF's own chord tracks carry (roots
    // 10..14). Every segment without a chord track plays through it.
    static LTDMChord s;
    if (s.m_Sub.empty())
    {
        LTDMSubChord c;
        c.m_nChordPattern = 0x891;          // 1 3 5 7
        c.m_nScalePattern = 0xAB5AB5;       // C major, two octaves
        c.m_nInversionPoints = 0xFFFFFF;
        c.m_nLevels = 0xFFFFFFFF;
        c.m_nChordRoot = 12;
        c.m_nScaleRoot = 0;
        s.m_nTime = 0;
        s.m_nGroup = 0xFFFFFFFFu;
        s.m_sName = "M7";
        s.m_Sub.push_back(c);
    }
    return s;
}

unsigned int CLTDMPerformance::s_nOverChord = 0;

namespace
{
    // nSteps scale tones up from pitch p (the scale pattern is relative to
    // the scale root).
    int ScaleUp(int p, int nSteps, const LTDMSubChord& c)
    {
        for (int nGuard = 0; nSteps > 0 && nGuard < 96; ++nGuard)
        {
            ++p;
            const int pc = ((p - c.m_nScaleRoot) % 12 + 12) % 12;
            if (c.m_nScalePattern & (1u << pc)) --nSteps;
        }
        return p;
    }
}

// A music value (DMUS_NOTE_PMSG wMusicValue): octave (bits 12-15), chord
// position (8-11), scale position (4-7), accidental (0-3, signed). The note is
// the chord position-th tone of the chord over its root, then scale position
// scale tones up, then the accidental, in the octave given.
//
// ⚠️ TWO DECISIONS HERE ARE THE PORT'S, made from NOLF's own data
// (HANDOVER 5cj), not from any implementation:
//   * THE OCTAVE: 12 x (octave - 1). With 12 x octave, 4,471 of 34,126 notes
//     over all six music sets fell outside every region of their instrument
//     (silent); one octave down, 37. The composers wrote for the samples they
//     shipped. LT_DMOCTAVE=<n> overrides the -1 (says so).
//   * PAST THE TOP OF THE CHORD (ALWAYSPLAY): Microsoft's documentation says
//     the note is then placed "using intervals from the scale". Read here as
//     the chord carrying on in thirds of the scale -- two scale steps per
//     further position -- which gives back the note a composer entered
//     against a C major 7 (position 3 = the seventh) when it plays over the
//     triad of the same root. NORMALCHORD does not play such a note (the
//     documented DMUS_S_OVER_CHORD). LT_DMOVERCHORD=wrap = once round the
//     chord again, an octave up (old; says so).
int CLTDMPerformance::MusicToMIDI(uint16_t nValue, uint8_t nMode, const LTDMSubChord& c)
{
    if (nMode == 0) return nValue & 0x7F;                               // FIXED
    if (nMode == kPMKeyRoot) return (nValue + c.m_nScaleRoot) & 0x7F;   // FIXEDTOKEY
    if (nMode == kPMChordRoot) return (nValue + c.m_nChordRoot) & 0x7F; // FIXEDTOCHORD

    static const int s_nOctave = []() {
        const char* e = getenv("LT_DMOCTAVE");
        const int n = e ? atoi(e) : -1;
        if (e) { printf("[LTDM] LT_DMOCTAVE=%d -- chord-relative notes at 12 x (octave %+d)\n", n, n); fflush(stdout); }
        return n; }();
    static const bool s_bWrap = []() {
        const char* e = getenv("LT_DMOVERCHORD");
        const bool b = e && !strcmp(e, "wrap");
        if (b) { printf("[LTDM] LT_DMOVERCHORD=wrap -- past the top of a chord wraps an octave up (old)\n"); fflush(stdout); }
        return b; }();

    const int nOctave   = (nValue >> 12) & 0xF;
    const int nChordPos = (nValue >> 8) & 0xF;
    const int nScalePos = (nValue >> 4) & 0xF;
    int nAcc = nValue & 0xF;
    if (nAcc > 7) nAcc -= 16;

    const int nRoot = (nMode & kPMChordRoot) ? c.m_nChordRoot : c.m_nScaleRoot;
    int nBase = 0;
    if (nMode & kPMChord)
    {
        int aTones[24], n = 0;
        for (int i = 0; i < 24; ++i) if (c.m_nChordPattern & (1u << i)) aTones[n++] = i;
        if (!n) { aTones[0] = 0; n = 1; }
        if (nChordPos < n) nBase = aTones[nChordPos];
        else
        {
            ++s_nOverChord;
            if (!(nMode & kPMScale)) return -1;                         // NORMALCHORD: not played
            if (s_bWrap) nBase = aTones[nChordPos % n] + 12 * (nChordPos / n);
            else nBase = ScaleUp(nRoot + aTones[n - 1], 2 * (nChordPos - (n - 1)), c) - nRoot;
        }
    }
    if ((nMode & kPMScale) && nScalePos) nBase = ScaleUp(nRoot + nBase, nScalePos, c) - nRoot;

    int nNote = 12 * (nOctave + s_nOctave) + nRoot + nBase + nAcc;
    // DMUS_S_UP_OCTAVE / DMUS_S_DOWN_OCTAVE: into range by whole octaves.
    while (nNote < 0) nNote += 12;
    while (nNote > 127) nNote -= 12;
    return nNote;
}

const LTDMSubChord& CLTDMPerformance::ChordAt(const LTDMSegment* pSeg, int nSegTime, uint8_t nLevel, uint32_t nGroup) const
{
    // The chord track sharing a group bit with the asking style track.
    const LTDMChord* pChord = 0;
    if (pSeg)
        for (size_t i = 0; i < pSeg->m_Chords.size(); ++i)
        {
            const LTDMChord& c = pSeg->m_Chords[i];
            if (!(c.m_nGroup & nGroup)) continue;
            if (!pChord || c.m_nTime <= nSegTime) pChord = &c;
            if (c.m_nTime > nSegTime) break;
        }
    if (!pChord || pChord->m_Sub.empty()) pChord = &DefaultChord();
    const uint32_t nBit = nLevel < 32 ? 1u << nLevel : 1u;
    for (size_t i = 0; i < pChord->m_Sub.size(); ++i)
        if (pChord->m_Sub[i].m_nLevels & nBit) return pChord->m_Sub[i];
    return pChord->m_Sub[0];
}

const LTDMSubChord& CLTDMPerformance::ChordAtTick(double fTick, uint8_t nLevel) const
{
    // The primary sounding at that tick -- secondaries and motifs take the
    // primary's chord.
    const State* p = 0;
    for (size_t i = 0; i < m_States.size(); ++i)
    {
        const State& s = m_States[i];
        if (s.m_bPrimary && s.m_pSeg && s.m_fStart <= fTick && fTick < s.m_fEnd)
            if (!p || s.m_fStart >= p->m_fStart) p = &s;
    }
    if (!p) return ChordAt(0, 0, nLevel, 0xFFFFFFFFu);
    const LTDMSegment& g = *p->m_pSeg;
    int t = (int)(fTick - p->m_fStart);
    if (g.m_nRepeats && t >= g.m_nLength)
    {
        const int nLoopEnd = g.m_nLoopEnd > g.m_nLoopStart ? g.m_nLoopEnd : g.m_nLength;
        const int nLoop = nLoopEnd - g.m_nLoopStart;
        if (nLoop > 0 && t >= nLoopEnd) t = g.m_nLoopStart + (t - nLoopEnd) % nLoop;
    }
    return ChordAt(&g, t, nLevel, 0xFFFFFFFFu);
}

bool CLTDMPerformance::MuteAt(const LTDMSegment* pSeg, int nSegTime, uint32_t& nPChannel, uint32_t nGroup) const
{
    if (!pSeg) return false;
    const LTDMMute* pHit = 0;
    for (size_t i = 0; i < pSeg->m_Mutes.size(); ++i)
    {
        const LTDMMute& m = pSeg->m_Mutes[i];
        if ((m.m_nGroup & nGroup) && m.m_nPChannel == nPChannel && m.m_nTime <= nSegTime) pHit = &m;
    }
    if (!pHit) return false;
    if (pHit->m_nMap == 0xFFFFFFFFu) return true;
    nPChannel = pHit->m_nMap;
    return false;
}

// ---- generation ------------------------------------------------------------ //

void CLTDMPerformance::Push(Event e)
{
    static const int kOrder[] = { 0, 1, 2, 3, 3, 4, 5, 6 };
    e.m_nOrder = kOrder[e.m_eKind];
    e.m_nSeq = m_nSeq++;
    m_Events.push_back(e);
    std::push_heap(m_Events.begin(), m_Events.end(), Later());
}

void CLTDMPerformance::Note(const State& s, double fTick, uint32_t nPChannel, int nMidi, int nVel, double fDur)
{
    if (fTick < s.m_fStart) fTick = s.m_fStart;
    if (fDur < 1.0) fDur = 1.0;
    nVel = nVel < 1 ? 1 : (nVel > 127 ? 127 : nVel);
    Event e;
    memset(&e, 0, sizeof(e));
    e.m_fTick = fTick;
    e.m_eKind = Event::kNoteOn;
    e.m_nTag = s.m_nId;
    e.m_nPChannel = nPChannel;
    e.m_nA = nMidi;
    e.m_nB = nVel;
    Push(e);
    e.m_fTick = fTick + fDur;
    e.m_eKind = Event::kNoteOff;
    e.m_fValue = fTick;
    Push(e);
}

void CLTDMPerformance::ChooseVariations(Instance& in, const LTDMSubChord& chord)
{
    const uint32_t fn = ChordFunction(chord);
    std::map<int, int> lockVar;
    in.m_Var.assign(in.m_pPattern->m_PartRefs.size(), -1);
    for (size_t k = 0; k < in.m_pPattern->m_PartRefs.size(); ++k)
    {
        const LTDMPartRef& r = in.m_pPattern->m_PartRefs[k];
        if (r.m_nPart < 0 || r.m_nPart >= (int)in.m_pStyle->m_Parts.size()) continue;
        const LTDMStylePart& part = in.m_pStyle->m_Parts[r.m_nPart];

        if (r.m_nLockId)
        {
            std::map<int, int>::iterator l = lockVar.find(r.m_nLockId);
            if (l != lockVar.end()) { in.m_Var[k] = l->second; continue; }
        }
        int aOk[32], n = 0;
        for (int v = 0; v < 32; ++v)
            if ((part.m_VariationChoices[v] & fn) == fn) aOk[n++] = v;
        if (!n)     // nothing fits this chord: anything enabled
            for (int v = 0; v < 32; ++v)
                if (part.m_VariationChoices[v] & kVarFunctionMask) aOk[n++] = v;
        if (!n) { aOk[0] = 0; n = 1; }

        int v;
        if (r.m_nRandomVariation == 0)
        {
            // DMUS_VARIATIONT_SEQUENTIAL: the next enabled one after the last.
            std::map<const LTDMStylePart*, int>::iterator s = m_SeqVar.find(&part);
            const int nLast = s == m_SeqVar.end() ? -1 : s->second;
            v = aOk[0];
            for (int i = 0; i < n; ++i) if (aOk[i] > nLast) { v = aOk[i]; break; }
            m_SeqVar[&part] = v;
        }
        else v = aOk[Rand() % (uint32_t)n];
        in.m_Var[k] = v;
        if (r.m_nLockId) lockVar[r.m_nLockId] = v;
    }
}

void CLTDMPerformance::GenerateInstanceMeasure(const State& s, const Instance& in, int nMeasure,
                                               double /*fMeasureStart*/, int nSegTime, double fOffset, int nLimit)
{
    const LTDMPattern& pat = *in.m_pPattern;
    const LTDMStyle& sty = *in.m_pStyle;
    const LTDMSegment* pSeg = s.m_pSeg;
    bool bOwnChords = false;
    if (pSeg) for (size_t i = 0; i < pSeg->m_Chords.size(); ++i) if (pSeg->m_Chords[i].m_nGroup & in.m_nGroup) bOwnChords = true;

    for (size_t k = 0; k < pat.m_PartRefs.size(); ++k)
    {
        const LTDMPartRef& r = pat.m_PartRefs[k];
        const int v = in.m_Var[k];
        if (v < 0 || r.m_nPart < 0 || r.m_nPart >= (int)sty.m_Parts.size()) continue;
        const LTDMStylePart& part = sty.m_Parts[r.m_nPart];
        const uint32_t nBit = 1u << v;
        const int nPartMeasures = part.m_nMeasures ? part.m_nMeasures : 1;
        const int pm = nMeasure % nPartMeasures;
        const LTDMTimeSig& ts = part.m_TimeSig;
        const int nMT = ts.MeasureTicks();

        for (size_t i = 0; i < part.m_Notes.size(); ++i)
        {
            const LTDMStyleNote& n = part.m_Notes[i];
            if (!(n.m_nVariation & nBit)) continue;
            const int gt = ts.GridToTicks(n.m_nGridStart);
            if (gt / nMT != pm) continue;
            const int t = nSegTime + (gt - pm * nMT);
            if (t >= nLimit) continue;

            uint32_t nPCh = r.m_nPChannel;
            if (MuteAt(pSeg, t, nPCh, in.m_nGroup)) { ++m_Stats.m_nMuted; continue; }

            const uint8_t nMode = n.m_nPlayMode == kPMNone ? part.m_nPlayMode : n.m_nPlayMode;
            const double fAbs = s.m_fStart + fOffset + t;
            const LTDMSubChord& c = (s.m_bPrimary || bOwnChords) ? ChordAt(pSeg, t, r.m_nSubChordLevel, in.m_nGroup)
                                                                 : ChordAtTick(fAbs, r.m_nSubChordLevel);
            const int nMidi = MusicToMIDI(n.m_nMusicValue, nMode, c);
            if (nMidi < 0) continue;

            int nTime = n.m_nTimeOffset, nDur = n.m_nDuration, nVel = n.m_nVelocity;
            if (n.m_nTimeRange) nTime += (int)(Rand() % (n.m_nTimeRange + 1u)) - n.m_nTimeRange / 2;
            if (n.m_nDurRange)  nDur  += (int)(Rand() % (n.m_nDurRange + 1u)) - n.m_nDurRange / 2;
            if (n.m_nVelRange)  nVel  += (int)(Rand() % (n.m_nVelRange + 1u)) - n.m_nVelRange / 2;
            Note(s, fAbs + nTime, nPCh, nMidi, nVel, nDur);
        }

        for (size_t i = 0; i < part.m_Curves.size(); ++i)
        {
            const LTDMStyleCurve& cv = part.m_Curves[i];
            if (!(cv.m_nVariation & nBit)) continue;
            if (cv.m_nEventType != kCurvePB && cv.m_nEventType != kCurveCC) continue;
            const int gt = ts.GridToTicks(cv.m_nGridStart);
            if (gt / nMT != pm) continue;
            const int t = nSegTime + (gt - pm * nMT);
            if (t >= nLimit) continue;
            uint32_t nPCh = r.m_nPChannel;
            if (MuteAt(pSeg, t, nPCh, in.m_nGroup)) continue;

            const double fAbs = s.m_fStart + fOffset + t + cv.m_nTimeOffset;
            Event e;
            memset(&e, 0, sizeof(e));
            e.m_eKind = cv.m_nEventType == kCurvePB ? Event::kBend : Event::kCC;
            e.m_nTag = s.m_nId;
            e.m_nPChannel = nPCh;
            e.m_nA = cv.m_nCC;
            const int nSteps = (cv.m_nShape == kShapeInstant || cv.m_nDuration <= 0) ? 0
                             : std::max(1, std::min(64, cv.m_nDuration / 24));
            int nLast = -1;
            for (int j = 0; j <= nSteps; ++j)
            {
                const double x = nSteps ? (double)j / nSteps : 1.0;
                const int val = (int)lround(cv.m_nStart + (cv.m_nEnd - cv.m_nStart) * ShapeAt(cv.m_nShape, x));
                if (val == nLast) continue;
                nLast = val;
                e.m_fTick = std::max(s.m_fStart, fAbs + x * (nSteps ? cv.m_nDuration : 0));
                e.m_nB = val;
                Push(e);
            }
            if (cv.m_nFlags & 1)
            {
                e.m_fTick = std::max(s.m_fStart, fAbs + cv.m_nDuration + cv.m_nResetDuration);
                e.m_nB = cv.m_nReset;
                Push(e);
            }
        }
    }
}

void CLTDMPerformance::GenerateStyleSpan(const State& s, int nFrom, int nTo, double fOffset, uint32_t nGroup)
{
    // ONE style track (one group): its styles, and the commands, chords and
    // mutes of the tracks sharing a group bit with it.
    const LTDMSegment& g = *s.m_pSeg;
    std::vector<const LTDMStyleRef*> refs;
    for (size_t i = 0; i < g.m_Styles.size(); ++i) if (g.m_Styles[i].m_nGroup == nGroup) refs.push_back(&g.m_Styles[i]);
    std::vector<const LTDMCommand*> cmds;
    for (size_t i = 0; i < g.m_Commands.size(); ++i) if (g.m_Commands[i].m_nGroup & nGroup) cmds.push_back(&g.m_Commands[i]);
    if (refs.empty()) return;

    Instance cur;
    cur.m_pStyle = 0; cur.m_pPattern = 0; cur.m_fStart = 0; cur.m_nGroup = nGroup;
    int nPatEnd = 0;
    size_t nNextCmd = 0;
    while (nNextCmd < cmds.size() && cmds[nNextCmd]->m_nTime < nFrom) ++nNextCmd;

    int t = nFrom;
    while (t < nTo)
    {
        // The style in force.
        const LTDMStyleRef* pRef = refs[0];
        for (size_t i = 0; i < refs.size(); ++i) if (refs[i]->m_nTime <= t) pRef = refs[i];
        std::map<std::string, LTDMStyle*>::const_iterator it = m_Styles.find(
            Lower(!pRef->m_Ref.m_sFile.empty() ? pRef->m_Ref.m_sFile : pRef->m_Ref.m_sName + ".sty"));
        const LTDMStyle* pStyle = it != m_Styles.end() ? it->second : 0;
        if (!pStyle) return;

        // The groove in force, and whether a command lands in this measure.
        const LTDMCommand* pCmd = 0;
        for (size_t i = 0; i < cmds.size(); ++i) if (cmds[i]->m_nTime <= t) pCmd = cmds[i];
        bool bNewCmd = false;
        while (nNextCmd < cmds.size() && cmds[nNextCmd]->m_nTime <= t) { bNewCmd = true; ++nNextCmd; }

        if (!cur.m_pPattern || t >= nPatEnd || bNewCmd || pStyle != cur.m_pStyle)
        {
            int nGroove = pCmd ? pCmd->m_nGrooveLevel : 1;
            if (pCmd && pCmd->m_nGrooveRange)
                nGroove += (int)(Rand() % (pCmd->m_nGrooveRange + 1u)) - pCmd->m_nGrooveRange / 2;
            nGroove = nGroove < 1 ? 1 : (nGroove > 100 ? 100 : nGroove);

            std::vector<const LTDMPattern*> aFit;
            const LTDMPattern* pNear = 0;
            int nNear = 1000;
            for (size_t i = 0; i < pStyle->m_Patterns.size(); ++i)
            {
                const LTDMPattern& p = pStyle->m_Patterns[i];
                if (p.m_nEmbellishment & 16) continue;          // a motif
                if (p.m_nGrooveBottom <= nGroove && nGroove <= p.m_nGrooveTop) aFit.push_back(&p);
                const int d = nGroove < p.m_nGrooveBottom ? p.m_nGrooveBottom - nGroove : nGroove - p.m_nGrooveTop;
                if (d < nNear) { nNear = d; pNear = &p; }
            }
            const LTDMPattern* pPat = !aFit.empty() ? aFit[Rand() % (uint32_t)aFit.size()] : pNear;
            if (!pPat) { t += pStyle->m_TimeSig.MeasureTicks(); continue; }

            cur.m_pStyle = pStyle;
            cur.m_pPattern = pPat;
            cur.m_fStart = t;
            ChooseVariations(cur, ChordAt(&g, t, 0, nGroup));
            nPatEnd = t + (pPat->m_nMeasures ? pPat->m_nMeasures : 1) * pPat->m_TimeSig.MeasureTicks();
        }

        const int nMT = cur.m_pPattern->m_TimeSig.MeasureTicks();
        const int nMeasure = (int)((t - cur.m_fStart) / nMT);
        GenerateInstanceMeasure(s, cur, nMeasure, 0.0, t, fOffset, nTo);
        t += nMT;
    }
}

void CLTDMPerformance::Generate(const State& s)
{
    const LTDMSegment& g = *s.m_pSeg;

    // Spans of segment time and where each lands: the segment once, with its
    // loop region played m_nRepeats more times (DirectMusic's own repeats).
    struct Span { int m_nFrom, m_nTo; double m_fOffset; };
    std::vector<Span> spans;
    const int nLoopEnd = g.m_nLoopEnd > g.m_nLoopStart ? g.m_nLoopEnd : g.m_nLength;
    const int nLoop = nLoopEnd - g.m_nLoopStart;
    if (g.m_nRepeats && nLoop > 0)
    {
        Span a = { 0, nLoopEnd, 0.0 };
        spans.push_back(a);
        for (uint32_t r = 1; r <= g.m_nRepeats; ++r)
        {
            Span b = { g.m_nLoopStart, nLoopEnd, (double)nLoopEnd + (double)(r - 1) * nLoop - g.m_nLoopStart };
            spans.push_back(b);
        }
        if (nLoopEnd < g.m_nLength)
        {
            Span c = { nLoopEnd, g.m_nLength, (double)g.m_nRepeats * nLoop };
            spans.push_back(c);
        }
    }
    else
    {
        Span a = { 0, g.m_nLength, 0.0 };
        spans.push_back(a);
    }

    Event e;
    for (size_t k = 0; k < spans.size(); ++k)
    {
        const Span& sp = spans[k];
        const double fBase = s.m_fStart + sp.m_fOffset;

        memset(&e, 0, sizeof(e));
        e.m_nTag = s.m_nId;
        e.m_eKind = Event::kTempo;
        for (size_t i = 0; i < g.m_Tempos.size(); ++i)
        {
            const LTDMTempo& tp = g.m_Tempos[i];
            const bool bIn = tp.m_nTime >= sp.m_nFrom && tp.m_nTime < sp.m_nTo;
            // A repeat starts at the tempo in force at the loop start.
            const bool bCarry = k && tp.m_nTime < sp.m_nFrom
                             && (i + 1 == g.m_Tempos.size() || g.m_Tempos[i + 1].m_nTime > sp.m_nFrom);
            if (!bIn && !bCarry) continue;
            e.m_fTick = fBase + (bIn ? tp.m_nTime : sp.m_nFrom);
            e.m_fValue = tp.m_fBPM;
            Push(e);
        }
        e.m_eKind = Event::kBand;
        for (size_t i = 0; i < g.m_Bands.size(); ++i)
        {
            const LTDMBand& b = g.m_Bands[i];
            if (b.m_nTime < sp.m_nFrom || b.m_nTime >= sp.m_nTo) continue;
            e.m_fTick = fBase + b.m_nTime;
            e.m_pBand = &b;
            Push(e);
        }
        e.m_pBand = 0;
        for (size_t i = 0; i < g.m_Seq.size(); ++i)
        {
            const LTDMSeqNote& n = g.m_Seq[i];
            if (n.m_nTime < sp.m_nFrom || n.m_nTime >= sp.m_nTo) continue;
            const double fAt = fBase + n.m_nTime;
            switch (n.m_nStatus & 0xF0)
            {
                case 0x90: Note(s, fAt, n.m_nPChannel, n.m_nData1, n.m_nData2, n.m_nDuration); break;
                case 0xB0: e.m_eKind = Event::kCC; e.m_fTick = fAt; e.m_nPChannel = n.m_nPChannel;
                           e.m_nA = n.m_nData1; e.m_nB = n.m_nData2; Push(e); break;
                case 0xE0: e.m_eKind = Event::kBend; e.m_fTick = fAt; e.m_nPChannel = n.m_nPChannel;
                           e.m_nB = n.m_nData1 | (n.m_nData2 << 7); Push(e); break;
                default: break;
            }
        }
        // Every style track, each in its own group.
        std::vector<uint32_t> groups;
        for (size_t i = 0; i < g.m_Styles.size(); ++i)
            if (std::find(groups.begin(), groups.end(), g.m_Styles[i].m_nGroup) == groups.end())
                groups.push_back(g.m_Styles[i].m_nGroup);
        for (size_t i = 0; i < groups.size(); ++i) GenerateStyleSpan(s, sp.m_nFrom, sp.m_nTo, sp.m_fOffset, groups[i]);
    }
}

// ---- playback -------------------------------------------------------------- //

const CLTDMPerformance::State* CLTDMPerformance::SoundingPrimary() const
{
    const State* p = 0;
    for (size_t i = 0; i < m_States.size(); ++i)
    {
        const State& s = m_States[i];
        if (s.m_bPrimary && s.m_fStart <= m_fTick && m_fTick < s.m_fEnd)
            if (!p || s.m_fStart >= p->m_fStart) p = &s;
    }
    return p;
}

const CLTDMPerformance::State* CLTDMPerformance::LatestPrimary() const
{
    const State* p = 0;
    for (size_t i = 0; i < m_States.size(); ++i)
        if (m_States[i].m_bPrimary && (!p || m_States[i].m_nId > p->m_nId)) p = &m_States[i];
    return p;
}

double CLTDMPerformance::Boundary(Enact e, const LTDMSegment* pSeg) const
{
    if (e == kDefault)
    {
        const uint32_t r = pSeg ? pSeg->m_nResolution : kSegfMeasure;
        e = (r & kSegfSegEnd) ? kSegmentEnd : (r & kSegfMeasure) ? kMeasure
          : (r & kSegfBeat) ? kBeat : (r & kSegfGrid) ? kGrid : kImmediate;
    }
    const double now = m_fTick;
    if (e == kImmediate) return now;
    if (e == kSegmentEnd)
    {
        const State* p = LatestPrimary();
        return p && p->m_fEnd > now ? p->m_fEnd : now;
    }
    const State* p = SoundingPrimary();
    if (!p) return now;
    const double unit = e == kGrid ? p->m_TimeSig.GridTicks()
                      : e == kBeat ? p->m_TimeSig.BeatTicks() : p->m_TimeSig.MeasureTicks();
    if (unit <= 0.0) return now;
    const double n = ceil((now - p->m_fStart) / unit - 1e-6);
    return p->m_fStart + n * unit;
}

void CLTDMPerformance::CutState(unsigned int nId, double fTick, bool bRelease)
{
    std::vector<Event> keep;
    keep.reserve(m_Events.size());
    for (size_t i = 0; i < m_Events.size(); ++i)
    {
        const Event& e = m_Events[i];
        if (e.m_nTag == nId && e.m_fTick >= fTick && !(e.m_eKind == Event::kNoteOff && e.m_fValue < fTick)) continue;
        keep.push_back(e);
    }
    m_Events.swap(keep);
    std::make_heap(m_Events.begin(), m_Events.end(), Later());
    if (bRelease)
    {
        Event e;
        memset(&e, 0, sizeof(e));
        e.m_fTick = fTick;
        e.m_eKind = Event::kStopTag;
        e.m_nTag = nId;
        Push(e);
    }
    for (size_t i = 0; i < m_States.size(); ++i)
        if (m_States[i].m_nId == nId)
        {
            State& s = m_States[i];
            if (s.m_fEnd > fTick) s.m_fEnd = fTick;
            if (s.m_fStart > s.m_fEnd) s.m_fStart = s.m_fEnd;
        }
}

namespace
{
    LTDMTimeSig SegmentTimeSig(const LTDMSegment& g, const std::map<std::string, LTDMStyle*>& styles)
    {
        if (!g.m_Styles.empty())
        {
            const LTDMRef& r = g.m_Styles[0].m_Ref;
            std::string k = !r.m_sFile.empty() ? r.m_sFile : r.m_sName + ".sty";
            for (size_t i = 0; i < k.size(); ++i) k[i] = (char)tolower((unsigned char)k[i]);
            std::map<std::string, LTDMStyle*>::const_iterator it = styles.find(k);
            if (it != styles.end() && it->second) return it->second->m_TimeSig;
        }
        if (!g.m_TimeSigs.empty()) return g.m_TimeSigs[0].m_TimeSig;
        return LTDMTimeSig();
    }
}

unsigned int CLTDMPerformance::PlayPrimary(const LTDMSegment* pSeg, Enact e)
{
    if (!pSeg) return 0;
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    const double T = Boundary(e, pSeg);
    for (size_t i = 0; i < m_States.size(); ++i)
        if (m_States[i].m_bPrimary && m_States[i].m_fEnd > T) CutState(m_States[i].m_nId, T, false);

    State s;
    s.m_nId = m_nNextId++;
    s.m_pSeg = pSeg;
    s.m_bPrimary = true;
    s.m_fStart = T;
    const int nLoopEnd = pSeg->m_nLoopEnd > pSeg->m_nLoopStart ? pSeg->m_nLoopEnd : pSeg->m_nLength;
    s.m_fEnd = T + pSeg->m_nLength + (double)pSeg->m_nRepeats * (nLoopEnd - pSeg->m_nLoopStart);
    s.m_TimeSig = SegmentTimeSig(*pSeg, m_Styles);
    s.m_bStartSent = s.m_bAlmostSent = false;
    m_States.push_back(s);
    Generate(s);
    return s.m_nId;
}

unsigned int CLTDMPerformance::PlaySecondary(const LTDMSegment* pSeg, Enact e)
{
    if (!pSeg) return 0;
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    State s;
    s.m_nId = m_nNextId++;
    s.m_pSeg = pSeg;
    s.m_bPrimary = false;
    s.m_fStart = Boundary(e, pSeg);
    s.m_fEnd = s.m_fStart + pSeg->m_nLength;
    s.m_TimeSig = SegmentTimeSig(*pSeg, m_Styles);
    s.m_bStartSent = s.m_bAlmostSent = false;
    m_States.push_back(s);
    Generate(s);
    return s.m_nId;
}

unsigned int CLTDMPerformance::PlayMotif(const LTDMStyle* pStyle, const LTDMPattern* pMotif, Enact e)
{
    if (!pStyle || !pMotif) return 0;
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    const int nMT = pMotif->m_TimeSig.MeasureTicks();
    const int nMeasures = pMotif->m_nMeasures ? pMotif->m_nMeasures : 1;
    State s;
    s.m_nId = m_nNextId++;
    s.m_pSeg = 0;
    s.m_bPrimary = false;
    s.m_fStart = Boundary(e == kDefault ? kMeasure : e, 0);
    s.m_fEnd = s.m_fStart + (double)nMeasures * nMT;
    s.m_TimeSig = pMotif->m_TimeSig;
    s.m_bStartSent = s.m_bAlmostSent = false;
    m_States.push_back(s);

    Instance in;
    in.m_pStyle = pStyle;
    in.m_pPattern = pMotif;
    in.m_fStart = 0;
    in.m_nGroup = 0xFFFFFFFFu;
    ChooseVariations(in, ChordAtTick(s.m_fStart, 0));
    for (int m = 0; m < nMeasures; ++m)
        GenerateInstanceMeasure(s, in, m, 0.0, m * nMT, 0.0, nMeasures * nMT);
    return s.m_nId;
}

void CLTDMPerformance::PlayBandNow(const LTDMBand& b)
{
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    ApplyBand(b);
}

void CLTDMPerformance::StopState(unsigned int nId, Enact e)
{
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    for (size_t i = 0; i < m_States.size(); ++i)
        if (m_States[i].m_nId == nId)
        {
            const double T = Boundary(e, m_States[i].m_pSeg);
            CutState(nId, T, true);
            return;
        }
}

void CLTDMPerformance::StopAll(Enact e)
{
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    const State* p = SoundingPrimary();
    const double T = Boundary(e, p ? p->m_pSeg : 0);
    std::vector<unsigned int> ids;
    for (size_t i = 0; i < m_States.size(); ++i) ids.push_back(m_States[i].m_nId);
    for (size_t i = 0; i < ids.size(); ++i) CutState(ids[i], T, true);
}

bool CLTDMPerformance::IsPlaying(unsigned int nId) const
{
    for (size_t i = 0; i < m_States.size(); ++i) if (m_States[i].m_nId == nId) return true;
    return false;
}

void CLTDMPerformance::SetPaused(bool b)
{
    std::lock_guard<std::recursive_mutex> g(m_Lock);
    if (b && !m_bPaused) m_Synth.AllNotesOff(false);
    m_bPaused = b;
}

// ---- dispatch -------------------------------------------------------------- //

void CLTDMPerformance::ApplyBand(const LTDMBand& b)
{
    for (size_t i = 0; i < b.m_Instruments.size(); ++i)
    {
        const LTDMBandInstrument& bi = b.m_Instruments[i];
        const LTDLSInstrument* pi = 0;
        const CLTDLSCollection* pc = 0;
        if ((bi.m_nFlags & kInstPatch) && !Resolve(bi, pi, pc)) ++m_Stats.m_nBandMisses;
        m_Synth.SetChannel(bi.m_nPChannel, pi, pc,
                           (bi.m_nFlags & kInstVolume) ? bi.m_nVolume : -1,
                           (bi.m_nFlags & kInstPan) ? bi.m_nPan : -1,
                           (bi.m_nFlags & kInstPriority) ? bi.m_nPriority : 0);
        if (pi) m_ChannelInst[bi.m_nPChannel] = pi;
        if (bi.m_nFlags & kInstTranspose) m_Transpose[bi.m_nPChannel] = bi.m_nTranspose;
    }
}

void CLTDMPerformance::Dispatch(const Event& e)
{
    const uint64_t nKey = ((uint64_t)e.m_nTag << 40) | ((uint64_t)(e.m_nPChannel & 0xFFFFFFu) << 8) | (uint64_t)(e.m_nA & 0xFF);
    switch (e.m_eKind)
    {
        case Event::kTempo:
            if (e.m_fValue > 1.0) m_fTempo = e.m_fValue;
            break;
        case Event::kBand:
            if (e.m_pBand) ApplyBand(*e.m_pBand);
            break;
        case Event::kNoteOn:
        {
            std::map<uint32_t, int>::const_iterator tr = m_Transpose.find(e.m_nPChannel);
            int n = e.m_nA + (tr != m_Transpose.end() ? tr->second : 0);
            n = n < 0 ? 0 : (n > 127 ? 127 : n);
            m_Sounding[nKey] = n;

            ++m_Stats.m_nNotes;
            if ((unsigned)n < m_Stats.m_nMinNote) m_Stats.m_nMinNote = n;
            if ((unsigned)n > m_Stats.m_nMaxNote) m_Stats.m_nMaxNote = n;
            std::map<uint32_t, const LTDLSInstrument*>::const_iterator ci = m_ChannelInst.find(e.m_nPChannel);
            const LTDLSInstrument* pi = ci != m_ChannelInst.end() ? ci->second : 0;
            if (!pi)
            {
                // No band ever set this PChannel: the synth's power-on
                // instrument, as General MIDI has it -- the drum kit on MIDI
                // channel 10 (PChannel % 16 == 9), program 0 elsewhere.
                // NOLFOrch's drum fills on PChannel 73 are exactly that.
                std::map<std::string, CLTDLSCollection*>::iterator gm = m_DLS.find("");
                if (gm != m_DLS.end() && gm->second)
                {
                    const uint32_t nPatch = (e.m_nPChannel % 16 == 9) ? 0x80000000u : 0u;
                    if ((pi = gm->second->Find(nPatch, false)) != 0)
                    {
                        m_Synth.SetChannel(e.m_nPChannel, pi, gm->second, -1, -1, 0);
                        m_ChannelInst[e.m_nPChannel] = pi;
                        ++m_Stats.m_nGMDefaults;
                    }
                }
            }
            ChannelRange& cr = m_Ranges[e.m_nPChannel];
            if (!cr.m_nNotes) { cr.m_nLo = 127; cr.m_nHi = 0; cr.m_nRegLo = 127; cr.m_nRegHi = 0; cr.m_nMissed = 0; }
            ++cr.m_nNotes;
            if (n < cr.m_nLo) cr.m_nLo = n;
            if (n > cr.m_nHi) cr.m_nHi = n;
            if (!pi) { ++m_Stats.m_nNoInstrument; ++cr.m_nMissed; }
            else
            {
                if (cr.m_sInst != pi->m_sName)
                {
                    cr.m_sInst = pi->m_sName;
                    for (size_t r = 0; r < pi->m_Regions.size(); ++r)
                    {
                        if (pi->m_Regions[r].m_nKeyLo < cr.m_nRegLo) cr.m_nRegLo = pi->m_Regions[r].m_nKeyLo;
                        if (pi->m_Regions[r].m_nKeyHi > cr.m_nRegHi) cr.m_nRegHi = pi->m_Regions[r].m_nKeyHi;
                    }
                }
                if (!pi->FindRegion(n, e.m_nB)) { ++m_Stats.m_nNoRegion; ++cr.m_nMissed; }
            }
            m_Synth.NoteOn(e.m_nPChannel, n, e.m_nB, e.m_nTag);
            break;
        }
        case Event::kNoteOff:
        {
            std::map<uint64_t, int>::iterator it = m_Sounding.find(nKey);
            int n = e.m_nA;
            if (it != m_Sounding.end()) { n = it->second; m_Sounding.erase(it); }
            m_Synth.NoteOff(e.m_nPChannel, n, e.m_nTag);
            break;
        }
        case Event::kCC:
            m_Synth.Controller(e.m_nPChannel, e.m_nA, e.m_nB);
            break;
        case Event::kBend:
            m_Synth.PitchBend(e.m_nPChannel, e.m_nB < 0 ? 0 : (e.m_nB > 16383 ? 16383 : e.m_nB));
            break;
        case Event::kStopTag:
            m_Synth.ReleaseTag(e.m_nTag);
            break;
        default:
            break;
    }
}

void CLTDMPerformance::Render(float* pLR, unsigned int nFrames)
{
    if (!pLR || !nFrames) return;
    std::lock_guard<std::recursive_mutex> g(m_Lock);

    static thread_local std::vector<float> s_Mix;
    if (s_Mix.size() < (size_t)nFrames * 2) s_Mix.resize((size_t)nFrames * 2);
    float* pMix = &s_Mix[0];
    memset(pMix, 0, sizeof(float) * nFrames * 2);

    if (m_bPaused) m_Synth.Render(pMix, nFrames);      // release tails only
    else
    {
        unsigned int nPos = 0;
        std::vector<std::pair<Notify, unsigned int> > notes;
        while (nPos < nFrames)
        {
            while (!m_Events.empty() && m_Events.front().m_fTick <= m_fTick + 1e-6)
            {
                std::pop_heap(m_Events.begin(), m_Events.end(), Later());
                const Event e = m_Events.back();
                m_Events.pop_back();
                Dispatch(e);
            }

            // DirectMusic's notifications. Almost-end comes half a beat (at
            // most a second) early: enough for a queued segment to land on
            // the end, and inside the last beat so a BEAT or MEASURE change
            // made in answer still resolves to the end.
            notes.clear();
            const double fTPS = m_fTempo * kLTDMPPQ / 60.0;
            for (size_t i = 0; i < m_States.size(); )
            {
                State& s = m_States[i];
                if (!s.m_bStartSent && m_fTick >= s.m_fStart && s.m_fStart < s.m_fEnd)
                { s.m_bStartSent = true; notes.push_back(std::make_pair(kNotifyStart, s.m_nId)); }
                const double fEarly = std::min(fTPS, s.m_TimeSig.BeatTicks() * 0.5);
                if (!s.m_bAlmostSent && m_fTick >= std::max(s.m_fStart, s.m_fEnd - fEarly))
                { s.m_bAlmostSent = true; notes.push_back(std::make_pair(kNotifyAlmostEnd, s.m_nId)); }
                if (m_fTick >= s.m_fEnd)
                {
                    notes.push_back(std::make_pair(kNotifyEnd, s.m_nId));
                    m_States.erase(m_States.begin() + i);
                    continue;
                }
                ++i;
            }
            if (m_pfnNotify)
                for (size_t i = 0; i < notes.size(); ++i) m_pfnNotify(m_pNotifyUser, notes[i].first, notes[i].second);

            const double fTPF = m_fTempo * kLTDMPPQ / (60.0 * m_nRate);
            unsigned int nStep = std::min(nFrames - nPos, 256u);
            if (!m_Events.empty())
            {
                const double dt = m_Events.front().m_fTick - m_fTick;
                const double f = ceil(dt / fTPF);
                if (f < (double)nStep) nStep = f < 1.0 ? 1u : (unsigned int)f;
            }
            m_Synth.Render(pMix + (size_t)nPos * 2, nStep);
            m_fTick += nStep * fTPF;
            nPos += nStep;
        }
    }

    // The master volume, ramped across the block.
    const float g0 = m_fGain, g1 = m_fTargetGain;
    const float dg = (g1 - g0) / nFrames;
    for (unsigned int f = 0; f < nFrames; ++f)
    {
        const float gg = g0 + dg * (f + 1);
        pLR[f * 2]     += pMix[f * 2] * gg;
        pLR[f * 2 + 1] += pMix[f * 2 + 1] * gg;
    }
    m_fGain = g1;
}
