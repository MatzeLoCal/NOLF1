// ----------------------------------------------------------------------- //
//
// MODULE  : ltconparse.h
//
// PURPOSE : The engine's command-string parser -- how a level's authored text
//           becomes things that happen.
//
// ⚠️ THIS IS HOW LEVELS ARE SCRIPTED, and none of it ran. Every trigger, door,
// key-framer and cinematic in NOLF is driven by strings authored in DEdit and
// stored in the world file, e.g. M01S01's own start point:
//
//     TriggerMessage = "msg player lockmusic;msg player (music i 2);
//                       msg player (changeweapon delisle)"
//
// The game hands that to the ENGINE to tokenise. With Parse a stub the string
// was never split, so no command in any level had ever executed -- and the
// levels looked like they merely lacked AI rather than lacking their script.
//
// ⚠️ AND IT WAS ALSO A CRASH, the SEVENTH of the uninitialised-out-parameter
// family. Prop::TriggerMsg (Prop.cpp:449) does:
//
//     bMore = g_pLTServer->Parse(pCommand, &s_pCommandPos, s_tokenSpace,
//                                s_pTokens, &nArgs);
//     if (!_stricmp(s_pTokens[0], "ANIM"))        // read unconditionally
//
// A stub that returns 0 without writing s_pTokens leaves the caller comparing
// against whatever the array held. Eight of the 102 worlds died in exactly
// this shape.
//
// SYNTAX, which is the game's and not a choice
//
//   ;            ends one command; the parser stops there and reports "more"
//   ( ... )      groups a whole sub-command into ONE token, so
//                `msg player (music i 2)` is two tokens, the second of which
//                is `music i 2`. Nested parens are kept literally.
//   " ... "      the same grouping, spelled differently
//   %x           escapes the next character, whatever it is
//   control chars are command separators, exactly like ';'
//
// ----------------------------------------------------------------------- //

#ifndef __LT_CONPARSE_H__
#define __LT_CONPARSE_H__

#include <stddef.h>

class ConParse;

// Tokenise ONE command out of pCommand.
//
// ⚠️ EVERY OUTPUT IS WRITTEN ON EVERY PATH, including the one where nothing
// parsed. That is not politeness -- see the header note above; the game reads
// these whether or not the call succeeded.
//
// ⚠️ AND ppArgs[0] IS NEVER LEFT NULL. When there are no tokens it points at
// an empty string, so a caller that reads m_Args[0] without checking m_nArgs
// compares against "" instead of dereferencing null. That is a deliberate
// deviation from the original engine, which left the array untouched: it turns
// a crash in the GAME's code into a comparison that simply does not match.
//
// Returns 1 when a ';' ended this command and there is more to parse (in which
// case *ppNewPos points just past it), 0 when this was the last one.
int LTConParse(const char* pCommand,
               const char** ppNewPos,
               char* pArgBuffer, size_t nArgBufferBytes,
               char** ppArgs, int nMaxArgs,
               int* pnArgs);

// ⚠️ ONE BODY FOR ILTCommon::Parse, USED BY BOTH SIDES. It is a pure virtual
// on an interface the client and the server each implement separately, so
// without this there would be two copies of the same three lines -- and this
// port has already had to chase three bugs that were only ever "the two halves
// drifted".
//
// Returns LT_OK while there are tokens and LT_FINISHED when the string is
// exhausted, which is the loop condition iltcommon.h:84 documents.
LTRESULT LTConParse_Common(ConParse* pParse);

#endif // __LT_CONPARSE_H__
