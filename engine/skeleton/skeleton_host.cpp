// ----------------------------------------------------------------------- //
//
// MODULE  : skeleton_host.cpp
//
// PURPOSE : A minimal host that stands in for the engine so NOLF 1's game code
//           can be INSTANTIATED and observed.
//
// WHAT THIS IS FOR
//
//   Everything before this point could only be compiled or linked. This is the
//   first thing that RUNS game code. It constructs the stub engine interfaces,
//   hands them to the game's own entry points --
//
//       IClientShell* CreateClientShell(ILTClient*)     ClientShellDLL
//       IServerShell* CreateServerShell(ILTServer*)     ObjectDLL
//
//   -- and then reports every engine call the game made on the way up.
//
//   That report is the point. It turns "which of the 275 engine methods does
//   NOLF 1 actually need, and in what order?" from a guess into a measurement,
//   and the answer is the implementation order for the real engine.
//
// ⚠️ WHAT IT IS NOT
//
//   It is not the engine, and it is not a game. Every stub returns LT_ERROR, so
//   the game will fail early and deliberately. A short census here is a SUCCESS:
//   it means we reached the first genuine dependency and now know what it is.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <chrono>
#include <signal.h>
#include <unistd.h>

#include "skeleton_generated.h"
#include "ltstub.h"
#include "ltserverhost.h"
#include "ltserverbrush.h"
#include "ltserverphysics.h"

#include "ltfilemgr.h"
#include "ltrecord.h"
#include "ltnullrender.h"
#include "ltscene.h"
#include "ltworldcollide.h"
#include "ltconsole.h"
#include "ltmathimpl.h"
#include "ltnullcursor.h"
#include "ltstringmgr.h"
#include "ltclock.h"
#include "ltwindow.h"
#include "ltsoundmgr.h"
#include "ltaudio.h"
#include "ltcomp.h"
#include "ltmetal.h"
#include "ltinput.h"
#include "ltbindings.h"
#include "ltworldwalk.h"
#include "ltworldscene.h"
#include "ltworldrender.h"
#include "ltsession.h"
#include "ltmessage.h"
#include "ltconparse.h"
#include "ltreplicate.h"

#include <map>
#include <set>
#include <string.h>
#include <stdlib.h>

#include "iclientshell.h"
#include "iltdirectmusic.h"

// engine/sound/ltdmhost.cpp: the music manager, or 0 to keep the stub.
ILTDirectMusicMgr* LTDM_HostSetup();
#include "iservershell.h"

// LT_MOVERTRACE: client moves still to print (reset on each level entry).
static int g_nMoverTrace = 0;


// ----------------------------------------------------------------------- //
// WHERE THE GAME DATA IS.
//
// The two FULLGAME installs are disc images rather than installed game
// directories, so the archives named by startup.txt are spread across two of
// them: GOTY-disc2/Game holds startup.txt and the executables, GOTY-disc1/Data
// and GOTY-disc2/Data hold the archives. CLTFileMgr::InitFromGameDirs resolves
// each authored name against every directory it is given, so the split does
// not matter as long as all of them are listed.
//
// Override with NOLF_GAME_DIR, a colon-separated list, highest-priority last
// -- the same ordering rule the tree stack itself uses.
//
// ⚠️ PORT_PLAN.md §5: these are REAL INSTALLS. Nothing here writes to them.
// CLTFileMgr opens every tree read-only and has no write path at all.
// ----------------------------------------------------------------------- //

#define NOLF_DEFAULT_GAME_DIRS                                          \
    "/Users/matthiasschonder/Development/NOLF1/FULLGAME/GOTY/GOTY-disc2/Game:"   \
    "/Users/matthiasschonder/Development/NOLF1/FULLGAME/GOTY/GOTY-disc1/Data:"   \
    "/Users/matthiasschonder/Development/NOLF1/FULLGAME/GOTY/GOTY-disc2/Data"

// CreateClientShell is declared by iclientshell.h with normal C++ linkage --
// re-declaring it as extern "C" is a linkage mismatch, not a convenience.


// ----------------------------------------------------------------------- //
// MISSING-FILE CENSUS.
//
// The stub census answers "which methods?"; this answers "which files?". It is
// the same instrument applied one level down, and it is what turns "the game
// stopped" into "the game asked for these eleven names and got none of them".
//
// Distinct names only, and in first-asked order, for the reason ltstub.h gives:
// these sit on paths that retry, and printing every call buries the signal.
//
// ⚠️ EVERYTHING DIAGNOSTIC GOES TO stderr, and that is not a style choice.
// ltstub.cpp writes its [LTSTUB] lines to stderr, which is unbuffered; stdout
// is block-buffered as soon as the run is piped into anything. Mixing the two
// makes the ORDER OF THE TWO INSTRUMENTS a lie -- the first traced run showed
// every file open after stub 12, when they actually happen between 4 and 5.
// An instrument that misreports sequence is worse than none (PORT_PLAN.md §5).
// ----------------------------------------------------------------------- //

#define LTFILE_MAX_MISSES 128

static char  g_MissNames[LTFILE_MAX_MISSES][256];
static int   g_nMisses      = 0;
static int   g_nMissesOver  = 0;

static void LTFile_ReportMiss(const char* pName)
{
    for (int i = 0; i < g_nMisses; ++i)
        if (strcasecmp(g_MissNames[i], pName) == 0) return;

    if (g_nMisses >= LTFILE_MAX_MISSES) { ++g_nMissesOver; return; }

    strncpy(g_MissNames[g_nMisses], pName, sizeof(g_MissNames[0]) - 1);
    g_MissNames[g_nMisses][sizeof(g_MissNames[0]) - 1] = 0;
    ++g_nMisses;

    if (!getenv("LT_STUB_QUIET"))
    {
        fprintf(stderr, "[LTFILE MISS] %s\n", pName);
        fflush(stderr);
    }
}

static void LTFile_DumpCensus()
{
    CLTFileMgr* pFM = LTGetFileMgr();

    fprintf(stderr, "\n===== file system =====\n");
    fprintf(stderr, "  trees: %d  (searched last-added first)\n", pFM->GetNumTrees());
    for (int i = pFM->GetNumTrees() - 1; i >= 0; --i)
    {
        fprintf(stderr, "    %-4s %-6u entries  %s\n",
               pFM->GetTreeType(i) == CLTFileMgr::kRezTree ? "rez" : "dir",
               pFM->GetTreeNumEntries(i),
               pFM->GetTreePath(i));
    }
    fprintf(stderr, "  opened  : %u\n", pFM->GetNumOpened());
    fprintf(stderr, "  missing : %u call(s), %d distinct name(s)\n",
           pFM->GetNumNotFound(), g_nMisses);

    for (int i = 0; i < g_nMisses; ++i)
        fprintf(stderr, "    %s\n", g_MissNames[i]);
    if (g_nMissesOver)
        fprintf(stderr, "    (+%d more, census cap reached)\n", g_nMissesOver);
    fprintf(stderr, "=======================\n");
    fflush(stderr);
}


// ----------------------------------------------------------------------- //
// ⚠️ THE INTERFACES CARRY DATA, NOT JUST METHODS.
//
// ILTCSBase is not purely abstract. It has protected members and INLINE
// accessors that return them directly:
//
//     inline ILTPhysics* Physics()    { return m_pPhysicsLT; }
//     inline ILTMath*    GetMathLT()  { return &m_MathLT; }
//
// Overriding the virtual methods therefore is NOT enough -- those pointers stay
// uninitialised, and CreateClientShell walks straight from
// `g_pPhysicsLT = g_pLTClient->Physics()` into
// `g_pPhysicsLT->SetStairHeight(...)`. The first run crashed exactly there,
// on a vtable read through uninitialised memory that happened to hold ASCII.
//
// Because the accessors are inline they never reach a stub, so nothing was
// reported before the crash -- the ABSENCE of a [LTSTUB] line was the clue that
// these are data, not calls.
//
// This subclass supplies the sub-interfaces. m_MathLT is held BY VALUE and
// default-constructs, so only the six pointers need wiring.
// ----------------------------------------------------------------------- //

// ----------------------------------------------------------------------- //
// TWO STUBS THAT WERE THROWING AWAY THE ANSWER.
//
// ShutdownWithMessage and DebugOut both take the message the game wanted to
// show, and the generated stub discards it -- so a run that ends in
// "[LTSTUB] ILTClient::ShutdownWithMessage" reports that the game gave up
// without reporting WHY, when the why was in the argument all along.
//
// ⚠️ These are FUNCTION-POINTER MEMBERS, not virtuals (iltclient.h:125, :723).
// Declaring them as overrides in this class would compile, would never be
// called, and would look like the game had stopped reaching them. They have to
// be ASSIGNED, after LTSkel_WireFnPtrs_ILTClient has filled in the defaults.
// HANDOVER.md §4.5 is about exactly this.
// ----------------------------------------------------------------------- //

static void LTHost_ShutdownWithMessage(char* pMsg, ...)
{
    // ⚠️ LTSTUB, not LTStub_Report. LTStub_Report counts EVERY call as a new
    // census entry; the macro is the once-only guard. A first draft used the
    // raw call and four consecutive DebugOut messages became census entries
    // 11-14, which reads as four distinct engine methods.
    LTIMPL("ILTClient::ShutdownWithMessage");

    char buf[1024];
    va_list args;
    va_start(args, pMsg);
    vsnprintf(buf, sizeof(buf), pMsg ? pMsg : "(null)", args);
    va_end(args);

    fprintf(stderr, "[LTSHUTDOWN] %s\n", buf);
    fflush(stderr);
}

static void LTHost_DebugOut(char* pMsg, ...)
{
    LTIMPL("ILTClient::DebugOut");

    char buf[1024];
    va_list args;
    va_start(args, pMsg);
    vsnprintf(buf, sizeof(buf), pMsg ? pMsg : "(null)", args);
    va_end(args);

    fprintf(stderr, "[LTDEBUG] %s", buf);
    fflush(stderr);
}


// ----------------------------------------------------------------------- //
// A CRASH MUST STILL PRODUCE A RECORD.
//
// The game does not check every value the engine hands it -- on Win32 some of
// them could not fail. CGameSettings::Init calls
// stricmp(GetStringData(FormatString(IDS_ALLOW_NO_GORE)), "TRUE") with no NULL
// check, because ClientRes.dll was always present. Here it is not, and the
// donor's own FormatString returns LTNULL in exactly that situation
// (clientde_impl.cpp:1339), so the segfault is the authentic consequence rather
// than a port defect to paper over.
//
// What must NOT happen is the run record vanishing at the one moment it has
// something to say. With these handlers a crash is a LINE IN THE CONTRACT --
// "the run reaches here and dies" -- and moving that line is a diff.
// ----------------------------------------------------------------------- //

static void LTHost_FatalSignal(int sig)
{
    const char* pName = "signal";
    switch (sig)
    {
        case SIGSEGV: pName = "SIGSEGV"; break;
        case SIGBUS:  pName = "SIGBUS";  break;
        case SIGABRT: pName = "SIGABRT"; break;
        case SIGILL:  pName = "SIGILL";  break;
        default: break;
    }

    LTRecord_NoteFatalAndClose(pName);

    // ⚠️ write(2), not fprintf: this is a signal handler (see ltrecord.h).
    ssize_t ignored;
    ignored = write(2, "\n[LTFATAL] ", 11);
    ignored = write(2, pName, strlen(pName));
    ignored = write(2, " -- run record terminated. Rebuild with NOLF_ASAN=1"
                       " to name the site (HANDOVER.md §4.6).\n", 88);
    (void)ignored;

    // Restore the default and re-raise, so the shell still reports the real
    // signal and a debugger still sees it.
    signal(sig, SIG_DFL);
    raise(sig);
}

// ----------------------------------------------------------------------- //
// THE WINDOW HOOK NOLF NEEDS.
//
// HookWindow() asks for "HWND", saves the existing procedure, and installs its
// own. Neither the handle nor the default procedure has to DO anything -- but
// both must be non-NULL, because the game tests both and gives up quietly.
// ⚠️ (HWND)1 rather than 0: a null handle is indistinguishable from failure.
// ----------------------------------------------------------------------- //

static LRESULT LTHost_DefaultWndProc(HWND, UINT, WPARAM, LPARAM) { return 0; }

static LTRESULT LTHost_GetEngineHook(char* pName, void** pData)
{
    LTIMPL("ILTClient::GetEngineHook");
    if (!pName || !pData) return LT_ERROR;

    if (strcasecmp(pName, "HWND") == 0)
    {
        *pData = (void*)(intptr_t)1;
        return LT_OK;
    }
    return LT_ERROR;
}

// Defined after the class, next to the sound manager it needs. Declared here
// because the constructor wires it into a function-pointer member.
static LTRESULT LTHost_ClientPlaySound(PlaySoundInfo* pInfo);
static LTRESULT LTHost_SndInit(InitSoundInfo* pInfo);
static void     LTHost_SndKill(HLTSOUND h);
static void     LTHost_SndKillLoop(HLTSOUND h);
static void     LTHost_SndPause();
static void     LTHost_SndResume();
static LTRESULT LTHost_SndDuration(HLTSOUND h, LTFLOAT* pf);
static LTRESULT LTHost_SndGetData(HLTSOUND h, int16*& p16, int8*& p8,
                                  uint32* pRate, uint32* pChannels);
static LTRESULT LTHost_SndGetOffset(HLTSOUND h, uint32* pOffset, uint32* pSize);
static void     LTHost_SndSetVolume(unsigned short n);
static unsigned short LTHost_SndGetVolume();

static void LTHost_InstallCrashHandlers()
{
    // ⚠️ LT_NOCRASHHANDLER=1 LEAVES THE FAULT ALONE, and it exists because
    // this handler HIDES AddressSanitizer. Both install a SIGSEGV handler and
    // the last one wins; this one runs, prints a line and re-raises with
    // SIG_DFL, so the process dies before ASan can name the site -- turning
    // "stack-buffer-overflow at PolyGridFX.cpp:174" into "SIGSEGV".
    //
    // A debugger wants it off for the same reason.
    if (getenv("LT_NOCRASHHANDLER")) return;

    signal(SIGSEGV, LTHost_FatalSignal);
    signal(SIGBUS,  LTHost_FatalSignal);
    signal(SIGILL,  LTHost_FatalSignal);
}
// ----------------------------------------------------------------------- //
// ⚠️ THE CLIENT'S PHYSICS -- WHERE THE PLAYER ACTUALLY MOVES.
//
// It is easy to assume movement is the server's job, implement
// `ILTPhysics` there, and find that nothing about the player changed. That is
// what happened here. NOLF is explicit about it:
//
//   CPlayerObj.cpp:168   m_dwFlags &= ~FLAG_GRAVITY; // This is controlled by
//                                                    // the client.
//
// and `CMoveMgr` does every SetVelocity, GetAcceleration and GetStandingOn on
// `g_pPhysicsLT`, which on the client is THIS object. Measured on M01S01: not
// one server object carries FLAG_GRAVITY -- the union of every object's flags
// across a 300-frame run is 0x19ad7da7, and bit 17 is clear.
//
// ⚠️ The state lives on LTSceneObject and the collision is the SHARED grid, so
// the player and the AI cannot disagree about where a wall is.
// ----------------------------------------------------------------------- //

// ⚠️ THE KEY-ASSIGNMENT SCREEN'S PATH -- a third input door, and the reason
// Options -> Customize Controls could navigate with Enter/Escape and never see
// a letter. See ltbindings.h.
static LTRESULT LTHost_StartDeviceTrack(uint32 nDevices, uint32 nBufferSize)
{
    LTDevice_StartTrack(nDevices, nBufferSize);
    return LT_OK;
}

static LTRESULT LTHost_TrackDevice(DeviceInput* pArray, uint32* pnInOut)
{
    if (!pnInOut) return LT_INVALIDPARAMS;
    unsigned n = (unsigned)*pnInOut;
    LTDevice_Track(pArray, &n);
    *pnInOut = n;
    return LT_OK;
}

static LTRESULT LTHost_EndDeviceTrack()
{
    LTDevice_EndTrack();
    return LT_OK;
}

static DeviceBinding* LTHost_GetDeviceBindings(uint32 nFlags)
{
    return (DeviceBinding*)LTDevice_GetBindings(nFlags);
}

static void LTHost_FreeDeviceBindings(DeviceBinding* pList)
{
    LTDevice_FreeBindings(pList);
}

static DeviceObject* LTHost_GetDeviceObjects(uint32 nFlags)
{
    return (DeviceObject*)LTDevice_GetObjects(nFlags);
}

static void LTHost_FreeDeviceObjects(DeviceObject* pList)
{
    LTDevice_FreeObjects(pList);
}

static LTRESULT LTHost_GetDeviceName(uint32 nType, char* pBuf, uint32 nLen)
{
    // ⚠️ Written on every path: the game passes a 2048-byte stack buffer and
    // feeds the result straight into a sprintf format (FolderCustomControls
    // .cpp:434). An untouched buffer becomes a rangebind built from a stack
    // pattern.
    if (!pBuf || !nLen) return LT_INVALIDPARAMS;
    pBuf[0] = 0;
    return LTDevice_GetName(nType, pBuf, nLen) ? LT_OK : LT_ERROR;
}

// ⚠️ THE CLIENT'S RAY -- the same question the server asks, from the other
// side. `ClientIntersectQuery` is a #define for IntersectQuery
// (ltbasedefs.h:993), so the two share a shape and can share an answer.
//
// ⚠️ IT GOES THROUGH THE SERVER'S TEST ON PURPOSE. The world triangles and the
// object boxes both live there, and a client that answered from a second,
// client-only view of the level would let the player shoot through a door the
// server thinks is shut.
// ⚠️ THE CLIENT'S SURFACE LOOKUP -- footsteps (CollisionInfo) and client
// impact effects ask it. Always writes *pFlags; the stub did not.
static LTRESULT LTHost_ClientPolyTextureFlags(HPOLY hPoly, uint32* pFlags)
{
    if (!pFlags) return LT_ERROR;
    *pFlags = 0;
    unsigned int n = 0;
    CLTWorldScene* pScene = LTGetWorldScene();
    if (!pScene || !pScene->PolySurfaceType((unsigned int)hPoly, &n)) return LT_ERROR;
    *pFlags = n;
    return LT_OK;
}

// ⚠️ THE CLIENT'S RAY HITS CLIENT OBJECTS (2026-10-13, HANDOVER 5bw). The
// ray runs against the server's records -- the client has no physics of its
// own -- but every handle the GAME sees on this side must be a client handle.
// The game's own filter was handed SERVER handles, asked the client for their
// flags, got 0 for every one and threw them all away, and the hit was then
// reported as the world. So nothing the client aims at was ever an object:
// the belt buckle (Zipcord) never found its ZipHook, the camera never found
// its intel, the activate crosshair never lit (PlayerStats.cpp
// TestForActivationObject, IsGadgetActivatable).
//
// Now an object with a client mirror is filtered AS the mirror and reported
// as it. A mirror is made only for the object a ray actually hits (making
// one for everything a ray passes would replicate the whole level); if the
// filter then refuses it, it is excluded and the ray goes again. An object
// the client cannot represent at all keeps the old answer.
// LT_CLIENTRAYWORLD=1 = the old rule (says so).
namespace
{
    struct ClientRayCtx
    {
        ObjectFilterFn          m_fnObj;
        PolyFilterFn            m_fnPoly;
        void*                   m_pUser;
        const std::set<void*>*  m_pExcluded;
    };

    bool LTHost_ClientRayObjFilter(void* hServer, void* pCtx)
    {
        const ClientRayCtx* c = (const ClientRayCtx*)pCtx;
        if (c->m_pExcluded->count(hServer)) return false;
        if (!c->m_fnObj) return true;
        if (void* hClient = LTReplicate_Find(hServer))
            return c->m_fnObj((HOBJECT)hClient, c->m_pUser) != 0;
        return true;        // no mirror yet: decided if it is the nearest hit
    }

    bool LTHost_ClientRayPolyFilter(unsigned int nPoly, void* pCtx)
    {
        const ClientRayCtx* c = (const ClientRayCtx*)pCtx;
        return !c->m_fnPoly || c->m_fnPoly((HPOLY)nPoly, c->m_pUser) != 0;
    }
}

static LTBOOL LTHost_ClientIntersectSegment(IntersectQuery* pQuery,
                                            IntersectInfo* pInfo)
{
    if (!pQuery || !pInfo) return LTFALSE;

    pInfo->m_Point.Init();
    pInfo->m_Plane.m_Normal.Init();
    pInfo->m_Plane.m_Dist = 0.0f;
    pInfo->m_hObject = LTNULL;
    pInfo->m_hPoly = INVALID_HPOLY;
    pInfo->m_SurfaceFlags = 0;

    const float from[3] = { pQuery->m_From.x, pQuery->m_From.y, pQuery->m_From.z };
    const float to[3]   = { pQuery->m_To.x,   pQuery->m_To.y,   pQuery->m_To.z };

    static const bool s_bOld = []() {
        const bool b = getenv("LT_CLIENTRAYWORLD") != 0;
        if (b) { printf("[CLIENT] LT_CLIENTRAYWORLD=1 -- client rays report every hit as the world\n"); fflush(stdout); }
        return b; }();

    float pt[3], nrm[3];
    void* hHit = 0;
    unsigned int nPolyId = 0;
    void* hClientHit = 0;

    if (s_bOld)
    {
        // The game's polygon filter too -- see LTServerPhys_RayTestFiltered.
        if (!LTServerPhys_RayTestFiltered(from, to,
                                  (bool (*)(void*, void*))pQuery->m_FilterFn,
                                  pQuery->m_pUserData,
                                  (bool (*)(unsigned int, void*))pQuery->m_PolyFilterFn,
                                  pt, nrm, &hHit,
                                  true, false, &nPolyId))
            return LTFALSE;
    }
    else
    {
        std::set<void*> excluded;
        ClientRayCtx ctx = { pQuery->m_FilterFn, pQuery->m_PolyFilterFn,
                             pQuery->m_pUserData, &excluded };
        bool bDone = false;
        for (int nTry = 0; nTry < 8 && !bDone; ++nTry)
        {
            hHit = 0; nPolyId = 0; hClientHit = 0;
            if (!LTServerPhys_RayTestFiltered(from, to, &LTHost_ClientRayObjFilter, &ctx,
                                      pQuery->m_PolyFilterFn ? &LTHost_ClientRayPolyFilter : 0,
                                      pt, nrm, &hHit, true, false, &nPolyId))
                return LTFALSE;
            if (!hHit) { bDone = true; break; }         // the world itself

            hClientHit = LTReplicate_Find(hHit);
            if (!hClientHit)
            {
                // First sight: the mirror the client would have had all along.
                hClientHit = LTReplicate_Ensure(hHit);
                // The game's filter decides on what it can see -- the mirror,
                // or (an object the client cannot represent) the old answer.
                void* hAsk = hClientHit ? hClientHit : hHit;
                if (pQuery->m_FilterFn && !pQuery->m_FilterFn((HOBJECT)hAsk, pQuery->m_pUserData))
                {
                    excluded.insert(hHit);
                    continue;
                }
            }
            bDone = true;
        }
        if (!bDone) return LTFALSE;
    }
    pInfo->m_hPoly = nPolyId ? (HPOLY)nPolyId : INVALID_HPOLY;

    pInfo->m_Point.Init(pt[0], pt[1], pt[2]);
    pInfo->m_Plane.m_Normal.Init(nrm[0], nrm[1], nrm[2]);
    pInfo->m_Plane.m_Dist = nrm[0]*pt[0] + nrm[1]*pt[1] + nrm[2]*pt[2];

    // ⚠️ SERVER handles do not resolve on the client (§5p): a hit object is
    // reported as its client mirror, or as the world when it has none.
    pInfo->m_hObject = (HOBJECT)hClientHit;

    // LT_CLIENTRAYTRACE=1 -- every client ray: length, what it hit, and
    // whether that came back as an object (says so on first use).
    // LT_CLIENTRAYTRACE=2 -- only when the object a ray of that length hits
    // CHANGES: the activation/gadget ray (the weapon's range, 100 for the
    // lighter) becomes one line per new target, short enough for a player's
    // `play.sh --trace` log.
    static const int s_nTrace = []() {
        const char* e = getenv("LT_CLIENTRAYTRACE");
        const int n = e ? (atoi(e) == 2 ? 2 : 1) : 0;
        if (n) { printf("[CRAY] LT_CLIENTRAYTRACE=%d -- client rays and their hits%s\n", n,
                        n == 2 ? " (only when the target changes)" : ""); fflush(stdout); }
        return n; }();
    bool bSay = (s_nTrace != 0);
    if (s_nTrace == 2)
    {
        static std::map<int, void*> s_Last;      // ray length (rounded) -> last hit
        const float dx = to[0]-from[0], dy = to[1]-from[1], dz = to[2]-from[2];
        const int nLen = (int)(sqrtf(dx*dx + dy*dy + dz*dz) + 0.5f);
        std::map<int, void*>::iterator it = s_Last.find(nLen);
        bSay = (it == s_Last.end() || it->second != hHit);
        s_Last[nLen] = hHit;
    }
    if (bSay)
    {
        const float dx = to[0]-from[0], dy = to[1]-from[1], dz = to[2]-from[2];
        const float hx = pt[0]-from[0], hy = pt[1]-from[1], hz = pt[2]-from[2];
        float bmn[3] = { 0, 0, 0 }, bmx[3] = { 0, 0, 0 };
        const bool bBox = hHit && LTServerHost_RayBoxOf(hHit, bmn, bmx);
        printf("[CRAY] %.0f-unit ray from (%.0f %.0f %.0f) -> %s %s at %.0f, reported %s"
               "  point (%.1f %.1f %.1f)",
               sqrtf(dx*dx + dy*dy + dz*dz), from[0], from[1], from[2],
               hHit ? LTServerHost_ClassNameOf(hHit) : "world", hHit ? LTServerHost_ObjectNameOf(hHit) : "",
               sqrtf(hx*hx + hy*hy + hz*hz), hClientHit ? "as its mirror" : "as the world",
               pt[0], pt[1], pt[2]);
        if (bBox) printf("  box (%.1f %.1f %.1f)..(%.1f %.1f %.1f)", bmn[0], bmn[1], bmn[2], bmx[0], bmx[1], bmx[2]);
        printf("\n");
        fflush(stdout);
    }
    return LTTRUE;
}

// ⚠️ MOUSE LOOK. The game uses these as raw yaw/pitch deltas; the gain and the
// take-and-clear rule live in ltinput.h.
static void LTHost_GetAxisOffsets(LTFLOAT* pOffsets)
{
    if (!pOffsets) return;
    float f[3] = { 0.0f, 0.0f, 0.0f };
    LTInput_TakeAxisOffsets(f);
    pOffsets[0] = f[0]; pOffsets[1] = f[1]; pOffsets[2] = f[2];
}

// Defined with the sound host below (they need g_pHostSound).
static void LTHost_SetListener(LTBOOL bInClient, LTVector* pPos, LTRotation* pRot);
static void LTHost_GetListener(LTBOOL* pInClient, LTVector* pPos, LTRotation* pRot);

// ⚠️ The command state the whole movement system reads. See ltinput.h.
static LTBOOL LTHost_IsCommandOn(int nCommand)
{
    return LTCommand_IsOn(nCommand) ? LTTRUE : LTFALSE;
}


// The objects the game moves through the client physics -- CMoveMgr's own
// local object (CMoveMgr.cpp:2074), NOT the client object, which only follows
// it. These are what a lift carries (LTScene_CarryOnMovers). Handles are
// validated by the scene, so a removed one is simply skipped.
static std::set<void*> g_ClientMovers;

class LTHostClientPhysics : public LTStub_ILTClientPhysics
{
public:
    LTRESULT GetVelocity(HOBJECT hObj, LTVector* pVel) override
    {
        if (!pVel) return LT_INVALIDPARAMS;
        float v[3];
        const bool b = LTScene_GetVelocity((void*)hObj, v);
        pVel->Init(v[0], v[1], v[2]);
        return b ? LT_OK : LT_ERROR;
    }
    LTRESULT SetVelocity(HOBJECT hObj, LTVector* pVel) override
    {
        if (!pVel) return LT_INVALIDPARAMS;
        const float v[3] = { pVel->x, pVel->y, pVel->z };
        // LT_VELTRACE=1 -- every client SetVelocity with a vertical part: a
        // jump is the game asking for +y, and whether it asks is the question.
        static const bool s_bTrace = (getenv("LT_VELTRACE") != 0);
        if (s_bTrace && (v[1] > 1.0f || v[1] < -1.0f))
        {
            printf("[VEL] obj %p set (%.1f %.1f %.1f) ground=%d\n", (void*)hObj, v[0], v[1], v[2],
                   (int)LTScene_GetOnGround((void*)hObj));
            fflush(stdout);
        }
        return LTScene_SetVelocity((void*)hObj, v) ? LT_OK : LT_ERROR;
    }
    LTRESULT GetAcceleration(HOBJECT hObj, LTVector* pAccel) override
    {
        if (!pAccel) return LT_INVALIDPARAMS;
        float a[3];
        const bool b = LTScene_GetAcceleration((void*)hObj, a);
        pAccel->Init(a[0], a[1], a[2]);
        return b ? LT_OK : LT_ERROR;
    }
    LTRESULT SetAcceleration(HOBJECT hObj, LTVector* pAccel) override
    {
        if (!pAccel) return LT_INVALIDPARAMS;
        const float a[3] = { pAccel->x, pAccel->y, pAccel->z };
        return LTScene_SetAcceleration((void*)hObj, a) ? LT_OK : LT_ERROR;
    }
    LTRESULT GetObjectDims(HOBJECT hObj, LTVector* pDims) override
    {
        if (!pDims) return LT_INVALIDPARAMS;
        float d[3];
        const bool b = LTScene_GetPhysDims((void*)hObj, d);
        pDims->Init(d[0], d[1], d[2]);
        return b ? LT_OK : LT_ERROR;
    }

    // ⚠️ FRICTION ON THE CLIENT TOO. CMoveMgr sets this every frame while
    // standing (CMoveMgr.cpp:2030) and it was left a stub here when the rest
    // of the client physics was written -- so the SETTER threw the value away
    // and, worse, the GETTER left the caller's `float coeff` untouched. That
    // is the uninitialised out-parameter this port has now paid for five
    // times; it does not get a sixth.
    //
    // Stored per object rather than globally: the game sets a different
    // coefficient per surface the player is standing on.
    LTRESULT SetFrictionCoefficient(HOBJECT hObj, float coeff) override
    {
        m_Friction[(void*)hObj] = coeff;
        return LT_OK;
    }
    LTRESULT GetFrictionCoefficient(HOBJECT hObj, float& coeff) override
    {
        std::map<void*, float>::const_iterator it = m_Friction.find((void*)hObj);
        coeff = (it == m_Friction.end()) ? 0.0f : it->second;
        return LT_OK;
    }
    LTRESULT SetObjectDims(HOBJECT hObj, LTVector* pDims, uint32 flags) override
    {
        (void)flags;
        if (!pDims) return LT_INVALIDPARAMS;
        const float d[3] = { pDims->x, pDims->y, pDims->z };
        if (getenv("LT_DIMSTRACE"))
            printf("[DIMS/C] SET obj %p -> (%.1f %.1f %.1f) flags 0x%x\n",
                   (void*)hObj, d[0], d[1], d[2], flags);
        return LTScene_SetPhysDims((void*)hObj, d) ? LT_OK : LT_ERROR;
    }
    LTRESULT MoveObject(HOBJECT hObj, LTVector* pPos, uint32 flags) override
    {
        if (!pPos) return LT_INVALIDPARAMS;
        const float want[3] = { pPos->x, pPos->y, pPos->z };

        // LT_MOVERTRACE=1 -- the first 200 client moves after each level
        // entry (CMoveMgr's mover): where the client thinks the player is
        // when its fall-damage bookkeeping starts.
        if (g_nMoverTrace > 0)
        {
            --g_nMoverTrace;
            printf("[MOVER] obj %p %s -> (%.1f %.1f %.1f)\n", (void*)hObj,
                   (flags & MOVEOBJECT_TELEPORT) ? "TELEPORT" : "move", want[0], want[1], want[2]);
            fflush(stdout);
        }

        // ⚠️ THREE OF CMoveMgr's FOUR CALLS PASS THIS (CMoveMgr.cpp:2233,
        // 2257, 2307) -- respawn, level start, and being placed by the game.
        // Colliding on those would quietly relocate the player at level start.
        if (flags & MOVEOBJECT_TELEPORT)
        {
            LTScene_TeleportTo((void*)hObj, want);
            return LT_OK;
        }
        if (g_ClientMovers.size() > 16) g_ClientMovers.clear();
        g_ClientMovers.insert((void*)hObj);
        LTScene_MoveWithCollision((void*)hObj, want, 0);
        return LT_OK;
    }
    LTRESULT GetStandingOn(HOBJECT hObj, CollisionInfo* pInfo) override
    {
        if (!pInfo) return LT_INVALIDPARAMS;
        // Zeroed first, always -- CollisionInfo is a bare local at the call
        // site and the game reads m_hObject without checking. DEVIATIONS E-001.
        memset(pInfo, 0, sizeof(*pInfo));

        // ⚠️ REPORTING THE WORLD OBJECT IS WHAT LETS THE PLAYER LAND.
        // CMoveMgr tests `if (Info.m_hObject)` and nothing else; leaving it
        // null means "in the air" forever. m_hPoly stays INVALID, which the
        // game already handles -- it falls back to ST_UNKNOWN for the surface.
        // LT_GROUNDTRACE=1 -- each CHANGE of the answer, with the height: a
        // fall-damage death is "how long was the player not on the ground".
        {
            static const bool s_bTrace = (getenv("LT_GROUNDTRACE") != 0);
            static std::map<HOBJECT, int> s_Last;
            if (s_bTrace)
            {
                const int nNow = LTScene_GetOnGround((void*)hObj) ? 1 : 0;
                std::map<HOBJECT, int>::iterator it = s_Last.find(hObj);
                if (it == s_Last.end() || it->second != nNow)
                {
                    printf("[GROUND] t=%.2f obj %p %s\n", LTGetClock()->GetTime(),
                           (void*)hObj, nNow ? "ON ground" : "in the AIR");
                    fflush(stdout);
                    s_Last[hObj] = nNow;
                }
            }
        }
        if (LTScene_GetOnGround((void*)hObj))
        {
            pInfo->m_hObject = (HOBJECT)LTScene_WorldObject();

            // ⚠️ AND THE PLANE STOOD ON. CMoveMgr::UpdateOnGround, right after
            // "Info.m_hObject -> on ground", does
            //     if (Info.m_Plane.m_Normal.y < 0.707) m_bOnGround = LTFALSE;
            // ("make sure we fall down step slopes"). A zeroed plane is a
            // vertical wall, so the player was never on the ground: no jump,
            // ever (the game asked for no +y velocity), and the landing
            // bookkeeping ran on a falling player. The donor fills it from the
            // BSP node stood on (moveobject.cpp m_pNodeStandingOn), with its
            // polygon -- which is also the footstep surface (HANDOVER 5bi).
            // LT_FLATGROUND=1 = the old zeroed plane (announces itself).
            static const bool s_bOld = []() {
                const bool b = (getenv("LT_FLATGROUND") != 0);
                if (b) { printf("[PHYS] LT_FLATGROUND=1 -- GetStandingOn reports no ground plane\n"); fflush(stdout); }
                return b; }();
            if (!s_bOld)
            {
                float n[3] = { 0.0f, 1.0f, 0.0f }, pt[3] = { 0.0f, 0.0f, 0.0f };
                unsigned int nPoly = 0;
                if (LTScene_GetGroundContact((void*)hObj, n, pt, &nPoly))
                    pInfo->m_hPoly = (HPOLY)nPoly;
                else
                { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
                pInfo->m_Plane.m_Normal.Init(n[0], n[1], n[2]);
                pInfo->m_Plane.m_Dist = n[0]*pt[0] + n[1]*pt[1] + n[2]*pt[2];
            }
        }

        return LT_OK;
    }
    LTRESULT IsWorldObject(HOBJECT hObj) override
    {
        return LTScene_IsWorldObject((void*)hObj) ? LT_YES : LT_NO;
    }

    // ⚠️ THE CLIENT'S MOVEMENT INTEGRATOR, and the stub version of it is what
    // flung the player 1.2e21 units off the map.
    //
    // `CMoveMgr::UpdateMovement` (CMoveMgr.cpp:1806) does:
    //
    //     MoveInfo info;                       <- a bare local, uninitialised
    //     info.m_dt = GetFrameTime();
    //     pPhysics->UpdateMovement(&info);     <- was a stub: wrote NOTHING
    //     if (info.m_Offset.MagSqr() > 0.01f)  <- reads the garbage
    //         MoveObject(m_hObject, curPos + info.m_Offset, 0);
    //
    // ⚠️ THE STUB'S RETURN CODE IS IGNORED, so LT_ERROR communicated nothing;
    // the game moved by whatever was on the stack. Measured: a first frame
    // asking to travel 56,182,016 units straight down. This is the THIRD
    // untouched out-parameter to cost this port real time (§2, §5u) and the
    // pattern is always the same -- the caller believes the buffer.
    //
    // ⚠️ IT ALSO ADVANCES THE VELOCITY. The game reads velocity back through
    // GetVelocity to decide footstep sounds and fall damage, so an integrator
    // that produced an offset without updating velocity would leave those
    // reading zero forever.
    std::map<void*, float> m_Friction;

    LTRESULT UpdateMovement(MoveInfo* pInfo) override
    {
        if (!pInfo) return LT_INVALIDPARAMS;

        // Written FIRST and on every path, including the failure ones.
        pInfo->m_Offset.Init(0.0f, 0.0f, 0.0f);

        void* h = (void*)pInfo->m_hObject;
        if (!h) return LT_ERROR;

        float dt = pInfo->m_dt;
        if (dt <= 0.0f) return LT_OK;
        // Clamped for the reason every step in this port is: a stalled frame
        // must not teleport anything across the level.
        if (dt > 0.10f) dt = 0.10f;


        float v[3], a[3], g[3];
        LTScene_GetVelocity(h, v);
        LTScene_GetAcceleration(h, a);
        LTServerPhys_GetGlobalForce(g);

        // ⚠️ GRAVITY ONLY WHILE AIRBORNE. Resting on the floor it would
        // accumulate forever, and the first sideways step would launch the
        // player at whatever speed it had silently reached.
        const bool bGround = LTScene_GetOnGround(h);

        // ⚠️ AND ONLY FOR AN OBJECT THAT ASKS FOR IT. The donor passes
        // `(m_Flags & FLAG_GRAVITY) != 0` to CalcMotion (client_iltphysics.cpp
        // UpdateMovement). CMoveMgr clears the flag on a ladder
        // (UpdateOnLadder) and while swimming; applied anyway, Cate sank down
        // every ladder she let go of (170 -> 127 in 2.7 s, HANDOVER 5bm).
        // LT_ALWAYSGRAVITY=1 = the old rule (says so).
        static const bool s_bAlways = []() {
            const bool b = (getenv("LT_ALWAYSGRAVITY") != 0);
            if (b) { printf("[PHYS] LT_ALWAYSGRAVITY=1 -- gravity regardless of FLAG_GRAVITY\n"); fflush(stdout); }
            return b; }();
        const bool bGravity = s_bAlways || (LTScene_GetObjectFlags(h) & FLAG_GRAVITY) != 0;

        for (int k = 0; k < 3; ++k)
        {
            float acc = a[k];
            if (bGravity && (!bGround || k != 1)) acc += g[k];
            v[k] += acc * dt;
        }
        if (bGround && v[1] < 0.0f) v[1] = 0.0f;

        // ⚠️ FRICTION, AND WITHOUT IT THE PLAYER IS ON ICE. The integrator
        // added acceleration and never took anything away, so velocity only
        // ever grew and releasing a key left the player coasting. The game
        // SETS a coefficient for the surface being stood on
        // (CMoveMgr.cpp:2030) and this port was storing it and ignoring it.
        //
        // ⚠️ GROUND ONLY. Damping horizontal motion in mid-air would make
        // jumps and falls feel like wading, and the game already handles air
        // control itself.
        if (bGround)
        {
            float fCoeff = 0.0f;
            GetFrictionCoefficient(pInfo->m_hObject, fCoeff);

            // A sane floor: with no coefficient set yet the player must still
            // be able to stop. Expressed per second and converted to this
            // frame, so the feel does not change with frame rate.
            if (fCoeff <= 0.0f) fCoeff = 5.0f;

            float fDamp = 1.0f - fCoeff * dt;
            if (fDamp < 0.0f) fDamp = 0.0f;
            if (fDamp > 1.0f) fDamp = 1.0f;

            v[0] *= fDamp;
            v[2] *= fDamp;

            // ⚠️ Below a threshold, stop dead. Exponential decay never reaches
            // zero, and a player drifting at 0.01 units/s never reports as
            // standing still to anything that asks.
            if (v[0]*v[0] + v[2]*v[2] < 1.0f) { v[0] = 0.0f; v[2] = 0.0f; }
        }

        LTScene_SetVelocity(h, v);

        pInfo->m_Offset.Init(v[0] * dt, v[1] * dt, v[2] * dt);

        if (getenv("LT_PHYSTRACE"))
        {
            static unsigned n = 0;
            // Only the calls where the GAME asked for horizontal acceleration
            // -- the rest are the idle case and drown it out.
            if ((a[0] || a[2]) && n++ < 8)
                printf("[PHYS] UpdateMovement dt=%.4f accel=(%.0f %.0f %.0f) "
                       "g=(%.0f %.0f %.0f) ground=%d -> vel=(%.1f %.1f %.1f) "
                       "offset=(%.3f %.3f %.3f)\n",
                       dt, a[0], a[1], a[2], g[0], g[1], g[2], (int)bGround,
                       v[0], v[1], v[2],
                       pInfo->m_Offset.x, pInfo->m_Offset.y, pInfo->m_Offset.z);
        }
        return LT_OK;
    }
    LTRESULT GetGlobalForce(LTVector& vec) override
    {
        float f[3]; LTServerPhys_GetGlobalForce(f);
        vec.Init(f[0], f[1], f[2]);
        return LT_OK;
    }
    LTRESULT SetGlobalForce(LTVector& vec) override
    {
        const float f[3] = { vec.x, vec.y, vec.z };
        LTServerPhys_SetGlobalForce(f);
        return LT_OK;
    }
    LTRESULT GetStairHeight(float& fHeight) override
    {
        fHeight = LTServerPhys_GetStairHeight();
        return LT_OK;
    }
    LTRESULT SetStairHeight(float fHeight) override
    {
        LTServerPhys_SetStairHeight(fHeight);
        return LT_OK;
    }
};




// ⚠️ THE FOG THE LEVEL ASKS FOR, read from the CLIENT console every frame
// (HANDOVER 5bo) -- the game writes it there: CGameClientShell::ResetGlobalFog
// copies WorldProperties' values at level start, UpdateContainerFX swaps in a
// water volume's while the camera is inside. Defaults are the donor
// renderer's (rendererconsolevars.h: FogR/G/B 255, FogFarZ 2000, SkyFogFarZ
// 2000). Off when near == far (the donor's d3d_init.cpp rule). The sky fogs
// only when the world does and SkyFogEnable says so. LT_NOFOG=1 = no fog
// (says so).
//
// ⚠️ AND FarZ, THE FAR CLIP PLANE (HANDOVER 5bp), from the same console:
// ResetGlobalFog -> MirrorSConVar("FarZ") -> SetFarZ -> "+FarZ n" (only when
// n > 50); the loading screen holds it at 10000 and puts it back. Default
// 10000, the donor's (rendererconsolevars.h). A value at or inside the near
// plane (4) would leave no frustum, so it falls back to the default.
// LT_NOFARZ=1 = the old fixed 40000 (says so).
static void LTHost_UpdateFog()
{
    CLTWorldScene* pW = LTGetWorldScene();
    if (!pW) return;
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_NOFOG") != 0);
        if (b) { printf("[LT3D] LT_NOFOG=1 -- fog not drawn\n"); fflush(stdout); }
        return b; }();
    static const bool s_bNoFarZ = []() {
        const bool b = (getenv("LT_NOFARZ") != 0);
        if (b) { printf("[LT3D] LT_NOFARZ=1 -- FarZ ignored, far plane fixed at 40000\n"); fflush(stdout); }
        return b; }();
    CLTConsole* c = LTGetConsole();
    auto F = [c](const char* n, float fDef) -> float {
        const LTConsoleVar* v = c ? c->Find(n) : 0;
        if (!v || v->m_sValue.empty()) return fDef;
        return v->m_bHasFloat ? v->m_fValue : (float)atof(v->m_sValue.c_str());
    };
    // LT_FARZFORCE=n: a test override that wins over the console, so a
    // headless run can make the clip bite where a level's own FarZ is past
    // everything in view (says so).
    static const float s_fForce = []() {
        const char* p = getenv("LT_FARZFORCE");
        const float f = p ? (float)atof(p) : 0.0f;
        if (f > 0.0f) { printf("[LT3D] LT_FARZFORCE=%.0f -- FarZ forced, the level's is ignored\n", f); fflush(stdout); }
        return f; }();
    float fFarZ = 0.0f;
    if (!s_bNoFarZ)
    {
        fFarZ = s_fForce > 0.0f ? s_fForce : F("FarZ", 10000.0f);
        if (!(fFarZ > 4.0f)) fFarZ = 10000.0f;
    }
    pW->SetFarZ(fFarZ);
    // ⚠️ MIRRORS (HANDOVER 5de): the game's own "Mirrors" option writes
    // DrawPortals (FolderAdvDisplay.cpp:196) -- and FolderDisplay forces it
    // to 0 whenever BitDepth is 16. Not set at all = on: the port always
    // renders 32-bit, which is what the original asks for before it offers
    // reflections.
    LTWorldSetMirrors(F("DrawPortals", 1.0f) != 0.0f);
    LTFogState w, sky;
    if (!s_bOff)
    {
        w.m_bOn   = F("FogEnable", 0.0f) != 0.0f;
        w.m_fNear = F("FogNearZ", 0.0f);
        w.m_fFar  = F("FogFarZ", 2000.0f);
        w.m_RGB[0] = F("FogR", 255.0f); w.m_RGB[1] = F("FogG", 255.0f); w.m_RGB[2] = F("FogB", 255.0f);
        if (w.m_fNear == w.m_fFar) w.m_bOn = false;
        sky = w;
        sky.m_bOn   = w.m_bOn && F("SkyFogEnable", 0.0f) != 0.0f;
        sky.m_fNear = F("SkyFogNearZ", 0.0f);
        sky.m_fFar  = F("SkyFogFarZ", 2000.0f);
        if (sky.m_fNear == sky.m_fFar) sky.m_bOn = false;
        static const bool s_bTrace = (getenv("LT_FOGTRACE") != 0);
        static std::string s_Last;
        if (s_bTrace)
        {
            char b[200];
            snprintf(b, sizeof b, "world %s %.0f..%.0f rgb(%.0f %.0f %.0f)  sky %s %.0f..%.0f  farz %.0f",
                     w.m_bOn ? "ON" : "off", w.m_fNear, w.m_fFar, w.m_RGB[0], w.m_RGB[1], w.m_RGB[2],
                     sky.m_bOn ? "ON" : "off", sky.m_fNear, sky.m_fFar, fFarZ);
            if (s_Last != b) { printf("[FOG] %s\n", b); fflush(stdout); s_Last = b; }
        }
    }
    pW->SetFog(w, sky);
}

class LTSkeletonClient : public LTStub_ILTClient
{
public:
    // ⚠️ THE SERVER'S GAME CONSOLE, READ BY THE CLIENT (HANDOVER 5bo). Both
    // were stubs (0 / NULL), so CGameClientShell::ResetGlobalFog mirrored
    // "FogEnable 0" -- and "FarZ 0" -- into the client on EVERY level: no
    // level ever had its fog. In single player the server is in-process, so
    // the value is read where WorldProperties wrote it.
    float GetServerConVarValueFloat(char* pName) override
    {
        LTIMPL("ILTClient::GetServerConVarValueFloat");
        float f = 0.0f;
        LTServerHost_GameConVarFloat(pName, &f);
        return f;
    }
    char* GetServerConVarValueString(char* pName) override
    {
        LTIMPL("ILTClient::GetServerConVarValueString");
        return (char*)LTServerHost_GameConVarString(pName);
    }

    // ⚠️ THE CLIENT'S CONTAINERS (HANDOVER 5bn). CGameClientShell::
    // UpdateContainerFX asks which container holds the CAMERA, then that
    // container's code and its CVolumeBrushFX -- the underwater tint, fog and
    // sound. Both calls were stubs ("none"). The answer is the server's
    // (LTServerHost_PointContainers), handed back as the containers' CLIENT
    // copies -- the handles the volume's effect is attached to.
    uint32 GetPointContainers(LTVector* pPoint, HOBJECT* pList, uint32 maxListSize) override
    {
        LTIMPL("ILTCSBase::GetPointContainers");
        if (!pPoint || !pList || !maxListSize) return 0;
        void* srv[16];
        const float pt[3] = { pPoint->x, pPoint->y, pPoint->z };
        const unsigned int nS = LTServerHost_PointContainers(pt, srv, 16);
        uint32 n = 0;
        for (unsigned int i = 0; i < nS && n < maxListSize; ++i)
            if (void* hClient = LTReplicate_Ensure(srv[i])) pList[n++] = (HOBJECT)hClient;
        return n;
    }
    LTBOOL GetContainerCode(HOBJECT hObj, uint16* pCode) override
    {
        LTIMPL("ILTCSBase::GetContainerCode");
        if (!pCode) return LTFALSE;
        *pCode = 0;
        void* hServer = LTReplicate_ServerOf((void*)hObj);
        unsigned short c = 0;
        if (!hServer || !LTServerHost_ContainerCode(hServer, &c)) return LTFALSE;
        *pCode = c;
        return LTTRUE;
    }
    LTSkeletonClient()
    {
        // ⚠️ GENERATED WIRING FIRST -- it assigns EVERY sub-interface data
        // member, including the four on ILTClient itself (m_pVideoMgr,
        // m_pTexMod, m_pCursorLT, m_pDirectMusicMgr) that this constructor used
        // to leave NULL. That omission was invisible for as long as the game
        // stopped before CInterfaceMgr::InitCursor, and then it was a segfault
        // with no [LTSTUB] line in front of it. Generating the list means there
        // is no next one to miss.
        LTSkel_WireData();

        // The music manager (HANDOVER 5cj) replaces the generated stub the
        // wiring just installed -- unless LT_NOMUSIC=1, which says so.
        if (ILTDirectMusicMgr* pMusic = LTDM_HostSetup()) m_pDirectMusicMgr = pMusic;

        // Then the deliberate choices, which must come SECOND so they win.
        // Safe in the constructor body: base and members are both fully
        // constructed by the time it runs.
        m_pCommonLT    = &m_common;
        m_pPhysicsLT   = &m_physics;
        m_pModelLT     = &m_model;
        m_pTransformLT = &m_transform;
        m_pLightAnimLT = &m_lightAnim;
        m_pSoundMgr    = &m_soundMgr;

        // ⚠️ AND the 208 function-pointer MEMBERS. These are not virtuals, so
        // no override reaches them; left NULL they jump to address zero with no
        // diagnostic whatsoever. This is what crashed HookWindow() at
        // g_pLTClient->GetScreenSurface() with pc 0x0.
        // (ILTCSBase has none -- it is a pure abstract class.)
        LTSkel_WireFnPtrs_ILTClient(this);

        // ...then replace the two that carry a diagnostic. Order matters:
        // wiring runs first and would overwrite these.
        // ⚠️ EVERY MOVEMENT DECISION IN THE GAME GOES THROUGH THIS. The
        // generated stub answered LTFALSE, so COMMAND_ID_FORWARD was never on
        // and the player could not walk regardless of the physics under it.
        IsCommandOn         = LTHost_IsCommandOn;
        SetListener         = LTHost_SetListener;
        GetListener         = LTHost_GetListener;
        GetAxisOffsets      = LTHost_GetAxisOffsets;
        IntersectSegment    = LTHost_ClientIntersectSegment;
        GetPolyTextureFlags = LTHost_ClientPolyTextureFlags;
        StartDeviceTrack    = LTHost_StartDeviceTrack;
        TrackDevice         = LTHost_TrackDevice;
        EndDeviceTrack      = LTHost_EndDeviceTrack;
        GetDeviceName       = LTHost_GetDeviceName;
        GetDeviceBindings   = LTHost_GetDeviceBindings;
        FreeDeviceBindings  = LTHost_FreeDeviceBindings;
        GetDeviceObjects    = LTHost_GetDeviceObjects;
        FreeDeviceObjects   = LTHost_FreeDeviceObjects;

        ShutdownWithMessage = LTHost_ShutdownWithMessage;
        DebugOut            = LTHost_DebugOut;
        GetEngineHook       = LTHost_GetEngineHook;

        // ⚠️ THE SOUND ENTRY THE GAME ACTUALLY USES. `m_pSoundMgr` above is
        // the modern ILTSoundMgr and it has been wired for a while; the game
        // does not call it. `CClientSoundMgr::PlaySound` ends at
        // `g_pLTClient->PlaySound(&psi)` (ClientSoundMgr.cpp:181), which is
        // this FUNCTION POINTER -- the hybrid interface again (§4.5), and a
        // whole sound system sitting behind an unwired pointer.
        PlaySound        = LTHost_ClientPlaySound;
        SetSoundVolume   = LTHost_SndSetVolume;
        GetSoundVolume   = LTHost_SndGetVolume;

        // ⚠️ THE REST OF THE FAMILY, and they are wired for one reason each
        // rather than for tidiness. InitSound is what the game calls before it
        // will play anything at all; KillSound and KillSoundLoop are how a
        // cinematic stops a line it is skipping; GetSoundDuration is how the
        // dialogue window decides how long to leave a subtitle up.
        InitSound        = LTHost_SndInit;
        KillSound        = LTHost_SndKill;
        KillSoundLoop    = LTHost_SndKillLoop;
        PauseSounds      = LTHost_SndPause;
        ResumeSounds     = LTHost_SndResume;
        GetSoundDuration = LTHost_SndDuration;

        // ⚠️ LIP SYNC. CNodeController averages the playing voice's amplitude
        // around the play cursor and opens the jaw by it; with these stubbed
        // the average was 0 and no character's mouth ever moved.
        GetSoundData     = LTHost_SndGetData;
        GetSoundOffset   = LTHost_SndGetOffset;

        // ...and then the null renderer, for the same reason and in the same
        // order. Everything it does NOT claim stays an announcing LT_ERROR
        // stub, which is how the census keeps choosing the next piece of work.
        LTNullRender_Wire(this);
        LTScene_Wire(this);
        LTConsole_Wire(this);

        // The cursor is a DATA member (ILTClient::Cursor() is an inline
        // accessor), so it is replaced the same way the sub-interfaces are:
        // after LTSkel_WireData has put a stub there.
        m_pCursorLT = LTGetNullCursor();
    }

    // ------------------------------------------------------------------ //
    // THE FIRST REAL ENGINE METHOD.
    //
    // Everything else on this object still returns LT_ERROR and says so. This
    // one does the job: it is census entry 5, and the reason the run stopped
    // there was that CGameButeMgr::Parse (Shared/GameButeMgr.cpp:117) could
    // not open a single attribute file, so m_GlobalMgr.Init() failed and
    // OnEngineInitialized returned LT_ERROR.
    //
    // OpenFile is a VIRTUAL on ILTCSBase, not one of ILTClient's 208 function
    // pointers, so overriding it here is enough -- see HANDOVER.md §4.5 for
    // why that distinction is worth checking before assuming.
    // ------------------------------------------------------------------ //

    LTRESULT OpenFile(char *pFilename, ILTStream **pStream) override
    {
        if (pStream) *pStream = 0;
        if (!pFilename || !pStream) return LT_ERROR;

        ILTStream* p = LTGetFileMgr()->OpenFile(pFilename);
        if (!p)
        {
            // Reported once per distinct name, not once per call: a retry loop
            // would otherwise bury everything else in the run.
            LTFile_ReportMiss(pFilename);
            return LT_ERROR;
        }

        *pStream = p;
        return LT_OK;
    }

    LTRESULT CopyFile(const char *pszSourceFile, const char *pszDestFile) override
    {
        if (!pszSourceFile || !pszDestFile) return LT_ERROR;
        return LTGetFileMgr()->CopyFileTo(pszSourceFile, pszDestFile) ? LT_OK : LT_ERROR;
    }

    // ------------------------------------------------------------------ //
    // ClientRes: the string table.
    //
    // These are VIRTUALS on ILTCSBase (iltcsbase.h:173-186), not function
    // pointers, so overriding reaches them -- but check, do not assume; the
    // interface is a hybrid and the two shapes look identical at the call site
    // (HANDOVER.md §4.5).
    //
    // ⚠️ FormatString takes `...`, so it CANNOT forward by calling another
    // variadic function -- the va_list has to be started here and passed by
    // reference. That is why CLTStringMgr::Format takes a va_list rather than
    // an ellipsis of its own.
    // ------------------------------------------------------------------ //

    // The clock. ⚠️ Virtuals on ILTCSBase, and the reason the first composited
    // frame was black: with these returning 0 the interface fade never
    // advanced, so a correct compositor drew a correct full-screen black
    // overlay three times. See ltclock.h.
    LTFLOAT GetTime() override      { return LTGetClock()->GetTime(); }
    LTFLOAT GetFrameTime() override { return LTGetClock()->GetFrameTime(); }

    // ⚠️ THE GAME'S OWN VOICE ON THE CLIENT SIDE -- see the server's CPrint
    // for why a stub here was expensive. `CSPrint` is a wrapper around this
    // and it is all over the client shell, including the interface state
    // machine's own change-of-state reporting.
    void CPrint(char* pMsg, ...) override
    {
        static const bool bQuiet = (getenv("LT_NOGAMEPRINT") != 0);
        if (bQuiet || !pMsg) return;

        char szBuf[1024];
        va_list args;
        va_start(args, pMsg);
        vsnprintf(szBuf, sizeof(szBuf), pMsg, args);
        va_end(args);

        printf("[GAME/C] %s\n", szBuf);
        fflush(stdout);
    }

    HSTRING FormatString(int messageCode, ...) override
    {
        va_list args;
        va_start(args, messageCode);
        HSTRING h = LTGetStringMgr()->Format(messageCode, args);
        va_end(args);
        return h;
    }

    HSTRING CreateString(char *pString) override   { return LTGetStringMgr()->Create(pString); }
    HSTRING CopyString(HSTRING hString) override   { return LTGetStringMgr()->Copy(hString); }
    void    FreeString(HSTRING hString) override   { LTGetStringMgr()->Free(hString); }
    char*   GetStringData(HSTRING hString) override { return LTStringData_ForGame(hString); }

    LTBOOL  CompareStrings(HSTRING a, HSTRING b) override      { return LTGetStringMgr()->Compare(a, b); }
    LTBOOL  CompareStringsUpper(HSTRING a, HSTRING b) override { return LTGetStringMgr()->CompareUpper(a, b); }

    // ---- the message pipe, client end -------------------------------- //
    //
    // ⚠️ THE OTHER HALF OF engine/shared/ltmessage.cpp. ILTClient declares its
    // own StartMessage/EndMessage/EndMessage2 as pure virtuals -- separate
    // declarations from ILTServer's, with different signatures -- so each side
    // has to override its own. The BUFFER and the queue are shared; only the
    // address is not, and the client has exactly one address to send to.
    HMESSAGEWRITE StartMessage(uint8 messageID) override
    {
        return (HMESSAGEWRITE)LTMessage_StartToServer(
                    LTServerHost_ClientHandle(), messageID);
    }

    HMESSAGEWRITE StartHMessageWrite() override
    {
        return (HMESSAGEWRITE)LTMessage_StartData();
    }

    LTRESULT EndMessage(HMESSAGEWRITE h) override
    {
        return LTMessage_End((CLTMessage*)h, MESSAGE_GUARANTEED);
    }

    LTRESULT EndMessage2(HMESSAGEWRITE h, uint32 flags) override
    {
        return LTMessage_End((CLTMessage*)h, flags);
    }

    // ---- model animation --------------------------------------------- //
    //
    // ⚠️ All VIRTUALS, and note the pair: ILTClient and ILTCSBase BOTH declare
    // SetModelAnimation/SetModelLooping. Overriding only one leaves the other
    // a stub, and which one the game reaches depends on the pointer it holds.
    // Both are routed to the same place.
    // ⚠️ NODE CONTROL -- the game's hook into a model's hierarchy, and how
    // NOLF moves a speaking character's jaw. Was a stub; see
    // LTScene_SetNodeControl. The SDK signature is cast to the SDK-free one
    // the scene stores: (HOBJECT, HMODELNODE, LTMatrix*, void*) and
    // (void*, unsigned, void*, void*) are the same four registers.
    LTRESULT ModelNodeControl(HOBJECT hObj, NodeControlFn fn, void* pUserData) override
    {
        LTIMPL("ILTClient::ModelNodeControl");
        LTScene_SetNodeControl((void*)hObj, (LTSceneNodeFn)fn, pUserData);
        return LT_OK;
    }

    HMODELANIM GetAnimIndex(HOBJECT hObj, char* pAnimName) override
    {
        LTIMPL("ILTCSBase::GetAnimIndex");
        int n = LTScene_GetAnimIndex(hObj, pAnimName);
        return (n < 0) ? INVALID_MODEL_ANIM : (HMODELANIM)n;
    }
    void SetModelAnimation(HLOCALOBJ hObj, uint32 iAnim) override
    {
        LTIMPL("ILTClient::SetModelAnimation");
        LTScene_SetAnim(hObj, (iAnim == INVALID_MODEL_ANIM) ? -1 : (int)iAnim);
    }
    void SetModelLooping(HLOCALOBJ hObj, LTBOOL bLoop) override
    {
        LTIMPL("ILTClient::SetModelLooping");
        LTScene_SetLooping(hObj, bLoop != 0);
    }

    // ⚠️ THE READS, AND THE WEAPON IS MADE OF THEM. Setting an animation was
    // implemented from the start; reading one back was not, and NOLF's weapon
    // is a state machine that only ever asks:
    //
    //     dwState = GetModelPlaybackState(m_hObject);        WeaponModel.cpp:2188
    //     if (!IsFireAni(GetModelAnimation(m_hObject)) || (dwState & MS_PLAYDONE))
    //
    // With GetModelAnimation stuck at 0 and the state stuck at "not done" the
    // machine cannot leave whatever state it is in. That is the whole of
    // "fire does nothing": the trigger was delivered, the command was set, and
    // the weapon could not act on it.
    //
    // ⚠️ ILTClient AND ILTCSBase BOTH DECLARE THESE. Overriding one leaves the
    // other a stub and which the game reaches depends on the pointer it holds
    // -- the pair trap from HANDOVER 4.5, which has now cost this port
    // GetModelAnimUserDims, the rotation maths and this. Both are routed here.
    uint32 GetModelAnimation(HLOCALOBJ hObj) override
    {
        LTIMPL("ILTClient::GetModelAnimation");
        const int n = LTScene_GetAnim(hObj);
        return (n < 0) ? (uint32)INVALID_MODEL_ANIM : (uint32)n;
    }
    LTBOOL GetModelLooping(HLOCALOBJ hObj) override
    {
        LTIMPL("ILTClient::GetModelLooping");
        return LTScene_GetLooping(hObj) ? LTTRUE : LTFALSE;
    }
    LTRESULT ResetModelAnimation(HLOCALOBJ hObj) override
    {
        LTIMPL("ILTClient::ResetModelAnimation");
        LTScene_RestartAnim(hObj);
        return LT_OK;
    }
    uint32 GetModelPlaybackState(HLOCALOBJ hObj) override
    {
        LTIMPL("ILTClient::GetModelPlaybackState");
        return LTScene_IsAnimDone(hObj) ? (uint32)MS_PLAYDONE : (uint32)0;
    }

    // ---- rotation maths ---------------------------------------------- //
    //
    // ⚠️ ALL VIRTUALS on ILTCSBase, and all load-bearing: the interface builds
    // every menu object's position out of the camera's up/right/forward
    // vectors. While GetRotationVectors was a stub those three globals held
    // uninitialised memory and every FX landed at a NaN position, silently.
    // See ltmathimpl.h.
    //
    // ⚠️ The parameter order here is (UP, RIGHT, FORWARD) -- ILTCSBase's, not
    // ILTMath's, which swaps the first two. iltcsbase.h:210 warns about this
    // in capitals.
    LTRESULT GetRotationVectors(LTRotation* pRotation, LTVector* pUp,
                                LTVector* pRight, LTVector* pForward) override
    {
        LTIMPL("ILTCSBase::GetRotationVectors");
        if (!pRotation) return LT_ERROR;
        float r[3], u[3], f[3];
        float q[4] = { (*pRotation)[0], (*pRotation)[1], (*pRotation)[2], (*pRotation)[3] };
        LTMathImpl::GetVectors(q, r, u, f);
        if (pRight)   pRight->Init(r[0], r[1], r[2]);
        if (pUp)      pUp->Init(u[0], u[1], u[2]);
        if (pForward) pForward->Init(f[0], f[1], f[2]);
        return LT_OK;
    }

    void RotateAroundAxis(LTRotation* pRotation, LTVector* pAxis, float amount) override
    {
        LTIMPL("ILTCSBase::RotateAroundAxis");
        if (!pRotation || !pAxis) return;
        float q[4] = { (*pRotation)[0], (*pRotation)[1], (*pRotation)[2], (*pRotation)[3] };
        float a[3] = { pAxis->x, pAxis->y, pAxis->z };
        LTMathImpl::RotateAroundAxis(q, a, amount);
        pRotation->Init(q[0], q[1], q[2], q[3]);
    }

    void EulerRotateX(LTRotation* pRotation, float amount) override
    {
        LTIMPL("ILTCSBase::EulerRotateX");
        LTVector ax(1.0f, 0.0f, 0.0f); RotateAroundAxis(pRotation, &ax, amount);
    }
    void EulerRotateY(LTRotation* pRotation, float amount) override
    {
        LTIMPL("ILTCSBase::EulerRotateY");
        LTVector ax(0.0f, 1.0f, 0.0f); RotateAroundAxis(pRotation, &ax, amount);
    }
    void EulerRotateZ(LTRotation* pRotation, float amount) override
    {
        LTIMPL("ILTCSBase::EulerRotateZ");
        LTVector ax(0.0f, 0.0f, 1.0f); RotateAroundAxis(pRotation, &ax, amount);
    }

    void AlignRotation(LTRotation* pRotation, LTVector* pVector, LTVector* pUp) override
    {
        LTIMPL("ILTCSBase::AlignRotation");
        if (!pRotation || !pVector) return;
        float q[4];
        float d[3] = { pVector->x, pVector->y, pVector->z };
        float u[3] = { 0.0f, 1.0f, 0.0f };
        if (pUp) { u[0] = pUp->x; u[1] = pUp->y; u[2] = pUp->z; }
        LTMathImpl::AlignRotation(q, d, u);
        pRotation->Init(q[0], q[1], q[2], q[3]);
    }

    LTRESULT SetupEuler(LTRotation* pRotation, float pitch, float yaw, float roll) override
    {
        LTIMPL("ILTCSBase::SetupEuler");
        if (!pRotation) return LT_ERROR;
        float q[4];
        LTMathImpl::SetupEuler(q, pitch, yaw, roll);
        pRotation->Init(q[0], q[1], q[2], q[3]);
        return LT_OK;
    }

    LTRESULT InterpolateRotation(LTRotation* pDest, LTRotation* pRot1,
                                 LTRotation* pRot2, float t) override
    {
        LTIMPL("ILTCSBase::InterpolateRotation");
        if (!pDest || !pRot1 || !pRot2) return LT_ERROR;
        float a[4] = { (*pRot1)[0], (*pRot1)[1], (*pRot1)[2], (*pRot1)[3] };
        float b[4] = { (*pRot2)[0], (*pRot2)[1], (*pRot2)[2], (*pRot2)[3] };
        float o[4];
        LTMathImpl::Slerp(o, a, b, t);
        pDest->Init(o[0], o[1], o[2], o[3]);
        return LT_OK;
    }

    // ---- console variables ------------------------------------------- //
    //
    // ⚠️ GetConsoleVar / RunConsoleString are function POINTERS (iltclient.h
    // :634, :731); GetVarValueFloat / GetVarValueString are VIRTUALS
    // (iltcsbase.h:191, :195). The hybrid again -- §4.5.
    float GetVarValueFloat(HCONSOLEVAR hVar) override
    {
        LTIMPL("ILTCSBase::GetVarValueFloat");
        return LTGetConsole()->GetFloat((const LTConsoleVar*)hVar);
    }
    char* GetVarValueString(HCONSOLEVAR hVar) override
    {
        LTIMPL("ILTCSBase::GetVarValueString");
        // NULL for a NULL handle, as the SDK says -- see the server's copy.
        if (!hVar && !getenv("LT_NULLVARSTRING")) return 0;
        return (char*)LTGetConsole()->GetString((const LTConsoleVar*)hVar);
    }

    // ---- the scene's VIRTUAL half ------------------------------------ //
    //
    // ⚠️ These five cannot be assigned by LTScene_Wire because they are
    // virtuals, not function pointers -- §4.5's hybrid trap. The rest of the
    // object API is wired as pointers in LTScene_Wire; these have to be here,
    // and forgetting one is a silent no-op rather than a crash.
    LTRESULT RemoveObject(HOBJECT hObj) override
    {
        LTIMPL("ILTCSBase::RemoveObject");
        return LTScene_RemoveObject(hObj) ? LT_OK : LT_ERROR;
    }
    LTRESULT SetObjectPos(HLOCALOBJ hObj, LTVector* pPos, LTBOOL bForce = LTFALSE) override
    {
        LTIMPL("ILTClient::SetObjectPos");
        if (!pPos) return LT_ERROR;
        return LTScene_SetObjectPos(hObj, pPos->x, pPos->y, pPos->z) ? LT_OK : LT_ERROR;
    }
    uint32 GetObjectFlags(HOBJECT hObj) override
    {
        LTIMPL("ILTClient::GetObjectFlags");
        return LTScene_GetObjectFlags(hObj);
    }
    void SetObjectFlags(HOBJECT hObj, uint32 flags) override
    {
        LTIMPL("ILTClient::SetObjectFlags");
        LTScene_SetObjectFlags(hObj, flags);
    }

private:
    // ⚠️ A REAL ILTCommon, and it is not optional.
    //
    //   ILTCSBase::GetObjectType is an inline wrapper (iltcsbase.h:208):
    //
    //       uint32 GetObjectType(HOBJECT h)
    //       { uint32 temp; Common()->GetObjectType(h, &temp); return temp; }
    //
    //   `temp` is UNINITIALISED and a stub Common() never writes it -- so the
    //   caller gets stack garbage. CInterfaceMgr::AddInterfaceSFX guards its
    //   whole animation-selection block on
    //   `GetObjectType(hObj) == OT_MODEL` (InterfaceMgr.cpp:4940), which was
    //   therefore a coin flip. The same shape as GetObjectPos and
    //   GetRotationVectors: an out-parameter the caller never initialises.
    class HostCommon : public LTStub_ILTCommon
    {
    public:
        // ⚠️ THE COMMAND PARSER, shared with the server -- see ltconparse.h.
        // The client reaches it through Common() for the same trigger strings
        // the server does.
        LTRESULT Parse(ConParse* pParse) override { return LTConParse_Common(pParse); }

        // ⚠️ THE PLAYER'S FACING, AND IT NEVER REACHED THE SERVER (HANDOVER
        // 5df). CommonUtilities.cpp's CompressRotationByte -- how the client
        // sends its yaw in every MID_PLAYER_UPDATE (GameClientShell.cpp:2246)
        // -- asks THIS interface for the rotation's vectors, and the stub
        // wrote none: atan2 of untouched locals, the server's player always
        // facing +Z whatever way she looked. Seen first in a mirror, where
        // Cate stood with her back to it. The server's twin of these two
        // (ltserverhost.cpp LTHostServerCommon) uses the same LTMathImpl, so
        // both ends agree on what a yaw is. LT_OLDCLIENTROT=1 = the stubs
        // (old, says so).
        static bool OldClientRot()
        {
            static const bool b = []() {
                const bool v = (getenv("LT_OLDCLIENTROT") != 0);
                if (v) { printf("[HOST] LT_OLDCLIENTROT=1 -- client ILTCommon rotation helpers are stubs (old)\n"); fflush(stdout); }
                return v; }();
            return b;
        }
        LTRESULT GetRotationVectors(LTRotation& rot, LTVector& up, LTVector& right, LTVector& fwd) override
        {
            LTIMPL("ILTCommon::GetRotationVectors");
            if (OldClientRot()) return LTStub_ILTCommon::GetRotationVectors(rot, up, right, fwd);
            const float q[4] = { rot.m_Quat[0], rot.m_Quat[1], rot.m_Quat[2], rot.m_Quat[3] };
            float r[3], u[3], f[3];
            LTMathImpl::GetVectors(q, r, u, f);
            right.Init(r[0], r[1], r[2]); up.Init(u[0], u[1], u[2]); fwd.Init(f[0], f[1], f[2]);
            return LT_OK;
        }
        LTRESULT SetupEuler(LTRotation& rot, float pitch, float yaw, float roll) override
        {
            LTIMPL("ILTCommon::SetupEuler");
            if (OldClientRot()) return LTStub_ILTCommon::SetupEuler(rot, pitch, yaw, roll);
            float q[4];
            LTMathImpl::SetupEuler(q, pitch, yaw, roll);
            rot.Init(q[0], q[1], q[2], q[3]);
            return LT_OK;
        }

        // ⚠️ THE PLAYER'S SIZE. CMoveMgr asks for this to size its collision
        // box (CMoveMgr.cpp:1674); the stub left it at its (1,1,1) default, so
        // the player was a one-unit sphere and a carpet was an obstacle.
        //
        // ⚠️ Written on every path -- the game passes a bare LTVector and uses
        // it whatever the return says.
        LTRESULT GetModelAnimUserDims(HOBJECT hObj, LTVector* pDims,
                                      HMODELANIM hAnim) override
        {
            if (!pDims) return LT_INVALIDPARAMS;

            float d[3] = { 0.0f, 0.0f, 0.0f };
            const bool bGot = LTScene_GetAnimDims((void*)hObj, (int)hAnim, d);
            if (getenv("LT_DIMSTRACE"))
            {
                float cur0[3] = { 0, 0, 0 };
                LTScene_GetPhysDims((void*)hObj, cur0);
                printf("[DIMS/C] obj %p anim %d -> %s (%.1f %.1f %.1f)   now "
                       "(%.1f %.1f %.1f)\n", (void*)hObj, (int)hAnim,
                       bGot ? "ok " : "MISS", d[0], d[1], d[2],
                       cur0[0], cur0[1], cur0[2]);
                fflush(stdout);
            }
            if (bGot && (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f))
            {
                pDims->Init(d[0], d[1], d[2]);
                return LT_OK;
            }

            // ⚠️ ON FAILURE, THE DIMS THE OBJECT ALREADY HAS -- never zeros,
            // and this one put the player through the lobby floor.
            //
            // CMoveMgr::UpdateModelDims (CMoveMgr.cpp:1673) seeds (1,1,1) and
            // then calls this, using whatever comes back without looking at
            // the return -- the seed IS the game's fallback. Writing zeros
            // overwrote it, and a zero-radius player is inside no geometry at
            // all: measured on T01S02, one frame at r=1.0 carried the player
            // from y=31.5 to y=2.5 straight through a floor at y=0, and the
            // next frame with the real radius expelled it downward to -24.
            //
            // The lookup fails for a frame or two whenever the model is not
            // resolvable yet, which a level change guarantees.
            float cur[3] = { 0.0f, 0.0f, 0.0f };
            if (LTScene_GetPhysDims((void*)hObj, cur) &&
                (cur[0] != 0.0f || cur[1] != 0.0f || cur[2] != 0.0f))
                pDims->Init(cur[0], cur[1], cur[2]);
            // else: leave the game's own seed exactly as it passed it.
            return LT_ERROR;
        }

        // ⚠️ THE CLIENT'S SetObjectFilenames -- the server's has existed since
        // the AI work; the client's was the stub, so changing weapon never
        // changed the player-view model (the ILTCSBase pair trap, again).
        LTRESULT SetObjectFilenames(HOBJECT hObj, ObjectCreateStruct* pStruct) override
        {
            LTIMPL("ILTCommon::SetObjectFilenames (client)");
            if (!hObj || !pStruct) return LT_ERROR;
            std::vector<std::string> skins;
            for (int k = 0; k < MAX_MODEL_TEXTURES; ++k)
                if (pStruct->m_SkinNames[k][0]) skins.push_back(pStruct->m_SkinNames[k]);
            return LTScene_SetObjectFilenames((void*)hObj, pStruct->m_Filename, skins)
                ? LT_OK : LT_ERROR;
        }

        LTRESULT GetPolyTextureFlags(HPOLY hPoly, uint32* pFlags) override
        {
            LTIMPL("ILTCommon::GetPolyTextureFlags (client)");
            return LTHost_ClientPolyTextureFlags(hPoly, pFlags);
        }

        LTRESULT GetObjectType(HOBJECT hObj, uint32* type) override
        {
            LTIMPL("ILTCommon::GetObjectType");
            if (!type) return LT_ERROR;
            *type = 0;
            return LTScene_GetObjectType(hObj, type) ? LT_OK : LT_ERROR;
        }
        // ⚠️ flagType PICKS THE WORD, and it used to be ignored: every
        // OFT_Flags2 call (20 in ClientShellDLL) read and wrote the MAIN
        // flags. FLAG2_MULTIPLY / _ADDITIVE / _CHROMAKEY landed on
        // FLAG_ bits 3 / 2 / 1, so no client-made model or sprite ever
        // got its blend mode -- the intel popup's multiply backdrop drew
        // as an opaque sheet -- and PVFXMgr.cpp:233's
        // FLAG2_PORTALINVISIBLE (bit 0) switched FLAG_VISIBLE on.
        // LT_OLDFLAGTYPE=1 = the old single word (announces itself).
        static bool OldFlagType()
        {
            static const bool b = []() {
                const bool bOld = (getenv("LT_OLDFLAGTYPE") != 0);
                if (bOld) { printf("[HOST] LT_OLDFLAGTYPE=1 -- ILTCommon object flags ignore OFT_Flags2 (old)\n"); fflush(stdout); }
                return bOld; }();
            return b;
        }
        LTRESULT GetObjectFlags(const HOBJECT hObj, const ObjFlagType flagType,
                                uint32& dwFlags) override
        {
            LTIMPL("ILTCommon::GetObjectFlags");
            dwFlags = (flagType == OFT_Flags2 && !OldFlagType())
                      ? LTScene_GetObjectFlags2(hObj) : LTScene_GetObjectFlags(hObj);
            return LT_OK;
        }
        LTRESULT SetObjectFlags(HOBJECT hObj, const ObjFlagType flagType,
                                uint32 dwFlags) override
        {
            LTIMPL("ILTCommon::SetObjectFlags");
            if (flagType == OFT_Flags2 && !OldFlagType()) LTScene_SetObjectFlags2(hObj, dwFlags);
            else                                           LTScene_SetObjectFlags(hObj, dwFlags);
            return LT_OK;
        }
    };
    HostCommon                m_common;
    LTHostClientPhysics       m_physics;      // the client-side variant
    // ⚠️ A real ILTModel, for sockets. Attachments (Cate's sunglasses and her
    // P38) are separate OT_MODEL objects that the interface parents to a
    // socket on the character -- CBaseFolder::CreateAttachFX returns early if
    // GetSocket fails, so a stub here silently drops every attachment.
    class HostModel : public LTStub_ILTModel
    {
        // ⚠️ THE CLIENT'S ANIMATION TRACKERS, which were all stubs -- and the
        // stub GetCurAnim wrote nothing, so CMoveMgr sized the player's box from
        // an uninitialised local: crouching never shrank it (HANDOVER 5bi).
        // LithTech replicates a server object's trackers to its client object
        // by index; here the client's k-th tracker READS the server's k-th
        // (AddTracker order). The server owns the animation; client writes
        // are accepted and have no effect.
        struct CliTracker { void* hObj; int nIndex; };
        std::map<LTAnimTracker*, CliTracker> m_Trackers;
    public:
        LTRESULT AddTracker(HOBJECT hObj, LTAnimTracker* pTracker) override
        {
            LTIMPL("ILTModel::AddTracker");
            if (!hObj || !pTracker) return LT_ERROR;
            // ⚠️ THE INDEX IS THE POSITION IN THE CALLER'S ARRAY, not a count
            // per object. CCharacterFX adds m_aAnimTrackers[0..n-1] in a loop
            // (CharacterFX.cpp:121), and the player's effect can be created
            // twice -- a per-object count then asked the server for tracker 6
            // of 5. The previous array slot registered on the same object
            // means "next index"; otherwise this is a new array, index 0.
            int n = 0;
            std::map<LTAnimTracker*, CliTracker>::const_iterator prev = m_Trackers.find(pTracker - 1);
            if (prev != m_Trackers.end() && prev->second.hObj == (void*)hObj) n = prev->second.nIndex + 1;
            CliTracker t = { (void*)hObj, n };
            m_Trackers[pTracker] = t;
            return LT_OK;
        }
        LTRESULT RemoveTracker(HOBJECT, LTAnimTracker* pTracker) override
        {
            LTIMPL("ILTModel::RemoveTracker");
            m_Trackers.erase(pTracker);
            return LT_OK;
        }
        LTRESULT GetCurAnim(LTAnimTracker* pTracker, HMODELANIM& hAnim) override
        {
            LTIMPL("ILTModel::GetCurAnim");
            hAnim = 0;                          // written on every path
            std::map<LTAnimTracker*, CliTracker>::const_iterator it = m_Trackers.find(pTracker);
            if (it == m_Trackers.end()) return LT_ERROR;
            void* hServer = LTReplicate_ServerOf(it->second.hObj);
            int nAnim = 0;
            if (!hServer || !LTServerHost_GetTrackerAnim(hServer, it->second.nIndex, &nAnim))
                return LT_ERROR;
            hAnim = (HMODELANIM)nAnim;
            return LT_OK;
        }
        LTRESULT ResetAnim(LTAnimTracker* pTracker) override
        {
            LTIMPL("ILTModel::ResetAnim");
            return m_Trackers.count(pTracker) ? LT_OK : LT_ERROR;
        }

        LTRESULT GetSocket(HOBJECT hObj, char* pSocketName, HMODELSOCKET& hSocket) override
        {
            LTIMPL("ILTModel::GetSocket");
            int n = LTScene_FindSocket(hObj, pSocketName);
            if (n < 0) return LT_ERROR;
            hSocket = (HMODELSOCKET)n;
            return LT_OK;
        }
        LTRESULT GetSocketTransform(HOBJECT hObj, HMODELSOCKET hSocket,
                                    LTransform& transform, LTBOOL bWorldSpace) override
        {
            LTIMPL("ILTModel::GetSocketTransform");
            float p[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 };
            if (!LTScene_GetSocketTransform(hObj, (int)hSocket, bWorldSpace ? 1 : 0, p, q))
                return LT_ERROR;
            transform.m_Pos.Init(p[0], p[1], p[2]);
            transform.m_Rot.Init(q[0], q[1], q[2], q[3]);
            return LT_OK;
        }
    };
    HostModel                 m_model;
    // ⚠️ A real ILTTransform. LTransform is just { LTVector m_Pos; LTRotation
    // m_Rot; } (ltbasetypes.h:123), so these are pure field moves -- but the
    // game reads them into UNINITIALISED locals:
    //
    //     LTVector vPos; LTRotation rRot;
    //     g_pModelLT->GetSocketTransform(..., transform, LTTRUE);
    //     g_pTransLT->Get(transform, vPos, rRot);      // <- if this no-ops...
    //     SetObjectPos(pSFX->GetObject(), &vPos, LTTRUE);
    //
    // ...every attachment is placed at a garbage position and silently
    // disappears (BaseFolder.cpp:2203). The third instance of exactly this
    // shape in one session.
    class HostTransform : public LTStub_ILTTransform
    {
    public:
        LTRESULT Get(LTransform& t, LTVector& pos, LTRotation& rot) override
        {
            LTIMPL("ILTTransform::Get");
            pos = t.m_Pos; rot = t.m_Rot; return LT_OK;
        }
        LTRESULT Set(LTransform& t, LTVector& pos, LTRotation& rot) override
        {
            LTIMPL("ILTTransform::Set");
            t.m_Pos = pos; t.m_Rot = rot; return LT_OK;
        }
        LTRESULT GetPos(LTransform& t, LTVector& pos) override
        {
            LTIMPL("ILTTransform::GetPos");
            pos = t.m_Pos; return LT_OK;
        }
        LTRESULT GetRot(LTransform& t, LTRotation& rot) override
        {
            LTIMPL("ILTTransform::GetRot");
            rot = t.m_Rot; return LT_OK;
        }
    };
    HostTransform             m_transform;
    LTStub_ILTLightAnim       m_lightAnim;
public:
    // The host needs it to start the device and push the listener; the GAME
    // only ever sees it through ILTClient::m_pSoundMgr.
    CLTSoundMgr& GetSoundMgr() { return m_soundMgr; }

private:
    // ⚠️ THE REAL SOUND MANAGER, not the stub. It is inert until something
    // asks it to play, so installing it unconditionally costs a headless run
    // nothing -- and the frame contract's recorded run never touches it (zero
    // sound events in skeleton_client.record), which is why this can be the
    // default rather than another opt-in flag.
    CLTSoundMgr               m_soundMgr;
};

// ⚠️ WHICH ATTACHED EFFECTS THE CLIENT CAN SURVIVE. See the call site.
//
// The ids are SFXMsgIds.h's. The default list is the set that has been run
// headless on M01S01 and T01S01 without a fault; everything else is still
// dropped and still counted, which is where it was yesterday.
static bool LTHost_SFXAllowed(unsigned int nId)
{
    static bool  bInit = false;
    static bool  bAll  = false;
    static std::set<unsigned int> allow;

    if (!bInit)
    {
        bInit = true;
        const char* pEnv = getenv("LT_SFXALLOW");
        if (pEnv && *pEnv)
        {
            if (strcmp(pEnv, "all") == 0) { bAll = true; }
            else
            {
                const char* p = pEnv;
                while (*p)
                {
                    char* pEnd = 0;
                    unsigned long v = strtoul(p, &pEnd, 10);
                    if (pEnd == p) break;
                    allow.insert((unsigned int)v);
                    p = pEnd;
                    while (*p == ',' || *p == ' ') ++p;
                }
            }
        }
        else
        {
            // ⚠️ EACH ONE WAS TURNED ON ALONE AND RUN, on M01S01 and on
            // T01S01, headless, 600 updates. That is the whole basis for this
            // list -- not what looks harmless.
            allow.insert(13);   // SFX_CAMERA_ID       what cinematics need
            allow.insert(21);   // SFX_LIGHT_ID
            allow.insert(24);   // SFX_CHARACTER_ID    the people
            allow.insert(35);   // SFX_STEAM_ID
            allow.insert(31);   // SFX_LENSFLARE_ID
            allow.insert(11);   // SFX_VOLUMEBRUSH_ID
            allow.insert(27);   // SFX_WEATHER_ID

            // ⚠️ SFX_POLYGRID_ID (1) -- the water surfaces. It was absent
            // because it crashed: CPolyGridFX::CreateObject writes its palette
            // through GetPolyGridInfo's table pointer, and with the polygrid
            // API all stubs that was a stack-buffer-overflow at
            // PolyGridFX.cpp:174 (AddressSanitizer). The API is implemented
            // now (ltscene.cpp SC_*PolyGrid*, 2026-10-14); LT_NOPOLYGRID=1
            // gates it again.
            if (LTPolyGridEnabled()) allow.insert(1);
        }
    }

    return bAll || allow.find(nId) != allow.end();
}



// ⚠️ THE CLIENT'S END OF THE MESSAGE PIPE. The shell is created inside main,
// and the pipe needs it from a callback, so it is parked here rather than
// threaded through -- the same shape as g_pHostSound below and for the same
// reason.
static IClientShell* g_pHostClientShell = 0;

// LT_LIPTRACE=1 -- the client mirrors that received a CHARACTER effect
// (SFX_CHARACTER_ID 24) in this world, so a lip-sync message (MID_SFX_MESSAGE
// 231, SFX_CHARACTER_ID, object, CFX_NODECONTROL_LIP_SYNC ...) can be checked
// against them: does it name a mirror whose CharacterFX exists?
static std::set<void*> g_LipFX;
static void LTHost_ListenerStateForTrace(bool* pIn, float* pPos3, float* pQuat4);   // below g_pHostSound
static void LTHost_LipTraceReset() { g_LipFX.clear(); }

static bool LTHost_DeliverToClient(CLTMessage* pMsg)
{
    if (!pMsg || !g_pHostClientShell) return false;

    switch (pMsg->Dest())
    {
        case kLTMsg_ToClient:
        {
            static const bool s_bLip = (getenv("LT_LIPTRACE") != 0);
            if (s_bLip && pMsg->GetId() == 231 && pMsg->Size() >= 1 + sizeof(uintptr_t) + 1
                && pMsg->Bytes()[0] == 24)
            {
                uintptr_t v = 0;
                memcpy(&v, pMsg->Bytes() + 1, sizeof(v));
                void* hMirror = LTReplicate_Find((void*)v);
                const unsigned nCFX = pMsg->Bytes()[1 + sizeof(uintptr_t)];
                bool bIn = true; float lp[3] = { 0, 0, 0 }, lq[4] = { 0, 0, 0, 1 };
                LTHost_ListenerStateForTrace(&bIn, lp, lq);
                unsigned int nT = 0, nF = 0, nU = 0; float sp[3] = { 0, 0, 0 }, sq[4]; const char* pF = 0;
                const bool bPos = v && LTServerHost_GetObjectState((void*)v, &nT, sp, sq, &nF, &nU, &pF);
                const float dx = sp[0]-lp[0], dy = sp[1]-lp[1], dz = sp[2]-lp[2];
                printf("[LIP] t=%.2f character msg %u for server %p -> mirror %p %s | listener %s (%.0f %.0f %.0f), speaker %s %.0f units away\n",
                       LTServerHost_Time(), nCFX, (void*)v, hMirror,
                       !hMirror ? "NO MIRROR" : (g_LipFX.count(hMirror) ? "has CharacterFX"
                                                                         : "mirror has NO CharacterFX in this world"),
                       bIn ? "IN-CLIENT" : "at camera", lp[0], lp[1], lp[2],
                       bPos ? "" : "(no pos)", bPos ? sqrtf(dx*dx + dy*dy + dz*dz) : -1.0f);
                fflush(stdout);
            }
            g_pHostClientShell->OnMessage((uint8)pMsg->GetId(), (HMESSAGEREAD)pMsg);
            return true;
        }

        // ⚠️ AN INSTANT EFFECT HAS NO OBJECT BY CONSTRUCTION -- the game asked
        // for one at a POSITION -- so a null HLOCALOBJ here is the truth, and
        // CGameClientShell::SpecialEffectNotify is written for it (`if (hObj)`).
        case kLTMsg_SFXInstant:
            g_pHostClientShell->SpecialEffectNotify((HLOCALOBJ)0, (HMESSAGEREAD)pMsg);
            return true;

        // ⚠️ AN ATTACHED EFFECT NAMES A SERVER OBJECT, and the client's
        // HLOCALOBJ is a different thing entirely. This used to be DROPPED AND
        // COUNTED, with the census line standing in for the size of the
        // replication job -- six camera effects and twenty-four character
        // effects thrown away in M01S01's first ten seconds (HANDOVER §5n).
        //
        // Now the server object is MIRRORED on demand and the client is handed
        // its own handle. See ltreplicate.h for why on demand rather than
        // wholesale.
        //
        // ⚠️ STILL DROPPED when there is no mirror, and that is the honest
        // answer rather than a fallback: the client scene refuses a type it
        // cannot represent, and passing NULL would make the effect detach and
        // pile up at the origin -- which looks like replication working.
        case kLTMsg_SFX:
        {
            // ⚠️ GATED BY EFFECT TYPE, and this is not timidity. Replication
            // makes the client HANDLE honest; it does not make the client's FX
            // code work. The first run with the pipe open crashed in
            // CPolyGridFX::CreateObject -- a stack overflow, because the
            // polygrid engine calls it depends on (SetupPolyGrid, FitPolyGrid,
            // GetPolyGridInfo) are stubs returning zeros and the game sized a
            // buffer from them. Delivering an effect whose engine support does
            // not exist turns a counted drop into a crash, which is strictly
            // worse.
            //
            // ⚠️ So the list is what has been RUN, not what looks plausible.
            // LT_SFXALLOW=<ids, comma-separated> overrides it for widening the
            // list by measurement; LT_SFXALLOW=all opens the gate completely.
            if (!LTHost_SFXAllowed(pMsg->SFXId())) return false;

            void* hServer = LTServerHost_HandleOf(pMsg->m_pSFXObject);
            void* hClient = LTReplicate_Ensure(hServer);
            if (!hClient) return false;
            // LT_SFXTRACE=1 -- an attached effect delivered to a client object
            // that has already had one (LT2 delivers an object's effect once,
            // when the object appears on the client; HANDOVER 5bz).
            if (getenv("LT_SFXTRACE"))
            {
                static std::map<void*, int> s_n;
                const int n = ++s_n[hClient];
                if (n > 1)
                {
                    printf("[SFX] id %u arrives again (%d) for client %p of server %s '%s' -- dropped unless LT_SFXEVERYTIME\n",
                           (unsigned)pMsg->SFXId(), n, hClient, LTServerHost_ClassNameOf(hServer),
                           LTServerHost_ObjectNameOf(hServer));
                    fflush(stdout);
                }
            }
            // ⚠️ ONCE PER CLIENT OBJECT, as in LT2 (see LTSceneObject::
            // m_bSFXDelivered). The port delivered every call: the player's
            // character effect arrived twice at every level start (spawn,
            // then Respawn's CreateSpecialFX(TRUE), PlayerObj.cpp:2070) and
            // three M01S01 cameras twice -- two CharacterFX for one player.
            // The repeat only updates the server's stored message (captured
            // at EndMessage). LT_SFXEVERYTIME=1 = the old delivery.
            static const bool s_bEvery = []() {
                const bool b = (getenv("LT_SFXEVERYTIME") != 0);
                if (b) { printf("[SFX] LT_SFXEVERYTIME=1 -- every attached effect delivered, repeats included (old)\n"); fflush(stdout); }
                return b; }();
            if (!s_bEvery && LTScene_SFXDelivered(hClient)) return true;
            LTScene_SetSFXDelivered(hClient);
            if (pMsg->SFXId() == 24) g_LipFX.insert(hClient);
            if (pMsg->SFXId() == 24 && getenv("LT_REPTRACE"))
            {
                printf("[SFX] character effect for server %p -> client %p%s%s\n", hServer, hClient,
                       hServer == LTServerHost_PlayerHandle() ? " (the PLAYER)" : "",
                       hClient == LTSession_GetClientObject() ? " = the client object" : "");
                fflush(stdout);
            }
            g_pHostClientShell->SpecialEffectNotify((HLOCALOBJ)hClient,
                                                    (HMESSAGEREAD)pMsg);
            return true;
        }

        default:
            return false;
    }
}

// ----------------------------------------------------------------------- //
//
// The four hooks ltreplicate.h asks for. Each is two lines because the work is
// already done on either side -- what the replicator needed was not new
// machinery but a place where both halves are in scope, and this file is the
// only one where they are.
//
// ----------------------------------------------------------------------- //

namespace
{
    double       g_fFrameMs3D = 0.0;
    unsigned int g_nFrames3D  = 0;

    bool Rep_GetServerState(void* hServer, unsigned int* pType,
                            float* pPos3, float* pQuat4,
                            unsigned int* pFlags, unsigned int* pUserFlags,
                            const char** ppFile)
    {
        return LTServerHost_GetObjectState(hServer, pType, pPos3, pQuat4,
                                           pFlags, pUserFlags, ppFile);
    }

    void* Rep_CreateClient(unsigned int nType, const float* pPos3,
                           const float* pQuat4, unsigned int nFlags,
                           const char* pFile)
    {
        return LTScene_CreateMirror(nType, pPos3, pQuat4, nFlags, pFile);
    }

    // ⚠️ THE GAME IS TOLD BEFORE ITS OBJECT GOES -- the reference's
    // RemoveObjectFromClientWorld (clientmgr.cpp:2079) calls
    // IClientShell::OnObjectRemove for an object with CF_NOTIFYREMOVE, which
    // the game sets on every object it gives an effect
    // (GameClientShell.cpp:5793) and answers with m_sfxMgr.RemoveSpecialFX.
    // Never called here: a removed character's CharacterFX lived on to the
    // end of the level, and a new object at a reused handle found the OLD
    // effect first -- its crosshair class, its sounds. Suspected for the
    // M01S01 assassin that showed "don't shoot" (user report 2026-10-17).
    // LT_NOREMOVENOTIFY=1 = never called (old); LT_REMOVETRACE=1 lists calls.
    void Rep_DestroyClient(void* hClient)
    {
        static const bool s_bOff = []() {
            const bool b = (getenv("LT_NOREMOVENOTIFY") != 0);
            if (b) { printf("[HOST] LT_NOREMOVENOTIFY=1 -- OnObjectRemove is never called (old)\n"); fflush(stdout); }
            return b; }();
        if (!s_bOff && g_pHostClientShell && (LTScene_GetClientFlags(hClient) & 1u))   // CF_NOTIFYREMOVE
        {
            static const bool s_bTrace = (getenv("LT_REMOVETRACE") != 0);
            if (s_bTrace) { printf("[REMOVE] OnObjectRemove client %p\n", hClient); fflush(stdout); }
            g_pHostClientShell->OnObjectRemove((HLOCALOBJ)hClient);
        }
        LTScene_RemoveObject(hClient);
    }

    void Rep_MoveClient(void* hClient, const float* pPos3, const float* pQuat4)
    {
        if (pPos3)  LTScene_SetObjectPos(hClient, pPos3[0], pPos3[1], pPos3[2]);
        if (pQuat4) LTScene_SetObjectRotation(hClient, pQuat4);
    }

    void Rep_SetClientUserFlags(void* hClient, unsigned int nUserFlags)
    {
        LTScene_SetObjectUserFlags(hClient, nUserFlags);
    }

    void Rep_SetClientModel(void* hClient, const char* pFilename)
    {
        LTScene_SetObjectModel(hClient, pFilename);
    }

    void Rep_Install()
    {
        LTReplicateHooks h;
        h.GetServerState = Rep_GetServerState;
        h.CreateClient   = Rep_CreateClient;
        h.DestroyClient  = Rep_DestroyClient;
        h.MoveClient     = Rep_MoveClient;
        h.SetClientUserFlags = Rep_SetClientUserFlags;
        h.SetClientModel     = Rep_SetClientModel;
        LTReplicate_SetHooks(&h);
    }
}


// ⚠️ THE CLIENT HALF OF ENTERING A WORLD, and it is the engine's job, not the
// game's. The server has the level; this gives the CLIENT the three things it
// needs before it will stop drawing menus:
//
//   1. the world's geometry, in the shared scene (RenderCamera draws it)
//   2. a client-side player object, because
//      CInterfaceMgr::UpdateLoadingLevelState refuses to leave the loading
//      screen while GetClientObject() answers null (InterfaceMgr.cpp:1041)
//   3. PreLoadWorld then OnEnterWorld -- "a world is coming", then "you are in
//      it". The game uses the first to tear its menu down and the second to
//      set up the camera, the HUD and its input state.
//
// ⚠️ ORDER: the player object is made BEFORE OnEnterWorld, because
// OnEnterWorld is what starts the state machine that immediately asks for it.
// One frame later is still too late.
static void LTHost_ProbeEnterWorld(const char* pWorld);     // LT_PLAYPROBE, below

// ⚠️ THE CLIENT'S OPEN WORLD, and leaving it. See LTServerHost_SetWorldClosingHook.
static IClientShell* g_pClosingShell = 0;
static bool          g_bClientWorldOpen = false;
static bool LTHost_LateExitWorld()
{
    static const bool b = []() {
        const bool v = (getenv("LT_LATEEXITWORLD") != 0);
        if (v) { printf("[HOST] LT_LATEEXITWORLD=1 -- OnExitWorld when the client ENTERS the new world (5be timing)\n"); fflush(stdout); }
        return v; }();
    return b;
}
static void LTHost_WorldClosing()
{
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_NOEXITWORLD") != 0);
        if (b) { printf("[HOST] LT_NOEXITWORLD=1 -- OnExitWorld NOT called on a world change\n"); fflush(stdout); }
        return b; }();
    if (!g_bClientWorldOpen || !g_pClosingShell) return;
    g_bClientWorldOpen = false;
    if (s_bOff) return;
    printf("[HOST] leaving the previous world (OnExitWorld)\n");
    fflush(stdout);
    g_pClosingShell->OnExitWorld();
    // The game has removed every effect (CSFXMgr); objects that survive the
    // world change -- the player's -- take their effect again in the next.
    LTScene_ResetSFXDelivered();
}
// The server's hook: the early (donor) timing unless LT_LATEEXITWORLD.
static void LTHost_ServerWorldClosing()
{
    if (!LTHost_LateExitWorld()) LTHost_WorldClosing();
}

// The replicator's question: is hServer the local player? Then its client
// object is the session's own (made now if the client has not entered yet).
static void* LTHost_ReplicatePlayerAlias(void* hServer)
{
    if (!hServer || hServer != LTServerHost_PlayerHandle()) return 0;
    void* h = LTSession_GetClientObject();
    if (!h) { h = LTScene_CreatePlayerObject(); LTSession_SetClientObject(h); }
    return h;
}

// A client-written object handle -> the server object it stands for: the
// client's own object is the server's player; a mirror is its server object.
static void* LTHost_ClientToServerObject(void* hClient)
{
    if (!hClient) return 0;
    if (hClient == LTSession_GetClientObject()) return LTServerHost_PlayerHandle();
    return LTReplicate_ServerOf(hClient);
}

static void LTHost_ClientEnterWorld(IClientShell* pShell, const char* pWorld)
{
    if (!pShell || !pWorld || !*pWorld) return;
    g_pClosingShell = pShell;

    char szWorld[256];
    strncpy(szWorld, pWorld, sizeof(szWorld) - 1);
    szWorld[sizeof(szWorld) - 1] = '\0';

    printf("\nclient entering %s...\n", szWorld);
    fflush(stdout);

    if (!LTWorldScene_Load(LTGetFileMgr(), szWorld))
    {
        fprintf(stderr, "[HOST] the client could not load %s -- staying in the "
                        "menu rather than entering a world it cannot draw\n",
                szWorld);
        return;
    }

    if (!LTSession_GetClientObject())
        LTSession_SetClientObject(LTScene_CreatePlayerObject());
    LTSession_SetConnected(true);
    LTHost_ProbeEnterWorld(szWorld);

    // ⚠️ THE OLD WORLD IS LEFT FIRST -- OnExitWorld, which the engine never
    // called. The donor does it whenever an open world closes
    // (clientshell.cpp NotifyWorldClosing -> CClientMgr::OnExitWorld). The
    // game resets m_bStartedLevel there and ONLY there
    // (GameClientShell.cpp:1524), and OnEnterWorld sends MID_SINGLEPLAYER_
    // START -> CPlayerObj::StartLevel -> the GameStartPoint's TriggerMessage
    // only while it is false. So every level after the first never "started":
    // T01S03's elevator door (`msg arrive trigger` -> edoor1) stayed shut
    // after the lift from T01S02, while `play.sh game t01s03` opened it
    // (user report, 5be). LT_NOEXITWORLD=1 restores the old sequence (says so).
    //
    // ⚠️ AND IT IS LEFT WHEN THE SERVER STARTS THE NEW WORLD, not here. By the
    // time the client enters, the new level's camera and character effects
    // have been delivered, and OnExitWorld (m_sfxMgr.RemoveAll) wiped them --
    // silent cinematics after every level change (5bi). LTHost_WorldClosing
    // runs from the server's LoadWorld; this is only the fallback for a path
    // that did not go through it. LT_LATEEXITWORLD=1 = the 5be timing.
    {
        if (g_bClientWorldOpen) LTHost_WorldClosing();
        g_bClientWorldOpen = true;
    }

    pShell->PreLoadWorld(szWorld);
    pShell->OnEnterWorld();

    // ⚠️ MOUSE LOOK IS NOT TURNED ON HERE. It follows the game's own cursor
    // mode (CLTNullCursor::SetCursorMode): captured while the game wants no
    // pointer, released the moment it asks for one. Capturing unconditionally
    // made the dialogue window unclickable and the opening cutscene
    // unadvanceable.

    printf("client is in the world  (stubs so far: %d)\n", LTStub_Count());
    fflush(stdout);
}


// ⚠️ Walk mode asks for a sound by NAME and knows nothing about the sound
// manager -- see LTWalkSoundFn. This is the only place the two meet.
static CLTSoundMgr* g_pHostSound = 0;

// ⚠️ ILTClient::SetListener / GetListener -- BOTH WERE STUBS, AND GetListener
// WROTE NOTHING. CClientSoundMgr::PlaySound (ClientSoundMgr.cpp:172) drops any
// non-looping 3D sound farther from the listener than its radius, measured
// against GetListener's position -- an uninitialised LTVector here. So every
// lip-synced line the client plays in GAMEPLAY (3D, attached to the speaker)
// was thrown away before it reached the mixer, while cinematic lines, played
// CLIENTLOCAL, survived: "the conversation stops when I can move" (user,
// 5bd). CSubtitle::Draw reads it too. Every output is written on every path.
static void LTHost_SetListener(LTBOOL bInClient, LTVector* pPos, LTRotation* pRot)
{
    if (g_pHostSound) g_pHostSound->SetListener(bInClient, pPos, pRot, LTFALSE);
}

static void LTHost_ListenerStateForTrace(bool* pIn, float* pPos3, float* pQuat4)
{
    if (g_pHostSound) g_pHostSound->GetListenerState(pIn, pPos3, pQuat4);
}

static void LTHost_GetListener(LTBOOL* pInClient, LTVector* pPos, LTRotation* pRot)
{
    bool b = true;
    float p[3] = { 0.0f, 0.0f, 0.0f }, q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (g_pHostSound) g_pHostSound->GetListenerState(&b, p, q);
    if (pInClient) *pInClient = b ? LTTRUE : LTFALSE;
    if (pPos) pPos->Init(p[0], p[1], p[2]);
    if (pRot) { float* r = (float*)pRot; r[0] = q[0]; r[1] = q[1]; r[2] = q[2]; r[3] = q[3]; }
}

// ⚠️ The handle goes back through the CALLER'S struct, not a return value.
// PlaySoundInfo::m_hSound is an out-parameter that the game reads when it
// asked for PLAYSOUND_GETHANDLE, and a sound played without filling it is a
// sound nobody can stop.
static LTRESULT LTHost_ClientPlaySound(PlaySoundInfo* pInfo)
{
    if (!pInfo) return LT_ERROR;
    if (!g_pHostSound)
    {
        // Before the manager is attached -- the menu's first frames.
        pInfo->m_hSound = 0;
        return LT_ERROR;
    }
    return g_pHostSound->PlaySound(pInfo, pInfo->m_hSound);
}

// ⚠️ InitSound answers LT_OK WITHOUT a device, and that is the honest answer:
// the question is "can the sound system run", and it can -- it loads, decodes
// and tracks sounds either way. Whether a speaker is attached is a separate
// fact the game neither asks about nor needs.
static LTRESULT LTHost_SndInit(InitSoundInfo* pInfo)
{
    if (!g_pHostSound) return LT_ERROR;
    return g_pHostSound->InitSound(pInfo);
}

static void LTHost_SndKill(HLTSOUND h)
{
    if (g_pHostSound) g_pHostSound->KillSound(h);
}

static void LTHost_SndKillLoop(HLTSOUND h)
{
    if (g_pHostSound) g_pHostSound->KillSoundLoop(h);
}

static void LTHost_SndPause()
{
    if (g_pHostSound) g_pHostSound->PauseSounds();
}

static void LTHost_SndResume()
{
    if (g_pHostSound) g_pHostSound->ResumeSounds();
}

static void LTHost_SndSetVolume(unsigned short n)
{
    if (g_pHostSound) g_pHostSound->SetVolume(n);
}

static unsigned short LTHost_SndGetVolume()
{
    uint16 n = 0;
    if (g_pHostSound) g_pHostSound->GetVolume(n);
    return (unsigned short)n;
}

// ⚠️ THE SERVER'S END OF THE SAME THREE QUESTIONS. See LTServerHost_
// SetSoundQueries: the server has the dialogue's handle, the client has the
// mixer, and there is one pair of speakers.
static LTRESULT LTHost_SrvSndDone(HLTSOUND h, LTBOOL* pbDone)
{
    if (!pbDone) return LT_INVALIDPARAMS;
    if (!g_pHostSound) { *pbDone = LTTRUE; return LT_OK; }

    LTBOOL b = LTFALSE;
    const LTRESULT r = g_pHostSound->IsSoundDone(h, b);
    *pbDone = b;
    return r;
}

static LTRESULT LTHost_SrvSndDuration(HLTSOUND h, LTFLOAT* pf)
{
    if (!pf) return LT_INVALIDPARAMS;
    *pf = 0.0f;
    if (!g_pHostSound) return LT_ERROR;
    return g_pHostSound->GetSoundDuration(h, *pf);
}

static LTRESULT LTHost_SndDuration(HLTSOUND h, LTFLOAT* pf)
{
    if (!pf) return LT_ERROR;
    // ⚠️ Written on every path. The game seeds nothing and divides by it.
    *pf = 0.0f;
    if (!g_pHostSound) return LT_ERROR;
    return g_pHostSound->GetSoundDuration(h, *pf);
}

static LTRESULT LTHost_SndGetData(HLTSOUND h, int16*& p16, int8*& p8,
                                  uint32* pRate, uint32* pChannels)
{
    LTIMPL("ILTClient::GetSoundData");
    p16 = 0; p8 = 0;
    if (pRate) *pRate = 0;
    if (pChannels) *pChannels = 0;
    if (!g_pHostSound) return LT_ERROR;
    short* q16 = 0; signed char* q8 = 0;
    unsigned int nRate = 0, nCh = 0;
    const LTRESULT r = g_pHostSound->GetSoundData(h, q16, q8, &nRate, &nCh);
    p16 = (int16*)q16; p8 = (int8*)q8;
    if (pRate) *pRate = nRate;
    if (pChannels) *pChannels = nCh;
    return r;
}

static LTRESULT LTHost_SndGetOffset(HLTSOUND h, uint32* pOffset, uint32* pSize)
{
    LTIMPL("ILTClient::GetSoundOffset");
    if (pOffset) *pOffset = 0;
    if (pSize) *pSize = 0;
    if (!g_pHostSound) return LT_ERROR;
    unsigned int nO = 0, nS = 0;
    const LTRESULT r = g_pHostSound->GetSoundOffset(h, &nO, &nS);
    if (pOffset) *pOffset = nO;
    if (pSize) *pSize = nS;
    return r;
}

static void WalkPlaySound(const char* pName, const float* pPos3,
                          float fInner, float fOuter)
{
    if (!g_pHostSound || !pName || !*pName) return;

    PlaySoundInfo si;
    memset(&si, 0, sizeof(si));
    snprintf(si.m_szSoundName, sizeof(si.m_szSoundName), "%s", pName);
    si.m_dwFlags      = pPos3 ? PLAYSOUND_3D : PLAYSOUND_LOCAL;
    si.m_fInnerRadius = fInner;
    si.m_fOuterRadius = fOuter;
    si.m_nVolume      = 100;
    si.m_fPitchShift  = 1.0f;
    if (pPos3) for (int k = 0; k < 3; ++k) ((float*)&si.m_vPosition)[k] = pPos3[k];

    HLTSOUND h = 0;
    si.m_hSound = 0;
    LTRESULT r = g_pHostSound->PlaySound(&si, h);
    if (r != LT_OK)
        printf("[SOUND] '%s' did not play (%d)\n", pName, (int)r);
}



// ----------------------------------------------------------------------- //
// LT_CINESHOT -- frames of a cinematic, from the cinematic's own camera.
//
// See the call site for why this is an observer and not a render path.
// ----------------------------------------------------------------------- //

static const char* LTCineShot_Dir()
{
    static const char* p = getenv("LT_CINESHOT");
    return (p && *p) ? p : 0;
}

namespace
{
    int   g_nCineShot   = 0;        // frames written
    int   g_nCineFrame  = 0;        // live-camera frames seen
    bool  g_bCineWasOn  = false;

    // ⚠️ EVERY Nth LIVE FRAME, not every Nth frame of the run. A cinematic is
    // a small slice of a long run, and sampling the run would spend the whole
    // budget on the part where nothing is happening.
    int LTCineShot_Every()
    {
        static const int n = getenv("LT_CINEEVERY")
                           ? atoi(getenv("LT_CINEEVERY")) : 60;
        return n > 0 ? n : 60;
    }
}

static void LTCineShot_Idle()
{
    if (g_bCineWasOn)
    {
        printf("[CINE] camera went dark at t=%.3f after %d frame(s), %d shot(s)\n",
               LTServerHost_Time(), g_nCineFrame, g_nCineShot);
        fflush(stdout);
        g_bCineWasOn = false;
    }
}

static void LTCineShot_Frame(const float* pPos3, const float* pQuat4, float fTime)
{
    if (!g_bCineWasOn)
    {
        printf("[CINE] camera live at t=%.3f\n", fTime);
        fflush(stdout);
        g_bCineWasOn = true;
    }

    const int nThis = g_nCineFrame++;
    if (nThis % LTCineShot_Every()) return;

    CLTWorldScene* pScene = LTGetWorldScene();
    if (!pScene) return;

    const int W = 640, H = 480;
    static std::vector<unsigned char> colour;
    static std::vector<float>         depth;
    colour.assign((size_t)W * H * 4, 0);
    depth.assign((size_t)W * H, 0.0f);

    // ⚠️ The camera's FORWARD from its quaternion. NOLF's world is left-handed
    // with +Z forward, so the look vector is the quaternion applied to
    // (0,0,1) -- not a Euler guess, which is what put the first version of
    // this inside a wall.
    const float x = pQuat4[0], y = pQuat4[1], z = pQuat4[2], w = pQuat4[3];
    const float fwd[3] = {
        2.0f * (x*z + w*y),
        2.0f * (y*z - w*x),
        1.0f - 2.0f * (x*x + y*y)
    };

    const float at[3] = { pPos3[0] + fwd[0] * 100.0f,
                          pPos3[1] + fwd[1] * 100.0f,
                          pPos3[2] + fwd[2] * 100.0f };

    pScene->Render(&colour[0], &depth[0], W, H, pPos3, at, 1.4f);

    char szPath[1024];
    snprintf(szPath, sizeof szPath, "%s/cine_%03d_t%06.2f.ppm",
             LTCineShot_Dir(), g_nCineShot, fTime);

    if (FILE* pF = fopen(szPath, "wb"))
    {
        fprintf(pF, "P6\n%d %d\n255\n", W, H);
        for (size_t i = 0; i < (size_t)W * H; ++i)
            fwrite(&colour[i * 4], 1, 3, pF);
        fclose(pF);
        printf("[CINE] t=%7.3f  eye (%.0f %.0f %.0f)  -> %s\n",
               fTime, pPos3[0], pPos3[1], pPos3[2], szPath);
        fflush(stdout);
        ++g_nCineShot;
    }
}


// ⚠️ Walk mode needs HELD keys; the queue only reports transitions. This
// observes the same events the shell receives and never consumes them.
static void WalkInputObserver(const LTInputEvent& ev)
{
    if (ev.m_Kind == LTInputEvent::kKeyDown) LTWorldWalk_OnKey(ev.m_Code, true);
    else if (ev.m_Kind == LTInputEvent::kKeyUp) LTWorldWalk_OnKey(ev.m_Code, false);
    else if (ev.m_Kind == LTInputEvent::kMouseMove) LTWorldWalk_OnMouse(ev.m_DX, ev.m_DY);
}


// ⚠️ WHICH RENDERER. Metal is the default whenever there is a window;
// headless runs keep the software rasteriser (the frame contract's reference)
// unless LT_METAL=1. LT_SOFTWARE=1 or LT_METAL=0 forces software.
// ⚠️ LT_AIMAT="<class or name substring>[,from-frame]" -- AIM LIKE A PLAYER.
// A headless run fires wherever the level left the camera, so "does a hit
// kill?" could not be asked. This turns the view toward the nearest live
// character matching the text, THROUGH THE MOUSE: the angle error goes in as
// axis offsets (radians, GameClientShell.cpp:2633), so the game's own camera,
// zoom and fire direction do the rest. Half the error per frame.
static void LTHost_AimTick(int nFrame)
{
    static const char* pAim = getenv("LT_AIMAT");
    if (!pAim || !*pAim) return;
    // ⚠️ A SEQUENCE: "a,2400;b,2800" aims at a from 2400 and at b from 2800
    // -- the latest entry whose frame has come is the one in force. M01S01's
    // assassins arrive one after another; a player switches targets.
    static std::vector<std::pair<std::string, int> > s_Aims;
    static bool bParsed = false;
    if (!bParsed)
    {
        bParsed = true;
        std::string all(pAim);
        size_t from = 0;
        while (from <= all.size())
        {
            size_t end = all.find(';', from);
            if (end == std::string::npos) end = all.size();
            std::string one = all.substr(from, end - from);
            from = end + 1;
            if (one.empty()) continue;
            int n = 0;
            const size_t c = one.find(',');
            if (c != std::string::npos) { n = atoi(one.c_str() + c + 1); one = one.substr(0, c); }
            s_Aims.push_back(std::make_pair(one, n));
            fprintf(stderr, "[AIM] aiming at '%s' from frame %d\n", one.c_str(), n);
        }
    }
    const std::string* pSub = 0;
    for (size_t i = 0; i < s_Aims.size(); ++i)
        if (nFrame >= s_Aims[i].second) pSub = &s_Aims[i].first;
    if (!pSub) return;
    const std::string& sSub = *pSub;

    float cp[3], cq[4];
    if (!LTScene_GetLastCamera(cp, cq)) return;
    float tp[3]; std::string sName;
    // "@x y z" (or "@x:y:z") aims at a fixed POINT -- a window, a switch --
    // instead of a character. ',' separates the frame, so the point uses ':'.
    if (!sSub.empty() && sSub[0] == '@')
    {
        if (sscanf(sSub.c_str() + 1, "%f:%f:%f", &tp[0], &tp[1], &tp[2]) != 3) return;
        sName = sSub;
    }
    else if (!LTServerHost_FindCharacter(sSub.c_str(), cp, tp, &sName)) return;

    float M[16];
    LT3D::FromQuatPos(cq, cp, M);
    const float f[3] = { M[2], M[6], M[10] };
    const float d[3] = { tp[0]-cp[0], tp[1]-cp[1], tp[2]-cp[2] };
    const float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dl < 1.0f) return;
    // LithTech: forward = (sin yaw, -sin pitch, cos yaw) for small pitch.
    const float yawNow = atan2f(f[0], f[2]), yawWant = atan2f(d[0], d[2]);
    const float pitchNow = -asinf(f[1] < -1 ? -1 : (f[1] > 1 ? 1 : f[1]));
    const float pitchWant = -asinf(d[1] / dl);
    float dy = yawWant - yawNow;
    while (dy >  3.14159265f) dy -= 6.2831853f;
    while (dy < -3.14159265f) dy += 6.2831853f;
    const float dp = pitchWant - pitchNow;
    LTInput_AddAxisRadians(dy * 0.5f, dp * 0.5f);

    static int s_n = 0;
    if ((s_n++ % 60) == 0)
        fprintf(stderr, "[AIM] frame %d: %s at %.0f units, error yaw %.2f pitch %.2f deg\n",
                nFrame, sName.c_str(), dl, dy * 57.2958f, dp * 57.2958f);
}


// ⚠️ LT_CONSOLE_AT="<frame>:<console line>[;<frame>:<line>...]" -- TYPE IN
// THE CONSOLE MID-GAME, as a player can. LT_CONSOLE runs once at start-up,
// before any level; this runs a line at a chosen frame through the same
// console. Its use: `ExitLevel` -- a program the game registers
// unconditionally (GameClientShell.cpp:1006), so a Windows player can type it
// too -- moves to the mission's next level WITH the loadout, which the
// `Record` path cannot give.
// LT_GODAT=<frame> -- the game's own god-mode cheat at that frame, exactly
// what the play probe sends (MID_PLAYER_CHEAT, CHEAT_GOD) -- for scripted
// fights the harness should survive (HANDOVER 5cz).
static void LTHost_GodTick(int nFrame)
{
    static const int s_nAt = getenv("LT_GODAT") ? atoi(getenv("LT_GODAT")) : -1;
    if (s_nAt < 0 || nFrame != s_nAt) return;
    if (CLTMessage* pCheat = LTMessage_StartToServer(LTServerHost_ClientHandle(), 107))
    {
        pCheat->PutByte(0);     // CHEAT_GOD (CheatDefs.h)
        pCheat->PutByte(1);
        LTMessage_End(pCheat, MESSAGE_GUARANTEED);
        printf("[GOD] frame %d: god mode toggled (MID_PLAYER_CHEAT, CHEAT_GOD)\n", nFrame);
        fflush(stdout);
    }
}

static void LTHost_ConsoleTick(int nFrame)
{
    static const char* pAll = getenv("LT_CONSOLE_AT");
    if (!pAll || !*pAll) return;
    static std::vector<std::pair<int, std::string> > s_Lines;
    static bool bParsed = false;
    if (!bParsed)
    {
        bParsed = true;
        std::string all(pAll);
        size_t from = 0;
        while (from <= all.size())
        {
            size_t end = all.find(';', from);
            if (end == std::string::npos) end = all.size();
            std::string one = all.substr(from, end - from);
            from = end + 1;
            const size_t c = one.find(':');
            if (c == std::string::npos) continue;
            s_Lines.push_back(std::make_pair(atoi(one.c_str()), one.substr(c + 1)));
        }
    }
    for (size_t i = 0; i < s_Lines.size(); ++i)
        if (s_Lines[i].first == nFrame)
        {
            printf("[CONSOLE] frame %d: %s\n", nFrame, s_Lines[i].second.c_str());
            fflush(stdout);
            LTGetConsole()->RunString(s_Lines[i].second.c_str());
        }
}


// ⚠️ LT_PLAYPROBE=1 -- PLAY EVERY LEVEL OF A MISSION THE WAY A PLAYER WOULD,
// AND SAY WHAT HAPPENED. One line per level, `[PROBE] LEVEL ...`.
//
// A fixed-frame key script cannot do this for 65 levels: intros differ in
// length, some ask a dialogue question, and a level reached by ExitLevel
// starts whenever the previous one finished loading. So this reacts:
//
//   INTRO   while a cinematic camera is live (USRFLG_CAMERA_LIVE, the bit the
//           game itself reads), a centre click every 4 s answers a dialogue
//           choice -- a key cannot (5am). Past LT_PLAYPROBE_SKIP frames
//           (default 7200 = 120 s) Space, the game's own skip
//           (GameClientShell.cpp:4385), every 5 s, and the line says so.
//   CONTROL 1.5 s with no live camera, at least 3 s into the level.
//   WALK    Forward held 2 s; the PLAYER OBJECT's displacement is measured.
//           Under 32 units (facing a wall) -> turn 90 degrees through the mouse
//           path and try again, up to four directions.
//   WATCH   LT_PLAYPROBE_WATCH frames (default 1200 = 20 s) idle: AI alive /
//           moved, player deaths (MID_PLAYER_STATE_CHANGE). One frame is
//           written to LT_PLAYPROBE_DIR/<world>.ppm when that is set.
//   EXIT    the game's own `ExitLevel` -> the mission's next level, with the
//           loadout. 30 s with no new level ends the run (the mission is over).
//   GOD     the game's own god-mode cheat, in EVERY level, the moment the
//           probe has control: what typing it in the message box sends
//           (CCheatMgr::SendCheatMessage -> MID_PLAYER_CHEAT, CHEAT_GOD).
//           ⚠️ It is a TOGGLE, and CPlayerObj::Respawn switches it OFF
//           (PlayerObj.cpp:1928) -- at every level start AND when the
//           handshake completes, after the enter. Sent on entering, the
//           handshake's respawn undid it (measured: M08S02 died exactly as
//           mortal). At control every respawn is over and it is off, so one
//           toggle turns it on. An idle probe is a target; since the carried
//           player updates (5bz) it was shot dead in M01S03/M08S02/M13S02 and
//           the chain after it was lost. LT_PLAYPROBE_MORTAL=1 = no cheat.
//
// It drives input only through the paths a player has (keys, click, mouse,
// commands, console) and reads only engine state.
namespace
{
    struct PlayProbe
    {
        std::string sWorld;
        int   nState = 0;             // 0 menus, 1 intro, 2 walk, 3 watch, 4 exited
        int   nEnter = 0, nLastCam = 0, nCamFrames = 0, nClicks = 0, nSkips = 0;
        int   nControl = -1, nDir = 0, nWalkFrom = 0, nWatchFrom = 0, nExitAt = 0;
        float p0[3] = { 0, 0, 0 };
        float fBest = 0.0f;
        int   nAlive = 0, nMoved = 0;
        unsigned int nDeaths0 = 0;
        int   nLevels = 0;
        int   nLedges = 0;            // turns away from an edge before walking
        bool  bEdgeStop = false;      // stopped mid-walk at an edge
    };
    PlayProbe   g_Probe;
    bool        g_bProbeEnter = false;
    std::string g_sProbeNext;
    // ⚠️ THE FRAME IS WRITTEN AFTER IT IS DRAWN, by the LT_DUMP_EVERY code
    // further down the loop. The probe ticks right after ClearScreenForFrame,
    // so a dump from inside the tick is a cleared, all-black buffer.
    std::string g_sProbeDump;

    void ProbeKey(int vk)
    {
        LTInputEvent ev; memset(&ev, 0, sizeof(ev));
        ev.m_Kind = LTInputEvent::kKeyDown; ev.m_Code = vk; LTInput_Push(ev);
        ev.m_Kind = LTInputEvent::kKeyUp;                   LTInput_Push(ev);
    }
    void ProbeClick()
    {
        LTInputEvent ev; memset(&ev, 0, sizeof(ev));
        ev.m_Kind = LTInputEvent::kMouseDown; ev.m_Button = 0; ev.m_X = 320; ev.m_Y = 240;
        LTInput_Push(ev);
        ev.m_Kind = LTInputEvent::kMouseUp; LTInput_Push(ev);
    }
    bool ProbePlayerPos(float* p)
    {
        float q[4];
        return LTServerHost_GetPlayerTransform(p, q);
    }
    // ⚠️ A PLAYER DOES NOT WALK OFF A LEDGE. M13S01 starts Cate on a catwalk
    // facing its open edge; walking blindly forward was a 340-unit fall and
    // the game's own formula (CMoveMgr FallDamageMin/MaxHeight 300/500) made
    // it lethal -- a probe death, not a game one, and it left the next levels
    // of the chain with a dead player (HANDOVER 5bm). True when, fAhead units
    // along the view's horizontal forward, there is no floor within a safe
    // drop (250, under the 300 at which fall damage starts) of the feet.
    bool ProbeLedgeAhead(float fAhead)
    {
        float p[3], q[4], cp[3], cq[4];
        if (!LTServerHost_GetPlayerTransform(p, q) || !LTScene_GetLastCamera(cp, cq)) return false;
        if (!LTWorldCollide_Ready() || !LTGetWorldCollide()) return false;
        float M[16];
        LT3D::FromQuatPos(cq, cp, M);
        float f[2] = { M[2], M[10] };
        const float l = sqrtf(f[0]*f[0] + f[1]*f[1]);
        if (l < 1e-3f) return false;
        f[0] /= l; f[1] /= l;
        const float from[3] = { p[0] + f[0]*fAhead, p[1], p[2] + f[1]*fAhead };
        const float to[3]   = { from[0], p[1] - 53.0f - 250.0f, from[2] };
        float hit[3], n[3], t = 0.0f;
        return !LTGetWorldCollide()->RaySegment(from, to, hit, n, &t);
    }

    float ProbeDist(const float* a, const float* b)
    {
        const float dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
        return sqrtf(dx*dx + dy*dy + dz*dz);
    }
    void ProbeReport(int nFrame, const char* pHow)
    {
        PlayProbe& P = g_Probe;
        unsigned int nDeaths = 0; LTMessage_PlayerState(0, &nDeaths);
        const unsigned int nDied = nDeaths - P.nDeaths0;
        std::string sVerdict;
        if (P.nControl < 0)                    sVerdict += " NO-CONTROL";
        else if (P.fBest < 32.0f)              sVerdict += " COULD-NOT-WALK";
        if (nDied)                             sVerdict += " PLAYER-DIED";
        if (P.nSkips)                          sVerdict += " INTRO-SKIPPED";
        if (sVerdict.empty())                  sVerdict = " PLAYABLE";
        char szCtl[32];
        if (P.nControl >= 0) snprintf(szCtl, sizeof(szCtl), "%.1fs", (P.nControl - P.nEnter) / 60.0f);
        else                 snprintf(szCtl, sizeof(szCtl), "never");
        printf("[PROBE] LEVEL %-22s | control after %s (camera %.1fs, %d click(s), %d skip(s)) "
               "| walked %.0f units (%d turn(s)) | AI %d alive, %d moved | died %u | %s |%s\n",
               P.sWorld.c_str(), szCtl, P.nCamFrames / 60.0f, P.nClicks, P.nSkips,
               P.fBest, P.nDir, P.nAlive, P.nMoved, nDied, pHow, sVerdict.c_str());
        fflush(stdout);
        (void)nFrame;
    }
}

static void LTHost_ProbeEnterWorld(const char* pWorld)
{
    LTHost_LipTraceReset();
    static const bool s_bMover = (getenv("LT_MOVERTRACE") != 0);
    if (s_bMover) g_nMoverTrace = 200;
    g_sProbeNext = pWorld ? pWorld : "";
    g_bProbeEnter = true;
}

// Returns true when the run should end (the mission's chain is over).
static bool LTHost_PlayProbeTick(int nFrame)
{
    static const bool s_bOn = (getenv("LT_PLAYPROBE") != 0);
    if (!s_bOn) return false;
    static const int kWatch  = getenv("LT_PLAYPROBE_WATCH") ? atoi(getenv("LT_PLAYPROBE_WATCH")) : 1200;
    static const int kSkip   = getenv("LT_PLAYPROBE_SKIP")  ? atoi(getenv("LT_PLAYPROBE_SKIP"))  : 7200;
    static const int kGiveUp = kSkip + 7200;
    const int kForward = 0;                    // COMMAND_ID_FORWARD (CommandIDs.h)
    PlayProbe& P = g_Probe;

    if (g_bProbeEnter)
    {
        g_bProbeEnter = false;
        if (P.nState >= 1 && P.nState <= 3)
        {
            LTCommand_Set(kForward, false);
            ProbeReport(nFrame, "the game moved on by itself");
        }
        const int nLevels = P.nLevels + 1;
        P = PlayProbe();
        P.nLevels = nLevels;
        P.sWorld = g_sProbeNext;
        P.nState = 1; P.nEnter = nFrame; P.nLastCam = nFrame;
        LTMessage_PlayerState(0, &P.nDeaths0);
        LTServerHost_ProbeCharacters(true, 0, 0);
        printf("[PROBE] enter %s at frame %d\n", P.sWorld.c_str(), nFrame);
        fflush(stdout);

    }

    const int dt = nFrame - P.nEnter;

    // LT_PLAYPROBE_POS=1 -- the player's position once a second while the
    // probe has control (walk, watch): falls, lifts and teleports, measured.
    static const bool s_bPos = (getenv("LT_PLAYPROBE_POS") != 0);
    if (s_bPos && (P.nState == 2 || P.nState == 3) && (dt % 60) == 0)
    {
        float pp[3];
        if (ProbePlayerPos(pp))
        { printf("[PROBE] pos t=%.1f (%.1f %.1f %.1f)\n", dt / 60.0f, pp[0], pp[1], pp[2]); fflush(stdout); }
    }

    switch (P.nState)
    {
    case 0:
        if (nFrame > 12000 && P.nLevels == 0)
        { printf("[PROBE] no level reached in 200 s -- the menus did not start one\n"); fflush(stdout); return true; }
        return false;

    case 1:
    {
        const bool bCam = LTServerHost_GetLiveCameraTransform(0, 0);
        if (bCam) { P.nLastCam = nFrame; ++P.nCamFrames; }
        if (bCam && dt % 240 == 120) { ProbeClick(); ++P.nClicks; }
        // ⚠️ AND RETURN, which is how a player answers a dialogue DECISION
        // without aiming: CLTDecisionWnd::OnKeyDown (LTDecisionWnd.cpp:483)
        // selects the highlighted choice -- the first one by default. A centre
        // click only hits a choice laid out under it: M01S01's is, M03S01's is
        // not, and that intro waited 480 s for an answer (sweep 2, HANDOVER 5bi).
        if (bCam && dt % 240 == 0 && dt > 0) { ProbeKey(13); ++P.nClicks; }
        if (bCam && dt > kSkip && dt % 300 == 0) { ProbeKey(32); ++P.nSkips; }
        if (!bCam && nFrame - P.nLastCam >= 90 && dt >= 180 && ProbePlayerPos(P.p0))
        {
            P.nControl = nFrame; P.nState = 2; P.nWalkFrom = nFrame;
            static const bool s_bMortal = (getenv("LT_PLAYPROBE_MORTAL") != 0);
            if (s_bMortal)
                printf("[PROBE] LT_PLAYPROBE_MORTAL=1 -- no god mode, the probe can die\n");
            else if (CLTMessage* pCheat = LTMessage_StartToServer(LTServerHost_ClientHandle(), 107))
            {
                // MID_PLAYER_CHEAT (MsgIDs.h:25): cheat code, data byte.
                pCheat->PutByte(0);     // CHEAT_GOD (CheatDefs.h)
                pCheat->PutByte(1);
                LTMessage_End(pCheat, MESSAGE_GUARANTEED);
                printf("[PROBE] god mode ON -- the game's own cheat (MID_PLAYER_CHEAT, CHEAT_GOD), "
                       "at control in every level; LT_PLAYPROBE_MORTAL=1 to play mortal\n");
            }
            fflush(stdout);
        }
        else if (dt > kGiveUp)
        {
            LTServerHost_ProbeCharacters(false, &P.nAlive, &P.nMoved);
            ProbeReport(nFrame, "exited by ExitLevel");
            LTGetConsole()->RunString("ExitLevel");
            P.nState = 4; P.nExitAt = nFrame;
        }
        return false;
    }

    case 2:
        if (nFrame == P.nWalkFrom)
        {
            // Look before walking: an edge ahead is a turn, not a step.
            if (ProbeLedgeAhead(96.0f))
            {
                if (++P.nLedges >= 4)
                {
                    printf("[PROBE] edges on every side -- not walking\n"); fflush(stdout);
                    P.nState = 3; P.nWatchFrom = nFrame;
                    return false;
                }
                printf("[PROBE] edge ahead -- turning instead of walking (%d)\n", P.nLedges);
                fflush(stdout);
                LTInput_AddAxisRadians(1.5707963f, 0.0f);
                P.nWalkFrom = nFrame + 15;
                return false;
            }
            P.bEdgeStop = false;
            LTCommand_Set(kForward, true);
        }
        if (nFrame > P.nWalkFrom && nFrame < P.nWalkFrom + 120 && !P.bEdgeStop
            && ProbeLedgeAhead(40.0f))
        {
            LTCommand_Set(kForward, false);
            P.bEdgeStop = true;
            printf("[PROBE] stopped at an edge\n"); fflush(stdout);
        }
        if (nFrame == P.nWalkFrom + 120)
        {
            LTCommand_Set(kForward, false);
            float p[3];
            if (ProbePlayerPos(p)) { const float d = ProbeDist(p, P.p0); if (d > P.fBest) P.fBest = d; }
            if (P.fBest >= 32.0f || P.nDir >= 3)
            { P.nState = 3; P.nWatchFrom = nFrame; }
            else
            {
                ++P.nDir;
                LTInput_AddAxisRadians(1.5707963f, 0.0f);
                if (ProbePlayerPos(p)) for (int k = 0; k < 3; ++k) P.p0[k] = p[k];
                P.nWalkFrom = nFrame + 15;      // let the turn land first
            }
        }
        return false;

    case 3:
        if ((nFrame - P.nWatchFrom) % 60 == 0)
            LTServerHost_ProbeCharacters(false, &P.nAlive, &P.nMoved);
        if (nFrame - P.nWatchFrom == 60)
            if (const char* pDir = getenv("LT_PLAYPROBE_DIR"))
            {
                std::string s = P.sWorld;
                for (size_t k = 0; k < s.size(); ++k) if (s[k] == '\\' || s[k] == '/') s[k] = '_';
                char szPath[512];
                snprintf(szPath, sizeof(szPath), "%s/%02d_%s.ppm", pDir, P.nLevels, s.c_str());
                g_sProbeDump = szPath;
            }
        if (nFrame - P.nWatchFrom >= kWatch)
        {
            ProbeReport(nFrame, "exited by ExitLevel");
            printf("[CONSOLE] frame %d: ExitLevel (LT_PLAYPROBE)\n", nFrame); fflush(stdout);
            LTGetConsole()->RunString("ExitLevel");
            P.nState = 4; P.nExitAt = nFrame;
        }
        return false;

    case 4:
        if (nFrame - P.nExitAt > 1800)
        {
            printf("[PROBE] chain ends after %s: no further level within 30 s\n", P.sWorld.c_str());
            fflush(stdout);
            return true;
        }
        return false;
    }
    return false;
}


// LT_PYTRACE=1 -- the server player's height every 5 frames, and the client
// camera's: a jump, a crouch and a fall are numbers, not impressions.
static void LTHost_PlayerYTick(int nFrame)
{
    static const bool s_bOn = (getenv("LT_PYTRACE") != 0);
    if (!s_bOn || (nFrame % 5) != 0) return;
    float p[3], q[4], cp[3], cq[4];
    if (!LTServerHost_GetPlayerTransform(p, q)) return;
    const bool bCam = LTScene_GetLastCamera(cp, cq);
    printf("[PY] frame %d player y %.1f  camera y %.1f  (x %.1f z %.1f)\n", nFrame, p[1],
           bCam ? cp[1] : -1.0f, p[0], p[2]);
    fflush(stdout);
}


static bool LTHost_WantMetal(bool bWindow)
{
    if (getenv("LT_SOFTWARE")) return false;
    if (const char* p = getenv("LT_METAL")) return p[0] && p[0] != '0';
    return bWindow;
}


int main(int argc, char** argv)
{
    bool bClient = true;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--server")) bClient = false;

    printf("=== NOLF 1 engine skeleton host ===\n");

    // ⚠️ FIRST, before anything that could produce an event. A record that
    // starts halfway through a run is a contract with a hole in it.
    LTRecord_Init();
    LTHost_InstallCrashHandlers();
    LTGetClock()->Init();
    LTInput_InitScript();
    LTCommand_InitScript();
    LTRecord_Event("host", "mode %s", bClient ? "client" : "server");

    // ------------------------------------------------------------------ //
    // Bring up the file system BEFORE the game exists. CreateClientShell runs
    // constructors that reach the engine, so a tree stack that appeared later
    // would be a different run from the one the census describes.
    // ------------------------------------------------------------------ //
    {
        const char* pEnv = getenv("NOLF_GAME_DIR");
        char dirs[2048];
        strncpy(dirs, pEnv ? pEnv : NOLF_DEFAULT_GAME_DIRS, sizeof(dirs) - 1);
        dirs[sizeof(dirs) - 1] = 0;

        const char* aDirs[16];
        int nDirs = 0;
        for (char* p = strtok(dirs, ":"); p && nDirs < 16; p = strtok(0, ":"))
            aDirs[nDirs++] = p;

        printf("mounting game data (%d director%s)...\n",
               nDirs, nDirs == 1 ? "y" : "ies");

        int nTrees = LTGetFileMgr()->InitFromGameDirs(aDirs, nDirs);
        printf("mounted %d tree(s)\n", nTrees);
        LTRecord_Event("host", "trees %d", nTrees);

        // ⚠️ THE PLAYER'S OWN CONTROLS, and they are DATA. autoexec.cfg carries
        // `AddAction Forward 0` and `rangebind "##keyboard" "##17" ...
        // "Forward"`, so running it is what gives the game a keyboard. Before
        // this, IsCommandOn had nothing to answer from and the player could
        // not be driven by a person at all.
        //
        // ⚠️ AFTER the trees are mounted, because the config is read THROUGH
        // them -- and read-only: the file manager has no write path, which is
        // what keeps the install untouched by construction.
        if (nTrees > 0)
        {
            LTBind_Register();
            const int nCfg = LTBind_LoadConfig("autoexec.cfg");
            // The player's own settings on top (ltbindings.h, LTConfig_*):
            // ~/Library/Application Support/NOLF1/autoexec.cfg. Only once
            // the install's config was read may it ever be written back.
            if (nCfg > 0)
            {
                LTConfig_SetCanSave(true);
                LTConfig_LoadUser("autoexec.cfg");
            }
            if (getenv("LT_KEYCHECK")) LTBind_CheckMacKeys();
            printf("bindings: %d config line(s), %u action(s), %u key(s)\n",
                   nCfg, LTBind_ActionCount(), LTBind_BindingCount());

            // ⚠️ `ModelAdd` -- the constant light every model gets. The
            // shipped config says "50 50 50"; whatever the config says wins,
            // because the model light is the engine's and the engine reads it
            // from here. See CLTWorldScene::BuildModelLights.
            if (LTConsoleVar* pMA = LTGetConsole()->Find("ModelAdd"))
            {
                float rgb[3] = { 50.0f, 50.0f, 50.0f };
                const char* pStr = LTGetConsole()->GetString(pMA);
                if (pStr && sscanf(pStr, "%f %f %f", &rgb[0], &rgb[1], &rgb[2]) == 3)
                    CLTWorldScene::SetModelAdd(rgb);
                printf("ModelAdd: %s\n", pStr ? pStr : "(unreadable)");
            }
        }

        // ClientRes, once the trees exist -- it is read THROUGH them, so the
        // tree stack decides whether the base game's CRES.DLL or GOTY's wins.
        if (nTrees > 0)
        {
            CLTStringMgr* pSM = LTGetStringMgr();
            if (pSM->LoadResourceDll(LTGetFileMgr(), "CRES.DLL"))
                printf("string table: %u strings\n", pSM->GetTableCount());
            else
                printf("⚠️  NO STRING TABLE -- every FormatString will return NULL,\n"
                       "    and game code does not check for that.\n");
        }

        if (nTrees == 0)
        {
            // The donor treats this as fatal (CClientMgr::Init ->
            // LT_NOGAMERESOURCES, clientmgr.cpp:252). Say so plainly rather
            // than letting every later failure look like a game-code bug.
            printf("⚠️  NO GAME RESOURCES. Set NOLF_GAME_DIR to a colon-separated\n"
                   "    list of directories holding startup.txt and the .rez files.\n");
        }
    }

    // ⚠️ AN INITIAL RENDER MODE, BEFORE THE GAME EXISTS.
    //
    // HookWindow() runs at the TOP of OnEngineInitialized -- before the game
    // calls SetRenderMode -- and its first line is
    // `if (!GetScreenSurface()) return FALSE`. With no mode set the null
    // renderer correctly refuses a screen surface, the hook fails SILENTLY
    // (a normal code path, no diagnostic), and NOLF ends up with no mouse
    // input at all.
    //
    // The real engine creates its window from screenwidth/screenheight/
    // screendepth in autoexec.cfg before the shell runs, so the surface is
    // already there. Doing the same here is faithful, not a workaround.
    {
        RMode boot;
        memset(&boot, 0, sizeof(boot));
        boot.m_bHardware = LTTRUE;
        // ⚠️ THE SIZE IS NOT DECIDED HERE. The GAME sets the mode during
        // startup (autoexec.cfg says 640x480) and it replaces whatever this
        // boot mode says, so a resolution chosen here is a resolution nobody
        // sees. CLTNullRender::SetMode owns it -- LT_SCREEN, one place.
        boot.m_Width = 640; boot.m_Height = 480; boot.m_BitDepth = 32;
        strcpy(boot.m_RenderDLL,    "nullrender");
        strcpy(boot.m_InternalName, "nullrender");
        strcpy(boot.m_Description,  "engine boot mode");
        LTGetScene()->Init(LTGetFileMgr());
        LTGetNullRender()->SetMode(&boot);

        // And the default window procedure the game will save and chain to.
        SetWindowLongA((HWND)1, GWL_WNDPROC, (intptr_t)LTHost_DefaultWndProc);
        LTInput_SetHostDefaultProc((void*)LTHost_DefaultWndProc);
    }

    printf("constructing stub engine interfaces...\n");

    LTSkeletonClient client;

    printf("stubs reached during construction: %d\n\n", LTStub_Count());

    // ⚠️ THE SERVER SIDE, OPT-IN. Everything drawn so far is the CLIENT; the
    // game's LOGIC -- triggers, cinematics, AI, objectives -- lives in
    // ObjectDLL and has never run in this port. It is brought up BEFORE the
    // client because that is the order the real engine uses: the server owns
    // the world and the client attaches to it.
    //
    // ⚠️ Off unless LT_SERVER=1. The frame contract is a recording of a run
    // with no server in it; switching one on by default would change the
    // recorded run before anyone had looked at whether it behaves.
    if (LTServerHost_Enabled())
    {
        printf("\n");
        if (LTServerHost_Start(LTGetFileMgr(), 0))
        {
            // ⚠️ THE HOOKS GO IN HERE, before the world does. Objects send
            // their attached special effects as they are CREATED, which is
            // during level load -- 29 of M01S01's 31 arrive before the client
            // has entered the world. Installing at world-start missed all of
            // them and looked like replication barely working.
            Rep_Install();

            // ⚠️ AND THE CHARACTERS. Replication gives the client a copy of
            // every object; this gives the in-game VIEW the objects to draw.
            // Without it RenderCamera drew the level and nothing living in it.
            LTScene_SetDynamicSource(LTServerHost_DynamicObjects());
            LTScene_SetModelXform(LTServerBrush_Xform());
            // ⚠️ Drive the sequence the real engine drives, one step at a time,
            // and let each report its own stub count. The point of the first
            // bring-up is the CENSUS -- what the game asks the engine for --
            // not the running, so a step that reaches a wall is a result.
            LTServerHost_StartWorld();
            LTServerHost_AddClient();

            // ⚠️ THE SOAK IS NOW THE HEADLESS CASE ONLY. Before the message
            // pipe existed the server could only be measured on its own, so it
            // ran 600 frames here and then stopped, and the client started
            // afterwards against a world that had already aged ten seconds.
            // With the two halves talking they have to tick TOGETHER -- the
            // per-frame LTServerHost_Update in the loop below is the real
            // thing, and running both would start the level ten seconds in.
            //
            // LT_SERVERSOAK=1 forces the soak back on for a server-only
            // measurement, which is still the fastest way to see what the
            // game's logic does across all 102 worlds.
            const bool bSoak = !bClient || getenv("LT_SERVERSOAK") != 0;
            const int nFrames = bSoak ? 600 : 0;         // ten seconds at 60Hz
            unsigned int nTotal = 0, nPeak = 0;
            for (int i = 0; i < nFrames; ++i)
            {
                LTServerHost_Update(1.0f / 60.0f);
                const unsigned int n = LTServerHost_ObjectsUpdated();
                nTotal += n;
                if (n > nPeak) nPeak = n;
            }
            if (nFrames)
                printf("[SERVER] %d frames (%.1fs): %u object updates, peak %u in one "
                       "frame  (stubs in the last frame: %d)\n\n",
                       nFrames, LTServerHost_Time(), nTotal, nPeak,
                       LTServerHost_StubsLastStep());
            else
                printf("[SERVER] ready -- ticking with the client, %s\n\n",
                       "one server frame per client frame");
        }
    }

    if (bClient)
    {
        printf("calling the game's CreateClientShell()...\n");
        fflush(stdout);

        IClientShell* pShell = CreateClientShell(&client);
        LTRecord_Event("host", "CreateClientShell %s", pShell ? "ok" : "null");

        // ⚠️ IMMEDIATELY, before OnEngineInitialized. The shell talks to the
        // server during its own initialisation, and a pipe wired up afterwards
        // would be correct and too late -- the same ordering trap that made
        // HookWindow fail silently (HANDOVER.md §2).
        g_pHostClientShell = pShell;
        LTMessage_SetClientDeliver(LTHost_DeliverToClient);
        printf("\nCreateClientShell returned %s  (stubs so far: %d)\n",
               pShell ? "a shell" : "NULL", LTStub_Count());

        if (pShell)
        {
            // Drive the shell the way the engine would. Constructing the shell
            // barely touches the engine; OnEngineInitialized is where the game
            // actually asks for things, so this is what produces a census worth
            // reading.
            RMode mode;
            memset(&mode, 0, sizeof(mode));
            mode.m_bHardware = LTTRUE;
            mode.m_Width     = 640;      // the resolution NOLF's UI is authored in
            mode.m_Height    = 480;
            mode.m_BitDepth  = 32;
            mode.m_pNext     = 0;
            strcpy(mode.m_RenderDLL,     "skeleton");
            strcpy(mode.m_InternalName,  "skeleton");
            strcpy(mode.m_Description,   "engine skeleton");

            LTGUID guid;
            memset(&guid, 0, sizeof(guid));

            printf("\ncalling OnEngineInitialized()...\n");
            fflush(stdout);
            LTRESULT r = pShell->OnEngineInitialized(&mode, &guid);
            LTRecord_Event("host", "OnEngineInitialized %d", (int)r);
            printf("OnEngineInitialized returned %d  (stubs so far: %d)\n",
                   (int)r, LTStub_Count());

            // ⚠️ WALK MODE, and it is OFF unless LT_WORLD names a world. With
            // it unset every line below behaves as it always did -- which is
            // what the frame contract was recorded against.
            // ⚠️ BRACED, and it was not. The two statements below were indented
            // as though they were part of this `if` and were not inside it, so
            // they ran with no world loaded -- which meant a HEADLESS run called
            // LTWindow_SetMouseLook(true) and hid the user's cursor. Indentation
            // that lies about scope is worse than none.
            //
            // ⚠️ AND IT IS THE VIEWER, NOT THE GAME. With LT_SERVER=1 the
            // game's own client runs instead: it has a server to talk to, a
            // player object to take its camera from, and its own Update to
            // draw with. Walk mode would fight it for the same screen, because
            // the two are alternative answers to "what is on screen this
            // frame" -- so LT_WORLD alone means the viewer and LT_WORLD with a
            // server means the game.
            if (!LTServerHost_Enabled() && LTWorldWalk_Init(LTGetFileMgr()))
            {
                LTInput_SetObserver(WalkInputObserver);
            }
            else if (LTServerHost_Enabled())
            {
                // ⚠️ THE CLIENT NEEDS THE WORLD TOO, and for a different
                // reason than the server does. The server has the world's
                // OBJECTS; the client has to DRAW its geometry, and
                // ILTClient::RenderCamera reads it from the shared scene.
                //
                // ⚠️ Same scene object as walk mode's -- see ltworldscene.h.
                // Loading it twice would be two copies of a 2.3-million-
                // triangle world, and the load is a no-op when the server has
                // already brought this level up.
                const char* pWorld = getenv("LT_WORLD");
                if (pWorld && *pWorld && LTWorldScene_Load(LTGetFileMgr(), pWorld))
                {
                    LTHost_ClientEnterWorld(pShell, pWorld);
                }
            }


            // ⚠️ RUN A CONSOLE STRING, WHICH IS THE GAME'S OWN FRONT DOOR.
            // LT_CONSOLE="Record WORLDS\M01S01.DAT demo" reaches
            // CGameClientShell::HandleRecord -> DoLoadWorld -> the loading
            // state -> StartGame -> MID_LOAD_GAME. Every step of that is the
            // GAME deciding to start a level; the engine only delivers the
            // command, exactly as a person typing into the console would.
            //
            // ⚠️ THIS IS NOT A BACK DOOR, and the distinction matters. Forcing
            // CInterfaceMgr::ChangeState from here would give a screen that
            // looks right and an interface manager that believes something
            // untrue. Running a command the game registered itself leaves the
            // game in charge of its own state machine.
            //
            // ⚠️ AFTER OnEngineInitialized, because that is where the game
            // calls RegisterConsoleProgram for all fifteen of its commands.
            // Before it, the registry is empty and the string does nothing.
            // ⚠️ THE LOADING SCREEN IS TURNED OFF HERE, ONCE, FOR EVERYBODY.
            //
            // NOLF's ANIMATED loading screen runs on a thread it spawns with
            // `CreateThread` (LoadingScreen.cpp:498, guarded by the game's own
            // `DynamicLoadScreen` console variable). This port has no
            // CreateThread, so the game asked for a thread, got nothing, and
            // the level load simply stopped -- with the last frame still on
            // screen and no error anywhere.
            //
            // ⚠️ AND IT MADE THE MENU LOOK BROKEN WHILE THE GAME WAS FINE.
            // Starting a mission from the real menus died here every time; the
            // only reason a level ever loaded in this port is that play.sh
            // passed `DynamicLoadScreen 0` by hand on the console, so the one
            // path a PLAYER uses was the one path that was never exercised.
            //
            // ⚠️ THIS IS THE GAME'S OWN SWITCH, not a patch to its code -- the
            // static loading screen is a shipped, supported option. Recorded
            // in DEVIATIONS.md. An explicit LT_CONSOLE can still set it back,
            // which is why this runs BEFORE that and not after.
            //
            // ⚠️ Implementing CreateThread instead would put a second thread
            // inside the renderer and the file manager, neither of which is
            // thread-safe here. That is a bigger change than the feature is
            // worth, and it would trade a missing animation for a race.
            {
                LTGetConsole()->RunString("DynamicLoadScreen 0");
            }

            if (const char* pCon = getenv("LT_CONSOLE"))
            {
                if (*pCon)
                {
                    printf("\nconsole: %s\n", pCon);
                    fflush(stdout);

                    // ⚠️ SEMICOLON-SEPARATED, like a real console line. A
                    // level start usually needs a setting first -- e.g.
                    // `DynamicLoadScreen 0`, which stops the game spawning a
                    // Win32 thread for its animated loading screen -- and
                    // forcing that into a second environment variable would
                    // make the order between them invisible.
                    std::string sAll(pCon);
                    size_t nFrom = 0;
                    while (nFrom <= sAll.size())
                    {
                        size_t nEnd = sAll.find(';', nFrom);
                        if (nEnd == std::string::npos) nEnd = sAll.size();

                        std::string sCmd = sAll.substr(nFrom, nEnd - nFrom);
                        nFrom = nEnd + 1;

                        while (!sCmd.empty() && sCmd[0] == ' ') sCmd.erase(0, 1);
                        if (sCmd.empty()) continue;

                        std::string sName(sCmd);
                        const size_t nSp = sName.find(' ');
                        if (nSp != std::string::npos) sName.erase(nSp);

                        // ⚠️ Only warn for something that is NEITHER a
                        // registered command NOR an existing variable --
                        // `DynamicLoadScreen 0` is a perfectly good console
                        // line and is not a program.
                        if (!LTGetConsole()->FindProgram(sName.c_str()) &&
                            !LTGetConsole()->Find(sName.c_str()) && nSp == std::string::npos)
                        {
                            std::string sList;
                            LTGetConsole()->ListPrograms(sList);
                            fprintf(stderr, "[CONSOLE] no such command \"%s\". "
                                            "The game registered: %s\n",
                                    sName.c_str(), sList.c_str());
                        }

                        printf("[CONSOLE] %s\n", sCmd.c_str());
                        fflush(stdout);
                        LTGetConsole()->RunString(sCmd.c_str());
                    }
                }
            }

            // ⚠️ The sound manager needs the file system before it can load
            // anything, and it is the SAME file manager the rest of the engine
            // uses -- a second one would open every archive twice.
            client.GetSoundMgr().Attach(LTGetFileMgr());

            // ⚠️ THE MANAGER IS AVAILABLE WHETHER OR NOT THE DEVICE IS, and the
            // distinction is load-bearing. Playing a sound loads and decodes it
            // and hands back a HANDLE; the DEVICE only decides whether anyone
            // hears it. NOLF's dialogue advances by asking IsSoundDone about
            // the line it is speaking, so a PlaySound that always failed would
            // leave a cinematic waiting on a sound that never started -- and a
            // headless test could never see the sound path work at all.
            g_pHostSound = &client.GetSoundMgr();

            // ⚠️ And the SERVER's sounds reach the same mixer. See
            // LTServerHost_SetSoundPlayer for why it is installed rather than
            // called directly.
            LTServerHost_SetSoundPlayer(&LTHost_ClientPlaySound);
            LTServerHost_SetCommandQuery(&LTHost_IsCommandOn);
            LTMessage_SetClientToServerObject(&LTHost_ClientToServerObject);
            LTMessage_SetSFXCapture(&LTServerHost_CaptureSFX);
            LTServerHost_SetWorldClosingHook(&LTHost_ServerWorldClosing);
            LTReplicate_SetPlayerHook(&LTHost_ReplicatePlayerAlias);
            LTWorldScene_SetLiveQuery(&LTServerHost_WorldObjectLive);
            LTWorldScene_SetLivePoseQuery(&LTServerHost_WorldObjectPose);
            LTWorldScene_SetLiveAnimQuery(&LTServerHost_WorldObjectAnim);
            LTServerHost_SetSoundQueries(&LTHost_SrvSndDone,
                                         &LTHost_SrvSndDuration,
                                         &LTHost_SndKill,
                                         &LTHost_SndKillLoop);

            // ⚠️ THE CLOCK IS ADVANCED HERE, ONCE PER FRAME, and nowhere else.
            // In fixed-step mode that is what makes the run reproducible: N
            // frames always produce the same pixels. LT_UPDATES raises the
            // count for experiments -- the contract's reference uses the
            // default, so changing it changes the record.
            int nUpdates = 3;
            const char* pN = getenv("LT_UPDATES");
            if (pN && atoi(pN) > 0) nUpdates = atoi(pN);

            // ⚠️ The window is opened BEFORE the frame count is announced, or
            // the announcement is a lie: the first version printed "3 times"
            // and then silently ran until closed.
            bool bWindow = LTWindow_Wanted();
            bool bMetalActive = false;      // for the title bar
            if (bWindow)
            {
                uint32 w = 640, h = 480;
                LTGetNullRender()->GetDims(LTGetNullRender()->GetScreenSurface(), &w, &h);
                LTWindow_Open((int)w, (int)h, "NOLF (macOS port)");

                // With a window, 3 frames is a flicker. Run until the user
                // closes it unless LT_UPDATES says otherwise -- but NEVER
                // change the default for a headless run, which the contract
                // depends on being exactly nUpdates frames long.
                if (!pN) nUpdates = 0;      // 0 = until closed

                // ⚠️ AFTER LTWindow_Open, AND THAT ORDER IS THE POINT. Metal
                // attaches a CAMetalLayer to the window's CONTENT VIEW, so
                // attaching before the window exists finds no view, reports
                // "unavailable", and falls back to software -- silently doing
                // the right thing for the wrong reason, which is the hardest
                // kind of bug to notice from the outside.
                //
                // ⚠️ The offscreen validation harness cannot catch this: it
                // never opens a window. Passing tests said nothing about it.
                // ⚠️ THE DEVICE STARTS ONLY WITH A WINDOW. A headless run --
                // which is what the contract and every test script use -- must
                // not start making noise on the machine running it.
                //
                // ⚠️ AND IT USED TO START ONLY IN WALK MODE, which is why the
                // GAME was silent. The sound manager, the mixer and CoreAudio
                // have all been finished for a while; the game path just never
                // opened the device, and `ILTClient::PlaySound` -- the function
                // pointer the game actually calls -- was still a stub in front
                // of it.
                {
                    if (LTAudio_Start(&client.GetSoundMgr().GetMixer()))
                    {
                        printf("[SOUND] CoreAudio running at %u Hz\n",
                               (unsigned)CLTMixer::kRate);
                    }
                    else
                    {
                        printf("[SOUND] no output device (%s) -- silent\n",
                               LTAudio_LastError() ? LTAudio_LastError() : "?");
                    }
                    fflush(stdout);
                }

                if (LTWorldWalk_Active())
                {
                    // Grab the pointer: walk mode exists to be walked around.
                    // Tab gives it back.
                    LTWindow_SetMouseLook(LTWorldWalk_GetMouseLook());

                    LTWorldWalk_SetSoundPlayer(&WalkPlaySound);

                    if (LTHost_WantMetal(true))
                    {
                        // ⚠️ The window's OWN size, not a literal 640x480: the
                        // layer's drawable must match what is being rendered or
                        // the frame is scaled.
                        if (LTMetal_Attach((int)w, (int)h) && LTWorldMetal_Install())
                        {
                            printf("[METAL] backend active (%ux%u)\n", w, h);
                            bMetalActive = true;
                        }
                        else
                            printf("[METAL] unavailable (%s) -- software renderer\n",
                                   LTMetal_LastError() ? LTMetal_LastError() : "?");
                        fflush(stdout);
                    }
                }
            }

            // ⚠️ THE GAME'S RENDERER: METAL BY DEFAULT WITH A WINDOW. The 3D
            // view is drawn on the GPU offscreen and read back into the screen
            // surface, so the 2D interface composites on top unchanged.
            // Headless runs stay on the SOFTWARE rasteriser unless LT_METAL=1:
            // it is the deterministic reference the frame contract and every
            // pixel comparison are built on. LT_SOFTWARE=1 (or LT_METAL=0)
            // forces software with a window too.
            if (!LTWorldWalk_Active() && LTHost_WantMetal(bWindow))
            {
                char szErr[256];
                if (LTMetal_SelfTest(szErr, sizeof(szErr)))
                {
                    LTScene_SetGpuCamera(&LTWorldMetal_GameCamera);
                    bMetalActive = true;

                    // ⚠️ THE COMPOSITOR (HANDOVER 5bq): the 3D at the window's
                    // native pixels, the 2D layers combined on the GPU. The
                    // offscreen camera above stays installed as the fallback
                    // for a frame the compositor cannot draw.
                    bool bComp = false;
                    if (LTComp_Wanted())
                    {
                        bComp = !bWindow || LTMetal_CompAttach();
                        if (bComp)
                        {
                            LTComp_Install(bWindow ? &LTMetal_CompOutputSize : 0,
                                           &LTMetal_Composite, &LTMetal_CompFrameEnd);
                            LTScene_SetGpuScene(&LTWorldMetal_GameScene);
                            LTScene_SetGpuObjects(&LTWorldMetal_MenuObjects);
                            if (bWindow) LTWindow_SetPointMapper(&LTComp_OutputToSurface);
                            int ow = 0, oh = 0;
                            LTComp_OutputSize(&ow, &oh);
                            uint32 sw = 0, sh = 0;
                            LTGetNullRender()->GetDims(LTGetNullRender()->GetScreenSurface(), &sw, &sh);
                            printf("[COMP] compositor on: 3D at the output's %dx%d, 2D %ux%u "
                                   "(menus zoomed to fit)\n", ow, oh, (unsigned)sw, (unsigned)sh);
                        }
                        else
                            printf("[COMP] unavailable (%s) -- 3D read back instead\n",
                                   LTMetal_LastError() ? LTMetal_LastError() : "?");
                    }
                    if (!bComp)
                        printf("[METAL] game camera on the GPU (offscreen + readback)\n");
                }
                else
                    printf("[METAL] unavailable (%s) -- software renderer\n", szErr);
                fflush(stdout);
            }

            if (nUpdates)
                printf("\ncalling Update() %d times (timestep %.4fs)...\n",
                       nUpdates, LTGetClock()->GetStep());
            else
                printf("\nrunning until the window is closed (timestep %.4fs)...\n",
                       LTGetClock()->GetStep());
            fflush(stdout);

            for (int i = 0; nUpdates == 0 || i < nUpdates; ++i)
            {
                // ⚠️ FRAME TIME, measured -- "it is slow" is a number. The
                // census reports the mean over frames in which a 3D view was
                // actually drawn, split into RenderCamera and everything else.
                static auto s_tFrame = std::chrono::steady_clock::now();
                {
                    const auto tNow = std::chrono::steady_clock::now();
                    static unsigned int s_nLastRC = 0;
                    const double ms = std::chrono::duration<double, std::milli>(tNow - s_tFrame).count();
                    if (LTScene_RenderCameraCalls() != s_nLastRC && i > 0)
                    { g_fFrameMs3D += ms; ++g_nFrames3D; }
                    s_nLastRC = LTScene_RenderCameraCalls();
                    s_tFrame = tNow;
                }
                LTGetClock()->Tick();

                // ⚠️ The engine owns the frame's starting state. Without this
                // anything that moves smears -- the mouse cursor leaves a
                // trail -- because the game only clears 3 times in 600 frames
                // and relies on the 3D scene to repaint the viewport.
                LTGetNullRender()->ClearScreenForFrame();

                // ⚠️ Scripted keys go in BEFORE Update, and are pumped whether
                // or not a window exists -- the whole point is a HEADLESS run
                // that can reach any screen.
                LTInput_ScriptTick(i);
                LTInput_ClickTick(i);
                LTHost_AimTick(i);
                LTHost_ConsoleTick(i);
                LTHost_GodTick(i);
                LTCommand_ScriptTick(i);
                if (LTHost_PlayProbeTick(i)) break;
                LTHost_PlayerYTick(i);

                // ⚠️ LT_CLIPOS="x,y,z[,frame]" -- PUT THE PLAYER SOMEWHERE, and
                // unlike LT_PLAYERPOS this one STICKS.
                //
                // LT_PLAYERPOS moves the SERVER's player object, and the client
                // overwrites the server's copy every frame from its own
                // position (the client is authoritative for the player here),
                // so a server-side move survives about one frame. This moves
                // the CLIENT's object, which is the one that then tells the
                // server where it is.
                //
                // It exists because a headless run cannot walk: testing whether
                // touching a weapon picks it up means standing on the weapon,
                // and the weapon is several rooms and a staircase away.
                {
                    // ⚠️ HELD FOR A WINDOW OF FRAMES, NOT SET ONCE. Both halves
                    // of the player are written by something else every frame
                    // -- the client mover, and the server being told where the
                    // client is -- so a single write is undone before anything
                    // can touch anything. LT_CLIPOS_HOLD sets how many frames
                    // to keep asserting it (default 60).
                    // ⚠️ A SEQUENCE TOO (5ch): "x,y,z,frame;x,y,z,frame" places
                    // the player at each in turn, each held LT_CLIPOS_HOLD
                    // frames -- into a water volume and back out, in one run.
                    static const char* pWant = getenv("LT_CLIPOS");
                    static const int nHoldEach = getenv("LT_CLIPOS_HOLD")
                                     ? atoi(getenv("LT_CLIPOS_HOLD")) : 60;
                    static int nHold = nHoldEach;
                    static const char* pAt = pWant;
                    static bool bDone = false;
                    static bool bSaid = false;
                    if (pAt && !bDone)
                    {
                        float v[4] = { 0, 0, 0, 0 };
                        const int n = sscanf(pAt, "%f,%f,%f,%f",
                                             &v[0], &v[1], &v[2], &v[3]);
                        if (n >= 3 && (float)i >= v[3])
                        {
                            void* h = LTSession_GetClientObject();
                            if (h)
                            {
                                const bool bLast = (--nHold <= 0);
                                const float pos[3] = { v[0], v[1], v[2] };
                                LTScene_TeleportTo(h, pos);
                                // And the game's own movers: CMoveMgr drives
                                // its local object, not the client object.
                                for (std::set<void*>::const_iterator itM = g_ClientMovers.begin();
                                     itM != g_ClientMovers.end(); ++itM)
                                    LTScene_TeleportTo(*itM, pos);

                                // ⚠️ AND THE SERVER'S COPY. The client is
                                // authoritative only while it is driving; in a
                                // cinematic it is not, and the server's player
                                // would stay where it was while the hook
                                // reported success.
                                const bool bSrv = LTServerHost_PlacePlayer(pos);
                                if (!bSaid)
                                {
                                    bSaid = true;
                                    printf("[CLIPOS] frame %d: holding the player at "
                                           "(%.1f %.1f %.1f) for %d frame(s)  [client%s]\n",
                                           i, v[0], v[1], v[2], nHold + 1,
                                           bSrv ? " and server" : " only -- NO SERVER PLAYER");
                                    fflush(stdout);
                                }
                                if (bLast)
                                {
                                    const char* pNext = strchr(pAt, ';');
                                    if (pNext) { pAt = pNext + 1; nHold = nHoldEach; bSaid = false; }
                                    else bDone = true;
                                }
                            }
                        }
                    }
                }
                LTInput_Pump(pShell);

                // ⚠️ THE COMMAND TRANSITIONS, AND THE HOST IS WHO DELIVERS
                // THEM. `IsCommandOn` answers a level and covers every
                // CONTINUOUS action; every DISCRETE one -- activate, change
                // weapon, reload, flashlight, zoom -- is handled in
                // `CGameClientShell::OnCommandOn`, which the ENGINE is
                // required to call on the off->on transition. Nothing called
                // it, so it had never run once.
                //
                // ⚠️ THIS IS ALSO HOW A CINEMATIC ENDS. T01S01's opening
                // cinematic is a LOOP by design -- its key path jumps
                // cama38 -> cama51 -> cama38 forever -- and its only exit is
                // the player pressing Activate: OnCommandOn -> DoActivate ->
                // MID_PLAYER_ACTIVATE -> CPlayerObj::Activate ->
                // PlayingCinematic(TRUE) -> "SKIP" -> HandleOff. Without this
                // call the cutscene could never end and the player could never
                // be given control, on any level in the game.
                //
                // ⚠️ AFTER LTInput_Pump, so a key pressed this frame produces
                // its edge this frame rather than next.
                {
                    LTCommandEdge edges[32];
                    const int nEdges = LTCommand_TakeEdges(edges, 32);
                    for (int e = 0; e < nEdges; ++e)
                    {
                        if (edges[e].m_bOn) pShell->OnCommandOn(edges[e].m_nCommand);
                        else                pShell->OnCommandOff(edges[e].m_nCommand);

                        // ⚠️ AND THE SERVER HEARS IT TOO. In the real engine
                        // the client's command state is replicated, and
                        // `CGameServerShell::OnCommandOn` acts on it (run
                        // lock). Delivering to the client only would leave the
                        // two halves disagreeing about what is held.
                        LTServerHost_OnCommand(edges[e].m_nCommand, edges[e].m_bOn);

                        static const bool bTrace = (getenv("LT_CMDTRACE") != 0);
                        if (bTrace)
                            printf("[CMD] %s command %d\n",
                                   edges[e].m_bOn ? "ON " : "OFF", edges[e].m_nCommand);
                    }
                }

                // ⚠️ MODEL KEYS -- THE ANIMATION TELLING THE GAME TO ACT,
                // and the missing half of "the trigger does nothing".
                //
                // NOLF's weapon sets m_bFire in exactly two places, both
                // inside CWeaponModel::OnModelKey (WeaponModel.cpp:1645,
                // :1707), and Fire() runs only when m_bFire is set. So the
                // whole chain -- trigger, command, control flag, fire
                // animation -- was working and ended at a door the engine
                // never knocked on. Footsteps and character FX arrive the same
                // way.
                //
                // ⚠️ THE STRING IS SPLIT ON SPACES INTO argv, because that is
                // what ArgList is: "SOUND Fire" is a key with an argument, and
                // the game reads argv[0] to decide what the key means.
                {
                    LTModelKey keys[32];
                    const unsigned nKeys = LTScene_TakeModelKeys(keys, 32);
                    for (unsigned k = 0; k < nKeys; ++k)
                    {
                        char szKey[256];
                        strncpy(szKey, keys[k].m_pCommand ? keys[k].m_pCommand : "",
                                sizeof(szKey) - 1);
                        szKey[sizeof(szKey) - 1] = 0;

                        char* argv[16];
                        int   argc = 0;
                        for (char* t = szKey; *t && argc < 16; )
                        {
                            while (*t == ' ' || *t == '\t') ++t;
                            if (!*t) break;
                            argv[argc++] = t;
                            while (*t && *t != ' ' && *t != '\t') ++t;
                            if (*t) *t++ = 0;
                        }
                        if (!argc) continue;

                        ArgList args;
                        args.argv = argv;
                        args.argc = argc;
                        pShell->OnModelKey((HLOCALOBJ)keys[k].m_hObj, &args);

                        static const bool bKeyTrace = (getenv("LT_KEYTRACE") != 0);
                        if (bKeyTrace)
                            printf("[MKEY] %s\n", keys[k].m_pCommand);
                    }
                }

                // ⚠️ THE SERVER RUNS INSIDE THE CLIENT'S FRAME, AND BEFORE IT.
                // This is the step that turns two programs sharing a process
                // into a game: the server advances the world and answers
                // whatever the client asked for last frame, then the client
                // draws the result. Reversing them would draw the world one
                // frame stale and make every input feel a frame late.
                //
                // ⚠️ Its timestep is the CLIENT's, not a hard-coded 1/60 --
                // two clocks that disagree is how a level runs at a different
                // speed than it is drawn.
                //
                // ⚠️ Then PumpClient, which is the server->client direction.
                // It runs AFTER the server update so a message the server sent
                // this frame is handled this frame, and BEFORE pShell->Update
                // so the shell draws having already heard it.
                // ⚠️ NO DEVICE MEANS NOBODY CONSUMES THE MIXER, and a voice
                // ends only when it is consumed. See CLTMixer::Advance: without
                // this a headless run's dialogue stops after its first line,
                // because IsSoundDone never becomes true. With a device running
                // CoreAudio does this work, and doing it here as well would
                // play every sound twice as fast.
                //
                // ⚠️ OUTSIDE the server branch, because it is the CLIENT's
                // mixer -- a client-only run plays menu and interface sounds
                // and they have to end too.
                if (g_pHostSound && !LTAudio_Running())
                    // GetFrameTime for the same reason as the server update
                    // below: on the wall clock a voice has to be consumed at
                    // the rate time is actually passing, or dialogue outlasts
                    // the scene it belongs to.
                    g_pHostSound->GetMixer().Advance(
                        (unsigned int)(LTGetClock()->GetFrameTime()
                                       * (float)CLTMixer::kRate + 0.5f));

                if (LTServerHost_Enabled())
                {
                    // ⚠️ GetFrameTime, NOT GetStep -- AND THIS IS WHERE THE
                    // CINEMATICS' SLOW MOTION ACTUALLY LIVED.
                    //
                    // GetStep() is the FIXED step the clock was configured
                    // with; GetFrameTime() is what the frame just cost. In
                    // fixed-timestep mode they are the same number, so this
                    // read correctly for as long as that was the only mode --
                    // and then kept returning 1/60 per frame after the wall
                    // clock arrived, which left the SERVER running in slow
                    // motion while the client ran at real speed. Every
                    // keyframer, and therefore every cinematic camera, is on
                    // this clock.
                    LTServerHost_Update(LTGetClock()->GetFrameTime());
                    // The brushes have just been placed: carry the player on
                    // any lift she stands on (ltscene.cpp, HANDOVER 5bl).
                    for (std::set<void*>::const_iterator itC = g_ClientMovers.begin();
                         itC != g_ClientMovers.end(); ++itC)
                        LTScene_CarryOnMovers(*itC);

                    // ⚠️ THE GAME CHOSE A LEVEL, so finish the job. RunWorld
                    // happens deep inside the game's own message handler; this
                    // is where that news comes back out. Adding the client and
                    // entering the world from in here -- rather than from
                    // inside the handler -- keeps the game's call stack out of
                    // the engine's re-entrancy.
                    //
                    // ⚠️ WHAT THE NEW WORLD SENT IS DELIVERED IN TWO PARTS,
                    // AROUND THE ENTER. LT2 (s_client.cpp, serverde_impl.cpp)
                    // puts a game message on the connection the moment it is
                    // written -- so what the server said while it loaded the
                    // world arrives before SMSG_LOADWORLD -- and an object's
                    // effect only in an object update, once the client is in
                    // the world. (The "camera faces a wall" that once argued
                    // for entering first was the save lacking clocks and
                    // countdowns; with save version 4 both orders restore the
                    // same view, measured.)
                    //
                    // ⚠️ SPLIT, AS LT2 DOES IT (measured, 5bz): the GAME's
                    // messages before the enter -- MID_PLAYER_LOADCLIENT sets
                    // m_bRestoringGame, which OnEnterWorld reads
                    // (GameClientShell.cpp:1497) -- and the EFFECTS after it.
                    // Effects delivered first drew the player's sleeve red
                    // after a load. LT_PUMPBEFOREENTER=1 = everything first,
                    // LT_PUMPAFTERENTER=1 = everything after (both say so).
                    static const int s_nPumpOrder = []() {
                        int n = 0;
                        if (getenv("LT_PUMPBEFOREENTER")) { n = 1; printf("[HOST] LT_PUMPBEFOREENTER=1 -- client messages AND effects delivered before a new world is entered (old)\n"); }
                        else if (getenv("LT_PUMPAFTERENTER")) { n = 2; printf("[HOST] LT_PUMPAFTERENTER=1 -- client messages delivered only after a new world is entered\n"); }
                        fflush(stdout);
                        return n; }();
                    const bool bWorldStarted = LTServerHost_TakeWorldStarted();
                    if (s_nPumpOrder == 1) LTMessage_PumpClient();
                    else if (s_nPumpOrder == 0 && bWorldStarted) LTMessage_PumpClientGameOnly();
                    if (bWorldStarted)
                    {
                        LTServerHost_AddClient();
                        LTHost_ClientEnterWorld(pShell, LTWorldScene_Name());

                    }
                    if (s_nPumpOrder != 1) LTMessage_PumpClient();

                    // ⚠️ AFTER the server has run and BEFORE the frame is
                    // drawn. A mirror synced after the draw is a frame behind,
                    // which is invisible standing still and very visible on a
                    // camera move -- which is exactly what this is for.
                    LTReplicate_Sync();

                    // ⚠️ MIRROR THE SERVER'S PLAYER ONTO THE CLIENT'S.
                    // The server owns where the player is; the client's copy is
                    // what the camera and the interface ask about. Without this
                    // the client's player sits at the origin while the server's
                    // walks the level, and the camera follows the wrong one.
                    //
                    // ⚠️ Guarded on the server ANSWERING. LTServerHost_
                    // GetPlayerTransform returns false before the player
                    // exists, and writing the origin in that case would be the
                    // uninitialised-out-parameter mistake with extra steps.
                    float p3[3], q4[4];
                    if (LTServerHost_GetPlayerTransform(p3, q4))
                    {
                        void* h = LTSession_GetClientObject();
                        LTScene_SetObjectPos(h, p3[0], p3[1], p3[2]);
                        LTScene_SetObjectRotation(h, q4);
                        // ⚠️ AND THE PLAYER'S USER FLAGS. In the donor the client
                        // object IS the replicated server player, so flags the
                        // server sets arrive. Single-player DUCK is read from
                        // them, not from the key: CMoveMgr.cpp:431 tests
                        // USRFLG_PLAYER_DUCK (set by PlayerObj.cpp:3929) -- so
                        // Cate never crouched (eye stayed at y=350 with Duck
                        // held, M01S01, HANDOVER 5bi). LT_NOPLAYERFLAGS=1 = old.
                        static const bool s_bNoFlags = []() {
                            const bool b = (getenv("LT_NOPLAYERFLAGS") != 0);
                            if (b) { printf("[HOST] LT_NOPLAYERFLAGS=1 -- server player user flags NOT copied to the client object\n"); fflush(stdout); }
                            return b; }();
                        unsigned int nT = 0, nF = 0, nU = 0; float sp[3], sq[4]; const char* pF = 0;
                        if (!s_bNoFlags && LTServerHost_PlayerHandle()
                            && LTServerHost_GetObjectState(LTServerHost_PlayerHandle(), &nT, sp, sq, &nF, &nU, &pF))
                            LTScene_SetObjectUserFlags(h, nU);
                        // The ears follow the client object (donor soundmgr
                        // .cpp:1319) unless a cinematic camera holds them.
                        if (g_pHostSound) g_pHostSound->UpdateClientListener(p3, q4);
                    }
                }

                // ⚠️ Walk mode draws the WORLD instead of the game's UI. The
                // game's Update paints menus over the whole screen, so the two
                // cannot both run; this is a viewer standing in the client's
                // frame loop, not the game running a level.
                // ⚠️ LT_CINESHOT=<dir> -- AN OBSERVER, KEPT FOR A REASON THAT
                // NO LONGER APPLIES.
                //
                // ⚠️ THE JUSTIFICATION HERE WAS OUT OF DATE, and that is worth
                // more than the feature: this said "`ILTClient::RenderCamera`
                // is still a stub, so the game's own camera reaches no
                // renderer". RenderCamera is fully implemented and wired, and
                // the game draws its own cinematics -- that is what every
                // screenshot of the T01S01 opening is. A stale comment claiming
                // a finished function is missing is exactly the failure that
                // cost this port 56 misfiled census entries (HANDOVER.md §5w).
                //
                // What it still does honestly: the server knows where the
                // cinematic camera IS (the Camera object carrying
                // USRFLG_CAMERA_LIVE) and the world renderer will draw from any
                // eye, so this gives an independent view of where the camera
                // went, sampled on a schedule. That remains useful for checking
                // a camera PATH without trusting the game's own draw.
                if (LTCineShot_Dir() && LTWorldScene_IsLoaded())
                {
                    float cp[3], cq[4];
                    if (LTServerHost_GetLiveCameraTransform(cp, cq))
                        LTCineShot_Frame(cp, cq, LTServerHost_Time());
                    else
                        LTCineShot_Idle();
                }

                if (LTWorldWalk_Active())
                {
                    LTWindow_SetMouseLook(LTWorldWalk_GetMouseLook());

                    // ⚠️ THE EARS FOLLOW THE CAMERA, every frame and before
                    // the frame is drawn. A listener updated after the fact
                    // pans one frame late, which is inaudible standing still
                    // and very audible turning quickly.
                    {
                        float lp[3], lr[3];
                        LTWorldWalk_GetListener(lp, lr);
                        client.GetSoundMgr().UpdateListener(lp, lr);
                    }

                LTWorldWalk_Frame(
                        LTGetNullRender()->GetView(LTGetNullRender()->GetScreenSurface()),
                        LTGetClock()->GetStep());
                }
                else
                {
                    LTHost_UpdateFog();
                    pShell->Update();

                    // ⚠️ AND PostUpdate, WHICH THE ENGINE NEVER CALLED. The
                    // donor runs it after every Update (clientshell.cpp:450,
                    // clientmgr.cpp:905). NOLF quick-saves there
                    // (GameClientShell.cpp PostUpdate: m_bQuickSave ->
                    // SaveGame(QUICKSAVE_FILENAME)), so F6 printed "Quick
                    // saving..." and wrote nothing. Its CInterfaceMgr::PostUpdate
                    // also calls FlipScreen, which is recorded but does not
                    // present: the host presents below.
                    // LT_NOPOSTUPDATE=1 = the old loop (announces itself).
                    static const bool s_bNoPost = []() {
                        const bool b = (getenv("LT_NOPOSTUPDATE") != 0);
                        if (b) { printf("[HOST] LT_NOPOSTUPDATE=1 -- IClientShell::PostUpdate not called (no quick save)\n"); fflush(stdout); }
                        return b; }();
                    if (!s_bNoPost)
                    {
                        LTNullRender_SetHostPresents(true);
                        pShell->PostUpdate();
                        LTNullRender_SetHostPresents(false);
                    }
                }

                // ⚠️ LT_DUMP_EVERY=<n> -- A CONTACT SHEET, NOT A SNAPSHOT.
                //
                // LT_DUMP_SCREEN writes ONE frame, at the end. That cannot
                // tell "the screen is black" from "the screen was black at
                // that instant": a fade, a load, a cinematic cut and a real
                // hang all produce the same single black image, and each was
                // guessed at rather than measured at least once here.
                //
                // Writes <LT_DUMP_SCREEN or /tmp/nolf>_<frame>.ppm. Costs
                // nothing when unset, and the frame number is in the NAME so a
                // sheet of them reads as a timeline.
                if (!g_sProbeDump.empty())
                {
                    LTGetNullRender()->DumpScreenPPM(g_sProbeDump.c_str());
                    g_sProbeDump.clear();
                }
                // LT_CAMVIEWTRACE=<n> -- every n frames, where the game camera
                // was and which way it looked (heading and pitch, degrees).
                {
                    static const int s_nCamEvery = getenv("LT_CAMVIEWTRACE") ? atoi(getenv("LT_CAMVIEWTRACE")) : 0;
                    float cp[3], cq[4];
                    if (s_nCamEvery > 0 && (i % s_nCamEvery) == 0 && LTScene_GetLastCamera(cp, cq))
                    {
                        const float x = cq[0], y = cq[1], z = cq[2], w = cq[3];
                        const float fx = 2.0f*(x*z + w*y), fy = 2.0f*(y*z - w*x), fz = 1.0f - 2.0f*(x*x + y*y);
                        printf("[CAMVIEW] frame %d at (%.0f %.0f %.0f) heading %.1f pitch %.1f\n", i, cp[0], cp[1], cp[2],
                               atan2f(fx, fz) * 57.29578f, asinf(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy)) * 57.29578f);
                        fflush(stdout);
                    }
                }
                {
                    static const char* pEvery = getenv("LT_DUMP_EVERY");
                    static const int   nEvery = pEvery ? atoi(pEvery) : 0;
                    // LT_DUMP_FROM=<n> -- nothing before frame n, so a dense
                    // sheet around one moment (a 100 ms blink) costs little.
                    static const char* pFrom = getenv("LT_DUMP_FROM");
                    static const int   nFrom = pFrom ? atoi(pFrom) : 0;
                    if (nEvery > 0 && i >= nFrom && (i % nEvery) == 0)
                    {
                        const char* pBase = getenv("LT_DUMP_SCREEN");
                        char szPath[512];
                        snprintf(szPath, sizeof(szPath), "%s_%06d.ppm",
                                 (pBase && pBase[0]) ? pBase : "/tmp/nolf", i);
                        LTGetNullRender()->DumpScreenPPM(szPath);

                        // ⚠️ And the in-game probe beside it, when LT_UVPROBE
                        // is on -- the same frame, measured per pixel.
                        LTWorldScene_ProbeDump(szPath);

                        // Logged, so a contact sheet can pair each image with
                        // whatever else the log says was true at that frame.
                        fprintf(stderr, "[DUMP] frame %d -> %s\n", i, szPath);
                    }
                }

                // ⚠️ THE HOST PRESENTS, NOT THE GAME.
                //
                // Presentation was hung off ILTClient::FlipScreen -- and the
                // game NEVER CALLS IT. Measured: 837 Start3D, 837 End3D, 0
                // FlipScreen. In the real engine the ENGINE's main loop flips
                // after the shell updates; the shell only draws into the
                // buffer. So the window was correct, the compositor was
                // correct, and nothing was ever shown.
                //
                // ⚠️ The general shape: "the game will call X" is an assumption
                // about someone else's control flow. The draw census could have
                // answered it before a line was written -- FlipScreen simply
                // was not in the list.
                // Headless: the frame's GPU work is committed here (a window
                // commits it with the present just below).
                if (!bWindow) LTComp_FrameEnd();

                // ⚠️ LT_WINRESIZE=WxH@frame -- A TEST SWITCH: resizes the window's
                // content to W x H points at that frame, the one thing a headless
                // harness cannot do by hand (says so).
                if (bWindow)
                {
                    static int s_nRW = 0, s_nRH = 0, s_nRF = -1;
                    static bool s_bParsed = false;
                    if (!s_bParsed)
                    {
                        s_bParsed = true;
                        if (const char* pR = getenv("LT_WINRESIZE"))
                            if (sscanf(pR, "%dx%d@%d", &s_nRW, &s_nRH, &s_nRF) == 3)
                                printf("[WINDOW] LT_WINRESIZE: content -> %dx%d points at frame %d\n",
                                       s_nRW, s_nRH, s_nRF);
                    }
                    if (i == s_nRF) LTWindow_SetContentSize(s_nRW, s_nRH);
                }

                if (bWindow)
                {
                    LTGetNullRender()->PresentToWindow();
                    LTComp_FrameEnd();
                    LTWindow_PumpEvents();

                    // ⚠️ THE TITLE BAR: which renderer draws the 3D view, and
                    // the frame rate measured over the last half second of
                    // wall time (the loop's own 1/60 s sleep included, so 60
                    // is the ceiling). Metal exists only in walk mode -- the
                    // game path is the software rasteriser (play.sh).
                    // LT_NOTITLESTATS=1 leaves the title alone (an A/B switch).
                    static const bool s_bNoTitle = (getenv("LT_NOTITLESTATS") != 0);
                    static const bool s_bSaidT = (s_bNoTitle
                        ? (printf("[WINDOW] LT_NOTITLESTATS=1 -- title bar not updated\n"), true) : true);
                    (void)s_bSaidT;
                    if (!s_bNoTitle)
                    {
                        static auto s_tLast = std::chrono::steady_clock::now();
                        static int  s_nFrames = 0;
                        ++s_nFrames;
                        const auto tNow = std::chrono::steady_clock::now();
                        const double dt = std::chrono::duration<double>(tNow - s_tLast).count();
                        if (dt >= 0.5)
                        {
                            uint32 sw = 0, sh = 0;
                            LTGetNullRender()->GetDims(LTGetNullRender()->GetScreenSurface(), &sw, &sh);
                            char szTitle[200];
                            int ow = 0, oh = 0;
                            if (LTComp_Active() && LTComp_OutputSize(&ow, &oh))
                                snprintf(szTitle, sizeof(szTitle),
                                         "NOLF (macOS port) \u2014 Metal \u2014 3D %dx%d, 2D %ux%u \u2014 %.1f FPS",
                                         ow, oh, (unsigned)sw, (unsigned)sh, s_nFrames / dt);
                            else
                            snprintf(szTitle, sizeof(szTitle),
                                     "NOLF (macOS port) \u2014 %s \u2014 %ux%u \u2014 %.1f FPS",
                                     bMetalActive ? "Metal" : "Software renderer",
                                     (unsigned)sw, (unsigned)sh, s_nFrames / dt);
                            LTWindow_SetTitle(szTitle);
                            s_tLast = tNow;
                            s_nFrames = 0;
                        }
                    }

                    // ⚠️ AppKit fills the queue during PumpEvents, so this
                    // drains what the platform just produced. The scripted
                    // pump above handles the headless case.
                    LTInput_Pump(pShell);

                    if (LTWindow_ShouldClose()) break;

                    // ⚠️ Without this the loop spins a core flat and the clock
                    // races ahead of anything a human could see. 1/60s is the
                    // fixed timestep the clock already assumes.
                    //
                    // ⚠️ PACED, NOT FIXED: sleep only what is LEFT of the 1/60s
                    // since the previous frame. A flat 16.7 ms on top of every
                    // frame capped the Metal game at ~29 FPS when the frame
                    // itself took ~17 ms. LT_FIXEDSLEEP=1 restores the flat sleep.
                    {
                        static const bool s_bFixed = (getenv("LT_FIXEDSLEEP") != 0);
                        static auto s_tPrev = std::chrono::steady_clock::now();
                        if (s_bFixed) usleep(16667);
                        else
                        {
                            const auto tNow = std::chrono::steady_clock::now();
                            const long us = (long)std::chrono::duration_cast<std::chrono::microseconds>(
                                                tNow - s_tPrev).count();
                            if (us < 16667) usleep((useconds_t)(16667 - us));
                        }
                        s_tPrev = std::chrono::steady_clock::now();
                    }
                }
            }

            // ⚠️ THE AUDIO DEVICE STOPS BEFORE ANYTHING IS TORN DOWN. Every
            // windowed run crashed on the way out (SIGSEGV or SIGBUS after the
            // last census line, never headless): CoreAudio's IO thread was
            // still calling CLTMixer::Render on a mixer that lives in this
            // function's client object and had already gone. lldb named it --
            // ltmixer.cpp:227, thread com.apple.audio.IOThread.client.
            // LTAudio_Stop existed and nothing called it.
            LTAudio_Stop();

            // The compositor lets go before the window closes: frames still on
            // the GPU are waited for and its layer comes off the view.
            LTMetal_CompShutdown();
            if (bWindow) LTWindow_Close();

            // ⚠️ TEAR THE SHELL DOWN. THE ENGINE OWES THE GAME THIS.
            //
            // iclientshell.h declares both halves of the contract --
            // OnEngineTerm() and DeleteClientShell() -- and the host called
            // neither, so the game never destroyed its interface, its controls
            // were never destructed, and every HSTRING they legitimately held
            // was still held at exit. The string manager reported 12 leaks,
            // which was TRUE and was the engine's fault, not the game's.
            //
            // ⚠️ A leak report that blames the wrong side is worse than none:
            // it sends the next person looking for a missing FreeString in
            // game code that is behaving correctly.
            // ⚠️ THE PLAYER'S SETTINGS, SAVED AT EXIT -- the reference's
            // SaveAutoConfig("autoexec.cfg") in CClientMgr's termination.
            // Before OnEngineTerm, while the game's state is still whole.
            LTConfig_Write("autoexec.cfg");

            printf("\ncalling OnEngineTerm()...\n");
            fflush(stdout);
            pShell->OnEngineTerm();

            DeleteClientShell(pShell);
            pShell = 0;
            printf("Update done  (stubs so far: %d, t=%.3fs)\n",
                   LTStub_Count(), LTGetClock()->GetTime());
            LTRecord_Event("host", "frames %d", nUpdates);
        }
    }

    // ⚠️ THE FIRST REAL FRAME.
    //
    // LT_DUMP_SCREEN=<path> writes the screen surface as a binary PPM. This is
    // what feeds harness/framediff.py -- written and validated against
    // synthetic images long before there was anything to diff, and dormant
    // ever since. Dump it, archive it, and the pixel half of the frame
    // contract starts working.
    {
        const char* pDump = getenv("LT_DUMP_SCREEN");
        if (pDump && pDump[0])
        {
            bool bOk = LTGetNullRender()->DumpScreenPPM(pDump);
            fprintf(stderr, "[LTDUMP] screen -> %s : %s\n",
                    pDump, bOk ? "written" : "FAILED");
        }
    }

    // The census is the deliverable: what the game asked the engine for.
    LTBind_DumpCensus();
    if (LTServerHost_Enabled()) LTServerPhys_DumpCensus();
    LTScene_DumpPhysCensus();
    LTScene_DumpModelCensus();

    if (g_nFrames3D)
    {
        const double fr = g_fFrameMs3D / g_nFrames3D;
        const double rc = LTScene_RenderCameraMs() / (LTScene_RenderCameraCalls() ? LTScene_RenderCameraCalls() : 1);
        printf("\n[FRAME] %u frame(s) with a 3D view: %.1f ms per frame (%.1f fps)\n"
               "        RenderCamera %.1f ms (%.0f%%), everything else %.1f ms\n",
               g_nFrames3D, fr, 1000.0 / fr, rc, 100.0 * rc / fr, fr - rc);
        fflush(stdout);
    }
    // ⚠️ DOES THE GAME KEEP UP WITH THE WORLD? The one number that answers
    // "is it slow, or is it in slow motion", and they are different faults with
    // different fixes. Wall time is what a person experiences; game time is
    // what the cinematic, the door and the AI think has passed. A ratio below
    // 100% is slow MOTION -- the frame cap in ltclock.cpp holding simulated
    // time back because frames cost more than the cap allows -- and no amount
    // of extra frames per second fixes it on its own.
    {
        const double fWall = LTClock_WallElapsed();
        const double fGame = LTGetClock()->GetTime();
        if (fWall > 0.5)
            printf("\n[CLOCK] %.1fs of wall time, %.1fs of game time "
                   "(%.0f%% of real speed)%s\n", fWall, fGame,
                   100.0 * fGame / fWall,
                   LTGetClock()->IsRealTime() ? "" : "  -- FIXED TIMESTEP, "
                   "this ratio is meaningless");
        if (LTServerHost_Enabled())
            printf("        the server ran %.1fs of it\n", LTServerHost_Time());
        fflush(stdout);
    }

    LTCommand_DumpCensus();
    if (g_pHostSound) g_pHostSound->DumpCensus();
    if (LTServerHost_Enabled()) LTServerHost_DumpTouchCensus();
    if (LTServerHost_Enabled()) LTServerHost_DumpClassCensus();
    LTReplicate_DumpCensus();
    LTStub_DumpCensus();
    LTFile_DumpCensus();
    LTGetNullRender()->DumpCensus();
    LTGetNullCursor()->DumpCensus();
    LTGetStringMgr()->DumpCensus();
    LTMessage_DumpStats();

    if (LTWindow_Wanted() || LTInput_GetNumDelivered())
        fprintf(stderr, "\n===== input =====\n"
                        "  delivered : %u\n  dropped   : %u\n"
                        "  unmapped keys: %u\n=================\n",
                LTInput_GetNumDelivered(), LTInput_GetNumDropped(),
                LTInput_GetNumUnmapped());

    // ⚠️ TOTALS LAST, and they are not redundant with the lines above. The
    // event lines say what happened once each; these say HOW MANY TIMES. A
    // retry loop that runs twice as often produces an identical event list and
    // a different count, and the counts are the only thing that would catch it.
    {
        CLTFileMgr* pFM = LTGetFileMgr();
        LTRecord_Event("count", "stubs %d",        LTStub_Count());
        LTRecord_Event("count", "files-opened %u", pFM->GetNumOpened());
        LTRecord_Event("count", "files-missing %u", pFM->GetNumNotFound());
        LTGetNullRender()->RecordTotals();
        LTGetNullCursor()->RecordTotals();
        LTGetStringMgr()->RecordTotals();
    }
    LTRecord_Close();

    printf("Interpretation: each line is an engine method NOLF 1 needs. They are\n"
           "listed in the order first reached, which is a dependency order for\n"
           "implementing the real engine.\n");

    return 0;
}
