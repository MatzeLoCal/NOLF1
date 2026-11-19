// ----------------------------------------------------------------------- //
//
// dumpstrings.cpp -- writes the game's string table (CRES.DLL, resolved
//                    through the tree stack like the engine does) as UTF-8,
//                    one string per line:  <id> TAB <text>
//
//   Newlines, tabs and backslashes inside a string are escaped as \n \t \\,
//   which is the same escaping the translation loader (ltstringmgr.cpp)
//   reads back.
//
//   usage: dumpstrings <out.txt> <game-dir> [<game-dir> ...]
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltperes.h"
#include "ltfilemgr.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>

// CP1252 0x80-0x9F, the same table as ltperes.cpp
static const unsigned short kCP1252_80_9F[32] =
{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static void AppendUtf8(std::string& s, unsigned int c)
{
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: dumpstrings <out.txt> <game-dir> [<game-dir> ...]\n");
        return 2;
    }

    const char* pDirs[8];
    int nDirs = 0;
    for (int i = 2; i < argc && nDirs < 8; ++i) pDirs[nDirs++] = argv[i];

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(pDirs, nDirs) == 0) { fprintf(stderr, "no game data mounted\n"); return 2; }

    // Same path as CLTStringMgr::LoadResourceDll.
    ILTStream* pStream = fm.OpenFile("CRES.DLL");
    if (!pStream) { fprintf(stderr, "CRES.DLL not found\n"); return 1; }
    unsigned long nLen = 0;
    pStream->GetLen(&nLen);
    std::vector<unsigned char> buf(nLen);
    bool bRead = (nLen > 0) && (pStream->Read(&buf[0], nLen) == LT_OK);
    pStream->Release();
    if (!bRead) { fprintf(stderr, "CRES.DLL could not be read\n"); return 1; }

    CLTPEStringTable table;
    if (!table.Load(&buf[0], (unsigned int)nLen)) { fprintf(stderr, "CRES.DLL is not a readable PE\n"); return 1; }

    std::vector<int> ids;
    table.GetIds(ids);
    std::sort(ids.begin(), ids.end());

    FILE* out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); return 1; }
    for (int id : ids)
    {
        const unsigned char* p = (const unsigned char*)table.Find(id);
        std::string line;
        for (; *p; ++p)
        {
            unsigned int c = *p;
            if (c == '\n') line += "\\n";
            else if (c == '\r') line += "\\r";
            else if (c == '\t') line += "\\t";
            else if (c == '\\') line += "\\\\";
            else if (c >= 0x80 && c <= 0x9F) AppendUtf8(line, kCP1252_80_9F[c - 0x80]);
            else AppendUtf8(line, c);
        }
        fprintf(out, "%d\t%s\n", id, line.c_str());
    }
    fclose(out);
    fprintf(stderr, "%u strings -> %s\n", (unsigned)ids.size(), argv[1]);
    return 0;
}
