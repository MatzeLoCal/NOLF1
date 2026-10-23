// ----------------------------------------------------------------------- //
//
// MODULE  : ltperes.cpp
//
// PURPOSE : PE resource STRINGTABLE reader. See ltperes.h for why the shipped
//           DLL is the source and why the output encoding is CP1252.
//
// ----------------------------------------------------------------------- //

#include "ltperes.h"

#include <stdio.h>
#include <string.h>
#include <vector>


// ----------------------------------------------------------------------- //
// PE layout, from the published format. Read field by field rather than by
// casting structs over the buffer -- a struct cast assumes this host's
// alignment and endianness, and the whole point is that the file was written
// by a different one.
// ----------------------------------------------------------------------- //

#define RT_STRING           6
#define DIR_ENTRY_IS_SUBDIR 0x80000000u
#define MAX_RES_DEPTH       8

static unsigned int rd16(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd32(const unsigned char* p)
{
    return  (unsigned int)p[0]
         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}


namespace
{
    struct Section
    {
        unsigned int m_VirtualAddress;
        unsigned int m_VirtualSize;
        unsigned int m_RawPtr;
        unsigned int m_RawSize;
    };

    struct Image
    {
        const unsigned char*    m_p;
        unsigned int            m_nSize;
        std::vector<Section>    m_Sections;

        // Every read goes through here. A resource directory is a graph of
        // file offsets read FROM the file, so a corrupt or hostile image can
        // point anywhere; bounds-checking at the single choke point is the
        // only way that stays true as the parser grows.
        bool In(unsigned int off, unsigned int len) const
        {
            return off <= m_nSize && len <= m_nSize - off;
        }

        // RVA -> file offset. Returns 0xFFFFFFFF when the RVA is in no section.
        unsigned int Rva2Off(unsigned int rva) const
        {
            for (size_t i = 0; i < m_Sections.size(); ++i)
            {
                const Section& s = m_Sections[i];
                unsigned int span = s.m_VirtualSize > s.m_RawSize
                                  ? s.m_VirtualSize : s.m_RawSize;
                if (rva >= s.m_VirtualAddress && rva < s.m_VirtualAddress + span)
                    return s.m_RawPtr + (rva - s.m_VirtualAddress);
            }
            return 0xFFFFFFFFu;
        }
    };
}


CLTPEStringTable::CLTPEStringTable()
    : m_nLossy(0)
{
}

void CLTPEStringTable::Clear()
{
    m_Strings.clear();
    m_nLossy = 0;
}

const char* CLTPEStringTable::Find(int nId) const
{
    std::unordered_map<int, std::string>::const_iterator it = m_Strings.find(nId);
    return (it == m_Strings.end()) ? 0 : it->second.c_str();
}

void CLTPEStringTable::GetIds(std::vector<int>& out) const
{
    out.clear();
    out.reserve(m_Strings.size());
    for (std::unordered_map<int, std::string>::const_iterator it = m_Strings.begin();
         it != m_Strings.end(); ++it)
        out.push_back(it->first);
}


// ----------------------------------------------------------------------- //
// UTF-16 -> CP1252.
//
// CP1252 is Latin-1 except for 0x80-0x9F, which Latin-1 leaves as control
// codes and CP1252 fills with typography. That range is not decoration here:
// every one of the 16 non-ASCII strings in NOLF 1's table needs either it or
// the Latin-1 part, and dropping it would silently turn ‘em into em.
// ----------------------------------------------------------------------- //

static const unsigned short kCP1252_80_9F[32] =
{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

void CLTPEStringTable::Utf16ToCP1252(const unsigned short* pIn, unsigned int nChars,
                                     std::string& sOut, unsigned int* pnLossy)
{
    sOut.clear();
    sOut.reserve(nChars);

    for (unsigned int i = 0; i < nChars; ++i)
    {
        unsigned short c = pIn[i];

        if (c < 0x80 || (c >= 0xA0 && c <= 0xFF))
        {
            sOut.push_back((char)(unsigned char)c);
            continue;
        }

        bool bFound = false;
        for (int k = 0; k < 32; ++k)
        {
            if (kCP1252_80_9F[k] == c)
            {
                sOut.push_back((char)(unsigned char)(0x80 + k));
                bFound = true;
                break;
            }
        }
        if (bFound) continue;

        // ⚠️ '?' is what WideCharToMultiByte substitutes for an unmappable
        // character, so this matches the original rather than inventing a
        // policy -- but it is COUNTED, because a silent lossy conversion is
        // how a localisation bug hides for months.
        sOut.push_back('?');
        if (pnLossy) ++(*pnLossy);
    }
}


// ----------------------------------------------------------------------- //

bool CLTPEStringTable::ParseBlock(const unsigned char* p, unsigned int nSize,
                                  unsigned int nBlockId)
{
    // ⚠️ A string block holds EXACTLY 16 strings, and their ids are implied by
    // position: id = (blockId - 1) * 16 + index. An empty slot is a zero
    // length, not an absent entry -- so the index has to be advanced for empty
    // slots too, or every string after the first gap lands on the wrong id.
    unsigned int o = 0;
    for (unsigned int i = 0; i < 16; ++i)
    {
        if (o + 2 > nSize) break;
        unsigned int n = rd16(p + o);
        o += 2;

        if (n == 0) continue;
        if (o + n * 2 > nSize) return false;

        std::string s;
        CLTPEStringTable::Utf16ToCP1252((const unsigned short*)(p + o), n, s, &m_nLossy);
        o += n * 2;

        m_Strings[(int)((nBlockId - 1) * 16 + i)] = s;
    }
    return true;
}


bool CLTPEStringTable::Load(const void* pData, unsigned int nSize)
{
    Clear();

    Image img;
    img.m_p     = (const unsigned char*)pData;
    img.m_nSize = nSize;

    if (!pData || nSize < 0x40) return false;
    if (img.m_p[0] != 'M' || img.m_p[1] != 'Z') return false;

    unsigned int e_lfanew = rd32(img.m_p + 0x3C);
    if (!img.In(e_lfanew, 24)) return false;
    if (memcmp(img.m_p + e_lfanew, "PE\0\0", 4) != 0) return false;

    unsigned int coff      = e_lfanew + 4;
    unsigned int nSections = rd16(img.m_p + coff + 2);
    unsigned int optSize   = rd16(img.m_p + coff + 16);
    unsigned int opt       = coff + 20;

    if (!img.In(opt, optSize) || optSize < 2) return false;

    unsigned int magic = rd16(img.m_p + opt);
    unsigned int ddOff;
    if      (magic == 0x10b) ddOff = opt + 96;      // PE32
    else if (magic == 0x20b) ddOff = opt + 112;     // PE32+
    else return false;

    // Data directory entry 2 is the resource table.
    if (!img.In(ddOff + 8 * 2, 8)) return false;
    unsigned int resRva = rd32(img.m_p + ddOff + 8 * 2);
    if (resRva == 0) return false;

    unsigned int secTable = opt + optSize;
    for (unsigned int i = 0; i < nSections; ++i)
    {
        unsigned int o = secTable + i * 40;
        if (!img.In(o, 40)) return false;

        Section s;
        s.m_VirtualSize    = rd32(img.m_p + o + 8);
        s.m_VirtualAddress = rd32(img.m_p + o + 12);
        s.m_RawSize        = rd32(img.m_p + o + 16);
        s.m_RawPtr         = rd32(img.m_p + o + 20);
        img.m_Sections.push_back(s);
    }

    unsigned int resBase = img.Rva2Off(resRva);
    if (resBase == 0xFFFFFFFFu || !img.In(resBase, 16)) return false;

    // --- walk type -> name -> language ------------------------------- //
    //
    // Three fixed levels, so this is written as three loops rather than a
    // recursion. Depth is a property of the format, not of the data, and
    // spelling it out makes the level each id comes from unmistakable.

    unsigned int nNamed = rd16(img.m_p + resBase + 12);
    unsigned int nId    = rd16(img.m_p + resBase + 14);

    for (unsigned int i = 0; i < nNamed + nId; ++i)
    {
        unsigned int e = resBase + 16 + i * 8;
        if (!img.In(e, 8)) return false;

        unsigned int type = rd32(img.m_p + e);
        unsigned int off  = rd32(img.m_p + e + 4);

        if (type & DIR_ENTRY_IS_SUBDIR) continue;   // a NAMED type, not RT_STRING
        if (type != RT_STRING) continue;
        if (!(off & DIR_ENTRY_IS_SUBDIR)) continue;

        unsigned int lvl2 = resBase + (off & ~DIR_ENTRY_IS_SUBDIR);
        if (!img.In(lvl2, 16)) return false;

        unsigned int n2Named = rd16(img.m_p + lvl2 + 12);
        unsigned int n2Id    = rd16(img.m_p + lvl2 + 14);

        for (unsigned int j = 0; j < n2Named + n2Id; ++j)
        {
            unsigned int e2 = lvl2 + 16 + j * 8;
            if (!img.In(e2, 8)) return false;

            unsigned int blk  = rd32(img.m_p + e2) & ~DIR_ENTRY_IS_SUBDIR;
            unsigned int off2 = rd32(img.m_p + e2 + 4);
            if (!(off2 & DIR_ENTRY_IS_SUBDIR)) continue;
            if (blk == 0) continue;                 // id 0 would underflow below

            unsigned int lvl3 = resBase + (off2 & ~DIR_ENTRY_IS_SUBDIR);
            if (!img.In(lvl3, 16)) return false;

            unsigned int n3 = (unsigned int)rd16(img.m_p + lvl3 + 12)
                            + (unsigned int)rd16(img.m_p + lvl3 + 14);
            if (n3 == 0) continue;

            // ⚠️ FIRST LANGUAGE ONLY. NOLF 1 ships one language per DLL and
            // swaps the whole file for a localised build (that is what
            // nolfl.rez is for), so taking the first is correct here. If a
            // multi-language DLL ever turns up, this silently picks one --
            // which is why it says so out loud rather than looking deliberate.
            unsigned int e3   = lvl3 + 16;
            if (!img.In(e3, 8)) return false;
            unsigned int leaf = rd32(img.m_p + e3 + 4);
            if (leaf & DIR_ENTRY_IS_SUBDIR) continue;

            unsigned int dat = resBase + leaf;
            if (!img.In(dat, 16)) return false;

            unsigned int dataRva  = rd32(img.m_p + dat);
            unsigned int dataSize = rd32(img.m_p + dat + 4);

            unsigned int dataOff = img.Rva2Off(dataRva);
            if (dataOff == 0xFFFFFFFFu || !img.In(dataOff, dataSize)) continue;

            ParseBlock(img.m_p + dataOff, dataSize, blk);
        }
    }

    return !m_Strings.empty();
}
