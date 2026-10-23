// ----------------------------------------------------------------------- //
//
// MODULE  : ltreplicate.cpp
//
// PURPOSE : See ltreplicate.h.
//
// ----------------------------------------------------------------------- //

#include "ltreplicate.h"

#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include <string>
#include <set>

namespace
{
    struct Mirror
    {
        void*   m_hClient;

        // ⚠️ The last transform PUSHED, not the last one read. Sync skips the
        // write when nothing moved, and a level's furniture never moves --
        // without this the replicator would write a few hundred identical
        // transforms into the scene every frame for nothing.
        float   m_Pos[3];
        float   m_Quat[4];
        unsigned int m_UserFlags;

        // The last model PUSHED, so a change can be detected. Empty until the
        // server names one.
        std::string  m_sFilename;
    };

    LTReplicateHooks            g_Hooks = { 0, 0, 0, 0, 0 };
    bool                        g_bHooked = false;
    std::map<void*, Mirror>     g_Map;

    // The CLIENT handles that are mirrors -- see LTReplicate_IsMirror.
    std::set<void*>             g_Clients;

    // ⚠️ ADOPTED client objects: not made by the replicator, so never
    // destroyed by it. The local player's client object is the one -- see
    // LTReplicate_SetPlayerHook.
    std::set<void*>             g_Adopted;
    LTReplicatePlayerFn         g_pPlayerHook = 0;

    // ⚠️ REMEMBERED, so a type the client refuses is refused ONCE. Without it
    // every sync would retry the same impossible object and the census would
    // count a new refusal each frame -- a number that grows on its own is a
    // number nobody trusts.
    std::map<void*, int>        g_Refused;

    unsigned int g_nMade    = 0;
    unsigned int g_nReaped  = 0;
    unsigned int g_nRefused = 0;
    unsigned int g_nNoServer = 0;

    bool Tracing()
    {
        static const bool b = (getenv("LT_REPTRACE") != 0);
        return b;
    }

    bool Same3(const float* a, const float* b)
    {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
    }

    bool Same4(const float* a, const float* b)
    {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
    }
}


void LTReplicate_SetHooks(const LTReplicateHooks* pHooks)
{
    if (!pHooks)
    {
        LTReplicate_Reset();
        g_bHooked = false;
        return;
    }

    g_Hooks   = *pHooks;
    g_bHooked = (g_Hooks.GetServerState && g_Hooks.CreateClient
                 && g_Hooks.DestroyClient && g_Hooks.MoveClient
                 && g_Hooks.SetClientUserFlags);
}


void* LTReplicate_Find(void* hServer)
{
    std::map<void*, Mirror>::iterator it = g_Map.find(hServer);
    return (it == g_Map.end()) ? 0 : it->second.m_hClient;
}


void* LTReplicate_Ensure(void* hServer)
{
    if (!g_bHooked || !hServer) return 0;

    std::map<void*, Mirror>::iterator it = g_Map.find(hServer);
    if (it != g_Map.end()) return it->second.m_hClient;

    // Already known to be un-mirrorable. See g_Refused.
    if (g_Refused.find(hServer) != g_Refused.end()) return 0;

    unsigned int nType = 0, nFlags = 0, nUser = 0;
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const char* pFile = 0;

    if (!g_Hooks.GetServerState(hServer, &nType, pos, quat, &nFlags, &nUser, &pFile))
    {
        // ⚠️ COUNTED SEPARATELY FROM A REFUSAL. "The client cannot draw that"
        // and "the server does not own that handle" are different problems
        // with different fixes, and one census number covering both would
        // point at the wrong half of the engine.
        ++g_nNoServer;
        if (Tracing()) printf("[REP] no server object for handle %p\n", hServer);
        return 0;
    }

    // ⚠️ THE LOCAL PLAYER IS NOT MIRRORED, IT IS ADOPTED. In the donor the
    // client object IS the client-side copy of the server player, so the
    // player's own special effects attach to it -- and the game depends on
    // that: CCharacterFX registers with the move manager only when its object
    // == GetClientObject() (CharacterFX.cpp:260), and the move manager sizes
    // the crouching box from that FX's dims tracker (CMoveMgr.cpp:1645). A
    // separate mirror meant no registration: the box never shrank and the
    // dims animation was an uninitialised local (HANDOVER 5bi).
    // LT_PLAYERMIRROR=1 = the old separate mirror (announces itself).
    static const bool s_bOldMirror = []() {
        const bool b = (getenv("LT_PLAYERMIRROR") != 0);
        if (b) { printf("[REP] LT_PLAYERMIRROR=1 -- the local player gets a separate mirror\n"); fflush(stdout); }
        return b; }();
    void* hAdopt = (!s_bOldMirror && g_pPlayerHook) ? g_pPlayerHook(hServer) : 0;

    void* hClient = hAdopt ? hAdopt : g_Hooks.CreateClient(nType, pos, quat, nFlags, pFile);
    if (!hClient)
    {
        // ⚠️ NOT AN ERROR. The client scene answers null for a type it cannot
        // represent rather than handing back a handle that never draws, and
        // that decision is deliberate (ltscene.h). Counting it is the point:
        // the refusal census is the list of object types still to do.
        g_Refused[hServer] = 1;
        ++g_nRefused;
        if (Tracing())
            printf("[REP] refused  server %p  type %u  file %s\n",
                   hServer, nType, pFile ? pFile : "(none)");
        return 0;
    }

    g_Hooks.SetClientUserFlags(hClient, nUser);

    Mirror m;
    m.m_hClient = hClient;
    m.m_UserFlags = nUser;

    // ⚠️ Seeded with what CreateClient was actually given, so the first Sync
    // does not re-push a model the client already has -- and so an object that
    // never had one starts EMPTY and is therefore detected the moment it gets
    // one.
    m.m_sFilename = pFile ? pFile : "";
    if (hAdopt)
    {
        // ⚠️ THE MODEL NOW, not at the next Sync: the player's CharacterFX is
        // created by the very message that called this, and it sets up its
        // node control and animation trackers on the object's model -- a
        // model-less object fails that and the effect is discarded.
        if (g_Hooks.SetClientModel && pFile && *pFile) g_Hooks.SetClientModel(hClient, pFile);
        g_Adopted.insert(hClient);
        if (Tracing()) printf("[REP] adopted  server %p -> client %p (the local player)\n", hServer, hClient);
    }
    for (int k = 0; k < 3; ++k) m.m_Pos[k]  = pos[k];
    for (int k = 0; k < 4; ++k) m.m_Quat[k] = quat[k];
    g_Map[hServer] = m;
    g_Clients.insert(hClient);
    ++g_nMade;

    if (Tracing())
        printf("[REP] mirrored server %p -> client %p  type %u  file %s\n",
               hServer, hClient, nType, pFile ? pFile : "(none)");

    return hClient;
}


void LTReplicate_Sync()
{
    if (!g_bHooked || g_Map.empty()) return;

    // ⚠️ COLLECT THEN ERASE. Reaping inside the walk invalidates the iterator,
    // and the obvious `it = g_Map.erase(it)` is a C++11 change of meaning this
    // file does not need to depend on -- the map is tens of entries.
    std::vector<void*> dead;

    for (std::map<void*, Mirror>::iterator it = g_Map.begin();
         it != g_Map.end(); ++it)
    {
        unsigned int nType = 0, nFlags = 0, nUser = 0;
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const char* pFile = 0;

        if (!g_Hooks.GetServerState(it->first, &nType, pos, quat, &nFlags,
                                    &nUser, &pFile))
        {
            dead.push_back(it->first);
            continue;
        }

        if (nUser != it->second.m_UserFlags)
        {
            // ⚠️ THE OLD AND NEW VALUES, not just "it changed". The game's own
            // script is written in these bits -- USRFLG_CAMERA_LIVE (1<<5)
            // going 1 -> 0 is a cinematic ENDING -- so a trace that said only
            // "flags changed" would answer none of the questions it is read to
            // answer.
            if (Tracing())
                printf("[REP] userflags server %p  0x%08x -> 0x%08x%s\n",
                       it->first, it->second.m_UserFlags, nUser,
                       ((it->second.m_UserFlags ^ nUser) & 0x00000020u)
                           ? ((nUser & 0x00000020u) ? "   CAMERA_LIVE on"
                                                    : "   CAMERA_LIVE OFF")
                           : "");
            g_Hooks.SetClientUserFlags(it->second.m_hClient, nUser);
            it->second.m_UserFlags = nUser;
        }

        // ⚠️ THE MODEL, BEFORE THE EARLY-OUT BELOW. The transform test skips
        // anything that did not move, and a character standing still while its
        // model arrives would have been skipped with it -- which is most of a
        // level's population at the moment they are built.
        {
            const char* pNow = pFile ? pFile : "";
            if (it->second.m_sFilename != pNow)
            {
                if (g_Hooks.SetClientModel && *pNow)
                {
                    g_Hooks.SetClientModel(it->second.m_hClient, pNow);
                    if (Tracing())
                        printf("[REP] model server %p -> '%s'\n", it->first, pNow);
                }
                it->second.m_sFilename = pNow;
            }
        }

        if (Same3(pos, it->second.m_Pos) && Same4(quat, it->second.m_Quat))
            continue;

        g_Hooks.MoveClient(it->second.m_hClient, pos, quat);
        for (int k = 0; k < 3; ++k) it->second.m_Pos[k]  = pos[k];
        for (int k = 0; k < 4; ++k) it->second.m_Quat[k] = quat[k];
    }

    for (size_t i = 0; i < dead.size(); ++i)
    {
        std::map<void*, Mirror>::iterator it = g_Map.find(dead[i]);
        if (it == g_Map.end()) continue;

        g_Clients.erase(it->second.m_hClient);
        if (g_Adopted.erase(it->second.m_hClient) == 0)
            g_Hooks.DestroyClient(it->second.m_hClient);
        if (Tracing())
            printf("[REP] reaped   server %p -> client %p\n",
                   it->first, it->second.m_hClient);
        g_Map.erase(it);
        ++g_nReaped;
    }
}


void LTReplicate_Reset()
{
    if (g_bHooked && g_Hooks.DestroyClient)
    {
        for (std::map<void*, Mirror>::iterator it = g_Map.begin();
             it != g_Map.end(); ++it)
        {
            if (!g_Adopted.count(it->second.m_hClient))
                g_Hooks.DestroyClient(it->second.m_hClient);
        }
    }
    g_Map.clear();
    g_Clients.clear();
    g_Adopted.clear();
    g_Refused.clear();
}

void LTReplicate_SetPlayerHook(LTReplicatePlayerFn fn) { g_pPlayerHook = fn; }

void* LTReplicate_ServerOf(void* hClient)
{
    if (!hClient || !g_Clients.count(hClient)) return 0;
    for (std::map<void*, Mirror>::const_iterator it = g_Map.begin(); it != g_Map.end(); ++it)
        if (it->second.m_hClient == hClient) return it->first;
    return 0;
}

bool LTReplicate_IsMirror(void* hClient)
{
    return hClient && g_Clients.count(hClient) != 0;
}


unsigned int LTReplicate_Live()    { return (unsigned int)g_Map.size(); }
unsigned int LTReplicate_Made()    { return g_nMade; }
unsigned int LTReplicate_Reaped()  { return g_nReaped; }
unsigned int LTReplicate_Refused() { return g_nRefused; }


void LTReplicate_DumpCensus()
{
    if (!g_nMade && !g_nRefused && !g_nNoServer) return;

    printf("\n[REP] object replication\n");
    printf("      mirrored %u, reaped %u, live %u, refused %u, no server object %u\n",
           g_nMade, g_nReaped, (unsigned int)g_Map.size(), g_nRefused, g_nNoServer);
    if (g_nRefused)
        printf("      ⚠️  a refusal is the CLIENT scene declining a type it "
               "cannot represent yet,\n"
               "          not a failure to replicate. LT_REPTRACE=1 names them.\n");
    fflush(stdout);
}
