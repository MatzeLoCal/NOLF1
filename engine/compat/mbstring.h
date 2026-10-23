// ----------------------------------------------------------------------- //
//
// MODULE  : mbstring.h  (macOS compatibility)
//
// PURPOSE : MSVC's multibyte-string CRT routines, as used by NOLF 1.
//
// ⚠️ SCOPE / ASSUMPTION
//
//   These are mapped onto the plain byte-oriented C routines, which is EXACT
//   for single-byte character sets (the English and European NOLF 1 builds)
//   and NOT exact for a true multibyte locale such as Shift-JIS, where one
//   character may span two bytes.
//
//   Concretely, under a genuine MBCS locale:
//     * _mbstrlen would count CHARACTERS; strlen counts BYTES.
//     * _mbsinc would advance past a whole lead+trail byte pair; here it
//       advances one byte.
//
//   Only the eight routines NOLF 1 actually references are provided, and the
//   census that established that list is:
//       _mbstrlen 8, _mbsicmp 8, _mbsnbcmp 7, _mbsinc 6,
//       _mbscpy 6, _mbsnbcnt 2, _mbsncpy 1, _mbscmp 1
//
//   If a CJK localisation is ever attempted, THIS FILE IS THE FIRST THING TO
//   REVISIT -- it will not fail loudly, it will silently mis-measure strings.
//
// ----------------------------------------------------------------------- //

#ifndef __LTCOMPAT_MBSTRING_H__
#define __LTCOMPAT_MBSTRING_H__

#include <string.h>
#include <strings.h>
#include <stddef.h>

// MSVC spells these with `unsigned char*`. Inline wrappers keep the call sites
// compiling unmodified whichever flavour of char pointer they pass.

static inline size_t _mbstrlen(const char* s)
{
    return s ? strlen(s) : 0;                       // SBCS: chars == bytes
}

static inline int _mbsicmp(const unsigned char* a, const unsigned char* b)
{
    return strcasecmp((const char*)a, (const char*)b);
}

static inline int _mbscmp(const unsigned char* a, const unsigned char* b)
{
    return strcmp((const char*)a, (const char*)b);
}

static inline int _mbsnbcmp(const unsigned char* a, const unsigned char* b, size_t n)
{
    return strncmp((const char*)a, (const char*)b, n);   // nb = "n bytes"
}

static inline unsigned char* _mbsinc(const unsigned char* s)
{
    return (unsigned char*)(s + 1);                 // SBCS: one byte per char
}

static inline unsigned char* _mbscpy(unsigned char* d, const unsigned char* s)
{
    strcpy((char*)d, (const char*)s);
    return d;
}

static inline unsigned char* _mbsncpy(unsigned char* d, const unsigned char* s, size_t n)
{
    strncpy((char*)d, (const char*)s, n);
    return d;
}

// Bytes occupied by the first `n` characters. SBCS: n bytes, clamped to the
// actual string length so a short string cannot over-report.
static inline size_t _mbsnbcnt(const unsigned char* s, size_t n)
{
    if (!s) return 0;
    size_t len = strlen((const char*)s);
    return (n < len) ? n : len;
}

#endif // __LTCOMPAT_MBSTRING_H__
