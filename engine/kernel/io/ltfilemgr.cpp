// ----------------------------------------------------------------------- //
//
// MODULE  : ltfilemgr.cpp
//
// PURPOSE : The engine's file system. See ltfilemgr.h for the search-order
//           rule and where it comes from.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltfilemgr.h"
#include "ltrezfile.h"
#include "ltfilestream.h"
#include "ltrecord.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

#include <sys/stat.h>
#include <dirent.h>

#include <algorithm>


// ----------------------------------------------------------------------- //
// Path helpers.
// ----------------------------------------------------------------------- //

static bool PathIsDir(const char* pPath)
{
    struct stat st;
    if (stat(pPath, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool PathIsFile(const char* pPath)
{
    struct stat st;
    if (stat(pPath, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}


// ⚠️ THE SEPARATOR CONVERSION RUNS THE OTHER WAY HERE.
//
// CHelpers::FormatFilename (libs/stdlith/helpers.cpp:225) converts '/' to '\\'
// and uppercases -- that is the form every game caller produces, and the form
// a rez archive is keyed on. To touch the real filesystem the same name has to
// go back the other way. The two conversions are not inverses: case is lost on
// the way in, which is why the on-disk lookup below has to be prepared to be
// case-insensitive rather than just swapping slashes back.
static void DosToPosix(const char* pIn, std::string& sOut)
{
    sOut.clear();
    if (!pIn) return;
    for (const char* p = pIn; *p; ++p)
        sOut.push_back(*p == '\\' ? '/' : *p);
}


// The last n components of a path, for the run record -- enough to identify a
// directory without embedding the machine it lives on.
static std::string TailPath(const std::string& sPath, int nComponents)
{
    size_t pos = sPath.size();
    for (int i = 0; i < nComponents; ++i)
    {
        if (pos == 0) break;
        size_t slash = sPath.rfind('/', pos - 1);
        if (slash == std::string::npos) { pos = 0; break; }
        pos = slash;
    }
    return (pos == 0) ? sPath : sPath.substr(pos + 1);
}


// Resolves one path component inside a directory, ignoring case. Returns the
// real on-disk spelling, or an empty string.
static std::string ResolveComponentNoCase(const std::string& sDir, const std::string& sName)
{
    DIR* pDir = opendir(sDir.c_str());
    if (!pDir) return std::string();

    std::string sFound;
    struct dirent* pEnt;
    while ((pEnt = readdir(pDir)) != 0)
    {
        if (strcasecmp(pEnt->d_name, sName.c_str()) == 0)
        {
            sFound = pEnt->d_name;
            break;
        }
    }
    closedir(pDir);
    return sFound;
}


// Turns a tree-relative DOS name into a real path under sBase, ignoring case.
// Returns an empty string if no such file exists.
//
// The fast path is a plain stat(): macOS volumes are usually case-insensitive,
// in which case the first attempt succeeds and nothing is enumerated. The slow
// path exists because "usually" is not "always", and a case-sensitive volume
// would otherwise turn every uppercased game path into a missing file.
static std::string ResolveOnDisk(const std::string& sBase, const char* pDosName)
{
    std::string sRel;
    DosToPosix(pDosName, sRel);

    std::string sFull = sBase + "/" + sRel;
    if (PathIsFile(sFull.c_str()))
        return sFull;

    // Walk it a component at a time, matching case-insensitively.
    std::string sCur = sBase;
    size_t i = 0;
    while (i < sRel.size())
    {
        size_t j = sRel.find('/', i);
        if (j == std::string::npos) j = sRel.size();

        std::string sComp = sRel.substr(i, j - i);
        i = (j < sRel.size()) ? j + 1 : j;

        if (sComp.empty() || sComp == ".") continue;

        std::string sReal = ResolveComponentNoCase(sCur, sComp);
        if (sReal.empty()) return std::string();

        sCur += "/";
        sCur += sReal;
    }

    return PathIsFile(sCur.c_str()) ? sCur : std::string();
}


// ----------------------------------------------------------------------- //
// CLTFileMgr
// ----------------------------------------------------------------------- //

CLTFileMgr::CLTFileMgr()
    : m_nOpened(0), m_nNotFound(0)
{
}

CLTFileMgr::~CLTFileMgr()
{
    Term();
}

void CLTFileMgr::Term()
{
    for (size_t i = 0; i < m_Trees.size(); ++i)
        delete m_Trees[i].m_pRez;
    m_Trees.clear();
}


bool CLTFileMgr::AddTree(const char* pPath)
{
    if (!pPath || !pPath[0]) return false;

    Tree t;
    t.m_Path = pPath;
    t.m_pRez = 0;

    if (PathIsDir(pPath))
    {
        t.m_Type = kDosTree;
    }
    else if (PathIsFile(pPath))
    {
        t.m_Type = kRezTree;
        t.m_pRez = new CLTRezArchive;
        if (!t.m_pRez->Open(pPath))
        {
            // Exists but is not a RezMgr v1 archive. Reported, not ignored --
            // an unreadable archive and an absent one produce the same missing
            // files later, and only this line distinguishes them.
            fprintf(stderr, "[LTFILE] not a RezMgr v1 archive: %s\n", pPath);
            delete t.m_pRez;
            return false;
        }
    }
    else
    {
        return false;
    }

    m_Trees.push_back(t);

    // The tree stack is part of the contract: the same run against a different
    // set of archives is a different run, and without this it would look
    // identical right up until a file resolved differently.
    //
    // ⚠️ Directory trees are recorded with their LAST TWO path components,
    // archives with just the basename. A basename alone is ambiguous for
    // directories -- the GOTY data lives in two directories both called `Data`,
    // and two identical lines in a contract are two lines that cannot fail
    // independently. This does tie the reference record to a particular data
    // layout, which is already true of it (see harness/README.md).
    LTRecord_Event("tree", "%s %s %u",
                   t.m_Type == kRezTree ? "rez" : "dir",
                   t.m_Type == kRezTree ? LTRecord_BaseName(t.m_Path.c_str())
                                        : TailPath(t.m_Path, 2).c_str(),
                   t.m_pRez ? t.m_pRez->GetNumEntries() : 0u);

    return true;
}


int CLTFileMgr::InitFromGameDirs(const char* const* pDirs, int nDirs)
{
    int nAdded = 0;

    // The directories themselves, first, so they end up LOWEST priority.
    for (int i = 0; i < nDirs; ++i)
    {
        if (PathIsDir(pDirs[i]) && AddTree(pDirs[i]))
            ++nAdded;
    }

    // ------------------------------------------------------------------ //
    // startup.txt is the authored answer to "which archives, in what order".
    // PORT_PLAN.md §5: authored data beats heuristics -- go find where the
    // original engine reads it rather than inventing an ordering.
    //
    //     [Rez]
    //     Rez0 = "nolf"
    //     Rez1 = "nolf.rez"
    //     ...
    // ------------------------------------------------------------------ //

    std::vector<std::string> names;

    for (int i = 0; i < nDirs && names.empty(); ++i)
    {
        std::string sStartup = ResolveOnDisk(pDirs[i], "startup.txt");
        if (sStartup.empty()) continue;

        FILE* fp = fopen(sStartup.c_str(), "rb");
        if (!fp) continue;

        fprintf(stderr, "[LTFILE] reading tree list from %s\n", sStartup.c_str());

        char line[512];
        while (fgets(line, sizeof(line), fp))
        {
            const char* p = strchr(line, '=');
            if (!p) continue;
            if (strncasecmp(line, "Rez", 3) != 0) continue;

            // Value is quoted: Rez1 = "nolf.rez"
            const char* q1 = strchr(p, '"');
            if (!q1) continue;
            const char* q2 = strchr(q1 + 1, '"');
            if (!q2) continue;

            names.push_back(std::string(q1 + 1, (size_t)(q2 - q1 - 1)));
        }
        fclose(fp);
    }

    if (names.empty())
    {
        // No startup.txt anywhere. Fall back to every .rez in each directory,
        // sorted, which at least gives a deterministic order -- and say so,
        // because the override chain is then a guess.
        fprintf(stderr, "[LTFILE] ⚠️  no startup.txt found; falling back to *.rez by name\n");

        for (int i = 0; i < nDirs; ++i)
        {
            DIR* pDir = opendir(pDirs[i]);
            if (!pDir) continue;

            std::vector<std::string> found;
            struct dirent* pEnt;
            while ((pEnt = readdir(pDir)) != 0)
            {
                const char* pDot = strrchr(pEnt->d_name, '.');
                if (pDot && strcasecmp(pDot, ".rez") == 0)
                    found.push_back(pEnt->d_name);
            }
            closedir(pDir);

            std::sort(found.begin(), found.end());
            for (size_t k = 0; k < found.size(); ++k)
            {
                std::string sFull = std::string(pDirs[i]) + "/" + found[k];
                if (AddTree(sFull.c_str())) ++nAdded;
            }
        }

        return nAdded;
    }

    // Resolve each authored name against every game directory, in file order.
    for (size_t k = 0; k < names.size(); ++k)
    {
        bool bAdded = false;

        for (int i = 0; i < nDirs && !bAdded; ++i)
        {
            // A tree can be a directory ("nolf") as well as an archive, so try
            // the plain join before the file-only resolver.
            std::string sJoin = std::string(pDirs[i]) + "/" + names[k];
            if (PathIsDir(sJoin.c_str()))
            {
                if (AddTree(sJoin.c_str())) { bAdded = true; break; }
            }

            std::string sFull = ResolveOnDisk(pDirs[i], names[k].c_str());
            if (!sFull.empty() && AddTree(sFull.c_str()))
                bAdded = true;
        }

        if (bAdded) ++nAdded;
        else        fprintf(stderr, "[LTFILE] tree not found (skipped): %s\n", names[k].c_str());
    }

    return nAdded;
}


const char* CLTFileMgr::GetTreePath(int i) const
{
    if (i < 0 || i >= (int)m_Trees.size()) return "";
    return m_Trees[i].m_Path.c_str();
}

CLTFileMgr::ETreeType CLTFileMgr::GetTreeType(int i) const
{
    if (i < 0 || i >= (int)m_Trees.size()) return kDosTree;
    return m_Trees[i].m_Type;
}

unsigned int CLTFileMgr::GetTreeNumEntries(int i) const
{
    if (i < 0 || i >= (int)m_Trees.size()) return 0;
    return m_Trees[i].m_pRez ? m_Trees[i].m_pRez->GetNumEntries() : 0;
}


ILTStream* CLTFileMgr::OpenInTree(const Tree& tree, const char* pName)
{
    if (tree.m_Type == kRezTree)
    {
        const CLTRezArchive::Entry* pEntry = tree.m_pRez->Find(pName);
        if (!pEntry) return 0;

        // A separate handle per stream -- see ltfilestream.h.
        FILE* fp = fopen(tree.m_Path.c_str(), "rb");
        if (!fp) return 0;

        return CLTFileStream::Create(fp, pEntry->m_Pos, pEntry->m_Size);
    }

    std::string sFull = ResolveOnDisk(tree.m_Path, pName);
    if (sFull.empty()) return 0;

    FILE* fp = fopen(sFull.c_str(), "rb");
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    long nLen = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (nLen < 0) { fclose(fp); return 0; }

    return CLTFileStream::Create(fp, 0, (unsigned long)nLen);
}


// LT_TRACE_FILE=1 names every successful open and the tree it came from.
//
// ⚠️ This is the instrument that shows the OVERRIDE CHAIN actually firing.
// "0 files missing" only proves a stream was produced; it says nothing about
// WHICH copy of Attributes\FX.txt was served, and the patched copy differs from
// the base one by 5 KB of content. Two probes, not one (PORT_PLAN.md §5).
static int LTFile_TraceEnabled()
{
    static int s = -1;
    if (s < 0) s = getenv("LT_TRACE_FILE") ? 1 : 0;
    return s;
}

ILTStream* CLTFileMgr::OpenFile(const char* pFilename)
{
    if (!pFilename || !pFilename[0]) return 0;

    // ⚠️ Reverse of add order. See ltfilemgr.h.
    for (int i = (int)m_Trees.size() - 1; i >= 0; --i)
    {
        ILTStream* pStream = OpenInTree(m_Trees[i], pFilename);
        if (pStream)
        {
            ++m_nOpened;

            if (LTFile_TraceEnabled() || LTRecord_Enabled())
            {
                unsigned long nLen = 0;
                pStream->GetLen(&nLen);

                const char* pTree = LTRecord_BaseName(m_Trees[i].m_Path.c_str());

                if (LTFile_TraceEnabled())
                    fprintf(stderr, "[LTFILE] %-40s %8lu bytes  <- %s\n",
                            pFilename, nLen, pTree);

                // ⚠️ SIZE AND SOURCE TREE, not just the name. The name alone
                // would have passed even while the wrong archive was serving
                // the file -- and the whole point of the tree stack is which
                // copy wins. `Fx.txt` from nolf2.rez and from nolfu003.rez
                // differ by 5003 bytes, and only this line can tell them apart.
                LTRecord_Event("file", "%s %lu %s", pFilename, nLen, pTree);
            }

            return pStream;
        }
    }

    ++m_nNotFound;
    LTRecord_Event("miss", "%s", pFilename);
    return 0;
}


bool CLTFileMgr::FileExists(const char* pFilename)
{
    ILTStream* pStream = OpenFile(pFilename);
    if (!pStream) return false;
    pStream->Release();
    return true;
}


bool CLTFileMgr::CopyFileTo(const char* pSrcName, const char* pDestPath)
{
    if (!pSrcName || !pDestPath) return false;

    ILTStream* pSrc = OpenFile(pSrcName);
    if (!pSrc) return false;

    unsigned long nLen = 0;
    pSrc->GetLen(&nLen);

    FILE* pOut = fopen(pDestPath, "wb");
    if (!pOut)
    {
        pSrc->Release();
        return false;
    }

    bool bOk = true;
    char buf[4096];
    unsigned long nLeft = nLen;
    while (nLeft > 0)
    {
        unsigned long nChunk = (nLeft < sizeof(buf)) ? nLeft : (unsigned long)sizeof(buf);
        if (pSrc->Read(buf, nChunk) != LT_OK)      { bOk = false; break; }
        if (fwrite(buf, 1, nChunk, pOut) != nChunk) { bOk = false; break; }
        nLeft -= nChunk;
    }

    fclose(pOut);
    pSrc->Release();

    if (!bOk) remove(pDestPath);
    return bOk;
}


// ----------------------------------------------------------------------- //

static CLTFileMgr g_FileMgr;

CLTFileMgr* LTGetFileMgr()
{
    return &g_FileMgr;
}
