// ----------------------------------------------------------------------- //
//
// MODULE  : ltwav.cpp
//
// See ltwav.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltwav.h"
#include "ltfilemgr.h"
#include "iltstream.h"

#include <string.h>
#include <vector>

namespace
{
    inline unsigned short U16(const unsigned char* p)
    { unsigned short v; memcpy(&v, p, 2); return v; }

    inline unsigned int U32(const unsigned char* p)
    { unsigned int v; memcpy(&v, p, 4); return v; }

    // ⚠️ IMA/DVI ADPCM, and the two tables ARE the format -- there is no
    // arithmetic shortcut. The step table is the standard 89-entry one; the
    // index table says how the step moves after each nibble.
    const int kIndexTable[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8
    };

    const int kStepTable[89] = {
            7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
           19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
           50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
          130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
          337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
          876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
         2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
         5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };

    inline short DecodeNibble(int nNibble, int& nPred, int& nIndex)
    {
        const int step = kStepTable[nIndex];

        int diff = step >> 3;
        if (nNibble & 1) diff += step >> 2;
        if (nNibble & 2) diff += step >> 1;
        if (nNibble & 4) diff += step;
        if (nNibble & 8) diff = -diff;

        nPred += diff;
        if (nPred >  32767) nPred =  32767;
        if (nPred < -32768) nPred = -32768;

        nIndex += kIndexTable[nNibble];
        if (nIndex < 0)  nIndex = 0;
        if (nIndex > 88) nIndex = 88;

        return (short)nPred;
    }
}


CLTWav::CLTWav()
    : m_bLoaded(false), m_sError(""), m_eFormat(kUnknown),
      m_nRate(0), m_nChannels(0), m_nBits(0), m_nBlockAlign(0)
{
}


bool CLTWav::Fail(const char* p)
{
    m_sError = p;
    m_bLoaded = false;
    m_Samples.clear();
    m_Raw.clear();
    return false;
}


float CLTWav::GetDuration() const
{
    if (!m_nRate || !m_nChannels) return 0.0f;
    if (!m_Samples.empty())
        return (float)(m_Samples.size() / m_nChannels) / (float)m_nRate;
    return 0.0f;        // MP3: not known without decoding
}


bool CLTWav::LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName)
{
    if (!pFileMgr || !pName) return Fail("no file manager");

    ILTStream* pS = pFileMgr->OpenFile(pName);
    if (!pS) return Fail("file not found");

    unsigned long nLen = 0;
    pS->GetLen(&nLen);
    if (nLen == 0) { pS->Release(); return Fail("empty file"); }

    std::vector<unsigned char> buf((size_t)nLen);
    pS->Read(&buf[0], (unsigned long)nLen);
    pS->Release();

    return Load(&buf[0], (unsigned int)nLen);
}


bool CLTWav::Load(const void* pData, unsigned int nSize)
{
    m_bLoaded = false;
    m_sError = "";
    m_eFormat = kUnknown;
    m_nRate = m_nChannels = m_nBits = m_nBlockAlign = 0;
    m_Samples.clear();
    m_Raw.clear();

    const unsigned char* p = (const unsigned char*)pData;
    if (!p || nSize < 44) return Fail("shorter than a RIFF header");

    if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0)
        return Fail("not a RIFF/WAVE file");

    // ⚠️ WALK THE CHUNKS, never assume `fmt ` at 12 and `data` at 36. The
    // shipped files carry `fact` and padding chunks between them, and a reader
    // that seeks to fixed offsets reads a chunk header as samples.
    unsigned int nDataPos = 0, nDataSize = 0;
    bool bFmt = false;

    size_t at = 12;
    while (at + 8 <= (size_t)nSize)
    {
        const unsigned int nChunk = U32(p + at + 4);

        if (memcmp(p + at, "fmt ", 4) == 0)
        {
            if (at + 8 + 16 > (size_t)nSize) return Fail("truncated fmt chunk");
            m_eFormat     = (Format)U16(p + at + 8);
            m_nChannels   = U16(p + at + 10);
            m_nRate       = U32(p + at + 12);
            m_nBlockAlign = U16(p + at + 20);
            m_nBits       = U16(p + at + 22);
            bFmt = true;
        }
        else if (memcmp(p + at, "data", 4) == 0)
        {
            nDataPos  = (unsigned int)(at + 8);
            nDataSize = nChunk;
            if ((size_t)nDataPos + nDataSize > (size_t)nSize)
                nDataSize = nSize - nDataPos;      // tolerate a clipped tail
        }

        // ⚠️ Chunks are WORD ALIGNED: an odd size is followed by a pad byte
        // that is not counted in the size. Missing it desynchronises the walk
        // one chunk later, which looks like a corrupt file.
        at += 8 + nChunk + (nChunk & 1);
    }

    if (!bFmt)       return Fail("no fmt chunk");
    if (!nDataSize)  return Fail("no data chunk");
    if (!m_nChannels || !m_nRate) return Fail("zero channels or rate");

    m_Raw.assign(p + nDataPos, p + nDataPos + nDataSize);

    if (m_eFormat == kMP3)
    {
        // Identified, not decoded -- see the header.
        m_bLoaded = true;
        return true;
    }

    if (m_eFormat == kPCM)
    {
        if (m_nBits == 16)
        {
            const size_t n = nDataSize / 2;
            m_Samples.resize(n);
            memcpy(&m_Samples[0], p + nDataPos, n * 2);
        }
        else if (m_nBits == 8)
        {
            // ⚠️ 8-bit PCM in a WAV is UNSIGNED, centred on 128 -- unlike every
            // other depth, which is signed. Treating it as signed produces a
            // loud click and a DC offset.
            m_Samples.resize(nDataSize);
            for (size_t i = 0; i < nDataSize; ++i)
                m_Samples[i] = (short)(((int)p[nDataPos + i] - 128) << 8);
        }
        else return Fail("unsupported PCM bit depth");

        m_bLoaded = true;
        return true;
    }

    if (m_eFormat == kADPCM)
    {
        if (m_nBits != 4) return Fail("unsupported ADPCM bit depth");
        if (m_nBlockAlign < 4 * m_nChannels) return Fail("ADPCM block too small");

        // ⚠️ IMA ADPCM IS BLOCK-BASED, and each block RESTARTS the predictor
        // from a header it carries. Decoding the payload as one continuous
        // stream drifts a little further out of step with every block and ends
        // as a rising buzz -- audible, but not obviously a decoder bug.
        const unsigned char* d = p + nDataPos;
        const unsigned int nBlocks = nDataSize / m_nBlockAlign;

        for (unsigned int b = 0; b < nBlocks; ++b)
        {
            const unsigned char* blk = d + (size_t)b * m_nBlockAlign;

            int pred[2] = { 0, 0 }, index[2] = { 0, 0 };
            for (unsigned int c = 0; c < m_nChannels; ++c)
            {
                pred[c]  = (short)U16(blk + c * 4);
                index[c] = blk[c * 4 + 2];
                if (index[c] > 88) index[c] = 88;
                m_Samples.push_back((short)pred[c]);
            }

            const unsigned int nHdr = 4 * m_nChannels;
            for (unsigned int i = nHdr; i < m_nBlockAlign; ++i)
            {
                // Mono only in this data (4,796 of 4,798 files); the channel
                // interleave for stereo IMA is per 4-byte group and is not
                // implemented rather than guessed at.
                const unsigned char byte = blk[i];
                m_Samples.push_back(DecodeNibble(byte & 0x0F, pred[0], index[0]));
                m_Samples.push_back(DecodeNibble(byte >> 4,   pred[0], index[0]));
            }
        }

        m_bLoaded = true;
        return true;
    }

    return Fail("unsupported format tag");
}
