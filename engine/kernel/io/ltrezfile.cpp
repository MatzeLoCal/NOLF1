// ----------------------------------------------------------------------- //
//
// MODULE  : ltrezfile.cpp
//
// PURPOSE : RezMgr v1 archive reader. See ltrezfile.h for the format's
//           provenance and for why the directory tree is flattened.
//
// ----------------------------------------------------------------------- //

#include "ltrezfile.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// --------------------------------------------------------------------- //
// The on-disk layout, from rezmgr.cpp:56-108 under #pragma pack(1).
//
// ⚠️ THIS IS READ FIELD BY FIELD, NOT AS A STRUCT. Declaring the packed
// structs and fread()ing them would work today and is exactly the shape of
// defect PORT_PLAN.md §3 is about: the header's ten counters are DWORD, which
// was 32-bit on Win32 and is 32-bit here only because the port fixed the
// typedefs. Reading four bytes at a time and assembling them makes the width
// a property of this file rather than of a typedef somewhere else.
//
//   char CR1, LF1;                     2
//   char FileType[60];                60
//   char CR2, LF2;                     2
//   char UserTitle[60];               60
//   char CR3, LF3, EOF1;               3      -- 127 so far
//   DWORD FileFormatVersion;                  -- must be 1
//   DWORD RootDirPos, RootDirSize, RootDirTime;
//   DWORD NextWritePos, Time;
//   DWORD LargestKeyAry, LargestDirNameSize;
//   DWORD LargestRezNameSize, LargestCommentSize;   -- 10 DWORDs = 40
//   BYTE  IsSorted;                            1   -- 168 total
// --------------------------------------------------------------------- //

static const unsigned int kRezHeaderSize    = 168;
static const unsigned int kRezTitleSize     = 60;
static const unsigned int kRezFormatVersion = 1;

// Directory-entry type codes (FileDirEntryType, rezmgr.cpp:81).
static const unsigned int kEntryResource  = 0;
static const unsigned int kEntryDirectory = 1;

// A malformed archive must not be able to spin this forever.
static const int kMaxDirDepth = 32;


static unsigned int rd32(const unsigned char* p)
{
    return  (unsigned int)p[0]
         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}


// TypeToStr, rezmgr.cpp:1677. The four bytes spell the extension BACKWARDS;
// the length is however many trailing bytes are non-zero.
static void TypeToExt(unsigned int nType, char sExt[5])
{
    unsigned char b[4];
    b[0] = (unsigned char)( nType        & 0xFF);
    b[1] = (unsigned char)((nType >>  8) & 0xFF);
    b[2] = (unsigned char)((nType >> 16) & 0xFF);
    b[3] = (unsigned char)((nType >> 24) & 0xFF);

    int nLen = 0;
    if      (b[3]) nLen = 4;
    else if (b[2]) nLen = 3;
    else if (b[1]) nLen = 2;
    else if (b[0]) nLen = 1;

    for (int i = 0; i < nLen; ++i)
        sExt[i] = (char)b[nLen - 1 - i];
    sExt[nLen] = '\0';
}


// ----------------------------------------------------------------------- //

CLTRezArchive::CLTRezArchive()
    : m_pFile(0)
{
}

CLTRezArchive::~CLTRezArchive()
{
    Close();
}

void CLTRezArchive::Close()
{
    if (m_pFile)
    {
        fclose(m_pFile);
        m_pFile = 0;
    }
    m_Path.clear();
    m_FileType.clear();
    m_UserTitle.clear();
    m_Entries.clear();
    m_Index.clear();
}


void CLTRezArchive::NormalisePath(const char* pIn, std::string& sOut)
{
    sOut.clear();
    if (!pIn) return;

    // Mirrors CHelpers::FormatFilename (libs/stdlith/helpers.cpp:225) on the
    // Windows branch: uppercase, and '/' becomes '\\'.
    for (const char* p = pIn; *p; ++p)
    {
        char c = *p;
        if (c == '/') c = '\\';
        sOut.push_back((char)toupper((unsigned char)c));
    }

    // A leading separator is stripped by CRezDir::GetRezFromDosPath before it
    // does anything else (rezmgr.cpp:2113).
    if (sOut.size() > 1 && sOut[0] == '\\')
        sOut.erase(sOut.begin());
}


bool CLTRezArchive::Open(const char* pPath)
{
    Close();
    if (!pPath || !pPath[0]) return false;

    m_pFile = fopen(pPath, "rb");
    if (!m_pFile) return false;

    unsigned char hdr[kRezHeaderSize];
    if (fread(hdr, 1, kRezHeaderSize, m_pFile) != kRezHeaderSize)
    {
        Close();
        return false;
    }

    // The two title strings are fixed-width and space-padded.
    {
        char buf[kRezTitleSize + 1];

        memcpy(buf, hdr + 2, kRezTitleSize);  buf[kRezTitleSize] = '\0';
        m_FileType = buf;
        while (!m_FileType.empty() && isspace((unsigned char)m_FileType.back()))
            m_FileType.pop_back();

        memcpy(buf, hdr + 64, kRezTitleSize); buf[kRezTitleSize] = '\0';
        m_UserTitle = buf;
        while (!m_UserTitle.empty() && isspace((unsigned char)m_UserTitle.back()))
            m_UserTitle.pop_back();
    }

    unsigned int nVersion  = rd32(hdr + 127);
    unsigned int nRootPos  = rd32(hdr + 131);
    unsigned int nRootSize = rd32(hdr + 135);

    // ⚠️ An EQUALITY test, exactly as the donor does. A "version >= 1" test
    // would silently accept a format this code cannot read -- the same shape
    // of mistake FORMATS.md §3/§4 records for ABC and for worlds.
    if (nVersion != kRezFormatVersion)
    {
        Close();
        return false;
    }

    m_Path = pPath;

    if (!ReadDirectory(nRootPos, nRootSize, std::string(), 0))
    {
        Close();
        return false;
    }

    return true;
}


bool CLTRezArchive::ReadDirectory(unsigned int pos, unsigned int size,
                                  const std::string& sPrefix, int nDepth)
{
    if (size == 0)  return true;     // an empty directory is legal
    if (nDepth >= kMaxDirDepth) return false;

    if (fseek(m_pFile, (long)pos, SEEK_SET) != 0) return false;

    std::vector<unsigned char> blk(size);
    if (fread(&blk[0], 1, size, m_pFile) != size) return false;

    // Sub-directories are collected and recursed into AFTER this block is
    // fully parsed -- recursing inline would move the file position out from
    // under the loop.
    struct SubDir { unsigned int pos, size; std::string prefix; };
    std::vector<SubDir> subdirs;

    unsigned int o = 0;
    while (o < size)
    {
        if (o + 4 > size) return false;
        unsigned int nType = rd32(&blk[o]); o += 4;

        if (nType == kEntryDirectory)
        {
            if (o + 12 > size) return false;
            unsigned int dPos  = rd32(&blk[o]);
            unsigned int dSize = rd32(&blk[o + 4]);
            o += 12;                                  // pos, size, time

            unsigned int nameStart = o;
            while (o < size && blk[o] != 0) ++o;
            if (o >= size) return false;              // unterminated name
            std::string sName((const char*)&blk[nameStart], o - nameStart);
            ++o;

            SubDir sd;
            sd.pos    = dPos;
            sd.size   = dSize;
            sd.prefix = sPrefix + sName + "\\";
            subdirs.push_back(sd);
        }
        else if (nType == kEntryResource)
        {
            if (o + 24 > size) return false;
            unsigned int rPos   = rd32(&blk[o]);
            unsigned int rSize  = rd32(&blk[o + 4]);
            unsigned int rTime  = rd32(&blk[o + 8]);
            unsigned int rExt   = rd32(&blk[o + 16]);   // ID at +12, Type at +16
            unsigned int nKeys  = rd32(&blk[o + 20]);
            o += 24;

            unsigned int nameStart = o;
            while (o < size && blk[o] != 0) ++o;
            if (o >= size) return false;
            std::string sName((const char*)&blk[nameStart], o - nameStart);
            ++o;

            // The comment/description string, which is skipped.
            while (o < size && blk[o] != 0) ++o;
            if (o >= size) return false;
            ++o;

            // Then nKeys uint32 key values.
            if ((unsigned long)o + (unsigned long)nKeys * 4 > (unsigned long)size)
                return false;
            o += nKeys * 4;

            char sExt[5];
            TypeToExt(rExt, sExt);

            std::string sFull = sPrefix + sName;
            if (sExt[0])
            {
                sFull += '.';
                sFull += sExt;
            }

            std::string sKey;
            NormalisePath(sFull.c_str(), sKey);

            Entry e;
            e.m_Pos  = rPos;
            e.m_Size = rSize;
            e.m_Time = rTime;

            // ⚠️ FIRST WINS within one archive. A duplicate key inside a single
            // archive is not something the shipped files do; keeping the first
            // makes the outcome defined rather than dependent on scan order.
            if (m_Index.find(sKey) == m_Index.end())
            {
                m_Index[sKey] = (unsigned int)m_Entries.size();
                m_Entries.push_back(std::make_pair(sKey, e));
            }
        }
        else
        {
            // Not a type this format defines. Stop rather than resynchronise:
            // guessing where the next record starts turns a detectable problem
            // into a plausible-looking wrong answer.
            return false;
        }
    }

    for (size_t i = 0; i < subdirs.size(); ++i)
    {
        if (!ReadDirectory(subdirs[i].pos, subdirs[i].size,
                           subdirs[i].prefix, nDepth + 1))
            return false;
    }

    return true;
}


const CLTRezArchive::Entry* CLTRezArchive::Find(const char* pName) const
{
    if (!m_pFile || !pName) return 0;

    std::string sKey;
    NormalisePath(pName, sKey);

    std::unordered_map<std::string, unsigned int>::const_iterator it =
        m_Index.find(sKey);
    if (it == m_Index.end()) return 0;

    return &m_Entries[it->second].second;
}


unsigned int CLTRezArchive::ReadAt(unsigned int pos, void* pData, unsigned int size)
{
    if (!m_pFile || !pData) return 0;
    if (fseek(m_pFile, (long)pos, SEEK_SET) != 0) return 0;
    return (unsigned int)fread(pData, 1, size, m_pFile);
}
