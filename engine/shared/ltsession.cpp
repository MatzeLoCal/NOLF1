// ----------------------------------------------------------------------- //
//
// MODULE  : ltsession.cpp
//
// See ltsession.h for why these four answers live together.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltsession.h"

namespace
{
    bool  g_bConnected = false;

    // ⚠️ GAMEMODE_NONE, not 0 -- 0 is STARTGAME_HOST. The game reads this as
    // "am I in a session and what kind", and defaulting to a real mode would
    // tell it it was already hosting one.
    int   g_nGameMode  = GAMEMODE_NONE;

    void* g_hClientObject = 0;
}

bool  LTSession_IsConnected()        { return g_bConnected; }
void  LTSession_SetConnected(bool b) { g_bConnected = b; }

int   LTSession_GetGameMode()        { return g_nGameMode; }
void  LTSession_SetGameMode(int n)   { g_nGameMode = n; }

void* LTSession_GetClientObject()      { return g_hClientObject; }
void  LTSession_SetClientObject(void* h) { g_hClientObject = h; }
