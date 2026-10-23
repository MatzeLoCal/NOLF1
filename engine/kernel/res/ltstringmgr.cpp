// ----------------------------------------------------------------------- //
//
// MODULE  : ltstringmgr.cpp
//
// PURPOSE : HSTRING and FormatString. See ltstringmgr.h for the reference-
//           counting contract and for why the format language is
//           FormatMessage's rather than printf's.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltstringmgr.h"
#include "ltfilemgr.h"
#include "ltrecord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <string>
#include <vector>


// ----------------------------------------------------------------------- //
// One string.
//
// ⚠️ HSTRING is `struct {int __nothing;}*`, an opaque pointer type, so this
// object's address IS the handle. Find() still walks the live list before
// dereferencing: the game frees strings, and a stale handle would otherwise be
// a wild read with no diagnostic. That walk is why the count is tracked -- if
// it ever gets long enough to matter, the number will say so first.
// ----------------------------------------------------------------------- //

struct CLTStringMgr::Str
{
    std::string     m_Text;
    int             m_nRefs;
    Str*            m_pNext;
};


CLTStringMgr::CLTStringMgr()
    : m_pLive(0)
    , m_nLive(0)
    , m_nCreated(0)
    , m_nMissingIds(0)
    , m_nBadHandles(0)
{
}

CLTStringMgr::~CLTStringMgr()
{
    while (m_pLive)
    {
        Str* pNext = m_pLive->m_pNext;
        delete m_pLive;
        m_pLive = pNext;
    }
}


bool CLTStringMgr::LoadResourceDll(CLTFileMgr* pFileMgr, const char* pDllName)
{
    if (!pFileMgr || !pDllName) return false;

    ILTStream* pStream = pFileMgr->OpenFile(pDllName);
    if (!pStream)
    {
        fprintf(stderr, "[LTSTR] ⚠️  %s not found -- no string table\n", pDllName);
        return false;
    }

    unsigned long nLen = 0;
    pStream->GetLen(&nLen);

    // The whole DLL, in memory. It is ~750 KB and the resource directory is a
    // graph of offsets into it, so streaming would mean seeking constantly for
    // no benefit at this size.
    std::vector<unsigned char> buf(nLen);
    bool bRead = (nLen > 0) && (pStream->Read(&buf[0], nLen) == LT_OK);
    pStream->Release();

    if (!bRead)
    {
        fprintf(stderr, "[LTSTR] ⚠️  %s could not be read (%lu bytes)\n", pDllName, nLen);
        return false;
    }

    if (!m_Table.Load(&buf[0], (unsigned int)nLen))
    {
        fprintf(stderr, "[LTSTR] ⚠️  %s is not a PE image with a string table\n", pDllName);
        return false;
    }

    LTRecord_Event("res", "%s %u strings", pDllName, m_Table.GetCount());

    if (m_Table.GetLossyCount())
    {
        // Counted rather than ignored: a lossy conversion is invisible in the
        // output and shows up months later as one wrong character in one line.
        fprintf(stderr, "[LTSTR] ⚠️  %u character(s) had no CP1252 spelling\n",
                m_Table.GetLossyCount());
        LTRecord_Event("res", "lossy %u", m_Table.GetLossyCount());
    }

    return true;
}


// ----------------------------------------------------------------------- //
// Reference counting.
// ----------------------------------------------------------------------- //

CLTStringMgr::Str* CLTStringMgr::Find(HSTRING h) const
{
    if (!h) return 0;
    for (Str* p = m_pLive; p; p = p->m_pNext)
        if ((HSTRING)p == h) return p;
    return 0;
}

HSTRING CLTStringMgr::Create(const char* pString)
{
    Str* p = new Str;
    p->m_Text  = pString ? pString : "";
    p->m_nRefs = 1;
    p->m_pNext = m_pLive;
    m_pLive = p;

    ++m_nLive;
    ++m_nCreated;

    return (HSTRING)p;
}

HSTRING CLTStringMgr::Copy(HSTRING hString)
{
    // "Copy a string.. much more efficient than CreateString()" -- it shares
    // the object and bumps the count, which is the whole point of the design.
    Str* p = Find(hString);
    if (!p) { ++m_nBadHandles; return 0; }

    ++p->m_nRefs;
    return hString;
}

void CLTStringMgr::Free(HSTRING hString)
{
    if (!hString) return;               // freeing NULL is legal and common

    Str* pFound = Find(hString);
    if (!pFound) { ++m_nBadHandles; return; }

    if (--pFound->m_nRefs > 0) return;

    Str** pp = &m_pLive;
    while (*pp && *pp != pFound) pp = &(*pp)->m_pNext;
    if (!*pp) return;

    *pp = pFound->m_pNext;
    delete pFound;
    --m_nLive;
}

char* CLTStringMgr::GetData(HSTRING hString)
{
    Str* p = Find(hString);
    if (!p) { ++m_nBadHandles; return 0; }

    // ⚠️ Returns a pointer INTO the live object, which is what the SDK
    // promises ("you really should only use this for strings that you stored
    // off"). It stays valid until the last reference is freed -- so nothing
    // here may ever reallocate m_Text after creation, and nothing does.
    return const_cast<char*>(p->m_Text.c_str());
}

LTBOOL CLTStringMgr::Compare(HSTRING a, HSTRING b)
{
    Str* pa = Find(a);
    Str* pb = Find(b);
    if (!pa || !pb) return LTFALSE;
    return (pa->m_Text == pb->m_Text) ? LTTRUE : LTFALSE;
}

LTBOOL CLTStringMgr::CompareUpper(HSTRING a, HSTRING b)
{
    Str* pa = Find(a);
    Str* pb = Find(b);
    if (!pa || !pb) return LTFALSE;
    return (strcasecmp(pa->m_Text.c_str(), pb->m_Text.c_str()) == 0) ? LTTRUE : LTFALSE;
}


// ----------------------------------------------------------------------- //
// FormatMessage-style substitution.
//
// ⚠️ TWO PASSES, AND THE REASON IS ONE STRING IN 6031.
//
// A va_list cannot be indexed -- there is no portable way to ask for argument
// 2 without having walked argument 1. So the format is scanned FIRST to learn
// how many arguments there are and what type each one is; only then is the
// va_list walked once, in order, into an array; only then is the text built.
//
// A left-to-right implementation (fetch an argument each time a placeholder is
// met) is simpler, passes on 6030 of the shipped strings, and hands an int to
// %s on id 603, "Picked up %2!s! ( %1!d! )".
// ----------------------------------------------------------------------- //

#define LTSTR_MAX_ARGS 8

bool CLTStringMgr::Substitute(const char* pFmt, va_list args, std::string& sOut)
{
    sOut.clear();
    if (!pFmt) return false;

    // --- pass 1: what does this format ask for? ----------------------- //

    char aType[LTSTR_MAX_ARGS + 1];
    memset(aType, 0, sizeof(aType));
    int nMax = 0;

    for (const char* p = pFmt; *p; ++p)
    {
        if (*p != '%') continue;
        if (p[1] == '%') { ++p; continue; }
        if (!isdigit((unsigned char)p[1])) continue;

        const char* q = p + 1;
        int nArg = 0;
        while (isdigit((unsigned char)*q)) { nArg = nArg * 10 + (*q - '0'); ++q; }

        if (*q != '!') continue;
        ++q;
        char conv = *q;
        if (!conv || q[1] != '!') continue;

        if (nArg < 1 || nArg > LTSTR_MAX_ARGS)
        {
            fprintf(stderr, "[LTSTR] ⚠️  argument index %d out of range in \"%s\"\n",
                    nArg, pFmt);
            return false;
        }
        if (conv != 's' && conv != 'd')
        {
            // ⚠️ Measured: the shipped table uses only !s! and !d!. `!f!` would
            // need a double, which on AAPCS64 travels in a DIFFERENT register
            // bank from integers and pointers -- so it cannot just be added to
            // the loop below. Refuse loudly rather than read the wrong bank.
            fprintf(stderr, "[LTSTR] ⚠️  unsupported conversion !%c! in \"%s\"\n",
                    conv, pFmt);
            return false;
        }

        aType[nArg] = conv;
        if (nArg > nMax) nMax = nArg;
        p = q + 1;
    }

    // --- pass 2: pull the arguments, IN ORDER ------------------------- //

    const char* aStr[LTSTR_MAX_ARGS + 1] = { 0 };
    int         aInt[LTSTR_MAX_ARGS + 1] = { 0 };

    for (int i = 1; i <= nMax; ++i)
    {
        // A gap means the format never said what argument i is, and skipping
        // it would misalign everything after. Measured: zero gaps in the
        // shipped table -- so this is a guard against a future string, and it
        // says so instead of guessing a width.
        if (aType[i] == 0)
        {
            fprintf(stderr, "[LTSTR] ⚠️  argument %d is never referenced in \"%s\";"
                            " cannot know its type\n", i, pFmt);
            return false;
        }
        if (aType[i] == 's') aStr[i] = va_arg(args, const char*);
        else                 aInt[i] = va_arg(args, int);
    }

    // --- pass 3: build ------------------------------------------------ //

    for (const char* p = pFmt; *p; ++p)
    {
        if (*p != '%') { sOut.push_back(*p); continue; }
        if (p[1] == '%') { sOut.push_back('%'); ++p; continue; }
        if (!isdigit((unsigned char)p[1])) { sOut.push_back(*p); continue; }

        const char* q = p + 1;
        int nArg = 0;
        while (isdigit((unsigned char)*q)) { nArg = nArg * 10 + (*q - '0'); ++q; }

        if (*q != '!' || !q[1] || q[2] != '!')
        {
            sOut.push_back(*p);         // not a placeholder after all
            continue;
        }

        if (aType[nArg] == 's')
        {
            const char* s = aStr[nArg];
            sOut += s ? s : "";
        }
        else
        {
            char num[32];
            snprintf(num, sizeof(num), "%d", aInt[nArg]);
            sOut += num;
        }

        p = q + 2;
    }

    return true;
}


HSTRING CLTStringMgr::Format(int nMessageCode, va_list args)
{
    const char* pFmt = m_Table.Find(nMessageCode);
    if (!pFmt)
    {
        // ⚠️ NULL, not "". See ltstringmgr.h -- the donor returns LTNULL here
        // and an invented empty string would hide a wrong string table.
        ++m_nMissingIds;
        LTRecord_Event("res", "MISSING id %d", nMessageCode);
        return 0;
    }

    // No placeholders is the common case (142 of 188 call sites pass no
    // arguments), and it must not walk the va_list at all.
    if (!strchr(pFmt, '%'))
        return Create(pFmt);

    std::string s;
    if (!Substitute(pFmt, args, s))
        return Create(pFmt);            // report happened inside; keep the text

    return Create(s.c_str());
}


// ----------------------------------------------------------------------- //

void CLTStringMgr::DumpCensus() const
{
    fprintf(stderr, "\n===== strings =====\n");
    fprintf(stderr, "  table     : %u string(s)\n", m_Table.GetCount());
    fprintf(stderr, "  created   : %u, %u still live\n", m_nCreated, m_nLive);

    if (m_nMissingIds)
        fprintf(stderr, "  ⚠️  %u request(s) for an id the table does not have\n",
                m_nMissingIds);
    if (m_nBadHandles)
        fprintf(stderr, "  ⚠️  %u operation(s) on an unknown HSTRING\n", m_nBadHandles);

    // "If you forget to free up any strings, LithTech will spit out a message
    // telling you about it.." -- iltcsbase.h:166. That is authored behaviour,
    // so it is reproduced rather than left as a comment.
    if (m_nLive)
    {
        fprintf(stderr, "  ⚠️  %u STRING(S) LEAKED:\n", m_nLive);
        int n = 0;
        for (Str* p = m_pLive; p && n < 10; p = p->m_pNext, ++n)
            fprintf(stderr, "        refs=%d \"%.48s\"\n", p->m_nRefs, p->m_Text.c_str());
        if (m_nLive > 10)
            fprintf(stderr, "        ... and %u more\n", m_nLive - 10);
    }

    fprintf(stderr, "===================\n");
    fflush(stderr);
}

void CLTStringMgr::RecordTotals() const
{
    LTRecord_Event("count", "strings-table %u",   m_Table.GetCount());
    LTRecord_Event("count", "strings-created %u", m_nCreated);
    LTRecord_Event("count", "strings-leaked %u",  m_nLive);
    LTRecord_Event("count", "strings-missing %u", m_nMissingIds);
}


static CLTStringMgr g_StringMgr;

CLTStringMgr* LTGetStringMgr()
{
    return &g_StringMgr;
}
