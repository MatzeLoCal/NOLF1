// ------------------------------------------------------------------------- //
//
// ltdls.cpp -- see ltdls.h.
//
// ------------------------------------------------------------------------- //

#include "ltdls.h"

#include <math.h>
#include <string.h>

namespace
{
    template <class T> T Rd(const unsigned char* p) { T v; memcpy(&v, p, sizeof(T)); return v; }

    constexpr uint32_t FCC(const char (&s)[5])
    { return (uint32_t)(unsigned char)s[0] | ((uint32_t)(unsigned char)s[1] << 8)
           | ((uint32_t)(unsigned char)s[2] << 16) | ((uint32_t)(unsigned char)s[3] << 24); }

    struct Chunk { uint32_t m_nId, m_nType; const unsigned char* m_p; size_t m_n; };

    std::vector<Chunk> Children(const unsigned char* p, size_t n)
    {
        std::vector<Chunk> out;
        size_t o = 0;
        while (o + 8 <= n)
        {
            Chunk c;
            c.m_nId = Rd<uint32_t>(p + o);
            const uint32_t nSz = Rd<uint32_t>(p + o + 4);
            if (o + 8 + nSz > n) break;
            c.m_nType = 0; c.m_p = p + o + 8; c.m_n = nSz;
            if ((c.m_nId == FCC("RIFF") || c.m_nId == FCC("LIST")) && nSz >= 4)
            { c.m_nType = Rd<uint32_t>(c.m_p); c.m_p += 4; c.m_n -= 4; }
            out.push_back(c);
            o += 8 + nSz + (nSz & 1);
        }
        return out;
    }

    const Chunk* FindChunk(const std::vector<Chunk>& v, uint32_t nId, uint32_t nType = 0)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].m_nId == nId && (nType == 0 || v[i].m_nType == nType)) return &v[i];
        return 0;
    }

    // DLS time cents -> seconds. 0x80000000 is DLS's "zero time".
    float TimeCents(int32_t tc)
    {
        if (tc == (int32_t)0x80000000) return 0.0f;
        return (float)pow(2.0, (double)tc / (1200.0 * 65536.0));
    }

    // wsmp: cbSize, usUnityNote, sFineTune, lAttenuation, fulOptions,
    // cSampleLoops, then the loops (cbSize, type, start, length).
    void ReadWsmp(const Chunk& c, int& nUnity, int& nFine, float& fGain,
                  bool& bLoop, unsigned int& nStart, unsigned int& nLen)
    {
        if (c.m_n < 20) return;
        const uint32_t cb = Rd<uint32_t>(c.m_p);
        nUnity = Rd<uint16_t>(c.m_p + 4);
        nFine  = Rd<int16_t>(c.m_p + 6);
        const int32_t att = Rd<int32_t>(c.m_p + 8);
        // 1/65536 centibel, as a relative gain (negative = quieter).
        fGain = (float)pow(10.0, (double)att / 65536.0 / 200.0);
        const uint32_t nLoops = Rd<uint32_t>(c.m_p + 16);
        bLoop = false;
        if (nLoops > 0 && cb + 16 <= c.m_n)
        {
            const unsigned char* l = c.m_p + cb;
            nStart = Rd<uint32_t>(l + 8);
            nLen   = Rd<uint32_t>(l + 12);
            bLoop  = nLen > 1;
        }
    }
}

LTDLSArt::LTDLSArt()
    : m_fAttack(0.0f), m_fDecay(0.0f), m_fSustain(1.0f), m_fRelease(0.0f),
      m_fVelToAttack(0.0f), m_fKeyToDecay(0.0f),
      m_fAttack2(0.0f), m_fDecay2(0.0f), m_fSustain2(1.0f), m_fRelease2(0.0f),
      m_fKeyToDecay2(0.0f), m_fEG2ToPitch(0.0f),
      m_fLFOFreq(5.0f), m_fLFODelay(0.0f), m_fLFOToPitch(0.0f), m_fLFOToAtten(0.0f),
      m_fModToPitch(0.0f), m_fModToAtten(0.0f),
      m_fPan(0.0f), m_fVelToAtten(960.0f), m_bVelToAttenSet(false)
{
}

void LTDLSArt::Apply(const unsigned char* p, size_t n)
{
    if (n < 8) return;
    const uint32_t cb = Rd<uint32_t>(p), nConn = Rd<uint32_t>(p + 4);
    for (uint32_t i = 0; i < nConn && cb + (i + 1) * 12 <= n; ++i)
    {
        const unsigned char* c = p + cb + i * 12;
        const uint16_t src = Rd<uint16_t>(c), ctl = Rd<uint16_t>(c + 2);
        const uint16_t dst = Rd<uint16_t>(c + 4);
        const int32_t  sc  = Rd<int32_t>(c + 8);
        const float    f   = (float)sc / 65536.0f;
        if (src == 0 && ctl == 0)
        {
            switch (dst)
            {
                case 0x0206: m_fAttack   = TimeCents(sc); break;
                case 0x0207: m_fDecay    = TimeCents(sc); break;
                case 0x0209: m_fRelease  = TimeCents(sc); break;
                case 0x020a: m_fSustain  = f / 1000.0f;   break;
                case 0x030a: m_fAttack2  = TimeCents(sc); break;
                case 0x030b: m_fDecay2   = TimeCents(sc); break;
                case 0x030d: m_fRelease2 = TimeCents(sc); break;
                case 0x030e: m_fSustain2 = f / 1000.0f;   break;
                case 0x0104: m_fLFOFreq  = 440.0f * (float)pow(2.0, (f - 6900.0) / 1200.0); break;
                case 0x0105: m_fLFODelay = TimeCents(sc); break;
                case 0x0004: m_fPan      = f / 1000.0f;   break;
                default: break;     // 0x0500 filter cutoff: not modelled
            }
        }
        else if (src == 0x0002 && dst == 0x0001) { m_fVelToAtten = -f; m_bVelToAttenSet = true; }
        else if (src == 0x0002 && dst == 0x0206) m_fVelToAttack = f;
        else if (src == 0x0003 && dst == 0x0207) m_fKeyToDecay  = f;
        else if (src == 0x0003 && dst == 0x030b) m_fKeyToDecay2 = f;
        else if (src == 0x0005 && dst == 0x0003) m_fEG2ToPitch  = f;
        else if (src == 0x0001 && ctl == 0 && dst == 0x0003)      m_fLFOToPitch = f;
        else if (src == 0x0001 && ctl == 0 && dst == 0x0001)      m_fLFOToAtten = f;
        else if (src == 0x0001 && ctl == 0x0081 && dst == 0x0003) m_fModToPitch = f;
        else if (src == 0x0001 && ctl == 0x0081 && dst == 0x0001) m_fModToAtten = f;
    }
}

const LTDLSRegion* LTDLSInstrument::FindRegion(int nKey, int nVel) const
{
    // Key range decides (DLS 1); a velocity range only where one is authored.
    for (size_t i = 0; i < m_Regions.size(); ++i)
    {
        const LTDLSRegion& r = m_Regions[i];
        if (nKey < r.m_nKeyLo || nKey > r.m_nKeyHi) continue;
        if (r.m_nVelHi > r.m_nVelLo && (nVel < r.m_nVelLo || nVel > r.m_nVelHi)) continue;
        return &r;
    }
    return 0;
}

bool CLTDLSCollection::Load(const unsigned char* p, size_t n)
{
    std::vector<Chunk> top = Children(p, n);
    const Chunk* riff = FindChunk(top, FCC("RIFF"), FCC("DLS "));
    if (!riff) return false;
    std::vector<Chunk> k = Children(riff->m_p, riff->m_n);

    // ---- the wave pool, by cue index ------------------------------------- //
    const Chunk* wvpl = FindChunk(k, FCC("LIST"), FCC("wvpl"));
    const Chunk* ptbl = FindChunk(k, FCC("ptbl"));
    std::vector<const unsigned char*> waves;     // each a LIST 'wave' header
    if (wvpl)
    {
        if (ptbl && ptbl->m_n >= 8)
        {
            const uint32_t cb = Rd<uint32_t>(ptbl->m_p), nCues = Rd<uint32_t>(ptbl->m_p + 4);
            for (uint32_t i = 0; i < nCues && cb + (i + 1) * 4 <= ptbl->m_n; ++i)
            {
                const uint32_t o = Rd<uint32_t>(ptbl->m_p + cb + i * 4);
                waves.push_back(o + 12 <= wvpl->m_n ? wvpl->m_p + o : 0);
            }
        }
        else
        {
            std::vector<Chunk> wk = Children(wvpl->m_p, wvpl->m_n);
            for (size_t i = 0; i < wk.size(); ++i)
                if (wk[i].m_nType == FCC("wave")) waves.push_back(wk[i].m_p - 12);
        }
    }
    m_Waves.resize(waves.size());
    for (size_t i = 0; i < waves.size(); ++i)
    {
        if (!waves[i]) continue;
        const uint32_t nSz = Rd<uint32_t>(waves[i] + 4);
        const unsigned char* end = wvpl->m_p + wvpl->m_n;
        if (waves[i] + 8 + nSz > end || Rd<uint32_t>(waves[i] + 8) != FCC("wave")) continue;
        std::vector<Chunk> wk = Children(waves[i] + 12, nSz - 4);
        const Chunk* fmt = FindChunk(wk, FCC("fmt "));
        const Chunk* dat = FindChunk(wk, FCC("data"));
        if (!fmt || !dat || fmt->m_n < 16) continue;
        const uint16_t tag = Rd<uint16_t>(fmt->m_p), ch = Rd<uint16_t>(fmt->m_p + 2);
        const uint32_t rate = Rd<uint32_t>(fmt->m_p + 4);
        const uint16_t bits = Rd<uint16_t>(fmt->m_p + 14);
        if (tag != 1 || ch == 0 || (bits != 8 && bits != 16)) continue;
        LTDLSWave& w = m_Waves[i];
        w.m_nRate = rate ? rate : 22050;
        const size_t nFrames = dat->m_n / (ch * (bits / 8));
        w.m_Samples.resize(nFrames);
        for (size_t f = 0; f < nFrames; ++f)
        {
            int s = 0;
            for (int c2 = 0; c2 < ch; ++c2)
                s += (bits == 16) ? Rd<int16_t>(dat->m_p + (f * ch + c2) * 2)
                                  : ((int)dat->m_p[f * ch + c2] - 128) * 256;
            w.m_Samples[f] = (int16_t)(s / ch);
        }
        if (const Chunk* ws = FindChunk(wk, FCC("wsmp")))
        {
            w.m_bHasWsmp = true;
            ReadWsmp(*ws, w.m_nUnity, w.m_nFineTune, w.m_fGain, w.m_bLoop, w.m_nLoopStart, w.m_nLoopLength);
        }
    }

    // ---- the instruments ------------------------------------------------- //
    const Chunk* lins = FindChunk(k, FCC("LIST"), FCC("lins"));
    if (!lins) return true;
    std::vector<Chunk> ins = Children(lins->m_p, lins->m_n);
    for (size_t i = 0; i < ins.size(); ++i)
    {
        if (ins[i].m_nType != FCC("ins ")) continue;
        std::vector<Chunk> ik = Children(ins[i].m_p, ins[i].m_n);
        const Chunk* insh = FindChunk(ik, FCC("insh"));
        if (!insh || insh->m_n < 12) continue;
        LTDLSInstrument inst;
        inst.m_nBank = Rd<uint32_t>(insh->m_p + 4);
        inst.m_nProgram = Rd<uint32_t>(insh->m_p + 8);
        inst.m_bHasArt = false;
        if (const Chunk* info = FindChunk(ik, FCC("LIST"), FCC("INFO")))
        {
            std::vector<Chunk> nk = Children(info->m_p, info->m_n);
            if (const Chunk* nm = FindChunk(nk, FCC("INAM"))) inst.m_sName.assign((const char*)nm->m_p, strnlen((const char*)nm->m_p, nm->m_n));
        }
        if (const Chunk* lart = FindChunk(ik, FCC("LIST"), FCC("lart")))
        {
            std::vector<Chunk> ak = Children(lart->m_p, lart->m_n);
            for (size_t a = 0; a < ak.size(); ++a)
                if (ak[a].m_nId == FCC("art1")) { inst.m_Art.Apply(ak[a].m_p, ak[a].m_n); inst.m_bHasArt = true; }
        }
        if (const Chunk* lrgn = FindChunk(ik, FCC("LIST"), FCC("lrgn")))
        {
            std::vector<Chunk> rk = Children(lrgn->m_p, lrgn->m_n);
            for (size_t r = 0; r < rk.size(); ++r)
            {
                if (rk[r].m_nType != FCC("rgn ") && rk[r].m_nType != FCC("rgn2")) continue;
                std::vector<Chunk> gk = Children(rk[r].m_p, rk[r].m_n);
                const Chunk* rgnh = FindChunk(gk, FCC("rgnh"));
                const Chunk* wlnk = FindChunk(gk, FCC("wlnk"));
                if (!rgnh || rgnh->m_n < 12 || !wlnk || wlnk->m_n < 12) continue;
                LTDLSRegion reg;
                reg.m_nKeyLo = (uint8_t)Rd<uint16_t>(rgnh->m_p);
                reg.m_nKeyHi = (uint8_t)Rd<uint16_t>(rgnh->m_p + 2);
                reg.m_nVelLo = (uint8_t)Rd<uint16_t>(rgnh->m_p + 4);
                reg.m_nVelHi = (uint8_t)Rd<uint16_t>(rgnh->m_p + 6);
                reg.m_nKeyGroup = Rd<uint16_t>(rgnh->m_p + 10);
                reg.m_nWave = Rd<uint32_t>(wlnk->m_p + 8);
                reg.m_bHasWsmp = false;
                reg.m_nUnity = 60; reg.m_nFineTune = 0; reg.m_fGain = 1.0f;
                reg.m_bLoop = false; reg.m_nLoopStart = reg.m_nLoopLength = 0;
                if (const Chunk* ws = FindChunk(gk, FCC("wsmp")))
                {
                    reg.m_bHasWsmp = true;
                    ReadWsmp(*ws, reg.m_nUnity, reg.m_nFineTune, reg.m_fGain,
                             reg.m_bLoop, reg.m_nLoopStart, reg.m_nLoopLength);
                }
                reg.m_bHasArt = false;
                if (const Chunk* lart = FindChunk(gk, FCC("LIST"), FCC("lart")))
                {
                    reg.m_Art = inst.m_Art;     // start from the instrument's
                    std::vector<Chunk> ak = Children(lart->m_p, lart->m_n);
                    for (size_t a = 0; a < ak.size(); ++a)
                        if (ak[a].m_nId == FCC("art1")) { reg.m_Art.Apply(ak[a].m_p, ak[a].m_n); reg.m_bHasArt = true; }
                }
                inst.m_Regions.push_back(reg);
            }
        }
        m_Instruments.push_back(inst);
    }
    return true;
}

const LTDLSInstrument* CLTDLSCollection::Find(uint32_t nPatch, bool bExact) const
{
    const uint32_t nMSB = (nPatch >> 16) & 0x7F, nLSB = (nPatch >> 8) & 0x7F, nProg = nPatch & 0x7F;
    const bool bDrum = (nPatch & 0x80000000u) != 0;
    for (size_t i = 0; i < m_Instruments.size(); ++i)
    {
        const LTDLSInstrument& in = m_Instruments[i];
        const bool bInDrum = (in.m_nBank & 0x80000000u) != 0;
        if (bInDrum != bDrum || (in.m_nProgram & 0x7F) != nProg) continue;
        if (((in.m_nBank >> 8) & 0x7F) == nMSB && (in.m_nBank & 0x7F) == nLSB) return &in;
    }
    if (bExact) return 0;
    for (size_t i = 0; i < m_Instruments.size(); ++i)
    {
        const LTDLSInstrument& in = m_Instruments[i];
        const bool bInDrum = (in.m_nBank & 0x80000000u) != 0;
        if (bInDrum == bDrum && (bDrum || (in.m_nProgram & 0x7F) == nProg)) return &in;
    }
    return 0;
}
