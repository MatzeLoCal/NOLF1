// ----------------------------------------------------------------------- //
//
// MODULE  : ltspr.cpp
//
// See ltspr.h. The format was settled by predicting every shipped file's size
// from its own header; 173 of 173 land exactly.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltspr.h"
#include "ltfilemgr.h"
#include "ltfilestream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace
{
    // ⚠️ Read through memcpy rather than a cast. The buffer is a byte array
    // with no alignment guarantee, and an unaligned 32-bit load is undefined
    // behaviour even where it happens to work.
    unsigned int U32(const unsigned char* p)
    { unsigned int v; memcpy(&v, p, 4); return v; }

    unsigned short U16(const unsigned char* p)
    { unsigned short v; memcpy(&v, p, 2); return v; }
}


bool CLTSprite::Fail(const char* pWhy)
{
    m_bLoaded = false;
    m_Frames.clear();
    m_nFPS = 0;
    m_sError = pWhy ? pWhy : "?";
    return false;
}


bool CLTSprite::Load(const void* pData, unsigned int nSize)
{
    m_bLoaded = false;
    m_Frames.clear();
    m_nFPS = 0;
    m_bTransparent = m_bTranslucent = false;
    m_nColourKey = 0;
    m_sError.clear();

    if (!pData) return Fail("no data");

    // 20 bytes of header before the first frame name.
    if (nSize < 20) return Fail("shorter than the 20-byte header");

    const unsigned char* p = (const unsigned char*)pData;

    const unsigned int nFrames = U32(p + 0);
    m_nFPS                     = U32(p + 4);

    // ⚠️ Bytes 8..19 are the donor's bTransparent, bTranslucent and colourKey
    // (see the header), and four shipped sprites -- every smoke particle
    // texture -- set the first two. They used to be refused here, which is why
    // smoke and steam drew nothing (5dh). A wrongly-sized header is still
    // caught: the exact-length check at the end is what proves the layout.
    static const bool s_bOld = []() {
        const bool b = (getenv("LT_OLDSPRHEADER") != 0);
        if (b) { printf("[LTSPR] LT_OLDSPRHEADER=1 -- a sprite with non-zero header flags is refused (old)\n"); fflush(stdout); }
        return b; }();
    if (s_bOld)
        for (int k = 8; k < 20; ++k)
            if (p[k] != 0) return Fail("bytes 8..19 are not zero -- header is not what this reader expects");
    m_bTransparent = U32(p + 8) != 0;
    m_bTranslucent = U32(p + 12) != 0;
    m_nColourKey   = U32(p + 16);

    // A frame count that cannot fit is refused before any allocation.
    if (nFrames > nSize) return Fail("frame count exceeds the file size");

    unsigned int off = 20;
    m_Frames.reserve(nFrames);

    for (unsigned int i = 0; i < nFrames; ++i)
    {
        if (off + 2 > nSize) return Fail("ran out of data reading a frame name length");
        const unsigned short nLen = U16(p + off);
        off += 2;

        if (off + nLen > nSize) return Fail("a frame name runs past the end of the file");
        m_Frames.push_back(std::string((const char*)(p + off), nLen));
        off += nLen;
    }

    // ⚠️ THE CHECK THAT MAKES THIS A READER RATHER THAN A GUESS. The header
    // predicts the file's exact length; anything left over means the layout is
    // not what is assumed, and a partial sprite is worse than a refused one.
    if (off != nSize) return Fail("the frame list did not end exactly at the end of the file");

    m_bLoaded = true;
    return true;
}


bool CLTSprite::LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName)
{
    if (!pFileMgr || !pName) return Fail("no file manager or name");

    ILTStream* pStream = pFileMgr->OpenFile(pName);
    if (!pStream)
    {
        std::string sTry(pName);
        sTry += ".spr";
        pStream = pFileMgr->OpenFile(sTry.c_str());
    }
    if (!pStream) return Fail("not found in any tree");

    const unsigned long nLen = pStream->GetLen();
    std::vector<unsigned char> buf(nLen ? nLen : 1, 0);
    if (nLen) pStream->Read(&buf[0], nLen);
    pStream->Release();

    return Load(&buf[0], (unsigned int)nLen);
}


const char* CLTSprite::GetFrame(unsigned int i) const
{
    return (i < m_Frames.size()) ? m_Frames[i].c_str() : "";
}


unsigned int CLTSprite::FrameAt(float fSeconds) const
{
    if (m_Frames.empty()) return 0;

    // ⚠️ A zero frame rate is a STILL, not a division. Nothing shipped declares
    // one, which is exactly why the guard is here: the case that never occurs
    // in the data is the one nobody notices is missing.
    if (m_nFPS == 0) return 0;

    if (fSeconds < 0.0f) fSeconds = 0.0f;
    const unsigned int n = (unsigned int)(fSeconds * (float)m_nFPS);
    return n % (unsigned int)m_Frames.size();
}
