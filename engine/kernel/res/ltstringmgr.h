// ----------------------------------------------------------------------- //
//
// MODULE  : ltstringmgr.h
//
// PURPOSE : HSTRING -- the engine's reference-counted string objects, and
//           FormatString, which is how the game reads ClientRes.
//
// THE CONTRACT, FROM THE SDK's OWN COMMENT (iltcsbase.h:161)
//
//   "Strings are reference counted objects that cannot be manipulated. When you
//    create one with FormatString or CreateString, the reference count is 1.
//    When you copy a string with CopyString, the reference count is
//    incremented. When you free one with FreeString, it decrements the
//    reference count.. when the reference count goes to zero, it deletes the
//    string. If you forget to free up any strings, LithTech will spit out a
//    message telling you about it.."
//
//   That last sentence is a feature, not a remark, so it is implemented:
//   Term() reports every string still alive. Leaks are the failure mode a
//   reference-counted API has, and the original engine chose to shout about
//   them.
//
// ⚠️ FormatString RETURNS NULL FOR AN UNKNOWN ID, AND THAT IS DELIBERATE
//
//   It is tempting to return an empty string so callers cannot crash --
//   CGameSettings::Init does `stricmp(GetStringData(FormatString(id)), "TRUE")`
//   with no NULL check. Resist it. The donor's own implementation returns
//   LTNULL when it cannot resolve the id (clientde_impl.cpp:1339), so NULL is
//   the authentic answer, and an empty string is data invented from nothing.
//   A missing id means the string table is wrong, and that should be loud.
//
// ⚠️ THE FORMAT LANGUAGE IS FormatMessage's, NOT printf's
//
//   "%1!s! %2!s! (the number is the argument number and the !s! says its a
//   string)" -- iltcsbase.h:170. Measured across all 6031 shipped strings:
//
//     46 strings use placeholders     conversions: !d! x46, !s! x19
//     0  have gaps in their numbering
//     1  is OUT OF ORDER: id 603, "Picked up %2!s! ( %1!d! )"
//
//   That single string out of 6031 is why substitution cannot be a left-to-
//   right walk of the va_list -- exactly the shape of FORMATS.md §5's "10.0f
//   occurs once in ~33,000 values". A naive implementation gets 6030 strings
//   right and hands an int to %s in the 6031st.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_STRINGMGR_H__
#define __LT_STRINGMGR_H__

#include <stdarg.h>

#include "ltbasedefs.h"
#include "ltperes.h"

class CLTFileMgr;

class CLTStringMgr
{
public:

    CLTStringMgr();
    ~CLTStringMgr();

    // Loads the string table out of a resource DLL, THROUGH the file manager
    // so the tree stack decides which copy wins -- GOTY's CRES.DLL beats the
    // base game's, the same way its Attributes\Missions.txt does.
    bool            LoadResourceDll(CLTFileMgr* pFileMgr, const char* pDllName);

    bool            HasTable() const { return m_Table.GetCount() > 0; }
    unsigned int    GetTableCount() const { return m_Table.GetCount(); }

    // --- the ILTCSBase surface ---------------------------------------- //

    HSTRING         Create(const char* pString);
    HSTRING         Copy(HSTRING hString);
    void            Free(HSTRING hString);
    char*           GetData(HSTRING hString);

    LTBOOL          Compare(HSTRING a, HSTRING b);
    LTBOOL          CompareUpper(HSTRING a, HSTRING b);

    HSTRING         Format(int nMessageCode, va_list args);

    // --- diagnostics --------------------------------------------------- //

    void            DumpCensus() const;
    void            RecordTotals() const;

    unsigned int    GetNumLive() const      { return m_nLive; }
    unsigned int    GetNumMissingIds() const { return m_nMissingIds; }

private:

    struct Str;

    Str*            Find(HSTRING h) const;

    // Substitutes %N!s! / %N!d! from `args`. Returns false if the format uses
    // something this cannot do, having said what.
    bool            Substitute(const char* pFmt, va_list args, std::string& sOut);

    CLTPEStringTable    m_Table;

    Str*                m_pLive;        // singly-linked, newest first
    unsigned int        m_nLive;
    unsigned int        m_nCreated;
    unsigned int        m_nMissingIds;
    unsigned int        m_nBadHandles;
};

CLTStringMgr* LTGetStringMgr();

// ⚠️ ILTClient / ILTServer GetStringData: a NULL handle answers "" -- the
// donor's rule (clientde_impl.cpp CLTClient::GetStringData). NOLF strcpy()s it
// unchecked: the server stops a character's lip sync with a NULL HSTRING
// (Character.cpp:1999) and NodeController.cpp:1044 copies its text. A handle
// the manager does not know answers "" too, rather than a crash in game code.
// ⚠️ CLIENT ONLY. The server's GetStringData answers NULL for NULL, as the
// reference does -- the game's head-skin lookup depends on it (ltserverhost.cpp).
inline char* LTStringData_ForGame(HSTRING h)
{
    static char s_szEmpty[1] = { 0 };
    char* p = h ? LTGetStringMgr()->GetData(h) : 0;
    return p ? p : s_szEmpty;
}

#endif // __LT_STRINGMGR_H__
