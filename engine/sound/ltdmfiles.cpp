// ------------------------------------------------------------------------- //
//
// ltdmfiles.cpp -- see ltdmfiles.h.
//
// ⚠️ EVERY ARRAY CHUNK STARTS WITH ITS ELEMENT SIZE, and the element size is
// what the loop strides by -- never sizeof() of a struct here. DX7 and DX8
// files differ only in how much each element carries at its END, so reading
// the prefix of each element at the stored stride accepts both.
//
// ------------------------------------------------------------------------- //

#include "ltdmfiles.h"

#include <string.h>

namespace
{
    template <class T> T Rd(const unsigned char* p) { T v; memcpy(&v, p, sizeof(T)); return v; }

    // One chunk: id, payload, and (for RIFF/LIST) the form type.
    struct Chunk
    {
        uint32_t                m_nId;
        uint32_t                m_nType;    // 0 unless RIFF/LIST
        const unsigned char*    m_p;        // payload (after the type for RIFF/LIST)
        size_t                  m_n;
    };

    constexpr uint32_t FCC(const char (&s)[5])
    { return (uint32_t)(unsigned char)s[0] | ((uint32_t)(unsigned char)s[1] << 8)
           | ((uint32_t)(unsigned char)s[2] << 16) | ((uint32_t)(unsigned char)s[3] << 24); }

    // The children of a chunk body. Stops at the first chunk that would run
    // past the end -- a truncated file loses its tail, never reads beyond.
    std::vector<Chunk> Children(const unsigned char* p, size_t n)
    {
        std::vector<Chunk> out;
        size_t o = 0;
        while (o + 8 <= n)
        {
            Chunk c;
            c.m_nId = Rd<uint32_t>(p + o);
            uint32_t nSz = Rd<uint32_t>(p + o + 4);
            if (o + 8 + nSz > n) break;
            c.m_nType = 0;
            c.m_p = p + o + 8;
            c.m_n = nSz;
            if ((c.m_nId == FCC("RIFF") || c.m_nId == FCC("LIST")) && nSz >= 4)
            {
                c.m_nType = Rd<uint32_t>(c.m_p);
                c.m_p += 4;
                c.m_n -= 4;
            }
            out.push_back(c);
            o += 8 + nSz + (nSz & 1);
        }
        return out;
    }

    const Chunk* Find(const std::vector<Chunk>& v, uint32_t nId, uint32_t nType = 0)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].m_nId == nId && (nType == 0 || v[i].m_nType == nType)) return &v[i];
        return 0;
    }

    // UTF-16LE (as DirectMusic stores names) to ASCII; stops at the first NUL.
    std::string Wide(const unsigned char* p, size_t n)
    {
        std::string s;
        for (size_t i = 0; i + 1 < n; i += 2)
        {
            const uint16_t c = Rd<uint16_t>(p + i);
            if (!c) break;
            s += (c < 128) ? (char)c : '?';
        }
        return s;
    }

    std::string NameOf(const std::vector<Chunk>& kids)
    {
        // LIST UNFO { UNAM "..." } -- the object's own name.
        if (const Chunk* u = Find(kids, FCC("LIST"), FCC("UNFO")))
        {
            std::vector<Chunk> uk = Children(u->m_p, u->m_n);
            if (const Chunk* nm = Find(uk, FCC("UNAM"))) return Wide(nm->m_p, nm->m_n);
        }
        return std::string();
    }

    LTDMRef ReadRef(const Chunk& dmrf)
    {
        LTDMRef r;
        std::vector<Chunk> k = Children(dmrf.m_p, dmrf.m_n);
        if (const Chunk* c = Find(k, FCC("name"))) r.m_sName = Wide(c->m_p, c->m_n);
        if (const Chunk* c = Find(k, FCC("file"))) r.m_sFile = Wide(c->m_p, c->m_n);
        return r;
    }

    LTDMTimeSig ReadTimeSig(const unsigned char* p)
    {
        LTDMTimeSig t;
        t.m_nBeatsPerMeasure = p[0];
        t.m_nBeat = p[1];
        t.m_nGridsPerBeat = Rd<uint16_t>(p + 2);
        return t;
    }

    // RIFF DMBD -- one band: LIST lbil { LIST lbin { bins [LIST DMRF] } }.
    bool ReadBand(const Chunk& dmbd, LTDMBand& b)
    {
        std::vector<Chunk> k = Children(dmbd.m_p, dmbd.m_n);
        b.m_sName = NameOf(k);
        const Chunk* lbil = Find(k, FCC("LIST"), FCC("lbil"));
        if (!lbil) return true;
        std::vector<Chunk> ins = Children(lbil->m_p, lbil->m_n);
        for (size_t i = 0; i < ins.size(); ++i)
        {
            if (ins[i].m_nId != FCC("LIST") || ins[i].m_nType != FCC("lbin")) continue;
            std::vector<Chunk> ik = Children(ins[i].m_p, ins[i].m_n);
            const Chunk* bins = Find(ik, FCC("bins"));
            if (!bins || bins->m_n < 36) continue;
            LTDMBandInstrument in;
            const unsigned char* q = bins->m_p;
            in.m_nPatch     = Rd<uint32_t>(q + 0);
            in.m_nPChannel  = Rd<uint32_t>(q + 24);
            in.m_nFlags     = Rd<uint32_t>(q + 28);
            in.m_nPan       = q[32];
            in.m_nVolume    = q[33];
            in.m_nTranspose = Rd<int16_t>(q + 34);
            in.m_nPriority  = bins->m_n >= 40 ? Rd<uint32_t>(q + 36) : 0;
            if (const Chunk* r = Find(ik, FCC("LIST"), FCC("DMRF")))
                in.m_sDLSFile = ReadRef(*r).m_sFile;
            b.m_Instruments.push_back(in);
        }
        return true;
    }

    // A size-prefixed array chunk: calls fn(element, elementSize) per element.
    template <class F> void ForEach(const Chunk& c, size_t nMin, F fn)
    {
        if (c.m_n < 4) return;
        const uint32_t nSz = Rd<uint32_t>(c.m_p);
        if (nSz < nMin || nSz > 4096) return;
        for (size_t o = 4; o + nSz <= c.m_n; o += nSz) fn(c.m_p + o, nSz);
    }
}

int LTDMTimeSig::GridToTicks(int nGrid) const
{
    const int gpb = m_nGridsPerBeat ? m_nGridsPerBeat : 1;
    const int bpm = m_nBeatsPerMeasure ? m_nBeatsPerMeasure : 4;
    const int nPerMeasure = gpb * bpm;
    const int nMeasure = nGrid / nPerMeasure;
    const int nRest    = nGrid % nPerMeasure;
    return nMeasure * MeasureTicks() + (nRest / gpb) * BeatTicks() + (nRest % gpb) * GridTicks();
}

bool LTDMStyle::Load(const unsigned char* p, size_t n)
{
    std::vector<Chunk> top = Children(p, n);
    const Chunk* riff = Find(top, FCC("RIFF"), FCC("DMST"));
    if (!riff) return false;
    std::vector<Chunk> k = Children(riff->m_p, riff->m_n);
    m_sName = NameOf(k);
    m_fTempo = 120.0;
    if (const Chunk* h = Find(k, FCC("styh")))
    {
        if (h->m_n >= 4) m_TimeSig = ReadTimeSig(h->m_p);
        if (h->m_n >= 12) m_fTempo = Rd<double>(h->m_p + h->m_n - 8);
    }

    for (size_t i = 0; i < k.size(); ++i)
    {
        const Chunk& c = k[i];
        if (c.m_nId == FCC("LIST") && c.m_nType == FCC("part"))
        {
            std::vector<Chunk> pk = Children(c.m_p, c.m_n);
            const Chunk* h = Find(pk, FCC("prth"));
            if (!h || h->m_n < 153) continue;
            LTDMStylePart part;
            part.m_TimeSig = ReadTimeSig(h->m_p);
            for (int v = 0; v < 32; ++v) part.m_VariationChoices[v] = Rd<uint32_t>(h->m_p + 4 + v * 4);
            memcpy(part.m_Guid, h->m_p + 132, 16);
            part.m_nMeasures = Rd<uint16_t>(h->m_p + 148);
            part.m_nPlayMode = h->m_p[150];
            if (const Chunk* nt = Find(pk, FCC("note")))
                ForEach(*nt, 22, [&](const unsigned char* q, size_t) {
                    LTDMStyleNote x;
                    x.m_nGridStart  = Rd<int32_t>(q + 0);
                    x.m_nVariation  = Rd<uint32_t>(q + 4);
                    x.m_nDuration   = Rd<int32_t>(q + 8);
                    x.m_nTimeOffset = Rd<int16_t>(q + 12);
                    x.m_nMusicValue = Rd<uint16_t>(q + 14);
                    x.m_nVelocity = q[16]; x.m_nTimeRange = q[17];
                    x.m_nDurRange = q[18]; x.m_nVelRange  = q[19];
                    x.m_nInversion = q[20]; x.m_nPlayMode = q[21];
                    part.m_Notes.push_back(x);
                });
            if (const Chunk* cv = Find(pk, FCC("crve")))
                ForEach(*cv, 28, [&](const unsigned char* q, size_t) {
                    LTDMStyleCurve x;
                    x.m_nGridStart     = Rd<int32_t>(q + 0);
                    x.m_nVariation     = Rd<uint32_t>(q + 4);
                    x.m_nDuration      = Rd<int32_t>(q + 8);
                    x.m_nResetDuration = Rd<int32_t>(q + 12);
                    x.m_nTimeOffset    = Rd<int16_t>(q + 16);
                    x.m_nStart = Rd<int16_t>(q + 18);
                    x.m_nEnd   = Rd<int16_t>(q + 20);
                    x.m_nReset = Rd<int16_t>(q + 22);
                    x.m_nEventType = q[24]; x.m_nShape = q[25]; x.m_nCC = q[26]; x.m_nFlags = q[27];
                    part.m_Curves.push_back(x);
                });
            m_Parts.push_back(part);
        }
        else if (c.m_nId == FCC("LIST") && c.m_nType == FCC("pttn"))
        {
            std::vector<Chunk> pk = Children(c.m_p, c.m_n);
            const Chunk* h = Find(pk, FCC("ptnh"));
            if (!h || h->m_n < 10) continue;
            LTDMPattern pat;
            pat.m_sName = NameOf(pk);
            pat.m_TimeSig = ReadTimeSig(h->m_p);
            pat.m_nGrooveBottom  = h->m_p[4];
            pat.m_nGrooveTop     = h->m_p[5];
            pat.m_nEmbellishment = Rd<uint16_t>(h->m_p + 6);
            pat.m_nMeasures      = Rd<uint16_t>(h->m_p + 8);
            for (size_t j = 0; j < pk.size(); ++j)
            {
                if (pk[j].m_nId != FCC("LIST") || pk[j].m_nType != FCC("pref")) continue;
                std::vector<Chunk> rk = Children(pk[j].m_p, pk[j].m_n);
                const Chunk* r = Find(rk, FCC("prfc"));
                if (!r || r->m_n < 22) continue;
                LTDMPartRef ref;
                memcpy(ref.m_Guid, r->m_p, 16);
                ref.m_nPart = -1;
                ref.m_nPChannel       = Rd<uint16_t>(r->m_p + 16);
                ref.m_nLockId         = r->m_p[18];
                ref.m_nSubChordLevel  = r->m_p[19];
                ref.m_nPriority       = r->m_p[20];
                ref.m_nRandomVariation = r->m_p[21];
                // DX8 appends a 32-bit PChannel after a WORD of padding.
                if (r->m_n >= 28) ref.m_nPChannel = Rd<uint32_t>(r->m_p + 24);
                pat.m_PartRefs.push_back(ref);
            }
            m_Patterns.push_back(pat);
        }
        else if (c.m_nId == FCC("RIFF") && c.m_nType == FCC("DMBD"))
        {
            LTDMBand b; b.m_nTime = 0;
            ReadBand(c, b);
            m_Bands.push_back(b);
        }
        else if (c.m_nId == FCC("LIST") && c.m_nType == FCC("lbnd"))
        {
            // Style bands may also sit one level down.
            std::vector<Chunk> bk = Children(c.m_p, c.m_n);
            for (size_t j = 0; j < bk.size(); ++j)
                if (bk[j].m_nId == FCC("RIFF") && bk[j].m_nType == FCC("DMBD"))
                { LTDMBand b; b.m_nTime = 0; ReadBand(bk[j], b); m_Bands.push_back(b); }
        }
    }

    // Resolve each pattern's part references to parts by GUID.
    for (size_t pi = 0; pi < m_Patterns.size(); ++pi)
        for (size_t r = 0; r < m_Patterns[pi].m_PartRefs.size(); ++r)
        {
            LTDMPartRef& ref = m_Patterns[pi].m_PartRefs[r];
            for (size_t q = 0; q < m_Parts.size(); ++q)
                if (memcmp(m_Parts[q].m_Guid, ref.m_Guid, 16) == 0) { ref.m_nPart = (int)q; break; }
        }
    return true;
}

bool LTDMSegment::Load(const unsigned char* p, size_t n)
{
    std::vector<Chunk> top = Children(p, n);
    const Chunk* riff = Find(top, FCC("RIFF"), FCC("DMSG"));
    if (!riff) return false;
    std::vector<Chunk> k = Children(riff->m_p, riff->m_n);
    m_sName = NameOf(k);
    m_nRepeats = 0; m_nLength = 0; m_nPlayStart = m_nLoopStart = m_nLoopEnd = 0;
    m_nResolution = 0; m_nChordKey = 0;
    if (const Chunk* h = Find(k, FCC("segh")))
        if (h->m_n >= 24)
        {
            m_nRepeats    = Rd<uint32_t>(h->m_p + 0);
            m_nLength     = Rd<int32_t>(h->m_p + 4);
            m_nPlayStart  = Rd<int32_t>(h->m_p + 8);
            m_nLoopStart  = Rd<int32_t>(h->m_p + 12);
            m_nLoopEnd    = Rd<int32_t>(h->m_p + 16);
            m_nResolution = Rd<uint32_t>(h->m_p + 20);
        }

    const Chunk* trkl = Find(k, FCC("LIST"), FCC("trkl"));
    if (!trkl) return true;
    std::vector<Chunk> tracks = Children(trkl->m_p, trkl->m_n);
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        if (tracks[t].m_nId != FCC("RIFF") || tracks[t].m_nType != FCC("DMTK")) continue;
        std::vector<Chunk> tk = Children(tracks[t].m_p, tracks[t].m_n);
        // DMUS_IO_TRACK_HEADER: GUID, dwPosition, dwGroup -- a style track reads
        // commands, chords and mutes only from tracks sharing a group bit.
        uint32_t nGroup = 1;
        if (const Chunk* th = Find(tk, FCC("trkh"))) if (th->m_n >= 24) nGroup = Rd<uint32_t>(th->m_p + 20);
        for (size_t j = 0; j < tk.size(); ++j)
        {
            const Chunk& c = tk[j];
            if (c.m_nId == FCC("cmnd"))
                ForEach(c, 11, [&](const unsigned char* q, size_t) {
                    LTDMCommand x;
                    x.m_nTime = Rd<int32_t>(q); x.m_nMeasure = Rd<uint16_t>(q + 4);
                    x.m_nBeat = q[6]; x.m_nCommand = q[7]; x.m_nGrooveLevel = q[8];
                    x.m_nGrooveRange = q[9]; x.m_nRepeatMode = q[10];
                    x.m_nGroup = nGroup;
                    m_Commands.push_back(x);
                });
            else if (c.m_nId == FCC("tetr"))
                ForEach(c, 12, [&](const unsigned char* q, size_t sz) {
                    LTDMTempo x; x.m_nTime = Rd<int32_t>(q);
                    x.m_fBPM = Rd<double>(q + (sz >= 16 ? 8 : 4));
                    m_Tempos.push_back(x);
                });
            else if (c.m_nId == FCC("tims"))
                ForEach(c, 8, [&](const unsigned char* q, size_t) {
                    LTDMTimeSigItem x; x.m_nTime = Rd<int32_t>(q); x.m_TimeSig = ReadTimeSig(q + 4);
                    x.m_nGroup = nGroup;
                    m_TimeSigs.push_back(x);
                });
            else if (c.m_nId == FCC("mute"))
                ForEach(c, 12, [&](const unsigned char* q, size_t) {
                    LTDMMute x; x.m_nTime = Rd<int32_t>(q);
                    x.m_nPChannel = Rd<uint32_t>(q + 4); x.m_nMap = Rd<uint32_t>(q + 8);
                    x.m_nGroup = nGroup;
                    m_Mutes.push_back(x);
                });
            else if (c.m_nId == FCC("LIST") && c.m_nType == FCC("sttr"))
            {
                std::vector<Chunk> sk = Children(c.m_p, c.m_n);
                for (size_t s = 0; s < sk.size(); ++s)
                {
                    if (sk[s].m_nId != FCC("LIST") || sk[s].m_nType != FCC("strf")) continue;
                    std::vector<Chunk> fk = Children(sk[s].m_p, sk[s].m_n);
                    LTDMStyleRef r; r.m_nTime = 0; r.m_nGroup = nGroup;
                    if (const Chunk* tm = Find(fk, FCC("stmp"))) if (tm->m_n >= 4) r.m_nTime = Rd<int32_t>(tm->m_p);
                    if (const Chunk* rf = Find(fk, FCC("LIST"), FCC("DMRF"))) r.m_Ref = ReadRef(*rf);
                    m_Styles.push_back(r);
                }
            }
            else if (c.m_nId == FCC("RIFF") && c.m_nType == FCC("DMBT"))
            {
                std::vector<Chunk> bk = Children(c.m_p, c.m_n);
                const Chunk* lbdl = Find(bk, FCC("LIST"), FCC("lbdl"));
                if (!lbdl) continue;
                std::vector<Chunk> bands = Children(lbdl->m_p, lbdl->m_n);
                for (size_t b = 0; b < bands.size(); ++b)
                {
                    if (bands[b].m_nId != FCC("LIST") || bands[b].m_nType != FCC("lbnd")) continue;
                    std::vector<Chunk> bb = Children(bands[b].m_p, bands[b].m_n);
                    LTDMBand band; band.m_nTime = 0;
                    if (const Chunk* h = Find(bb, FCC("bdih"))) { if (h->m_n >= 4) band.m_nTime = Rd<int32_t>(h->m_p); }
                    else if (const Chunk* h2 = Find(bb, FCC("bd2h"))) { if (h2->m_n >= 4) band.m_nTime = Rd<int32_t>(h2->m_p); }
                    if (const Chunk* d = Find(bb, FCC("RIFF"), FCC("DMBD"))) ReadBand(*d, band);
                    m_Bands.push_back(band);
                }
            }
            else if (c.m_nId == FCC("LIST") && c.m_nType == FCC("cord"))
            {
                std::vector<Chunk> ck = Children(c.m_p, c.m_n);
                for (size_t q = 0; q < ck.size(); ++q)
                {
                    if (ck[q].m_nId == FCC("crdh") && ck[q].m_n >= 4) m_nChordKey = Rd<uint32_t>(ck[q].m_p);
                    if (ck[q].m_nId != FCC("crdb") || ck[q].m_n < 8) continue;
                    const unsigned char* d = ck[q].m_p;
                    const uint32_t nCS = Rd<uint32_t>(d);
                    if (nCS < 38 || 4 + nCS + 8 > ck[q].m_n) continue;
                    LTDMChord ch;
                    ch.m_nGroup = nGroup;
                    ch.m_sName = Wide(d + 4, 32);
                    ch.m_nTime = Rd<int32_t>(d + 36);
                    size_t o = 4 + nCS;
                    const uint32_t nSub = Rd<uint32_t>(d + o), nSS = Rd<uint32_t>(d + o + 4);
                    o += 8;
                    for (uint32_t s = 0; s < nSub && nSS >= 18 && o + nSS <= ck[q].m_n; ++s, o += nSS)
                    {
                        LTDMSubChord sc;
                        sc.m_nChordPattern    = Rd<uint32_t>(d + o);
                        sc.m_nScalePattern    = Rd<uint32_t>(d + o + 4);
                        sc.m_nInversionPoints = Rd<uint32_t>(d + o + 8);
                        sc.m_nLevels          = Rd<uint32_t>(d + o + 12);
                        sc.m_nChordRoot = d[o + 16];
                        sc.m_nScaleRoot = d[o + 17];
                        ch.m_Sub.push_back(sc);
                    }
                    m_Chords.push_back(ch);
                }
            }
            else if (c.m_nId == FCC("LIST") && c.m_nType == FCC("seqt"))
            {
                std::vector<Chunk> sk = Children(c.m_p, c.m_n);
                if (const Chunk* ev = Find(sk, FCC("evtl")))
                    ForEach(*ev, 17, [&](const unsigned char* q, size_t) {
                        LTDMSeqNote x;
                        x.m_nTime = Rd<int32_t>(q) + Rd<int16_t>(q + 12);
                        x.m_nDuration = Rd<int32_t>(q + 4);
                        x.m_nPChannel = Rd<uint32_t>(q + 8);
                        x.m_nStatus = q[14]; x.m_nData1 = q[15]; x.m_nData2 = q[16];
                        m_Seq.push_back(x);
                    });
            }
        }
    }
    return true;
}
