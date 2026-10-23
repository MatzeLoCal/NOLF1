// ----------------------------------------------------------------------- //
//
// MODULE  : ltconparse.cpp
//
// See ltconparse.h for the syntax and for why a stub here cost eight worlds.
//
// ⚠️ THIS FILE ALSO DEFINES ConParse::Parse AND ConParse::ParseFind. They are
// declared in ltbasedefs.h with no body -- the ENGINE is expected to supply
// them, and until now nothing did, so any translation unit that called one
// would not have linked. They are members, so they can reach m_pCommandPos and
// m_ArgBuffer, which is exactly why they belong here rather than in a helper.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltconparse.h"

#include <string.h>

namespace
{
    const char kQuote   = '"';
    const char kEscape  = '%';

    // ⚠️ NOT `iscntrl`. The original engine carries a note that some release
    // builds of iscntrl answer true for -127, which is a legitimate byte in
    // multi-byte (e.g. Japanese) text -- so a localised trigger string would
    // be cut in half at a character that is not a separator at all. Testing
    // the range directly cannot do that, and `unsigned char` keeps the
    // comparison from going negative on this platform's signed char.
    inline bool IsCtrl(char c)
    {
        const unsigned char u = (unsigned char)c;
        return u <= 31 || u == 127;
    }

    inline bool IsSeparator(char c) { return c == ';' || IsCtrl(c); }

    enum ETok { kTok_None = 0, kTok_Got, kTok_Semicolon };

    // Pull one token out of *ppPos into *ppOut, both advanced in place.
    // pOutEnd is one past the last byte the token buffer owns.
    ETok NextToken(const char*& pPos, char*& pOut, char* pOutEnd)
    {
        int nParens = 0;
        bool bEnd = false;

        while (*pPos == ' ') ++pPos;
        if (*pPos == 0) return kTok_None;

        char* const pTokenStart = pOut;

        // ⚠️ Room for this character AND its terminator, checked before every
        // write. The original truncates against PARSE_MAXARGLEN (256) while
        // every caller in the game supplies a PARSE_MAXTOKENS*PARSE_MAXTOKENSIZE
        // buffer -- 30 tokens of 256 would be 7680 bytes in a 5120-byte array.
        // Bounding by the buffer the caller actually gave cannot overflow it,
        // whatever the token limit is.
        for (;;)
        {
            char c = *pPos;
            bool bAdd = false;

            if (c == 0)
            {
                bEnd = true;
            }
            else if (c == kEscape)
            {
                // Whatever follows is a literal, including ';' and ')'.
                ++pPos;
                c = *pPos;
                if (c == 0) bEnd = true;
                else        bAdd = true;
            }
            else if (IsSeparator(c))
            {
                if (pOut < pOutEnd) *pOut = 0;
                ++pOut;

                if (pTokenStart[0] == 0)
                {
                    // ⚠️ Step PAST the separator, or the next call sees the
                    // same ';' and the caller's `while (bMore)` never ends.
                    ++pPos;
                    return kTok_Semicolon;
                }
                return kTok_Got;
            }
            else if (c == '(')
            {
                // The FIRST paren opens a group and is dropped; deeper ones
                // are content, so `(a (b) c)` yields the token `a (b) c`.
                if (++nParens != 1) bAdd = true;
            }
            else if (c == ')')
            {
                if (--nParens <= 0) { ++pPos; bEnd = true; }
                else                bAdd = true;
            }
            else if (c == kQuote)
            {
                // A quote opens a group or closes every open one.
                if (nParens) { ++pPos; bEnd = true; }
                else         nParens = 1;
            }
            else if (c == ' ' && nParens == 0)
            {
                bEnd = true;
            }
            else
            {
                bAdd = true;
            }

            if (bAdd)
            {
                if (pOut + 1 < pOutEnd) *pOut++ = c;
                // Out of room: the character is dropped but the token keeps
                // being consumed, so the stream stays in step.
            }

            if (bEnd)
            {
                if (pOut < pOutEnd) *pOut = 0;
                ++pOut;
                return pTokenStart[0] ? kTok_Got : kTok_None;
            }

            ++pPos;
        }
    }
}


int LTConParse(const char* pCommand,
               const char** ppNewPos,
               char* pArgBuffer, size_t nArgBufferBytes,
               char** ppArgs, int nMaxArgs,
               int* pnArgs)
{
    // ⚠️ THE OUTPUTS FIRST, BEFORE ANY CHANCE OF AN EARLY RETURN. Every
    // caller in the game reads them without checking the result.
    static char s_Empty[1] = { 0 };

    if (pnArgs)   *pnArgs   = 0;
    if (ppNewPos) *ppNewPos = pCommand;
    if (ppArgs && nMaxArgs > 0) ppArgs[0] = s_Empty;

    if (!pCommand || !pArgBuffer || !ppArgs || !pnArgs || nMaxArgs <= 0 ||
        nArgBufferBytes == 0)
        return 0;

    const char* pPos    = pCommand;
    char*       pOut    = pArgBuffer;
    char* const pOutEnd = pArgBuffer + nArgBufferBytes;

    for (;;)
    {
        char* const pToken = pOut;
        const ETok e = NextToken(pPos, pOut, pOutEnd);

        if (e == kTok_None)
            return 0;                       // nothing left

        if (e == kTok_Semicolon)
        {
            if (ppNewPos) *ppNewPos = pPos;
            return 1;                       // more commands follow
        }

        // ⚠️ A token that did not fit is DROPPED rather than pointed at past
        // the end of the buffer. Handing the game a pointer outside the array
        // it supplied is how a parser turns a long trigger string into a
        // crash somewhere else entirely.
        if (pToken < pOutEnd)
            ppArgs[*pnArgs] = pToken;
        else
            ppArgs[*pnArgs] = s_Empty;

        if (++(*pnArgs) >= nMaxArgs)
            break;
    }

    if (ppNewPos) *ppNewPos = pPos;
    return 0;
}


// ----------------------------------------------------------------------- //
// ConParse -- the SDK's own wrapper, whose bodies the engine owes.
// ----------------------------------------------------------------------- //

LTBOOL ConParse::Parse()
{
    // ⚠️ m_nArgs and m_Args are cleared by LTConParse on every path, so a
    // ConParse that parses nothing still reads as empty rather than as
    // whatever the last command left in it.
    if (!m_pCommandPos) return LTFALSE;

    const char* pNext = 0;
    if (!LTConParse(m_pCommandPos, &pNext,
                    m_ArgBuffer, sizeof(m_ArgBuffer),
                    m_Args, PARSE_MAXTOKENS, &m_nArgs))
    {
        // ⚠️ NULL means "there is no next command", which is what makes the
        // game's `while (Parse())` terminate. This call still returns TRUE:
        // the LAST command has been parsed and its tokens are valid, and the
        // loop ends on the call after it. Returning FALSE here would silently
        // drop every level's final command.
        m_pCommandPos = LTNULL;
    }
    else
    {
        m_pCommandPos = (char*)pNext;
    }

    return LTTRUE;
}


LTBOOL ConParse::ParseFind(char* pLookFor, LTBOOL bCaseSensitive, uint32 minTokens)
{
    if (!pLookFor) return LTFALSE;
    if (minTokens == 0) minTokens = 1;

    while (Parse())
    {
        if (m_nArgs < (int)minTokens) continue;

        const bool bEqual = bCaseSensitive
                          ? (strcmp(m_Args[0], pLookFor) == 0)
                          : (strcasecmp(m_Args[0], pLookFor) == 0);
        if (bEqual) return LTTRUE;
    }
    return LTFALSE;
}


// ----------------------------------------------------------------------- //

LTRESULT LTConParse_Common(ConParse* pParse)
{
    if (!pParse) return LT_ERROR;

    // ⚠️ LT_FINISHED, NOT LT_ERROR, at the end. The game's loop is
    // `while (Parse(&parse) == LT_OK)`, so any non-LT_OK ends it -- but
    // LT_ERROR would also be reported by anything that logs failures, and a
    // string that simply ran out is not a failure.
    return pParse->Parse() ? LT_OK : LT_FINISHED;
}
