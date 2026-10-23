// ----------------------------------------------------------------------- //
//
// MODULE  : cryptmgr_impl.cpp
//
// PURPOSE : CCryptMgr, declared in LT2/lithshared/cryptmgr/cryptmgr.h.
//
// WHY THIS EXISTS, AND WHY IT IS NOT A REAL CIPHER
//
//   lithshared ships headers only; the original build linked a prebuilt
//   CryptMgr.lib. These definitions are needed for ButeMgr to LINK, because
//   ButeMgr.h's inline Parse(..., const char* cryptKey) overload calls
//   SetKey()/Decrypt().
//
//   That overload is compiled but NEVER EXECUTED by NOLF 1. Measured:
//     * CGameButeMgr::m_pCryptKey is initialised to LTNULL and there is no
//       setter anywhere in the game code, so the `if (m_pCryptKey)` branch that
//       reaches this class is never taken.
//     * The shipped Attributes\*.txt files are plain text -- 100% printable
//       bytes -- so nothing needs decrypting. (See Docs/FORMATS.md §5.)
//
//   Reimplementing the original cipher would therefore be work with no way to
//   verify it and nothing to verify it against.
//
// ⚠️ SO THESE ANNOUNCE THEMSELVES INSTEAD OF PRETENDING.
//
//   If a crypt key is ever introduced -- a mod, a localisation, a later patch
//   -- Decrypt() will fire. Silently copying the input through would hand
//   ButeMgr encrypted bytes, which fails as a baffling parse error hundreds of
//   lines away from the real cause. Instead it says exactly what happened, once.
//
// ----------------------------------------------------------------------- //

#include "windows.h"        // first, so _WINDOWS_ is set (see mfcs_impl.cpp)

#include <stdio.h>
#include <string.h>

#include "cryptmgr.h"

// CCryptMgr declares no data members, so key state lives here.
static char  s_ltKey[57];       // the header notes a 56-character maximum
static bool  s_ltKeySet   = false;
static bool  s_ltReported = false;

static void ltcrypt_ReportOnce(const char* what)
{
    if (s_ltReported)
        return;
    s_ltReported = true;

    fprintf(stderr,
        "\n[CRYPTMGR] %s was called with a key set.\n"
        "[CRYPTMGR] This port does NOT implement the original cipher: NOLF 1's\n"
        "[CRYPTMGR] shipped attribute files are plain text and never reach this\n"
        "[CRYPTMGR] path. Data is being passed through UNCHANGED, so whatever\n"
        "[CRYPTMGR] consumes it next will see encrypted bytes and fail oddly.\n"
        "[CRYPTMGR] If you are seeing this, an encrypted bute file has appeared\n"
        "[CRYPTMGR] and CCryptMgr needs a real implementation.\n\n", what);
    fflush(stderr);
}

CCryptMgr::CCryptMgr()
{
    s_ltKey[0]  = '\0';
    s_ltKeySet  = false;
}

CCryptMgr::CCryptMgr(char* key)
{
    s_ltKey[0]  = '\0';
    s_ltKeySet  = false;
    SetKey(key);
}

CCryptMgr::~CCryptMgr()
{
    // Do not leave a key sitting in memory after the object is gone.
    memset(s_ltKey, 0, sizeof(s_ltKey));
    s_ltKeySet = false;
}

void CCryptMgr::SetKey(const char* key)
{
    if (!key || !*key)
    {
        s_ltKey[0] = '\0';
        s_ltKeySet = false;
        return;
    }

    strncpy(s_ltKey, key, sizeof(s_ltKey) - 1);
    s_ltKey[sizeof(s_ltKey) - 1] = '\0';
    s_ltKeySet = true;
}

// Both directions are a byte-for-byte copy. With no key set that is exactly
// correct -- which is the only case NOLF 1 actually produces. With a key set it
// is wrong, and says so.
void CCryptMgr::Encrypt(istream& is, ostream& os)
{
    if (s_ltKeySet)
        ltcrypt_ReportOnce("CCryptMgr::Encrypt");

    os << is.rdbuf();
}

void CCryptMgr::Decrypt(istream& is, ostream& os)
{
    if (s_ltKeySet)
        ltcrypt_ReportOnce("CCryptMgr::Decrypt");

    os << is.rdbuf();
}
