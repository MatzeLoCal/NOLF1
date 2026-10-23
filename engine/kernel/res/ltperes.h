// ----------------------------------------------------------------------- //
//
// MODULE  : ltperes.h
//
// PURPOSE : Reads the STRINGTABLE out of a Win32 PE resource DLL.
//
//           This is ClientRes: `g_pLTClient->FormatString(nStringID)` is an
//           ENGINE call, so the engine has to be able to serve NOLF 1's string
//           table. 192 call sites, 673 distinct IDS_* constants.
//
// ⚠️ WHY THE SHIPPED DLL AND NOT THE `.rc` IN THE TREE
//
//   Both were parsed and compared, because "which source is authoritative" is
//   a measurement, not a preference (PORT_PLAN.md §5):
//
//     ids only in NOLF/ClientRes/ClientRes.rc     0
//     ids only in the shipped CRES.DLL           75
//     ids in both                              5956
//
//   Every `.rc` id is in the DLL and the DLL has 75 more -- GOTY strings the
//   mod SDK's copy never had. The `.rc` is a strict subset, so using it would
//   mean 75 silently missing strings.
//
//   ⚠️ And there is more than one CRES.DLL. The tree stack resolves it the
//   same way it resolves everything else, so `startup.txt` order decides:
//
//     NOLF2.REZ      741376 bytes   5876 strings   (base game)
//     NOLFGOTY.REZ   774144 bytes   6031 strings   (GOTY -- wins)
//
//   Reading it through CLTFileMgr rather than off the disk is what makes that
//   automatic.
//
// ⚠️ ENCODING: UTF-16 IN, CP1252 OUT
//
//   PE string resources are UTF-16LE; the game wants `char*`. Measured across
//   all 6031: 16 strings contain characters above 126, and the highest is
//   U+201D. They are German umlauts (ß, ü, ö) and typographic quotes.
//
//   The conversion is CP1252, not Latin-1 and not "drop anything non-ASCII",
//   because CP1252 is what WideCharToMultiByte produced for the English and
//   European builds this game shipped as -- so it reproduces the bytes the
//   original engine handed the original game code. Latin-1 alone would lose
//   every smart quote (U+2018..U+201D live in CP1252's 0x80-0x9F range, which
//   Latin-1 leaves as control codes).
//
// ----------------------------------------------------------------------- //

#ifndef __LT_PERES_H__
#define __LT_PERES_H__

#include <string>
#include <unordered_map>
#include <vector>

class CLTPEStringTable
{
public:

    CLTPEStringTable();

    // Parses a whole PE image held in memory. Returns false and stays empty on
    // anything that is not a readable PE32/PE32+ with a resource section.
    bool                Load(const void* pData, unsigned int nSize);

    void                Clear();

    // Returns 0 if the id is not in the table. ⚠️ NOT "" -- see the note in
    // ltstringmgr.h about why an absent string must stay distinguishable from
    // an empty one.
    const char*         Find(int nId) const;

    unsigned int        GetCount() const { return (unsigned int)m_Strings.size(); }

    // Enumeration, for the cross-check harness.
    void                GetIds(std::vector<int>& out) const;

    // UTF-16LE -> CP1252, the conversion the original Win32 build performed.
    static void         Utf16ToCP1252(const unsigned short* pIn, unsigned int nChars,
                                      std::string& sOut, unsigned int* pnLossy);

    unsigned int        GetLossyCount() const { return m_nLossy; }

private:

    bool                ParseBlock(const unsigned char* p, unsigned int nSize,
                                   unsigned int nBlockId);

    std::unordered_map<int, std::string> m_Strings;
    unsigned int        m_nLossy;   // characters that had no CP1252 spelling
};

#endif // __LT_PERES_H__
