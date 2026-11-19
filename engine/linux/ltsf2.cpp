// Linux port only. Xcode compiles every file under engine/ (synchronized
// folder), so this file is empty on macOS.
#ifndef __APPLE__  // Linux port only
// ----------------------------------------------------------------------- //
//
// MODULE  : ltsf2.cpp  (Linux port)
//
// PURPOSE : A SoundFont 2 bank read INTO a CLTDLSCollection, so the DirectMusic
//           synth plays its GM fallback on Linux exactly as it plays Apple's
//           gs_instruments.dls on a Mac: same voices, same envelope code.
//
// What is read: presets -> instruments (bank 128 = drums), zones -> regions
// (key/velocity ranges intersected), samples -> the wave pool, and the
// generators the DLS articulation has a place for: root key, tuning,
// attenuation, pan, loop mode and points, the volume envelope (delay+attack,
// hold+decay, sustain, release, key -> decay). Modulation envelopes, filters
// and LFOs are left at their defaults -- the GM fallback is a handful of
// patches under NOLF's own DLS banks, not the music itself.
//
// ----------------------------------------------------------------------- //

#include "ltdls.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <algorithm>

namespace
{
    template <class T> T Rd(const unsigned char* p) { T v; memcpy(&v, p, sizeof(T)); return v; }
    uint32_t FCC(const char* s) { return (uint32_t)(unsigned char)s[0] | ((uint32_t)(unsigned char)s[1] << 8)
                                       | ((uint32_t)(unsigned char)s[2] << 16) | ((uint32_t)(unsigned char)s[3] << 24); }

    struct Chunk { uint32_t id, type; const unsigned char* p; size_t n; };

    std::vector<Chunk> Children(const unsigned char* p, size_t n)
    {
        std::vector<Chunk> v;
        size_t at = 0;
        while (at + 8 <= n)
        {
            Chunk c;
            c.id = Rd<uint32_t>(p + at);
            const uint32_t sz = Rd<uint32_t>(p + at + 4);
            if (at + 8 + sz > n) break;
            c.type = 0; c.p = p + at + 8; c.n = sz;
            if ((c.id == FCC("RIFF") || c.id == FCC("LIST")) && sz >= 4) { c.type = Rd<uint32_t>(c.p); c.p += 4; c.n -= 4; }
            v.push_back(c);
            at += 8 + sz + (sz & 1);
        }
        return v;
    }

    const Chunk* Find(const std::vector<Chunk>& v, uint32_t id, uint32_t type = 0)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].id == id && (!type || v[i].type == type)) return &v[i];
        return 0;
    }

    // Generator operators (SF2.04 §8.1.2).
    enum
    {
        kStartOfs = 0, kEndOfs = 1, kStartLoopOfs = 2, kEndLoopOfs = 3, kStartCoarse = 4,
        kPan = 17, kDelayVol = 33, kAttackVol = 34, kHoldVol = 35, kDecayVol = 36, kSustainVol = 37,
        kReleaseVol = 38, kKeyToVolDecay = 40, kInstrument = 41, kKeyRange = 43, kVelRange = 44,
        kStartLoopCoarse = 45, kAttenuation = 48, kEndLoopCoarse = 50, kCoarseTune = 51, kFineTune = 52,
        kSampleID = 53, kSampleModes = 54, kScaleTuning = 56, kRootKey = 58, kEndCoarse = 12,
        kNumGens = 64
    };

    struct Zone
    {
        bool  set[kNumGens];
        int   val[kNumGens];
        Zone() { memset(set, 0, sizeof(set)); memset(val, 0, sizeof(val)); }
    };

    // One bag's generators.
    Zone ReadZone(const unsigned char* gen, size_t nGen, uint16_t from, uint16_t to)
    {
        Zone z;
        for (uint32_t g = from; g < to && g < nGen; ++g)
        {
            const unsigned char* q = gen + g * 4;
            const uint16_t op = Rd<uint16_t>(q);
            if (op >= kNumGens) continue;
            z.set[op] = true;
            if (op == kKeyRange || op == kVelRange) z.val[op] = q[2] | (q[3] << 8);   // lo | hi << 8
            else z.val[op] = Rd<int16_t>(q + 2);
        }
        return z;
    }

    int Get(const Zone& z, const Zone* pGlobal, int op, int def)
    {
        if (z.set[op]) return z.val[op];
        if (pGlobal && pGlobal->set[op]) return pGlobal->val[op];
        return def;
    }

    float TC(int tc) { return tc <= -12000 ? 0.0f : powf(2.0f, (float)tc / 1200.0f); }   // timecents -> s
}

// Reads `p` (a whole .sf2 file) into `coll`. False when it is not one.
bool LTSF2_Load(CLTDLSCollection& coll, const unsigned char* p, size_t n)
{
    std::vector<Chunk> top = Children(p, n);
    const Chunk* riff = Find(top, FCC("RIFF"), FCC("sfbk"));
    if (!riff) return false;
    std::vector<Chunk> k = Children(riff->p, riff->n);
    const Chunk* sdta = Find(k, FCC("LIST"), FCC("sdta"));
    const Chunk* pdta = Find(k, FCC("LIST"), FCC("pdta"));
    if (!sdta || !pdta) return false;
    std::vector<Chunk> sd = Children(sdta->p, sdta->n), pd = Children(pdta->p, pdta->n);
    const Chunk* smpl = Find(sd, FCC("smpl"));
    const Chunk *phdr = Find(pd, FCC("phdr")), *pbag = Find(pd, FCC("pbag")), *pgen = Find(pd, FCC("pgen"));
    const Chunk *inst = Find(pd, FCC("inst")), *ibag = Find(pd, FCC("ibag")), *igen = Find(pd, FCC("igen"));
    const Chunk *shdr = Find(pd, FCC("shdr"));
    if (!smpl || !phdr || !pbag || !pgen || !inst || !ibag || !igen || !shdr) return false;

    const int16_t* pcm = (const int16_t*)smpl->p;
    const size_t nPCM = smpl->n / 2;
    const size_t nPhdr = phdr->n / 38, nPbag = pbag->n / 4, nPgen = pgen->n / 4;
    const size_t nInst = inst->n / 22, nIbag = ibag->n / 4, nIgen = igen->n / 4, nShdr = shdr->n / 46;
    if (nPhdr < 2 || nInst < 2 || nShdr < 2) return false;

    coll.m_sName = "SoundFont";
    std::map<uint32_t, unsigned int> waveOf;       // sample index -> wave pool index

    for (size_t ph = 0; ph + 1 < nPhdr; ++ph)          // the last record is the terminal EOP
    {
        const unsigned char* h = phdr->p + ph * 38;
        const unsigned char* hn = h + 38;
        char name[21]; memcpy(name, h, 20); name[20] = 0;
        const uint16_t prog = Rd<uint16_t>(h + 20), bank = Rd<uint16_t>(h + 22);
        const uint16_t b0 = Rd<uint16_t>(h + 24), b1 = Rd<uint16_t>(hn + 24);
        if (prog > 127) continue;

        LTDLSInstrument out;
        out.m_sName = name;
        out.m_nProgram = prog;
        out.m_nBank = (bank == 128) ? 0x80000000u : ((uint32_t)(bank & 0x7F) << 8);
        out.m_bHasArt = false;

        Zone pGlobal; bool bPGlobal = false;
        for (uint32_t pb = b0; pb < b1 && pb + 1 < nPbag; ++pb)
        {
            const Zone pz = ReadZone(pgen->p, nPgen, Rd<uint16_t>(pbag->p + pb * 4), Rd<uint16_t>(pbag->p + (pb + 1) * 4));
            if (!pz.set[kInstrument])
            {
                if (pb == b0) { pGlobal = pz; bPGlobal = true; }
                continue;
            }
            const Zone* pG = bPGlobal ? &pGlobal : 0;
            const uint32_t ii = (uint32_t)pz.val[kInstrument];
            if (ii + 1 >= nInst) continue;
            const int pkr = Get(pz, pG, kKeyRange, 0x7F00), pvr = Get(pz, pG, kVelRange, 0x7F00);
            const unsigned char* ih = inst->p + ii * 22;
            const uint16_t i0 = Rd<uint16_t>(ih + 20), i1 = Rd<uint16_t>(ih + 22 + 20);

            Zone iGlobal; bool bIGlobal = false;
            for (uint32_t ib = i0; ib < i1 && ib + 1 < nIbag; ++ib)
            {
                const Zone iz = ReadZone(igen->p, nIgen, Rd<uint16_t>(ibag->p + ib * 4), Rd<uint16_t>(ibag->p + (ib + 1) * 4));
                if (!iz.set[kSampleID])
                {
                    if (ib == i0) { iGlobal = iz; bIGlobal = true; }
                    continue;
                }
                const Zone* iG = bIGlobal ? &iGlobal : 0;
                const uint32_t si = (uint32_t)(uint16_t)iz.val[kSampleID];
                if (si + 1 >= nShdr) continue;
                const unsigned char* sh = shdr->p + si * 46;
                const uint16_t sType = Rd<uint16_t>(sh + 44);
                if (sType & 0x8000) continue;                   // ROM samples

                // Ranges: the instrument zone's within the preset zone's.
                const int ikr = Get(iz, iG, kKeyRange, 0x7F00), ivr = Get(iz, iG, kVelRange, 0x7F00);
                const int kl = std::max(ikr & 0xFF, pkr & 0xFF), kh = std::min(ikr >> 8, pkr >> 8);
                const int vl = std::max(ivr & 0xFF, pvr & 0xFF), vh = std::min(ivr >> 8, pvr >> 8);
                if (kl > kh || vl > vh) continue;

                // The sample, with this zone's address offsets.
                const uint32_t s0 = Rd<uint32_t>(sh + 20), s1 = Rd<uint32_t>(sh + 24);
                const uint32_t l0 = Rd<uint32_t>(sh + 28), l1 = Rd<uint32_t>(sh + 32);
                const uint32_t rate = Rd<uint32_t>(sh + 36);
                const int origKey = sh[40];
                const int corr = (int8_t)sh[41];
                const long st = (long)s0 + Get(iz, iG, kStartOfs, 0) + 32768L * Get(iz, iG, kStartCoarse, 0);
                const long en = (long)s1 + Get(iz, iG, kEndOfs, 0) + 32768L * Get(iz, iG, kEndCoarse, 0);
                const long ls = (long)l0 + Get(iz, iG, kStartLoopOfs, 0) + 32768L * Get(iz, iG, kStartLoopCoarse, 0);
                const long le = (long)l1 + Get(iz, iG, kEndLoopOfs, 0) + 32768L * Get(iz, iG, kEndLoopCoarse, 0);
                if (st < 0 || en <= st || (size_t)en > nPCM) continue;

                // One wave per distinct (sample, start, end): offsets are rare.
                const uint32_t key = si ^ ((uint32_t)(st - (long)s0) << 20) ^ ((uint32_t)(en - (long)s1) << 26);
                unsigned int nWave;
                std::map<uint32_t, unsigned int>::iterator wit = waveOf.find(key);
                if (wit != waveOf.end()) nWave = wit->second;
                else
                {
                    LTDLSWave w;
                    w.m_Samples.assign(pcm + st, pcm + en);
                    w.m_nRate = rate ? rate : 22050;
                    w.m_bHasWsmp = false;
                    nWave = (unsigned int)coll.m_Waves.size();
                    coll.m_Waves.push_back(w);
                    waveOf[key] = nWave;
                }

                LTDLSRegion r;
                r.m_nKeyLo = (uint8_t)kl; r.m_nKeyHi = (uint8_t)kh;
                r.m_nVelLo = (uint8_t)vl; r.m_nVelHi = (uint8_t)vh;
                r.m_nKeyGroup = 0;
                r.m_nWave = nWave;
                r.m_bHasWsmp = true;
                const int root = Get(iz, iG, kRootKey, -1);
                r.m_nUnity = (root >= 0 && root <= 127) ? root : origKey;
                // Coarse/fine tuning (instrument + preset are additive) and the
                // sample's own correction, all in cents relative to unity.
                const int coarse = Get(iz, iG, kCoarseTune, 0) + (pz.set[kCoarseTune] ? pz.val[kCoarseTune] : (pG && pG->set[kCoarseTune] ? pG->val[kCoarseTune] : 0));
                const int fine   = Get(iz, iG, kFineTune, 0)   + (pz.set[kFineTune]   ? pz.val[kFineTune]   : (pG && pG->set[kFineTune]   ? pG->val[kFineTune]   : 0));
                r.m_nFineTune = coarse * 100 + fine + corr;
                // Attenuation in centibels; the 0.4 is the EMU convention every
                // SoundFont is voiced for (FluidSynth applies the same).
                const int att = Get(iz, iG, kAttenuation, 0) + (pz.set[kAttenuation] ? pz.val[kAttenuation] : (pG && pG->set[kAttenuation] ? pG->val[kAttenuation] : 0));
                r.m_fGain = powf(10.0f, -0.4f * (float)(att > 0 ? att : 0) / 200.0f);
                const int modes = Get(iz, iG, kSampleModes, 0) & 3;
                r.m_bLoop = (modes == 1 || modes == 3) && le > ls && ls >= st && le <= en;
                r.m_nLoopStart = r.m_bLoop ? (unsigned int)(ls - st) : 0;
                r.m_nLoopLength = r.m_bLoop ? (unsigned int)(le - ls) : 0;

                // The volume envelope.
                r.m_bHasArt = true;
                LTDLSArt& a = r.m_Art;
                a.m_fAttack  = TC(Get(iz, iG, kDelayVol, -12000)) + TC(Get(iz, iG, kAttackVol, -12000));
                a.m_fDecay   = TC(Get(iz, iG, kHoldVol, -12000)) + TC(Get(iz, iG, kDecayVol, -12000));
                int sus = Get(iz, iG, kSustainVol, 0);                    // centibels of attenuation
                if (sus < 0) sus = 0;
                if (sus > 1440) sus = 1440;
                a.m_fSustain = 1.0f - ((float)sus / 10.0f) / 96.0f;       // the synth's -96 dB floor scale
                a.m_fRelease = TC(Get(iz, iG, kReleaseVol, -12000));
                if (a.m_fRelease < 0.01f) a.m_fRelease = 0.01f;           // no click on note-off
                a.m_fKeyToDecay = 0.0f;                                     // (key -> decay: left at none)
                a.m_fPan = (float)Get(iz, iG, kPan, 0) / 1000.0f;
                out.m_Regions.push_back(r);
            }
        }
        if (!out.m_Regions.empty()) coll.m_Instruments.push_back(out);
    }
    printf("[LTDM] SoundFont: %u instruments, %u waves\n",
           (unsigned)coll.m_Instruments.size(), (unsigned)coll.m_Waves.size());
    fflush(stdout);
    return !coll.m_Instruments.empty();
}
#endif  // !__APPLE__
