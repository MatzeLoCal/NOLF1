// ----------------------------------------------------------------------- //
//
// MODULE  : ltrezfile.h
//
// PURPOSE : Read-only reader for a RezMgr v1 archive (`.rez`).
//
// PROVENANCE
//
//   The on-disk format is taken from the donor's own writer/reader,
//   Engine/lithtech/libs/rezmgr/rezmgr.cpp -- specifically the #pragma pack(1)
//   structures FileMainHeaderStruct / FileDirEntryHeader at rezmgr.cpp:56-108,
//   and CRezMgr::StrToType / TypeToStr at rezmgr.cpp:1658. Per PORT_PLAN.md §2
//   the REZ format is an approved direct donor: NOLF 1's archives are RezMgr
//   v1, which is the format Jupiter still writes.
//
//   The CODE is new. The donor's CRezMgr is a read/write manager with hash
//   tables, an ObjectBank allocator, LT_MEM_TRACK_ALLOC and an error-throwing
//   convention -- ~69 KB of source, nearly all of it the write path and none of
//   it needed to read a shipped archive. What is reproduced here is the format
//   and the LOOKUP SEMANTICS, which are the parts that have to match exactly.
//
// THE ONE STRUCTURAL SURPRISE
//
//   A resource entry does NOT store "FX.TXT". It stores the name "FX" and a
//   separate uint32 `Type` holding the extension -- packed BACKWARDS:
//
//       StrToType("TXT"):  pType[2]='T'  pType[1]='X'  pType[0]='T'
//
//   so the four bytes, read in file order on a little-endian host, spell the
//   extension reversed. Reconstructing "NAME.EXT" is therefore part of reading
//   the directory, not a formatting nicety.
//
// LOOKUP SEMANTICS, AND WHY THIS FLATTENS THE TREE
//
//   The donor resolves a path by walking directories, then finding the type,
//   then finding the name (CRezDir::GetRezFromDosPath -> GetDirFromPath ->
//   GetRezFromDosName -> GetRez). This class instead flattens the whole
//   directory tree once, at open, into one map keyed by the full uppercased DOS
//   path ("ATTRIBUTES\FX.TXT").
//
//   That is equivalent, not merely similar, because the donor's own lookup is
//   case-insensitive at every level:
//     * names   -- CRezDir::GetRez calls Find(sRes, !GetLowerCaseUsed()), and
//                  m_bLowerCaseUsed is FALSE, so bIgnoreCase is TRUE;
//     * extensions -- GetRezFromDosName strupr()s the extension before packing;
//     * callers -- CHelpers::FormatFilename uppercases the whole path and
//                  converts '/' to '\\' before any file request is made.
//
//   ⚠️ ONE DELIBERATE WIDENING. The donor compares the packed type as a uint32,
//   so an archive storing a lowercase extension would not match an uppercased
//   query. This reader uppercases the reconstructed extension when building the
//   key, so such an entry WOULD match. Every extension in all four shipped
//   archives is already uppercase, so the two behaviours are identical on real
//   data; the widening can only ever add a match, never change one.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_REZFILE_H__
#define __LT_REZFILE_H__

#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>

class CLTRezArchive
{
public:

    struct Entry
    {
        unsigned int    m_Pos;      // byte offset of the payload in the archive
        unsigned int    m_Size;     // payload length
        unsigned int    m_Time;     // last-modified stamp, as stored
    };

    CLTRezArchive();
    ~CLTRezArchive();

    // Opens and fully indexes the archive. Returns false and leaves the object
    // closed if the file is missing, truncated, or not RezMgr v1.
    //
    // ⚠️ Indexing is eager and complete. The donor can load one directory at a
    // time; doing that here would trade a measurable one-off cost (the largest
    // shipped archive indexes 4871 entries) for a lazy path that has to be
    // right in two places instead of one.
    bool                Open(const char* pPath);
    void                Close();
    bool                IsOpen() const { return m_pFile != 0; }

    // pName is a DOS-style relative path ("Attributes\\FX.txt"). Case and
    // separator style do not matter -- it is normalised here.
    // Returns 0 if the archive does not contain it.
    const Entry*        Find(const char* pName) const;

    // Enumeration, in the order the directory stream lists them.
    unsigned int        GetNumEntries() const { return (unsigned int)m_Entries.size(); }
    const char*         GetEntryName(unsigned int i) const { return m_Entries[i].first.c_str(); }
    const Entry&        GetEntry(unsigned int i) const { return m_Entries[i].second; }

    const char*         GetPath() const { return m_Path.c_str(); }
    const char*         GetFileType() const { return m_FileType.c_str(); }
    const char*         GetUserTitle() const { return m_UserTitle.c_str(); }

    // Reads `size` bytes at `pos` in the archive. Returns bytes actually read.
    // Every stream over this archive uses its own FILE*, so this is only used
    // for one-shot reads.
    unsigned int        ReadAt(unsigned int pos, void* pData, unsigned int size);

    // Uppercases and converts '/' to '\\', matching CHelpers::FormatFilename.
    static void         NormalisePath(const char* pIn, std::string& sOut);

private:

    // Recursively reads one directory block. Returns false on a malformed
    // block -- which is reported, never ignored: a directory that fails to
    // parse would otherwise look exactly like an archive that simply lacks
    // those files.
    bool                ReadDirectory(unsigned int pos, unsigned int size,
                                      const std::string& sPrefix, int nDepth);

    FILE*                                       m_pFile;
    std::string                                 m_Path;
    std::string                                 m_FileType;
    std::string                                 m_UserTitle;

    std::vector<std::pair<std::string, Entry> > m_Entries;
    std::unordered_map<std::string, unsigned int> m_Index;   // key -> m_Entries idx
};

#endif // __LT_REZFILE_H__
