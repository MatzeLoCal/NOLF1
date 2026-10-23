// ----------------------------------------------------------------------- //
//
// MODULE  : ltsession.h
//
// PURPOSE : Is a game running, and which object is the local player's.
//
// ⚠️ WHY THIS IS ITS OWN THING. "There is a server" is the SERVER's business
// and "there is a camera" is the RENDERER's, but four `ILTClient` calls ask a
// question that belongs to neither -- and the game's whole menu-to-level flow
// turns on their answers:
//
//     m_InterfaceMgr.ChangeState(GS_LOADINGLEVEL);
//     g_pLTClient->StartGame(&request);           -- GameClientShell.cpp:6122
//     ...
//     if (g_pGameClientShell->IsInWorld() && ... && g_pLTClient->GetClientObject())
//         ChangeState(GS_PLAYING);                -- InterfaceMgr.cpp:1041
//
// ⚠️ `GetClientObject()` RETURNING NULL IS WHY THE GAME STAYS ON THE MENU.
// That guard is the last one between "loading" and "playing", and it is not
// about the world or the camera -- it asks whether the local player has an
// object on the CLIENT. Until it does, the shell keeps drawing folders: 120
// Start3D/End3D pairs and not one RenderCamera, with a fully-built world and a
// spawned player sitting behind it.
//
// ⚠️ ONE CLIENT, ONE PLAYER. Single player in the original is still a client
// and a server, so there is exactly one of each here. Nothing about this file
// generalises to multiplayer and it should not pretend to.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SESSION_H__
#define __LT_SESSION_H__

// Is a game session up? This is what ILTClient::IsConnected answers, and the
// game uses it to decide whether it may send messages at all.
bool    LTSession_IsConnected();
void    LTSession_SetConnected(bool b);

// The STARTGAME_ / GAMEMODE_ value ILTClient::GetGameMode reports.
int     LTSession_GetGameMode();
void    LTSession_SetGameMode(int n);

// ⚠️ THE LOCAL PLAYER'S CLIENT-SIDE OBJECT. This is an HLOCALOBJ -- a CLIENT
// object, not the server's CPlayerObj. They are different things in different
// halves of the game and confusing them is how a server pointer ends up being
// dereferenced through the client's object list.
void*   LTSession_GetClientObject();
void    LTSession_SetClientObject(void* h);

#endif // __LT_SESSION_H__
