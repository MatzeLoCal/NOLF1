// ----------------------------------------------------------------------- //
//
// MODULE  : gamespy_stub.cpp
//
// PURPOSE : CGameSpyMgr / CGameSpyClientMgr / CGameSpyServer / CGameSpyPlayer.
//
// ⚠️ THIS IS A DELIBERATE STUB, NOT AN IMPLEMENTATION.
//
//   GameSpy was NOLF 1's internet server browser and heartbeat service. The
//   service itself was shut down in 2014, so there is no master server left to
//   query even with a perfect client. Reimplementing the protocol would
//   produce something that connects to nothing.
//
//   These definitions exist so the game links and single-player runs. That is
//   the whole intent, and it is recorded here rather than left to be
//   rediscovered.
//
// WHAT THAT COSTS, STATED PLAINLY
//
//   The internet server BROWSER will not list servers. LAN play and direct
//   connect do NOT go through this class and are unaffected. Single-player is
//   entirely unaffected.
//
// ⚠️ WHY THESE REFUSE RATHER THAN RETURN EMPTY SUCCESS
//
//   Init() returns FALSE so callers take their "multiplayer unavailable" path.
//   An empty-but-successful init would instead show the player a server browser
//   that spins forever finding nothing -- a worse failure, because it looks
//   like a network problem on their end rather than a missing feature.
//
//   Every entry point announces itself once, so a puzzling UI can be traced
//   here immediately instead of being mistaken for a bug elsewhere.
//
// ----------------------------------------------------------------------- //

#include "windows.h"        // first, so _WINDOWS_ is set (see mfcs_impl.cpp)
#include "lithtech.h"
#include "mfcstub.h"

#include <stdio.h>

#include "GameSpyMgr.h"
#include "GameSpyClientMgr.h"
#include "GameSpyServer.h"
#include "GameSpyPlayer.h"

// ----------------------------------------------------------------------- //

static void gs_ReportOnce()
{
    static bool s_reported = false;
    if (s_reported)
        return;
    s_reported = true;

    fprintf(stderr,
        "\n[GAMESPY] The GameSpy server browser is not implemented in this port.\n"
        "[GAMESPY] The service was shut down in 2014, so there is no master\n"
        "[GAMESPY] server to query. Internet server listing is unavailable;\n"
        "[GAMESPY] LAN and direct-connect do not use this path and still work.\n\n");
    fflush(stderr);
}

// A stable empty string: several accessors return char*, so they must never
// hand back a pointer to a local.
static char s_empty[1] = { '\0' };

// ----------------------------------------------------------------------- //
// CGameSpyMgr -- the server-side heartbeat/query responder.
// ----------------------------------------------------------------------- //

BOOL CGameSpyMgr::Init(const char* sGameName, const char* sGameVer, const char* sGameKey,
                       int nGameLoc, int nGamePort, int nSpyPort, DWORD dwFlags)
{
    (void)sGameName; (void)sGameVer; (void)sGameKey;
    (void)nGameLoc; (void)nGamePort; (void)nSpyPort; (void)dwFlags;
    gs_ReportOnce();
    return FALSE;
}

void CGameSpyMgr::Term()   {}
void CGameSpyMgr::Update() {}

BOOL CGameSpyMgr::OnQuery(char* sAddr, DWORD dwPort, char* sData, int nLen)
{
    (void)sAddr; (void)dwPort; (void)sData; (void)nLen;
    return FALSE;   // never answer a heartbeat we cannot honour
}

void CGameSpyMgr::SendResponseInfo(const char* sKey, const char* sValue)
{
    (void)sKey; (void)sValue;
}

void CGameSpyMgr::SendResponseInfo(const char* sKey, int nValue)
{
    (void)sKey; (void)nValue;
}

// ----------------------------------------------------------------------- //
// CGameSpyClientMgr -- the client-side browser.
// ----------------------------------------------------------------------- //

BOOL CGameSpyClientMgr::Init(const char* sGameName, const char* sSecretKey)
{
    (void)sGameName; (void)sSecretKey;
    gs_ReportOnce();
    return FALSE;       // callers take their "unavailable" path
}

void CGameSpyClientMgr::Term()   {}
void CGameSpyClientMgr::Clear()  {}
void CGameSpyClientMgr::Update() {}

int  CGameSpyClientMgr::GetNumServers() { return 0; }
int  CGameSpyClientMgr::GetState()      { return 0; }

BOOL CGameSpyClientMgr::RefreshServers(BOOL bAsync)
{
    (void)bAsync;
    gs_ReportOnce();
    return FALSE;
}

// No servers exist, so every accessor reports absence rather than a
// placeholder the UI would try to display.
CGameSpyServer* CGameSpyClientMgr::GetServerFromHandle(void* pHandle) { (void)pHandle; return 0; }
CGameSpyServer* CGameSpyClientMgr::GetFirstServer()                   { return 0; }
CGameSpyServer* CGameSpyClientMgr::GetNextServer()                    { return 0; }
BOOL            CGameSpyClientMgr::ExistNewServer()                   { return FALSE; }

void CGameSpyClientMgr::SortServersByName(BOOL b)     { (void)b; }
void CGameSpyClientMgr::SortServersByPing(BOOL b)     { (void)b; }
void CGameSpyClientMgr::SortServersByPlayers(BOOL b)  { (void)b; }
void CGameSpyClientMgr::SortServersByMap(BOOL b)      { (void)b; }
void CGameSpyClientMgr::SortServersByGameType(BOOL b) { (void)b; }

// ----------------------------------------------------------------------- //
// CGameSpyServer
// ----------------------------------------------------------------------- //

void  CGameSpyServer::Term()          {}
void  CGameSpyServer::Clear()         {}
char* CGameSpyServer::GetName()       { return s_empty; }
char* CGameSpyServer::GetGameType()   { return s_empty; }
char* CGameSpyServer::GetMap()        { return s_empty; }
char* CGameSpyServer::GetAddress()    { return s_empty; }
int   CGameSpyServer::GetPing()       { return 0; }
int   CGameSpyServer::GetNumPlayers() { return 0; }
int   CGameSpyServer::GetMaxPlayers() { return 0; }
int   CGameSpyServer::UpdatePlayers() { return 0; }

CGameSpyPlayer* CGameSpyServer::GetFirstPlayer() { return 0; }
CGameSpyPlayer* CGameSpyServer::GetNextPlayer()  { return 0; }

char* CGameSpyServer::GetStringValue(char* sKey, char* sDefault)
{
    (void)sKey;
    return sDefault ? sDefault : s_empty;
}

int CGameSpyServer::GetIntValue(char* sKey, int nDefault)
{
    (void)sKey;
    return nDefault;
}

float CGameSpyServer::GetFloatValue(char* sKey, float fDefault)
{
    (void)sKey;
    return fDefault;
}

void CGameSpyServer::SortPlayers(BOOL bAscending, int nKey)
{
    (void)bAscending; (void)nKey;
}

// ----------------------------------------------------------------------- //
// CGameSpyPlayer
// ----------------------------------------------------------------------- //

void  CGameSpyPlayer::Term()     {}
void  CGameSpyPlayer::Clear()    {}
char* CGameSpyPlayer::GetName()  { return s_empty; }
int   CGameSpyPlayer::GetPing()  { return 0; }
int   CGameSpyPlayer::GetFrags() { return 0; }
