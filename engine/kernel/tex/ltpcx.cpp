// ----------------------------------------------------------------------- //
//
// MODULE  : ltpcx.cpp
//
// PURPOSE : PCX -> RGBA8888. See ltpcx.h for why this and not DTX, and for
//           the two traps in the format.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltpcx.h"
#include "ltfilemgr.h"

#include <stdio.h>
#include <string.h>
#include <string>


// PCX header, 128 bytes.
//
//   0 uint8  Manufacturer (10)    1 uint8  Version (5)
//   2 uint8  Encoding (1=RLE)     3 uint8  BitsPerPixel (per plane)
//   4 uint16 XMin                 6 uint16 YMin
//   8 uint16 XMax                10 uint16 YMax
//  12 uint16 HDpi                14 uint16 VDpi
//  16 uint8  Colormap[48]        64 uint8  Reserved
//  65 uint8  NPlanes             66 uint16 BytesPerLine
//  68 uint16 PaletteInfo         70 uint16 HScreenSize
//  72 uint16 VScreenSize         74 uint8  Filler[54]

#define PCX_HEADER_SIZE     128
#define PCX_MANUFACTURER    10
#define PCX_RLE             1
#define PCX_PALETTE_MARKER  0x0C
#define PCX_PALETTE_BYTES   769     // marker + 256*3

// A guard, not a format limit: the largest shipped PCX is 640x480. Anything
// wildly beyond that is a misparse, and refusing it beats allocating from a
// corrupt header.
#define PCX_MAX_DIM         4096


static unsigned int rd16(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}


CLTPcx::CLTPcx()
    : m_Width(0), m_Height(0), m_Planes(0), m_BytesPerLine(0), m_bHasPalette(false)
{
}

void CLTPcx::Clear()
{
    m_Width = m_Height = m_Planes = m_BytesPerLine = 0;
    m_bHasPalette = false;
    m_Scratch.clear();
    m_RGBA.clear();
}


// ----------------------------------------------------------------------- //
// RLE.
//
// ⚠️ Decoded as ONE stream, not reset per scanline. The PCX spec says runs do
// not cross scanline boundaries, but encoders exist that let them, and reading
// the whole body into one stride-padded buffer is correct either way. Resetting
// per line would desynchronise on exactly those files and produce diagonal
// tearing -- an image that still looks like an image.
// ----------------------------------------------------------------------- //

bool CLTPcx::Expand(const unsigned char* p, unsigned int nSize, unsigned int nTotal)
{
    m_Scratch.assign(nTotal, 0);

    unsigned int in = 0;
    unsigned int out = 0;

    while (out < nTotal)
    {
        if (in >= nSize) return false;              // stream ended early

        unsigned char b = p[in++];

        if ((b & 0xC0) == 0xC0)
        {
            unsigned int nRun = b & 0x3F;
            if (in >= nSize) return false;
            unsigned char v = p[in++];

            // A run that would overrun the image is a malformed file, not
            // something to clamp quietly.
            if (out + nRun > nTotal) return false;

            memset(&m_Scratch[out], v, nRun);
            out += nRun;
        }
        else
        {
            m_Scratch[out++] = b;
        }
    }

    return true;
}


bool CLTPcx::Load(const void* pData, unsigned int nSize)
{
    Clear();

    const unsigned char* p = (const unsigned char*)pData;
    if (!p || nSize < PCX_HEADER_SIZE) return false;

    if (p[0] != PCX_MANUFACTURER) return false;
    if (p[2] != PCX_RLE)          return false;     // no uncompressed variant ships
    if (p[3] != 8)                return false;     // 8 bits per plane, all 289

    unsigned int xmin = rd16(p + 4),  ymin = rd16(p + 6);
    unsigned int xmax = rd16(p + 8),  ymax = rd16(p + 10);
    if (xmax < xmin || ymax < ymin) return false;

    m_Width  = xmax - xmin + 1;
    m_Height = ymax - ymin + 1;
    m_Planes = p[65];
    m_BytesPerLine = rd16(p + 66);

    if (m_Width == 0 || m_Height == 0) return false;
    if (m_Width > PCX_MAX_DIM || m_Height > PCX_MAX_DIM) { Clear(); return false; }
    if (m_Planes != 1 && m_Planes != 3) { Clear(); return false; }

    // ⚠️ BytesPerLine is the STRIDE and is padded to even, so it can exceed the
    // width. Using it as the width shifts every row after the first.
    if (m_BytesPerLine < m_Width) { Clear(); return false; }

    unsigned int nTotal = m_BytesPerLine * m_Planes * m_Height;
    if (!Expand(p + PCX_HEADER_SIZE, nSize - PCX_HEADER_SIZE, nTotal))
    {
        Clear();
        return false;
    }

    // --- palette ------------------------------------------------------ //

    unsigned char pal[256 * 3];
    memset(pal, 0, sizeof(pal));

    if (m_Planes == 1)
    {
        // 256-colour palette lives in the LAST 769 bytes, behind a 0x0C marker.
        if (nSize >= PCX_PALETTE_BYTES &&
            p[nSize - PCX_PALETTE_BYTES] == PCX_PALETTE_MARKER)
        {
            memcpy(pal, p + nSize - PCX_PALETTE_BYTES + 1, 256 * 3);
            m_bHasPalette = true;
        }
        else
        {
            // The 16-colour EGA map in the header is the fallback the format
            // defines. No shipped file needs it, so if this ever fires it is
            // worth knowing rather than silently producing a dim image.
            fprintf(stderr, "[LTPCX] ⚠️  8-bit image with no 0x0C palette;"
                            " falling back to the header's 16-colour map\n");
            memcpy(pal, p + 16, 48);
        }
    }

    // --- to RGBA ------------------------------------------------------ //

    m_RGBA.assign((size_t)m_Width * m_Height * 4, 255);

    for (unsigned int y = 0; y < m_Height; ++y)
    {
        const unsigned char* pRow = &m_Scratch[(size_t)y * m_BytesPerLine * m_Planes];
        unsigned char* pOut = &m_RGBA[(size_t)y * m_Width * 4];

        if (m_Planes == 1)
        {
            for (unsigned int x = 0; x < m_Width; ++x)
            {
                const unsigned char* c = &pal[(size_t)pRow[x] * 3];
                pOut[x * 4 + 0] = c[0];
                pOut[x * 4 + 1] = c[1];
                pOut[x * 4 + 2] = c[2];
                pOut[x * 4 + 3] = 255;
            }
        }
        else
        {
            // ⚠️ PLANE-INTERLEAVED BY ROW: a full row of red, then green, then
            // blue -- not RGB triplets. See ltpcx.h.
            const unsigned char* pR = pRow;
            const unsigned char* pG = pRow + m_BytesPerLine;
            const unsigned char* pB = pRow + m_BytesPerLine * 2;

            for (unsigned int x = 0; x < m_Width; ++x)
            {
                pOut[x * 4 + 0] = pR[x];
                pOut[x * 4 + 1] = pG[x];
                pOut[x * 4 + 2] = pB[x];
                pOut[x * 4 + 3] = 255;
            }
        }
    }

    // The planar scratch is only needed during conversion.
    m_Scratch.clear();
    m_Scratch.shrink_to_fit();

    return true;
}


bool CLTPcx::LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName)
{
    Clear();
    if (!pFileMgr || !pName || !pName[0]) return false;

    // ⚠️ The name arrives both with and without an extension -- see ltpcx.h.
    // As given first, so a name that already says .pcx is not turned into
    // "...pcx.pcx".
    ILTStream* pStream = pFileMgr->OpenFile(pName);
    if (!pStream)
    {
        std::string sTry(pName);
        sTry += ".pcx";
        pStream = pFileMgr->OpenFile(sTry.c_str());
    }
    if (!pStream) return false;

    unsigned long nLen = 0;
    pStream->GetLen(&nLen);

    std::vector<unsigned char> buf(nLen);
    bool bRead = (nLen > 0) && (pStream->Read(&buf[0], nLen) == LT_OK);
    pStream->Release();

    if (!bRead) return false;
    return Load(&buf[0], (unsigned int)nLen);
}
