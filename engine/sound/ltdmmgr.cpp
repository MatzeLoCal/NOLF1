// ------------------------------------------------------------------------- //
//
// ltdmmgr.cpp -- LithTech's music manager over CLTDMPerformance
// (HANDOVER 5cj). See ltdmmgr.h.
//
// The intensity / transition / command-queue logic is the donor's, step for
// step (Jupiter EX ltdirectmusic_impl.cpp: ChangeIntensity, the SEGALMOSTEND
// handler, PlaySecondary, PlayMotif), with two donor hangs NOT carried over:
//   * a queued ChangeIntensity that fails left the command pointer where it
//     was and spun forever -- here it moves on;
//   * StopMotif / StopSecondary with NextSegment never removed the state from
//     the list it was looping over -- here the loop always advances.
//
// ------------------------------------------------------------------------- //

#include "ltdmmgr.h"

#include <chrono>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace
{
    bool LTDMLog()
    {
        static int s = -1;
        if (s < 0) s = getenv("LT_DMTRACE") ? 1 : 0;
        return s == 1;
    }

    // ---- the control file (the donor's CControlFileMgr grammar) -------- //
    //
    // Whitespace is " =,\t\r"; ';' starting a token comments out the rest of
    // the line; '#ifdef / #ifndef / #else / #endif NAME' as in C; '[name]'
    // opens a section. GetKey(NULL, ...) -- all the manager ever asks --
    // searches the first, unnamed section only.
    struct CFKey { std::string m_sName; std::vector<std::string> m_Words; };

    bool IsWS(char c) { return c == ' ' || c == '=' || c == ',' || c == '\t' || c == '\r'; }

    std::vector<CFKey> ParseControlFile(const std::vector<unsigned char>& data,
                                        const std::vector<std::string>& defines)
    {
        std::vector<CFKey> keys;
        std::vector<int> stack;             // 0 = taking lines, 1 = skipping
        int nSkip = 0;
        bool bFirstSection = true;
        size_t i = 0;
        const size_t n = data.size();
        while (i < n)
        {
            size_t e = i;
            while (e < n && data[e] != '\n') ++e;
            std::vector<std::string> tok;
            size_t p = i;
            while (p < e)
            {
                while (p < e && IsWS((char)data[p])) ++p;
                if (p >= e) break;
                const size_t s = p;
                while (p < e && !IsWS((char)data[p])) ++p;
                tok.push_back(std::string((const char*)&data[s], p - s));
            }
            i = e + 1;
            if (tok.empty() || tok[0][0] == ';') continue;

            if (tok[0][0] == '#')
            {
                // "#ifdef X" or "# ifdef X" both reach here as tokens.
                std::string cmd = tok[0].substr(1), arg;
                size_t a = 1;
                if (cmd.empty() && tok.size() > 1) { cmd = tok[1]; a = 2; }
                if (tok.size() > a) arg = tok[a];
                bool bDef = false;
                for (size_t d = 0; d < defines.size(); ++d) if (!strcasecmp(defines[d].c_str(), arg.c_str())) bDef = true;
                if (!strcasecmp(cmd.c_str(), "ifdef") || !strcasecmp(cmd.c_str(), "ifndef"))
                {
                    const bool bTake = !strcasecmp(cmd.c_str(), "ifdef") ? bDef : !bDef;
                    stack.push_back(bTake ? 0 : 1);
                    if (!bTake) ++nSkip;
                }
                else if (!strcasecmp(cmd.c_str(), "else") && !stack.empty())
                {
                    if (stack.back()) { stack.back() = 0; --nSkip; }
                    else { stack.back() = 1; ++nSkip; }
                }
                else if (!strcasecmp(cmd.c_str(), "endif") && !stack.empty())
                {
                    if (stack.back()) --nSkip;
                    stack.pop_back();
                }
                continue;
            }
            if (nSkip > 0) continue;
            if (tok[0][0] == '[') { bFirstSection = false; continue; }
            if (!bFirstSection) continue;

            CFKey k;
            k.m_sName = tok[0];
            for (size_t w = 1; w < tok.size(); ++w)
            {
                if (tok[w][0] == ';') break;
                k.m_Words.push_back(tok[w]);
            }
            keys.push_back(k);
        }
        return keys;
    }

    const CFKey* FirstKey(const std::vector<CFKey>& keys, const char* pName)
    {
        for (size_t i = 0; i < keys.size(); ++i) if (!strcasecmp(keys[i].m_sName.c_str(), pName)) return &keys[i];
        return 0;
    }

    template <class T> void KeyVal(const std::vector<CFKey>& keys, const char* pName, T& v)
    {
        const CFKey* k = FirstKey(keys, pName);
        if (k && !k->m_Words.empty()) v = (T)atof(k->m_Words[0].c_str());
    }

    bool ReadViaFileMgr(const char* pPath, std::vector<unsigned char>& out, void*);
}

// The engine's reader is supplied by the host (skeleton) -- this file stays
// free of the file manager so the offline tools can link it.
static CLTDMPerformance::ReadFn g_pfnDefaultRead = 0;
void LTDM_SetDefaultReader(CLTDMPerformance::ReadFn fn) { g_pfnDefaultRead = fn; }

namespace
{
    bool ReadViaFileMgr(const char* pPath, std::vector<unsigned char>& out, void* pUser)
    {
        return g_pfnDefaultRead ? g_pfnDefaultRead(pPath, out, pUser) : false;
    }
}

// --------------------------------------------------------------------------- //

CLTDMMusicMgr::CLTDMMusicMgr()
    : m_Perf(44100), m_bInitialized(false), m_bLevelInitialized(false),
      m_nNumIntensities(0), m_nInitialIntensity(0), m_nInitialVolume(0), m_nVolumeOffset(0),
      m_nCurIntensity(0), m_nPrevIntensity(0), m_nPaused(0), m_nLast(-1),
      m_bRun(false), m_bPulling(false), m_pfnActive(0), m_nWrite(0), m_nRead(0)
{
    m_DefaultTransition.m_eEnact = LTDMEnactNextMeasure;
    m_DefaultTransition.m_bManual = true;
    m_DefaultTransition.m_pSeg = 0;
    memset(m_Ring, 0, sizeof(m_Ring));
    m_Perf.SetNotify(&CLTDMMusicMgr::NotifyThunk, this);
}

CLTDMMusicMgr::~CLTDMMusicMgr()
{
    StopThread();
}

void CLTDMMusicMgr::NotifyThunk(void* pUser, CLTDMPerformance::Notify e, unsigned int nId)
{
    static_cast<CLTDMMusicMgr*>(pUser)->OnNotify(e, nId);
}

CLTDMPerformance::Enact CLTDMMusicMgr::ToEnact(LTDMEnactTypes e) const
{
    switch (e)
    {
        case LTDMEnactImmediatly:   return CLTDMPerformance::kImmediate;
        case LTDMEnactNextBeat:     return CLTDMPerformance::kBeat;
        case LTDMEnactNextMeasure:  return CLTDMPerformance::kMeasure;
        case LTDMEnactNextGrid:     return CLTDMPerformance::kGrid;
        case LTDMEnactNextSegment:  return CLTDMPerformance::kSegmentEnd;
        case LTDMEnactDefault:      return CLTDMPerformance::kDefault;
        default:                    return CLTDMPerformance::kImmediate;
    }
}

// ---- lifetime -------------------------------------------------------------- //

LTRESULT CLTDMMusicMgr::Init()
{
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    if (m_bInitialized) return LT_ERROR;
    m_Perf.SetReader(&ReadViaFileMgr, 0);
    m_bInitialized = true;
    if (LTDMLog()) fprintf(stderr, "[LTDM] Init\n");
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::Term()
{
    if (!m_bInitialized) return LT_ERROR;
    TermLevel();
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    m_bInitialized = false;
    return LT_OK;
}

CLTDMMusicMgr::Named* CLTDMMusicMgr::FindSegment(const char* pName)
{
    if (!pName) return 0;
    for (size_t i = 0; i < m_Segments.size(); ++i)
        if (!strcasecmp(m_Segments[i]->m_sName.c_str(), pName)) return m_Segments[i];
    return 0;
}

CLTDMMusicMgr::Named* CLTDMMusicMgr::FindMotif(const char* pName)
{
    if (!pName) return 0;
    for (size_t i = 0; i < m_MotifList.size(); ++i)
        if (!strcasecmp(m_MotifList[i]->m_sName.c_str(), pName)) return m_MotifList[i];
    return 0;
}

LTRESULT CLTDMMusicMgr::InitLevel(const char* sWorkingDirectory, const char* sControlFileName,
                                  const char* sDefine1, const char* sDefine2, const char* sDefine3)
{
    if (!m_bInitialized) return LT_ERROR;
    if (m_bLevelInitialized) return LT_ERROR;
    if (!sControlFileName || !sWorkingDirectory) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());

    m_Perf.SetDirectory(sWorkingDirectory);
    std::string sPath = sControlFileName;
    if (!strchr(sControlFileName, '\\') && !strchr(sControlFileName, '/') && !strchr(sControlFileName, ':'))
    {
        sPath = sWorkingDirectory;
        if (!sPath.empty() && sPath[sPath.size() - 1] != '\\' && sPath[sPath.size() - 1] != '/') sPath += '\\';
        sPath += sControlFileName;
    }
    std::vector<unsigned char> data;
    if (!ReadViaFileMgr(sPath.c_str(), data, 0) || data.empty())
    {
        fprintf(stderr, "[LTDM] InitLevel: control file not found: %s\n", sPath.c_str());
        return LT_ERROR;
    }
    std::vector<std::string> defines;
    if (sDefine1) defines.push_back(sDefine1);
    if (sDefine2) defines.push_back(sDefine2);
    if (sDefine3) defines.push_back(sDefine3);
    const std::vector<CFKey> keys = ParseControlFile(data, defines);

    m_nNumIntensities = 0; m_nInitialIntensity = 0; m_nInitialVolume = 0; m_nVolumeOffset = 0;
    int nVoices = 64;
    KeyVal(keys, "NUMINTENSITIES", m_nNumIntensities);
    KeyVal(keys, "INITIALINTENSITY", m_nInitialIntensity);
    KeyVal(keys, "INITIALVOLUME", m_nInitialVolume);
    KeyVal(keys, "VOLUMEOFFSET", m_nVolumeOffset);
    KeyVal(keys, "VOICES", nVoices);

    // Reverb: the donor's defaults, then the file's.
    bool bReverb = false;
    float fInGain = 0.0f, fHF = 0.001f, fMix = -10.0f, fTime = 1000.0f;
    if (const CFKey* k = FirstKey(keys, "REVERB"))
        if (!k->m_Words.empty() && !strcasecmp(k->m_Words[0].c_str(), "ON")) bReverb = true;
    KeyVal(keys, "REVERBINGAIN", fInGain);
    KeyVal(keys, "REVERBHIGHFREQRTRATIO", fHF);
    KeyVal(keys, "REVERBMIX", fMix);
    KeyVal(keys, "REVERBTIME", fTime);
    m_Perf.Synth().SetReverb(bReverb, fInGain, fMix, fTime, fHF);
    m_Perf.Synth().SetMaxVoices(nVoices);
    // The GM set, for band instruments naming no collection and for PChannels
    // no band sets (loaded here: the music thread must not read files).
    m_Perf.GetDLS("");

    if (m_nNumIntensities < 1) { fprintf(stderr, "[LTDM] InitLevel: no intensities in %s\n", sPath.c_str()); return LT_ERROR; }
    m_Intensities.assign(m_nNumIntensities + 1, Intensity());
    m_Transitions.assign((m_nNumIntensities + 1) * (m_nNumIntensities + 1), m_DefaultTransition);

    // DLS banks, then styles and their bands (each named band is played once,
    // now, as the donor's LoadBand does).
    for (size_t i = 0; i < keys.size(); ++i)
        if (!strcasecmp(keys[i].m_sName.c_str(), "DLSBANK") && !keys[i].m_Words.empty())
            m_Perf.GetDLS(keys[i].m_Words[0].c_str());
    for (size_t i = 0; i < keys.size(); ++i)
    {
        if (strcasecmp(keys[i].m_sName.c_str(), "STYLE") || keys[i].m_Words.empty()) continue;
        const std::string& sStyle = keys[i].m_Words[0];
        const LTDMStyle* pStyle = m_Perf.GetStyle(sStyle.c_str());
        if (!pStyle) continue;
        for (size_t j = 0; j < keys.size(); ++j)
        {
            if (strcasecmp(keys[j].m_sName.c_str(), "BAND") || keys[j].m_Words.size() < 2) continue;
            if (strcasecmp(keys[j].m_Words[0].c_str(), sStyle.c_str())) continue;
            for (size_t b = 0; b < pStyle->m_Bands.size(); ++b)
                if (!strcasecmp(pStyle->m_Bands[b].m_sName.c_str(), keys[j].m_Words[1].c_str()))
                {
                    m_Perf.PrepareBand(pStyle->m_Bands[b]);
                    m_Perf.PlayBandNow(pStyle->m_Bands[b]);
                }
        }
    }

    // Segments by name, loaded once however often they are named.
    struct Load
    {
        static Named* Seg(CLTDMMusicMgr& m, const std::string& s)
        {
            if (Named* p = m.FindSegment(s.c_str())) return p->m_pSeg ? p : 0;
            Named* p = new Named;
            p->m_sName = s;
            p->m_pSeg = m.m_Perf.GetSegment(s.c_str());
            p->m_pStyle = 0; p->m_pPattern = 0;
            p->m_eEnact = LTDMEnactNextMeasure;
            m.m_Segments.push_back(p);
            return p->m_pSeg ? p : 0;
        }
    };

    for (size_t i = 0; i < keys.size(); ++i)
    {
        const CFKey& k = keys[i];
        if (!strcasecmp(k.m_sName.c_str(), "INTENSITY"))
        {
            if (k.m_Words.size() < 4) continue;
            const int n = atoi(k.m_Words[0].c_str());
            if (n <= 0 || n > m_nNumIntensities) continue;
            Intensity& in = m_Intensities[n];
            for (size_t w = 3; w < k.m_Words.size(); ++w)
            {
                Named* p = Load::Seg(*this, k.m_Words[w]);
                if (!p) break;
                in.m_Segs.push_back(p->m_pSeg);
            }
            in.m_nLoops = atoi(k.m_Words[1].c_str());
            in.m_nNext = atoi(k.m_Words[2].c_str());
        }
        else if (!strcasecmp(k.m_sName.c_str(), "SECONDARYSEGMENT"))
        {
            if (k.m_Words.empty()) continue;
            if (Named* p = Load::Seg(*this, k.m_Words[0]))
                if (k.m_Words.size() > 1) p->m_eEnact = StringToEnactType(k.m_Words[1].c_str());
        }
        else if (!strcasecmp(k.m_sName.c_str(), "MOTIF"))
        {
            if (k.m_Words.size() < 2) continue;
            const LTDMStyle* pStyle = m_Perf.GetStyle(k.m_Words[0].c_str());
            if (!pStyle) continue;
            const LTDMPattern* pPat = 0;
            for (size_t p = 0; p < pStyle->m_Patterns.size() && !pPat; ++p)
                if ((pStyle->m_Patterns[p].m_nEmbellishment & 16)
                    && !strcasecmp(pStyle->m_Patterns[p].m_sName.c_str(), k.m_Words[1].c_str()))
                    pPat = &pStyle->m_Patterns[p];
            if (!pPat) { fprintf(stderr, "[LTDM] motif not in %s: %s\n", k.m_Words[0].c_str(), k.m_Words[1].c_str()); continue; }
            Named* m = new Named;
            m->m_sName = k.m_Words[1];
            m->m_pSeg = 0;
            m->m_pStyle = pStyle;
            m->m_pPattern = pPat;
            m->m_eEnact = k.m_Words.size() > 2 ? StringToEnactType(k.m_Words[2].c_str()) : LTDMEnactNextMeasure;
            m_MotifList.push_back(m);
        }
        else if (!strcasecmp(k.m_sName.c_str(), "TRANSITION"))
        {
            if (k.m_Words.size() < 4) continue;
            const int nFrom = atoi(k.m_Words[0].c_str()), nTo = atoi(k.m_Words[1].c_str());
            if (nFrom <= 0 || nFrom > m_nNumIntensities || nTo <= 0 || nTo > m_nNumIntensities) continue;
            const LTDMEnactTypes e = StringToEnactType(k.m_Words[2].c_str());
            if (e == LTDMEnactInvalid) continue;
            bool bManual;
            if (!strcasecmp(k.m_Words[3].c_str(), "MANUAL")) bManual = true;
            else if (!strcasecmp(k.m_Words[3].c_str(), "AUTOMATIC")) bManual = false;
            else continue;
            Transition& t = m_Transitions[nFrom * (m_nNumIntensities + 1) + nTo];
            if (k.m_Words.size() > 4)
            {
                Named* p = Load::Seg(*this, k.m_Words[4]);
                t.m_pSeg = p ? p->m_pSeg : 0;
            }
            t.m_eEnact = e;
            t.m_bManual = bManual;
        }
    }

    m_bLevelInitialized = true;
    SetVolume(m_nInitialVolume);
    m_nLast = -1;
    m_nCurIntensity = 0;
    m_nPrevIntensity = 0;
    if (LTDMLog())
        fprintf(stderr, "[LTDM] InitLevel %s: %d intensities, %zu segments, %zu motifs, reverb %s\n",
                sPath.c_str(), m_nNumIntensities, m_Segments.size(), m_MotifList.size(), bReverb ? "on" : "off");
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::TermLevel()
{
    if (!m_bInitialized) return LT_ERROR;
    if (!m_bLevelInitialized) return LT_OK;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    ClearCommands();
    Stop(LTDMEnactImmediatly);
    m_Perf.Reset();
    for (size_t i = 0; i < m_Segments.size(); ++i) delete m_Segments[i];
    for (size_t i = 0; i < m_MotifList.size(); ++i) delete m_MotifList[i];
    m_Segments.clear();
    m_MotifList.clear();
    m_Intensities.clear();
    m_Transitions.clear();
    m_Primaries.clear();
    m_Secondaries.clear();
    m_Motifs.clear();
    m_nPaused = 0;
    m_Perf.SetPaused(false);
    m_bLevelInitialized = false;
    return LT_OK;
}

// ---- playing --------------------------------------------------------------- //

void CLTDMMusicMgr::ClearCommands()
{
    m_Cmds.clear();
    m_Cmds2.clear();
}

const CLTDMMusicMgr::Transition& CLTDMMusicMgr::GetTransition(int nFrom, int nTo) const
{
    if (nFrom <= 0 || nTo <= 0 || nFrom > m_nNumIntensities || nTo > m_nNumIntensities) return m_DefaultTransition;
    return m_Transitions[nFrom * (m_nNumIntensities + 1) + nTo];
}

LTRESULT CLTDMMusicMgr::Play()
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    return ChangeIntensity(m_nInitialIntensity, LTDMEnactImmediatly);
}

LTRESULT CLTDMMusicMgr::Stop(const LTDMEnactTypes nStart)
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    if (nStart != LTDMEnactNextSegment) m_Perf.StopAll(ToEnact(nStart));
    while (m_nPaused) UnPause();
    ClearCommands();
    m_Primaries.clear();
    m_Secondaries.clear();
    m_Motifs.clear();
    m_nLast = -1;
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::Pause(const LTDMEnactTypes /*nStart*/)
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    if (m_nPaused == 0) m_Perf.SetPaused(true);
    ++m_nPaused;
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::UnPause()
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    if (!m_nPaused) return LT_OVERFLOW;
    if (--m_nPaused == 0) m_Perf.SetPaused(false);
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::SetVolume(const long nVolume)
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    // GUID_PerfMasterVolume: hundredths of a dB, DirectMusic's range.
    long v = nVolume + m_nVolumeOffset;
    if (v > 2000) v = 2000;
    if (v < -10000) v = -10000;
    m_Perf.SetVolumeDB(v <= -10000 ? -200.0f : v / 100.0f);
    if (LTDMLog()) fprintf(stderr, "[LTDM] SetVolume %ld -> %.2f dB\n", nVolume, v / 100.0f);
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::ChangeIntensity(const int nNewIntensity, const LTDMEnactTypes nStart)
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    if (nNewIntensity == 0) { Stop(nStart); return LT_ERROR; }
    if (nNewIntensity < 0 || nNewIntensity > m_nNumIntensities) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    if (m_nCurIntensity == nNewIntensity) return LT_OK;

    // A change before the last one is heard takes the transition from the
    // intensity still sounding.
    const Transition& tr = GetTransition(m_nPrevIntensity ? m_nPrevIntensity : m_nCurIntensity, nNewIntensity);
    m_nPrevIntensity = m_nCurIntensity;

    ClearCommands();
    const LTDMEnactTypes eEnact = nStart == LTDMEnactInvalid ? tr.m_eEnact : nStart;
    const Intensity& in = m_Intensities[nNewIntensity];
    bool bSkipFirst = false;
    if (tr.m_pSeg)
    {
        if (unsigned int id = m_Perf.PlayPrimary(tr.m_pSeg, ToEnact(eEnact))) m_Primaries.push_back(id);
    }
    else if (!in.m_Segs.empty())
    {
        if (unsigned int id = m_Perf.PlayPrimary(in.m_Segs[0], ToEnact(eEnact))) m_Primaries.push_back(id);
        bSkipFirst = true;
    }
    for (size_t i = 0; i < in.m_Segs.size(); ++i)
    {
        Command c = { kCmdPlaySegment, in.m_Segs[i], 0, 0 };
        m_Cmds.push_back(c);
    }
    m_nLast = (bSkipFirst && !m_Cmds.empty()) ? 0 : -1;

    Command loop = { kCmdLoopToStart, 0, in.m_nNext == nNewIntensity ? -1 : in.m_nLoops, 0 };
    m_Cmds.push_back(loop);
    if (in.m_nNext > 0)
    {
        Command ci = { kCmdChangeIntensity, 0, in.m_nNext, 0 };
        m_Cmds.push_back(ci);
    }
    m_nCurIntensity = nNewIntensity;
    if (LTDMLog())
        fprintf(stderr, "[LTDM] ChangeIntensity %d (from %d, enact %d, transition %s) -> %zu commands\n",
                nNewIntensity, m_nPrevIntensity, (int)eEnact, tr.m_pSeg ? tr.m_pSeg->m_sName.c_str() : "none", m_Cmds.size());
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::PlaySecondary(const char* sSecondarySegment, const LTDMEnactTypes nStart)
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    Named* p = FindSegment(sSecondarySegment);
    if (!p || !p->m_pSeg) return LT_ERROR;
    const LTDMEnactTypes e = nStart == LTDMEnactDefault ? p->m_eEnact : nStart;
    if (e != LTDMEnactNextSegment)
    {
        if (unsigned int id = m_Perf.PlaySecondary(p->m_pSeg, ToEnact(e)))
        { Playing pl = { id, p }; m_Secondaries.push_back(pl); }
    }
    else
    {
        Command c = { kCmdPlaySecondary, p->m_pSeg, 0, p };
        m_Cmds2.insert(m_Cmds2.begin(), c);
    }
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::StopSecondary(const char* sSecondarySegment, const LTDMEnactTypes nStart)
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    for (size_t i = 0; i < m_Secondaries.size(); )
    {
        Playing& pl = m_Secondaries[i];
        if (sSecondarySegment && strcasecmp(pl.m_pNamed->m_sName.c_str(), sSecondarySegment)) { ++i; continue; }
        const LTDMEnactTypes e = nStart == LTDMEnactDefault ? pl.m_pNamed->m_eEnact : nStart;
        if (e != LTDMEnactNextSegment) m_Perf.StopState(pl.m_nId, ToEnact(e));
        m_Secondaries.erase(m_Secondaries.begin() + i);
    }
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::PlayMotif(const char* /*sStyleName*/, const char* sMotifName, const LTDMEnactTypes nStart)
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    // The donor ignores the style name too: motifs are found by name.
    Named* p = FindMotif(sMotifName);
    if (!p) return LT_ERROR;
    const LTDMEnactTypes e = nStart == LTDMEnactDefault ? p->m_eEnact : nStart;
    if (e != LTDMEnactNextSegment)
    {
        if (unsigned int id = m_Perf.PlayMotif(p->m_pStyle, p->m_pPattern, ToEnact(e)))
        { Playing pl = { id, p }; m_Motifs.push_back(pl); }
    }
    else
    {
        Command c = { kCmdPlayMotif, 0, 0, p };
        m_Cmds2.insert(m_Cmds2.begin(), c);
    }
    if (LTDMLog()) fprintf(stderr, "[LTDM] PlayMotif %s enact %d\n", sMotifName, (int)e);
    return LT_OK;
}

LTRESULT CLTDMMusicMgr::StopMotif(const char* /*sStyleName*/, const char* sMotifName, const LTDMEnactTypes nStart)
{
    if (!m_bInitialized || !m_bLevelInitialized) return LT_ERROR;
    std::lock_guard<std::recursive_mutex> g(m_Perf.Lock());
    for (size_t i = 0; i < m_Motifs.size(); )
    {
        Playing& pl = m_Motifs[i];
        if (sMotifName && strcasecmp(pl.m_pNamed->m_sName.c_str(), sMotifName)) { ++i; continue; }
        const LTDMEnactTypes e = nStart == LTDMEnactDefault ? pl.m_pNamed->m_eEnact : nStart;
        if (e != LTDMEnactNextSegment) m_Perf.StopState(pl.m_nId, ToEnact(e));
        m_Motifs.erase(m_Motifs.begin() + i);
    }
    return LT_OK;
}

// ---- notifications (called from Render, lock held) ------------------------- //

void CLTDMMusicMgr::OnNotify(CLTDMPerformance::Notify e, unsigned int nId)
{
    switch (e)
    {
        case CLTDMPerformance::kNotifyStart:
            m_nPrevIntensity = 0;
            break;
        case CLTDMPerformance::kNotifyAlmostEnd:
            OnAlmostEnd(nId);
            break;
        case CLTDMPerformance::kNotifyEnd:
            for (size_t i = 0; i < m_Primaries.size(); ++i)
                if (m_Primaries[i] == nId) { m_Primaries.erase(m_Primaries.begin() + i); return; }
            for (size_t i = 0; i < m_Secondaries.size(); ++i)
                if (m_Secondaries[i].m_nId == nId) { m_Secondaries.erase(m_Secondaries.begin() + i); return; }
            for (size_t i = 0; i < m_Motifs.size(); ++i)
                if (m_Motifs[i].m_nId == nId) { m_Motifs.erase(m_Motifs.begin() + i); return; }
            break;
    }
}

void CLTDMMusicMgr::OnAlmostEnd(unsigned int nId)
{
    for (size_t i = 0; i < m_Secondaries.size(); ++i) if (m_Secondaries[i].m_nId == nId) return;
    for (size_t i = 0; i < m_Motifs.size(); ++i) if (m_Motifs[i].m_nId == nId) return;
    // Only the newest primary runs the queue.
    if (!m_Primaries.empty() && m_Primaries.back() != nId) return;

    int cur = m_nLast >= 0 ? m_nLast + 1 : 0;
    bool bDone = false;
    while (!bDone && cur >= 0 && cur < (int)m_Cmds.size())
    {
        Command& c = m_Cmds[cur];
        switch (c.m_eKind)
        {
            case kCmdLoopToStart:
                if (c.m_n == 0) { ++cur; break; }
                if (c.m_n > 0) --c.m_n;
                if (c.m_n == 0) ++cur;
                else cur = 0;
                break;
            case kCmdChangeIntensity:
            {
                const int nTo = c.m_n;
                if (ChangeIntensity(nTo) == LT_OK) { cur = m_nLast; bDone = true; }
                else ++cur;     // the donor stayed here forever
                break;
            }
            case kCmdPlaySegment:
                if (unsigned int id = m_Perf.PlayPrimary(c.m_pSeg, CLTDMPerformance::kSegmentEnd)) m_Primaries.push_back(id);
                bDone = true;
                break;
            default:
                ++cur;
                break;
        }
    }
    m_nLast = (cur >= 0 && cur < (int)m_Cmds.size()) ? cur : -1;

    // Then whatever was waiting for the next segment.
    for (size_t i = 0; i < m_Cmds2.size(); ++i)
    {
        const Command& c = m_Cmds2[i];
        if (c.m_eKind == kCmdPlaySecondary && c.m_pNamed && c.m_pNamed->m_pSeg)
        {
            if (unsigned int id = m_Perf.PlaySecondary(c.m_pNamed->m_pSeg, CLTDMPerformance::kMeasure))
            { Playing pl = { id, c.m_pNamed }; m_Secondaries.push_back(pl); }
        }
        else if (c.m_eKind == kCmdPlayMotif && c.m_pNamed)
        {
            if (unsigned int id = m_Perf.PlayMotif(c.m_pNamed->m_pStyle, c.m_pNamed->m_pPattern, CLTDMPerformance::kMeasure))
            { Playing pl = { id, c.m_pNamed }; m_Motifs.push_back(pl); }
        }
    }
    m_Cmds2.clear();
}

// ---- small things ---------------------------------------------------------- //

int  CLTDMMusicMgr::GetCurIntensity()       { return m_nCurIntensity; }
int  CLTDMMusicMgr::GetNumIntensities()     { return m_nNumIntensities; }
int  CLTDMMusicMgr::GetInitialIntensity()   { return m_nInitialIntensity; }
long CLTDMMusicMgr::GetInitialVolume()      { return m_nInitialVolume; }

LTDMEnactTypes CLTDMMusicMgr::StringToEnactType(const char* s)
{
    if (!s) return LTDMEnactInvalid;
    if (!strcasecmp(s, "Invalid")) return LTDMEnactInvalid;
    if (!strcasecmp(s, "Default")) return LTDMEnactDefault;
    if (!strcasecmp(s, "Immediatly") || !strcasecmp(s, "Immediately") || !strcasecmp(s, "Immediate")) return LTDMEnactImmediatly;
    if (!strcasecmp(s, "NextBeat") || !strcasecmp(s, "Beat")) return LTDMEnactNextBeat;
    if (!strcasecmp(s, "NextMeasure") || !strcasecmp(s, "Measure")) return LTDMEnactNextMeasure;
    if (!strcasecmp(s, "NextGrid") || !strcasecmp(s, "Grid")) return LTDMEnactNextGrid;
    if (!strcasecmp(s, "NextSegment") || !strcasecmp(s, "Segment")) return LTDMEnactNextSegment;
    return LTDMEnactInvalid;
}

void CLTDMMusicMgr::EnactTypeToString(LTDMEnactTypes n, char* s)
{
    if (!s) return;
    switch (n)
    {
        case LTDMEnactInvalid:      strcpy(s, "Invalid"); break;
        case LTDMEnactDefault:      strcpy(s, "Default"); break;
        case LTDMEnactImmediatly:   strcpy(s, "Immediate"); break;
        case LTDMEnactNextBeat:     strcpy(s, "Beat"); break;
        case LTDMEnactNextMeasure:  strcpy(s, "Measure"); break;
        case LTDMEnactNextGrid:     strcpy(s, "Grid"); break;
        case LTDMEnactNextSegment:  strcpy(s, "Segment"); break;
        default:                    strcpy(s, ""); break;
    }
}

// ---- the music thread ------------------------------------------------------ //

bool CLTDMMusicMgr::StartThread(bool (*pfnActive)())
{
    if (m_bRun) return true;
    m_pfnActive = pfnActive;
    m_nWrite = 0;
    m_nRead = 0;
    m_bRun = true;
    m_Thread = std::thread(&CLTDMMusicMgr::ThreadMain, this);
    m_bPulling = true;
    return true;
}

void CLTDMMusicMgr::StopThread()
{
    m_bPulling = false;
    if (!m_bRun) return;
    m_bRun = false;
    if (m_Thread.joinable()) m_Thread.join();
}

void CLTDMMusicMgr::ThreadMain()
{
    float block[kBlockFrames * 2];
    while (m_bRun)
    {
        const uint32_t w = m_nWrite.load(std::memory_order_relaxed);
        const uint32_t r = m_nRead.load(std::memory_order_acquire);
        if ((m_pfnActive && !m_pfnActive()) || kRingFrames - (w - r) < (uint32_t)kBlockFrames)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        memset(block, 0, sizeof(block));
        m_Perf.Render(block, kBlockFrames);
        for (unsigned int f = 0; f < (unsigned int)kBlockFrames; ++f)
        {
            const uint32_t at = (w + f) % kRingFrames;
            m_Ring[at * 2] = block[f * 2];
            m_Ring[at * 2 + 1] = block[f * 2 + 1];
        }
        m_nWrite.store(w + kBlockFrames, std::memory_order_release);
    }
}

void CLTDMMusicMgr::Pull(float* pLR, unsigned int nFrames)
{
    if (!m_bPulling) return;
    const uint32_t r = m_nRead.load(std::memory_order_relaxed);
    const uint32_t w = m_nWrite.load(std::memory_order_acquire);
    uint32_t n = w - r;
    if (n > nFrames) n = nFrames;
    for (uint32_t f = 0; f < n; ++f)
    {
        const uint32_t at = (r + f) % kRingFrames;
        pLR[f * 2] += m_Ring[at * 2];
        pLR[f * 2 + 1] += m_Ring[at * 2 + 1];
    }
    m_nRead.store(r + n, std::memory_order_release);
}

CLTDMMusicMgr* LTDM_GetMusicMgr()
{
    // Never destroyed: the audio callback may still be pulling at exit.
    static CLTDMMusicMgr* s = new CLTDMMusicMgr;
    return s;
}
