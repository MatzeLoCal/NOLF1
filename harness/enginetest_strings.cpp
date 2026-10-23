// ----------------------------------------------------------------------- //
//
// enginetest_strings.cpp -- ClientRes and the HSTRING manager, with negative
//                           controls.
//
// WHY NEGATIVE CONTROLS AND NOT JUST "DOES IT LOAD"
//
//   A string table that returned the same wrong string for every id would pass
//   "6031 strings loaded". A reference count that never decremented would pass
//   "no crashes". FORMATS.md records that ButeMgr's suite only became evidence
//   once it asserted that PRESENT keys ignore the supplied default -- without
//   that, a parser returning defaults for everything scored full marks.
//
//   So every check here is paired with one that must FAIL.
//
// ⚠️ The expected values below are pinned to the GOTY CRES.DLL (6031 strings).
//   Against the base game's copy (5876) the ids still resolve but the count
//   differs -- that is the tree stack working, not a bug. See FORMATS.md §6.
//
// ----------------------------------------------------------------------- //

// ⚠️ INCLUDE ORDER, mirroring the game's own Stdafx.h -- HANDOVER.md §4.3.
// windows.h first, because ltbasedefs.h uses _MAX_PATH and mfcstub defers its
// POINT/RECT only once _WINDOWS_ is defined. A test compiled against different
// types than the engine is not testing the engine.
#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltperes.h"
#include "ltstringmgr.h"
#include "ltfilemgr.h"

#include <stdio.h>
#include <string.h>

static int g_nPass = 0;
static int g_nFail = 0;

static void ok(const char* what)          { printf("  ok    %s\n", what); ++g_nPass; }
static void bad(const char* what)         { printf("  FAIL  %s\n", what); ++g_nFail; }
static void check(bool b, const char* w)  { b ? ok(w) : bad(w); }


// FormatString is variadic, so exercising it needs a variadic caller.
static HSTRING Fmt(CLTStringMgr* pSM, int id, ...)
{
    va_list args;
    va_start(args, id);
    HSTRING h = pSM->Format(id, args);
    va_end(args);
    return h;
}


int main(int argc, char** argv)
{
    const char* pDirs[8];
    int nDirs = 0;
    for (int i = 1; i < argc && nDirs < 8; ++i) pDirs[nDirs++] = argv[i];
    if (nDirs == 0)
    {
        fprintf(stderr, "usage: enginetest_strings <game-dir> [<game-dir> ...]\n");
        return 2;
    }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(pDirs, nDirs) == 0)
    {
        fprintf(stderr, "no game data mounted\n");
        return 2;
    }

    CLTStringMgr sm;
    if (!sm.LoadResourceDll(&fm, "CRES.DLL"))
    {
        fprintf(stderr, "CRES.DLL did not load\n");
        return 1;
    }

    printf("string table: %u strings\n", sm.GetTableCount());

    // --- the table itself --------------------------------------------- //

    check(sm.GetTableCount() > 5000, "table has a plausible number of strings");

    {
        // A known id with known text. ⚠️ This is the check that a table
        // returning one string for everything cannot pass.
        HSTRING h = Fmt(&sm, 1);
        const char* p = h ? sm.GetData(h) : 0;
        check(p && strcmp(p, "NO ONE LIVES FOREVER") == 0,
              "id 1 is exactly \"NO ONE LIVES FOREVER\"");
        if (p && strcmp(p, "NO ONE LIVES FOREVER") != 0)
            printf("        got \"%s\"\n", p);
        sm.Free(h);

        HSTRING h2 = Fmt(&sm, 104);
        const char* p2 = h2 ? sm.GetData(h2) : 0;
        check(p2 && strcmp(p2, "Cancel") == 0, "id 104 is exactly \"Cancel\"");
        sm.Free(h2);
    }

    {
        // ⚠️ NEGATIVE CONTROL: an absent id must be NULL, not "".
        // Returning an empty string would make every check above pass just as
        // happily while hiding a wrong table -- and the donor returns LTNULL
        // here (clientde_impl.cpp:1339).
        HSTRING h = Fmt(&sm, 999999);
        check(h == 0, "NEGATIVE CONTROL: an absent id returns NULL, not \"\"");
        if (h) sm.Free(h);
    }

    // --- CP1252 conversion -------------------------------------------- //

    {
        // ⚠️ id 13725 is "Give ‘em hell!" -- U+2018, which lives in CP1252's
        // 0x80-0x9F range and does NOT exist in Latin-1. A converter that only
        // did Latin-1 would drop it and still pass every ASCII check above.
        HSTRING h = Fmt(&sm, 13725);
        const char* p = h ? sm.GetData(h) : 0;
        check(p && (unsigned char)p[5] == 0x91,
              "NEGATIVE CONTROL: U+2018 becomes CP1252 0x91, not dropped");
        if (p) printf("        id 13725 = \"%s\" (byte[5]=0x%02X)\n",
                      p, p ? (unsigned char)p[5] : 0);
        sm.Free(h);
    }

    {
        // A German umlaut, from the Latin-1 half.
        HSTRING h = Fmt(&sm, 10941);
        const char* p = h ? sm.GetData(h) : 0;
        check(p && strstr(p, "\xF6") != 0, "U+00F6 becomes CP1252/Latin-1 0xF6");
        sm.Free(h);
    }

    // --- FormatMessage substitution ----------------------------------- //

    {
        // "%1!d!x%2!d!x%3!d!"
        HSTRING h = Fmt(&sm, 501, 640, 480, 32);
        const char* p = h ? sm.GetData(h) : 0;
        check(p && strcmp(p, "640x480x32") == 0, "%N!d! substitution");
        if (p && strcmp(p, "640x480x32") != 0) printf("        got \"%s\"\n", p);
        sm.Free(h);
    }

    {
        // "%1!s! - %2!s!"
        HSTRING h = Fmt(&sm, 500, "alpha", "beta");
        const char* p = h ? sm.GetData(h) : 0;
        check(p && strcmp(p, "alpha - beta") == 0, "%N!s! substitution");
        sm.Free(h);
    }

    {
        // ⚠️ THE ONE THAT MATTERS: id 603 is "Picked up %2!s! ( %1!d! )" --
        // the ONLY out-of-order format in all 6031 strings. A left-to-right
        // implementation gets every other string right and hands the int to
        // %s here. This is FORMATS.md §5's "once in ~33,000 values" again.
        HSTRING h = Fmt(&sm, 603, 7, "briefcase");
        const char* p = h ? sm.GetData(h) : 0;
        check(p && strcmp(p, "Picked up briefcase ( 7 )") == 0,
              "OUT-OF-ORDER arguments (id 603) resolve by INDEX, not position");
        if (p && strcmp(p, "Picked up briefcase ( 7 )") != 0)
            printf("        got \"%s\"\n", p);
        sm.Free(h);
    }

    // --- reference counting -------------------------------------------- //

    {
        unsigned int before = sm.GetNumLive();

        HSTRING a = sm.Create("refcount");
        check(sm.GetNumLive() == before + 1, "Create adds one live string");

        HSTRING b = sm.Copy(a);
        check(b == a, "Copy shares the object rather than duplicating it");
        check(sm.GetNumLive() == before + 1, "Copy does not add a live string");

        sm.Free(a);
        // ⚠️ NEGATIVE CONTROL: one Free must NOT be enough. A refcount that
        // is ignored would delete here and pass every other test in this file.
        check(sm.GetNumLive() == before + 1,
              "NEGATIVE CONTROL: one Free of two references does not delete");

        sm.Free(b);
        check(sm.GetNumLive() == before, "the second Free deletes");
    }

    {
        // A stale handle must be refused, not dereferenced.
        HSTRING h = sm.Create("temp");
        sm.Free(h);
        unsigned int badBefore = sm.GetNumLive();
        char* p = sm.GetData(h);
        check(p == 0, "NEGATIVE CONTROL: a freed handle returns NULL, not garbage");
        check(sm.GetNumLive() == badBefore, "a stale handle changes nothing");
    }

    {
        check(sm.Compare(0, 0) == LTFALSE, "comparing NULL handles is false, not a crash");
        HSTRING a = sm.Create("Same");
        HSTRING b = sm.Create("same");
        check(sm.Compare(a, b) == LTFALSE, "CompareStrings is case-SENSITIVE");
        check(sm.CompareUpper(a, b) == LTTRUE, "CompareStringsUpper is case-insensitive");
        sm.Free(a); sm.Free(b);
    }

    check(sm.GetNumLive() == 0, "no strings leaked by this test");

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
