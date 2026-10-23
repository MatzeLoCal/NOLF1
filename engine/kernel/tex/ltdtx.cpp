// ----------------------------------------------------------------------- //
//
// MODULE  : ltdtx.cpp
//
// PURPOSE : DTX texture reader/decoder. See ltdtx.h for the format, how it was
//           measured, and why the S3TC interpolation truncates.
//
// ----------------------------------------------------------------------- //

// ⚠️ Include order mirrors the game's own Stdafx.h -- HANDOVER.md §4.3.
#include "windows.h"
#include "lithtech.h"

#include "ltdtx.h"

#include <ctype.h>
#include <stdlib.h>
#include "ltfilemgr.h"

#include <cstdio>
#include <cstring>

static const unsigned int kHeaderSize = 164;
static const int          kDtxVersion = -5;

// ⚠️ char[15] + char[10] + uint32 with natural alignment = 32, NOT 29. Those
// three padding bytes are exactly what makes the one sectioned file's size
// come out right, so they are part of the format as shipped.
static const unsigned int kSectionHeaderSize = 32;

static const unsigned int kCommandStringLen = 128;   // DTX_COMMANDSTRING_LEN


// ----------------------------------------------------------------------- //
// little-endian readers -- the file is packed and offsets are not aligned
// ----------------------------------------------------------------------- //

static unsigned short RdU16(const unsigned char* p)
{
    return (unsigned short)(p[0] | (p[1] << 8));
}

static unsigned int RdU32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}


// ----------------------------------------------------------------------- //

CLTDtx::CLTDtx()
{
    Clear();
}

void CLTDtx::Clear()
{
    m_nAlphaDiscriminates = -1;
    m_bLoaded = false;
    m_sError.clear();
    m_File.clear();
    m_nWidth = m_nHeight = 0;
    m_eFormat = kUnknown;
    m_nIFlags = m_nUserFlags = 0;
    std::memset(m_Extra, 0, sizeof(m_Extra));
    m_sCommand.clear();
    m_Mips.clear();
    m_Sections.clear();
    m_pPalette = 0;
}

bool CLTDtx::Fail(const char* pWhy)
{
    std::string sKeep = pWhy ? pWhy : "unknown";
    Clear();
    m_sError = sKeep;
    return false;
}

const char* CLTDtx::GetFormatName() const
{
    switch (m_eFormat)
    {
        case kBPP_32:  return "BPP_32";
        case kDXT1:    return "DXT1";
        case kDXT3:    return "DXT3";
        case kDXT5:    return "DXT5";
        case kBPP_32P: return "BPP_32P";
        default:       return "unknown";
    }
}

const CLTDtx::Mip* CLTDtx::GetMip(unsigned int i) const
{
    return (i < m_Mips.size()) ? &m_Mips[i] : 0;
}

const unsigned char* CLTDtx::GetMipData(unsigned int i) const
{
    if (i >= m_Mips.size()) return 0;
    return &m_File[m_Mips[i].m_nOffset];
}

const char* CLTDtx::GetSectionType(unsigned int i) const
{
    return (i < m_Sections.size()) ? m_Sections[i].m_sType.c_str() : "";
}

float CLTDtx::GetDetailTextureScale() const
{
    // memcpy, not a cast: m_Extra[6] is not 4-byte aligned within the header.
    float f;
    std::memcpy(&f, &m_Extra[6], sizeof(f));
    return f + 1.0f;
}

int CLTDtx::GetDetailTextureAngle() const
{
    return (int)(short)(m_Extra[10] | (m_Extra[11] << 8));
}

unsigned int CLTDtx::MipBytes(unsigned int w, unsigned int h) const
{
    unsigned int bw = (w + 3) / 4; if (bw == 0) bw = 1;
    unsigned int bh = (h + 3) / 4; if (bh == 0) bh = 1;

    switch (m_eFormat)
    {
        case kDXT1:    return bw * bh * 8;
        case kDXT3:
        case kDXT5:    return bw * bh * 16;
        case kBPP_32:  return w * h * 4;
        case kBPP_32P: return w * h;          // 8-bit indices
        default:       return 0;
    }
}


// ----------------------------------------------------------------------- //
// Load
// ----------------------------------------------------------------------- //

bool CLTDtx::Load(const void* pData, unsigned int nSize)
{
    Clear();

    if (!pData || nSize < kHeaderSize)
        return Fail("not a DTX: shorter than a 164-byte header");

    const unsigned char* p = (const unsigned char*)pData;

    unsigned int nResType = RdU32(p + 0);
    int          nVersion = (int)RdU32(p + 4);

    // ⚠️ An equality test, exactly as the donor's CURRENT_DTX_VERSION is.
    // Every shipped file is -5; anything else is refused by name rather than
    // guessed at.
    if (nVersion != kDtxVersion)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DTX version %d, expected %d",
                      nVersion, kDtxVersion);
        return Fail(buf);
    }
    if (nResType != 0)
        return Fail("DTX m_ResType is not 0");

    m_nWidth  = RdU16(p + 8);
    m_nHeight = RdU16(p + 10);
    unsigned int nMipmaps  = RdU16(p + 12);
    unsigned int nSections = RdU16(p + 14);
    m_nIFlags    = (int)RdU32(p + 16);
    m_nUserFlags = (int)RdU32(p + 20);
    std::memcpy(m_Extra, p + 24, 12);

    {
        const char* pCmd = (const char*)(p + 36);
        unsigned int n = 0;
        while (n < kCommandStringLen && pCmd[n]) ++n;
        m_sCommand.assign(pCmd, n);
    }

    if (m_nWidth == 0 || m_nHeight == 0)
        return Fail("DTX has a zero dimension");
    if (m_nWidth > 8192 || m_nHeight > 8192)
        return Fail("DTX dimensions are implausible");
    if (nMipmaps == 0 || nMipmaps > 16)
        return Fail("DTX mipmap count is implausible");

    // ⚠️ GetBPPIdent() folds 0 to BPP_32 (donor dtxmgr_lib.h:76). Taking the
    // enum literally would call 605 files 8-bit palettised.
    switch (m_Extra[2])
    {
        case 0: case 3: m_eFormat = kBPP_32;  break;
        case 4:         m_eFormat = kDXT1;    break;
        case 5:         m_eFormat = kDXT3;    break;
        case 6:         m_eFormat = kDXT5;    break;
        case 7:         m_eFormat = kBPP_32P; break;
        default:
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "unknown DTX BPPIdent %u",
                          (unsigned int)m_Extra[2]);
            return Fail(buf);
        }
    }

    m_File.assign(p, p + nSize);

    // --- the mip chain ------------------------------------------------ //
    unsigned int o = kHeaderSize;
    unsigned int w = m_nWidth, h = m_nHeight;
    for (unsigned int i = 0; i < nMipmaps; ++i)
    {
        unsigned int ww = w ? w : 1;
        unsigned int hh = h ? h : 1;
        unsigned int n = MipBytes(ww, hh);
        if (n == 0)
            return Fail("no size rule for this DTX format");
        if (o + n > nSize || o + n < o)
            return Fail("DTX mip data runs past the end of the file");

        Mip m;
        m.m_nWidth = ww; m.m_nHeight = hh; m.m_nOffset = o; m.m_nSize = n;
        m_Mips.push_back(m);

        o += n;
        w >>= 1; h >>= 1;
    }

    // --- sections ----------------------------------------------------- //
    //
    // The donor's layout comment describes a "1 if more sections, 0 if no
    // more" flag between sections. ⚠️ The one sectioned file in the shipped
    // set does NOT carry it -- it has DTX_SECTIONSFIXED (1<<3) set, which is
    // exactly the flag meaning "the sections count was screwed up originally"
    // and is now authoritative. So m_nSections is trusted, and the file's
    // total size comes out exact, which is the check that says so.
    for (unsigned int i = 0; i < nSections; ++i)
    {
        if (o + kSectionHeaderSize > nSize)
            return Fail("DTX section header runs past the end of the file");

        Section s;
        {
            const char* pType = (const char*)(p + o);
            unsigned int n = 0; while (n < 15 && pType[n]) ++n;
            s.m_sType.assign(pType, n);
            const char* pName = (const char*)(p + o + 15);
            n = 0; while (n < 10 && pName[n]) ++n;
            s.m_sName.assign(pName, n);
        }
        unsigned int nDataLen = RdU32(p + o + 28);
        o += kSectionHeaderSize;

        if (o + nDataLen > nSize || o + nDataLen < o)
            return Fail("DTX section data runs past the end of the file");

        s.m_Data.assign(p + o, p + o + nDataLen);
        o += nDataLen;
        m_Sections.push_back(s);
    }

    // Resolve the palette after the vector has stopped growing -- a pointer
    // taken mid-push_back would dangle the moment it reallocated.
    m_pPalette = 0;
    for (size_t i = 0; i < m_Sections.size(); ++i)
    {
        if (m_Sections[i].m_sType == "PALLETE32" && m_Sections[i].m_Data.size() == 1024)
            m_pPalette = &m_Sections[i].m_Data[0];
    }

    if (m_eFormat == kBPP_32P && !m_pPalette)
        return Fail("BPP_32P texture without a 1024-byte PALLETE32 section");

    // ⚠️ THE CHECK THAT MADE THIS FORMAT TRACTABLE. The header predicts the
    // file's exact length; anything else means the model is wrong, not that
    // the file is merely odd. It is what caught a previous census reading
    // m_Extra from the wrong offset.
    if (o != nSize)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "DTX content ends at %u but the file is %u bytes "
                      "(off by %+d)", o, nSize, (int)(o - nSize));
        return Fail(buf);
    }

    m_bLoaded = true;
    return true;
}


// ----------------------------------------------------------------------- //
// S3TC decode
// ----------------------------------------------------------------------- //

static void Expand565(unsigned short v, unsigned char* pOut)
{
    unsigned int r = (v >> 11) & 31;
    unsigned int g = (v >> 5) & 63;
    unsigned int b = v & 31;
    pOut[0] = (unsigned char)((r << 3) | (r >> 2));
    pOut[1] = (unsigned char)((g << 2) | (g >> 4));
    pOut[2] = (unsigned char)((b << 3) | (b >> 2));
}

void CLTDtx::DecodeBlocks(const unsigned char* pSrc, unsigned int w,
                          unsigned int h, EFormat eFmt, unsigned char* pDest)
{
    unsigned int bw = (w + 3) / 4; if (bw == 0) bw = 1;
    unsigned int bh = (h + 3) / 4; if (bh == 0) bh = 1;
    unsigned int nBlockSize = (eFmt == kDXT1) ? 8 : 16;

    for (unsigned int by = 0; by < bh; ++by)
    {
        for (unsigned int bx = 0; bx < bw; ++bx)
        {
            const unsigned char* pBlk = pSrc + (by * bw + bx) * nBlockSize;
            const unsigned char* pCol = (eFmt == kDXT1) ? pBlk : pBlk + 8;

            unsigned short c0 = RdU16(pCol + 0);
            unsigned short c1 = RdU16(pCol + 2);
            unsigned int   nBits = RdU32(pCol + 4);

            unsigned char cols[4][4];
            Expand565(c0, cols[0]); cols[0][3] = 255;
            Expand565(c1, cols[1]); cols[1][3] = 255;

            // ⚠️ Compared as the raw 565 words, exactly as the donor does
            // (pixelformat.cpp: `if (val1 > val2)`).
            if (eFmt == kDXT1 && c0 <= c1)
            {
                for (int i = 0; i < 3; ++i)
                    cols[2][i] = (unsigned char)((cols[0][i] + cols[1][i]) >> 1);
                cols[2][3] = 255;
                cols[3][0] = cols[3][1] = cols[3][2] = 0;
                cols[3][3] = 0;             // fully transparent
            }
            else
            {
                // ⚠️ Truncating division -- see the header comment.
                for (int i = 0; i < 3; ++i)
                {
                    cols[2][i] = (unsigned char)((cols[0][i] * 2 + cols[1][i]) / 3);
                    cols[3][i] = (unsigned char)((cols[0][i] + cols[1][i] * 2) / 3);
                }
                cols[2][3] = cols[3][3] = 255;
            }

            // Alpha tables for the two alpha-bearing formats.
            unsigned long long nAlphaBits = 0;
            unsigned char aTable[8];
            if (eFmt == kDXT3)
            {
                for (int i = 0; i < 8; ++i)
                    nAlphaBits |= (unsigned long long)pBlk[i] << (8 * i);
            }
            else if (eFmt == kDXT5)
            {
                unsigned char a0 = pBlk[0], a1 = pBlk[1];
                for (int i = 0; i < 6; ++i)
                    nAlphaBits |= (unsigned long long)pBlk[2 + i] << (8 * i);
                aTable[0] = a0; aTable[1] = a1;
                if (a0 > a1)
                {
                    for (int i = 0; i < 6; ++i)
                        aTable[2 + i] = (unsigned char)(((6 - i) * a0 + (1 + i) * a1) / 7);
                }
                else
                {
                    for (int i = 0; i < 4; ++i)
                        aTable[2 + i] = (unsigned char)(((4 - i) * a0 + (1 + i) * a1) / 5);
                    aTable[6] = 0;
                    aTable[7] = 255;
                }
            }

            for (unsigned int py = 0; py < 4; ++py)
            {
                for (unsigned int px = 0; px < 4; ++px)
                {
                    unsigned int x = bx * 4 + px;
                    unsigned int y = by * 4 + py;
                    if (x >= w || y >= h) continue;

                    unsigned int i = py * 4 + px;
                    const unsigned char* c = cols[(nBits >> (2 * i)) & 3];

                    unsigned char* d = pDest + (y * w + x) * 4;
                    d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = c[3];

                    if (eFmt == kDXT3)
                    {
                        unsigned int a4 = (unsigned int)((nAlphaBits >> (4 * i)) & 0xF);
                        d[3] = (unsigned char)(a4 * 17);    // 4-bit -> 8-bit
                    }
                    else if (eFmt == kDXT5)
                    {
                        d[3] = aTable[(nAlphaBits >> (3 * i)) & 7];
                    }
                }
            }
        }
    }
}


bool CLTDtx::DecodeMip(unsigned int i, std::vector<unsigned char>& out) const
{
    out.clear();
    if (!m_bLoaded || i >= m_Mips.size()) return false;

    const Mip& m = m_Mips[i];
    const unsigned char* pSrc = &m_File[m.m_nOffset];
    out.resize((size_t)m.m_nWidth * m.m_nHeight * 4);
    if (out.empty()) return false;

    switch (m_eFormat)
    {
        case kDXT1:
        case kDXT3:
        case kDXT5:
            DecodeBlocks(pSrc, m.m_nWidth, m.m_nHeight, m_eFormat, &out[0]);
            return true;

        case kBPP_32:
        {
            // ⚠️ Stored BGRA -- measured, see the header comment.
            unsigned int n = m.m_nWidth * m.m_nHeight;
            for (unsigned int k = 0; k < n; ++k)
            {
                out[k*4 + 0] = pSrc[k*4 + 2];
                out[k*4 + 1] = pSrc[k*4 + 1];
                out[k*4 + 2] = pSrc[k*4 + 0];
                out[k*4 + 3] = pSrc[k*4 + 3];
            }
            return true;
        }

        case kBPP_32P:
        {
            if (!m_pPalette) return false;
            unsigned int n = m.m_nWidth * m.m_nHeight;
            for (unsigned int k = 0; k < n; ++k)
            {
                const unsigned char* e = m_pPalette + (unsigned int)pSrc[k] * 4;
                out[k*4 + 0] = e[2];
                out[k*4 + 1] = e[1];
                out[k*4 + 2] = e[0];
                out[k*4 + 3] = e[3];
            }
            return true;
        }

        default:
            return false;
    }
}


// ----------------------------------------------------------------------- //

bool CLTDtx::LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName)
{
    Clear();
    if (!pFileMgr || !pName || !pName[0])
        return Fail("no file manager or no name");

    // As given first, so a name that already ends in .dtx is not turned into
    // "....dtx.dtx" -- the same rule ltpcx.cpp and ltabc.cpp follow.
    ILTStream* pStream = pFileMgr->OpenFile(pName);
    if (!pStream)
    {
        std::string sTry(pName);
        sTry += ".dtx";
        pStream = pFileMgr->OpenFile(sTry.c_str());
    }
    if (!pStream)
    {
        std::string sWhy = "no tree holds '";
        sWhy += pName;
        sWhy += "'";
        return Fail(sWhy.c_str());
    }

    unsigned long nLen = 0;
    pStream->GetLen(&nLen);

    std::vector<unsigned char> buf((size_t)nLen);
    bool bRead = (nLen > 0) && (pStream->Read(&buf[0], nLen) == LT_OK);
    pStream->Release();

    if (!bRead)
        return Fail("could not read the texture out of its archive");

    return Load(&buf[0], (unsigned int)nLen);
}


// ----------------------------------------------------------------------- //
//
// AlphaDiscriminates
//
// ⚠️ Does this texture's alpha channel carry a SHAPE, i.e. does it contain both
// transparent and opaque texels? That is the question the alpha test actually
// asks, and it is the one the old format-based rule answered by proxy.
//
// ⚠️ It decodes mip 0, so it is not free -- hence the cached answer. Callers
// ask it once per texture at load, never per pixel.
//
// ⚠️ THE UNIFORM CASES BOTH RETURN FALSE, and the reason differs:
//   * all transparent -- 816 of the 1502 BPP_32 textures. Alpha-testing these
//     erases the surface completely; this is the case the previous rule
//     existed to avoid, and it is still avoided, now by measuring instead of
//     by guessing from the format.
//   * all opaque -- nothing would ever be discarded, so claiming alpha is
//     meaningful only costs the renderer a filter suppression it does not need.
//
// ----------------------------------------------------------------------- //

// ----------------------------------------------------------------------- //
//
// GetAlphaRef -- the threshold the TEXTURE asks for. See ltdtx.h for why this
// exists and what a hard-coded 128 was costing.
//
// ⚠️ CASE-INSENSITIVE AND POSITION-INDEPENDENT. The command string is authored
// by hand in DEdit and carries several commands; "alpharef" is not reliably
// first and its case is not reliable either. Matching only a leading lowercase
// "alpharef " would silently find nothing on some files, which is the failure
// mode this whole fix exists to remove.
//
// ----------------------------------------------------------------------- //

bool CLTDtx::AlphaIsMeaningful() const
{
    return IsCompressed() || DeclaresAlpha();
}


int CLTDtx::GetAlphaRef() const
{
    // ⚠️ LT_ALPHAREF=<0..255> OVERRIDES EVERY TEXTURE'S AUTHORED VALUE.
    //
    // A "force a value" switch, and it earns its place twice. It pins the
    // threshold so a change elsewhere can be neutralised and re-measured
    // without a rebuild -- and it is the only way to PROVE the value reaches
    // the Metal shader, because the shipped textures have near-binary alpha
    // (Lamp_02: 21.5% at 0, 76.4% at 255, 18 texels in between), so the real
    // authored values move almost no pixels. Forcing an extreme one makes the
    // two backends either both change or visibly disagree.
    {
        static const char* pForce = getenv("LT_ALPHAREF");
        if (pForce && *pForce)
        {
            int n = atoi(pForce);
            if (n < 0)   n = 0;
            if (n > 255) n = 255;
            return n;
        }
    }

    if (m_sCommand.empty()) return -1;

    std::string lc(m_sCommand);
    for (size_t i = 0; i < lc.size(); ++i)
        lc[i] = (char)tolower((unsigned char)lc[i]);

    const size_t at = lc.find("alpharef");
    if (at == std::string::npos) return -1;

    const char* p = m_sCommand.c_str() + at + 8;
    while (*p == ' ' || *p == '\t' || *p == '=') ++p;
    if (*p < '0' || *p > '9') return -1;

    const int n = atoi(p);

    // ⚠️ Clamped, not trusted blindly. The field is hand-authored text; a
    // value outside 0..255 cannot mean anything as an 8-bit reference, and
    // silently using it would give a surface that is either wholly invisible
    // or wholly opaque for a reason nobody could find.
    if (n < 0)   return 0;
    if (n > 255) return 255;
    return n;
}


bool CLTDtx::AlphaDiscriminates() const
{
    if (m_nAlphaDiscriminates >= 0) return m_nAlphaDiscriminates != 0;

    m_nAlphaDiscriminates = 0;
    if (!m_bLoaded) return false;

    std::vector<unsigned char> rgba;
    if (!DecodeMip(0, rgba) || rgba.size() < 4) return false;

    bool bAnyClear = false, bAnySolid = false;
    for (size_t i = 3; i < rgba.size(); i += 4)
    {
        if (rgba[i] < 128) bAnyClear = true;
        else               bAnySolid = true;
        if (bAnyClear && bAnySolid) { m_nAlphaDiscriminates = 1; return true; }
    }
    return false;
}
