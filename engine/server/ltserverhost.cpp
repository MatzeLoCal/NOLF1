// ----------------------------------------------------------------------- //
//
// MODULE  : ltserverhost.cpp
//
// See ltserverhost.h for why the server side is a dylib rather than a link.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltserverhost.h"
#include "ltserverobjects.h"
#include "ltserverphysics.h"
#include "ltserverbrush.h"
#include "ltmessage.h"
#include "ltreplicate.h"
#include "ltmathimpl.h"
#include "ltconsole.h"
#include "lt3d.h"
#include "ltconparse.h"
#include "iltcommon.h"
#include "ltstringmgr.h"
#include "ltstub.h"
#include "skeleton_generated.h"
#include "ltrecord.h"

#include "iltserver.h"
#include "iltmodel.h"
#include "ltanimtracker.h"
#include "iservershell.h"
#include "ltserverobj.h"
#include "ltworld.h"
#include "ltworldscene.h"
#include "ltworldvis.h"
#include "ltworldcollide.h"
#include "ltscene.h"
#include "ltworldrender.h"
#include <algorithm>
#include "ltfilemgr.h"
#include "iltstream.h"

#include <execinfo.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>        // _NSGetExecutablePath -- see DefaultPath
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include <set>

// See LTServerHost_SetWorldClosingHook.
static LTServerWorldClosingFn g_pWorldClosing = 0;

namespace
{
    // ⚠️ THE UNINITIALISED-OUT-PARAMETER FAMILY, SIXTH INSTANCE -- and the
    // first one on the server side, found within minutes of switching it on.
    //
    //   NetGame* pGameInfo;                                  // uninitialised
    //   uint32   dwLen = sizeof(NetGame);
    //   g_pLTServer->GetGameInfo((void**)&pGameInfo, &dwLen);
    //   if (pGameInfo)                                       // stack garbage
    //       memcpy(&m_GameInfo, pGameInfo, sizeof(NetGame)); // wild pointer
    //                                    -- GameServerShell.cpp:2769
    //
    // A stub that returns LT_ERROR without writing `*ppData` leaves a garbage
    // pointer, the guard passes because garbage is rarely zero, and the memcpy
    // reads from nowhere. The crash was in `_platform_memmove` with a
    // degenerate backtrace, and it VANISHED under AddressSanitizer -- that
    // stack slot happens to hold zero there, so the guard failed and the game
    // survived. A bug that disappears under the tool you would use to find it.
    //
    // ⚠️ The fix is not "return LT_OK": it is to WRITE THE OUTPUTS. There is no
    // NetGame in single player, and null is the truthful answer -- which is
    // exactly what the caller's own `if (pGameInfo)` is asking.
    LTRESULT LTHost_GetGameInfo(void** ppData, uint32* pLen)
    {
        if (ppData) *ppData = 0;
        if (pLen)   *pLen   = 0;
        return LT_ERROR;
    }

    // The class table and the live objects. Declared before the thunks that
    // reach it, which is the whole reason this file is ordered as it is.
    CLTServerObjects    g_Objects;

    // ⚠️ AND THE RECORD SHORTHAND WITH IT, not further down next to its first
    // user. Everything below reaches the engine's per-object state through
    // this, and a typedef introduced halfway down means every helper added
    // above that point fails to compile for a reason that reads like a missing
    // header. This file has now been reordered four times for exactly that.
    typedef CLTServerObjects::Record SRec;

    // ⚠️ THE TWO CALLS THAT TURN A CLASS TABLE INTO A GAME. `CreatePlayer` asks
    // for "CPlayerObj" and gives up the moment GetClass returns null, which is
    // why OnClientEnterWorld returned NULL with no complaint: the stub answered
    // honestly and the game believed it.
    HCLASS LTHost_GetClass(char* pName)
    {
        return (HCLASS)g_Objects.FindClass(pName);
    }

    // ⚠️ THE RUNTIME CREATION PATH, AND IT OWES THE SAME HANDSHAKE AS THE
    // WORLD FILE. `CreateObject` is not "allocate and hand back": the engine
    // drives MID_PRECREATE (with PRECREATE_NORMAL, because this one really IS
    // being created at runtime), lets the game rewrite the struct, and only
    // then takes the object's name, position and flags from it.
    //
    // ⚠️ WITHOUT THIS THE PLAYER HAD NO NAME. CGameServerShell::CreatePlayer
    // fills m_Name in during precreate, and five separate call sites then look
    // the player up with FindNamedObjects(DEFAULT_PLAYERNAME) -- the camera,
    // the intel items, the objectives. An unnamed player is findable by none
    // of them, and every one of those failures is silent.
    // ⚠️ LT_NEWTRACE=1 -- objects the GAME creates at runtime, as opposed to
    // the ones loaded from the world file. A game-side factory that fails
    // returns silently (CinematicTrigger::CreateKeyFramer: `if (!pKeyFramer)
    // return;`) and leaves the caller holding a null it never checks again.
    LPBASECLASS LTHost_CreateObject(HCLASS hClass, ObjectCreateStruct* pStruct)
    {
        if (getenv("LT_NEWTRACE"))
        {
            const ClassDef* pC = (const ClassDef*)hClass;
            printf("[NEW] game asks for %-18s name '%s'  t=%.3f\n",
                   (pC && pC->m_ClassName) ? pC->m_ClassName : "(no class)",
                   (pStruct && pStruct->m_Name[0]) ? pStruct->m_Name : "(none)",
                   LTServerHost_Time());
            fflush(stdout);
        }

        BaseClass* pObj = g_Objects.Create((ClassDef*)hClass, pStruct);
        if (!pObj)
        {
            if (getenv("LT_NEWTRACE"))
            { printf("[NEW]   -> REFUSED\n"); fflush(stdout); }
            return 0;
        }

        SRec* pRec = g_Objects.RecordOf(pObj);

        if (pStruct)
        {
            const uint32 nPre = pObj->EngineMessageFn(MID_PRECREATE, pStruct,
                                                      PRECREATE_NORMAL);
            if (!nPre)
            {
                // The game declined. Same rule as the world-file path.
                g_Objects.Destroy(pRec);
                return 0;
            }
            g_Objects.Apply(pRec, pStruct);
        }

        pObj->EngineMessageFn(MID_INITIALUPDATE, 0, INITIALUPDATE_NORMAL);
        LTServer_SendActivating(pObj);
        return (LPBASECLASS)pObj;
    }

    // ⚠️ CreateObjectProps -- CreateObject with a PROPERTY STRING. Was a stub,
    // and every prop-type ATTACHMENT goes through it (CAttachments::
    // CreatePropAttachment / CreateObjectAttachment): hats, the lily, the
    // cigarette, and the radio headset M01S01's cinematic Cate wears in the
    // Windows build. The stub returned null, the game returned early, and no
    // character ever wore anything.
    //
    // The string is LithTech's "Name value;Name value" form (Attachments.txt:
    // `Properties = "Chromakey 1.0"`). It is parsed into a temporary world
    // object and served through the SAME reader world-file properties use, for
    // the duration of MID_PRECREATE with PRECREATE_STRINGPROP -- which is the
    // flag the game's ReadProp paths test for.
    LPBASECLASS LTHost_CreateObjectProps(HCLASS hClass, ObjectCreateStruct* pStruct, char* pszProps)
    {
        LTIMPL("ILTServer::CreateObjectProps");

        LTWorldObject props;
        props.m_sType = (hClass && ((const ClassDef*)hClass)->m_ClassName)
                      ? ((const ClassDef*)hClass)->m_ClassName : "";

        // One property: every representation filled -- ReadProp hands the
        // game all of them and it picks the one it wants.
        auto addProp = [&props](const std::string& name, const std::string& val) {
            LTWorldProperty pr = LTWorldProperty();
            pr.m_sName   = name;
            pr.m_nType   = kLTWP_String;
            pr.m_sString = val;
            pr.m_fReal   = (float)atof(val.c_str());
            pr.m_nInt    = atoi(val.c_str());
            pr.m_bBool   = (pr.m_fReal != 0.0f) || (strcasecmp(val.c_str(), "true") == 0);
            pr.m_Vec[0] = pr.m_Vec[1] = pr.m_Vec[2] = 0.0f;
            sscanf(val.c_str(), "%f %f %f", &pr.m_Vec[0], &pr.m_Vec[1], &pr.m_Vec[2]);
            props.m_Props.push_back(pr);
            if (getenv("LT_PROPSTRACE"))
            { printf("[PROPS] %s: %s = '%s'\n", props.m_sType.c_str(), name.c_str(), val.c_str()); fflush(stdout); }
        };

        // ⚠️ THE ENGINE'S OWN PARSER, AS THE DONOR (serverde_impl.cpp
        // si_CreateObjectProps): ConParse -- ';' ends a property, ( ) and " "
        // group and are DROPPED -- the first token is the name, the rest joined
        // by single spaces is the value. A split at every ';' that strips only
        // quotes kept M04S02's spawned guards' death message as
        // "(msg backstagedoorunlock trigger)": not a command, sent to "bogus",
        // lost -- the stage door counts four deaths and only ever got two, so
        // Goodman never came on stage (user report, HANDOVER 5cx).
        // LT_OLDPROPPARSE=1 = the old split (says so).
        static const bool s_bOldParse = []() {
            const bool b = (getenv("LT_OLDPROPPARSE") != 0);
            if (b) { printf("[SERVER] LT_OLDPROPPARSE=1 -- CreateObjectProps splits at every ';' and keeps ( ) (old)\n"); fflush(stdout); }
            return b; }();
        if (pszProps && !s_bOldParse)
        {
            ConParse parse;
            parse.Init(pszProps);
            while (parse.Parse())
            {
                if (parse.m_nArgs < 1 || !parse.m_Args[0] || !parse.m_Args[0][0]) continue;
                std::string val;
                for (int i = 1; i < parse.m_nArgs; ++i)
                {
                    const size_t nAdd = strlen(parse.m_Args[i]) + (i != 1 ? 1 : 0);
                    if (val.size() + nAdd >= MAX_GP_STRING_LEN) break;     // the donor's limit
                    if (i != 1) val += ' ';
                    val += parse.m_Args[i];
                }
                addProp(parse.m_Args[0], val);
            }
        }
        else if (pszProps)
        {
            std::string all(pszProps);
            size_t a = 0;
            while (a <= all.size())
            {
                size_t b = all.find(';', a);
                if (b == std::string::npos) b = all.size();
                std::string one = all.substr(a, b - a);
                a = b + 1;

                size_t f = one.find_first_not_of(" \t");
                if (f == std::string::npos) continue;
                one = one.substr(f);
                size_t sp = one.find_first_of(" \t");
                std::string name = one.substr(0, sp);
                std::string val  = (sp == std::string::npos) ? "" : one.substr(sp);
                size_t vf = val.find_first_not_of(" \t");
                val = (vf == std::string::npos) ? "" : val.substr(vf);
                while (!val.empty() && (val[val.size()-1] == ' ' || val[val.size()-1] == '\t'))
                    val.erase(val.size() - 1);
                if (val.size() >= 2 && val[0] == '"' && val[val.size()-1] == '"')
                    val = val.substr(1, val.size() - 2);
                if (name.empty()) continue;
                addProp(name, val);
            }
        }

        BaseClass* pObj = g_Objects.Create((ClassDef*)hClass, pStruct);
        if (!pObj) return 0;
        SRec* pRec = g_Objects.RecordOf(pObj);

        if (pStruct)
        {
            const LTWorldObject* pOuter = g_Objects.SwapCurrentProps(&props);
            const uint32 nPre = pObj->EngineMessageFn(MID_PRECREATE, pStruct,
                                                      PRECREATE_STRINGPROP);
            g_Objects.SwapCurrentProps(pOuter);
            if (!nPre)
            {
                g_Objects.Destroy(pRec);
                return 0;
            }
            g_Objects.Apply(pRec, pStruct);
        }

        pObj->EngineMessageFn(MID_INITIALUPDATE, 0, INITIALUPDATE_NORMAL);
        LTServer_SendActivating(pObj);
        return (LPBASECLASS)pObj;
    }

    // ⚠️ THE PROPERTY CALLS. The game reads its own level data through the
    // ENGINE: inside MID_PRECREATE it asks for "Type", "Filename", "Pos" and
    // the rest by name. Without these every object is created with default
    // values and a level full of correctly-classed objects behaves like an
    // empty one.
    //
    // ⚠️ On a miss the output is left ALONE, never zeroed. The game seeds a
    // default before the call and reads the result unconditionally, so zeroing
    // destroys the fallback -- the same reasoning as GetSurfaceDims on the
    // client (HANDOVER.md §2).
    LTRESULT LTHost_GetPropGeneric(char* pName, GenericProp* pProp)
    {
        return g_Objects.ReadProp(pName, pProp) ? LT_OK : LT_NOTFOUND;
    }

    LTRESULT LTHost_GetPropString(char* pName, char* pRet, int maxLen)
    {
        if (!pRet || maxLen <= 0) return LT_ERROR;
        GenericProp gp;
        if (!g_Objects.ReadProp(pName, &gp)) return LT_NOTFOUND;
        strncpy(pRet, gp.m_String, (size_t)maxLen - 1);
        pRet[maxLen - 1] = '\0';
        return LT_OK;
    }

    // ----------------------------------------------------------------- //
    //
    // THE GAME CONSOLE
    //
    // ⚠️ A SECOND, SEPARATE VARIABLE STORE, and iltserver.h says why in its own
    // words: "This is a set of console variables and functions that is used
    // internally by the application DLLs. The user can't access this at all."
    // Sharing the user console would let `autoexec.cfg` and the game's private
    // globals collide under one namespace, silently.
    //
    // ⚠️ WHAT ITS ABSENCE COST: a SEGV in CMusicMgr::Update on M02S01, and
    // nothing at all resembling a music bug in the symptom. WorldProperties
    // reads the level's MusicControlFile and stores it here; CharacterMgr
    // reads it back and only then calls CMusicMgr::Init. With the store a
    // stub, Init never ran, every mood kept its zero intensity count, and
    // Update did `GetRandom(0, m_acMoods[iMood]-1)` with a count of zero --
    // 0xffffffff as an array index (MusicMgr.cpp:234). The register dump named
    // it: x0 = 0xffffffff.
    //
    // ⚠️ Three lines of storage sat between "the level loads" and "the level
    // crashes eight hundred frames later inside the music manager".
    //
    // ----------------------------------------------------------------- //
    CLTConsole g_GameConsole;

    // ⚠️ LT_NOGAMECONVARS=1 puts the store back to what it was -- a stub that
    // accepts writes and forgets them. Kept because "how many levels did this
    // one feature fix" is a question worth being able to ANSWER rather than
    // estimate, and because the same switch turns a future regression here
    // into a one-run bisection.
    bool LTHost_GameConVarsEnabled()
    {
        static const bool b = (getenv("LT_NOGAMECONVARS") == 0);
        return b;
    }

    LTServerSoundFn     g_pSoundFn      = 0;
    LTServerSoundDoneFn g_pSoundDoneFn  = 0;
    LTServerSoundDurFn  g_pSoundDurFn   = 0;
    LTServerSoundKillFn g_pSoundKillFn  = 0;
    LTServerCommandFn   g_pCommandFn    = 0;

    LTBOOL LTHost_ServerIsCommandOn(HCLIENT hClient, int nCommand)
    {
        (void)hClient;
        return (g_pCommandFn && g_pCommandFn(nCommand)) ? LTTRUE : LTFALSE;
    }
    LTServerSoundKillFn g_pSoundKillLoopFn = 0;

    LTRESULT LTHost_ServerPlaySound(PlaySoundInfo* pInfo)
    {
        if (!pInfo) return LT_ERROR;
        // LT_SOUNDTRACE=1 -- a sound asked for with NO NAME, once per object
        // it is attached to: which object keeps asking (the game only prints
        // "Couldn't play sound ''", with no owner).
        if (!pInfo->m_szSoundName[0] && getenv("LT_SOUNDTRACE"))
        {
            static std::set<const void*> s_Said;
            if (s_Said.insert(LTServerObjects_Updating() ? LTServerObjects_Updating() : (const void*)pInfo->m_hObject).second)
            {
                SRec* p = CLTServerObjects::FromHandle(pInfo->m_hObject);
                const SRec* u = (const SRec*)LTServerObjects_Updating();
                printf("[SOUND] t=%.2f empty sound name, flags 0x%x, object %s '%s', during the update of %s '%s'\n",
                       LTServerHost_Time(), (unsigned)pInfo->m_dwFlags,
                       (p && p->m_pClass) ? p->m_pClass->m_ClassName : "(none)", p ? p->m_sName.c_str() : "",
                       (u && u->m_pClass) ? u->m_pClass->m_ClassName : "(no update)", u ? u->m_sName.c_str() : "");
                fflush(stdout);
            }
        }
        if (!g_pSoundFn) { pInfo->m_hSound = 0; return LT_ERROR; }
        return g_pSoundFn(pInfo);
    }

    // ⚠️ "NOT KNOWN" IS SPELLED "DONE", not LT_ERROR -- see the header. With no
    // sound manager installed at all (a server-only run) the honest answer to
    // "has this line finished?" is yes: nothing is playing it.
    LTRESULT LTHost_ServerIsSoundDone(HLTSOUND h, LTBOOL* pbDone)
    {
        if (!pbDone) return LT_INVALIDPARAMS;
        if (!g_pSoundDoneFn) { *pbDone = LTTRUE; return LT_OK; }
        return g_pSoundDoneFn(h, pbDone);
    }

    LTRESULT LTHost_ServerSoundDuration(HLTSOUND h, LTFLOAT* pf)
    {
        if (!pf) return LT_INVALIDPARAMS;
        *pf = 0.0f;
        if (!g_pSoundDurFn) return LT_ERROR;
        return g_pSoundDurFn(h, pf);
    }

    LTRESULT LTHost_ServerKillSound(HLTSOUND h)
    {
        if (g_pSoundKillFn) g_pSoundKillFn(h);
        return LT_OK;
    }

    // Attachments -- defined beside the dynamic-object source, which places
    // attached objects; declared here because the wiring below names them.
    LTRESULT LTHost_CreateAttachment(HOBJECT hParent, HOBJECT hChild, char* pSocketName,
                                     LTVector* pOffset, LTRotation* pRotOffset,
                                     HATTACHMENT* pAttachment);
    LTRESULT LTHost_RemoveAttachment(HATTACHMENT hAttachment);
    LTRESULT LTHost_FindAttachment(HOBJECT hParent, HOBJECT hChild, HATTACHMENT* pAttachment);
    void     LTHost_ForgetFreedAttachments();

    LTRESULT LTHost_ServerKillSoundLoop(HLTSOUND h)
    {
        if (g_pSoundKillLoopFn) g_pSoundKillLoopFn(h);
        return LT_OK;
    }

    void LTHost_SetGameConVar(char* pName, char* pVal)
    {
        if (!pName || !*pName || !LTHost_GameConVarsEnabled()) return;
        g_GameConsole.Set(pName, pVal ? pVal : "");
    }

    HCONVAR LTHost_GetGameConVar(char* pName)
    {
        // ⚠️ NULL FOR AN UNKNOWN NAME, and the game relies on it -- every
        // caller tests the handle before reading it. Creating the variable on
        // a read would make "is this set?" always answer yes.
        return (HCONVAR)g_GameConsole.Find(pName);
    }

    void LTHost_RunGameConString(char* pString)
    {
        if (!pString) return;
        g_GameConsole.RunString(pString);
    }

    // ⚠️ THE TYPED PROPERTY READERS, AND THEIR ABSENCE WAS INVISIBLE. Only
    // GetPropGeneric and GetPropString were implemented; every object that
    // reads its level data through a TYPED getter was silently taking its
    // compiled-in default instead of what the level says.
    //
    // ⚠️ HOW THAT SURFACED: a keyframer path with no duration. `Key` reads its
    // TimeStamp with GetPropReal (Key.cpp:126), the stub left it at 0.0, every
    // key in every chain therefore sat at path time zero, and the keyframer
    // consumed its entire 22-key path in ONE update and declared itself
    // finished. The cinematic camera moved for about a tenth of a second.
    //
    // ⚠️ And it is not a keyframer problem. These five are read by objects all
    // over the game, so the class of bug is "the level said one thing and the
    // object did another", with nothing logged either side.
    //
    // ⚠️ ON A MISS THE OUTPUT IS LEFT ALONE, never zeroed -- the same rule
    // GetPropGeneric follows and for the same reason: the game seeds a default
    // before the call and reads the result unconditionally, so zeroing on a
    // miss destroys the fallback.
    bool LTHost_ZeroMoveDirDoor();
    LTRESULT LTHost_GetPropReal(char* pName, float* pRet)
    {
        GenericProp gp;
        if (!pRet || !g_Objects.ReadProp(pName, &gp)) return LT_NOTFOUND;
        *pRet = gp.m_Float;
        // See LTHost_ZeroMoveDirDoor: no direction, no distance.
        if (strcasecmp(pName, "MoveDist") == 0 && LTHost_ZeroMoveDirDoor()) *pRet = 0.0f;
        return LT_OK;
    }

    LTRESULT LTHost_GetPropBool(char* pName, LTBOOL* pRet)
    {
        GenericProp gp;
        if (!pRet || !g_Objects.ReadProp(pName, &gp)) return LT_NOTFOUND;
        *pRet = gp.m_Bool;
        return LT_OK;
    }

    LTRESULT LTHost_GetPropLongInt(char* pName, long* pRet)
    {
        GenericProp gp;
        if (!pRet || !g_Objects.ReadProp(pName, &gp)) return LT_NOTFOUND;
        *pRet = (long)gp.m_Long;
        return LT_OK;
    }

    LTRESULT LTHost_GetPropFlags(char* pName, uint32* pRet)
    {
        GenericProp gp;
        if (!pRet || !g_Objects.ReadProp(pName, &gp)) return LT_NOTFOUND;
        *pRet = (uint32)gp.m_Long;
        return LT_OK;
    }

    // ⚠️ A DOOR WHOSE MoveDir IS ZERO CANNOT MOVE -- AND ON WINDOWS IT FINISHES
    // AT ONCE (2026-10-07, HANDOVER 5bg). Door::InitialUpdate normalises
    // MoveDir with VEC_NORM; for (0,0,0) that is 0*inf = NaN, so the open
    // position is NaN. Door::Opening ends with `vNewPos == m_vOpenPos`, which
    // the SHIPPED Object.lto compiles as fcomp / `test ah,0x40` / je (address
    // 0x100c0b37): x87 sets C3 for an UNORDERED compare too, so NaN == NaN is
    // TRUE there and the door "arrives" on its first update -- SetOpen, its
    // OpenedCommand. Under IEEE (arm64) NaN != NaN and it never arrives.
    // That is every M01S02 bomb charge (Switch, MoveDir 0, MoveDist 0): E made
    // the "activate" sound and the charge was never disarmed (user report).
    // 7 of 8 M01S02 switches, ~80% of all Doors (mostly invisible blockers).
    // The engine reproduces the Windows OUTCOME without the NaN: such a door
    // gets a unit direction and a distance of 0, so it arrives on its first
    // update and does not travel -- also for the few authored with a distance
    // (on Windows they never travelled either: their path was NaN).
    // LT_NANDOORS=1 hands the game the raw zero vector (says so).
    unsigned g_nZeroMoveDirDoors = 0;
    bool LTHost_ZeroMoveDirDoor()
    {
        static const bool s_bOff = []() {
            const bool b = (getenv("LT_NANDOORS") != 0);
            if (b) { printf("[DOOR] LT_NANDOORS=1 -- zero MoveDir handed to the game as is (NaN doors)\n"); fflush(stdout); }
            return b; }();
        if (s_bOff) return false;
        GenericProp gp;
        if (!g_Objects.ReadProp((char*)"MoveDir", &gp)) return false;
        return gp.m_Vec.x == 0.0f && gp.m_Vec.y == 0.0f && gp.m_Vec.z == 0.0f;
    }

    LTRESULT LTHost_GetPropVector(char* pName, LTVector* pRet)
    {
        GenericProp gp;
        if (!pRet || !g_Objects.ReadProp(pName, &gp)) return LT_NOTFOUND;
        *pRet = gp.m_Vec;
        if (strcasecmp(pName, "MoveDir") == 0 && LTHost_ZeroMoveDirDoor())
        {
            pRet->Init(0.0f, 1.0f, 0.0f);
            ++g_nZeroMoveDirDoors;
        }
        return LT_OK;
    }

    // ⚠️ CONTAINERS -- LADDERS, WATER, DAMAGE VOLUMES -- AND ALL FOUR CALLS
    // WERE STUBS. CPlayerObj::UpdateInContainerState asks GetObjectContainers
    // every update and sends the list to CMoveMgr (MID_PHYSICS_UPDATE); with
    // "none" ever, UpdateContainerMotion never saw CC_LADDER: Cate could not
    // climb a ladder up or down (user report, HANDOVER 5bm), and no volume
    // ever held anything (water, lava, kill volumes: VolumeBrush asks
    // GetContainedObjects). Inside = the object's box overlaps the container
    // brush's CURRENT world box (the same box the touch pass uses). The donor
    // asks DoesBoxIntersectBSP; for the box-shaped volumes the levels are
    // built from it is the same answer. LT_NOCONTAINERS=1 = the stubs' answer
    // (says so); LT_CONTAINERTRACE=1 prints each change of a character's set.
    extern float g_fServerTime;
    static bool LTHost_ContainersOff()
    {
        static const bool b = []() {
            const bool o = (getenv("LT_NOCONTAINERS") != 0);
            if (o) { printf("[CONTAINER] LT_NOCONTAINERS=1 -- nothing is ever inside a container\n"); fflush(stdout); }
            return o; }();
        return b;
    }
    static void LTHost_ObjBox(const SRec* p, float* pMin, float* pMax)
    {
        for (int k = 0; k < 3; ++k)
        {
            if (p->m_bHasBox)
            {
                const float d0 = p->m_Pos[k] - p->m_BoxRestPos[k];
                pMin[k] = p->m_BoxMin[k] + d0; pMax[k] = p->m_BoxMax[k] + d0;
            }
            else { pMin[k] = p->m_Pos[k] - p->m_Dims[k]; pMax[k] = p->m_Pos[k] + p->m_Dims[k]; }
        }
    }
    static bool LTHost_BoxesTouch(const float* aMin, const float* aMax, const float* bMin, const float* bMax)
    {
        for (int k = 0; k < 3; ++k) if (aMin[k] > bMax[k] || aMax[k] < bMin[k]) return false;
        return true;
    }
    static bool LTHost_IsLiveContainer(const SRec* p)
    {
        return p && g_Objects.IsLive(p) && p->m_pGame && p->m_Type == OT_CONTAINER;
    }

    uint32 LTHost_GetObjectContainers(HOBJECT hObj, HOBJECT* pList, uint32* pFlagList, uint32 nMax)
    {
        SRec* pObj = CLTServerObjects::FromHandle(hObj);
        if (!pObj || !pList || LTHost_ContainersOff()) return 0;
        float oMin[3], oMax[3];
        LTHost_ObjBox(pObj, oMin, oMax);
        uint32 n = 0;
        const unsigned int nAll = g_Objects.ObjectCount();
        for (unsigned int i = 0; i < nAll && n < nMax; ++i)
        {
            SRec* c = g_Objects.RecordAt(i);
            if (c == pObj || !LTHost_IsLiveContainer(c)) continue;
            float cMin[3], cMax[3];
            LTHost_ObjBox(c, cMin, cMax);
            if (!LTHost_BoxesTouch(oMin, oMax, cMin, cMax)) continue;
            if (pFlagList) pFlagList[n] = 0;
            pList[n++] = (HOBJECT)c;
        }
        static const bool s_bTrace = (getenv("LT_CONTAINERTRACE") != 0);
        if (s_bTrace)
        {
            static std::map<SRec*, std::string> s_Last;
            std::string sNow;
            for (uint32 k = 0; k < n; ++k)
            {
                const SRec* c = (const SRec*)pList[k];
                char b[96];
                snprintf(b, sizeof b, "%s%s(code %u)", k ? ", " : "", c->m_sName.c_str(), (unsigned)c->m_nContainerCode);
                sNow += b;
            }
            if (s_Last[pObj] != sNow)
            {
                printf("[CONTAINER] t=%.2f %s '%s' in: %s\n", g_fServerTime,
                       pObj->m_pClass ? pObj->m_pClass->m_ClassName : "?", pObj->m_sName.c_str(),
                       sNow.empty() ? "(none)" : sNow.c_str());
                fflush(stdout);
                s_Last[pObj] = sNow;
            }
        }
        return n;
    }

    uint32 LTHost_GetContainedObjects(HOBJECT hContainer, HOBJECT* pList, uint32* pFlagList, uint32 nMax)
    {
        SRec* c = CLTServerObjects::FromHandle(hContainer);
        if (!pList || !LTHost_IsLiveContainer(c) || LTHost_ContainersOff()) return 0;
        float cMin[3], cMax[3];
        LTHost_ObjBox(c, cMin, cMax);
        uint32 n = 0;
        const unsigned int nAll = g_Objects.ObjectCount();
        for (unsigned int i = 0; i < nAll && n < nMax; ++i)
        {
            SRec* p = g_Objects.RecordAt(i);
            if (!p || p == c || !g_Objects.IsLive(p) || !p->m_pGame) continue;
            if (p->m_Type == OT_WORLDMODEL || p->m_Type == OT_CONTAINER) continue;
            float oMin[3], oMax[3];
            LTHost_ObjBox(p, oMin, oMax);
            if (!LTHost_BoxesTouch(oMin, oMax, cMin, cMax)) continue;
            if (pFlagList) pFlagList[n] = 0;
            pList[n++] = (HOBJECT)p;
        }
        return n;
    }

    uint32 LTHost_ServerPointContainers(const LTVector* pPoint, HOBJECT* pList, uint32 nMax)
    {
        if (!pPoint || !pList || LTHost_ContainersOff()) return 0;
        const float pt[3] = { pPoint->x, pPoint->y, pPoint->z };
        uint32 n = 0;
        const unsigned int nAll = g_Objects.ObjectCount();
        for (unsigned int i = 0; i < nAll && n < nMax; ++i)
        {
            SRec* c = g_Objects.RecordAt(i);
            if (!LTHost_IsLiveContainer(c)) continue;
            float cMin[3], cMax[3];
            LTHost_ObjBox(c, cMin, cMax);
            if (LTHost_BoxesTouch(pt, pt, cMin, cMax)) pList[n++] = (HOBJECT)c;
        }
        return n;
    }

    LTBOOL LTHost_ServerContainerCode(HOBJECT hObj, uint16* pCode)
    {
        SRec* c = CLTServerObjects::FromHandle(hObj);
        if (!pCode) return LTFALSE;
        if (!c || c->m_Type != OT_CONTAINER) { *pCode = 0; return LTFALSE; }
        *pCode = c->m_nContainerCode;
        return LTTRUE;
    }

    unsigned int LTHost_PointContainersRaw(const float* pt, void** pList, unsigned int nMax)
    {
        LTVector v; v.Init(pt[0], pt[1], pt[2]);
        return LTHost_ServerPointContainers(&v, (HOBJECT*)pList, nMax);
    }

    LTRESULT LTHost_GetPropColor(char* pName, LTVector* pRet)
    {
        GenericProp gp;
        if (!pRet || !g_Objects.ReadProp(pName, &gp)) return LT_NOTFOUND;
        *pRet = gp.m_Color;
        return LT_OK;
    }

    // ⚠️ EULER, and the world file stores it that way -- LTWorldProperty keeps
    // a rotation's three angles in m_Euler and GetPropGeneric packs them into
    // m_Rotation's first three floats. Reading them back in the same order is
    // what keeps a keyframed camera pointing where the level says.
    // ⚠️ The angles come from ReadPropEuler now: GenericProp::m_Rotation is a
    // real quaternion (see ReadProp), so reading "angles" out of it would be
    // the old bug inverted.
    LTRESULT LTHost_GetPropRotationEuler(char* pName, LTVector* pAngles)
    {
        float e[3];
        if (!pAngles || !g_Objects.ReadPropEuler(pName, e)) return LT_NOTFOUND;
        pAngles->Init(e[0], e[1], e[2]);
        return LT_OK;
    }

    LTRESULT LTHost_GetPropRotation(char* pName, LTRotation* pRet)
    {
        GenericProp gp;
        if (!pRet || !g_Objects.ReadProp(pName, &gp)) return LT_NOTFOUND;
        *pRet = gp.m_Rotation;          // already the quaternion
        return LT_OK;
    }

    // ⚠️ THE PER-OBJECT ACCESSORS. Every one of these was a stub that returned
    // LT_ERROR without touching its output, and the game reads them
    // unconditionally -- which is why the first AI object crashed immediately
    // after GetObjectUserFlags. State the engine owns has to live somewhere;
    // CLTServerObjects::Record is where.

    // ⚠️ REMOVAL IS A MARK, NOT A FREE. See CLTServerObjects::Record::m_bDead:
    // the game calls this from inside an object's own MID_UPDATE while the
    // update loop is walking the list by index, and from inside another
    // object's destructor. Freeing at the call site shifts the loop's index or
    // deletes ground the caller is standing on.
    //
    // ⚠️ AND IT IS NOT COSMETIC THAT THIS WAS MISSING. A keyframer copies each
    // key into its own list and then removes the key from the world; with a
    // stub the 37 keys of M01S01 stayed live forever. One pass survives that;
    // a looping or re-triggered keyframer walks a world that still has its old
    // keys in it.
    // ⚠️ WHEN AN OBJECT DIES, AND WHAT IT WAS. A level's script is written in
    // object lifetimes -- a CinematicTrigger that has finished REMOVES ITSELF
    // (CinematicTrigger.cpp:1012), so "did the cinematic end?" is not a
    // question about rendering, it is a question about whether one object went
    // away and at what time. Off unless asked for; one line per removal.
    bool LTHost_LifeTracing()
    {
        static const bool b = (getenv("LT_LIFETRACE") != 0);
        return b;
    }

    LTRESULT LTHost_RemoveObject(HOBJECT h)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) return LT_ERROR;

        if (LTHost_LifeTracing() && g_Objects.IsLive(p))
        {
            printf("[LIFE] t=%7.3f  removed %-24s %s\n",
                   LTServerHost_Time(),
                   (p->m_pClass && p->m_pClass->m_ClassName)
                       ? p->m_pClass->m_ClassName : "(no class)",
                   p->m_sName.empty() ? "" : p->m_sName.c_str());
            fflush(stdout);
        }

        return g_Objects.MarkDead(p) ? LT_OK : LT_ERROR;
    }

    // Inter-object links -- see CLTServerObjects::CreateLink.
    // LT_NOLINKS=1 restores the old stubs (no links, no MID_LINKBROKEN).
    bool LTHost_NoLinks()
    {
        static const bool b = (getenv("LT_NOLINKS") != 0);
        static const bool bSaid = (b ? (printf("[SERVER] LT_NOLINKS=1 -- inter-object "
                                               "links are NOT kept; no MID_LINKBROKEN\n"), true) : true);
        (void)bSaid;
        return b;
    }

    LTRESULT LTHost_CreateInterObjectLink(HOBJECT hOwner, HOBJECT hLinked)
    {
        LTIMPL("ILTServer::CreateInterObjectLink");
        if (LTHost_NoLinks()) return LT_ERROR;
        SRec* pO = CLTServerObjects::FromHandle(hOwner);
        SRec* pL = CLTServerObjects::FromHandle(hLinked);
        const bool bOK = g_Objects.CreateLink(pO, pL);
        // LT_LIFETRACE=1 also names every link made or refused (5cf).
        if (LTHost_LifeTracing())
        {
            const bool bKnownO = pO && g_Objects.Contains(pO);
            const bool bKnownL = pL && g_Objects.Contains(pL);
            // An unnamed owner (a sense, a group, a hit box) shows its class
            // and address; so does an unknown target (5cu).
            char szO[160], szL[160];
            if (bKnownO && !pO->m_sName.empty()) snprintf(szO, sizeof(szO), "%s", pO->m_sName.c_str());
            else snprintf(szO, sizeof(szO), "%s@%p", bKnownO ? ((pO->m_pClass && pO->m_pClass->m_ClassName)
                          ? pO->m_pClass->m_ClassName : "?") : "(not an object)", (void*)pO);
            if (bKnownL) snprintf(szL, sizeof(szL), "%s", pL->m_sName.c_str());
            else
            {
                // Not a record: perhaps a GAME object passed for its handle.
                const SRec* pAsGame = pL ? g_Objects.RecordOf((const BaseClass*)pL) : 0;
                snprintf(szL, sizeof(szL), "(not an object)@%p, was %s%s%s", (void*)pL,
                         pL ? g_Objects.FreedName(pL).c_str() : "null",
                         pAsGame ? "; it is the GAME object of " : "",
                         pAsGame ? pAsGame->m_sName.c_str() : "");
            }
            printf("[LIFE] t=%7.3f  link %s -> %s%s\n", LTServerHost_Time(), szO, szL,
                   bOK ? "" : "   REFUSED");
            // LT_SAVEREFTRACE=1 -- who asks to link to something that is not an object.
            static const bool s_bWho = (getenv("LT_SAVEREFTRACE") != 0);
            if (s_bWho && !bOK && bKnownO && pL && !bKnownL)
            {
                fflush(stdout);
                void* frames[8];
                const int n = backtrace(frames, 8);
                backtrace_symbols_fd(frames + 1, n - 1, 1);
            }
            fflush(stdout);
        }
        return bOK ? LT_OK : LT_ERROR;
    }

    void LTHost_BreakInterObjectLink(HOBJECT hOwner, HOBJECT hLinked)
    {
        LTIMPL("ILTServer::BreakInterObjectLink");
        SRec* pO = CLTServerObjects::FromHandle(hOwner);
        SRec* pL = CLTServerObjects::FromHandle(hLinked);
        if (LTHost_LifeTracing())
        {
            printf("[LIFE] t=%7.3f  unlink %s -> %s\n", LTServerHost_Time(),
                   (pO && g_Objects.Contains(pO)) ? pO->m_sName.c_str() : "(not an object)",
                   (pL && g_Objects.Contains(pL)) ? pL->m_sName.c_str() : "(not an object)");
            fflush(stdout);
        }
        g_Objects.BreakLink(pO, pL);
    }

    // ⚠️ THE COLLISION A TOUCH NOTIFICATION IS ABOUT (5dn) -- the donor's
    // g_pServerMgr->m_pCollisionInfo: set by the mover just before it sends
    // MID_TOUCHNOTIFY, null otherwise, and ILTServer::GetLastCollision copies
    // it out. GetLastCollision was a STUB that wrote nothing, and every game
    // caller reads a bare local: the crossbow bolt (CSpear::HandleImpact)
    // passed its m_hObject -- stack garbage -- to GetSurfaceType, which read
    // the user flags of a pointer to nowhere. SIGSEGV on the first bolt that
    // touched a character.
    CollisionInfo g_TouchColl;
    bool          g_bTouchColl = false;

    // The contact of A touching B, from their boxes: B is the object, no
    // polygon (the SDK: "otherwise it is equal to LTNULL"), the plane faces A
    // through B's box point nearest A's centre, nothing to stop. The game reads
    // the plane only for world hits, which this pass never makes.
    void LTHost_SetTouchCollision(const float* aMin, const float* aMax,
                                  const float* bMin, const float* bMax, void* hB)
    {
        float ca[3], q[3], n[3];
        for (int k = 0; k < 3; ++k)
        {
            ca[k] = 0.5f * (aMin[k] + aMax[k]);
            q[k] = ca[k] < bMin[k] ? bMin[k] : (ca[k] > bMax[k] ? bMax[k] : ca[k]);
            n[k] = ca[k] - 0.5f * (bMin[k] + bMax[k]);
        }
        const float m = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (m > 0.0001f) { for (int k = 0; k < 3; ++k) n[k] /= m; }
        else             { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
        memset(&g_TouchColl, 0, sizeof(g_TouchColl));
        g_TouchColl.m_Plane.m_Normal.Init(n[0], n[1], n[2]);
        g_TouchColl.m_Plane.m_Dist = n[0]*q[0] + n[1]*q[1] + n[2]*q[2];
        g_TouchColl.m_hObject = (HOBJECT)hB;
        g_bTouchColl = true;
    }

    // ⚠️ ZEROED WHEN THERE IS NO COLLISION, where the donor writes nothing:
    // the callers read a bare local regardless, and a zero m_hObject / m_hPoly
    // is what GetSurfaceType skips. LT_OLDLASTCOLLISION=1 = the stub (says so).
    LTRESULT LTHost_GetLastCollision(CollisionInfo* pInfo)
    {
        static const bool s_bOld = []() {
            const bool b = (getenv("LT_OLDLASTCOLLISION") != 0);
            if (b) { printf("[TOUCH] LT_OLDLASTCOLLISION=1 -- GetLastCollision writes nothing (old: the crossbow crashed)\n"); fflush(stdout); }
            return b; }();
        if (s_bOld) return LT_ERROR;
        LTIMPL("ILTServer::GetLastCollision");
        if (!pInfo) return LT_INVALIDPARAMS;
        if (!g_bTouchColl) { memset(pInfo, 0, sizeof(*pInfo)); return LT_ERROR; }
        *pInfo = g_TouchColl;
        return LT_OK;
    }

    LPBASECLASS LTHost_HandleToObject(HOBJECT h)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        return p ? (LPBASECLASS)p->m_pGame : 0;
    }

    HOBJECT LTHost_ObjectToHandle(LPBASECLASS pObj)
    {
        return pObj ? ((BaseClass*)pObj)->m_hObject : 0;
    }

    // ⚠️ `void` return, so the CALLER CANNOT TELL IT FAILED -- which makes
    // writing the output on every path mandatory rather than polite. A bad
    // handle yields the origin, not the caller's stack.
    void LTHost_GetObjectPos(HOBJECT h, LTVector* pPos)
    {
        if (!pPos) return;
        SRec* p = CLTServerObjects::FromHandle(h);
        if (p) pPos->Init(p->m_Pos[0], p->m_Pos[1], p->m_Pos[2]);
        else   pPos->Init(0.0f, 0.0f, 0.0f);
    }

    // LT_PROPTRACE=<name part> -- a matching object's dims and every position
    // the game sets, on the server (says so). For "is the prop where the game
    // put it": the floor drop, scale and rotation questions start here.
    static const char* LTHost_PropTraceFilter()
    {
        static const char* s = []() -> const char* {
            const char* p = getenv("LT_PROPTRACE");
            if (p && p[0]) { printf("[PROP] LT_PROPTRACE=%s -- server dims and positions of matching objects\n", p); fflush(stdout); }
            return (p && p[0]) ? p : 0; }();
        return s;
    }
    static bool LTHost_PropTraced(const SRec* p)
    {
        const char* f = LTHost_PropTraceFilter();
        return f && p && strcasestr(p->m_sName.c_str(), f) != 0;
    }

    // ⚠️ A POSITION THAT IS NOT A NUMBER IS REFUSED (5dl). NOLF's dodge roll
    // reads ONE PAST the end of its distance table on the roll's last frame
    // (AIMovement.cpp GetMovementData: index fTime*70+1 at fTime 1.0, weight
    // 0). It is harmless while that float is a number, and in this build the
    // float after s_afRollRight is 0xFFFFFFFF -- a NaN -- so 0 x NaN made the
    // last step of every RIGHT roll NaN, the AI was teleported to NaN and
    // vanished: unkillable, and M04S02's stage fight never ended (Goodman
    // never came). The object stays where it was -- the roll's last step is
    // a fraction of a unit. Every refusal is printed (it should never
    // happen); LT_ALLOWNANPOS=1 = the old acceptance (says so).
    bool LTHost_PosRefused(const SRec* p, const LTVector* pPos, const char* pWhat)
    {
        if (isfinite(pPos->x) && isfinite(pPos->y) && isfinite(pPos->z)) return false;
        static const bool s_bAllow = []() {
            const bool b = (getenv("LT_ALLOWNANPOS") != 0);
            if (b) { printf("[POS] LT_ALLOWNANPOS=1 -- non-finite positions accepted (old)\n"); fflush(stdout); }
            return b; }();
        if (s_bAllow) return false;
        static unsigned int s_n = 0;
        if (s_n++ < 20)
        {
            printf("[POS] t=%.3f refused a non-finite %s for %s '%s' -- it stays at (%.1f %.1f %.1f)\n",
                   g_fServerTime, pWhat, p->m_pClass ? p->m_pClass->m_ClassName : "?",
                   p->m_sName.c_str(), p->m_Pos[0], p->m_Pos[1], p->m_Pos[2]);
            fflush(stdout);
        }
        return true;
    }

    void LTHost_SetObjectPos(HOBJECT h, LTVector* pPos)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p || !pPos) return;
        if (LTHost_PosRefused(p, pPos, "SetObjectPos")) return;
        if (LTHost_PropTraced(p))
            printf("[PROP] %-12s pos (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)  dims (%.1f %.1f %.1f)\n",
                   p->m_sName.c_str(), p->m_Pos[0], p->m_Pos[1], p->m_Pos[2],
                   pPos->x, pPos->y, pPos->z, p->m_Dims[0], p->m_Dims[1], p->m_Dims[2]);
        p->m_Pos[0] = pPos->x; p->m_Pos[1] = pPos->y; p->m_Pos[2] = pPos->z;
    }

    LTRESULT LTHost_GetObjectRotation(HOBJECT h, LTRotation* pRot)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p || !pRot) return LT_ERROR;
        pRot->Init(p->m_Rot[0], p->m_Rot[1], p->m_Rot[2], p->m_Rot[3]);
        return LT_OK;
    }

    LTRESULT LTHost_SetObjectRotation(HOBJECT h, LTRotation* pRot)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p || !pRot) return LT_ERROR;
        p->m_Rot[0] = pRot->m_Quat[0]; p->m_Rot[1] = pRot->m_Quat[1];
        p->m_Rot[2] = pRot->m_Quat[2]; p->m_Rot[3] = pRot->m_Quat[3];
        return LT_OK;
    }

    // ⚠️ ROTATE AND MOVE -- THE PHYSICS-FLAVOURED PAIR, and what they do NOT
    // do is the part worth writing down.
    //
    // In the original these go through the mover: MoveObject pushes what is in
    // the way, slides along surfaces, and can refuse. RotateObject does the
    // same for turning. Here they set the transform and nothing else.
    //
    // ⚠️ THAT IS HONEST FOR WHAT USES THEM TODAY AND NOT FOR EVERYTHING. A
    // KEYFRAMER is the caller that matters -- 364 of them across 49 worlds, and
    // a cinematic camera is one -- and a camera on rails has nothing to push.
    // A keyframed DOOR closing on the player does, and it will pass through
    // instead. Recorded rather than hidden, because the day that matters the
    // symptom will be "the crusher does not crush" and the cause will be here.
    //
    // ⚠️ RotateObject was found by running, not by reading: the keyframer
    // reached it the moment its object list started working, and it had been
    // a stub the whole time -- so every keyframed object would have moved
    // along its path without ever turning.
    LTRESULT LTHost_RotateObject(HOBJECT h, LTRotation* pRot)
    {
        return LTHost_SetObjectRotation(h, pRot);
    }

    // ⚠️ STILL SETS THE TRANSFORM, DELIBERATELY, EVEN NOW THAT COLLISION
    // EXISTS (2026-09-14). This is the OBSOLETE ILTServer::MoveObject, and its
    // one caller in the game is `KeyFramer::UpdateObjects` -- and only on the
    // `m_bPushObjects` branch, which means "carry whatever is in the way", not
    // "stop when something is". This port cannot push, so colliding here would
    // turn a lift that passes through the player into a lift that JAMS on him
    // and strands the level. Passing through is the lesser wrong answer until
    // there is object-vs-object physics.
    //
    // ⚠️ `ILTPhysics::MoveObject` -- the modern one CMoveMgr uses -- DOES
    // collide. Two calls, two behaviours, on purpose.
    // ⚠️ NO COLLISION, BY DEFINITION -- see ltserverphysics.h. "Teleport" is
    // the game saying it knows where the object belongs.
    // ⚠️ THE GAME'S RAY. See LTServerPhys_RayTest for why an object handle,
    // not just a surface, is what makes E open a door.
    LTRESULT LTHost_AnimUserDims(HOBJECT h, LTVector* pDims, HMODELANIM hAnim);
    void     LTHost_GetObjectDims(HOBJECT h, LTVector* pDims);
    SRec*    LTHost_FindPlayerRecord();

    // E-011 (5dq): while a bolt's touch on a door is delivered, THIS polygon's
    // "Wood Door" (100, hardness 0.65) reads as "Wood" (30, 0.5) -- see
    // LTServerHost_WorldTouch. 0 = no override.
    unsigned int g_nBoltDoorPoly = 0;
    const uint32 kSurfWoodDoor = 100, kSurfWood = 30;    // Attributes\Surface.txt

    // Shared by ILTServer, ILTCommon and the client: polygon -> surface type.
    LTRESULT LTHost_PolyTextureFlags(unsigned int nPolyId, uint32* pFlags)
    {
        if (!pFlags) return LT_ERROR;
        *pFlags = 0;                                    // ST_UNKNOWN, on every path
        unsigned int n = 0;
        CLTWorldScene* pScene = LTGetWorldScene();
        if (!pScene || !pScene->PolySurfaceType(nPolyId, &n)) return LT_ERROR;
        *pFlags = n;
        if (g_nBoltDoorPoly && nPolyId == g_nBoltDoorPoly && n == kSurfWoodDoor) *pFlags = kSurfWood;
        return LT_OK;
    }

    // ⚠️ THE MAIN WORLD IS AN OBJECT (2026-10-07, HANDOVER 5bg). In LithTech a
    // ray that hits level geometry reports the WORLD OBJECT -- a real, non-null
    // handle that IsWorldObject recognises. The port reported NULL, and NULL
    // already means "no object" to the game: CProjectile::DoVector starts with
    // `hLastHitbox = LTNULL` and stops a shot whose hit equals it
    // ("vector intersected character hitbox twice", Projectile.cpp:1166). So
    // EVERY bullet whose first hit was a wall was dropped -- no impact, no
    // shoot-through test -- which is most of what M01S01's cleaners fire
    // through the street-level room's door and walls ("the cleaners don't
    // attack", user report). One zero-initialised record stands for the world:
    // not in the object list, no game object, no class -- HandleToObject and
    // GetObjectClass answer null for it, as for the engine's own world object.
    // LT_NULLWORLDHIT=1 restores NULL (says so).
    SRec* LTHost_WorldRecord()
    {
        static SRec* s_p = []() {
            SRec* p = new SRec();
            p->m_Type  = OT_WORLDMODEL;
            p->m_Flags = FLAG_SOLID | FLAG_VISIBLE;
            p->m_fMass = 100000.0f;
            p->m_Color[0] = p->m_Color[1] = p->m_Color[2] = p->m_Color[3] = 255;
            return p; }();
        return s_p;
    }

    bool LTHost_NullWorldHits()
    {
        static const bool b = []() {
            const bool v = (getenv("LT_NULLWORLDHIT") != 0);
            if (v) { printf("[PHYS] LT_NULLWORLDHIT=1 -- world hits report a NULL object\n"); fflush(stdout); }
            return v; }();
        return b;
    }

    LTBOOL LTHost_IntersectSegment(IntersectQuery* pQuery, IntersectInfo* pInfo)
    {
        if (!pQuery || !pInfo) return LTFALSE;

        // ⚠️ Cleared FIRST and on every path. IntersectInfo has a constructor,
        // but the game reuses one across several casts and reads m_hObject
        // whatever the return says.
        pInfo->m_Point.Init();
        pInfo->m_Plane.m_Normal.Init();
        pInfo->m_Plane.m_Dist = 0.0f;
        pInfo->m_hObject = LTNULL;
        pInfo->m_hPoly = INVALID_HPOLY;
        pInfo->m_SurfaceFlags = 0;

        const float from[3] = { pQuery->m_From.x, pQuery->m_From.y, pQuery->m_From.z };
        const float to[3]   = { pQuery->m_To.x,   pQuery->m_To.y,   pQuery->m_To.z };

        float pt[3], nrm[3];
        void* hHit = 0;
        unsigned int nPolyId = 0;

        // ⚠️ THE QUERY FLAGS GO THROUGH. They were dropped here, so every
        // ray hit trigger volumes (IGNORE_NONSOLID ignored) and every ray
        // tested objects whether it asked to or not (INTERSECT_OBJECTS).
        const bool bHitAny = LTServerPhys_RayTestFiltered(from, to,
                                  (bool (*)(void*, void*))pQuery->m_FilterFn,
                                  pQuery->m_pUserData,
                                  (bool (*)(unsigned int, void*))pQuery->m_PolyFilterFn,
                                  pt, nrm, &hHit,
                                  (pQuery->m_Flags & INTERSECT_OBJECTS) != 0,
                                  (pQuery->m_Flags & IGNORE_NONSOLID) != 0,
                                  &nPolyId);

        // ⚠️ LT_SHOTTRACE=1 -- LONG rays (weapons, AI sight), and what they hit.
        // "The shot did nothing" is three different faults: it hit the world,
        // it hit nothing, or it hit a character and the damage went nowhere.
        if (getenv("LT_SHOTTRACE"))
        {
            const float dx = to[0]-from[0], dy = to[1]-from[1], dz = to[2]-from[2];
            const float len = sqrtf(dx*dx + dy*dy + dz*dz);
            // LT_SHOTTRACE_MIN=<units> lowers the length filter -- the
            // shoot-through test is a ~60-unit ray cast BACKWARDS.
            static const float s_fMin = getenv("LT_SHOTTRACE_MIN") ? (float)atof(getenv("LT_SHOTTRACE_MIN")) : 1000.0f;
            if (len >= s_fMin)
            {
                const SRec* h = (const SRec*)hHit;
                const float hx = pt[0]-from[0], hy = pt[1]-from[1], hz = pt[2]-from[2];
                uint32 nST = 0;
                if (bHitAny && nPolyId) LTHost_PolyTextureFlags(nPolyId, &nST);
                printf("[SHOT] surface %u  ", (unsigned)nST);
                printf("t=%.3f  %.0f-unit ray from (%.0f %.0f %.0f) objs=%d -> %s %s%s%s at %.0f\n",
                       LTServerHost_Time(), len, from[0], from[1], from[2],
                       (pQuery->m_Flags & INTERSECT_OBJECTS) ? 1 : 0,
                       bHitAny ? "HIT" : "nothing",
                       (bHitAny && h && h->m_pClass) ? h->m_pClass->m_ClassName : (bHitAny ? "world" : ""),
                       (bHitAny && h && !h->m_sName.empty()) ? " " : "",
                       (bHitAny && h) ? h->m_sName.c_str() : "",
                       bHitAny ? sqrtf(hx*hx + hy*hy + hz*hz) : 0.0f);
                fflush(stdout);
            }
        }
        if (!bHitAny) return LTFALSE;

        // ⚠️ LT_RAYTRACE=1 -- SHORT rays, with the server clock. This exists to
        // answer one question: did CPlayerObj::Activate get PAST its early
        // outs? It casts a short ray to find what the player is pointing at
        // (PlayerObj.cpp, after the cinematic check), so a press that produces
        // no short ray returned before that -- at the bute manager, the player
        // state, or the cinematic. A press that produces one did not.
        if (getenv("LT_RAYTRACE"))
        {
            const float dx = to[0]-from[0], dy = to[1]-from[1], dz = to[2]-from[2];
            const float len = sqrtf(dx*dx + dy*dy + dz*dz);
            if (len > 0.0f && len < 200.0f)
            {
                printf("[RAY] t=%.3f  %.0f units from (%.0f %.0f %.0f)\n",
                       LTServerHost_Time(), len, from[0], from[1], from[2]);
                fflush(stdout);
            }
        }

        // LT_TELETRACE=1 -- the long downward cast MoveObjectToFloor makes.
        // The player arriving at y=0.1 after a teleport is this ray's answer
        // plus the game's own "+ dims.y + 0.1", so both halves are printed.
        if (getenv("LT_TELETRACE") && (from[1] - to[1]) > 5000.0f)
        {
            printf("[TELE] floor cast from (%.1f %.1f %.1f) -> hit (%.1f %.1f %.1f) "
                   "obj=%p\n", from[0], from[1], from[2], pt[0], pt[1], pt[2], hHit);
            fflush(stdout);
        }

        pInfo->m_Point.Init(pt[0], pt[1], pt[2]);
        pInfo->m_Plane.m_Normal.Init(nrm[0], nrm[1], nrm[2]);
        pInfo->m_Plane.m_Dist =
            nrm[0]*pt[0] + nrm[1]*pt[1] + nrm[2]*pt[2];

        // ⚠️ A WORLD HIT REPORTS THE WORLD OBJECT, not null. `IsMainWorld`
        // (PlayerObj.cpp:3174) branches on it -- and `IsWorldObject` below
        // answers YES to a null handle for exactly this reason. Inventing a
        // fake handle instead would put a pointer into the game that
        // FromHandle would later try to read back as a Record.
        pInfo->m_hObject = (HOBJECT)hHit;
        if (!hHit && !LTHost_NullWorldHits()) pInfo->m_hObject = (HOBJECT)LTHost_WorldRecord();
        // ⚠️ THE POLYGON, so the game can ask its SURFACE TYPE (shoot-through
        // Invisible.dtx, the impact effect, ...). See LTHost_GetPolyTextureFlags.
        pInfo->m_hPoly = nPolyId ? (HPOLY)nPolyId : INVALID_HPOLY;
        return LTTRUE;
    }

    // ⚠️ THE OTHER HALF OF THE GAME'S VOICE. BPrint is a FUNCTION POINTER on
    // ILTServer while CPrint is a virtual on ILTCSBase -- the hybrid-interface
    // split again (HANDOVER.md §4.5), and the reason these two look like one
    // feature but need two separate implementations in two separate places.
    //
    // ⚠️ NOLF's own level script uses this: a Key with a BPRINT_KEY type calls
    // it by name (KeyFramer::ProcessCurrentKey), so a designer's note left in a
    // cinematic comes out here.
    void LTHost_BPrint(char* pMsg, ...)
    {
        static const bool bQuiet = (getenv("LT_NOGAMEPRINT") != 0);
        if (bQuiet || !pMsg) return;

        char szBuf[1024];
        va_list args;
        va_start(args, pMsg);
        vsnprintf(szBuf, sizeof(szBuf), pMsg, args);
        va_end(args);

        printf("[GAME/B] %s\n", szBuf);
        fflush(stdout);
    }

    LTRESULT LTHost_TeleportObject(HOBJECT h, LTVector* pPos)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p || !pPos) return LT_ERROR;
        if (LTHost_PosRefused(p, pPos, "TeleportObject")) return LT_ERROR;
        const float to[3] = { pPos->x, pPos->y, pPos->z };
        if (getenv("LT_TELETRACE"))
            printf("[TELE] %s '%s' -> (%.1f %.1f %.1f)  dims (%.1f %.1f %.1f)\n",
                   p->m_pClass ? p->m_pClass->m_ClassName : "?",
                   p->m_sName.c_str(), to[0], to[1], to[2],
                   p->m_Dims[0], p->m_Dims[1], p->m_Dims[2]);
        // LT_AIJUMPTRACE=1 -- an AI carried more than 30 units sideways by ONE
        // teleport. NOLF's humans move by "cheap movement": every step is a
        // TeleportObject with no collision, so a step that size is how one
        // leaves through a wall (5dl). Prints the animation it is playing,
        // its time and length -- a dodge roll's distance comes from those.
        static const bool s_bJump = (getenv("LT_AIJUMPTRACE") != 0);
        if (s_bJump && p->m_pClass && strncmp(p->m_pClass->m_ClassName, "AI_", 3) == 0)
        {
            const float dx = to[0] - p->m_Pos[0], dz = to[2] - p->m_Pos[2];
            const float d = sqrtf(dx*dx + dz*dz);
            if (!(d <= 30.0f))          // NaN too: the roll's bad step is one
            {
                printf("[AIJUMP] t=%.3f %s '%s' %.1f units: (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)  anim %d at %.0f of %d ms%s\n",
                       g_fServerTime, p->m_pClass->m_ClassName, p->m_sName.c_str(), d,
                       p->m_Pos[0], p->m_Pos[1], p->m_Pos[2], to[0], to[1], to[2], p->m_nAnim,
                       (g_fServerTime - p->m_fAnimStart) * 1000.0f,
                       LTScene_GetAnimLengthForFile(p->m_sFilename.c_str(), p->m_nAnim),
                       p->m_bAnimLooping ? " looping" : "");
                fflush(stdout);
            }
        }
        LTServerPhys_TeleportTo((void*)h, to);
        return LT_OK;
    }

    LTRESULT LTHost_MoveObject(HOBJECT h, LTVector* pPos)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p || !pPos) return LT_ERROR;
        if (LTHost_PosRefused(p, pPos, "MoveObject")) return LT_ERROR;
        // LT_MOVETRACE=<name> -- every obsolete MoveObject on that object:
        // where it was, where it is asked to go. A door that never "arrives"
        // shows as a call every frame.
        if (const char* pW = getenv("LT_MOVETRACE"))
            if (*pW && p->m_sName == pW)
            {
                static int s_n = 0;
                if (s_n++ < 30)
                    printf("[MOVE] t=%.3f %s (%.3f %.3f %.3f) -> (%.3f %.3f %.3f)\n", LTServerHost_Time(),
                           p->m_sName.c_str(), p->m_Pos[0], p->m_Pos[1], p->m_Pos[2], pPos->x, pPos->y, pPos->z);
            }
        LTHost_SetObjectPos(h, pPos);
        return LT_OK;
    }

    // ⚠️ This RETURNS the flags -- it has no out-parameter, which is worth
    // recording because the first guess was that it did. Reading the header
    // beat reasoning from the crash site: the AI's problem was a stub answering
    // 0 for flags it had just set, not an unwritten output.
    uint32 LTHost_GetObjectUserFlags(HOBJECT h)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        return p ? p->m_UserFlags : 0;
    }

    // ⚠️ THE OBJECT'S COLOUR (HANDOVER 5cf) -- serverde_impl.cpp's
    // si_Get/SetObjectColor: stored as bytes, `(uint8)(x * 255)`, and read back
    // as byte/255, so the game sees the same quantised values it saw on
    // Windows (TranslucentWorldModel's fade stops on a 0.01 comparison).
    // Clamped where the donor's cast would wrap; the game clamps its own fades.
    //
    // ⚠️ Both were STUBS: Get wrote nothing, so TranslucentWorldModel::
    // InitialUpdate handed Set the caller's stack for r g b -- and Set dropped
    // everything, so every authored `Alpha` (T01S02's projector beam at 0,
    // its slides fading in) was lost. Get writes every output even for a bad
    // handle. LT_NOSERVERCOLOR=1 keeps the old stubs (says so).
    bool LTHost_NoServerColor()
    {
        static const bool b = []() {
            const bool v = (getenv("LT_NOSERVERCOLOR") != 0);
            if (v) { printf("[SERVER] LT_NOSERVERCOLOR=1 -- server Get/SetObjectColor left as stubs (old)\n"); fflush(stdout); }
            return v; }();
        return b;
    }

    LTBOOL LTHost_GetObjectColor(HOBJECT h, float* r, float* g, float* b, float* a)
    {
        const SRec* p = CLTServerObjects::FromHandle(h);
        static const unsigned char kWhite[4] = { 255, 255, 255, 255 };
        const unsigned char* c = p ? p->m_Color : kWhite;
        if (r) *r = c[0] * (1.0f / 255.0f);
        if (g) *g = c[1] * (1.0f / 255.0f);
        if (b) *b = c[2] * (1.0f / 255.0f);
        if (a) *a = c[3] * (1.0f / 255.0f);
        return p ? LTTRUE : LTFALSE;
    }

    LTBOOL LTHost_SetObjectColor(HOBJECT h, float r, float g, float b, float a)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) return LTFALSE;
        const float v[4] = { r, g, b, a };
        for (int k = 0; k < 4; ++k)
        {
            float x = v[k] * 255.0f;
            if (!(x > 0.0f)) x = 0.0f;          // NaN too
            if (x > 255.0f)  x = 255.0f;
            p->m_Color[k] = (unsigned char)x;   // truncated, as the donor's cast
        }
        return LTTRUE;
    }

    LTRESULT LTHost_SetObjectUserFlags(HOBJECT h, uint32 flags)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) return LT_ERROR;

        // ⚠️ Under LT_CAMTRACE, every user-flag write on a Camera. This is how
        // you tell "Camera::TurnOff never ran" from "it ran and the flag came
        // back on" -- two different bugs with the same symptom.
        if (getenv("LT_CAMTRACE") && p->m_UserFlags != flags &&
            p->m_pClass && p->m_pClass->m_ClassName &&
            strcmp(p->m_pClass->m_ClassName, "Camera") == 0)
        {
            printf("[CAM] '%s' userflags 0x%08x -> 0x%08x at t=%.3f%s\n",
                   p->m_sName.empty() ? "(unnamed)" : p->m_sName.c_str(),
                   p->m_UserFlags, flags, LTServerHost_Time(),
                   ((p->m_UserFlags ^ flags) & 0x20u)
                       ? ((flags & 0x20u) ? "   LIVE on" : "   LIVE OFF") : "");
            fflush(stdout);
        }

        p->m_UserFlags = flags;
        return LT_OK;
    }

    // ⚠️ THE SERVER'S ILTCommon, AND IT IS WHERE LEVEL SCRIPTING LIVES.
    // Almost every parse in ObjectDLL goes through `g_pLTServer->Common()`
    // rather than the ILTServer::Parse function pointer -- Door, Trigger,
    // CommandMgr, CinematicTrigger, Intelligence and a dozen more. A stub
    // Common() meant every one of those `while (Parse(&parse) == LT_OK)` loops
    // executed zero times, so a level's authored commands were read from the
    // world file, stored, handed to the engine, and dropped.
    //
    // ⚠️ The generated wiring assigns m_pCommonLT to a STUB, so this must be
    // installed after LTSkel_WireData -- the same ordering rule as every other
    // deliberate replacement in this constructor.
    // ⚠️ ROTATION MATHS FOR THE SERVER -- AND THE REASON NPCs WERE FLAT DISCS.
    //
    // Every rotation helper the SERVER exposes -- ILTCSBase::SetupEuler,
    // GetRotationVectors, RotateAroundAxis, AlignRotation, InterpolateRotation,
    // their ILTCommon twins and the whole of ILTMath -- was the generated stub.
    // The client had real ones; the server never did.
    //
    // ⚠️ A STUBBED SetupEuler IS NOT A WRONG ROTATION, IT IS NO ROTATION. The
    // AI builds its facing like this:
    //
    //     LTRotation rRot;                           // uninitialised
    //     g_pLTServer->SetupEuler(&rRot, 0, yaw, 0); // stub: writes nothing
    //     g_pLTServer->SetObjectRotation(m_hObject, &rRot);
    //
    // so each character's rotation was whatever the stack held -- and garbage
    // is almost never a UNIT quaternion. A non-unit quaternion does not merely
    // turn a body, it scales and shears it: measured, every skeleton was
    // upright and correctly proportioned in model space (Y span ~90), yet on
    // screen the receptionist was a horizontal disc with her head smeared
    // across it. The sixth instance of the uninitialised-out-parameter bug.
    //
    // All of it goes through LTMathImpl, the SAME code the client uses, so the
    // two halves cannot disagree about what a yaw means.
    inline void LTQ(const LTRotation& r, float* q) { q[0]=r.m_Quat[0]; q[1]=r.m_Quat[1]; q[2]=r.m_Quat[2]; q[3]=r.m_Quat[3]; }
    inline void LTR(LTRotation& r, const float* q)  { r.Init(q[0], q[1], q[2], q[3]); }

    void QuatToMatrix(const float* q, LTMatrix& m)
    {
        float M[16];
        const float z3[3] = { 0, 0, 0 };
        LT3D::FromQuatPos(q, z3, M);
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) m.m[r][c] = M[r*4 + c];
    }

    void MatrixToQuat(const LTMatrix& mm, float* q)
    {
        const float (*m)[4] = mm.m;
        const float tr = m[0][0] + m[1][1] + m[2][2];
        if (tr > 0.0f)
        {
            float s = sqrtf(tr + 1.0f) * 2.0f;
            q[3] = 0.25f * s;
            q[0] = (m[2][1] - m[1][2]) / s;
            q[1] = (m[0][2] - m[2][0]) / s;
            q[2] = (m[1][0] - m[0][1]) / s;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
        {
            float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
            q[3] = (m[2][1] - m[1][2]) / s; q[0] = 0.25f * s;
            q[1] = (m[0][1] + m[1][0]) / s; q[2] = (m[0][2] + m[2][0]) / s;
        }
        else if (m[1][1] > m[2][2])
        {
            float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
            q[3] = (m[0][2] - m[2][0]) / s; q[0] = (m[0][1] + m[1][0]) / s;
            q[1] = 0.25f * s;               q[2] = (m[1][2] + m[2][1]) / s;
        }
        else
        {
            float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
            q[3] = (m[1][0] - m[0][1]) / s; q[0] = (m[0][2] + m[2][0]) / s;
            q[1] = (m[1][2] + m[2][1]) / s; q[2] = 0.25f * s;
        }
    }

    class LTHostServerCommon : public LTStub_ILTCommon
    {
    public:
        LTRESULT Parse(ConParse* pParse) override { return LTConParse_Common(pParse); }

        // A data buffer -- the one StartHMessageWrite makes, so Release() frees
        // it and EndMessage leaves it alone. Sprinkles fills one for
        // SetObjectSFXMessage (5dh).
        LTRESULT CreateMessage(ILTMessage*& pMsg) override
        {
            LTIMPL("ILTCommon::CreateMessage");
            pMsg = LTMessage_StartData();
            return pMsg ? LT_OK : LT_ERROR;
        }

        // The ILTCommon spelling of the same thing -- see the ILTServer one.
        LTRESULT GetModelAnimUserDims(HOBJECT hObject, LTVector* pDims,
                                      HMODELANIM hAnim) override
        {
            LTIMPL("ILTCommon::GetModelAnimUserDims");
            if (!pDims) return LT_INVALIDPARAMS;
            return LTHost_AnimUserDims(hObject, pDims, hAnim);
        }

        // (UP, RIGHT, FORWARD) -- see engine/kernel/math/ltmathlt.cpp.
        LTRESULT GetRotationVectors(LTRotation& rot, LTVector& up, LTVector& right, LTVector& fwd) override
        {
            LTIMPL("ILTCommon::GetRotationVectors");
            float q[4], r[3], u[3], f[3]; LTQ(rot, q);
            LTMathImpl::GetVectors(q, r, u, f);
            right.Init(r[0], r[1], r[2]); up.Init(u[0], u[1], u[2]); fwd.Init(f[0], f[1], f[2]);
            return LT_OK;
        }
        LTRESULT SetupEuler(LTRotation& rot, float pitch, float yaw, float roll) override
        {
            LTIMPL("ILTCommon::SetupEuler");
            float q[4]; LTMathImpl::SetupEuler(q, pitch, yaw, roll); LTR(rot, q);
            return LT_OK;
        }

        // ⚠️ THIS ONE CALL IS EVERY NPC IN THE GAME.
        //
        // `CAI::InitialUpdate` (AI.cpp:800) asks the bute manager for the
        // character's model and skins, fills a create struct, and hands it
        // over:
        //
        //     g_pLTServer->Common()->SetObjectFilenames(m_hObject, &cs);
        //
        // With a stub here the filename was DISCARDED. The AI still existed,
        // still ticked -- measured: 13 live AI objects taking 2038 MID_UPDATEs
        // each -- and object replication then had a position to mirror and
        // nothing to draw. So the level was fully populated and completely
        // empty, which is exactly how it was reported: "there are no NPCs at
        // all".
        //
        // ⚠️ THE OBJECT IS ALREADY CREATED WHEN THIS RUNS. It is not part of
        // the creation handshake -- InitialUpdate is after MID_PRECREATE -- so
        // it must write the record IN PLACE rather than go through Apply,
        // which would also rewrite name, position and flags from a struct the
        // caller only filled the filename fields of.
        LTRESULT GetPolyTextureFlags(HPOLY hPoly, uint32* pFlags) override
        {
            LTIMPL("ILTCommon::GetPolyTextureFlags");
            return LTHost_PolyTextureFlags((unsigned int)hPoly, pFlags);
        }

        LTRESULT SetObjectFilenames(HOBJECT hObj, ObjectCreateStruct* pStruct) override
        {
            LTIMPL("ILTCommon::SetObjectFilenames");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || !pStruct) return LT_ERROR;

            // ⚠️ An EMPTY filename leaves the model alone. The game clears the
            // struct and fills only what it means to change, so treating empty
            // as "no model" would erase the model of every object that only
            // wanted to change a skin.
            if (pStruct->m_Filename[0]) p->m_sFilename = pStruct->m_Filename;

            bool bAnySkin = false;
            for (int k = 0; k < MAX_MODEL_TEXTURES; ++k)
                if (pStruct->m_SkinNames[k][0]) { bAnySkin = true; break; }

            if (bAnySkin)
            {
                p->m_Skins.clear();
                for (int k = 0; k < MAX_MODEL_TEXTURES; ++k)
                    if (pStruct->m_SkinNames[k][0])
                        p->m_Skins.push_back(pStruct->m_SkinNames[k]);
            }

            if (getenv("LT_NAMETRACE"))
            {
                printf("[MODEL] '%s' -> %s\n",
                       p->m_sName.empty() ? "(unnamed)" : p->m_sName.c_str(),
                       p->m_sFilename.c_str());
                fflush(stdout);
            }
            return LT_OK;
        }
    };
    LTHostServerCommon  g_ServerCommon;

    // ⚠️ THE SERVER'S ILTModel -- THE ANIMATION API THE CHARACTERS ACTUALLY USE.
    //
    // Every character reported `anim -1` after SetModelAnimation was wired,
    // because the AI do not call it. CAnimator / CAnimationMgr drive the
    // model through TRACKERS: GetMainTracker, then SetCurAnim / SetLooping /
    // GetPlaybackState on the tracker. All of it was the generated stub, so
    // no character ever had an animation, and a character with no animation is
    // drawn in its AUTHORED REFERENCE POSE -- which for most NOLF models is
    // lying flat or T-posed. Measured: of the twelve characters in T01S02,
    // Cate stood upright and the rest floated horizontally in the air.
    //
    // ⚠️ THE TRACKER'S BYTES ARE NEVER READ. The game embeds LTAnimTracker by
    // value and never calls its members (checked: 15 uses, no member calls),
    // so the engine keeps its own state keyed by the tracker's ADDRESS. That
    // avoids depending on engine-internal types the struct points at
    // (Model*, ModelAnim*) which this port does not have.
    //
    // ⚠️ THE MAIN TRACKER IS THE RECORD. Its animation, start time and looping
    // live on SRec, the same fields ILTCSBase::SetModelAnimation writes -- so
    // the two APIs the game mixes cannot disagree about what a character is
    // playing, and the renderer reads one place.
    //
    // ⚠️ SECONDARY trackers (blink, recoil, pitch -- added with AddTracker)
    // are blended into the pose through their weight sets: LTHost_LayersFor
    // hands them to the renderer and to PlaceAttached, and LTModelDraw::
    // BuildPose applies the donor's BlendTransform rule (HANDOVER.md 5au).
    // Defined with the rest of the server clock further down this namespace.
    extern float g_fServerTime;
    extern float g_fServerFrameTime;

    struct LTTrackerState
    {
        SRec*   m_pRec;
        bool    m_bMain;
        int     m_nAnim;
        bool    m_bLooping;
        bool    m_bPlaying;
        float   m_fStart;
        int     m_nWeightSet;
        unsigned m_nSeq;        // AddTracker order -- the donor blends in it
        float   m_fFrozen;      // elapsed seconds while SetPlaying(FALSE)
    };
    std::map<LTAnimTracker*, LTTrackerState> g_Trackers;
    std::map<SRec*, LTAnimTracker*>          g_MainTracker;

    // ⚠️ THE BLEND-IN -- WHY EVERY CHARACTER REACTED A LITTLE TOO FAST.
    //
    // The shipped lithtech.exe never starts a new animation's clock at once.
    // trk_SetCurAnim (0x4011c0) opens a blend when its transition argument is
    // set and the old and new animations are both valid; trk_ScanToKeyFrame
    // (0x401370) then adds each frame's milliseconds to a counter and, until
    // the counter passes the animation's m_InterpolationMS, only cross-fades:
    // no time, no keys, no MS_PLAYDONE. The time left over starts the
    // animation. The transition argument is the object's FLAG_ANIMTRANSITION
    // (callers 0x44ff02, 0x478ad4 pass bit 7 of its flags) -- set on every
    // CCharacter and Prop, cleared on bodies (Character.cpp:116, Prop.cpp:346,
    // BodyState.cpp:49). The blend time is per animation, from the model
    // (LTABCAnim::m_nInterpMS: 200 on most, 0 on the AI fire loops).
    //
    // Modelled by starting the new animation's clock that much LATER: every
    // reader sees "before the start" during the blend -- first keyframe held
    // (this renderer steps keyframes, it does not cross-fade), keys and DONE
    // withheld. LT_NOANIMBLEND=1 restores the instant start (says so).
    static float LTHost_AnimStartAfterBlend(const SRec* p, int nPrev, int nNew)
    {
        static const bool bOff = (getenv("LT_NOANIMBLEND") != 0);
        if (bOff)
        {
            static bool bSaid = false;
            if (!bSaid) { bSaid = true; printf("[ANIM] animation blend-in DISABLED (LT_NOANIMBLEND)\n"); fflush(stdout); }
            return g_fServerTime;
        }
        if (!p || nPrev < 0 || nNew < 0) return g_fServerTime;
        if (!(p->m_Flags & FLAG_ANIMTRANSITION)) return g_fServerTime;
        const int nMs = LTScene_GetAnimInterpForFile(p->m_sFilename.c_str(), nNew);
        return g_fServerTime + (nMs > 0 ? nMs / 1000.0f : 0.0f);
    }

    // Seconds into the animation, never negative: during a blend-in it is
    // the first frame (see LTHost_AnimStartAfterBlend).
    static inline float LTHost_AnimElapsed(float fStart)
    {
        const float f = g_fServerTime - fStart;
        return f > 0.0f ? f : 0.0f;
    }

    // Per tracker (the record itself for the main one): how far its model
    // keys have been delivered. See LTHost_ServerModelKeys.
    struct LTKeyCursor { int m_nAnim; float m_fStart; float m_fUpToMs; };
    std::map<const void*, LTKeyCursor> g_KeyCursors;

    // Forgets every tracker. Called when the world's objects are destroyed --
    // the records they point at are gone.
    void LTHost_ResetTrackers()
    {
        g_KeyCursors.clear();
        for (std::map<SRec*, LTAnimTracker*>::iterator it = g_MainTracker.begin();
             it != g_MainTracker.end(); ++it)
            free(it->second);
        g_MainTracker.clear();
        g_Trackers.clear();
    }

    // The record's extra trackers that touch the pose, in AddTracker order:
    // an animation and a weight set are both required. LT_ANIMTRACE-style
    // tracing lives with the caller.
    void LTHost_LayersFor(const SRec* p, std::vector<LTDynamicLayer>& out)
    {
        out.clear();
        std::vector<std::pair<unsigned, LTDynamicLayer> > v;
        for (std::map<LTAnimTracker*, LTTrackerState>::const_iterator it = g_Trackers.begin();
             it != g_Trackers.end(); ++it)
        {
            const LTTrackerState& t = it->second;
            if (t.m_bMain || t.m_pRec != p || t.m_nAnim < 0 || t.m_nWeightSet < 0) continue;
            LTDynamicLayer L;
            L.m_nAnim      = t.m_nAnim;
            L.m_fAnimTime  = t.m_bPlaying ? LTHost_AnimElapsed(t.m_fStart)
                                          : (t.m_fFrozen > 0.0f ? t.m_fFrozen : 0.0f);
            L.m_bLooping   = t.m_bLooping;
            L.m_nWeightSet = t.m_nWeightSet;
            v.push_back(std::make_pair(t.m_nSeq, L));
        }
        std::sort(v.begin(), v.end(),
                  [](const std::pair<unsigned, LTDynamicLayer>& a,
                     const std::pair<unsigned, LTDynamicLayer>& b) { return a.first < b.first; });
        for (size_t i = 0; i < v.size(); ++i) out.push_back(v[i].second);
    }

    // ⚠️ SERVER MODEL KEYS -- MID_MODELSTRINGKEY, and the engine never sent one.
    //
    // An AI fires ONLY when its fire animation crosses the "FIRE" key:
    // CAIHumanStrategyShoot::HandleModelString sets m_bFired, and UpdateFiring
    // runs only when it is set (AIHumanStrategy.cpp:2683, :2752). With no
    // server keys every assassin in M01S01 walked to its node, aimed at the
    // ambassador for minutes and never pulled the trigger -- so the mission's
    // own failure (`failure` <- diplomat1's DamageTriggerTarget) could never
    // happen. The client half has delivered keys since 5ba; this is the other
    // side (ILTClient's keys never reach a SERVER object).
    //
    // Donor: s_object.cpp FullObjectUpdate -> ServerStringKeyCallback. Every
    // OT_MODEL each frame, every tracker, keys sent only to objects with
    // FLAG_MODELKEYS, the string split by ConParse (';' between commands, one
    // message per command). Keys fire by TIME CROSSED, as on the client
    // (ltscene.cpp TakeModelKeys): (last, now], a looping wrap takes the tail
    // then the head, a fresh play includes the key at 0.
    //
    // LT_NOSERVERKEYS=1 restores the old behaviour (says so). LT_SKEYTRACE=
    // <name/class substring> prints each key delivered.

    unsigned g_nServerKeys = 0;

    static void SendKeysFor(SRec* p, const void* pId, int nAnim, float fStart, bool bLoop,
                            const CLTABCModel* pModel)
    {
        if (nAnim < 0) return;
        const std::vector<LTABCAnim>& anims = pModel->GetAnims();
        if ((size_t)nAnim >= anims.size()) return;
        const LTABCAnim& a = anims[(size_t)nAnim];
        if (a.m_Keyframes.empty()) return;

        static const char* pTrace = getenv("LT_SKEYTRACE");
        const bool bTrace = pTrace && *pTrace &&
            (p->m_sName.find(pTrace) != std::string::npos ||
             (p->m_pClass && strstr(p->m_pClass->m_ClassName, pTrace)));

        LTKeyCursor& c = g_KeyCursors[pId];
        if (c.m_nAnim != nAnim || c.m_fStart != fStart)
        {
            if (bTrace && c.m_nAnim != nAnim)
            {
                unsigned nCmd = 0;
                for (size_t k = 0; k < a.m_Keyframes.size(); ++k)
                    if (!a.m_Keyframes[k].m_sCommand.empty()) ++nCmd;
                printf("[SKEY] t=%.3f %s '%s' %s anim %d '%s'%s, %u key(s)\n", g_fServerTime,
                       p->m_pClass ? p->m_pClass->m_ClassName : "?", p->m_sName.c_str(),
                       pId == (const void*)p ? "main" : "layer", nAnim, a.m_sName.c_str(),
                       bLoop ? " loop" : "", nCmd);
                fflush(stdout);
            }
            c.m_nAnim = nAnim; c.m_fStart = fStart; c.m_fUpToMs = -1.0f;
        }

        const float fLen = (float)a.m_Keyframes.back().m_nTime;
        float fMs = (g_fServerTime - fStart) * 1000.0f;
        // Still blending in: the donor processes no keys until the blend is
        // over, and then the key at 0 fires with the first real frame.
        if (fMs < 0.0f) return;

        std::vector<const char*> fire;
        float fFrom = c.m_fUpToMs;
        if (bLoop && fLen > 0.0f)
        {
            const float fWrapped = fmodf(fMs, fLen);
            if (fWrapped < fFrom)
            {
                for (size_t k = 0; k < a.m_Keyframes.size(); ++k)
                    if (!a.m_Keyframes[k].m_sCommand.empty() && (float)a.m_Keyframes[k].m_nTime > fFrom)
                        fire.push_back(a.m_Keyframes[k].m_sCommand.c_str());
                fFrom = -1.0f;
            }
            fMs = fWrapped;
        }
        else if (fLen > 0.0f && fMs > fLen)
        {
            fMs = fLen;
        }
        for (size_t k = 0; k < a.m_Keyframes.size(); ++k)
        {
            const float t = (float)a.m_Keyframes[k].m_nTime;
            if (!a.m_Keyframes[k].m_sCommand.empty() && t > fFrom && t <= fMs)
                fire.push_back(a.m_Keyframes[k].m_sCommand.c_str());
        }
        c.m_fUpToMs = fMs;

        for (size_t i = 0; i < fire.size(); ++i)
        {
            // The game may remove the object from inside the handler.
            if (p->m_bDead || !p->m_pGame) return;

            char szBuf[512];
            strncpy(szBuf, fire[i], sizeof(szBuf) - 1);
            szBuf[sizeof(szBuf) - 1] = 0;
            ConParse parse;
            parse.Init(szBuf);
            while (parse.Parse())
            {
                if (parse.m_nArgs <= 0) continue;
                if (bTrace)
                {
                    printf("[SKEY] t=%.3f %s '%s' anim %d: %s\n", g_fServerTime,
                           p->m_pClass ? p->m_pClass->m_ClassName : "?", p->m_sName.c_str(),
                           nAnim, fire[i]);
                    fflush(stdout);
                }
                ArgList args;
                args.argv = parse.m_Args;
                args.argc = parse.m_nArgs;
                p->m_pGame->EngineMessageFn(MID_MODELSTRINGKEY, &args, 0.0f);
                ++g_nServerKeys;
                if (p->m_bDead || !p->m_pGame) return;
            }
        }
    }

    // ⚠️ AN ANIMATION PUT AT A TIME HAS ALREADY PLAYED UP TO IT (5dd). The
    // cursor of tracker pId (the record itself for the main one) is moved to
    // "now" WITHOUT firing what lies between -- the donor's trk_SetCurTime
    // scans with bProcessKeys FALSE (animtracker.cpp:113), and a restore
    // brings back the tracker's m_CurKey. Without it SendKeysFor saw a new
    // start, reset the cursor to -1 and fired every key from the first frame:
    // an AI restored mid-fire would shoot again. Same arithmetic as
    // SendKeysFor.
    void LTHost_KeysDeliveredToNow(SRec* p, const void* pId, int nAnim, float fStart, bool bLoop)
    {
        if (!p || nAnim < 0) return;
        LTKeyCursor& c = g_KeyCursors[pId];
        c.m_nAnim = nAnim; c.m_fStart = fStart; c.m_fUpToMs = -1.0f;
        const CLTABCModel* pModel = LTScene_ModelForFile(p->m_sFilename.c_str());
        if (!pModel || (size_t)nAnim >= pModel->GetAnims().size()) return;
        const LTABCAnim& a = pModel->GetAnims()[(size_t)nAnim];
        if (a.m_Keyframes.empty()) return;
        const float fLen = (float)a.m_Keyframes.back().m_nTime;
        float fMs = (g_fServerTime - fStart) * 1000.0f;
        if (fMs < 0.0f) return;                         // still blending in
        if (bLoop && fLen > 0.0f)            fMs = fmodf(fMs, fLen);
        else if (fLen > 0.0f && fMs > fLen)  fMs = fLen;
        c.m_fUpToMs = fMs;
    }
    void LTHost_KeysDeliveredToNow(SRec* p)
    {
        if (p) LTHost_KeysDeliveredToNow(p, (const void*)p, p->m_nAnim, p->m_fAnimStart, p->m_bAnimLooping);
    }

    void LTHost_ServerModelKeys()
    {
        static const bool bOff = (getenv("LT_NOSERVERKEYS") != 0);
        if (bOff)
        {
            static bool bSaid = false;
            if (!bSaid) { bSaid = true; printf("[SKEY] server model keys DISABLED (LT_NOSERVERKEYS)\n"); fflush(stdout); }
            return;
        }

        const unsigned int n = g_Objects.ObjectCount();
        for (unsigned int i = 0; i < n; ++i)
        {
            SRec* p = g_Objects.RecordAt(i);
            if (!p || p->m_bDead || !p->m_pGame || p->m_Type != OT_MODEL) continue;
            if (!(p->m_Flags & FLAG_MODELKEYS)) continue;
            // Asleep (HANDOVER 5cp): its tracker does not run, so no keys.
            if (p->m_nInactive) continue;
            const CLTABCModel* pModel = LTScene_ModelForFile(p->m_sFilename.c_str());
            if (!pModel) continue;

            // The main tracker reads through to the record (see g_MainTracker).
            SendKeysFor(p, p, p->m_nAnim, p->m_fAnimStart, p->m_bAnimLooping, pModel);

            // Secondary trackers, in AddTracker order. Collected first: a key
            // handler may add or remove trackers.
            std::vector<std::pair<unsigned, LTAnimTracker*> > extra;
            for (std::map<LTAnimTracker*, LTTrackerState>::const_iterator it = g_Trackers.begin();
                 it != g_Trackers.end(); ++it)
                if (!it->second.m_bMain && it->second.m_pRec == p && it->second.m_bPlaying)
                    extra.push_back(std::make_pair(it->second.m_nSeq, it->first));
            std::sort(extra.begin(), extra.end());
            for (size_t k = 0; k < extra.size(); ++k)
            {
                std::map<LTAnimTracker*, LTTrackerState>::iterator it = g_Trackers.find(extra[k].second);
                if (it == g_Trackers.end() || p->m_bDead) continue;
                SendKeysFor(p, it->first, it->second.m_nAnim, it->second.m_fStart,
                            it->second.m_bLooping, pModel);
            }
        }
    }

    // Defined with the attachment code further down this namespace.
    bool LTHost_PoseOf(SRec* P, const CLTABCModel*& pModel, LTModelPose& pose);
    void LTHost_ObjectFrame(const SRec* P, const CLTABCModel& model, bool bMirror, float* W);
    void QuatFromRot(const float* m, float* q);

    class LTHostServerModel : public LTStub_ILTModel
    {
        static LTTrackerState* State(LTAnimTracker* p)
        {
            std::map<LTAnimTracker*, LTTrackerState>::iterator it = g_Trackers.find(p);
            return (it == g_Trackers.end()) ? 0 : &it->second;
        }

        // The animation a tracker is playing, and since when -- the main
        // tracker reads through to the record.
        static int   Anim(const LTTrackerState* t)  { return t->m_bMain ? t->m_pRec->m_nAnim : t->m_nAnim; }
        static float Start(const LTTrackerState* t) { return t->m_bMain ? t->m_pRec->m_fAnimStart : t->m_fStart; }
        static bool  Loop(const LTTrackerState* t)  { return t->m_bMain ? t->m_pRec->m_bAnimLooping : t->m_bLooping; }

        static int LengthMs(const LTTrackerState* t)
        {
            return LTScene_GetAnimLengthForFile(t->m_pRec->m_sFilename.c_str(), Anim(t));
        }

    public:
        LTRESULT GetMainTracker(HOBJECT hObj, LTAnimTracker*& pTracker) override
        {
            LTIMPL("ILTModel::GetMainTracker");
            pTracker = 0;                       // written on every path
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p) return LT_ERROR;

            std::map<SRec*, LTAnimTracker*>::iterator it = g_MainTracker.find(p);
            if (it != g_MainTracker.end()) { pTracker = it->second; return LT_OK; }

            // ⚠️ ZEROED, because nothing here reads it but the game might.
            LTAnimTracker* t = (LTAnimTracker*)calloc(1, sizeof(LTAnimTracker));
            if (!t) return LT_ERROR;
            g_MainTracker[p] = t;

            LTTrackerState st = { p, true, -1, false, true, 0.0f, -1 };
            g_Trackers[t] = st;
            pTracker = t;
            return LT_OK;
        }

        // ⚠️ TIME IS DERIVED FROM THE SERVER CLOCK, not accumulated here, so
        // there is nothing to advance -- but LT_OK matters: the game treats an
        // error from this as "the model is not animating".
        LTRESULT UpdateMainTracker(HOBJECT, LTFLOAT) override
        {
            LTIMPL("ILTModel::UpdateMainTracker");
            return LT_OK;
        }

        LTRESULT AddTracker(HOBJECT hObj, LTAnimTracker* pTracker) override
        {
            LTIMPL("ILTModel::AddTracker");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || !pTracker) return LT_ERROR;
            static unsigned s_nSeq = 0;
            LTTrackerState st = { p, false, -1, false, true, g_fServerTime, -1, ++s_nSeq, 0.0f };
            g_Trackers[pTracker] = st;
            return LT_OK;
        }

        LTRESULT RemoveTracker(HOBJECT, LTAnimTracker* pTracker) override
        {
            LTIMPL("ILTModel::RemoveTracker");
            LTTrackerState* t = State(pTracker);
            if (!t || t->m_bMain) return LT_ERROR;   // the main one is not removable
            g_Trackers.erase(pTracker);
            return LT_OK;
        }

        LTRESULT GetCurAnim(LTAnimTracker* pTracker, HMODELANIM& hAnim) override
        {
            LTIMPL("ILTModel::GetCurAnim");
            hAnim = INVALID_MODEL_ANIM;
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            const int n = Anim(t);
            if (n >= 0) hAnim = (HMODELANIM)n;
            return LT_OK;
        }

        LTRESULT SetCurAnim(LTAnimTracker* pTracker, HMODELANIM hAnim) override
        {
            LTIMPL("ILTModel::SetCurAnim");
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            const int n = (hAnim == INVALID_MODEL_ANIM) ? -1 : (int)hAnim;

            // ⚠️ SETTING THE SAME ANIMATION RESTARTS IT. Unlike the obsolete
            // SetModelAnimation, the tracker API is explicit: AI state code
            // calls SetCurAnim when it WANTS a fresh play, and waits on
            // MS_PLAYDONE for it -- so not restarting would leave a state
            // machine waiting on an animation that finished long ago.
            const float fStart = LTHost_AnimStartAfterBlend(t->m_pRec, Anim(t), n);
            if (t->m_bMain) { t->m_pRec->m_nAnim = n; t->m_pRec->m_fAnimStart = fStart; }
            else            { t->m_nAnim = n; t->m_fStart = fStart; }
            return LT_OK;
        }

        LTRESULT ResetAnim(LTAnimTracker* pTracker) override
        {
            LTIMPL("ILTModel::ResetAnim");
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            // trk_Reset rewinds the animation but leaves a running blend-in
            // running (it does not touch the interpolation counter).
            float& fStart = t->m_bMain ? t->m_pRec->m_fAnimStart : t->m_fStart;
            if (fStart <= g_fServerTime) fStart = g_fServerTime;
            return LT_OK;
        }

        LTRESULT GetLooping(LTAnimTracker* pTracker) override
        {
            LTIMPL("ILTModel::GetLooping");
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            return Loop(t) ? LT_YES : LT_NO;
        }

        LTRESULT SetLooping(LTAnimTracker* pTracker, LTBOOL bLooping) override
        {
            LTIMPL("ILTModel::SetLooping");
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            if (t->m_bMain) t->m_pRec->m_bAnimLooping = (bLooping != 0);
            else            t->m_bLooping = (bLooping != 0);
            return LT_OK;
        }

        LTRESULT GetPlaying(LTAnimTracker* pTracker) override
        {
            LTIMPL("ILTModel::GetPlaying");
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            return t->m_bPlaying ? LT_YES : LT_NO;
        }

        LTRESULT SetPlaying(LTAnimTracker* pTracker, LTBOOL bPlaying) override
        {
            LTIMPL("ILTModel::SetPlaying");
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            // A paused secondary tracker holds its frame (it is still
            // blended); the time is frozen here and resumed from there.
            const bool bNow = (bPlaying != 0);
            if (!t->m_bMain && bNow != t->m_bPlaying)
            {
                if (!bNow) t->m_fFrozen = g_fServerTime - t->m_fStart;
                else       t->m_fStart  = g_fServerTime - t->m_fFrozen;
            }
            t->m_bPlaying = bNow;
            return LT_OK;
        }

        // ⚠️ MS_PLAYDONE IS WHAT AI STATE MACHINES WAIT ON. A one-shot that
        // never reports done leaves a character frozen mid-sequence forever;
        // one that reports done instantly skips every animation. So it is
        // computed from the real length, and unknown length reports NOT done.
        LTRESULT GetPlaybackState(LTAnimTracker* pTracker, uint32& flags) override
        {
            LTIMPL("ILTModel::GetPlaybackState");
            flags = 0;
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            if (Anim(t) < 0 || Loop(t)) return LT_OK;
            const int nLen = LengthMs(t);
            if (nLen < 0) return LT_OK;
            if ((g_fServerTime - Start(t)) * 1000.0f >= (float)nLen) flags |= MS_PLAYDONE;
            return LT_OK;
        }

        LTRESULT GetCurAnimLength(LTAnimTracker* pTracker, uint32& length) override
        {
            LTIMPL("ILTModel::GetCurAnimLength");
            length = 0;
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            const int n = LengthMs(t);
            if (n < 0) return LT_ERROR;
            length = (uint32)n;
            return LT_OK;
        }

        LTRESULT GetCurAnimTime(LTAnimTracker* pTracker, uint32& curTime) override
        {
            LTIMPL("ILTModel::GetCurAnimTime");
            curTime = 0;
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            float ms = (g_fServerTime - Start(t)) * 1000.0f;
            const int nLen = LengthMs(t);
            if (nLen > 0) ms = Loop(t) ? fmodf(ms, (float)nLen) : (ms > nLen ? (float)nLen : ms);
            curTime = (uint32)(ms < 0 ? 0 : ms);
            return LT_OK;
        }

        LTRESULT SetCurAnimTime(LTAnimTracker* pTracker, uint32 curTime) override
        {
            LTIMPL("ILTModel::SetCurAnimTime");
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            const float fStart = g_fServerTime - curTime / 1000.0f;
            if (t->m_bMain) t->m_pRec->m_fAnimStart = fStart;
            else            t->m_fStart = fStart;
            // ⚠️ Moving the time skips the keys in between, as the donor's
            // trk_SetCurTime does (see LTHost_KeysDeliveredToNow). Body::Load
            // calls this on a restored body. LT_OLDANIMTIMEKEYS=1 = the old
            // way, every key from the first frame fires again (says so).
            static const bool s_bOldKeys = []() {
                const bool b = (getenv("LT_OLDANIMTIMEKEYS") != 0);
                if (b) { printf("[ANIM] LT_OLDANIMTIMEKEYS=1 -- SetCurAnimTime replays the keys before the new time (old)\n"); fflush(stdout); }
                return b; }();
            if (!s_bOldKeys)
                LTHost_KeysDeliveredToNow(t->m_pRec, t->m_bMain ? (const void*)t->m_pRec : (const void*)pTracker,
                                          Anim(t), Start(t), Loop(t));
            return LT_OK;
        }

        // ⚠️ THE BONES, FOR THE BULLETS. CCharacterHitBox does per-node hit
        // detection (CharacterHitBox.cpp:294): GetNode by name, then
        // GetNodeTransform in world space, and only a ray that passes a node's
        // radius DAMAGES -- otherwise the shot continues. Both were stubs, so
        // every shot that reached an enemy's hit box went on through him.
        LTRESULT GetNode(HOBJECT hObj, char* pNodeName, HMODELNODE& hNode) override
        {
            LTIMPL("ILTModel::GetNode");
            hNode = INVALID_MODEL_NODE;
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || !pNodeName) return LT_ERROR;
            const CLTABCModel* pModel = LTScene_ModelForFile(p->m_sFilename.c_str());
            if (!pModel) return LT_ERROR;
            const std::vector<LTABCNode>& nd = pModel->GetNodes();
            for (size_t i = 0; i < nd.size(); ++i)
                if (strcasecmp(nd[i].m_sName.c_str(), pNodeName) == 0)
                { hNode = (HMODELNODE)i; return LT_OK; }
            return LT_NOTFOUND;
        }

        LTRESULT GetNodeTransform(HOBJECT hObj, HMODELNODE hNode, LTransform& transform,
                                  LTBOOL bWorldSpace) override
        {
            LTIMPL("ILTModel::GetNodeTransform");
            transform.m_Pos.Init();
            transform.m_Rot.Init();
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p) return LT_ERROR;
            const CLTABCModel* pModel = 0;
            LTModelPose pose;
            if (!LTHost_PoseOf(p, pModel, pose)) return LT_ERROR;
            if ((unsigned)hNode >= pose.Count()) return LT_ERROR;

            static const bool bMirror = (getenv("LT_DYNMIRROR") != 0);
            float N[16];
            memcpy(N, pose.Get((unsigned)hNode), sizeof(N));
            // Model-space X mirror, S*N*S with S = diag(-1,1,1): row 0 and
            // column 0 negated (N[0] twice, so unchanged).
            if (bMirror)
            {
                for (int c = 0; c < 4; ++c) N[c] = -N[c];
                for (int r = 0; r < 4; ++r) N[r*4] = -N[r*4];
            }

            float R[16];
            if (bWorldSpace)
            {
                float W[16];
                LTHost_ObjectFrame(p, *pModel, bMirror, W);
                LT3D::Multiply(W, N, R);
            }
            else memcpy(R, N, sizeof(R));

            transform.m_Pos.Init(R[3], R[7], R[11]);
            float q[4];
            QuatFromRot(R, q);
            transform.m_Rot.Init(q[0], q[1], q[2], q[3]);

            // LT_EYETRACE=<object name> -- an AI's EYE as the game derives it.
            // CAIHuman::UpdateNodes takes Head_node's world rotation and passes
            // m_vEyeForward as the SECOND vector of ILTMath::GetRotationVectors,
            // whose order is (right, UP, forward) -- so the eye is the head's
            // Y axis ("on the head node vU faces forward", NodeController.cpp).
            // Every sight FOV test is made against it (AI.cpp:2059). Printed
            // with the body's own forward, every 30th ask (HANDOVER 5ct).
            {
                static const char* pEye = getenv("LT_EYETRACE");
                if (pEye && *pEye && bWorldSpace && strcasecmp(p->m_sName.c_str(), pEye) == 0)
                {
                    const std::vector<LTABCNode>& nd = pModel->GetNodes();
                    if ((unsigned)hNode < nd.size() && strcasecmp(nd[(unsigned)hNode].m_sName.c_str(), "Head_node") == 0)
                    {
                        static unsigned s_n = 0;
                        if ((s_n++ % 30) == 0)
                        {
                            float B[16];
                            LT3D::FromQuatPos(p->m_Rot, p->m_Pos, B);
                            const float ex = R[1], ey = R[5], ez = R[9];         // Y column: the game's eye
                            const float bx = B[2], by = B[6], bz = B[10];        // the body's forward
                            const float el = sqrtf(ex*ex + ey*ey + ez*ez), bl = sqrtf(bx*bx + by*by + bz*bz);
                            const float d = (el > 0 && bl > 0) ? (ex*bx + ey*by + ez*bz) / (el * bl) : 0.0f;
                            printf("[EYE] t=%.2f %s at (%.0f %.0f %.0f) anim %d eye(%.2f %.2f %.2f) body(%.2f %.2f %.2f) angle %.0f deg\n",
                                   g_fServerTime, p->m_sName.c_str(), p->m_Pos[0], p->m_Pos[1], p->m_Pos[2], p->m_nAnim,
                                   ex/el, ey/el, ez/el, bx/bl, by/bl, bz/bl,
                                   acosf(d < -1 ? -1 : (d > 1 ? 1 : d)) * 57.2958f);
                            fflush(stdout);
                        }
                    }
                }
            }

            // LT_BONETRACE=1 -- every bone the game asks for (the hit test),
            // with the object's position, anim and the bone's world point.
            // (LT_NODETRACE is the client's node-control trace, ltscene.cpp.)
            static const bool s_bTrace = (getenv("LT_BONETRACE") != 0);
            if (s_bTrace)
            {
                const std::vector<LTABCNode>& nd = pModel->GetNodes();
                printf("[BONE] t=%.3f %-14s pos(%.0f %.0f %.0f) anim %d ws%d node %-18s -> (%.1f %.1f %.1f)\n",
                       g_fServerTime, p->m_sName.c_str(), p->m_Pos[0], p->m_Pos[1], p->m_Pos[2],
                       p->m_nAnim, (int)bWorldSpace,
                       (unsigned)hNode < nd.size() ? nd[(unsigned)hNode].m_sName.c_str() : "?",
                       R[3], R[7], R[11]);
                fflush(stdout);
            }
            return LT_OK;
        }

        LTRESULT FindWeightSet(HOBJECT hObj, char* pSetName, HMODELWEIGHTSET& hSet) override
        {
            LTIMPL("ILTModel::FindWeightSet");
            hSet = INVALID_MODEL_WEIGHTSET;
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || !pSetName) return LT_ERROR;
            const int n = LTScene_GetWeightSetForFile(p->m_sFilename.c_str(), pSetName);
            if (n < 0) return LT_NOTFOUND;
            hSet = (HMODELWEIGHTSET)n;
            return LT_OK;
        }

        LTRESULT GetWeightSet(LTAnimTracker* pTracker, HMODELWEIGHTSET& hSet) override
        {
            LTIMPL("ILTModel::GetWeightSet");
            hSet = INVALID_MODEL_WEIGHTSET;
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            if (t->m_nWeightSet >= 0) hSet = (HMODELWEIGHTSET)t->m_nWeightSet;
            return LT_OK;
        }

        LTRESULT SetWeightSet(LTAnimTracker* pTracker, HMODELWEIGHTSET hSet) override
        {
            LTIMPL("ILTModel::SetWeightSet");
            LTTrackerState* t = State(pTracker);
            if (!t) return LT_ERROR;
            t->m_nWeightSet = (hSet == INVALID_MODEL_WEIGHTSET) ? -1 : (int)hSet;
            return LT_OK;
        }
    };
    LTHostServerModel   g_ServerModel;

    // ⚠️ THE SERVER'S ILTTransform -- the client's has been real for a while;
    // the server's was the generated stub, so every transform the hit
    // detection read back was garbage (g_pTransLT->Get on the server).
    class LTHostServerTransform : public LTStub_ILTTransform
    {
    public:
        LTRESULT Get(LTransform& t, LTVector& pos, LTRotation& rot) override
        { LTIMPL("ILTTransform::Get (server)"); pos = t.m_Pos; rot = t.m_Rot; return LT_OK; }
        LTRESULT Set(LTransform& t, LTVector& pos, LTRotation& rot) override
        { LTIMPL("ILTTransform::Set (server)"); t.m_Pos = pos; t.m_Rot = rot; return LT_OK; }
        LTRESULT GetPos(LTransform& t, LTVector& pos) override
        { LTIMPL("ILTTransform::GetPos (server)"); pos = t.m_Pos; return LT_OK; }
        LTRESULT GetRot(LTransform& t, LTRotation& rot) override
        { LTIMPL("ILTTransform::GetRot (server)"); rot = t.m_Rot; return LT_OK; }
    };
    LTHostServerTransform g_ServerTransform;

    // ⚠️ THE MODEL FILENAMES, AND THE FOURTH UNINITIALISED OUT-PARAMETER.
    //
    // `CPlayerObj::UpdateClientPhysics` (PlayerObj.cpp:1569) does:
    //
    //     char fileName[256], skinName[256];          // bare locals
    //     g_pLTServer->GetModelFilenames(m_hObject, fileName, ...);
    //     g_pLTServer->WriteToMessageString(hWrite, fileName);
    //
    // With the getter a stub the buffers were never written, so the SERVER
    // sent a stack pattern as the player's model name and the client
    // faithfully built an object called `@\xef\xbf\xbdx`. The message pipe,
    // the string encoding and the client's CreateObject were all correct; the
    // filename was garbage before it was ever written.
    //
    // ⚠️ THE RECORD ALREADY HELD THIS. m_sFilename and m_Skins were added for
    // object replication (§5p) and nothing was reading them back out.
    LTRESULT LTHost_SetModelFilenames(HOBJECT h, char* pFilename, char* pSkinName)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) return LT_ERROR;

        if (pFilename) p->m_sFilename = pFilename;
        if (pSkinName)
        {
            // ⚠️ Skin 0 REPLACED, not appended -- the game calls this whenever
            // a character changes outfit, and appending would grow the list
            // without bound and leave the renderer reading a stale skin 0.
            if (p->m_Skins.empty()) p->m_Skins.push_back(pSkinName);
            else                    p->m_Skins[0] = pSkinName;
        }
        return LT_OK;
    }

    LTBOOL LTHost_GetModelFilenames(HOBJECT h, char* pFilename, int nFileLen,
                                    char* pSkinName, int nSkinLen)
    {
        // ⚠️ BOTH BUFFERS WRITTEN ON EVERY PATH, INCLUDING FAILURE. That is
        // the whole point of this function existing -- see above. An empty
        // string is a truthful "no model"; an untouched buffer is a lie the
        // caller cannot detect.
        if (pFilename && nFileLen > 0) pFilename[0] = 0;
        if (pSkinName && nSkinLen > 0) pSkinName[0] = 0;

        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) return LTFALSE;

        if (pFilename && nFileLen > 0)
        {
            strncpy(pFilename, p->m_sFilename.c_str(), (size_t)nFileLen - 1);
            pFilename[nFileLen - 1] = 0;
        }
        if (pSkinName && nSkinLen > 0 && !p->m_Skins.empty())
        {
            strncpy(pSkinName, p->m_Skins[0].c_str(), (size_t)nSkinLen - 1);
            pSkinName[nSkinLen - 1] = 0;
        }
        return LTTRUE;
    }

    // Defined below, next to the other object setters; declared here because
    // the physics adapter delegates to it rather than duplicating the clamp.
    LTRESULT LTHost_SetObjectDims(HOBJECT h, LTVector* pDims);

    // ⚠️ THE PHYSICS INTERFACE, same replacement rule: the generated wiring
    // points m_pPhysicsLT at a stub, so this is installed after it. Every
    // method is a thin pass to ltserverphysics.cpp, which owns the collision
    // grid and the per-object state -- this class is the ADAPTER and holds
    // nothing, so there is exactly one place where physics state lives.
    class LTHostServerPhysics : public LTStub_ILTPhysics
    {
    public:
        LTRESULT GetVelocity(HOBJECT hObj, LTVector* pVel) override
        {
            if (!pVel) return LT_INVALIDPARAMS;
            SRec* p = CLTServerObjects::FromHandle(hObj);
            // ⚠️ Written on EVERY path -- the game does `LTVector v;
            // GetVelocity(h,&v);` and then uses v. See HANDOVER §2.
            pVel->Init(p ? p->m_Vel[0] : 0.0f, p ? p->m_Vel[1] : 0.0f,
                       p ? p->m_Vel[2] : 0.0f);
            return p ? LT_OK : LT_ERROR;
        }
        LTRESULT SetVelocity(HOBJECT hObj, LTVector* pVel) override
        {
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || !pVel) return LT_ERROR;
            p->m_Vel[0] = pVel->x; p->m_Vel[1] = pVel->y; p->m_Vel[2] = pVel->z;
            return LT_OK;
        }
        LTRESULT GetAcceleration(HOBJECT hObj, LTVector* pAccel) override
        {
            if (!pAccel) return LT_INVALIDPARAMS;
            SRec* p = CLTServerObjects::FromHandle(hObj);
            pAccel->Init(p ? p->m_Accel[0] : 0.0f, p ? p->m_Accel[1] : 0.0f,
                         p ? p->m_Accel[2] : 0.0f);
            return p ? LT_OK : LT_ERROR;
        }
        LTRESULT SetAcceleration(HOBJECT hObj, LTVector* pAccel) override
        {
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || !pAccel) return LT_ERROR;
            p->m_Accel[0] = pAccel->x; p->m_Accel[1] = pAccel->y; p->m_Accel[2] = pAccel->z;
            return LT_OK;
        }
        LTRESULT GetObjectMass(HOBJECT hObj, float& mass) override
        {
            SRec* p = CLTServerObjects::FromHandle(hObj);
            mass = p ? p->m_fMass : 0.0f;
            return p ? LT_OK : LT_ERROR;
        }
        LTRESULT SetObjectMass(HOBJECT hObj, float mass) override
        {
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p) return LT_ERROR;
            p->m_fMass = mass;
            return LT_OK;
        }
        LTRESULT GetFrictionCoefficient(HOBJECT hObj, float& coeff) override
        {
            SRec* p = CLTServerObjects::FromHandle(hObj);
            coeff = p ? p->m_fFriction : 0.0f;
            return p ? LT_OK : LT_ERROR;
        }
        LTRESULT SetFrictionCoefficient(HOBJECT hObj, float coeff) override
        {
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p) return LT_ERROR;
            p->m_fFriction = coeff;
            return LT_OK;
        }
        LTRESULT GetForceIgnoreLimit(HOBJECT hObj, float& limit) override
        {
            SRec* p = CLTServerObjects::FromHandle(hObj);
            limit = p ? p->m_fForceIgnoreLimit : 0.0f;
            return p ? LT_OK : LT_ERROR;
        }
        LTRESULT SetForceIgnoreLimit(HOBJECT hObj, float limit) override
        {
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p) return LT_ERROR;
            p->m_fForceIgnoreLimit = limit;
            return LT_OK;
        }
        LTRESULT GetObjectDims(HOBJECT hObj, LTVector* pDims) override
        {
            if (!pDims) return LT_INVALIDPARAMS;
            SRec* p = CLTServerObjects::FromHandle(hObj);
            LTHost_GetObjectDims(hObj, pDims);      // brush objects: their box
            return p ? LT_OK : LT_ERROR;
        }
        LTRESULT SetObjectDims(HOBJECT hObj, LTVector* pDims, uint32 flags) override
        {
            (void)flags;
            return LTHost_SetObjectDims(hObj, pDims);
        }
        LTRESULT MoveObject(HOBJECT hObj, LTVector* pPos, uint32 flags) override
        {
            if (!pPos) return LT_INVALIDPARAMS;
            const float want[3] = { pPos->x, pPos->y, pPos->z };

            // ⚠️ MOVEOBJECT_TELEPORT MEANS "DO NOT COLLIDE" (iltphysics.h:14),
            // and CMoveMgr uses it for three of its four calls -- respawn,
            // level start, and being put somewhere by the game. Running those
            // through the mover would let a spawn point a few units inside a
            // floor slide the player somewhere else entirely, once, at the
            // start of the level, which is the hardest kind of bug to catch.
            if (flags & MOVEOBJECT_TELEPORT)
            {
                LTServerPhys_TeleportTo((void*)hObj, want);
                return LT_OK;
            }

            LTServerPhys_MoveTo((void*)hObj, want, 0);
            return LT_OK;
        }
        // ⚠️ NULL IS THE WORLD. A ray that hits level geometry reports no
        // object (see LTHost_IntersectSegment), and `IsMainWorld` -- the only
        // way the game asks this (CommonUtilities.h:39) -- must then say yes,
        // or DoActivate reads a wall as a missed cast.
        LTRESULT IsWorldObject(HOBJECT hObj) override
        {
            // NULL stays "the world" for the paths that still report it
            // (GetStandingOn's zeroed CollisionInfo); see LTHost_WorldRecord.
            return (!hObj || hObj == (HOBJECT)LTHost_WorldRecord()) ? LT_YES : LT_NO;
        }

        LTRESULT GetStandingOn(HOBJECT hObj, CollisionInfo* pInfo) override
        {
            if (!pInfo) return LT_INVALIDPARAMS;
            // ⚠️ ZEROED FIRST, ALWAYS -- CollisionInfo is a bare local at every
            // call site and the game reads m_hObject without checking the
            // return. This is the fix from DEVIATIONS E-001 and it stays.
            memset(pInfo, 0, sizeof(*pInfo));

            // ⚠️ THE WORLD IS NOT AN HOBJECT HERE. We know whether the object
            // rests on world geometry, but there is no object-vs-object
            // collision, so a character on a crate still reports nothing.
            (void)LTServerPhys_GetGround((void*)hObj);
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
    LTHostServerPhysics g_ServerPhysics;

    // The raw function-pointer flavour, which Character, Prop and Body use
    // directly with their own token buffers.
    int LTHost_Parse(char* pCommand, char** ppNewCommandPos, char* pArgBuffer,
                     char** ppArgs, int* pnArgs)
    {
        // ⚠️ The buffer sizes are the ones every caller in the game actually
        // declares -- `char s[PARSE_MAXTOKENS*PARSE_MAXTOKENSIZE]` and
        // `char* p[PARSE_MAXTOKENS]` (Prop.cpp:69, Character.cpp:84). The
        // signature does not carry them, so they are a convention, and writing
        // the convention down is the only thing keeping it true.
        return LTConParse(pCommand, (const char**)ppNewCommandPos,
                          pArgBuffer, PARSE_MAXTOKENS * PARSE_MAXTOKENSIZE,
                          ppArgs, PARSE_MAXTOKENS, pnArgs);
    }

    // ⚠️ THE CLIENT'S USER DATA -- one client, one pointer. The GAME stores its
    // CPlayerObj here (`SetClientUserData(hClient, pPlayer)`) and reads it back
    // in nearly every message handler:
    //
    //     void* pData = g_pLTServer->GetClientUserData(hSender);
    //     CPlayerObj* pPlayer = (CPlayerObj*)pData;   -- GameServerShell.cpp:1854
    //
    // A stub returning 0 makes every one of those handlers take its "no player"
    // path in silence, which is indistinguishable from a player who is not
    // there. It is a single pointer because this port has a single client.
    void*   g_pClientUserData = 0;

    void  LTHost_SetClientUserData(HCLIENT, void* pData)
    {
        // ⚠️ LT_USERDATA=1 -- WHICH CPlayerObj the game stored, against the one
        // the engine believes is the player. Every client->server handler in
        // GameServerShell starts by reading this back, so a STALE pointer here
        // makes each of them act on an object nobody else is using -- and a
        // stale player's m_eState is PS_UNKNOWN, which makes
        // CPlayerObj::Activate return on its second line, silently.
        if (getenv("LT_USERDATA"))
        {
            SRec* p = LTHost_FindPlayerRecord();
            printf("[USERDATA] t=%.3f  the game stored %p; the engine's player "
                   "object is %p%s\n", LTServerHost_Time(), pData,
                   p ? (void*)p->m_pGame : (void*)0,
                   (p && p->m_pGame != pData) ? "   ⚠️  DIFFERENT" : "");
            fflush(stdout);
        }
        g_pClientUserData = pData;
    }
    void* LTHost_GetClientUserData(HCLIENT)              { return g_pClientUserData; }

    // ⚠️ OBJECT LISTS, which the game allocates from the engine and hands back.
    // The contract (iltserver.h:289) is explicit: "You must give back the list
    // to the engine with RelinquishList()". So these are a matched pair, and
    // the list owns its links but never the objects in it -- freeing an object
    // because a list that mentioned it went away would delete live game state.
    // ⚠️ LT_LISTTRACE=1 -- the life of every ObjectList.
    //
    // `KeyFramer::GoActive` returns SILENTLY when `m_pObjectList->m_nInList < 1`
    // (KeyFramer.cpp:639), which leaves a keyframer inactive AND unfinished --
    // and a CinematicTrigger waiting on `m_bFinished` then never ends. There is
    // no way to see that from outside: the list is the game's, the refusal
    // prints nothing, and the symptom is a cinematic that hangs.
    bool LTHost_ListTracing()
    {
        static const bool b = (getenv("LT_LISTTRACE") != 0);
        return b;
    }

    ObjectList* LTHost_CreateObjectList()
    {
        ObjectList* p = new ObjectList;
        p->m_pFirstLink = 0;
        p->m_nInList = 0;
        if (LTHost_ListTracing())
        {
            printf("[LIST] %p created                      t=%.3f\n",
                   (void*)p, LTServerHost_Time());
            fflush(stdout);
        }
        return p;
    }

    void LTHost_RelinquishList(ObjectList* pList)
    {
        if (!pList) return;
        if (LTHost_ListTracing())
        {
            printf("[LIST] %p FREED with %d entr%s        t=%.3f\n",
                   (void*)pList, pList->m_nInList,
                   pList->m_nInList == 1 ? "y" : "ies", LTServerHost_Time());
            fflush(stdout);
        }
        ObjectLink* p = pList->m_pFirstLink;
        while (p) { ObjectLink* pNext = p->m_pNext; delete p; p = pNext; }
        delete pList;
    }

    // ⚠️ THE THIRD ONE, AND ITS ABSENCE STOPPED EVERY KEYFRAMER IN THE GAME.
    // CreateObjectList and RelinquishList were here; the call that puts
    // anything IN a list was a stub that silently did nothing. A KeyFramer
    // then held a list it had successfully allocated, with nothing in it, and
    // `GoActive` refuses on `m_pObjectList->m_nInList < 1`
    // (KeyFramer.cpp:639) -- so four keyframers were created, updated once
    // each, and stood still. Nothing logged, nothing crashed.
    //
    // ⚠️ APPENDED, NOT PREPENDED, and the order is not cosmetic. A keyframer
    // applies its per-object OFFSETS by position in this list, so a reversed
    // list gives every object its neighbour's offset.
    //
    // ⚠️ THE LIST DOES NOT OWN THE OBJECTS -- RelinquishList frees the links
    // and nothing else. An object named by two keyframers is on two lists.
    ObjectLink* LTHost_AddObjectToList(ObjectList* pList, HOBJECT hObj)
    {
        if (!pList || !hObj) return 0;

        ObjectLink* pLink = new ObjectLink;
        pLink->m_hObject = hObj;
        pLink->m_pNext = 0;

        if (!pList->m_pFirstLink)
        {
            pList->m_pFirstLink = pLink;
        }
        else
        {
            ObjectLink* pTail = pList->m_pFirstLink;
            while (pTail->m_pNext) pTail = pTail->m_pNext;
            pTail->m_pNext = pLink;
        }

        ++pList->m_nInList;

        if (LTHost_ListTracing())
        {
            SRec* pAdded = CLTServerObjects::FromHandle(hObj);
            printf("[LIST] %p += '%s' (now %d)             t=%.3f\n",
                   (void*)pList,
                   (pAdded && !pAdded->m_sName.empty())
                       ? pAdded->m_sName.c_str() : "(unnamed)",
                   pList->m_nInList, LTServerHost_Time());
            fflush(stdout);
        }

        return pLink;
    }

    // ⚠️ SAVING OBJECTS, AND IT IS WHY NOTHING IN THIS GAME MOVED.
    //
    // `CCharacter::Update` begins `if (!g_bAutoSaved) return;` (Character.cpp
    // :1217). `g_bAutoSaved` turns true in exactly one place on the single-
    // player path: `CPlayerObj::Save`, which the engine calls while saving.
    // So the chain is
    //
    //   player spawns -> DoAutoSave -> MID_PLAYER_AUTOSAVE to the client
    //     -> CGameClientShell::AutoSave -> SaveGame -> MID_SAVE_GAME back
    //     -> CGameServerShell::HandleSaveGameMsg -> SaveObjects
    //     -> MID_SAVEOBJECT on every object -> CPlayerObj::Save
    //     -> g_bAutoSaved = LTTRUE -> every character in the level starts
    //
    // and with SaveObjects a stub the chain stopped at the last link. Every
    // AI, every civilian and every line of dialogue in the game was waiting on
    // a save that never happened -- which is why M01S01 played ONE voice line
    // and then stood still for the next fifty seconds. The census showed the
    // AI getting their 400 MID_UPDATEs; they were returning on line one.
    //
    // ⚠️ IT REALLY SERIALISES. A SaveObjects that only delivered the message
    // and wrote no file would unblock the game today and lose every save in
    // it, and the difference would not show up until someone tried to load.
    // Length-prefixed, the same shape the class and name fields already use.
    void LTSave_WriteStr(FILE* pF, const std::string& s)
    {
        const unsigned int n = (unsigned int)s.size();
        fwrite(&n, sizeof(n), 1, pF);
        if (n) fwrite(s.c_str(), 1, n, pF);
    }

    bool LTSave_ReadStr(FILE* pF, std::string& sOut)
    {
        unsigned int n = 0;
        if (fread(&n, sizeof(n), 1, pF) != 1) return false;
        if (n > (1u << 20)) return false;           // a length this big is a corrupt file
        sOut.assign((size_t)n, '\0');
        if (n && fread(&sOut[0], 1, n, pF) != n) return false;
        return true;
    }

    // What the world file's objects have become: made, still drawn, gone or
    // hidden. Printed by save AND restore, so a loaded game's link to the
    // world file is one comparison of two lines (5bz).
    std::string LTHost_WorldStateCensus()
    {
        const size_t n = g_Objects.WorldMade().size();
        unsigned int nMade = 0, nLive = 0, nGone = 0, nByGame = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const int st = g_Objects.WorldState(i);
            if (st < 0) continue;
            ++nMade;
            if (st == 0) ++nGone; else if (st == 2) ++nByGame; else ++nLive;
        }
        // LT_CENSUSTRACE=1 -- and every world-file object, one line each, to
        // diff a save's census against its restore's.
        if (getenv("LT_CENSUSTRACE"))
        {
            const std::vector<LTWorldObject>& obj = LTGetWorldScene()->GetWorld().GetObjects();
            for (size_t i = 0; i < n && i < obj.size(); ++i)
            {
                const SRec* r = g_Objects.WorldRecord(i);
                printf("[CENSUS] %4zu %2d %-22s %-24s %s flags 0x%08x\n", i, g_Objects.WorldState(i),
                       obj[i].m_sType.c_str(), obj[i].GetName() ? obj[i].GetName() : "",
                       r ? "record" : "none  ", r ? (unsigned)r->m_Flags : 0u);
            }
            fflush(stdout);
        }
        char sz[160];
        snprintf(sz, sizeof(sz), "world file: %u of %u object(s) made, %u drawn from the file, "
                 "%u by the game, %u gone or hidden", nMade, (unsigned)n, nLive, nByGame, nGone);
        return sz;
    }

    // ---- ATTACHMENTS (see ILTServer::CreateAttachment below) ---- //
    struct Attach
    {
        SRec*       pParent;
        SRec*       pChild;
        std::string sSocket;
        float       ofs[3];
        float       rot[4];
        // The parent's pose when the child was last placed -- see
        // LTHost_UpdateFreeAttachments for why a socket-less child moves
        // only when this changes.
        float       lastPPos[3];
        float       lastPRot[4];
    };
    std::vector<Attach*> g_Attach;

    LTRESULT LTHost_SaveObjects(char* pszSaveFileName, ObjectList* pList,
                                uint32 dwParam, uint32 flags)
    {
        (void)flags;
        if (!pszSaveFileName || !*pszSaveFileName || !pList) return LT_ERROR;

        // ⚠️ THE GAME'S PATH IS A WINDOWS PATH ("Save\\Reload.sav") and the
        // directory may not exist yet. Translating here rather than in fopen's
        // caller keeps the game's own string untouched.
        std::string sPath(pszSaveFileName);
        for (size_t i = 0; i < sPath.size(); ++i)
            if (sPath[i] == '\\') sPath[i] = '/';

        const size_t nSlash = sPath.rfind('/');
        if (nSlash != std::string::npos)
        {
            std::string sDir = sPath.substr(0, nSlash);
            mkdir(sDir.c_str(), 0755);      // EEXIST is the normal answer
        }

        FILE* pF = fopen(sPath.c_str(), "wb");
        if (!pF)
        {
            printf("[SERVER] SaveObjects: cannot write %s\n", sPath.c_str());
            return LT_ERROR;
        }

        // A header nobody but RestoreObjects reads: magic, version, count.
        // The count is written LAST, once it is known.
        fwrite("NLF1SAVE", 1, 8, pF);
        // ⚠️ VERSION 2 adds the ENGINE-OWNED object state -- dims, type,
        // model, skins -- after the flags. Version 1 carried only position,
        // rotation and flags, so a keep-alive came back with dims (0,0,0):
        // see the note on the reader for what that did to the player.
        // ⚠️ VERSION 3: the objects' payloads carry NULL HSTRINGs as NULL (one
        // marker byte -- ltmessage.cpp WriteHString), so a version 1 or 2 file
        // cannot be parsed by this build and is refused below.
        // ⚠️ VERSION 4 (HANDOVER 5bz) adds what a full LOAD needs and a
        // keep-alive never did, after the donor's game_serialize.cpp: the
        // server clock here, and per object its next-update countdown and its
        // special-effect message. Without them a loaded game never ticked.
        // ⚠️ VERSION 5 (5bz) adds the link to the WORLD FILE: the level's
        // name and which of its objects were ever made, here, and per object
        // its world-file index. A loaded game rebuilds every object from the
        // save, and without the link the prop pass drew the level's own props
        // on top of them and every restored door stood closed.
        // ⚠️ VERSION 6 (5cf) adds the object's COLOUR, 4 bytes after the world
        // index, as the donor saves m_ColorR..A: a TranslucentWorldModel sets
        // its alpha only in InitialUpdate, which a loaded object never gets,
        // so without it T01S02's projector beam came back solid after a load.
        // ⚠️ VERSION 7 (5cp) adds ACTIVATION: the level's portal flags after
        // the world link, and per object, after the colour, whether it is
        // asleep and its deactivation time and countdown. Without them an AI
        // that was awake when the game was saved never fell asleep again.
        // ⚠️ VERSION 8 (5cu) adds, after the last object, its ATTACHMENTS and
        // INTER-OBJECT LINKS, as the donor saves them (game_serialize.cpp
        // sm_SaveAttachments / sm_SaveInterlinks). Without them a loaded game
        // had no attachment at all: M04S01's window frame swung and its glass
        // pane (Door13, attached once, on the door's FIRST update) stayed put.
        // ⚠️ VERSION 9 (5dd) adds, per object after the activation, the MAIN
        // ANIMATION: index, looping, and seconds into it -- the donor saves
        // every model's trackers (game_serialize.cpp:508). The game keeps only
        // the TIME: Body::Save writes GetCurAnimTime and Body::Load sets it
        // back, trusting the engine with WHICH animation. Without it every
        // body came back standing in its reference pose (user report).
        const unsigned int nVersion = 9;
        fwrite(&nVersion, sizeof(nVersion), 1, pF);
        const long nCountAt = ftell(pF);
        unsigned int nSaved = 0;
        fwrite(&nSaved, sizeof(nSaved), 1, pF);
        fwrite(&g_fServerTime, sizeof(g_fServerTime), 1, pF);
        LTSave_WriteStr(pF, std::string(LTWorldScene_Name()));
        {
            const std::vector<unsigned char>& made = g_Objects.WorldMade();
            const unsigned int nMade = (unsigned int)made.size();
            fwrite(&nMade, sizeof(nMade), 1, pF);
            if (nMade) fwrite(&made[0], 1, nMade, pF);
        }
        // ---- version 7: the portals, in the level's order ------------- //
        {
            const CLTWorldVis* pVis = LTGetWorldVis();
            const unsigned int nPortals = pVis->NumPortals();
            fwrite(&nPortals, sizeof(nPortals), 1, pF);
            for (unsigned int k = 0; k < nPortals; ++k)
            {
                unsigned int f = pVis->PortalFlagsAt(k);
                fwrite(&f, sizeof(f), 1, pF);
            }
        }

        // ⚠️ THE SET IS DECIDED BEFORE ANY OBJECT WRITES A BYTE, because an
        // object's blob can name any other object in the file -- including one
        // that comes after it. Collect the survivors first, in file order, and
        // hand that order to the reference context: index i+1 in any blob means
        // "the object written i'th here".
        //
        // ⚠️ The filter has to be the SAME on both passes or the indices slide.
        // That is why the skip test below is not repeated -- it is applied once,
        // here, and the second loop walks this vector.
        std::vector<SRec*> order;
        for (ObjectLink* pLink = pList->m_pFirstLink; pLink; pLink = pLink->m_pNext)
        {
            SRec* pRec = CLTServerObjects::FromHandle(pLink->m_hObject);
            if (!pRec || !pRec->m_pGame || !g_Objects.IsLive(pRec)) continue;
            order.push_back(pRec);
        }

        LTSaveRefs_BeginSave(order.empty() ? 0 : (void* const*)&order[0],
                             (unsigned int)order.size());

        for (size_t iObj = 0; iObj < order.size(); ++iObj)
        {
            SRec* pRec = order[iObj];

            // ⚠️ A PLAIN DATA MESSAGE, not one addressed anywhere. The object
            // writes into it and the engine keeps the bytes; sending it would
            // deliver a save blob to a message handler.
            //
            // ⚠️ AND ON FAILURE THE RECORD IS STILL WRITTEN, empty. `continue`
            // here would drop an object from the file while the reference
            // context still holds its index -- every reference after it would
            // then resolve to its neighbour. A silently wrong save is worse
            // than a short one.
            CLTMessage* pMsg = LTMessage_StartData();

            const unsigned int nNullBefore = LTSaveRefs_NullWrites();
            if (pMsg)
                pRec->m_pGame->EngineMessageFn(MID_SAVEOBJECT, (void*)pMsg,
                                               (float)dwParam);
            // LT_SAVEREFTRACE=1 -- [SAVEREF] each reference this object wrote
            // to an object OUTSIDE the save set (it loads as null). 5cu.
            if (getenv("LT_SAVEREFTRACE"))
                for (unsigned int k = nNullBefore; k < LTSaveRefs_NullWrites(); ++k)
                {
                    const SRec* t = (const SRec*)LTSaveRefs_OutsideAt(k);
                    std::string sWhat;
                    if (g_Objects.Contains(t))
                        sWhat = t->m_sName + " (" + ((t->m_pClass && t->m_pClass->m_ClassName)
                                ? t->m_pClass->m_ClassName : "?") + (t->m_bDead ? ", removed" : ", live, not in the list") + ")";
                    else
                        sWhat = "freed " + g_Objects.FreedName(t);
                    printf("[SAVEREF] %s '%s' -> %s\n", (pRec->m_pClass && pRec->m_pClass->m_ClassName)
                           ? pRec->m_pClass->m_ClassName : "?", pRec->m_sName.c_str(), sWhat.c_str());
                    fflush(stdout);
                }

            const char* pClass = (pRec->m_pClass && pRec->m_pClass->m_ClassName)
                               ? pRec->m_pClass->m_ClassName : "";

            const unsigned int nClass = (unsigned int)strlen(pClass);
            const unsigned int nName  = (unsigned int)pRec->m_sName.size();
            const unsigned int nBytes = pMsg ? pMsg->Size() : 0u;

            fwrite(&nClass, sizeof(nClass), 1, pF);
            if (nClass) fwrite(pClass, 1, nClass, pF);
            fwrite(&nName, sizeof(nName), 1, pF);
            if (nName) fwrite(pRec->m_sName.c_str(), 1, nName, pF);
            fwrite(pRec->m_Pos, sizeof(float), 3, pF);
            fwrite(pRec->m_Rot, sizeof(float), 4, pF);
            fwrite(&pRec->m_Flags, sizeof(pRec->m_Flags), 1, pF);
            fwrite(&pRec->m_UserFlags, sizeof(pRec->m_UserFlags), 1, pF);

            // ---- version 2 ------------------------------------------------ //
            fwrite(pRec->m_Dims, sizeof(float), 3, pF);
            fwrite(&pRec->m_Type, sizeof(pRec->m_Type), 1, pF);
            LTSave_WriteStr(pF, pRec->m_sFilename);
            const unsigned int nSkins = (unsigned int)pRec->m_Skins.size();
            fwrite(&nSkins, sizeof(nSkins), 1, pF);
            for (unsigned int k = 0; k < nSkins; ++k)
                LTSave_WriteStr(pF, pRec->m_Skins[k]);
            // ---- version 4 ------------------------------------------------ //
            // The COUNTDOWN, as the donor saves m_NextUpdate: seconds until the
            // next MID_UPDATE, 0 = none. The record holds an absolute time.
            float fCountdown = 0.0f;
            if (pRec->m_fNextUpdate > 0.0f)
            {
                fCountdown = pRec->m_fNextUpdate - g_fServerTime;
                if (fCountdown < 0.0001f) fCountdown = 0.0001f;      // due: next frame
            }
            fwrite(&fCountdown, sizeof(fCountdown), 1, pF);
            const unsigned int nSFX = (unsigned int)pRec->m_SFXMsg.size();
            fwrite(&nSFX, sizeof(nSFX), 1, pF);
            if (nSFX) fwrite(&pRec->m_SFXMsg[0], 1, nSFX, pF);
            // ---- version 5 ------------------------------------------------ //
            const int nWorldIndex = pRec->m_nWorldIndex;
            fwrite(&nWorldIndex, sizeof(nWorldIndex), 1, pF);
            // ---- version 6 ------------------------------------------------ //
            fwrite(pRec->m_Color, 1, 4, pF);
            // ---- version 7 ------------------------------------------------ //
            fwrite(&pRec->m_nInactive, 1, 1, pF);
            fwrite(&pRec->m_fDeactTime, sizeof(float), 1, pF);
            fwrite(&pRec->m_fDeactTimer, sizeof(float), 1, pF);
            // ---- version 9 ------------------------------------------------ //
            // ⚠️ ELAPSED, not the start: a level change's keep-alives keep the
            // running clock, so an absolute start would mean nothing there.
            // Negative while a blend-in is still running (see
            // LTHost_AnimStartAfterBlend), and kept that way.
            {
                const int nAnim = pRec->m_nAnim;
                const unsigned char bLoop = pRec->m_bAnimLooping ? 1 : 0;
                const float fElapsed = g_fServerTime - pRec->m_fAnimStart;
                fwrite(&nAnim, sizeof(nAnim), 1, pF);
                fwrite(&bLoop, 1, 1, pF);
                fwrite(&fElapsed, sizeof(fElapsed), 1, pF);
            }

            fwrite(&nBytes, sizeof(nBytes), 1, pF);
            if (nBytes) fwrite(pMsg->Bytes(), 1, nBytes, pF);

            if (pMsg) LTMessage_Free(pMsg);
            ++nSaved;
        }

        const unsigned int nRefs = LTSaveRefs_Written();
        const unsigned int nRefsNull = LTSaveRefs_NullWrites();
        LTSaveRefs_End();

        // ---- version 8: attachments, then links, by index into the file ---- //
        unsigned int nAttSaved = 0, nLinkSaved = 0;
        {
            std::map<const SRec*, int> index;
            for (size_t k = 0; k < order.size(); ++k) index[order[k]] = (int)k;

            std::vector<const Attach*> att;
            for (size_t k = 0; k < g_Attach.size(); ++k)
                if (index.count(g_Attach[k]->pParent) && index.count(g_Attach[k]->pChild))
                    att.push_back(g_Attach[k]);
            const unsigned int nAtt = (unsigned int)att.size();
            fwrite(&nAtt, sizeof(nAtt), 1, pF);
            for (size_t k = 0; k < att.size(); ++k)
            {
                const int iParent = index[att[k]->pParent], iChild = index[att[k]->pChild];
                fwrite(&iParent, sizeof(iParent), 1, pF);
                fwrite(&iChild, sizeof(iChild), 1, pF);
                LTSave_WriteStr(pF, att[k]->sSocket);
                fwrite(att[k]->ofs, sizeof(float), 3, pF);
                fwrite(att[k]->rot, sizeof(float), 4, pF);
            }
            nAttSaved = nAtt;

            std::vector<std::pair<int, int> > links;
            const std::vector<std::pair<SRec*, SRec*> >& L = g_Objects.Links();
            for (size_t k = 0; k < L.size(); ++k)
                if (index.count(L[k].first) && index.count(L[k].second))
                    links.push_back(std::make_pair(index[L[k].first], index[L[k].second]));
            const unsigned int nLinks = (unsigned int)links.size();
            fwrite(&nLinks, sizeof(nLinks), 1, pF);
            for (size_t k = 0; k < links.size(); ++k)
            {
                fwrite(&links[k].first, sizeof(int), 1, pF);
                fwrite(&links[k].second, sizeof(int), 1, pF);
            }
            nLinkSaved = nLinks;
        }

        fseek(pF, nCountAt, SEEK_SET);
        fwrite(&nSaved, sizeof(nSaved), 1, pF);
        fclose(pF);

        printf("[SERVER] saved %u object(s) to %s (%u object reference(s), "
               "%u outside the set; %u attachment(s), %u link(s); %s)\n", nSaved, sPath.c_str(),
               nRefs, nRefsNull, nAttSaved, nLinkSaved, LTHost_WorldStateCensus().c_str());
        fflush(stdout);
        return LT_OK;
    }

    // ⚠️ RESTOREOBJECTS -- THE OTHER HALF, AND IT IS WHAT ENDS A LEVEL.
    //
    // This is not "load game" as a menu feature. NOLF moves between levels
    // through the SAME path: the tutorial's opening cinematic is skipped, the
    // level's own script fires its exit, and `CGameServerShell` saves the
    // KEEP-ALIVE objects, loads the next world, and restores them into it.
    // With this a stub the sequence got as far as
    //
    //     [SERVER] saved 3 object(s) to Save/Kpalv.sav
    //     [SERVER] LoadWorld("Worlds\t01s02", flags 0x5)
    //     Load Game Error: Couldn't restore keepalives
    //
    // and then the server abandoned the load with the new world half-built and
    // no client in it -- which the person at the keyboard sees as a BLACK
    // SCREEN with no error at all. Every level transition in the game ended
    // here.
    //
    // ⚠️ KEEP-ALIVES DO NOT EXIST IN THE NEW WORLD, so this has to CREATE as
    // well as restore. That is the whole point of them: they are the objects
    // that travel between levels (the player's weapons and inventory among
    // them), saved out of the old world and constructed into the new one.
    //
    // ⚠️ THE HANDSHAKE IS NOT THE WORLD-FILE ONE. An object built from a save
    // gets PRECREATE_SAVEGAME and INITIALUPDATE_SAVEGAME, and the game checks
    // for exactly those: `if (fData != INITIALUPDATE_SAVEGAME)` guards the
    // first-time setup in object after object, because a restored object must
    // NOT re-run the setup its saved state already reflects. Sending the
    // world-file values instead would give every restored object two
    // conflicting initialisations and look like data corruption.
    LTRESULT LTHost_RestoreObjects(char* pszRestoreFileName, uint32 dwParam,
                                   uint32 flags)
    {

        if (!pszRestoreFileName || !*pszRestoreFileName) return LT_ERROR;

        std::string sPath(pszRestoreFileName);
        for (size_t i = 0; i < sPath.size(); ++i)
            if (sPath[i] == '\\') sPath[i] = '/';

        FILE* pF = fopen(sPath.c_str(), "rb");
        if (!pF)
        {
            // ⚠️ NOT AN ERROR WORTH FAILING THE LOAD OVER when the file simply
            // is not there: the game asks for keep-alives on a level that may
            // never have written any. Returning LT_ERROR here would abort the
            // transition for the ordinary case of "nothing to carry over".
            printf("[SERVER] RestoreObjects: no %s -- nothing to restore\n",
                   sPath.c_str());
            fflush(stdout);
            return LT_OK;
        }

        char szMagic[8] = { 0 };
        unsigned int nVersion = 0, nCount = 0;
        if (fread(szMagic, 1, 8, pF) != 8 ||
            memcmp(szMagic, "NLF1SAVE", 8) != 0 ||
            fread(&nVersion, sizeof(nVersion), 1, pF) != 1 ||
            fread(&nCount, sizeof(nCount), 1, pF) != 1)
        {
            fclose(pF);
            printf("[SERVER] RestoreObjects: %s is not a save file\n", sPath.c_str());
            fflush(stdout);
            return LT_ERROR;
        }

        // ⚠️ VERSION 1 IS STILL READ, with the version-2 fields defaulted --
        // a save written before this build is not a corrupt save. Its objects
        // come back with the dims they get from the world, which is right for
        // everything the level places and wrong only for a keep-alive.
        // ⚠️ VERSIONS 1 AND 2 ARE REFUSED since version 3 (HANDOVER 5bz): their
        // payloads write a NULL HSTRING as "" with no marker byte, and reading
        // them as version 3 would shift every field after the first string.
        // ⚠️ VERSION 3 IS REFUSED since version 4 (5bz): it has no clock, no
        // next-update countdowns and no effect messages, and its records are
        // shorter -- read as version 4 they would shift.
        // ⚠️ VERSION 4 IS REFUSED since version 5 (5bz) for the same reason:
        // no world link, shorter header and records.
        // ⚠️ VERSION 5 IS STILL READ (5cf): version 6 only appends the colour
        // to each record, so a version-5 save loads with every object white
        // and opaque -- what it got before the colour was kept at all.
        // ⚠️ VERSION 8 IS STILL READ (5dd): version 9 only appends the
        // animation to each record, so an older save loads as before -- its
        // bodies stand until the game sets an animation.
        if (nVersion < 5 || nVersion > 9)
        {
            fclose(pF);
            printf("[SERVER] RestoreObjects: %s is version %u, this build reads 5 to 9\n",
                   sPath.c_str(), nVersion);
            fflush(stdout);
            return LT_ERROR;
        }
        float fSavedTime = 0.0f;
        if (fread(&fSavedTime, sizeof(fSavedTime), 1, pF) != 1)
        {
            fclose(pF);
            printf("[SERVER] RestoreObjects: %s has no clock -- refusing\n", sPath.c_str());
            fflush(stdout);
            return LT_ERROR;
        }
        std::string sSavedWorld;
        std::vector<unsigned char> savedMade;
        {
            unsigned int nMade = 0;
            bool bOK = LTSave_ReadStr(pF, sSavedWorld)
                    && fread(&nMade, sizeof(nMade), 1, pF) == 1 && nMade <= (1u << 20);
            if (bOK && nMade)
            {
                savedMade.resize(nMade);
                bOK = fread(&savedMade[0], 1, nMade, pF) == nMade;
            }
            if (!bOK)
            {
                fclose(pF);
                printf("[SERVER] RestoreObjects: %s has no world link -- refusing\n", sPath.c_str());
                fflush(stdout);
                return LT_ERROR;
            }
        }
        // Version 7: the portal flags. Applied after the objects are made.
        std::vector<unsigned int> savedPortals;
        bool bHavePortals = false;
        if (nVersion >= 7)
        {
            unsigned int nPortals = 0;
            bool bOK = fread(&nPortals, sizeof(nPortals), 1, pF) == 1 && nPortals <= 4096;
            if (bOK && nPortals)
            {
                savedPortals.resize(nPortals);
                bOK = fread(&savedPortals[0], sizeof(unsigned int), nPortals, pF) == nPortals;
            }
            if (!bOK)
            {
                fclose(pF);
                printf("[SERVER] RestoreObjects: %s has no portal block -- refusing\n", sPath.c_str());
                fflush(stdout);
                return LT_ERROR;
            }
            bHavePortals = true;
        }

        // ⚠️ READ THE WHOLE FILE FIRST, THEN CREATE, THEN LOAD -- THREE PHASES,
        // AND THE ORDER IS THE WHOLE POINT.
        //
        // An object's saved blob can name any other object in the file, by
        // index, including one that comes AFTER it. So every object has to
        // exist before any object is asked to load. Doing it in one pass was
        // the bug that crashed the first real level change: the weapon model
        // was restored after the attachment that referenced it.
        struct SavedRec
        {
            std::string  sClass, sName;
            float        pos[3], rot[4];
            uint32       dwFlags, dwUserFlags;

            // ⚠️ ENGINE STATE, version 2 on. bHasDims says whether the file
            // actually carried it, so a version-1 save does not silently
            // resize every object it restores to nothing.
            bool          bHasDims;
            float         dims[3];
            unsigned char type;
            std::string   sFilename;
            std::vector<std::string> skins;
            // ⚠️ VERSION 4: seconds to the next MID_UPDATE (0 = none) and the
            // object's special-effect message.
            float         fCountdown;
            std::vector<unsigned char> sfx;
            // ⚠️ VERSION 5: the world-file object this was made from, -1 none.
            int           nWorldIndex;
            // ⚠️ VERSION 6: r g b a bytes; bHasColor false for a version 5 file.
            bool          bHasColor;
            unsigned char color[4];
            // ⚠️ VERSION 7: asleep or awake, deactivation time and countdown.
            bool          bHasActivity;
            unsigned char nInactive;
            float         fDeactTime, fDeactTimer;
            // ⚠️ VERSION 9: the main animation, looping, seconds into it.
            bool          bHasAnim;
            int           nAnim;
            unsigned char nAnimLoop;
            float         fAnimElapsed;
            std::vector<unsigned char> blob;
        };

        std::vector<SavedRec> recs;
        recs.reserve(nCount);

        bool bTruncated = false;
        for (unsigned int i = 0; i < nCount; ++i)
        {
            unsigned int nClass = 0, nName = 0, nBytes = 0;
            char szClass[256] = { 0 };
            char szName[256]  = { 0 };
            SavedRec r;
            r.dwFlags = r.dwUserFlags = 0;
            r.bHasDims = false;
            r.dims[0] = r.dims[1] = r.dims[2] = 0.0f;
            r.type = 0;
            r.fCountdown = 0.0f;
            r.nWorldIndex = -1;
            r.bHasColor = false;
            r.color[0] = r.color[1] = r.color[2] = r.color[3] = 255;
            r.bHasActivity = false;
            r.nInactive = 0;
            r.fDeactTime = r.fDeactTimer = 0.0f;
            r.bHasAnim = false;
            r.nAnim = -1;
            r.nAnimLoop = 0;
            r.fAnimElapsed = 0.0f;
            if (fread(&nClass, sizeof(nClass), 1, pF) != 1) { bTruncated = true; break; }
            if (nClass >= sizeof(szClass))                  { bTruncated = true; break; }
            if (nClass && fread(szClass, 1, nClass, pF) != nClass) { bTruncated = true; break; }

            if (fread(&nName, sizeof(nName), 1, pF) != 1)   { bTruncated = true; break; }
            if (nName >= sizeof(szName))                    { bTruncated = true; break; }
            if (nName && fread(szName, 1, nName, pF) != nName) { bTruncated = true; break; }

            if (fread(r.pos, sizeof(float), 3, pF) != 3)    { bTruncated = true; break; }
            if (fread(r.rot, sizeof(float), 4, pF) != 4)    { bTruncated = true; break; }
            if (fread(&r.dwFlags, sizeof(r.dwFlags), 1, pF) != 1)         { bTruncated = true; break; }
            if (fread(&r.dwUserFlags, sizeof(r.dwUserFlags), 1, pF) != 1) { bTruncated = true; break; }

            if (nVersion >= 2)
            {
                unsigned int nSkins = 0;
                if (fread(r.dims, sizeof(float), 3, pF) != 3)      { bTruncated = true; break; }
                if (fread(&r.type, sizeof(r.type), 1, pF) != 1)    { bTruncated = true; break; }
                if (!LTSave_ReadStr(pF, r.sFilename))              { bTruncated = true; break; }
                if (fread(&nSkins, sizeof(nSkins), 1, pF) != 1)    { bTruncated = true; break; }
                if (nSkins > 64)                                   { bTruncated = true; break; }
                bool bBad = false;
                for (unsigned int k = 0; k < nSkins; ++k)
                {
                    std::string sSkin;
                    if (!LTSave_ReadStr(pF, sSkin)) { bBad = true; break; }
                    r.skins.push_back(sSkin);
                }
                if (bBad) { bTruncated = true; break; }
                r.bHasDims = true;
            }
            {
                unsigned int nSFX = 0;
                if (fread(&r.fCountdown, sizeof(r.fCountdown), 1, pF) != 1) { bTruncated = true; break; }
                if (fread(&nSFX, sizeof(nSFX), 1, pF) != 1)                 { bTruncated = true; break; }
                if (nSFX > (1u << 20))                                      { bTruncated = true; break; }
                r.sfx.resize(nSFX);
                if (nSFX && fread(&r.sfx[0], 1, nSFX, pF) != nSFX)          { bTruncated = true; break; }
                if (fread(&r.nWorldIndex, sizeof(r.nWorldIndex), 1, pF) != 1) { bTruncated = true; break; }
                if (nVersion >= 6)
                {
                    if (fread(r.color, 1, 4, pF) != 4) { bTruncated = true; break; }
                    r.bHasColor = true;
                }
                if (nVersion >= 7)
                {
                    if (fread(&r.nInactive, 1, 1, pF) != 1)                     { bTruncated = true; break; }
                    if (fread(&r.fDeactTime, sizeof(float), 1, pF) != 1)        { bTruncated = true; break; }
                    if (fread(&r.fDeactTimer, sizeof(float), 1, pF) != 1)       { bTruncated = true; break; }
                    r.bHasActivity = true;
                }
                if (nVersion >= 9)
                {
                    if (fread(&r.nAnim, sizeof(r.nAnim), 1, pF) != 1)               { bTruncated = true; break; }
                    if (fread(&r.nAnimLoop, 1, 1, pF) != 1)                         { bTruncated = true; break; }
                    if (fread(&r.fAnimElapsed, sizeof(float), 1, pF) != 1)          { bTruncated = true; break; }
                    r.bHasAnim = true;
                }
            }
            if (fread(&nBytes, sizeof(nBytes), 1, pF) != 1) { bTruncated = true; break; }

            r.blob.resize(nBytes);
            if (nBytes && fread(&r.blob[0], 1, nBytes, pF) != nBytes) { bTruncated = true; break; }

            r.sClass = szClass;
            r.sName  = szName;
            recs.push_back(r);
        }
        // version 8 (5cu): attachments and links, by index into recs
        struct SavedAtt { int iP, iC; std::string sSock; float ofs[3], rot[4]; };
        std::vector<SavedAtt> savedAtt;
        std::vector<std::pair<int, int> > savedLinks;
        if (nVersion >= 8 && !bTruncated)
        {
            unsigned int n = 0;
            if (fread(&n, sizeof(n), 1, pF) != 1) bTruncated = true;
            for (unsigned int k = 0; k < n && !bTruncated; ++k)
            {
                SavedAtt t;
                if (fread(&t.iP, sizeof(int), 1, pF) != 1 || fread(&t.iC, sizeof(int), 1, pF) != 1
                    || !LTSave_ReadStr(pF, t.sSock)
                    || fread(t.ofs, sizeof(float), 3, pF) != 3 || fread(t.rot, sizeof(float), 4, pF) != 4)
                { bTruncated = true; break; }
                savedAtt.push_back(t);
            }
            if (!bTruncated && fread(&n, sizeof(n), 1, pF) != 1) bTruncated = true;
            for (unsigned int k = 0; k < n && !bTruncated; ++k)
            {
                int o = 0, l = 0;
                if (fread(&o, sizeof(int), 1, pF) != 1 || fread(&l, sizeof(int), 1, pF) != 1)
                { bTruncated = true; break; }
                savedLinks.push_back(std::make_pair(o, l));
            }
        }
        fclose(pF);

        // ⚠️ A SHORT FILE IS A REFUSAL, not a partial restore. Indices in every
        // remaining blob refer to objects that are not there, so "restore what
        // we got" means handing the game references that silently resolve to
        // the wrong object.
        if (bTruncated || recs.size() != nCount)
        {
            printf("[SERVER] RestoreObjects: %s is truncated -- %u of %u "
                   "record(s) readable; refusing\n",
                   sPath.c_str(), (unsigned)recs.size(), nCount);
            fflush(stdout);
            return LT_ERROR;
        }

        unsigned int nRestored = 0, nCreated = 0, nFailed = 0, nSFXResent = 0;

        // ⚠️ THE CLOCK FIRST, before anything is created: an object's Load
        // reads back ABSOLUTE server times it saved (a door's stop time, an
        // AI's next think), so the clock must say what it said then. Only when
        // the game asks (RESTOREOBJECTS_RESTORETIME -- a loaded game; a level
        // change's keep-alives pass 0 and keep the running clock), as the
        // donor does (game_serialize.cpp). LT_NORESTORETIME=1 = keep the clock.
        {
            static const bool s_bNoTime = []() {
                const bool b = (getenv("LT_NORESTORETIME") != 0);
                if (b) { printf("[SERVER] LT_NORESTORETIME=1 -- a loaded game keeps the running clock (old)\n"); fflush(stdout); }
                return b; }();
            if ((flags & RESTOREOBJECTS_RESTORETIME) && !s_bNoTime)
            {
                printf("[SERVER] RestoreObjects: server clock %.3f -> %.3f (the saved game's)\n",
                       g_fServerTime, fSavedTime);
                fflush(stdout);
                g_fServerTime = fSavedTime;
            }
        }

        // ⚠️ THE WORLD LINK GOES BACK ONLY INTO THE SAME LEVEL, LOADED
        // GEOMETRY-ONLY -- a loaded game. A level change's keep-alives come
        // from ANOTHER world (their indices mean nothing here) into one whose
        // own objects were just made. LT_NOWORLDLINK=1 = the old restore
        // (no link, restored brushes left in the static world).
        static const bool s_bNoLink = []() {
            const bool b = (getenv("LT_NOWORLDLINK") != 0);
            if (b) { printf("[SERVER] LT_NOWORLDLINK=1 -- a loaded game is not linked back to the world file (old)\n"); fflush(stdout); }
            return b; }();
        const bool bLinkWorld = !s_bNoLink && !sSavedWorld.empty()
                             && strcasecmp(sSavedWorld.c_str(), LTWorldScene_Name()) == 0
                             && !g_Objects.HasWorldObjects() && !savedMade.empty();
        if (bLinkWorld) g_Objects.SetWorldMade(savedMade);
        unsigned int nLinked = 0;
        unsigned int nAsleep = 0;      // version 7: restored asleep (5cp)
        unsigned int nAnimRestored = 0;    // version 9: an animation set back (5dd)

        // ---- PHASE 2: find or create every object, in file order ---------- //
        //
        // ⚠️ THE VECTOR IS INDEX-ALIGNED WITH THE FILE and stays that way even
        // for entries that fail: a failure stores null rather than shrinking
        // the vector, or every index after it would resolve to its neighbour.
        std::vector<void*> handles(recs.size(), (void*)0);

        // ⚠️ ONLY WHAT EXISTED BEFORE THIS RESTORE CAN BE MATCHED (5bz). A
        // level reuses names -- M01S01 has two Triggers "locked", two
        // "locked2" -- and matching against the objects this restore had
        // just CREATED loaded the second one's save into the first: seven
        // objects of a loaded M01S01 silently became duplicates of others
        // ("580 created" of 587). LT_RESTOREMATCHALL=1 = the old matching.
        static const bool s_bMatchAll = []() {
            const bool b = (getenv("LT_RESTOREMATCHALL") != 0);
            if (b) { printf("[SERVER] LT_RESTOREMATCHALL=1 -- a restore may match objects it created itself (old)\n"); fflush(stdout); }
            return b; }();
        std::set<const SRec*> before;
        for (unsigned int k = 0; k < g_Objects.ObjectCount(); ++k)
            if (const SRec* p = g_Objects.RecordAt(k)) before.insert(p);

        for (size_t i = 0; i < recs.size(); ++i)
        {
            const SavedRec& r = recs[i];

            // ⚠️ MATCH ON NAME **AND** CLASS. A name alone is not an identity:
            // two levels both have a "Player", and a keep-alive restored onto
            // the wrong object would deliver one object's saved bytes to
            // another's Load -- which reads whatever it expects off the front
            // and produces plausible garbage rather than an error.
            SRec* pTarget = 0;
            if (!r.sName.empty())
            {
                for (unsigned int k = 0; k < g_Objects.ObjectCount(); ++k)
                {
                    SRec* p = g_Objects.RecordAt(k);
                    if (!p || !g_Objects.IsLive(p) || !p->m_pGame) continue;
                    if (!s_bMatchAll && !before.count(p)) continue;
                    if (strcasecmp(p->m_sName.c_str(), r.sName.c_str()) != 0) continue;

                    const char* pCls = (p->m_pClass && p->m_pClass->m_ClassName)
                                     ? p->m_pClass->m_ClassName : "";
                    if (!r.sClass.empty() && strcasecmp(pCls, r.sClass.c_str()) != 0)
                        continue;

                    pTarget = p;
                    break;
                }
            }

            // Not in this world -- build it. This is the keep-alive path.
            if (!pTarget && !r.sClass.empty())
            {
                ClassDef* pClass = g_Objects.FindClass(r.sClass.c_str());
                if (!pClass) { ++nFailed; continue; }

                ObjectCreateStruct cs;
                INIT_OBJECTCREATESTRUCT(cs);
                strncpy(cs.m_Name, r.sName.c_str(), sizeof(cs.m_Name) - 1);
                cs.m_Pos.x = r.pos[0]; cs.m_Pos.y = r.pos[1]; cs.m_Pos.z = r.pos[2];
                cs.m_Rotation.m_Quat[0] = r.rot[0];
                cs.m_Rotation.m_Quat[1] = r.rot[1];
                cs.m_Rotation.m_Quat[2] = r.rot[2];
                cs.m_Rotation.m_Quat[3] = r.rot[3];
                cs.m_Flags = r.dwFlags;

                BaseClass* pObj = g_Objects.Create(pClass, &cs);
                if (!pObj) { ++nFailed; continue; }

                // ⚠️ RecordOf TAKEN NOW, not m_Objects.back() later. Precreate
                // can create further objects -- see CreateWorldObjects and
                // HANDOVER.md §5y, where exactly this cost a level's worth of
                // misnamed keyframers.
                SRec* pMine = g_Objects.RecordOf(pObj);

                pObj->EngineMessageFn(MID_PRECREATE, &cs, PRECREATE_SAVEGAME);
                if (pMine)
                {
                    g_Objects.Apply(pMine, &cs);
                    pMine->m_UserFlags = r.dwUserFlags;
                    // ⚠️ THE SAVED NEXT UPDATE, before the object's own
                    // initial update and load -- either may still call
                    // SetNextUpdate and win, as in the donor (the create
                    // struct carries m_NextUpdate). Without it nothing
                    // restored ever ticked: the player never ran
                    // HandleGameRestore and a loaded game stayed black.
                    static const bool s_bNoCountdown = []() {
                        const bool b = (getenv("LT_NORESTORECOUNTDOWN") != 0);
                        if (b) { printf("[SERVER] LT_NORESTORECOUNTDOWN=1 -- a restored object does not get its saved next update (old)\n"); fflush(stdout); }
                        return b; }();
                    if (!s_bNoCountdown)
                        pMine->m_fNextUpdate = (r.fCountdown > 0.0f) ? g_fServerTime + r.fCountdown : 0.0f;
                    if (bLinkWorld && r.nWorldIndex >= 0)
                    {
                        g_Objects.RelinkWorld(pMine, r.nWorldIndex, LTGetWorldScene()->GetWorld());
                        if (pMine->m_nWorldIndex == r.nWorldIndex) ++nLinked;
                    }
                }
                pObj->EngineMessageFn(MID_INITIALUPDATE, 0, INITIALUPDATE_SAVEGAME);

                // ⚠️ BEFORE the MID_LOADOBJECT below, not after: CCharacter
                // SAVES its active count (Character.cpp:2859). Activating here
                // takes it 0 -> 1 and the load writes back the saved 1;
                // activating after the load would make it 2 and print "Active
                // count out of sync".
                LTServer_SendActivating(pObj);

                pTarget = pMine;
                ++nCreated;
            }

            if (!pTarget || !pTarget->m_pGame) { ++nFailed; continue; }

            // ⚠️ POSITION AND FLAGS BEFORE THE LOAD, so an object whose Load
            // reads its own position back gets the engine's copy already
            // agreeing with the file. They are engine state and the game
            // cannot set them from inside Load.
            pTarget->m_Pos[0] = r.pos[0];
            pTarget->m_Pos[1] = r.pos[1];
            pTarget->m_Pos[2] = r.pos[2];
            pTarget->m_Rot[0] = r.rot[0];
            pTarget->m_Rot[1] = r.rot[1];
            pTarget->m_Rot[2] = r.rot[2];
            pTarget->m_Rot[3] = r.rot[3];
            pTarget->m_Flags     = r.dwFlags;
            pTarget->m_UserFlags = r.dwUserFlags;

            // ⚠️ DIMS ARE ENGINE STATE AND NOTHING ELSE RESTORES THEM, which
            // is why the player fell out of the UNITY lobby.
            //
            // A level change carries the player across in Kpalv.sav and the
            // object is BUILT HERE, not by the world. CCharacter sets its
            // collision size in UpdateDims (Character.cpp:814), reached from
            // the normal InitialUpdate -- but a restored object is given
            // INITIALUPDATE_SAVEGAME, which deliberately skips that work
            // because the engine is supposed to have the dims already. With
            // version 1 it did not: the player came back (0,0,0), a box that
            // size is inside every floor in the level, and the game's own
            // MoveObjectToFloor then dropped it to y = dims.y + 0.1 = 0.1
            // before it fell through the world. T01S01 -> T01S02 measured
            // 50,237 units of falling; with the dims carried across, 39.
            //
            // ⚠️ Only when the file HAS them (version 2) and only when they
            // are non-zero -- restoring a zero would be the same bug wearing
            // a different hat.
            if (r.bHasDims &&
                (r.dims[0] != 0.0f || r.dims[1] != 0.0f || r.dims[2] != 0.0f))
            {
                pTarget->m_Dims[0] = r.dims[0];
                pTarget->m_Dims[1] = r.dims[1];
                pTarget->m_Dims[2] = r.dims[2];
            }

            // The model the object is drawn as. Without it a restored object
            // is a position the client can query and nothing it can draw.
            if (r.bHasDims)
            {
                if (r.type) pTarget->m_Type = r.type;
                if (!r.sFilename.empty()) pTarget->m_sFilename = r.sFilename;
                if (!r.skins.empty())     pTarget->m_Skins     = r.skins;
            }

            // The colour, before the object loads -- as the donor restores
            // m_ColorR..A with the rest of the engine state.
            // LT_NORESTORECOLOR=1 = the saved colour is ignored (says so).
            static const bool s_bNoColor = []() {
                const bool b = (getenv("LT_NORESTORECOLOR") != 0);
                if (b) { printf("[SERVER] LT_NORESTORECOLOR=1 -- restored objects keep the default colour (old)\n"); fflush(stdout); }
                return b; }();
            if (r.bHasColor && !s_bNoColor)
                for (int k = 0; k < 4; ++k) pTarget->m_Color[k] = r.color[k];

            // Version 9 (5dd): the main animation, before the object loads --
            // Body::Load then sets its saved time on it (SetCurAnimTime), as it
            // does in LT2. The key cursor is put at "now", as the donor saves
            // the tracker's m_CurKey: keys already played before the save must
            // not play again (a shot, a footstep, a death cry).
            // LT_NORESTOREANIM=1 = the animation is not restored (says so).
            static const bool s_bNoAnim = []() {
                const bool b = (getenv("LT_NORESTOREANIM") != 0);
                if (b) { printf("[SERVER] LT_NORESTOREANIM=1 -- restored models get no saved animation (old)\n"); fflush(stdout); }
                return b; }();
            if (r.bHasAnim && !s_bNoAnim)
            {
                pTarget->m_nAnim        = r.nAnim;
                pTarget->m_bAnimLooping = (r.nAnimLoop != 0);
                pTarget->m_fAnimStart   = g_fServerTime - r.fAnimElapsed;
                LTHost_KeysDeliveredToNow(pTarget);
                if (r.nAnim >= 0) ++nAnimRestored;
            }

            // Version 7: when it falls asleep, and whether it IS asleep.
            // ⚠️ ASLEEP BEFORE THE LOAD, for the reason the activation above
            // is: CCharacter SAVES its active count (Character.cpp:2859). It
            // was made awake (0 -> 1); MID_DEACTIVATING here takes it back to
            // 0 and the load writes the saved 0 over it. Put to sleep AFTER
            // the load, it went to -1 -- "Active count out of sync" for every
            // sleeping AI in a quick load (measured, M01S02).
            if (r.bHasActivity)
            {
                pTarget->m_fDeactTime  = r.fDeactTime;
                pTarget->m_fDeactTimer = r.fDeactTimer;
                if (r.nInactive)
                {
                    g_Objects.SetActivity(pTarget, r.nInactive, "restored");
                    ++nAsleep;
                }
            }

            handles[i] = (void*)pTarget;
        }

        // ---- PHASE 3: now, and only now, let them load -------------------- //
        //
        // ⚠️ THE CONTEXT IS OPEN FOR THE WHOLE PHASE, because a reference read
        // by object 0 may name object 9.
        LTSaveRefs_BeginLoad(handles.empty() ? 0 : &handles[0],
                             (unsigned int)handles.size());

        for (size_t i = 0; i < recs.size(); ++i)
        {
            SRec* pTarget = (SRec*)handles[i];
            if (!pTarget) continue;

            const SavedRec& r = recs[i];

            // ⚠️ A DATA MESSAGE POSITIONED AT ZERO. PutRaw appends and the read
            // cursor starts at the front, so the object reads back exactly the
            // bytes its Save wrote -- same order, same primitives.
            CLTMessage* pMsg = LTMessage_StartData();
            if (!pMsg) { ++nFailed; continue; }
            if (!r.blob.empty()) pMsg->PutRaw(&r.blob[0], (unsigned int)r.blob.size());

            pTarget->m_pGame->EngineMessageFn(MID_LOADOBJECT, (void*)pMsg,
                                              (float)dwParam);

            // ⚠️ AN OVERRUN MEANS SAVE AND LOAD DISAGREE, and it is the one
            // failure of this whole path that is otherwise silent: the object
            // gets zeros for everything past the end and carries on looking
            // fine. Report it per object -- which object it was is the entire
            // diagnostic.
            if (pMsg->Overran())
                printf("[SERVER] ⚠️  RestoreObjects: '%s' (%s) read past the end "
                       "of its %u saved byte(s)\n",
                       r.sName.empty() ? "(unnamed)" : r.sName.c_str(),
                       r.sClass.empty() ? "?" : r.sClass.c_str(),
                       (unsigned)r.blob.size());

            LTMessage_Free(pMsg);

            // ⚠️ AND ITS SPECIAL EFFECT, as the donor restores it with the
            // object: a restored object's InitialUpdate(SAVEGAME) does not make
            // its client effect again (characters, PolyGrids, volume FX rely on
            // the engine). Kept on the record and queued once; the client takes
            // an object's effect only once (skeleton_host.cpp kLTMsg_SFX), so
            // a game that ALSO re-sends one changes nothing.
            static const bool s_bNoSFX = []() {
                const bool b = (getenv("LT_NORESTORESFX") != 0);
                if (b) { printf("[SERVER] LT_NORESTORESFX=1 -- a restore does not re-send saved effects\n"); fflush(stdout); }
                return b; }();
            if (!r.sfx.empty() && !s_bNoSFX)
            {
                pTarget->m_SFXMsg = r.sfx;
                LTMessage_QueueSFX(pTarget->m_pGame, &r.sfx[0], (unsigned int)r.sfx.size());
                ++nSFXResent;
            }
            ++nRestored;
        }

        const unsigned int nRefsRead = LTSaveRefs_Read();
        const unsigned int nRefsBad  = LTSaveRefs_Unresolved();
        LTSaveRefs_End();

        // ⚠️ ATTACHMENTS AND LINKS COME BACK (v8, 5cu) once every object
        // exists -- the donor's sm_RestoreAttachments / sm_RestoreInterlinks.
        // The game attaches a Door's pane only on its FIRST update, so without
        // this a loaded M04S01 swung its window frame and left the glass.
        // LT_NORESTOREATTACH=1 = the old outcome (none come back; says so).
        unsigned int nAttBack = 0, nLinkBack = 0;
        if (getenv("LT_NORESTOREATTACH"))
        { printf("[SERVER] LT_NORESTOREATTACH=1 -- attachments and links are NOT restored (old)\n"); fflush(stdout); }
        else
        {
            for (size_t k = 0; k < savedAtt.size(); ++k)
            {
                const SavedAtt& t = savedAtt[k];
                if (t.iP < 0 || t.iC < 0 || (size_t)t.iP >= handles.size() || (size_t)t.iC >= handles.size()) continue;
                HOBJECT hP = (HOBJECT)handles[t.iP], hC = (HOBJECT)handles[t.iC];
                if (!hP || !hC || LTHost_FindAttachment(hP, hC, 0) == LT_OK) continue;
                LTVector v; v.x = t.ofs[0]; v.y = t.ofs[1]; v.z = t.ofs[2];
                LTRotation q; for (int c = 0; c < 4; ++c) q.m_Quat[c] = t.rot[c];
                if (LTHost_CreateAttachment(hP, hC, t.sSock.empty() ? 0 : (char*)t.sSock.c_str(), &v, &q, 0) == LT_OK)
                    ++nAttBack;
            }
            for (size_t k = 0; k < savedLinks.size(); ++k)
            {
                const int o = savedLinks[k].first, l = savedLinks[k].second;
                if (o < 0 || l < 0 || (size_t)o >= handles.size() || (size_t)l >= handles.size()) continue;
                if (handles[o] && handles[l]
                    && g_Objects.CreateLink((SRec*)handles[o], (SRec*)handles[l])) ++nLinkBack;
            }
            if (nVersion >= 8)
            { printf("[SERVER] RestoreObjects: %u/%u attachment(s), %u/%u link(s) restored\n",
                     nAttBack, (unsigned)savedAtt.size(), nLinkBack, (unsigned)savedLinks.size()); fflush(stdout); }
        }

        // ⚠️ AND THE RESTORED BRUSHES GET THEIR GEOMETRY BACK -- what the
        // world-objects load does after CreateWorldObjects, which a loaded
        // game never runs (5bz). Without it every door stayed in the static
        // world at its file pose: M01S01's balcony shutter stood closed in
        // front of a player saved looking through it. The box and the REST
        // pose come from the world file (Record::m_bRestFromFile); the pose
        // is the saved one. After phase 3: Build reads the flags Load set.
        if (bLinkWorld)
        {
            const CLTWorld& world = LTGetWorldScene()->GetWorld();
            const std::vector<LTWorldModelHeader>& wm = world.GetModels();
            const std::vector<LTWorldObject>& obj = world.GetObjects();
            unsigned int nBrush = 0, nBound = 0;
            for (size_t i = 0; i < handles.size(); ++i)
            {
                SRec* r = (SRec*)handles[i];
                if (!r || (r->m_Type != OT_WORLDMODEL && r->m_Type != OT_CONTAINER)) continue;
                if (r->m_nWorldIndex < 0 || (size_t)r->m_nWorldIndex >= obj.size()) continue;
                ++nBrush;
                const char* pWM = !r->m_sFilename.empty() ? r->m_sFilename.c_str() : r->m_sName.c_str();
                for (size_t k = 0; k < wm.size(); ++k)
                {
                    if (strcasecmp(wm[k].m_sName.c_str(), pWM) != 0
                        && strcasecmp(wm[k].m_sName.c_str(), r->m_sName.c_str()) != 0) continue;
                    for (int a = 0; a < 3; ++a)
                    {
                        r->m_BoxMin[a]     = wm[k].m_Min[a];
                        r->m_BoxMax[a]     = wm[k].m_Max[a];
                        r->m_BoxRestPos[a] = wm[k].m_Translation[a];
                    }
                    float eul[3] = { 0, 0, 0 };
                    r->m_RestRot[0] = r->m_RestRot[1] = r->m_RestRot[2] = 0.0f;
                    r->m_RestRot[3] = 1.0f;
                    if (obj[r->m_nWorldIndex].GetEuler(eul))
                        LTServerObjects_EulerToQuat(eul, r->m_RestRot);
                    r->m_bHasBox = true;
                    r->m_bRestFromFile = true;
                    ++nBound;
                    break;
                }
                if (!r->m_bRestFromFile && getenv("LT_WMTRACE"))
                    printf("[WM] restored %s '%s' (model '%s'): no world model of that name\n",
                           r->m_pClass ? r->m_pClass->m_ClassName : "?", r->m_sName.c_str(), pWM);
            }
            LTServerBrush_Build(world);
            LTServerPhys_Reset();
            LTServerPhys_BuildFor(&world);
            LTServerBrush_Update();
            printf("[SERVER] RestoreObjects: linked %u object(s) back to %s; %u of %u "
                   "brush object(s) bound to their geometry (rest pose from the file); %s\n",
                   nLinked, sSavedWorld.c_str(), nBound, nBrush, LTHost_WorldStateCensus().c_str());
            fflush(stdout);
        }
        else if (!sSavedWorld.empty() && !s_bNoLink && (flags & RESTOREOBJECTS_RESTORETIME))
        {
            printf("[SERVER] RestoreObjects: not linked to the world file (saved in %s, "
                   "now in %s%s)\n", sSavedWorld.c_str(), LTWorldScene_Name(),
                   g_Objects.HasWorldObjects() ? ", whose objects exist" : "");
            fflush(stdout);
        }

        // ---- version 7: the portals (HANDOVER 5cp) ---- //
        //
        // Only on a load of this same level -- a keep-alive restore carries
        // the LAST level's.
        if (bLinkWorld && bHavePortals && savedPortals.size() == LTGetWorldVis()->NumPortals())
            for (size_t k = 0; k < savedPortals.size(); ++k)
                LTGetWorldVis()->SetPortalFlagsAt((unsigned int)k, savedPortals[k]);
        if (nAsleep)
        {
            printf("[SERVER] RestoreObjects: %u object(s) restored asleep\n", nAsleep);
            fflush(stdout);
        }
        if (nAnimRestored)
        {
            printf("[SERVER] RestoreObjects: %u model(s) given their saved animation back\n", nAnimRestored);
            fflush(stdout);
        }

        printf("[SERVER] restored %u object(s) from %s (%u created, %u failed, "
               "%u reference(s) resolved, %u effect(s) re-sent", nRestored, sPath.c_str(),
               nCreated, nFailed, nRefsRead - nRefsBad, nSFXResent);
        if (nRefsBad) printf(", ⚠️  %u UNRESOLVED", nRefsBad);
        printf(")\n");
        fflush(stdout);
        LTRecord_Event("server", "RestoreObjects %s %u", sPath.c_str(), nRestored);

        return LT_OK;
    }

    // ⚠️ THE OBJECT-QUERY FAMILY, and it is what the whole level was waiting
    // on. `FindStartPoint` (GameServerShell.cpp:1517) asks
    // FindNamedObjects("GameStartPoint"); with a stub it got nothing, returned
    // null, and the player was never given a spawn position, a model style or
    // a physics model. Everything downstream of that -- where you stand, what
    // you look like, whether the level's opening trigger fires -- was waiting
    // on this one call.
    //
    // ⚠️ MATCHING IS BY NAME AND CASE-INSENSITIVE, not by class. The name is
    // whatever is in ObjectCreateStruct::m_Name AFTER MID_PRECREATE, which is
    // why CreateWorldObjects re-reads the struct: the game renames its start
    // points from "GameStartPoint0" to "GameStartPoint" in precreate and then
    // looks them up under the new name.
    LTRESULT LTHost_FindNamedObjects(char* pName, BaseObjArray<HOBJECT>& objArray,
                                     uint32* pnTotalFound)
    {
        // ⚠️ Reset FIRST and on every path. The game reuses one stack array
        // across several searches, so a query that matched nothing must leave
        // it empty rather than leave the previous query's answers in it.
        objArray.Reset();
        if (pnTotalFound) *pnTotalFound = 0;
        if (!pName) return LT_ERROR;

        uint32 nFound = 0;
        for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
        {
            SRec* p = g_Objects.RecordAt(i);
            if (!p || p->m_sName.empty()) continue;

            // ⚠️ A REMOVED OBJECT IS NOT FINDABLE, from the moment it is
            // marked. It is still in the list until the sweep, and a keyframer
            // that removes its keys as it copies them would otherwise find the
            // same key again on its next pass.
            if (!g_Objects.IsLive(p)) continue;

            if (strcasecmp(p->m_sName.c_str(), pName) != 0) continue;

            ++nFound;

            // ⚠️ THE COUNT AND THE ARRAY DIVERGE ON PURPOSE. AddObject refuses
            // once the caller's fixed-size array is full, but nTotalFound is
            // documented as the TOTAL -- the game uses the difference to know
            // its array was too small. Stopping the count at the array's
            // capacity would hide that.
            objArray.AddObject((HOBJECT)p);
        }

        // ⚠️ LT_NAMETRACE=1 prints every lookup and what it found. Object
        // lookup by NAME is how the game wires itself together -- a keyframer
        // finds its keys and the object it moves this way, a trigger finds its
        // target -- so "found 0" is the single most common reason a piece of
        // level script does nothing at all, and it is invisible from outside.
        if (getenv("LT_NAMETRACE"))
            printf("[NAME] \"%s\" -> %u\n", pName, (unsigned int)nFound);

        if (pnTotalFound) *pnTotalFound = nFound;
        return LT_OK;
    }

    // ⚠️ TWO OVERLOADS, AND BOTH ARE REACHED. The obsolete one returns a
    // pointer into the engine's own storage; the game copies it immediately,
    // but a std::string's c_str() stays valid as long as the record does, so
    // this is a borrow rather than a dangling return.
    //
    // ⚠️ A FREED HANDLE ANSWERS ITS OLD NAME, OR "" (HANDOVER 5ck). The AI's
    // reactions name "the enemy" from a sense's stimulus or its last damager
    // (AIHumanReactions.cpp GetEnemyTargetName, then sprintf %s), and near the
    // end of M01S01 that object had already been removed: m_sName.c_str() of
    // a freed record handed sprintf garbage and the server died in strlen.
    // On Windows the name sat in pooled object memory that outlived the
    // object; the remembered name is the nearest thing to that. Same policy
    // as GetObjectClass below. LT_NONAMEGUARD=1 = read any record (old, says so).
    static const char* LTHost_LiveName(SRec* p)
    {
        static const bool s_bNoGuard = []() {
            const bool b = (getenv("LT_NONAMEGUARD") != 0);
            if (b) { printf("[SERVER] LT_NONAMEGUARD=1 -- GetObjectName reads any handle, freed or not (old)\n"); fflush(stdout); }
            return b; }();
        if (s_bNoGuard || p == LTHost_WorldRecord() || g_Objects.Contains(p)) return p->m_sName.c_str();

        static std::set<std::string> s_Names;      // stable storage for the old names
        const std::string sWas = g_Objects.FreedName(p);
        static std::set<const void*> s_Told;
        if (s_Told.insert(p).second)
        {
            printf("[SERVER] t=%.2f GetObjectName(%p): not an object any more (was: %s) -- answering \"%s\"\n",
                   LTServerHost_Time(), (void*)p, sWas.empty() ? "?" : sWas.c_str(), sWas.c_str());
            fflush(stdout);
        }
        return s_Names.insert(sWas).first->c_str();
    }

    char* LTHost_GetObjectNameOld(HOBJECT h)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        return p ? (char*)LTHost_LiveName(p) : (char*)"";
    }

    LTRESULT LTHost_GetObjectName(HOBJECT h, char* pName, uint32 nBufSize)
    {
        if (!pName || !nBufSize) return LT_ERROR;
        pName[0] = '\0';

        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) return LT_ERROR;

        strncpy(pName, LTHost_LiveName(p), nBufSize - 1);
        pName[nBufSize - 1] = '\0';
        return LT_OK;
    }

    // ⚠️ A FREED HANDLE ANSWERS NULL, NAMED ONCE (HANDOVER 5cf). Reading
    // m_pClass out of a freed record handed IsKindOf garbage and crashed the
    // server (M01S01, the AI sense that asks IsCharacter(info.hObject) of a
    // shot's target). The donor would have read freed memory too; the game
    // relies on links to clear such handles, and this is the backstop for the
    // ones it misses -- same policy as the message guard (5bi).
    // LT_NOCLASSGUARD=1 = read the record whatever it is (old, says so).
    HCLASS LTHost_GetObjectClass(HOBJECT h)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) return (HCLASS)0;
        static const bool s_bNoGuard = []() {
            const bool b = (getenv("LT_NOCLASSGUARD") != 0);
            if (b) { printf("[SERVER] LT_NOCLASSGUARD=1 -- GetObjectClass reads any handle, freed or not (old)\n"); fflush(stdout); }
            return b; }();
        if (!s_bNoGuard && p != LTHost_WorldRecord() && !g_Objects.Contains(p))
        {
            static std::set<const void*> s_Told;
            if (s_Told.insert(p).second)
            {
                const std::string sWas = g_Objects.FreedName(p);
                printf("[SERVER] t=%.2f GetObjectClass(%p): not an object any more (was: %s) -- NULL\n",
                       LTServerHost_Time(), (void*)p, sWas.empty() ? "?" : sWas.c_str());
                fflush(stdout);
            }
            return (HCLASS)0;
        }
        return (HCLASS)p->m_pClass;
    }

    // ⚠️ WALKS THE PARENT CHAIN, because that is what "kind of" means: a Door
    // IS a GameBase. The header is explicit that the arguments are ordered
    // (IsKindOf(hClass, hTest) asks whether hClass derives from hTest) and that
    // getting them backwards returns FALSE rather than failing -- so a reversed
    // call is a silent wrong answer, not a crash.
    //
    // ⚠️ The chain is bounded. m_ParentClass comes from a module this engine
    // did not build; a cycle in it would hang the server inside a query.
    LTBOOL LTHost_IsKindOf(HCLASS hClass, HCLASS hTest)
    {
        const ClassDef* p = (const ClassDef*)hClass;
        const ClassDef* q = (const ClassDef*)hTest;
        if (!p || !q) return LTFALSE;

        for (int nGuard = 0; p && nGuard < 32; p = p->m_ParentClass, ++nGuard)
            if (p == q) return LTTRUE;

        return LTFALSE;
    }

    // ⚠️ NULL MEANS "START", not "no object" -- iltserver.h:599 says so in as
    // many words. Reading it as an error would make every walk of the object
    // list return nothing on its first step.
    HOBJECT LTHost_GetNextObject(HOBJECT h)
    {
        const unsigned int n = g_Objects.ObjectCount();
        if (!n) return (HOBJECT)0;

        // ⚠️ LT_WALKTRACE=1 -- HOW FAR A WALK OF THE OBJECT LIST GETS. The
        // game uses this to reach every object of a kind at once:
        // CPlayerObj::PlayingCinematic (PlayerObj.cpp:3243) walks the whole
        // list to send "SKIP" to every CinematicTrigger. A walk that stops
        // early skips the ones it never reached, and from outside that looks
        // like "the cinematic chain stalls".
        static const bool s_bWalk = (getenv("LT_WALKTRACE") != 0);
        static unsigned s_nSteps = 0;
        if (s_bWalk)
        {
            if (!h)
            {
                if (s_nSteps)
                    printf("[WALK] a walk ended after %u of %u object(s)%s\n",
                           s_nSteps, n,
                           (s_nSteps < n) ? "   ⚠️  SHORT" : "");
                s_nSteps = 0;
            }
            ++s_nSteps;
        }

        if (!h) return (HOBJECT)g_Objects.RecordAt(0);

        // ⚠️ Linear, and that is a known cost: a full walk is O(n^2) in a
        // level with 731 objects. The game itself calls this "generally a bad
        // idea" (iltserver.h:601) and uses it rarely; an index on the record
        // would be the fix if it ever shows up in a profile.
        for (unsigned int i = 0; i + 1 < n; ++i)
            if (g_Objects.RecordAt(i) == CLTServerObjects::FromHandle(h))
                return (HOBJECT)g_Objects.RecordAt(i + 1);

        // ⚠️ REACHING HERE WITH A HANDLE THE LIST DOES NOT HOLD IS NOT "the
        // end" -- it is a walk that lost its place, and it ends the walk
        // silently wherever it happened to be.
        if (s_bWalk)
        {
            bool bFound = false;
            for (unsigned int i = 0; i < n; ++i)
                if (g_Objects.RecordAt(i) == CLTServerObjects::FromHandle(h)) bFound = true;
            printf("[WALK] ended at step %u of %u -- the handle it was given is %s\n",
                   s_nSteps, n, bFound ? "the LAST object" : "⚠️  NOT IN THE LIST ANY MORE");
            s_nSteps = 0;
        }
        return (HOBJECT)0;
    }

    // ⚠️ ALWAYS NULL, AND THAT IS THE TRUTH RATHER THAN A STUB. This port has
    // no inactive-object list: nothing ever deactivates, so the set really is
    // empty and "no more objects" is the correct answer. If deactivation is
    // ever implemented this has to grow a real list -- but answering null now
    // is not a lie, and the game's loops terminate on it correctly.
    HOBJECT LTHost_GetNextInactiveObject(HOBJECT) { return (HOBJECT)0; }

    // ⚠️ FLAGS AND DIMS -- record fields, and every one of them was a stub
    // that returned 0 for something the game had just set. An object whose
    // flags read back as zero is invisible, has no collision and is not
    // solid, all at once.
    uint32 LTHost_GetObjectFlags(HOBJECT h)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        return p ? p->m_Flags : 0;
    }

    void LTHost_SetObjectFlags(HOBJECT h, uint32 flags)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (p) p->m_Flags = flags;
    }

    // ⚠️ `void` RETURN, so the caller cannot tell it failed -- which makes
    // writing the output on every path mandatory, exactly as for GetObjectPos.
    void LTHost_GetObjectDims(HOBJECT h, LTVector* pDims)
    {
        if (!pDims) return;
        SRec* p = CLTServerObjects::FromHandle(h);
        if (p && p->m_bHasBox && p->m_Dims[0] <= 0.0f && p->m_Dims[1] <= 0.0f && p->m_Dims[2] <= 0.0f)
        {
            // ⚠️ A BRUSH OBJECT'S DIMS ARE HALF ITS GEOMETRY BOX, as the engine
            // sets them (se_InitWorldModel). Not stored in m_Dims, which the
            // mover uses as a collision size (a door must not collide with its
            // own brush) -- only what the GAME is told.
            pDims->Init((p->m_BoxMax[0]-p->m_BoxMin[0])*0.5f, (p->m_BoxMax[1]-p->m_BoxMin[1])*0.5f,
                        (p->m_BoxMax[2]-p->m_BoxMin[2])*0.5f);
        }
        else if (p) pDims->Init(p->m_Dims[0], p->m_Dims[1], p->m_Dims[2]);
        else   pDims->Init(0.0f, 0.0f, 0.0f);
    }

    // ⚠️ ON FAILURE THIS HANDS BACK THE DIMS THE OBJECT ALREADY HAS, and that
    // is the whole point rather than tidiness.
    //
    // CCharacter::UpdateDims (Character.cpp:814) does
    //      g_pLTServer->GetModelAnimUserDims(m_hObject, &vDims, anim);
    //      SetDims(&vDims);
    // with NO look at the return. So whatever comes back is the character's
    // collision box, whether we could answer or not.
    //
    // Writing zeros there is how the player ended up falling out of the UNITY
    // lobby: the server's Player has no model filename of its own, the lookup
    // missed, dims became (0,0,0), and a zero-sized box is inside every floor
    // in the level. It also feeds the game's MoveObjectToFloor, whose
    //      vPos.y -= (fDist - (vDims.y + 0.1f))
    // put the player at y = 0.1 exactly -- the measured teleport target.
    //
    // Returning the CURRENT dims makes SetDims's own epsilon test (2429) see
    // no change and return early, so an object we cannot resolve simply keeps
    // the size it had. The doorknob fix stands: a model we CAN resolve still
    // gets its real anim dims, which is what stopped a 6e28-unit box from
    // swallowing every Activate ray in T01S03.
    LTRESULT LTHost_AnimUserDims(HOBJECT h, LTVector* pDims, HMODELANIM hAnim)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) { pDims->Init(0.0f, 0.0f, 0.0f); return LT_ERROR; }

        float d[3] = { 0.0f, 0.0f, 0.0f };
        if (LTScene_GetAnimDimsForFile(p->m_sFilename.c_str(), (int)hAnim, d) &&
            (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f))
        {
            pDims->Init(d[0], d[1], d[2]);
            return LT_OK;
        }

        // Unresolvable, or a model whose animation genuinely has no dims:
        // leave the object as it is.
        pDims->Init(p->m_Dims[0], p->m_Dims[1], p->m_Dims[2]);

        if (getenv("LT_DIMSTRACE"))
            printf("[DIMS] no anim dims for %-18s '%s' (model '%s') -- keeping "
                   "(%.1f %.1f %.1f)\n",
                   p->m_pClass ? p->m_pClass->m_ClassName : "?",
                   p->m_sName.c_str(), p->m_sFilename.c_str(),
                   p->m_Dims[0], p->m_Dims[1], p->m_Dims[2]);
        return LT_ERROR;
    }

    LTRESULT LTHost_SetObjectDims(HOBJECT h, LTVector* pDims)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p || !pDims) return LT_ERROR;
        if (LTHost_PropTraced(p))
            printf("[PROP] %-12s dims -> (%.1f %.1f %.1f)  model %s\n", p->m_sName.c_str(),
                   pDims->x, pDims->y, pDims->z, p->m_sFilename.c_str());
        p->m_Dims[0] = pDims->x; p->m_Dims[1] = pDims->y; p->m_Dims[2] = pDims->z;
        return LT_OK;
    }

    // ⚠️ THE ONE CLIENT, NAMED ONCE. The engine normally mints an HCLIENT when
    // a connection arrives; there is no network here and single player still
    // needs exactly one. What the game does with the handle is hand it back to
    // the engine, so a distinct non-null value is all it can legitimately
    // require -- but BOTH directions have to agree on which value, or a message
    // the client sends arrives addressed to a client the server does not have.
    HCLIENT LTHost_TheClient() { return (HCLIENT)(uintptr_t)1; }

    // ⚠️ CLIENT REFS -- AND THEY ARE WHAT MAKES A LEVEL CHANGE KEEP ITS PLAYER.
    //
    // `CGameServerShell::OnClientEnterWorld` does not just make a player. It
    // first walks the engine's CLIENT REFS looking for one that is this client,
    // and if it finds one it takes THAT ref's object and calls RespawnPlayer on
    // it instead (GameServerShell.cpp:317-357). Only when the walk finds
    // nothing does it fall through to `CreatePlayer`.
    //
    // ⚠️ SO A STUBBED GetNextClientRef IS NOT "a missing query", IT IS A
    // DUPLICATE PLAYER. On a level change the keep-alive CPlayerObj is restored
    // into the new world, then OnClientEnterWorld finds no ref, builds a second
    // player, and the game says so itself:
    //     [GAME/S] Error, 2 objects named "player" present in level
    // One of the two is the one carrying everything the player earned in the
    // last level -- weapons, ammo, health, objectives -- and it is not the one
    // the camera follows.
    //
    // ⚠️ THE REF IS DERIVED, NOT STORED, and that is deliberate. A stored
    // handle would have to be invalidated by ClearObjects, re-pointed by
    // RestoreObjects and re-pointed again by CreatePlayer -- three places that
    // must all be right, across a teardown whose whole purpose is to destroy
    // the thing being pointed at. Finding the live player by class at the
    // moment of the question cannot go stale.
    //
    // There is exactly one local client, so there is at most one ref, and its
    // handle is a constant the ref queries below recognise.
    const uintptr_t kTheClientRef = 1;

    // The live CPlayerObj in the world, or null before one exists.
    SRec* LTHost_FindPlayerRecord()
    {
        for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
        {
            SRec* p = g_Objects.RecordAt(i);
            if (!p || !g_Objects.IsLive(p) || !p->m_pGame) continue;
            if (!p->m_pClass || !p->m_pClass->m_ClassName) continue;
            if (strcasecmp(p->m_pClass->m_ClassName, "CPlayerObj") == 0)
                return p;
        }
        return 0;
    }

    HCLIENTREF LTHost_GetNextClientRef(HCLIENTREF hRef)
    {
        // ⚠️ NULL MEANS "START THE WALK" and anything else means "continue it".
        // With one client the walk is one step long, so a non-null argument is
        // always the end -- returning the same ref again would spin the game's
        // while loop forever.
        if (hRef) return (HCLIENTREF)0;

        // ⚠️ NO REF UNTIL THERE IS A PLAYER TO REFER TO. On the FIRST level
        // there is no player yet, the walk correctly finds nothing, and
        // OnClientEnterWorld creates one -- which is the right behaviour and
        // the reason this cannot simply always answer yes.
        if (!LTHost_FindPlayerRecord()) return (HCLIENTREF)0;

        return (HCLIENTREF)kTheClientRef;
    }

    uint32 LTHost_GetClientRefInfoFlags(HCLIENTREF hRef)
    {
        if ((uintptr_t)hRef != kTheClientRef) return 0;

        // ⚠️ CIF_LOCAL is the flag the game tests FIRST, and finding it set is
        // what makes it stop walking and adopt this ref's object. It is also
        // documented as not settable through SetClientInfoFlags -- it describes
        // the connection, and this connection is a function call.
        return CIF_LOCAL;
    }

    LTBOOL LTHost_GetClientRefName(HCLIENTREF hRef, char* pName, int maxLen)
    {
        if ((uintptr_t)hRef != kTheClientRef || !pName || maxLen <= 0)
            return LTFALSE;

        strncpy(pName, "Player", (size_t)maxLen - 1);
        pName[maxLen - 1] = '\0';
        return LTTRUE;
    }

    HOBJECT LTHost_GetClientRefObject(HCLIENTREF hRef)
    {
        if ((uintptr_t)hRef != kTheClientRef) return (HOBJECT)0;
        return (HOBJECT)LTHost_FindPlayerRecord();
    }

    // ⚠️ WHERE THE CLIENT IS LOOKING FROM, AS THE SERVER UNDERSTANDS IT. Not
    // the client's camera -- the server's idea of it, which is a different
    // thing and the source of the confusion this function invites.
    //
    // iltserver.h is explicit about the job: "Set where the client is
    // seeing/hearing from. This controls what the server SENDS the client. You
    // should set this every frame." It is the input to visibility culling, and
    // in the original that decides which objects reach the wire at all.
    //
    // ⚠️ SO IT DOES NOT MOVE THE VIEW, and implementing it will not make a
    // cinematic play. NOLF's own two callers say so: CPlayerObj hands over the
    // PLAYER's position every frame, and Camera::Update hands over the
    // CAMERA's -- two different answers to "what does the client see" for one
    // camera that the CLIENT owns and moves (GameClientShell.cpp:2455).
    float g_ClientViewPos[3] = { 0.0f, 0.0f, 0.0f };
    bool  g_bClientViewPosSet = false;

    LTRESULT LTHost_SetClientViewPos(HCLIENT hClient, LTVector* pPos)
    {
        // ⚠️ The handle is CHECKED rather than ignored. There is one client and
        // it would be easy to write this as "store it", but a call naming a
        // client this server does not have is the game believing something
        // untrue, and answering LT_OK to it hides that.
        if (!pPos || hClient != LTHost_TheClient()) return LT_ERROR;

        g_ClientViewPos[0] = pPos->x;
        g_ClientViewPos[1] = pPos->y;
        g_ClientViewPos[2] = pPos->z;
        g_bClientViewPosSet = true;
        return LT_OK;
    }

    // ⚠️ THE SERVER'S END OF THE PIPE. Everything the game sends INTO the
    // server -- a client's input, an object telling another object it was
    // triggered, an object telling the shell something happened -- arrives
    // here, once per frame, from LTMessage_PumpServer.
    //
    // ⚠️ Returns false when there was nothing to deliver TO, which is a
    // different fact from "no receiver is registered" and is counted
    // separately. A trigger firing at a door that was already removed is
    // ordinary; a thousand of them is a bug, and only the census can tell the
    // two apart.
    bool LTHost_DeliverToServer(CLTMessage* pMsg);

    // ⚠️ Forward-declared for the same reason: they need g_pShell, which the
    // class below creates, and the class needs THEM to wire its function
    // pointers. Something has to be a declaration; this is the half that costs
    // two lines instead of moving the shell pointer above its own owner.
    LTRESULT LTHost_LoadWorld(char* pszWorldFileName, uint32 flags);
    LTRESULT LTHost_RunWorld();

    // ⚠️ THE SERVER CLOCK. Fixed-step and owned here, NOT wall time: the whole
    // port is built on runs being reproducible (HANDOVER.md -- the second clock
    // that was still on GetTickCount cost three distinct menu frames from six
    // identical runs). A server whose objects update on wall time would put
    // that back.
    float g_fServerTime = 0.0f;

    // The elapsed time of the frame now being run, as the host measured it.
    // Defaults to a sane step so an object that asks before the first update
    // gets a frame rather than zero.
    float g_fServerFrameTime = 1.0f / 60.0f;

    void LTHost_SetNextUpdate(HOBJECT h, LTFLOAT fNext)
    {
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) return;

        // ⚠️ The game passes a DELAY, and 0 means "stop updating me". Storing
        // the delay as if it were an absolute time would make every object
        // that asked for 0.1s update once and never again.
        // ⚠️ Under LT_SCHEDTRACE=<name-substring>, every scheduling decision
        // for a named object. This separates "the object was never scheduled"
        // (its activator returned early) from "it was scheduled and the update
        // never fired" -- two different bugs, identical from outside.
        {
            static const char* pWant = getenv("LT_SCHEDTRACE");
            if (pWant && *pWant && !p->m_sName.empty() &&
                p->m_sName.find(pWant) != std::string::npos)
            {
                printf("[SCHED] %-18s next=%.4f  t=%.3f%s\n",
                       p->m_sName.c_str(), (float)fNext, g_fServerTime,
                       (fNext > 0.0f) ? "" : "   (STOP)");
                fflush(stdout);
            }
        }

        p->m_fNextUpdate = (fNext > 0.0f) ? (g_fServerTime + (float)fNext) : 0.0f;
    }

    // ----------------------------------------------------------------- //
    // ⚠️ ACTIVATION -- LT2's auto-deactivation (HANDOVER 5cp). The state is
    // on the record (Record::m_nInactive), the rules in ltserverobjects.cpp.
    // All five were stubs, so nothing in a level ever fell asleep and every
    // AI heard every shot in it.
    // ----------------------------------------------------------------- //

    // The one client's flags -- NOLF sets CIF_AUTOACTIVATEOBJECTS on it
    // (GameServerShell.cpp:412), and without it its view wakes nothing.
    uint32 g_ClientInfoFlags = 0;

    void LTHost_SetClientInfoFlags(HCLIENT hClient, uint32 dwClientFlags)
    {
        if (hClient != LTHost_TheClient()) return;
        g_ClientInfoFlags = dwClientFlags;
    }

    uint32 LTHost_GetClientInfoFlags(HCLIENT hClient)
    {
        return (hClient == LTHost_TheClient()) ? g_ClientInfoFlags : 0;
    }

    // "Sets the time which the engine will automatically deactivate an object.
    // If object is currently autodeactivated, then this call will activate the
    // object." (iltserver.h) -- lithtech.exe 0x472180 restarts the countdown
    // with it, too.
    void LTHost_SetDeactivationTime(HOBJECT hObj, LTFLOAT fDeactivationTime)
    {
        SRec* p = CLTServerObjects::FromHandle(hObj);
        if (!p || !g_Objects.IsLive(p)) return;
        p->m_fDeactTime = p->m_fDeactTimer = (float)fDeactivationTime;
        if (p->m_nInactive & CLTServerObjects::kAutoDeactivated)
            g_Objects.SetActivity(p, (unsigned char)(p->m_nInactive &
                                  (CLTServerObjects::kInactive | CLTServerObjects::kInactiveTouch)),
                                  "SetDeactivationTime");
    }

    // ⚠️ AUTODEACTIVATE_NOW only while auto-deactivation is on, as
    // lithtech.exe 0x472298 checks -- and it REPLACES the state (0x20), it
    // does not add to it.
    void LTHost_SetObjectState(HOBJECT hObj, int state)
    {
        SRec* p = CLTServerObjects::FromHandle(hObj);
        if (!p || !g_Objects.IsLive(p)) return;
        switch (state)
        {
        case OBJSTATE_ACTIVE:
            g_Objects.SetActivity(p, 0, "SetObjectState");
            break;
        case OBJSTATE_INACTIVE:
            g_Objects.SetActivity(p, CLTServerObjects::kInactive, "SetObjectState");
            break;
        case OBJSTATE_INACTIVE_TOUCH:
            g_Objects.SetActivity(p, CLTServerObjects::kInactiveTouch, "SetObjectState");
            break;
        case OBJSTATE_AUTODEACTIVATE_NOW:
            if (CLTServerObjects::AutoDeactivateOn())
                g_Objects.SetActivity(p, CLTServerObjects::kAutoDeactivated, "AUTODEACTIVATE_NOW");
            break;
        default:
            break;
        }
    }

    int LTHost_GetObjectState(HOBJECT hObj)
    {
        SRec* p = CLTServerObjects::FromHandle(hObj);
        if (!p || !g_Objects.IsLive(p)) return OBJSTATE_ACTIVE;
        if (p->m_nInactive & CLTServerObjects::kInactive)        return OBJSTATE_INACTIVE;
        if (p->m_nInactive & CLTServerObjects::kInactiveTouch)   return OBJSTATE_INACTIVE_TOUCH;
        if (p->m_nInactive & CLTServerObjects::kAutoDeactivated) return OBJSTATE_AUTODEACTIVATE_NOW;
        return OBJSTATE_ACTIVE;
    }

    void LTHost_PingObjects(HOBJECT hObj)
    {
        SRec* p = CLTServerObjects::FromHandle(hObj);
        if (!p || !g_Objects.IsLive(p)) return;
        g_Objects.PingFrom(p, g_fServerFrameTime);
    }

    // ⚠️ PORTALS ARE PART OF THE VISIBILITY. A door opening its portal
    // (Door.cpp:1352) lets the view see through it -- ltworldvis walks that
    // portal's lists while it is open. The game ORs PORTAL_OPEN into what Get
    // returned, so Get always writes.
    LTRESULT LTHost_GetPortalFlags(char* pPortalName, uint32* pFlags)
    {
        unsigned int n = 0;
        const bool bFound = LTGetWorldVis()->GetPortalFlags(pPortalName, &n);
        if (pFlags) *pFlags = n;
        return bFound ? LT_OK : LT_NOTFOUND;
    }

    LTRESULT LTHost_SetPortalFlags(char* pPortalName, uint32 flags)
    {
        const bool bFound = LTGetWorldVis()->SetPortalFlags(pPortalName, flags);
        static const bool bTrace = (getenv("LT_ACTTRACE") != 0);
        if (bTrace)
        {
            printf("[ACT] portal '%s' -> 0x%x%s\n", pPortalName ? pPortalName : "",
                   (unsigned)flags, bFound ? "" : " (no such portal)");
            fflush(stdout);
        }
        return bFound ? LT_OK : LT_NOTFOUND;
    }

    // ⚠️ The SERVER's ILTServer, wired the same way the client's is. The
    // generated LTSkel_WireData assigns every sub-interface pointer member;
    // leaving one null is a crash with no [LTSTUB] line in front of it,
    // because a data member reached through an inline accessor never goes
    // through a vtable (HANDOVER.md §4.5).
    class LTSkeletonServer : public LTStub_ILTServer
    {
    public:
        // Containers: see LTHost_GetObjectContainers.
        uint32 GetPointContainers(LTVector* pPoint, HOBJECT* pList, uint32 maxListSize) override
        {
            LTIMPL("ILTCSBase::GetPointContainers");
            return LTHost_ServerPointContainers(pPoint, pList, maxListSize);
        }
        LTBOOL GetContainerCode(HOBJECT hObj, uint16* pCode) override
        {
            LTIMPL("ILTCSBase::GetContainerCode");
            return LTHost_ServerContainerCode(hObj, pCode);
        }
        // ⚠️ ALWAYS WRITES *pFlags. The stub returned LT_ERROR and wrote
        // nothing, and the game reads its uninitialised local as the SURFACE
        // TYPE (SurfaceFunctions.cpp:380) -- every bullet hit a random surface,
        // which is why an opened shutter sometimes let a shot through.
        // ⚠️ THE WORLD'S EXTENTS. CPlayerObj::Update asks every frame whether
        // the player is outside them and, if so, kills her with infinite
        // DT_EXPLODE damage (PlayerObj.cpp:1135). The stub wrote nothing, so
        // the test read uninitialised vectors: 4,698 self-inflicted infinite
        // explosions in one M01S01 run. Always writes both outputs.
        LTRESULT GetWorldBox(LTVector& vMin, LTVector& vMax) override
        {
            LTIMPL("ILTServer::GetWorldBox");
            vMin.Init(-1e6f, -1e6f, -1e6f);
            vMax.Init( 1e6f,  1e6f,  1e6f);
            CLTWorldScene* pScene = LTGetWorldScene();
            if (!pScene || !pScene->GetWorld().HasGeometryInfo()) return LT_ERROR;
            const LTWorldInfo& wi = pScene->GetWorld().GetWorldInfo();
            vMin.Init(wi.m_ExtentsMin[0], wi.m_ExtentsMin[1], wi.m_ExtentsMin[2]);
            vMax.Init(wi.m_ExtentsMax[0], wi.m_ExtentsMax[1], wi.m_ExtentsMax[2]);
            return LT_OK;
        }

        LTRESULT GetPolyTextureFlags(HPOLY hPoly, uint32* pFlags) override
        {
            LTIMPL("ILTServer::GetPolyTextureFlags");
            return LTHost_PolyTextureFlags((unsigned int)hPoly, pFlags);
        }

    public:
        // ⚠️ OpenFile IS A VIRTUAL on ILTCSBase, not one of the 129 function
        // pointers, so it is overridden rather than assigned -- the same
        // distinction that matters on the client (HANDOVER.md §4.5).
        //
        // ⚠️ AND IT IS WHY THE GAME'S MANAGERS WERE ALL NULL. `CGlobalMgr::Init`
        // runs in the server shell's constructor and every one of its bute
        // managers reads an attribute file through this call. With a stub here
        // they all failed, `g_pModelButeMgr` stayed LTNULL, and the first
        // object to ask for a player model dereferenced it --
        // `CModelButeMgr::GetModelId(this=0x0, "Player")`. A file system is
        // not an optimisation for the server; it is the floor.
        // ⚠️ Virtuals, like OpenFile. An object that asks the time and gets 0
        // every frame computes a zero delta and never advances -- the client
        // side lost a whole menu to exactly that (HANDOVER.md §2).
        LTFLOAT GetTime() override      { return g_fServerTime; }
        // ⚠️ THE REAL ELAPSED FRAME, NOT A CONSTANT -- and the constant is
        // why the cinematics played in slow motion.
        //
        // Every server-driven thing in NOLF moves by this number: keyframers
        // (which is what a cinematic camera IS), doors, AI, projectiles. The
        // host already advances g_fServerTime by the true elapsed time, so
        // GetTime was right while GetFrameTime claimed 1/60 whatever the
        // frame actually cost. At the software renderer's ~8 fps that is a
        // world running at an eighth speed, with an absolute clock that
        // disagrees with it -- a cinematic crawling in one direction is
        // exactly what that looks like.
        LTFLOAT GetFrameTime() override { return g_fServerFrameTime; }

        // ---- rotation maths, ILTCSBase spelling (UP, RIGHT, FORWARD) ----- //
        // See engine/kernel/math/ltmathlt.cpp for why every one of these was an
        // NPC-flattening bug.
        LTRESULT GetRotationVectors(LTRotation* pRot, LTVector* pUp,
                                    LTVector* pRight, LTVector* pFwd) override
        {
            LTIMPL("ILTCSBase::GetRotationVectors");
            if (!pRot) return LT_ERROR;
            float q[4], r[3], u[3], f[3]; LTQ(*pRot, q);
            LTMathImpl::GetVectors(q, r, u, f);
            if (pRight) pRight->Init(r[0], r[1], r[2]);
            if (pUp)    pUp->Init(u[0], u[1], u[2]);
            if (pFwd)   pFwd->Init(f[0], f[1], f[2]);
            return LT_OK;
        }
        void RotateAroundAxis(LTRotation* pRot, LTVector* pAxis, float amount) override
        {
            LTIMPL("ILTCSBase::RotateAroundAxis");
            if (!pRot || !pAxis) return;
            float q[4], a[3] = { pAxis->x, pAxis->y, pAxis->z }; LTQ(*pRot, q);
            LTMathImpl::RotateAroundAxis(q, a, amount); LTR(*pRot, q);
        }
        void EulerRotateX(LTRotation* p, float a) override { LTVector v(1,0,0); RotateAroundAxis(p, &v, a); }
        void EulerRotateY(LTRotation* p, float a) override { LTVector v(0,1,0); RotateAroundAxis(p, &v, a); }
        void EulerRotateZ(LTRotation* p, float a) override { LTVector v(0,0,1); RotateAroundAxis(p, &v, a); }
        void AlignRotation(LTRotation* pRot, LTVector* pV, LTVector* pUp) override
        {
            LTIMPL("ILTCSBase::AlignRotation");
            if (!pRot || !pV) return;
            float q[4], d[3] = { pV->x, pV->y, pV->z }, u[3] = { 0, 1, 0 };
            if (pUp) { u[0] = pUp->x; u[1] = pUp->y; u[2] = pUp->z; }
            LTMathImpl::AlignRotation(q, d, u); LTR(*pRot, q);
        }
        LTRESULT SetupEuler(LTRotation* pRot, float pitch, float yaw, float roll) override
        {
            LTIMPL("ILTCSBase::SetupEuler");
            if (!pRot) return LT_ERROR;
            float q[4]; LTMathImpl::SetupEuler(q, pitch, yaw, roll); LTR(*pRot, q);
            return LT_OK;
        }
        LTRESULT InterpolateRotation(LTRotation* pD, LTRotation* pA, LTRotation* pB, float t) override
        {
            LTIMPL("ILTCSBase::InterpolateRotation");
            if (!pD || !pA || !pB) return LT_ERROR;
            float a[4], b[4], o[4]; LTQ(*pA, a); LTQ(*pB, b);
            LTMathImpl::Slerp(o, a, b, t); LTR(*pD, o);
            return LT_OK;
        }
        void SetupRotationMatrix(LTMatrix* pMat, LTRotation* pRot) override
        {
            LTIMPL("ILTCSBase::SetupRotationMatrix");
            if (!pMat || !pRot) return;
            float q[4]; LTQ(*pRot, q); QuatToMatrix(q, *pMat);
        }
        void SetupRotationFromMatrix(LTRotation* pRot, LTMatrix* pMat) override
        {
            LTIMPL("ILTCSBase::SetupRotationFromMatrix");
            if (!pRot || !pMat) return;
            float q[4]; MatrixToQuat(*pMat, q); LTR(*pRot, q);
        }

        // ---- model animation, SERVER side ---------------------------- //
        //
        // ⚠️ THESE ARE ILTCSBase VIRTUALS, SO THERE ARE TWO OF EACH -- one
        // reached through ILTClient and one through ILTServer. The client's
        // have been wired to the scene for a while; these had never been
        // overridden at all, so every animation the GAME selects fell into a
        // stub:
        //
        //     msg victim  (animate anim=walkstop)     a cinematic actor
        //     msg volkov  (animate anim=move1)        "
        //     CAnimationMgr / CAI state changes       every character
        //
        // That is why cinematics are "a slow dolly across a dead set": the
        // camera path is server-side and correct, and nothing it points at was
        // ever told to move.
        //
        // ⚠️ THE SERVER HAS NO SCENE, only the model's FILENAME. So the name
        // lookup resolves through the shared model cache the client draws
        // from -- one cache, one answer, no way for the two halves to disagree
        // about what index a name means.
        // ⚠️ The two-string form of the same thing (SecurityCamera.cpp:271
        // swaps its light's model this way). Routed to the same record so the
        // two spellings cannot disagree.
        LTRESULT SetObjectFilenames(HOBJECT hObj, char* pFilename,
                                    char* pSkinName) override
        {
            LTIMPL("ILTServer::SetObjectFilenames");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p) return LT_ERROR;

            if (pFilename && pFilename[0]) p->m_sFilename = pFilename;
            if (pSkinName && pSkinName[0])
            {
                p->m_Skins.clear();
                p->m_Skins.push_back(pSkinName);
            }
            return LT_OK;
        }

        // ⚠️ THE DIMS AN ANIMATION DECLARES -- AND A STUB HERE IS A 6e28-UNIT
        // DOORKNOB.
        //
        // `Prop::...` does exactly this, and trusts it:
        //
        //     LTVector vDims;                          // uninitialised
        //     g_pLTServer->GetModelAnimUserDims(m_hObject, &vDims, anim);
        //     vNewDims.x = m_vScale.x * vDims.x;       // used regardless
        //
        // The stub wrote nothing, so every Prop in the game took its collision
        // size from stack garbage. Measured on T01S03's DoorKnob1a:
        // dims (6.0e28, 0, 6.0e28). A box that size swallows every Activate
        // ray in the level, which is why the training door could not be opened
        // however carefully it was aimed at -- the knob in front of it was
        // effectively infinite.
        //
        // ⚠️ The client has had a correct version of this for a while; only
        // the SERVER's was missing, and the server is the half that owns
        // collision. Same shape as the other ILTCSBase pairs (HANDOVER 4.5).
        LTRESULT GetModelAnimUserDims(HOBJECT hObj, LTVector* pDims,
                                      HMODELANIM hAnim) override
        {
            LTIMPL("ILTServer::GetModelAnimUserDims");
            if (!pDims) return LT_INVALIDPARAMS;
            return LTHost_AnimUserDims(hObj, pDims, hAnim);
        }

        HMODELANIM GetAnimIndex(HOBJECT hObj, char* pAnimName) override
        {
            LTIMPL("ILTCSBase::GetAnimIndex");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || !pAnimName) return INVALID_MODEL_ANIM;

            const int n = LTScene_GetAnimIndexForFile(p->m_sFilename.c_str(),
                                                      pAnimName);
            return (n < 0) ? INVALID_MODEL_ANIM : (HMODELANIM)n;
        }

        void SetModelAnimation(HOBJECT hObj, HMODELANIM hAnim) override
        {
            LTIMPL("ILTCSBase::SetModelAnimation");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p) return;

            // ⚠️ INVALID means "none", and it must CLEAR rather than be
            // ignored: the game uses it to stop an animation.
            const int n = (hAnim == INVALID_MODEL_ANIM) ? -1 : (int)hAnim;

            // ⚠️ THE CLOCK RESTARTS ONLY ON A CHANGE. The game re-sends the
            // same animation every update while a state holds; restarting on
            // each call would freeze every character on its first frame.
            if (n != p->m_nAnim) p->m_fAnimStart = LTHost_AnimStartAfterBlend(p, p->m_nAnim, n);
            p->m_nAnim = n;
        }

        HMODELANIM GetModelAnimation(HOBJECT hObj) override
        {
            LTIMPL("ILTCSBase::GetModelAnimation");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || p->m_nAnim < 0) return INVALID_MODEL_ANIM;
            return (HMODELANIM)p->m_nAnim;
        }

        void SetModelLooping(HOBJECT hObj, LTBOOL bLoop) override
        {
            LTIMPL("ILTCSBase::SetModelLooping");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (p) p->m_bAnimLooping = (bLoop != 0);
        }

        LTBOOL GetModelLooping(HOBJECT hObj) override
        {
            LTIMPL("ILTCSBase::GetModelLooping");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            return (p && p->m_bAnimLooping) ? LTTRUE : LTFALSE;
        }

        // ⚠️ THE OLD-API TWIN OF ILTModel::GetPlaybackState, and it was the
        // stub (always 0). CAnimationContext (AnimationMgr.cpp:384) plays a
        // TRANSITION -- guard to aim, stand to crouch -- with SetModelAnimation
        // and waits here for MS_PLAYDONE before starting the real animation;
        // locked and "special" animations wait the same way. With no DONE ever,
        // every AI that needed a transition froze in it: M01S01's assassins
        // reached their nodes and stood in '+1StGd-1StGuAi' forever, never
        // reaching the fire animation whose FIRE key shoots.
        //
        // Same rule as the tracker version and the client's (ltscene.cpp
        // LTScene_IsAnimDone, donor modellt_impl.cpp trk_IsStopped): a looping
        // animation is never done, an unknown length is not done.
        // LT_NOPLAYDONE=1 restores the stub's answer (says so).
        uint32 GetModelPlaybackState(HOBJECT hObj) override
        {
            LTIMPL("ILTCSBase::GetModelPlaybackState");
            static const bool bOff = (getenv("LT_NOPLAYDONE") != 0);
            if (bOff)
            {
                static bool bSaid = false;
                if (!bSaid) { bSaid = true; printf("[ANIM] server MS_PLAYDONE DISABLED (LT_NOPLAYDONE)\n"); fflush(stdout); }
                return 0;
            }
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p || p->m_nAnim < 0 || p->m_bAnimLooping) return 0;
            const int nLen = LTScene_GetAnimLengthForFile(p->m_sFilename.c_str(), p->m_nAnim);
            if (nLen < 0) return 0;
            return ((g_fServerTime - p->m_fAnimStart) * 1000.0f >= (float)nLen) ? (uint32)MS_PLAYDONE : 0u;
        }

        LTRESULT ResetModelAnimation(HOBJECT hObj) override
        {
            LTIMPL("ILTCSBase::ResetModelAnimation");
            SRec* p = CLTServerObjects::FromHandle(hObj);
            if (!p) return LT_ERROR;

            // ⚠️ Restart, do not clear. The game calls this to play the SAME
            // animation again from the top -- clearing the index would freeze
            // the object in its bind pose instead. Re-selecting the index is
            // what the client's SetAnim treats as a restart.
            // play it again from the top -- a blend-in still running stays
            // (trk_Reset does not touch the interpolation counter)
            if (p->m_fAnimStart <= g_fServerTime) p->m_fAnimStart = g_fServerTime;
            return LT_OK;
        }

        // ⚠️ THE GAME'S OWN VOICE, AND IT WAS BEING THROWN AWAY.
        //
        // `CPrint` is how NOLF reports on itself -- a keyframer complaining
        // that it is out of sync, a trigger naming an object it could not
        // find, an AI saying which goal it refused. All of it went into a stub
        // that recorded only "ILTCSBase::CPrint was reached".
        //
        // ⚠️ This is not a diagnostic FOR the port, it is the diagnostic the
        // ORIGINAL AUTHORS wrote for this exact code, and it costs four lines
        // to get back. Every hour spent inferring game state from engine-side
        // traces was spent with this channel switched off.
        //
        // Prefixed and on stdout with the rest, so a run's log stays one
        // stream. LT_NOGAMEPRINT silences it for a run that wants only the
        // engine's own lines.
        void CPrint(char* pMsg, ...) override
        {
            static const bool bQuiet = (getenv("LT_NOGAMEPRINT") != 0);
            if (bQuiet || !pMsg) return;

            char szBuf[1024];
            va_list args;
            va_start(args, pMsg);
            vsnprintf(szBuf, sizeof(szBuf), pMsg, args);
            va_end(args);

            printf("[GAME/S] %s\n", szBuf);
            fflush(stdout);
        }

        LTRESULT OpenFile(char* pFilename, ILTStream** pStream) override
        {
            if (pStream) *pStream = 0;
            if (!pFilename || !pStream) return LT_ERROR;

            ILTStream* p = LTGetFileMgr()->OpenFile(pFilename);
            if (!p) return LT_ERROR;

            *pStream = p;
            return LT_OK;
        }

        // ⚠️ THE OBJECT QUERIES. Note the SHAPE of each one: FindNamedObjects,
        // both GetObjectName overloads, GetObjectFlags/SetObjectFlags and
        // GetObjectDims/SetObjectDims are VIRTUALS, while GetObjectClass,
        // IsKindOf and GetNextObject next to them are FUNCTION POINTERS. The
        // interface is a hybrid and the two look identical at the call site --
        // overriding a function-pointer member reaches nothing and assigning a
        // virtual does not compile, so each one is checked against the header
        // rather than guessed (HANDOVER.md §4.5).
        LTRESULT FindNamedObjects(char* pName, BaseObjArray<HOBJECT>& objArray,
                                  uint32* pnTotalFound = 0) override
        {
            return LTHost_FindNamedObjects(pName, objArray, pnTotalFound);
        }

        char* GetObjectName(HOBJECT h) override
        {
            return LTHost_GetObjectNameOld(h);
        }

        LTRESULT GetObjectName(HOBJECT h, char* pName, uint32 nBufSize) override
        {
            return LTHost_GetObjectName(h, pName, nBufSize);
        }

        uint32 GetObjectFlags(HOBJECT h) override        { return LTHost_GetObjectFlags(h); }
        void   SetObjectFlags(HOBJECT h, uint32 f) override { LTHost_SetObjectFlags(h, f); }

        void     GetObjectDims(HOBJECT h, LTVector* p) override { LTHost_GetObjectDims(h, p); }
        LTRESULT SetObjectDims(HOBJECT h, LTVector* p) override { return LTHost_SetObjectDims(h, p); }

        // ⚠️ A VIRTUAL, not a function pointer -- iltserver.h:637 marks it
        // OBSOLETE in favour of PhysicsLT::MoveObject and NOLF calls it
        // anyway. The hybrid interface again (§4.5): its neighbour
        // RotateObject two lines below in the same header IS a pointer.
        LTRESULT MoveObject(HOBJECT h, LTVector* p) override { return LTHost_MoveObject(h, p); }

        // ⚠️ ILTServer'S OWN VELOCITY / MASS / FRICTION VIRTUALS -- the same
        // hybrid-interface trap as the global force below: iltserver.h carries
        // obsolete twins of the ILTPhysics calls, NOLF uses both, and only the
        // physics ones were implemented. The twins were stubs, so a dying
        // character's body (BodyState.cpp:59, :513, :1143), the player's
        // gravity reset (PlayerObj.cpp:1081) and Character.cpp:2554 set
        // nothing, and GetVelocity (AI.cpp:1929, PlayerObj.cpp:1075, BodyState
        // .cpp:1168) left the caller's LTVector UNWRITTEN. Every one now goes
        // to g_ServerPhysics, so there is one store.
        LTRESULT GetVelocity(HOBJECT h, LTVector* p) override     { return g_ServerPhysics.GetVelocity(h, p); }
        LTRESULT SetVelocity(HOBJECT h, LTVector* p) override     { return g_ServerPhysics.SetVelocity(h, p); }
        LTRESULT GetAcceleration(HOBJECT h, LTVector* p) override { return g_ServerPhysics.GetAcceleration(h, p); }
        LTRESULT SetAcceleration(HOBJECT h, LTVector* p) override { return g_ServerPhysics.SetAcceleration(h, p); }
        float GetObjectMass(HOBJECT h) override
        {
            float f = 0.0f; g_ServerPhysics.GetObjectMass(h, f); return f;
        }
        void SetObjectMass(HOBJECT h, float f) override           { g_ServerPhysics.SetObjectMass(h, f); }
        float GetForceIgnoreLimit(HOBJECT h, float& f) override
        {
            f = 0.0f; g_ServerPhysics.GetForceIgnoreLimit(h, f); return f;
        }
        void SetForceIgnoreLimit(HOBJECT h, float f) override     { g_ServerPhysics.SetForceIgnoreLimit(h, f); }
        LTRESULT SetFrictionCoefficient(HOBJECT h, float f) override { return g_ServerPhysics.SetFrictionCoefficient(h, f); }

        // ⚠️ ILTServer HAS ITS OWN GLOBAL-FORCE PAIR, separate virtuals from
        // the ILTPhysics ones (iltserver.h:180 vs iltphysics.h:68). Both must
        // reach the SAME store or the two halves of the engine disagree about
        // gravity.
        //
        // ⚠️ THE FIFTH UNINITIALISED OUT-PARAMETER. `CPlayerObj::
        // UpdateClientPhysics` does `LTVector vGravity;
        // GetGlobalForce(&vGravity);` and sends the result to the client,
        // which installs it as the world's gravity. With the stub leaving the
        // local untouched the server shipped a stack pattern as gravity, and
        // the player accelerated upward at 1.7e27 units/s².
        LTRESULT GetGlobalForce(LTVector* pVec) override
        {
            if (!pVec) return LT_INVALIDPARAMS;
            float f[3]; LTServerPhys_GetGlobalForce(f);
            pVec->Init(f[0], f[1], f[2]);
            return LT_OK;
        }
        LTRESULT SetGlobalForce(LTVector* pVec) override
        {
            if (!pVec) return LT_INVALIDPARAMS;
            const float f[3] = { pVec->x, pVec->y, pVec->z };
            LTServerPhys_SetGlobalForce(f);
            return LT_OK;
        }

        // ⚠️ A VIRTUAL while its GETTER is a function pointer (iltserver.h:747
        // vs :750) -- the hybrid interface again (§4.5). Assigning this one
        // like a pointer compiles for the getter and silently does nothing for
        // the setter, which is how the pair gets half-wired.
        LTRESULT SetModelFilenames(HOBJECT h, char* pF, char* pS) override
        { return LTHost_SetModelFilenames(h, pF, pS); }

        // ⚠️ A VIRTUAL on ILTCSBase (iltcsbase.h:205), shared with the client
        // interface and therefore NOT a function pointer. The hybrid again.
        LTRESULT RemoveObject(HOBJECT h) override { return LTHost_RemoveObject(h); }

        // ⚠️ VIRTUALS on ILTCSBase, and the handle they take came from
        // GetGameConVar above -- so the server reads its OWN store, not the
        // user console the client reads. Same two function names, two
        // different sets of variables, which is what the SDK describes.
        float GetVarValueFloat(HCONSOLEVAR hVar) override
        {
            return g_GameConsole.GetFloat((const LTConsoleVar*)hVar);
        }

        char* GetVarValueString(HCONSOLEVAR hVar) override
        {
            // ⚠️ NULL FOR A NULL HANDLE -- the SDK's own words (iltcsbase.h:193).
            // The console answers "" for a missing variable, and
            // CDestructible::DebugDamageOn (Destructible.cpp:1817) reads the
            // unset `SetDamage` var as `!atoi(pVal)`: "" made it TRUE, and NO
            // OBJECT IN THE GAME EVER TOOK DAMAGE -- no enemy could be killed
            // (HANDOVER 5bb). LT_NULLVARSTRING=1 restores the "" answer.
            static const bool s_bOld = []() {
                const bool b = (getenv("LT_NULLVARSTRING") != 0);
                if (b) { printf("[SERVER] LT_NULLVARSTRING=1 -- a missing console var reads as \"\"\n"); fflush(stdout); }
                return b; }();
            if (!hVar && !s_bOld) return 0;
            // ⚠️ Non-const, because the SDK says so. The string belongs to the
            // console and outlives the call; the game reads it and copies.
            return (char*)g_GameConsole.GetString((const LTConsoleVar*)hVar);
        }

        // ⚠️ SetObjectDims2 differs from SetObjectDims only in whether the
        // engine may refuse a size that would put the object inside geometry.
        // With no such check here they are the same call -- and saying so is
        // better than leaving one of them a stub that silently keeps the old
        // dimensions.
        LTRESULT SetObjectDims2(HOBJECT h, LTVector* p) override { return LTHost_SetObjectDims(h, p); }

        // ⚠️ ZEROES THE STRUCT, AND THAT ALONE IS THE FIX. This port has no
        // collision system, so the honest answer is "nothing" -- but the stub
        // that answered LT_ERROR never touched the caller's struct, and
        // `CollisionInfo Info;` is a bare LOCAL with no constructor
        // (ltbasedefs.h:513). CCharacter::UpdateOnGround then reads
        // `Info.m_hObject` -- a stack pattern -- and hands it to IsKindOf,
        // which follows it as a ClassDef*. Measured: SEGV in LTHost_IsKindOf
        // on the first frame an AI updated.
        //
        // ⚠️ THE STUB'S RETURN CODE WAS NEVER THE PROBLEM. The game discards
        // it (Character.cpp:1773). An out-parameter left untouched is not a
        // "not implemented" signal to anybody -- it is garbage the caller
        // believes. Same lesson as HANDOVER §2.
        //
        // ⚠️ DEVIATION: characters never notice they are standing on a
        // Breakable, so a crate under an AI does not break. Recorded rather
        // than hidden; the fix is a collision system, not a different answer
        // here.
        LTRESULT GetStandingOn(HOBJECT hObj, CollisionInfo* pInfo) override
        {
            (void)hObj;
            if (!pInfo) return LT_INVALIDPARAMS;
            memset(pInfo, 0, sizeof(*pInfo));
            return LT_OK;
        }

        // ⚠️ THE MESSAGE PIPE, SERVER END. Every one of these is a pure
        // virtual on ILTServer, so the generated stub answered NULL and the
        // game wrote into it -- which is why nothing the server said had ever
        // reached the client. They are one-liners because the buffer and the
        // queueing are shared with the client half (engine/shared/ltmessage.cpp);
        // that sharing is the point, not brevity.
        HMESSAGEWRITE StartMessage(HCLIENT hSendTo, uint8 messageID) override
        {
            return (HMESSAGEWRITE)LTMessage_StartToClient(hSendTo, messageID);
        }

        HMESSAGEWRITE StartMessageToObject(LPBASECLASS pSender, HOBJECT hSendTo,
                                           uint32 messageID) override
        {
            return (HMESSAGEWRITE)LTMessage_StartToObject(pSender, hSendTo, messageID);
        }

        // ⚠️ THE ODD ONE OUT: it returns the handle through an OUT-PARAMETER
        // rather than as its result, so a failure that leaves the parameter
        // untouched hands the caller a garbage HMESSAGEWRITE to write into.
        // Written on every path, including the refusal.
        LTRESULT StartMessageToServer(LPBASECLASS pSender, uint32 messageID,
                                      HMESSAGEWRITE* hWrite) override
        {
            if (!hWrite) return LT_ERROR;
            *hWrite = (HMESSAGEWRITE)LTMessage_StartToShell(pSender, messageID);
            return *hWrite ? LT_OK : LT_ERROR;
        }

        HMESSAGEWRITE StartSpecialEffectMessage(LPBASECLASS pObject) override
        {
            return (HMESSAGEWRITE)LTMessage_StartSFX(pObject);
        }

        // ⚠️ THE MODERN SPELLING OF StartSpecialEffectMessage, and one class
        // uses it: Sprinkles (M06S03's plankton and drifting foliage) builds
        // its effect in a Common()->CreateMessage buffer and hands it over
        // here. The bytes become the object's effect exactly as an ended
        // StartSpecialEffectMessage would -- captured for the object (a save
        // and a later re-delivery carry it) and queued for the client. Both
        // halves were stubs, so the effect never reached the client (5dh).
        LTRESULT SetObjectSFXMessage(HOBJECT hObject, ILTMessage& msg) override
        {
            LTIMPL("ILTServer::SetObjectSFXMessage");
            LPBASECLASS pObj = LTHost_HandleToObject(hObject);
            const CLTMessage* pSrc = dynamic_cast<const CLTMessage*>(&msg);
            if (!pObj || !pSrc) return LT_INVALIDPARAMS;
            CLTMessage* p = LTMessage_StartSFX(pObj);
            if (!p) return LT_ERROR;
            if (pSrc->Size()) p->PutRaw(pSrc->Bytes(), pSrc->Size());
            return LTMessage_End(p, MESSAGE_GUARANTEED);
        }

        HMESSAGEWRITE StartInstantSpecialEffectMessage(LTVector* pPos) override
        {
            const float p[3] = { pPos ? pPos->x : 0.0f,
                                 pPos ? pPos->y : 0.0f,
                                 pPos ? pPos->z : 0.0f };
            return (HMESSAGEWRITE)LTMessage_StartInstantSFX(p);
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

        // ⚠️ THE STRING TABLE, WHICH THE SERVER NEVER HAD. These are virtuals
        // on ILTCSBase, the same family as OpenFile -- and they were stubs, so
        // every `CreateString` in ObjectDLL returned NULL. That is not a
        // cosmetic loss: an object's Target, its Name and every message it
        // sends by string are HSTRINGs, so a trigger whose target string is
        // null targets nothing, silently. It is the same shape of failure as
        // the null bute managers, one layer along.
        //
        // ⚠️ The SAME manager the client uses, deliberately. Two tables would
        // mean an HSTRING written by the server and read by the client resolved
        // in a table that had never heard of it.
        HSTRING FormatString(int messageCode, ...) override
        {
            va_list args;
            va_start(args, messageCode);
            HSTRING h = LTGetStringMgr()->Format(messageCode, args);
            va_end(args);
            return h;
        }

        // ⚠️ LT_STRTRACE="<text>" -- every string the GAME builds that matches.
        // Object-to-object trigger messages carry an HSTRING handle, not text,
        // so a message census shows "id 1000, 8 bytes" and tells you nothing
        // about WHICH command it is. Watching the strings being created names
        // them, with the server clock beside each one.
        HSTRING CreateString(char* pString) override
        {
            if (pString)
            {
                static const char* pWant = getenv("LT_STRTRACE");
                if (pWant && *pWant && strstr(pString, pWant))
                {
                    printf("[STR] t=%.3f  the game built \"%s\"\n",
                           g_fServerTime, pString);
                    fflush(stdout);
                }
            }
            return LTGetStringMgr()->Create(pString);
        }
        HSTRING CopyString(HSTRING h) override       { return LTGetStringMgr()->Copy(h); }
        void    FreeString(HSTRING h) override       { LTGetStringMgr()->Free(h); }
        // ⚠️ THE SERVER ANSWERS NULL FOR NULL -- only the CLIENT answers ""
        // (reference: serverde_impl.cpp:970 -> str_GetStringData, NULL for
        // NULL; clientde_impl.cpp:1384 ""). The game depends on the
        // difference: CCharacter::GetHeadSkinFilename (Character.h:268) passes
        // GetStringData(m_hstrHeadExtension) unchecked, and NULL means
        // "_head" -- given "" every head wore its BODY skin and the M01S01
        // assassins lost their faces (user report, 2026-10-17; 5bz had made
        // both sides answer ""). LT_SERVERSTRINGEMPTY=1 = that 5bz behaviour.
        char*   GetStringData(HSTRING h) override
        {
            static const bool s_bEmpty = []() {
                const bool b = (getenv("LT_SERVERSTRINGEMPTY") != 0);
                if (b) { printf("[SERVER] LT_SERVERSTRINGEMPTY=1 -- server GetStringData(NULL) answers \"\" (5bz, faceless heads)\n"); fflush(stdout); }
                return b; }();
            if (s_bEmpty) return LTStringData_ForGame(h);
            return h ? LTGetStringMgr()->GetData(h) : 0;
        }
        LTBOOL  CompareStrings(HSTRING a, HSTRING b) override      { return LTGetStringMgr()->Compare(a, b); }
        LTBOOL  CompareStringsUpper(HSTRING a, HSTRING b) override { return LTGetStringMgr()->CompareUpper(a, b); }

        LTSkeletonServer()
        {
            // Sub-interface POINTER members (reached through inline accessors).
            LTSkel_WireData();

            // ⚠️ AND the 129 function-pointer MEMBERS, which are the reason the
            // very first CreateServerShell() segfaulted with no [LTSTUB] line
            // in front of it. They are not virtuals, so no override reaches
            // them; left NULL the game jumps to address zero. This is the same
            // trap the client side hit at GetScreenSurface with pc 0x0 --
            // HANDOVER.md §4.5 -- and it caught the server side within one run
            // of switching it on.
            LTSkel_WireFnPtrs_ILTServer(this);

            // ...then the deliberate replacements, which must come SECOND so
            // they win. Each one is here because the GAME uses its output
            // unconditionally; see the note above LTHost_GetGameInfo.
            GetGameInfo  = LTHost_GetGameInfo;
            GetClass     = LTHost_GetClass;
            IsCommandOn  = LTHost_ServerIsCommandOn;
            CreateObject = LTHost_CreateObject;
            CreateObjectProps = LTHost_CreateObjectProps;
            GetPropGeneric = LTHost_GetPropGeneric;
            CreateAttachment = LTHost_CreateAttachment;
            RemoveAttachment = LTHost_RemoveAttachment;
            FindAttachment   = LTHost_FindAttachment;
            PlaySound        = LTHost_ServerPlaySound;
            IsSoundDone      = LTHost_ServerIsSoundDone;
            GetSoundDuration = LTHost_ServerSoundDuration;
            KillSound        = LTHost_ServerKillSound;
            KillSoundLoop    = LTHost_ServerKillSoundLoop;

            SetGameConVar    = LTHost_SetGameConVar;
            GetGameConVar    = LTHost_GetGameConVar;
            RunGameConString = LTHost_RunGameConString;

            GetPropString  = LTHost_GetPropString;
            GetPropReal    = LTHost_GetPropReal;
            GetPropBool    = LTHost_GetPropBool;
            GetPropLongInt = LTHost_GetPropLongInt;
            GetPropFlags   = LTHost_GetPropFlags;
            GetPropVector  = LTHost_GetPropVector;
            GetPropColor   = LTHost_GetPropColor;
            GetPropRotation      = LTHost_GetPropRotation;
            GetPropRotationEuler = LTHost_GetPropRotationEuler;

            HandleToObject      = LTHost_HandleToObject;
            GetLastCollision    = LTHost_GetLastCollision;
            CreateInterObjectLink = LTHost_CreateInterObjectLink;
            BreakInterObjectLink  = LTHost_BreakInterObjectLink;
            ObjectToHandle      = LTHost_ObjectToHandle;
            GetObjectPos        = LTHost_GetObjectPos;
            SetObjectPos        = LTHost_SetObjectPos;
            GetObjectContainers = LTHost_GetObjectContainers;
            GetContainedObjects = LTHost_GetContainedObjects;
            GetObjectRotation   = LTHost_GetObjectRotation;
            SetObjectRotation   = LTHost_SetObjectRotation;
            RotateObject        = LTHost_RotateObject;
            GetObjectUserFlags  = LTHost_GetObjectUserFlags;
            SetNextUpdate       = LTHost_SetNextUpdate;
            SetObjectUserFlags  = LTHost_SetObjectUserFlags;
            if (!LTHost_NoServerColor())
            {
                GetObjectColor  = LTHost_GetObjectColor;
                SetObjectColor  = LTHost_SetObjectColor;
            }

            // The function-pointer half of the object queries.
            GetObjectClass         = LTHost_GetObjectClass;
            IsKindOf               = LTHost_IsKindOf;
            GetNextObject          = LTHost_GetNextObject;
            GetNextInactiveObject  = LTHost_GetNextInactiveObject;
            Parse                  = LTHost_Parse;
            AddObjectToList        = LTHost_AddObjectToList;
            SaveObjects            = LTHost_SaveObjects;
            RestoreObjects         = LTHost_RestoreObjects;
            BPrint                 = LTHost_BPrint;

            // ⚠️ The four that decide whether a level change keeps its player
            // or builds a second one -- see LTHost_GetNextClientRef.
            GetNextClientRef       = LTHost_GetNextClientRef;
            GetClientRefInfoFlags  = LTHost_GetClientRefInfoFlags;
            GetClientRefName       = LTHost_GetClientRefName;
            GetClientRefObject     = LTHost_GetClientRefObject;

            SetClientViewPos       = LTHost_SetClientViewPos;
            // Activation, HANDOVER 5cp.
            SetClientInfoFlags     = LTHost_SetClientInfoFlags;
            GetClientInfoFlags     = LTHost_GetClientInfoFlags;
            SetDeactivationTime    = LTHost_SetDeactivationTime;
            SetObjectState         = LTHost_SetObjectState;
            GetObjectState         = LTHost_GetObjectState;
            PingObjects            = LTHost_PingObjects;
            GetPortalFlags         = LTHost_GetPortalFlags;
            SetPortalFlags         = LTHost_SetPortalFlags;
            SetClientUserData      = LTHost_SetClientUserData;
            GetClientUserData      = LTHost_GetClientUserData;
            CreateObjectList       = LTHost_CreateObjectList;
            RelinquishList         = LTHost_RelinquishList;

            // ⚠️ The pair the GAME uses to choose its own level -- see the
            // block above LTServerHost_StartWorld.
            TeleportObject         = LTHost_TeleportObject;
            IntersectSegment       = LTHost_IntersectSegment;
            GetModelFilenames      = LTHost_GetModelFilenames;
            LoadWorld              = LTHost_LoadWorld;
            RunWorld               = LTHost_RunWorld;

            // ⚠️ AFTER LTSkel_WireData, which pointed this at a stub.
            m_pCommonLT            = &g_ServerCommon;
            m_pModelLT             = &g_ServerModel;
            m_pPhysicsLT           = &g_ServerPhysics;
            m_pTransformLT         = &g_ServerTransform;
        }
    };


    LTSkeletonServer*   g_pServer   = 0;
    IServerShell*       g_pShell    = 0;
    void*               g_pLib      = 0;
    DeleteServerShellFn g_pDelete   = 0;

    // Defined here rather than above because it needs g_pShell, and a forward
    // declaration plus a definition next to the thing it uses beats moving the
    // shell pointer up above the class that creates it.
    bool LTHost_DeliverToServer(CLTMessage* pMsg)
    {
        if (!pMsg || !g_pShell) return false;

        switch (pMsg->Dest())
        {
            case kLTMsg_ToServer:
                g_pShell->OnMessage((HCLIENT)pMsg->m_hClient,
                                    (uint8)pMsg->GetId(), (HMESSAGEREAD)pMsg);
                return true;

            case kLTMsg_ToShell:
                g_pShell->OnObjectMessage((LPBASECLASS)pMsg->m_pSender,
                                          pMsg->GetId(), (HMESSAGEREAD)pMsg);
                return true;

            case kLTMsg_ToObject:
            {
                SRec* pTo = CLTServerObjects::FromHandle(pMsg->m_hTarget);

                // ⚠️ A HANDLE THAT IS NO LONGER AN OBJECT IS NOT DELIVERED.
                // The donor dereferences the target unchecked
                // (serverde_impl.cpp SendToObject) -- safe THERE because a
                // removed object stays allocated until the frame ends. Here
                // SweepDead frees it, so a game that kept the handle sent
                // MID_DAMAGE into freed memory: M03S02's intro, t=58,
                // SIGSEGV (HANDOVER 5bi). An object marked dead THIS frame is
                // still in the list and still gets its messages, as in the
                // donor. Dropped ones are named, once per sender and id.
                static const bool s_bNoGuard = []() {
                    const bool b = (getenv("LT_NOMSGGUARD") != 0);
                    if (b) { printf("[MSG] LT_NOMSGGUARD=1 -- object messages delivered to any handle, freed or not\n"); fflush(stdout); }
                    return b; }();
                // The world record (5bg) is not in the list and has no game
                // object: a message to the world -- steam or a bullet hitting
                // a wall -- goes nowhere, silently, as it always did.
                if (pTo == LTHost_WorldRecord()) return false;
                if (pTo && !s_bNoGuard && !g_Objects.Contains(pTo))
                {
                    static std::map<std::pair<const void*, unsigned>, unsigned> s_Told;
                    const SRec* pFrom = pMsg->m_pSender ? g_Objects.RecordOf((BaseClass*)pMsg->m_pSender) : 0;
                    if (s_Told[std::make_pair(pMsg->m_pSender, pMsg->GetId())]++ == 0)
                    {
                        const std::string sWas = g_Objects.FreedName(pTo);
                        printf("[MSG] t=%.2f object message %u from %s '%s' to %p (was: %s) -- not an object any more, DROPPED\n",
                               g_fServerTime, pMsg->GetId(),
                               (pFrom && pFrom->m_pClass) ? pFrom->m_pClass->m_ClassName : "?",
                               pFrom ? pFrom->m_sName.c_str() : "", (void*)pTo,
                               sWas.empty() ? "unknown" : sWas.c_str());
                        fflush(stdout);
                    }
                    return false;
                }
                if (!pTo || !pTo->m_pGame) return false;

                // ⚠️ ObjectMessageFn wants the sender's HOBJECT, not its
                // BaseClass*. The game stores its own handle in m_hObject, so
                // this is a read rather than a lookup -- but passing the
                // BaseClass* through would give every handler a pointer it
                // would then feed back to the engine as a handle.
                HOBJECT hSender = pMsg->m_pSender
                                ? ((BaseClass*)pMsg->m_pSender)->m_hObject
                                : (HOBJECT)0;

                // LT_DAMAGETRACE=1 -- every MID_DAMAGE (1001) with sender,
                // RECIPIENT and amount. "The shot hit and nothing died" is
                // a question about who the damage went to.
                // LT_TRIGTRACE=2 -- trigger commands with time, sender and
                // RECIPIENT. A level script is a graph; text alone cannot say
                // which trigger a "trigger" went to.
                {
                    static const char* pTT = getenv("LT_TRIGTRACE");
                    if (pMsg->GetId() == 1000 && pTT && !strcmp(pTT, "2") && pMsg->Size() >= sizeof(uintptr_t))
                    {
                        uintptr_t v = 0;
                        memcpy(&v, pMsg->Bytes(), sizeof(v));
                        const SRec* pFrom = pMsg->m_pSender ? g_Objects.RecordOf((BaseClass*)pMsg->m_pSender) : 0;
                        printf("[TRIG] t=%.2f %s -> %s: \"%s\"\n", g_fServerTime,
                               pFrom ? pFrom->m_sName.c_str() : "?", pTo->m_sName.c_str(),
                               v ? (const char*)v : "");
                        fflush(stdout);
                    }
                }
                if (pMsg->GetId() == 1001 && getenv("LT_DAMAGETRACE") && pMsg->Size() >= 21)
                {
                    float fDmg = 0.0f;
                    memcpy(&fDmg, pMsg->Bytes() + 16, 4);
                    if (fDmg != 0.0f)
                    {
                        const SRec* pFrom = pMsg->m_pSender ? g_Objects.RecordOf((BaseClass*)pMsg->m_pSender) : 0;
                        printf("[DMG] t=%.2f %.1f (type %u) from %s '%s' to %s '%s'\n", g_fServerTime, fDmg,
                               (unsigned)pMsg->Bytes()[20],
                               (pFrom && pFrom->m_pClass) ? pFrom->m_pClass->m_ClassName : "?",
                               pFrom ? pFrom->m_sName.c_str() : "",
                               pTo->m_pClass ? pTo->m_pClass->m_ClassName : "?", pTo->m_sName.c_str());
                        fflush(stdout);
                    }
                }

                pTo->m_pGame->ObjectMessageFn(hSender, pMsg->GetId(),
                                              (HMESSAGEREAD)pMsg);
                return true;
            }

            default:
                return false;
        }
    }


    typedef void (*GetFnsFn)(CreateServerShellFn*, DeleteServerShellFn*);
    typedef int  (*GetVerFn)();

    // Where the dylib is, unless NOLF_SERVER says otherwise. Beside the
    // executable is where build_skeleton.sh puts it.
    // ⚠️ BESIDE THIS EXECUTABLE -- NOT A FIXED PATH IN /tmp.
    //
    // This used to fall back to "/tmp/libnolfobject.dylib", and that is the
    // stale-binary trap that `nolfbin.sh` exists to prevent, wearing a
    // different hat. Any other build on the machine can write that path: an
    // ASan build did, and the next ordinary run dlopen'd a sanitized dylib into
    // an unsanitized process and aborted before it drew a frame. The gate
    // reported "no frame was dumped", which is true and says nothing about
    // where the dylib came from.
    //
    // ⚠️ A GAME'S SERVER MODULE SHIPS NEXT TO ITS EXECUTABLE. Asking the loader
    // where THIS binary is cannot pick up somebody else's build, needs no
    // environment variable, and is right for an installed copy as well as a
    // development tree. NOLF_SERVER still overrides, which is what the ASan
    // runs use on purpose.
    std::string DefaultPath()
    {
        if (const char* p = getenv("NOLF_SERVER")) return p;

        char szExe[4096];
        uint32_t nLen = sizeof(szExe);
        if (_NSGetExecutablePath(szExe, &nLen) == 0)
        {
            // ⚠️ May be a symlink or contain "..", and the answer is handed to
            // dlopen either way -- resolve it so the directory is the real one.
            char szReal[4096];
            const char* pUse = realpath(szExe, szReal) ? szReal : szExe;

            std::string s(pUse);
            const size_t i = s.find_last_of('/');
            if (i != std::string::npos)
                return s.substr(0, i) + "/libnolfobject.dylib";
        }

        // ⚠️ Last resort only, and it keeps NOLF_BIN working for the scripts
        // that set it. There is deliberately no /tmp default any more: failing
        // to find the module is better than finding the wrong one.
        if (const char* p = getenv("NOLF_BIN"))
        {
            std::string s(p);
            const size_t i = s.find_last_of('/');
            if (i != std::string::npos) return s.substr(0, i) + "/libnolfobject.dylib";
        }

        return "libnolfobject.dylib";
    }
}


// ⚠️ ON BY DEFAULT NOW, AND THAT IS THE POINT.
//
// This was `LT_SERVER != 0` -- off unless asked. The reason given was the
// frame contract, whose reference is a recording of a run with no server in
// it, and it was a good reason while the server was new. It stopped being one
// the moment the menus could start a level: a person who launches this game
// gets the main menu, picks a mission, presses Start -- and NOTHING HAPPENS,
// silently, because there is no server for the game to ask. The one path a
// player actually takes was the one path that needed an environment variable.
//
// ⚠️ THE CONTRACT IS PROTECTED WHERE IT IS MADE, NOT HERE. runrecord.sh now
// exports LT_NOSERVER=1, so the recorded run is the same headless no-server
// run the reference was blessed from -- and, better than before, it no longer
// depends on whether the CALLER happened to have LT_SERVER exported. The
// record used to be quietly different for anyone with it set in their shell.
//
// LT_SERVER is still accepted and still means yes, so every script and every
// line of documentation that sets it keeps working.
bool LTServerHost_Enabled()
{
    static const bool b = (getenv("LT_NOSERVER") == 0);
    return b;
}


bool LTServerHost_Start(CLTFileMgr* /*pFileMgr*/, const char* pPath)
{
    if (g_pShell) return true;

    const std::string sPath = pPath ? pPath : DefaultPath();

    // ⚠️ RTLD_NOW, not LAZY. The whole point of the first bring-up is to find
    // out what the game needs from the engine; a lazy bind would hide a missing
    // symbol until the exact moment it is called, which is the least useful
    // time to learn about it.
    g_pLib = dlopen(sPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_pLib)
    {
        // ⚠️ ONE dlerror() CALL. It clears itself: the old `dlerror() ?
        // dlerror() : "?"` printed the second, always-null answer, and the
        // launcher's black screen read "(null)" when the reason was the
        // hardened runtime refusing an ad-hoc-signed dylib (HANDOVER 5cq).
        const char* pWhy = dlerror();
        fprintf(stderr, "[SERVER] cannot load %s\n           %s\n",
                sPath.c_str(), pWhy ? pWhy : "?");
        LTRecord_Event("server", "dlopen failed");
        return false;
    }

    GetFnsFn pGetFns = (GetFnsFn)dlsym(g_pLib, "GetServerShellFunctions");
    GetVerFn pGetVer = (GetVerFn)dlsym(g_pLib, "GetServerShellVersion");
    if (!pGetFns)
    {
        fprintf(stderr, "[SERVER] %s has no GetServerShellFunctions\n", sPath.c_str());
        dlclose(g_pLib); g_pLib = 0;
        LTRecord_Event("server", "no entry point");
        return false;
    }

    // ⚠️ The version is the module's own statement about which interface it was
    // built against. Checking it costs nothing and turns "it crashed" into "it
    // is the wrong module".
    const int nVer = pGetVer ? pGetVer() : -1;
    printf("[SERVER] loaded %s (shell version %d, expected %d)\n",
           sPath.c_str(), nVer, SERVERSHELL_VERSION);
    if (pGetVer && nVer != SERVERSHELL_VERSION)
        fprintf(stderr, "[SERVER] ⚠️  shell version mismatch -- continuing anyway\n");

    CreateServerShellFn pCreate = 0;
    pGetFns(&pCreate, &g_pDelete);
    if (!pCreate)
    {
        fprintf(stderr, "[SERVER] GetServerShellFunctions gave no CreateServerShell\n");
        dlclose(g_pLib); g_pLib = 0;
        return false;
    }

    g_pServer = new LTSkeletonServer();

    // ⚠️ BEFORE CreateServerShell, like the class table below and for the same
    // reason: the shell's constructor can already send.
    LTMessage_SetServerDeliver(LTHost_DeliverToServer);

    // ⚠️ BEFORE CreateServerShell. The game's constructor may already look a
    // class up, and a class table that arrives afterwards would be correct and
    // too late -- the same ordering trap that made HookWindow fail silently
    // (HANDOVER.md §2).
    g_Objects.Init(g_pLib, g_pServer);

    const int nStubsBefore = LTStub_Count();
    printf("[SERVER] calling the game's CreateServerShell()...\n");
    fflush(stdout);

    g_pShell = pCreate(g_pServer);

    printf("[SERVER] CreateServerShell returned %s  (stubs reached: %d)\n",
           g_pShell ? "a shell" : "NULL", LTStub_Count() - nStubsBefore);
    fflush(stdout);

    LTRecord_Event("server", "CreateServerShell %s", g_pShell ? "ok" : "null");
    return g_pShell != 0;
}


HCLIENT       LTServerHost_ClientHandle() { return LTHost_TheClient(); }

// ⚠️ THE LIVE OBJECTS THE WORLD RENDERER MUST ADD -- see
// ILTWorldDynamicObjects for why characters were never on screen.
//
// Drawn straight from SERVER state, not from the client's replicated mirrors.
// In single player both halves share one process, the server is authoritative
// for position and -- as of today -- for which animation is playing, and the
// animation index is not yet replicated. Reading the record here means a
// character stands where the game put it and moves the way the game asked,
// with no copy in between to fall behind.
namespace
{
    bool ClassIsA(const ClassDef* p, const char* pName)
    {
        for (; p; p = p->m_ParentClass)
            if (p->m_ClassName && strcasecmp(p->m_ClassName, pName) == 0) return true;
        return false;
    }

    bool IsModelFile(const std::string& s)
    {
        return s.size() > 4 && strcasecmp(s.c_str() + s.size() - 4, ".abc") == 0;
    }

    // ---- ATTACHMENTS -- ILTServer::CreateAttachment and friends ------- //
    //
    // ⚠️ ALL THREE WERE STUBS. The game builds an attachment as an ordinary
    // object (CAttachments::CreateObjectAttachment / CreateWeaponAttachment)
    // and then asks the engine to keep it on a SOCKET of its parent every
    // frame. With the stub the object existed and was never placed -- so no
    // character in the port ever wore or held anything. The one that showed
    // it: M01S01's cinematic Cate carries `Head="headset"`, a radio headset
    // whose earpiece and microphone are plainly visible in the Windows build,
    // and the port drew her without it.
    //
    // ⚠️ PLACED ON DEMAND, from the parent's CURRENT pose -- the same model,
    // the same keyframe rule (LTModelDraw::KeyframeAt), the same origin
    // offset and the same node control the renderer uses for the parent. A
    // second placement rule is how an attachment ends up a frame behind, or
    // beside, the head it belongs to.
    // (struct Attach and g_Attach are above LTHost_SaveObjects -- saved since v8)

    // LT_ATTACHEVERYFRAME=1 -- the old rule: socket-less children re-placed
    // from their parent every frame, whether the parent moved or not.
    bool AttachEveryFrame()
    {
        static const bool b = []() {
            const bool bOld = (getenv("LT_ATTACHEVERYFRAME") != 0);
            if (bOld) { printf("[ATTACH] LT_ATTACHEVERYFRAME=1 -- socket-less attachments pinned to their parent every frame (old)\n"); fflush(stdout); }
            return bOld; }();
        return b;
    }

    Attach* AttachOfChild(const SRec* pChild)
    {
        for (size_t i = 0; i < g_Attach.size(); ++i)
            if (g_Attach[i]->pChild == pChild) return g_Attach[i];
        return 0;
    }

    LTRESULT LTHost_CreateAttachment(HOBJECT hParent, HOBJECT hChild, char* pSocketName,
                                     LTVector* pOffset, LTRotation* pRotOffset,
                                     HATTACHMENT* pAttachment)
    {
        LTIMPL("ILTServer::CreateAttachment");
        if (pAttachment) *pAttachment = 0;      // written on every path
        // ⚠️ NO SOCKET IS A VALID ATTACHMENT, and it was refused. Door::
        // AttachObject (Door.cpp:1214), ServerMark, Trigger and GameBase all
        // pass LTNULL: the child keeps a fixed offset from the parent object
        // itself. Refused, T02S01's display case lifted its door and left the
        // solid glass pane (sg1partition) where it was -- the glasses could
        // not be reached (user report, HANDOVER 5bl). See
        // LTHost_UpdateFreeAttachments.
        if (!hParent || !hChild) return LT_ERROR;

        Attach* a = new Attach;
        a->pParent = (SRec*)hParent;
        a->pChild  = (SRec*)hChild;
        a->sSocket = pSocketName ? pSocketName : "";
        a->ofs[0] = pOffset ? pOffset->x : 0.0f;
        a->ofs[1] = pOffset ? pOffset->y : 0.0f;
        a->ofs[2] = pOffset ? pOffset->z : 0.0f;
        a->rot[0] = pRotOffset ? pRotOffset->m_Quat[0] : 0.0f;
        a->rot[1] = pRotOffset ? pRotOffset->m_Quat[1] : 0.0f;
        a->rot[2] = pRotOffset ? pRotOffset->m_Quat[2] : 0.0f;
        a->rot[3] = pRotOffset ? pRotOffset->m_Quat[3] : 1.0f;
        for (int k = 0; k < 3; ++k) a->lastPPos[k] = a->pParent->m_Pos[k];
        for (int k = 0; k < 4; ++k) a->lastPRot[k] = a->pParent->m_Rot[k];
        g_Attach.push_back(a);
        if (pAttachment) *pAttachment = (HATTACHMENT)a;

        if (getenv("LT_ATTACHTRACE"))
        {
            printf("[ATTACH] %s '%s' on socket '%s' of %s '%s'\n", a->pChild->m_sFilename.c_str(),
                   a->pChild->m_sName.c_str(), pSocketName ? pSocketName : "(none)",
                   a->pParent->m_sFilename.c_str(), a->pParent->m_sName.c_str());
            fflush(stdout);
        }
        return LT_OK;
    }

    LTRESULT LTHost_RemoveAttachment(HATTACHMENT hAttachment)
    {
        LTIMPL("ILTServer::RemoveAttachment");
        for (size_t i = 0; i < g_Attach.size(); ++i)
            if ((HATTACHMENT)g_Attach[i] == hAttachment)
            { delete g_Attach[i]; g_Attach.erase(g_Attach.begin() + i); return LT_OK; }
        return LT_ERROR;
    }

    // ⚠️ AN ATTACHMENT DIES WITH EITHER OF ITS OBJECTS (5cu). Nothing removed
    // them when a record was freed -- the sweep or a world change -- and the
    // allocator hands the same address to the next object: a stale entry
    // would then pull an unrelated object along, and since v8 save it too.
    // Called right after every free, before anything new is created.
    // Contains() never dereferences the pointer.
    void LTHost_ForgetFreedAttachments()
    {
        for (size_t i = 0; i < g_Attach.size(); )
        {
            if (g_Objects.Contains(g_Attach[i]->pParent) && g_Objects.Contains(g_Attach[i]->pChild)) { ++i; continue; }
            delete g_Attach[i];
            g_Attach.erase(g_Attach.begin() + i);
        }
    }

    LTRESULT LTHost_FindAttachment(HOBJECT hParent, HOBJECT hChild, HATTACHMENT* pAttachment)
    {
        LTIMPL("ILTServer::FindAttachment");
        if (pAttachment) *pAttachment = 0;
        for (size_t i = 0; i < g_Attach.size(); ++i)
            if ((HOBJECT)g_Attach[i]->pParent == hParent && (HOBJECT)g_Attach[i]->pChild == hChild)
            { if (pAttachment) *pAttachment = (HATTACHMENT)g_Attach[i]; return LT_OK; }
        return LT_ERROR;
    }

    // The renderer's node-control trampoline, repeated for the socket pose.
    struct SockTramp { LTDynNodeFn fn; void* hObj; void* user; };
    void SockTrampoline(unsigned int nNode, float* pGlobal16, void* pUser)
    {
        const SockTramp* t = (const SockTramp*)pUser;
        t->fn(t->hObj, nNode, pGlobal16, t->user);
    }

    void QuatFromRot(const float* m, float* q)
    {
        // ⚠️ From an ORTHONORMALISED basis -- a scaled one gives a visible
        // tilt with a correct position (HANDOVER: the sunglasses).
        float c[3][3];
        for (int col = 0; col < 3; ++col)
        {
            float l = sqrtf(m[col]*m[col] + m[4+col]*m[4+col] + m[8+col]*m[8+col]);
            if (l < 1e-9f) l = 1.0f;
            for (int r = 0; r < 3; ++r) c[r][col] = m[r*4+col] / l;
        }
        const float tr = c[0][0] + c[1][1] + c[2][2];
        if (tr > 0.0f)
        {
            float s = sqrtf(tr + 1.0f) * 2.0f;
            q[3] = 0.25f * s; q[0] = (c[2][1] - c[1][2]) / s;
            q[1] = (c[0][2] - c[2][0]) / s; q[2] = (c[1][0] - c[0][1]) / s;
        }
        else if (c[0][0] > c[1][1] && c[0][0] > c[2][2])
        {
            float s = sqrtf(1.0f + c[0][0] - c[1][1] - c[2][2]) * 2.0f;
            q[3] = (c[2][1] - c[1][2]) / s; q[0] = 0.25f * s;
            q[1] = (c[0][1] + c[1][0]) / s; q[2] = (c[0][2] + c[2][0]) / s;
        }
        else if (c[1][1] > c[2][2])
        {
            float s = sqrtf(1.0f + c[1][1] - c[0][0] - c[2][2]) * 2.0f;
            q[3] = (c[0][2] - c[2][0]) / s; q[0] = (c[0][1] + c[1][0]) / s;
            q[1] = 0.25f * s; q[2] = (c[1][2] + c[2][1]) / s;
        }
        else
        {
            float s = sqrtf(1.0f + c[2][2] - c[0][0] - c[1][1]) * 2.0f;
            q[3] = (c[1][0] - c[0][1]) / s; q[0] = (c[0][2] + c[2][0]) / s;
            q[1] = (c[1][2] + c[2][1]) / s; q[2] = 0.25f * s;
        }
    }

    // ⚠️ AN ANIMATION NEVER SET IS ANIMATION 0, PLAYING (HANDOVER 5cl). The
    // shipped engine's tracker init (lithtech.exe 0x401050) starts every
    // model on animation 0 with the playing and looping flags set; the port
    // drew such a model in its raw stored mesh instead. Identical for most
    // models, but 61 carry a 180-degree turn at the root of animation 0 --
    // the Moroccan agents' sunglasses (an attachment whose Prop never finds a
    // "World" animation) sat on the backs of their heads. World-file props
    // already got this rule (5bt/5cg); game-spawned models did not.
    // LT_NODEFAULTANIM=1 = the raw mesh again (old, says so).
    int LTHost_DrawAnim(const SRec* P)
    {
        static const bool s_bOld = []() {
            const bool b = (getenv("LT_NODEFAULTANIM") != 0);
            if (b) { printf("[SERVER] LT_NODEFAULTANIM=1 -- a model never given an animation is drawn in its raw mesh (old)\n"); fflush(stdout); }
            return b; }();
        if (P->m_nAnim >= 0) return P->m_nAnim;
        return s_bOld ? -1 : 0;
    }

    // Where an attached child is THIS frame. False -> draw it where it is.
    // ⚠️ ONE CHARACTER'S POSE ON THE SERVER, NOW -- the renderer's rule
    // (animation, extra-tracker layers, the game's node control). Shared by
    // attachment placement (sockets) and ILTModel::GetNodeTransform (the
    // per-node hit detection), so a bullet tests the bones that are drawn.
    bool LTHost_PoseOf(SRec* P, const CLTABCModel*& pModel, LTModelPose& pose)
    {
        pModel = LTScene_ModelForFile(P->m_sFilename.c_str());
        if (!pModel) return false;

        bool bPose = false;
        const int nAnim = LTHost_DrawAnim(P);
        if (nAnim >= 0 && (size_t)nAnim < pModel->GetAnims().size())
        {
            const unsigned kf = LTModelDraw::KeyframeAt(*pModel, (unsigned)nAnim,
                                                        LTHost_AnimElapsed(P->m_fAnimStart),
                                                        P->m_bAnimLooping);
            SockTramp tr; tr.fn = 0; tr.hObj = 0; tr.user = 0;
            if (void* hClient = LTReplicate_Find((void*)P))
            {
                LTSceneNodeFn fn = 0; void* u = 0;
                if (LTScene_GetNodeControl(hClient, &fn, &u) && fn)
                { tr.fn = (LTDynNodeFn)fn; tr.hObj = hClient; tr.user = u; }
            }
            // The SAME layered pose the renderer draws -- a blink moves no
            // socket today, but a twitch or a pitch does.
            std::vector<LTDynamicLayer> dl;
            LTHost_LayersFor(P, dl);
            std::vector<LTModelDraw::Layer> layers;
            for (size_t l = 0; l < dl.size(); ++l)
            {
                if ((size_t)dl[l].m_nAnim >= pModel->GetAnims().size()) continue;
                LTModelDraw::Layer y;
                y.m_nAnim      = (unsigned)dl[l].m_nAnim;
                y.m_nKeyframe  = LTModelDraw::KeyframeAt(*pModel, (unsigned)dl[l].m_nAnim,
                                                         dl[l].m_fAnimTime, dl[l].m_bLooping);
                y.m_nWeightSet = dl[l].m_nWeightSet;
                layers.push_back(y);
            }
            bPose = LTModelDraw::BuildPose(*pModel, (unsigned)nAnim, kf, pose,
                                           tr.fn ? SockTrampoline : 0, tr.fn ? &tr : 0,
                                           layers.empty() ? 0 : &layers[0],
                                           (unsigned)layers.size());
        }
        if (!bPose && !LTModelDraw::BuildBindPose(*pModel, pose)) return false;
        return true;
    }

    // The object's world frame, exactly as the renderer builds it: rotation
    // and position, the model's declared origin offset taken off in MODEL space.
    void LTHost_ObjectFrame(const SRec* P, const CLTABCModel& model, bool bMirror, float* W)
    {
        float ofs[3] = { 0.0f, 0.0f, 0.0f };
        model.GetOriginOffset(P->m_nAnim >= 0 ? (unsigned)P->m_nAnim : 0u, ofs);
        if (bMirror) ofs[0] = -ofs[0];
        LT3D::FromQuatPos(P->m_Rot, P->m_Pos, W);
        // The donor ADDS it (HANDOVER 5bt) -- the renderer's rule, shared.
        const float sg = LTOriginOffsetSign();
        for (int r = 0; r < 3; ++r)
            W[r*4+3] += sg * (W[r*4+0]*ofs[0] + W[r*4+1]*ofs[1] + W[r*4+2]*ofs[2]);
    }

    // ⚠️ A SOCKET-LESS ATTACHMENT, the donor's rule (smoveabstract.cpp
    // MoveAttachments): position = parent position + parent rotation applied
    // to the offset; rotation = parent rotation * offset rotation.
    void FreeAttachTransform(const Attach* a, float* pPos, float* pQuat)
    {
        const SRec* P = a->pParent;
        float R[16];
        const float z3[3] = { 0.0f, 0.0f, 0.0f };
        LT3D::FromQuatPos(P->m_Rot, z3, R);
        for (int r = 0; r < 3; ++r)
            pPos[r] = P->m_Pos[r] + R[r*4+0]*a->ofs[0] + R[r*4+1]*a->ofs[1] + R[r*4+2]*a->ofs[2];
        const float* q1 = P->m_Rot;     // x y z w
        const float* q2 = a->rot;
        pQuat[3] = q1[3]*q2[3] - q1[0]*q2[0] - q1[1]*q2[1] - q1[2]*q2[2];
        pQuat[0] = q1[3]*q2[0] + q1[0]*q2[3] + q1[1]*q2[2] - q1[2]*q2[1];
        pQuat[1] = q1[3]*q2[1] - q1[0]*q2[2] + q1[1]*q2[3] + q1[2]*q2[0];
        pQuat[2] = q1[3]*q2[2] + q1[0]*q2[1] - q1[1]*q2[0] + q1[2]*q2[3];
    }

    // Every frame, after the objects have moved and before the brushes are
    // placed: socket-less children follow their parents, so a pane attached
    // to a rising door rises with it -- in collision as well as on screen.
    // LT_NOFREEATTACH=1 = the old refusal's outcome (says so).
    //
    // ⚠️ ONLY WHEN THE PARENT HAS MOVED -- the donor's rule: MoveAttachments
    // runs at the end of the PARENT's MoveObject (moveobject.cpp:1616,
    // smoveabstract.cpp), so a parent that never moves never touches its
    // children, and a child is free to move itself. Pinning every frame
    // froze M05S01's gate: the level lists `HingedDoor8` (the gate) as an
    // attachment of `HingedDoor10`, a closed door 6,100 units away, and the
    // gate's own swing was undone each frame -- unlocked, activated, and
    // never opening (user report, HANDOVER 5by). A parent "moved" when its
    // pose differs from the one at the child's last placement.
    void LTHost_UpdateFreeAttachments()
    {
        static const bool s_bOff = []() {
            const bool b = (getenv("LT_NOFREEATTACH") != 0);
            if (b) { printf("[ATTACH] LT_NOFREEATTACH=1 -- socket-less attachments stay put\n"); fflush(stdout); }
            return b; }();
        if (s_bOff) return;
        const bool bEvery = AttachEveryFrame();
        for (size_t i = 0; i < g_Attach.size(); ++i)
        {
            Attach* a = g_Attach[i];
            if (!a->sSocket.empty() || !a->pParent || !a->pChild) continue;
            if (!g_Objects.IsLive(a->pParent) || !g_Objects.IsLive(a->pChild)) continue;
            bool bMoved = bEvery;
            for (int k = 0; k < 3 && !bMoved; ++k) bMoved = (a->pParent->m_Pos[k] != a->lastPPos[k]);
            for (int k = 0; k < 4 && !bMoved; ++k) bMoved = (a->pParent->m_Rot[k] != a->lastPRot[k]);
            if (!bMoved) continue;
            float pos[3], q[4];
            FreeAttachTransform(a, pos, q);
            for (int k = 0; k < 3; ++k) a->pChild->m_Pos[k] = pos[k];
            for (int k = 0; k < 4; ++k) a->pChild->m_Rot[k] = q[k];
            for (int k = 0; k < 3; ++k) a->lastPPos[k] = a->pParent->m_Pos[k];
            for (int k = 0; k < 4; ++k) a->lastPRot[k] = a->pParent->m_Rot[k];
            // LT_ATTACHTRACE=1 -- also each re-placement (first 5 per child).
            if (getenv("LT_ATTACHTRACE"))
            {
                static std::map<const void*, int> s_n;
                if (s_n[a->pChild]++ < 5)
                {
                    printf("[ATTACH] t=%.3f '%s' follows '%s' to (%.1f %.1f %.1f)\n", LTServerHost_Time(),
                           a->pChild->m_sName.c_str(), a->pParent->m_sName.c_str(),
                           a->pChild->m_Pos[0], a->pChild->m_Pos[1], a->pChild->m_Pos[2]);
                    fflush(stdout);
                }
            }
        }
    }

    bool PlaceAttached(SRec* pChild, float* pPos, float* pQuat)
    {
        Attach* a = AttachOfChild(pChild);
        if (!a || !a->pParent || !g_Objects.IsLive(a->pParent)) return false;
        static const bool s_bNoFree = (getenv("LT_NOFREEATTACH") != 0);
        if (a->sSocket.empty())
        {
            if (s_bNoFree) return false;
            // The record is already right: LTHost_UpdateFreeAttachments
            // re-placed it if the parent moved, and otherwise it is where the
            // child put itself.
            if (!AttachEveryFrame()) return false;
            FreeAttachTransform(a, pPos, pQuat);
            return true;
        }
        SRec* P = a->pParent;

        const CLTABCModel* pModel = 0;
        LTModelPose pose;
        if (!LTHost_PoseOf(P, pModel, pose)) return false;

        // Characters are drawn UNmirrored (HANDOVER 5as) unless LT_DYNMIRROR.
        static const bool bMirror = (getenv("LT_DYNMIRROR") != 0);
        float S[16];
        if (!LTModelDraw::SocketTransform(*pModel, pose, a->sSocket.c_str(), bMirror, S))
            return false;

        // The parent's world frame, exactly as the renderer builds it: the
        // object's rotation and position, with the model's declared origin
        // offset taken off in MODEL space.
        float W[16];
        LTHost_ObjectFrame(P, *pModel, bMirror, W);

        // ...times the socket, times the game's own offset.
        float O[16], WS[16], R[16];
        LT3D::FromQuatPos(a->rot, a->ofs, O);
        LT3D::Multiply(W, S, WS);
        LT3D::Multiply(WS, O, R);

        pPos[0] = R[3]; pPos[1] = R[7]; pPos[2] = R[11];
        QuatFromRot(R, pQuat);
        return true;
    }

    class LTServerDynamicObjects : public ILTWorldDynamicObjects
    {
    public:
        // ⚠️ LT_NODYN=1 turns this pass off -- an A/B instrument, so the
        // pixels it contributes can be isolated by a diff rather than guessed.
        unsigned int Count() const override
        {
            static const bool bOff = (getenv("LT_NODYN") != 0);
            return bOff ? 0u : g_Objects.ObjectCount();
        }

        bool Get(unsigned int i, LTDynamicModel& out) const override
        {
            SRec* p = g_Objects.RecordAt(i);
            if (!p || !g_Objects.IsLive(p) || !p->m_pGame || !p->m_pClass) return false;
            // ⚠️ IN A MIRROR (5df) LT2 draws FLAG_PORTALVISIBLE objects too --
            // "Draw in portals even if the object is invisible" (ltbasedefs.h).
            // No server object here sets FLAG2_PORTALINVISIBLE (only client
            // code does), so the server half of the rule is this one test.
            const bool bPortal = LTWorldPortalPass();
            // LT_PORTALTRACE=1 -- the player record as a mirror pass sees it
            // (every 60th ask): flags, model, position.
            if (bPortal && getenv("LT_PORTALTRACE") && ClassIsA(p->m_pClass, "CPlayerObj"))
            {
                static int s_n = 0;
                if ((s_n++ % 60) == 0)
                {
                    float R[16]; LT3D::FromQuatPos(p->m_Rot, p->m_Pos, R);
                    printf("[PORTAL] player flags 0x%08x%s%s model '%s' anim %d pos (%.0f %.0f %.0f) forward (%.2f %.2f %.2f)\n",
                           (unsigned)p->m_Flags, (p->m_Flags & FLAG_VISIBLE) ? " VISIBLE" : "",
                           (p->m_Flags & FLAG_PORTALVISIBLE) ? " PORTALVISIBLE" : "",
                           p->m_sFilename.c_str(), p->m_nAnim, p->m_Pos[0], p->m_Pos[1], p->m_Pos[2],
                           R[2], R[6], R[10]);
                    fflush(stdout);
                }
            }
            if (!(p->m_Flags & (bPortal ? (FLAG_VISIBLE | FLAG_PORTALVISIBLE) : FLAG_VISIBLE)))
                return false;
            if (!IsModelFile(p->m_sFilename)) return false;

            // ⚠️ THE LOCAL PLAYER IS NOT DRAWN. In first person the camera sits
            // inside that model, and drawing it puts the inside of Cate's head
            // over the whole screen. The weapon in her hands is the view
            // model's job, not this pass's.
            // ⚠️ ...EXCEPT IN A MIRROR (5df): that is what the game hides her
            // for and marks her FLAG_PORTALVISIBLE for (PlayerObj.cpp:167).
            if (ClassIsA(p->m_pClass, "CPlayerObj") && !bPortal) return false;
            // ⚠️ ...AND ONLY THE PLAYER'S HAND-HELD WEAPON MODEL (HANDOVER 5cl).
            // Every weapon anyone holds is a CHHWeaponModel attached to a
            // RightHand socket (Attachments.cpp CreateAttachment) -- the AI's
            // and the cinematic Cate's too. Hiding the class hid every gun in
            // every hand. LT_NOHHWEAPONS=1 = hide them all again (old, says so).
            if (ClassIsA(p->m_pClass, "CHHWeaponModel"))
            {
                static const bool s_bHideAll = []() {
                    const bool b = (getenv("LT_NOHHWEAPONS") != 0);
                    if (b) { printf("[SERVER] LT_NOHHWEAPONS=1 -- no hand-held weapon model is drawn (old)\n"); fflush(stdout); }
                    return b; }();
                const Attach* a = AttachOfChild(p);
                if (s_bHideAll || !a || !a->pParent || !g_Objects.IsLive(a->pParent) || !a->pParent->m_pClass)
                    return false;
                // The player's own gun only in a mirror, in her hand there
                // (Attachments.cpp:424 makes it FLAG_PORTALVISIBLE).
                if (ClassIsA(a->pParent->m_pClass, "CPlayerObj") && !bPortal) return false;
            }

            // ⚠️ WORLD-FILE OBJECTS ARE DRAWN BY PASS 3 already, from their own
            // Filename property. Only CHARACTERS are drawn from here even when
            // the world file placed them -- their model comes from the bute
            // manager, so pass 3 finds no Filename and draws nothing.
            // ...unless the game gave it a model the file does not name
            // (CLTServerObjects::DrawnLive -- T02S01's ledger, every pickup).
            if (p->m_bFromWorld && !ClassIsA(p->m_pClass, "CCharacter")
                && !g_Objects.DrawnLive(p)) return false;

            // ⚠️ AN ATTACHED OBJECT IS WHERE ITS PARENT'S SOCKET IS -- see
            // PlaceAttached. Written back to the record too, so the game's
            // own GetObjectPos agrees with what is drawn.
            {
                float ap[3], aq[4];
                if (PlaceAttached(p, ap, aq))
                {
                    for (int k = 0; k < 3; ++k) p->m_Pos[k] = ap[k];
                    for (int k = 0; k < 4; ++k) p->m_Rot[k] = aq[k];
                }
            }

            out.sModel = p->m_sFilename;
            out.sSkins.clear();
            for (size_t k = 0; k < p->m_Skins.size(); ++k)
            {
                if (k) out.sSkins += ";";
                out.sSkins += p->m_Skins[k];
            }
            for (int k = 0; k < 3; ++k) { out.m_Pos[k] = p->m_Pos[k]; out.m_Scale[k] = 1.0f; }
            for (int k = 0; k < 4; ++k) out.m_Quat[k] = p->m_Rot[k];
            out.m_nAnim      = LTHost_DrawAnim(p);
            out.m_fAnimTime  = LTHost_AnimElapsed(p->m_fAnimStart);
            out.m_bLooping   = p->m_bAnimLooping;
            LTHost_LayersFor(p, out.m_Layers);

            // ⚠️ THE GAME'S NODE CONTROL lives on the CLIENT mirror of this
            // object -- CNodeController registers it on GetServerObj() as the
            // client sees it. Handed to the renderer with the pose.
            out.m_pfnNode = 0; out.m_hNodeObj = 0; out.m_pNodeUser = 0;
            if (void* hClient = LTReplicate_Find((void*)p))
            {
                LTSceneNodeFn fn = 0; void* pUser = 0;
                if (LTScene_GetNodeControl(hClient, &fn, &pUser) && fn)
                {
                    out.m_pfnNode   = (LTDynNodeFn)fn;
                    out.m_hNodeObj  = hClient;
                    out.m_pNodeUser = pUser;
                }
            }

            // ⚠️ LT_DYNTRACE=1 -- each drawn character ONCE, with where it is.
            // A screenshot that contains nobody cannot tell "not drawn" from
            // "drawn somewhere the camera is not looking"; a position can.
            static const bool bTrace = (getenv("LT_DYNTRACE") != 0);
            if (bTrace)
            {
                // Printed when first drawn AND whenever its animation changes --
                // "is the AI choosing animations" is the question, and a single
                // first sighting at anim -1 cannot answer it.
                static std::map<const void*, int> seen;
                std::map<const void*, int>::iterator it = seen.find(p);
                if (it == seen.end() || it->second != p->m_nAnim)
                {
                    seen[p] = p->m_nAnim;
                    const float ql = sqrtf(p->m_Rot[0]*p->m_Rot[0] + p->m_Rot[1]*p->m_Rot[1]
                                         + p->m_Rot[2]*p->m_Rot[2] + p->m_Rot[3]*p->m_Rot[3]);
                    printf("[DYN] %-24s %-44s at (%.0f %.0f %.0f) anim %d |q| %.4f\n",
                           p->m_pClass->m_ClassName, p->m_sFilename.c_str(),
                           p->m_Pos[0], p->m_Pos[1], p->m_Pos[2], p->m_nAnim, ql);
                    fflush(stdout);
                }
            }
            return true;
        }
    };
    LTServerDynamicObjects g_DynamicObjects;
}

const ILTWorldDynamicObjects* LTServerHost_DynamicObjects() { return &g_DynamicObjects; }


// ⚠️ THE SERVER'S HALF OF THE COMMAND TRANSITION. The client shell's
// OnCommandOn is where activate/weapon/reload live; the SERVER shell's
// OnCommandOn (GameServerShell.cpp:1446) is a separate callback on a separate
// interface, and it owns run lock. Both are required, and delivering only one
// leaves the two halves disagreeing about what the player is holding.
//
// ⚠️ SILENT BEFORE THE SHELL EXISTS, on purpose. Keys work in the menus, where
// there is no server at all, and a null check here is the difference between
// that and a crash on the first keypress of a menu-only run.
void LTServerHost_OnCommand(int nCommand, bool bOn)
{
    if (!g_pShell) return;
    if (bOn) g_pShell->OnCommandOn(LTHost_TheClient(), nCommand);
    else     g_pShell->OnCommandOff(LTHost_TheClient(), nCommand);
}
IServerShell* LTServerHost_GetShell()  { return g_pShell; }
ILTServer*    LTServerHost_GetServer() { return g_pServer; }


namespace { int g_nLastStep = 0; unsigned int g_nLastUpdated = 0;
             unsigned int g_nMsgIn = 0;

             // ⚠️ THE SERVER'S PLAYER OBJECT, kept so the host can mirror its
             // position onto the CLIENT's player object every frame. That
             // mirroring is one object's worth of replication -- deliberately
             // the only object replicated so far, because it is the one the
             // camera follows and everything else can wait for a real
             // server->client object protocol.
             HOBJECT g_hPlayer = 0; }
int LTServerHost_StubsLastStep() { return g_nLastStep; }


// ----------------------------------------------------------------------- //
//
// ⚠️ THE WORLD-LOAD PAIR THE GAME ITSELF DRIVES.
//
// These are not a convenience split. They are the sequence the engine performs,
// and the game reaches BOTH of them from inside its own message handler:
//
//     dResult = g_pLTServer->LoadWorld(pLGFileName, flags);   // :1945
//     ...
//     dResult = g_pLTServer->RunWorld();                      // :1988
//                                    -- CGameServerShell::HandleLoadGameMsg
//
// So "which level is playing" is the GAME's decision, arriving as a
// MID_LOAD_GAME message from the client's menu. The host's LT_WORLD path below
// is a harness convenience layered on top of the same two calls, never a
// second way of loading a level.
//
// ⚠️ THE HOOKS BELONG TO DIFFERENT HALVES OF THE PAIR:
//   PreStartWorld  -> LoadWorld, before anything exists ("one is coming")
//   PostStartWorld -> RunWorld,  after every object does ("go and find them")
// Collapsing them would run the game's level setup against an empty world, and
// it would find nothing, silently.
//
// ----------------------------------------------------------------------- //

namespace
{
    bool g_bWorldRunning = false;
    bool g_bWorldStartedLatch = false;

    // ⚠️ HAS A WORLD EVER BEEN LOADED -- which is NOT g_bWorldRunning. The
    // teardown must happen on the second LoadWorld and every one after it, and
    // must NOT happen on the first, where there is nothing to destroy and the
    // object list legitimately holds nothing. g_bWorldRunning is false during a
    // load by then, so it cannot answer this question.
    bool g_bWorldEverLoaded = false;

    LTRESULT LTHost_LoadWorld(char* pszWorldFileName, uint32 flags)
    {
        if (!g_pShell || !pszWorldFileName || !*pszWorldFileName) return LT_ERROR;

        printf("[SERVER] LoadWorld(\"%s\", flags 0x%x)\n", pszWorldFileName, flags);
        fflush(stdout);

        // ⚠️ "Switching worlds" is true only if one was already RUNNING. The
        // game uses it to decide whether to carry state across, so answering
        // yes on the first level would make it behave like a continuation.
        g_pShell->PreStartWorld(g_bWorldRunning ? LTTRUE : LTFALSE);
        g_bWorldRunning = false;

        // ⚠️ THE CLIENT IS TAKEN OUT OF THE OLD WORLD NOW, in the donor's
        // order (servermgr.cpp: PreStartWorld, then DoEndWorld ->
        // sm_GetClientOutOfWorld, s_client.cpp:557): first the SERVER game's
        // OnClientExitWorld -- it removes the old player object and clears the
        // client's user data -- then the local client unloads (OnExitWorld).
        //
        // ⚠️ BOTH WERE MISSING OR LATE. OnClientExitWorld was never called, so
        // after the old objects were destroyed GetClientUserData still pointed
        // at the freed player; any client message in the gap (the client's
        // OnExitWorld sends MID_PLAYER_CHATMODE) was a use-after-free in
        // CPlayerObj::SetChatting (ASan). And the client's OnExitWorld ran when
        // it ENTERED the new world, after the new level's camera and character
        // effects had been delivered -- the game's m_sfxMgr.RemoveAll() threw
        // them away: no camera effect, so the listener never moved to the
        // cinematic camera and every lip-synced line was culled as out of
        // range. Every level reached by a level change (HANDOVER 5bi).
        if (g_hPlayer)
        {
            printf("[SERVER] taking the client out of the old world (OnClientExitWorld)\n");
            fflush(stdout);
            g_pShell->OnClientExitWorld(LTHost_TheClient());
            g_hPlayer = 0;
        }
        if (g_pWorldClosing) g_pWorldClosing();

        // ⚠️ THE CLIENT'S MIRRORS BELONG TO THE OLD LEVEL, and this is the last
        // moment they are still valid -- the objects they mirror are about to
        // be destroyed. Clearing them later, at the point the CLIENT enters the
        // world, threw away the mirrors made DURING the load: 27 made, 1 alive,
        // which read as replication working and was replication being undone.
        LTReplicate_Reset();

        // ⚠️ AND NOW THE OLD WORLD'S OBJECTS ARE ACTUALLY DESTROYED. The
        // comment above has said "about to be destroyed" since it was written;
        // nothing did it, and the second level was built on top of the first.
        // The game found two players, two WorldProperties and two of every
        // shared trigger name, and reported all three itself.
        //
        // ⚠️ SAFE ONLY BECAUSE OF WHERE WE ARE. This runs inside the game's
        // HandleLoadGameMsg, reached from the message pump -- no object's
        // update is on the stack -- and immediately after PreStartWorld, which
        // is where the game drops the references that would otherwise dangle.
        // Calling it from anywhere else would be a use-after-free.
        //
        // ⚠️ ANYTHING THAT MUST SURVIVE HAS ALREADY BEEN WRITTEN OUT. The game
        // saves its keep-alives -- the player, the player's hit box and the
        // weapon in their hands -- BEFORE asking for the new world, and
        // RestoreObjects builds them into it a moment from now.
        if (g_bWorldEverLoaded)
        {
            // The host's own mirror of the player is a handle into what is
            // about to be freed.
            g_hPlayer = 0;

            // Trackers point at the records about to be freed.
            LTHost_ResetTrackers();

            const unsigned int nGone = g_Objects.ClearObjects();
            LTHost_ForgetFreedAttachments();
            printf("[SERVER] world change: destroyed %u object(s) from the "
                   "previous level\n", nGone);
        }
        g_bWorldEverLoaded = true;

        // ⚠️ THE SAME SCENE THE CLIENT DRAWS. The server needs the world's
        // objects and the client needs its geometry; loading it twice would be
        // two copies of a 2.3-million-triangle world and two answers to any
        // question about it. LTWorldScene_Load is a no-op when this level is
        // already the one loaded.
        if (!LTWorldScene_Load(LTGetFileMgr(), pszWorldFileName))
            return LT_ERROR;

        // ⚠️ COLLISION FOR THE NEW LEVEL, built from the SAME scene the client
        // draws -- so what stops an object is exactly what you can see stopping
        // it. Reset first: the grid is world-space and indexed, so carrying the
        // previous level's triangles into this one would put invisible walls
        // across the new map.
        LTServerPhys_Reset();

        // ⚠️ MOVABLE BRUSHES ARE DECIDED BEFORE THE GRID IS BUILT -- they are
        // excluded from it and supplied as dynamic triangles instead. Built
        // after CreateWorldObjects below would be too late; the objects do not
        // exist yet here, so the call is made once they do (see further down).
        LTServerBrush_Reset();
        LTServerPhys_BuildFor(&LTGetWorldScene()->GetWorld());

        // The level's visibility, which decides what the player's view keeps
        // awake (HANDOVER 5cp). Before the objects: a door opens its portal
        // in its own initial update.
        LTWorldVis_BuildFor(&LTGetWorldScene()->GetWorld());
        // ...and no view in it until the player's object reports one: the
        // last level's position means nothing here.
        g_bClientViewPosSet = false;

        // ⚠️ LOADWORLD_LOADWORLDOBJECTS is the game asking for the level's own
        // objects. Without the flag it wants the geometry only -- restoring a
        // save game, where the objects come from the save file instead -- and
        // creating them anyway would give it two of everything.
        if (flags & LOADWORLD_LOADWORLDOBJECTS)
        {
            CLTWorld world;
            if (!world.LoadThroughFileMgr(LTGetFileMgr(), pszWorldFileName))
            {
                printf("[SERVER] could not read objects from %s\n", pszWorldFileName);
                return LT_ERROR;
            }

            int nSkipped = 0;
            const unsigned int nMade = g_Objects.CreateWorldObjects(world, &nSkipped);

            // ⚠️ NOW the brush objects exist, so their geometry can be taken
            // out of the static grid and driven by the game instead. The grid
            // is rebuilt because the exclusion list has changed.
            LTServerBrush_Build(LTGetWorldScene()->GetWorld());
            if (LTServerBrush_Count())
            {
                LTServerPhys_Reset();
                LTServerPhys_BuildFor(&LTGetWorldScene()->GetWorld());
                LTServerBrush_Update();
            }

            // ⚠️ GIVE EVERY BRUSH OBJECT ITS BOX. See Record::m_bHasBox. The
            // world model carries the object's name and its world-space bounds;
            // matched here once, at load, case-insensitively.
            {
                const std::vector<LTWorldModelHeader>& wm = world.GetModels();
                unsigned int nBoxed = 0, nWm = 0;
                for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
                {
                    SRec* r = g_Objects.RecordAt(i);
                    if (!r || r->m_Type != OT_WORLDMODEL || r->m_sName.empty()) continue;
                    ++nWm;
                    // Boxed at creation already (CreateWorldObjects), with the
                    // rest position taken BEFORE the game could move it.
                    if (r->m_bHasBox) { ++nBoxed; continue; }
                    for (size_t k = 0; k < wm.size(); ++k)
                    {
                        if (strcasecmp(wm[k].m_sName.c_str(), r->m_sName.c_str()) != 0) continue;
                        for (int a = 0; a < 3; ++a)
                        {
                            r->m_BoxMin[a] = wm[k].m_Min[a];
                            r->m_BoxMax[a] = wm[k].m_Max[a];
                            r->m_BoxRestPos[a] = r->m_Pos[a];
                        }
                        r->m_bHasBox = true;
                        ++nBoxed;
                        break;
                    }
                }
                printf("[SERVER] %u of %u brush object(s) matched to their geometry\n",
                       nBoxed, nWm);
            }

            // ⚠️ LT_FINDMSG="<text>" -- WHO IN THIS LEVEL MENTIONS THIS?
            //
            // Level logic is written in string properties: "msg TrainingDoor1
            // unlock" sits in some trigger's Message field. When something
            // never happens, the question is which object was supposed to make
            // it happen -- and that is a text search over the world's own
            // properties, not over any code.
            if (const char* pFind = getenv("LT_FINDMSG"))
            {
                const std::vector<LTWorldObject>& obj = world.GetObjects();
                unsigned int nHits = 0;
                for (size_t i = 0; i < obj.size(); ++i)
                {
                    for (size_t k = 0; k < obj[i].m_Props.size(); ++k)
                    {
                        const LTWorldProperty& pr = obj[i].m_Props[k];
                        if (pr.m_sString.empty()) continue;
                        std::string hay = pr.m_sString, needle = pFind;
                        for (size_t c = 0; c < hay.size(); ++c) hay[c] = (char)tolower((unsigned char)hay[c]);
                        for (size_t c = 0; c < needle.size(); ++c) needle[c] = (char)tolower((unsigned char)needle[c]);
                        if (hay.find(needle) == std::string::npos) continue;
                        printf("[FIND] %-22s %-24s .%-24s = \"%s\"\n",
                               obj[i].m_sType.c_str(),
                               obj[i].GetName() ? obj[i].GetName() : "(unnamed)",
                               pr.m_sName.c_str(), pr.m_sString.c_str());
                        ++nHits;
                    }
                }
                printf("[FIND] %u mention(s) of \"%s\"\n", nHits, pFind);
                fflush(stdout);
            }

            // ⚠️ LT_NEARDUMP="x,y,z,r" -- every object within r of a point,
            // with class and name. "What is the player meant to use here?" is
            // answered by what the level put next to them.
            if (const char* pNear = getenv("LT_NEARDUMP"))
            {
                float c[4];
                if (sscanf(pNear, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) == 4)
                {
                    for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
                    {
                        SRec* r = g_Objects.RecordAt(i);
                        if (!r || !r->m_pClass) continue;
                        const float dx = r->m_Pos[0]-c[0], dy = r->m_Pos[1]-c[1], dz = r->m_Pos[2]-c[2];
                        const float d = sqrtf(dx*dx + dy*dy + dz*dz);
                        if (d > c[3]) continue;
                        printf("[NEAR] %5.0f  %-22s %-26s (%.0f %.0f %.0f) dims (%.0f %.0f %.0f)%s\n", d,
                               r->m_pClass->m_ClassName, r->m_sName.c_str(),
                               r->m_Pos[0], r->m_Pos[1], r->m_Pos[2],
                               r->m_Dims[0], r->m_Dims[1], r->m_Dims[2],
                               (r->m_Flags & (1u<<16)) ? "  TOUCH_NOTIFY" : "");
                    }
                    fflush(stdout);
                }
            }

            // ⚠️ LT_WMTRACE=1 -- every WORLD-MODEL object (doors, switches,
            // anything built from brushes) with the box the ray test would use.
            // A world model with no dims cannot be hit by IntersectSegment, so
            // pressing Activate on it hits "the world" instead.
            if (getenv("LT_WMTRACE"))
            {
                for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
                {
                    SRec* r = g_Objects.RecordAt(i);
                    if (!r || r->m_Type != OT_WORLDMODEL) continue;
                    printf("[WM] %-20s %-24s pos (%.0f %.0f %.0f) box %s (%.0f %.0f %.0f)-(%.0f %.0f %.0f) flags 0x%x\n",
                           r->m_pClass ? r->m_pClass->m_ClassName : "?",
                           r->m_sName.c_str(), r->m_Pos[0], r->m_Pos[1], r->m_Pos[2],
                           r->m_bHasBox ? "yes" : "NO ",
                           r->m_BoxMin[0], r->m_BoxMin[1], r->m_BoxMin[2],
                           r->m_BoxMax[0], r->m_BoxMax[1], r->m_BoxMax[2], r->m_Flags);
                    // USRFLG_CAN_ACTIVATE (1<<21) is the GAME saying "the
                    // player may use this" -- Door::InitialUpdate sets it only
                    // for doors whose ActivateTrigger property is on.
                    if (r->m_UserFlags & (1u << 21))
                        printf("       ^ CAN_ACTIVATE\n");
                }
                fflush(stdout);
            }
            printf("[SERVER] %s: created %u game objects, skipped %d "
                   "(no such class)\n", pszWorldFileName, nMade, nSkipped);
        }
        else
        {
            printf("[SERVER] geometry only -- the game did not ask for world "
                   "objects\n");
        }

        fflush(stdout);
        return LT_OK;
    }

    LTRESULT LTHost_RunWorld()
    {
        if (!g_pShell) return LT_ERROR;
        if (!LTWorldScene_IsLoaded()) return LT_NOTINWORLD;

        g_pShell->PostStartWorld();
        g_bWorldRunning = true;

        // ⚠️ A LATCH, READ ONCE. The GAME decides when a level starts (it
        // sends MID_LOAD_GAME from its own menu), so the host cannot know at
        // startup that a world is coming -- it has to be told, after the fact,
        // from inside a message handler several calls deep. A latch is the
        // smallest thing that carries that news back out to the frame loop,
        // where the client half of entering a world can safely happen.
        g_bWorldStartedLatch = true;

        // ⚠️ NO MID_ALLOBJECTSCREATED HERE, and that is checked rather than
        // assumed: the later engine sends one at this point, but LT2 defines
        // no such message (ltserverobj.h stops at MID_DEACTIVATING) and NOLF 1
        // never mentions it. Sending an id the game does not know would reach
        // its default case, which for most classes is "pass it to the parent"
        // and for some is anyone's guess.
        printf("[SERVER] RunWorld -- world is live\n");
        fflush(stdout);
        return LT_OK;
    }
}


void LTServerHost_StartWorld()
{
    if (!g_pShell) return;

    const int n0 = LTStub_Count();

    // ⚠️ THE HARNESS PATH, and it goes through the game's own two calls rather
    // than around them. LT_WORLD exists so a world can be brought up without
    // walking the menu; if it took a different route the thing being tested
    // would not be the thing that ships.
    const char* pWorld = getenv("LT_WORLD");
    if (pWorld && *pWorld)
    {
        char szWorld[256];
        strncpy(szWorld, pWorld, sizeof(szWorld) - 1);
        szWorld[sizeof(szWorld) - 1] = '\0';

        LTHost_LoadWorld(szWorld, LOADWORLD_LOADWORLDOBJECTS);
        LTHost_RunWorld();
    }
    else printf("[SERVER] no LT_WORLD -- the shell gets an empty world\n");

    g_nLastStep = LTStub_Count() - n0;
    printf("[SERVER] world hooks done  (stubs reached: %d)\n", g_nLastStep);
    fflush(stdout);
}


void LTServerHost_AddClient()
{
    if (!g_pShell) return;

    // ⚠️ NO CLIENT WITHOUT A RUNNING WORLD, and this is a real guard rather
    // than tidiness. `OnClientEnterWorld` -> `CreatePlayer` -> `Respawn` ->
    // `FindStartPoint`, and with no world there are no start points, so
    // `m_eModelStyle` keeps its `eModelStyleInvalid` (0xFF) and
    // `GetStyleName(0xFF)` reads 6816 bytes past the style array. It is the
    // same crash the world-file ordering bug caused, reached a different way:
    // the game has no defence against being asked to put a player somewhere
    // that does not exist, so the engine must not ask.
    //
    // ⚠️ This became reachable the moment the GAME started choosing its own
    // level: the host used to add the client at startup, when LT_WORLD had
    // already loaded one. With the console/menu path the world arrives later,
    // and the startup call was suddenly adding a client to nothing.
    if (!g_bWorldRunning)
    {
        printf("[SERVER] no world is running -- not adding a client yet\n");
        fflush(stdout);
        return;
    }

    const int n0 = LTStub_Count();
    printf("[SERVER] OnAddClient / OnClientEnterWorld...\n");
    fflush(stdout);

    // The one client -- see LTHost_TheClient. Both directions must name the
    // same value, so neither direction spells it out.
    HCLIENT hClient = LTHost_TheClient();

    g_pShell->OnAddClient(hClient);
    LPBASECLASS pPlayer = g_pShell->OnClientEnterWorld(hClient, 0, 0);
    g_hPlayer = pPlayer ? ((BaseClass*)pPlayer)->m_hObject : 0;

    g_nLastStep = LTStub_Count() - n0;
    printf("[SERVER] OnClientEnterWorld returned %s  (stubs reached: %d)\n",
           pPlayer ? "a player object" : "NULL", g_nLastStep);

    // ⚠️ WHERE THE PLAYER ACTUALLY IS, printed rather than assumed. "A player
    // object exists" and "the player is standing at the level's start point"
    // are different facts, and for a long time only the first was true: the
    // start-point lookup returned nothing, so the player kept the origin and
    // the port looked like it had a wrong spawn position rather than no spawn
    // logic at all. One line makes the two distinguishable from the outside.
    if (pPlayer)
    {
        SRec* p = CLTServerObjects::FromHandle(((BaseClass*)pPlayer)->m_hObject);
        if (p)
            printf("[SERVER] player \"%s\" at (%.1f %.1f %.1f)\n",
                   p->m_sName.c_str(), p->m_Pos[0], p->m_Pos[1], p->m_Pos[2]);
    }

    // And what it was looking for. A count of zero here is the whole story.
    {
        ObjArray<HOBJECT, 32> a;
        uint32 nTotal = 0;
        LTHost_FindNamedObjects((char*)"GameStartPoint", a, &nTotal);
        printf("[SERVER] %u GameStartPoint(s) findable by name\n", nTotal);
        for (uint32 i = 0; i < a.NumObjects(); ++i)
        {
            SRec* p = CLTServerObjects::FromHandle(a.GetObject(i));
            if (p) printf("           at (%.1f %.1f %.1f)\n",
                          p->m_Pos[0], p->m_Pos[1], p->m_Pos[2]);
        }
    }
    fflush(stdout);
}


// ⚠️ IS THIS A CHARACTER? Decided by CLASS ANCESTRY, not by a user flag.
//
// The game raises USRFLG_CHARACTER from CCharacter's own update, so a flag
// test answers "no" for the first frames of every character's life -- exactly
// the frames in which a level's opening triggers fire. Ancestry is true from
// creation.
static bool LTHost_IsCharacter(const SRec* p)
{
    for (const ClassDef* c = p ? p->m_pClass : 0; c; c = c->m_ParentClass)
        if (c->m_ClassName && strcasecmp(c->m_ClassName, "CCharacter") == 0)
            return true;
    return false;
}

// The object's world-space box. Brush objects carry a real geometry box; every
// other object is its position +/- its dims.
static void LTHost_TouchBox(const SRec* p, float* pMin, float* pMax)
{
    if (p->m_bHasBox)
    {
        for (int k = 0; k < 3; ++k)
        {
            const float d0 = p->m_Pos[k] - p->m_BoxRestPos[k];
            pMin[k] = p->m_BoxMin[k] + d0;
            pMax[k] = p->m_BoxMax[k] + d0;
        }
        return;
    }
    for (int k = 0; k < 3; ++k)
    {
        pMin[k] = p->m_Pos[k] - p->m_Dims[k];
        pMax[k] = p->m_Pos[k] + p->m_Dims[k];
    }
}

namespace { unsigned int g_nTouchSent = 0, g_nTouchPairs = 0; }

// ⚠️ AABB OVERLAP, EVERY FRAME WHILE OVERLAPPING -- which is what the original
// does. Trigger de-bounces itself (it tracks whether it has already fired), so
// an engine that notified only on the ENTERING frame would break objects that
// expect to be told continuously, and a trigger the player stands still inside
// would never fire if that one frame were missed.
//
// ⚠️ TOUCHERS ARE CHARACTERS ONLY, DELIBERATELY. A level is full of volumes
// that overlap each other permanently (AIVolume, TranslucentWorldModel,
// SoundFX); notifying those pairs fires every trigger in the level the moment
// it loads. A character and its OWN hit box is excluded for the same reason --
// they overlap by construction, every frame, for every character.
bool LTServerHost_IsLive(void* hObj)
{
    SRec* p = CLTServerObjects::FromHandle(hObj);
    return p && g_Objects.IsLive(p);
}

void LTServerHost_WorldTouch(void* hObj, void* hDoor, const float* pNormal3, float fPlaneDist,
                             unsigned int nPoly, const float* pStopVel3, float fForce)
{
    SRec* p = CLTServerObjects::FromHandle(hObj);
    if (!p || !g_Objects.IsLive(p) || !p->m_pGame || !pNormal3 || !pStopVel3) return;
    // ⚠️ A DOOR IS ITSELF, NOT THE WORLD (5dp). Its triangles are the
    // collider's dynamic set, now with an owner each; the donor reports a
    // world model it collided with as that object (collision.cpp,
    // m_pWorldObj). No polygon: the game then takes the surface from the
    // object (GetSurfaceType -> its user flags). LT_DOORASWORLD=1 = the old
    // "the world" (says so).
    static const bool s_bDoorAsWorld = []() {
        const bool b = (getenv("LT_DOORASWORLD") != 0);
        if (b) { printf("[PHYS] LT_DOORASWORLD=1 -- a door hit is reported as the world (old)\n"); fflush(stdout); }
        return b; }();
    SRec* pDoor = (hDoor && !s_bDoorAsWorld) ? CLTServerObjects::FromHandle(hDoor) : 0;
    if (pDoor && !g_Objects.IsLive(pDoor)) pDoor = 0;
    SRec* pW = pDoor ? pDoor : LTHost_WorldRecord();
    // The door's own polygon (5dq), as the donor's MakeHPoly gives it: its
    // texture's material, not the object's SurfaceOverride (most doors say
    // "Unknown", hardness 0.65).
    memset(&g_TouchColl, 0, sizeof(g_TouchColl));
    g_TouchColl.m_Plane.m_Normal.Init(pNormal3[0], pNormal3[1], pNormal3[2]);
    g_TouchColl.m_Plane.m_Dist = fPlaneDist;
    g_TouchColl.m_hObject = (HOBJECT)pW;
    g_TouchColl.m_hPoly   = (HPOLY)nPoly;
    g_TouchColl.m_vStopVel.Init(pStopVel3[0], pStopVel3[1], pStopVel3[2]);
    g_bTouchColl = true;
    // LT_WORLDTOUCHTRACE=1 -- every world touch: who, which surface, how hard.
    static const bool s_bTrace = (getenv("LT_WORLDTOUCHTRACE") != 0);
    if (s_bTrace)
    {
        uint32 nSurf = 0;
        if (nPoly) LTHost_PolyTextureFlags((HPOLY)nPoly, &nSurf);
        printf("[WTOUCH] t=%.3f %s '%s' at (%.1f %.1f %.1f) -> %s%s%s: normal (%.2f %.2f %.2f) poly %u surface %u stop (%.1f %.1f %.1f) force %.1f\n",
               g_fServerTime, p->m_pClass ? p->m_pClass->m_ClassName : "?", p->m_sName.c_str(),
               p->m_Pos[0], p->m_Pos[1], p->m_Pos[2],
               pDoor ? (pDoor->m_pClass ? pDoor->m_pClass->m_ClassName : "?") : "the world",
               pDoor ? " " : "", pDoor ? pDoor->m_sName.c_str() : "",
               pNormal3[0], pNormal3[1], pNormal3[2], nPoly, (unsigned)nSurf,
               pStopVel3[0], pStopVel3[1], pStopVel3[2], fForce);
        fflush(stdout);
    }
    // ⚠️ DEVIATIONS E-011 (5dq): A BOLT STICKS IN A DOOR. NOLF breaks a
    // crossbow bolt or speargun spear (CSpear) on any movable non-character
    // object -- "much easier to just break it" (ProjectileTypes.cpp) -- and
    // sticks it only in the main world, by spawning an AmmoBox (the bolt you
    // can pick up again) at the hit. The user asked for bolts in doors. So the
    // spear is told it touched the WORLD (the stick branch), while
    // GetLastCollision keeps naming the door: the stick-or-break hardness and
    // the hit plane are the door's. The AmmoBox the game spawns in that touch
    // is then attached to the door the way Door::AttachObject attaches (world
    // offset, rotation child x parent^-1), so it swings with it. Each bolt
    // gets its own attachment (the door's ATTACH message keeps only one).
    // ⚠️ AND A WOODEN DOOR COUNTS AS WOOD for that one touch. The game sticks
    // a bolt only in a surface of hardness <= 0.5, and its data makes "Wood
    // Door" 0.65 (plain "Wood" 0.5) -- so with the door's true material every
    // bolt still broke. Metal doors (0.75), glass and the rest keep the game's
    // rule and break it.
    // LT_NOBOLTSINDOORS=1 = the original: bolts break on doors (says so).
    static const bool s_bBreakInDoors = []() {
        const bool b = (getenv("LT_NOBOLTSINDOORS") != 0);
        if (b) { printf("[PHYS] LT_NOBOLTSINDOORS=1 -- bolts break on doors (original)\n"); fflush(stdout); }
        return b; }();
    const bool bBoltInDoor = pDoor && !s_bBreakInDoors && p->m_pClass && p->m_pClass->m_ClassName
                          && strcmp(p->m_pClass->m_ClassName, "CSpear") == 0;
    std::set<SRec*> boxesBefore;
    if (bBoltInDoor)
        for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
        {
            SRec* r = g_Objects.RecordAt(i);
            if (r && r->m_pClass && r->m_pClass->m_ClassName && strcmp(r->m_pClass->m_ClassName, "AmmoBox") == 0)
                boxesBefore.insert(r);
        }

    if (bBoltInDoor) g_nBoltDoorPoly = nPoly;
    p->m_pGame->EngineMessageFn(MID_TOUCHNOTIFY, (void*)(bBoltInDoor ? LTHost_WorldRecord() : pW), fForce);
    g_nBoltDoorPoly = 0;
    g_bTouchColl = false;
    ++g_nTouchSent;

    if (bBoltInDoor && g_Objects.IsLive(pDoor))
        for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
        {
            SRec* r = g_Objects.RecordAt(i);
            if (!r || !g_Objects.IsLive(r) || boxesBefore.count(r) || !r->m_pClass || !r->m_pClass->m_ClassName
                || strcmp(r->m_pClass->m_ClassName, "AmmoBox") != 0) continue;
            // Door::AttachObject's numbers: the offset in world axes, the
            // rotation the child's relative to the parent's.
            LTVector vOfs(r->m_Pos[0] - pDoor->m_Pos[0], r->m_Pos[1] - pDoor->m_Pos[1], r->m_Pos[2] - pDoor->m_Pos[2]);
            const float* qc = r->m_Rot;
            const float  qp[4] = { -pDoor->m_Rot[0], -pDoor->m_Rot[1], -pDoor->m_Rot[2], pDoor->m_Rot[3] };
            LTRotation rOfs;
            rOfs.m_Quat[0] = qc[3]*qp[0] + qc[0]*qp[3] + qc[1]*qp[2] - qc[2]*qp[1];
            rOfs.m_Quat[1] = qc[3]*qp[1] - qc[0]*qp[2] + qc[1]*qp[3] + qc[2]*qp[0];
            rOfs.m_Quat[2] = qc[3]*qp[2] + qc[0]*qp[1] - qc[1]*qp[0] + qc[2]*qp[3];
            rOfs.m_Quat[3] = qc[3]*qp[3] - qc[0]*qp[0] - qc[1]*qp[1] - qc[2]*qp[2];
            HATTACHMENT hA = 0;
            const LTRESULT res = LTHost_CreateAttachment((HOBJECT)pDoor, (HOBJECT)r, 0, &vOfs, &rOfs, &hA);
            if (s_bTrace)
            {
                printf("[WTOUCH] bolt stuck in %s '%s': %s '%s' attached at offset (%.1f %.1f %.1f)%s\n",
                       pDoor->m_pClass ? pDoor->m_pClass->m_ClassName : "?", pDoor->m_sName.c_str(),
                       r->m_pClass->m_ClassName, r->m_sName.c_str(), vOfs.x, vOfs.y, vOfs.z,
                       res == LT_OK ? "" : " -- FAILED");
                fflush(stdout);
            }
        }

    // ...and the door, if it asks: the plane turned round, nothing to stop
    // (doors are not moved by velocity), the force as the donor passes it.
    if (pDoor && g_Objects.IsLive(pDoor) && pDoor->m_pGame && (pDoor->m_Flags & FLAG_TOUCH_NOTIFY)
        && g_Objects.IsLive(p) && fForce * fForce >= pDoor->m_fForceIgnoreLimit * pDoor->m_fForceIgnoreLimit)
    {
        memset(&g_TouchColl, 0, sizeof(g_TouchColl));
        g_TouchColl.m_Plane.m_Normal.Init(-pNormal3[0], -pNormal3[1], -pNormal3[2]);
        g_TouchColl.m_Plane.m_Dist = -fPlaneDist;
        g_TouchColl.m_hObject = (HOBJECT)p;
        g_bTouchColl = true;
        pDoor->m_pGame->EngineMessageFn(MID_TOUCHNOTIFY, (void*)p, -fForce);
        g_bTouchColl = false;
        ++g_nTouchSent;
    }
}

void LTServerHost_TouchPass()
{
    if (!g_bWorldRunning) return;

    static const bool bOff = (getenv("LT_NOTOUCH") != 0);
    if (bOff)
    {
        static bool bSaid = false;
        if (!bSaid) { bSaid = true; printf("[TOUCH] disabled (LT_NOTOUCH)\n"); fflush(stdout); }
        return;
    }
    static const bool bTrace = (getenv("LT_TOUCHTRACE") != 0);

    // The characters, gathered once: the inner loop runs for every
    // touch-notify object in the level and there are hundreds of those.
    std::vector<SRec*> chars;
    const unsigned int nAll = g_Objects.ObjectCount();
    for (unsigned int i = 0; i < nAll; ++i)
    {
        SRec* p = g_Objects.RecordAt(i);
        if (!p || !g_Objects.IsLive(p) || !p->m_pGame) continue;
        if (LTHost_IsCharacter(p)) chars.push_back(p);
    }
    if (chars.empty()) return;

    for (unsigned int i = 0; i < nAll; ++i)
    {
        SRec* pA = g_Objects.RecordAt(i);
        if (!pA || !g_Objects.IsLive(pA) || !pA->m_pGame) continue;
        if (!(pA->m_Flags & FLAG_TOUCH_NOTIFY)) continue;

        float aMin[3], aMax[3];
        LTHost_TouchBox(pA, aMin, aMax);

        for (size_t c = 0; c < chars.size(); ++c)
        {
            SRec* pB = chars[c];
            if (pA == pB) continue;

            float bMin[3], bMax[3];
            LTHost_TouchBox(pB, bMin, bMax);

            if (aMin[0] > bMax[0] || aMax[0] < bMin[0]) continue;
            if (aMin[1] > bMax[1] || aMax[1] < bMin[1]) continue;
            if (aMin[2] > bMax[2] || aMax[2] < bMin[2]) continue;

            ++g_nTouchPairs;
            // The BOXES too: "the trigger fired too early" is a question of
            // how big each side's box was, and names alone cannot answer it.
            if (bTrace)
                printf("[TOUCH] t=%.2f %s '%s' (%.0f %.0f %.0f .. %.0f %.0f %.0f) <- %s '%s' (%.0f %.0f %.0f .. %.0f %.0f %.0f)\n",
                       g_fServerTime,
                       pA->m_pClass && pA->m_pClass->m_ClassName ? pA->m_pClass->m_ClassName : "?",
                       pA->m_sName.c_str(), aMin[0], aMin[1], aMin[2], aMax[0], aMax[1], aMax[2],
                       pB->m_pClass && pB->m_pClass->m_ClassName ? pB->m_pClass->m_ClassName : "?",
                       pB->m_sName.c_str(), bMin[0], bMin[1], bMin[2], bMax[0], bMax[1], bMax[2]);

            // ⚠️ THE OTHER OBJECT IS THE PAYLOAD, as an HOBJECT. Every handler
            // casts pData straight to HOBJECT (PickupItem.cpp:143,
            // Trigger.cpp:246) without checking anything.
            // The handle IS the record pointer here (CLTServerObjects::
            // FromHandle is a cast), so the record goes across as-is.
            LTHost_SetTouchCollision(aMin, aMax, bMin, bMax, (void*)pB);
            pA->m_pGame->EngineMessageFn(MID_TOUCHNOTIFY, (void*)pB, 0.0f);
            g_bTouchColl = false;
            ++g_nTouchSent;

            // ⚠️ AND BOTH WAYS WHEN BOTH ASKED. An AI walking into a trigger
            // volume is the trigger's business; an AI walking into a mine is
            // the AI's. Only objects carrying the flag are told.
            if (!g_Objects.IsLive(pA)) break;          // a pickup removes itself
            if (pB->m_Flags & FLAG_TOUCH_NOTIFY)
            {
                LTHost_SetTouchCollision(bMin, bMax, aMin, aMax, (void*)pA);
                pB->m_pGame->EngineMessageFn(MID_TOUCHNOTIFY, (void*)pA, 0.0f);
                g_bTouchColl = false;
                ++g_nTouchSent;
            }
        }
    }
}

void LTServerHost_Update(float fElapsed)
{
    if (!g_pShell) return;
    const int n0 = LTStub_Count();

    // ⚠️ ORDER, and every step of it is load-bearing:
    //
    //   1. DELIVER FIRST. What the client sent last frame -- its input, its
    //      request to start a level -- has to be in the player object before
    //      the player object updates, or every command acts a frame late.
    //   2. advance the clock,
    //   3. update the OBJECTS,
    //   4. then the shell, whose own Update reacts to what its objects did this
    //      frame; running it first would always act on last frame's world,
    //   5. DELIVER AGAIN, which drains everything 3 and 4 produced. A trigger
    //      that opens a door that fires another trigger completes inside this
    //      one call -- the queue is drained until empty, not once.
    g_nMsgIn = LTMessage_PumpServer();

    g_fServerFrameTime = (fElapsed > 0.0f) ? fElapsed : (1.0f / 60.0f);
    g_fServerTime += fElapsed;
    // LT_PLAYERFLAGTRACE=1 -- the CPlayerObj's engine flags and whether it is
    // scheduled to update, printed whenever either changes (5bz: a player
    // carried across a level change that never ticks keeps the flags
    // BuildKeepAlives cleared).
    if (getenv("LT_PLAYERFLAGTRACE"))
    {
        static const void* s_pLast = 0;
        static unsigned s_nFlags = 0xFFFFFFFFu;
        static int s_bSched = -1;
        for (unsigned int k = 0; k < g_Objects.ObjectCount(); ++k)
        {
            SRec* r = g_Objects.RecordAt(k);
            if (!r || !g_Objects.IsLive(r) || !ClassIsA(r->m_pClass, "CPlayerObj")) continue;
            const int bSched = (r->m_fNextUpdate > 0.0f) ? 1 : 0;
            if (r != s_pLast || (unsigned)r->m_Flags != s_nFlags || bSched != s_bSched)
            {
                printf("[PFLAGS] t=%.2f player %p flags 0x%08x%s%s next update %s\n", g_fServerTime, (void*)r,
                       (unsigned)r->m_Flags, (r->m_Flags & FLAG_VISIBLE) ? " VISIBLE" : "",
                       (r->m_Flags & FLAG_SOLID) ? " SOLID" : "", bSched ? "scheduled" : "NONE");
                fflush(stdout);
                s_pLast = r; s_nFlags = (unsigned)r->m_Flags; s_bSched = bSched;
            }
            break;
        }
    }
    // ⚠️ LT_PLAYERPOS="x,y,z[,frame]" -- A TEST HOOK. Puts the player where a
    // question needs them. A headless run cannot walk to a 32-unit trigger
    // cube 143 units away in an unknown direction, and making the test depend
    // on that tests the walking, not the trigger.
    {
        static const char* pWant = getenv("LT_PLAYERPOS");
        static bool  bDone  = false;
        static unsigned nFrame = 0;
        if (pWant && !bDone)
        {
            float v[4] = { 0, 0, 0, 0 };
            const int n = sscanf(pWant, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
            if (n >= 3 && (float)nFrame >= v[3])
            {
                SRec* p = LTHost_FindPlayerRecord();
                if (p)
                {
                    for (int k = 0; k < 3; ++k) { p->m_Pos[k] = v[k]; p->m_Vel[k] = 0.0f; }
                    printf("[PLAYERPOS] player moved to (%.0f %.0f %.0f) at frame %u\n",
                           v[0], v[1], v[2], nFrame);
                    fflush(stdout);
                    bDone = true;
                }
            }
            ++nFrame;
        }
    }

    // ⚠️ LT_ACTIVATE="<obj>,<frame>[,<message>]; ..." -- A TEST HOOK.
    //
    // Sends an object a trigger message at a chosen frame, as the PLAYER --
    // `Door::Activate` refuses anything that is not a character, so a hook with
    // no sender tests nothing. The default message is ACTIVATE, i.e. exactly
    // what pressing Activate on the object sends.
    //
    // Several may be listed, separated by ';', so a SEQUENCE can be driven:
    // start a conversation, then try the door it is supposed to unlock.
    // A headless run cannot walk to a 32-unit trigger cube and wait for a
    // voice line; making the test do that tests the walking.
    {
        struct Hook { std::string sName, sMsg; int nFrame; bool bDone; };
        static std::vector<Hook> s_Hooks;
        static bool s_bParsed = false;
        static unsigned s_nFrame = 0;

        if (!s_bParsed)
        {
            s_bParsed = true;
            if (const char* pWant = getenv("LT_ACTIVATE"))
            {
                std::string all(pWant);
                size_t from = 0;
                while (from <= all.size())
                {
                    size_t end = all.find(';', from);
                    if (end == std::string::npos) end = all.size();
                    std::string one = all.substr(from, end - from);
                    from = end + 1;
                    while (!one.empty() && one[0] == ' ') one.erase(0, 1);
                    if (one.empty()) continue;

                    Hook h; h.nFrame = 0; h.bDone = false; h.sMsg = "ACTIVATE";
                    const size_t c1 = one.find(',');
                    h.sName = one.substr(0, c1);
                    if (c1 != std::string::npos)
                    {
                        const size_t c2 = one.find(',', c1 + 1);
                        h.nFrame = atoi(one.substr(c1 + 1, c2 - c1 - 1).c_str());
                        if (c2 != std::string::npos) h.sMsg = one.substr(c2 + 1);
                    }
                    s_Hooks.push_back(h);
                }
            }
        }

        for (size_t i = 0; i < s_Hooks.size(); ++i)
        {
            Hook& h = s_Hooks[i];
            if (h.bDone || (int)s_nFrame < h.nFrame) continue;

            ObjArray<HOBJECT, 8> a;
            uint32 nFound = 0;
            LTHost_FindNamedObjects((char*)h.sName.c_str(), a, &nFound);
            if (a.NumObjects() == 0) continue;

            SRec* pPlayer = LTHost_FindPlayerRecord();
            if (!pPlayer) continue;

            // ⚠️ The string must outlive the SEND: the message carries a
            // pointer and the queue delivers it later in the frame.
            static std::vector<std::string*> s_Keep;
            std::string* pMsg = new std::string(h.sMsg);
            s_Keep.push_back(pMsg);

            const unsigned int kGameTriggerMsg = 1000;   // ObjectDLL/ObjectMsgs.h:24
            CLTMessage* pM = LTMessage_StartToObject(pPlayer->m_pGame,
                                                     a.GetObject(0), kGameTriggerMsg);
            if (pM)
            {
                pM->PutDWord((uintptr_t)pMsg->c_str());
                LTMessage_End(pM, 0);
                printf("[ACTIVATE] '%s' <- \"%s\" at frame %u\n",
                       h.sName.c_str(), h.sMsg.c_str(), s_nFrame);
                fflush(stdout);
                h.bDone = true;
            }
        }
        ++s_nFrame;
    }

    // ⚠️ LT_RAYTEST="x,y,z,dx,dy,dz,len[,frame]" -- WHAT WOULD ACTIVATE HIT?
    //
    // The same query CPlayerObj::Activate makes, reported. "E does nothing"
    // has two very different causes -- the ray hit the world instead of the
    // door, or it hit the door and the door refused -- and they look identical
    // from a chair.
    {
        static const char* pWant = getenv("LT_RAYTEST");
        static bool bDone = false;
        static unsigned nFrame = 0;
        if (pWant && !bDone)
        {
            float v[8] = { 0,0,0, 0,0,0, 100.0f, 0.0f };
            const int n = sscanf(pWant, "%f,%f,%f,%f,%f,%f,%f,%f",
                                 &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7]);
            if (n >= 7 && (float)nFrame >= v[7])
            {
                float len = sqrtf(v[3]*v[3] + v[4]*v[4] + v[5]*v[5]);
                if (len < 1e-6f) len = 1.0f;
                const float to[3] = { v[0] + v[3]/len*v[6],
                                      v[1] + v[4]/len*v[6],
                                      v[2] + v[5]/len*v[6] };
                float pt[3], nm[3];
                void* hHit = 0;
                // Same flags CPlayerObj::Activate uses: objects, ignoring non-solid.
                const bool bHit = LTServerPhys_RayTest(v, to, 0, 0, pt, nm, &hHit, true, true);
                if (!bHit) printf("[RAY] nothing hit\n");
                else if (!hHit) printf("[RAY] hit THE WORLD at (%.0f %.0f %.0f)\n", pt[0],pt[1],pt[2]);
                else
                {
                    SRec* p = CLTServerObjects::FromHandle(hHit);
                    printf("[RAY] hit %s '%s' at (%.0f %.0f %.0f)\n",
                           p && p->m_pClass && p->m_pClass->m_ClassName ? p->m_pClass->m_ClassName : "?",
                           p ? p->m_sName.c_str() : "?", pt[0],pt[1],pt[2]);
                }
                fflush(stdout);
                bDone = true;
            }
            ++nFrame;
        }
    }

    // Model keys before MID_UPDATE, as the donor's FullObjectUpdate orders
    // them: a FIRE key crossed this frame is acted on in this frame's update.
    LTHost_ServerModelKeys();

    g_nLastUpdated = g_Objects.UpdateObjects(g_fServerTime, g_fServerFrameTime);

    // Socket-less attachments follow their parents first, so the brushes
    // below are placed where the attached panes and gates now are.
    LTHost_UpdateFreeAttachments();

    // ⚠️ AFTER the objects have updated, so a door that moved this frame is
    // collided against and drawn where it is NOW, not a frame behind.
    LTServerBrush_Update();

    g_pShell->Update(fElapsed);

    // ⚠️ AFTER THE OBJECTS HAVE RUN, BEFORE THE MESSAGES ARE PUMPED.
    //
    // After, because an object that sets its velocity in its own MID_UPDATE
    // must move on THIS frame -- integrating first would make every object a
    // frame late, which is invisible standing still and very visible on
    // anything thrown.
    //
    // Before the pump, because a move can produce a message (a touch, a
    // trigger) and the pump is what delivers it; integrating after would hold
    // every such message for a frame.
    LTServerPhys_Update(fElapsed);

    // ⚠️ TOUCH NOTIFICATION. Without this NOTHING IN A LEVEL IS INTERACTIVE.
    //
    // A NOLF trigger does not look for the player -- the ENGINE tells it.
    // `Trigger::Update` says so in its own comment: "my object touch
    // notification will trigger new updates until then, I don't care."
    // MID_TOUCHNOTIFY is handled by nine game classes (Trigger, PickupItem,
    // Door, AI, Body, Prop, Mine, Projectile, Destructible), so with no
    // notification there is no pickup, no scripted conversation, no cinematic
    // hand-off and no auto-opening door, in any level.
    //
    // ⚠️ BEFORE THE PUMP, because a touch produces messages and the pump is
    // what delivers them; notifying afterwards holds every one for a frame.
    // ⚠️ LT_TOUCHTEST="<object name>" -- PUT THE ITEM ON THE PLAYER, rather
    // than the player on the item, and this inversion is the point.
    //
    // Moving the PLAYER cannot be made to stick from outside the game: the
    // client mover owns the position and re-asserts its own copy every frame,
    // so an engine-side teleport is undone before the touch pass runs. The
    // item has no such owner. One object moved, one frame, and the question
    // "does touching this pick it up" is answered without needing to walk.
    {
        static const char* pWhich = getenv("LT_TOUCHTEST");
        if (pWhich && *pWhich)
        {
            SRec* pPlayer = LTHost_FindPlayerRecord();
            if (pPlayer)
            {
                for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
                {
                    SRec* r = g_Objects.RecordAt(i);
                    if (!r || !g_Objects.IsLive(r)) continue;
                    if (strcasecmp(r->m_sName.c_str(), pWhich) != 0) continue;

                    static bool bSaid = false;
                    if (!bSaid)
                    {
                        bSaid = true;
                        printf("[TOUCHTEST] moving %s '%s' onto the player at "
                               "(%.0f %.0f %.0f)\n",
                               r->m_pClass && r->m_pClass->m_ClassName
                                   ? r->m_pClass->m_ClassName : "?",
                               r->m_sName.c_str(),
                               pPlayer->m_Pos[0], pPlayer->m_Pos[1], pPlayer->m_Pos[2]);
                        fflush(stdout);
                    }
                    for (int k = 0; k < 3; ++k) r->m_Pos[k] = pPlayer->m_Pos[k];
                    break;
                }
            }
        }
    }

    LTServerHost_TouchPass();

    g_nMsgIn += LTMessage_PumpServer();

    // ⚠️ WHAT THE PLAYER CAN SEE STAYS AWAKE; what ran out of time unseen
    // falls asleep (HANDOVER 5cp). Where the shipped engine does it: after
    // the objects, while building the client's update. MID_(DE)ACTIVATING
    // go straight to the objects, so this sits before the sweep.
    g_Objects.ActivationPass(g_bClientViewPosSet ? g_ClientViewPos : 0,
                             (g_ClientInfoFlags & CIF_AUTOACTIVATEOBJECTS) != 0,
                             g_fServerFrameTime);

    // ⚠️ LAST, AND AFTER THE PUMP. A destructor can send messages, and a
    // message can name an object; sweeping before the pump would deliver to
    // something already freed. Sweeping here means a removed object survives
    // to the end of the frame it was removed in, which is what the game
    // assumes -- it removes an object and keeps using its own pointer for the
    // rest of the call.
    g_Objects.SweepDead();
    LTHost_ForgetFreedAttachments();

    // LT_AIWALLTRACE=1 -- [AIWALL] each time an AI's centre goes INTO the level's
    // solid (outside every VisBSP leaf, or overlapping collision geometry
    // within 8 units) and when it comes back out, with where it came from.
    // NOLF's humans move by teleport along their path ("cheap movement",
    // FLAG_GOTHRUWORLD), so walls are only as good as the path (5cz).
    if (getenv("LT_AIWALLTRACE"))
    {
        static std::map<const SRec*, int> s_State;
        static std::map<const SRec*, std::vector<float> > s_Last;
        const unsigned int nObj = g_Objects.ObjectCount();
        for (unsigned int i = 0; i < nObj; ++i)
        {
            const SRec* p = g_Objects.RecordAt(i);
            if (!p || p->m_bDead || !p->m_pClass || !p->m_pClass->m_ClassName) continue;
            if (strncmp(p->m_pClass->m_ClassName, "AI_", 3) != 0) continue;
            const int nLeaf = LTGetWorldVis()->IsValid() ? LTGetWorldVis()->LeafAt(p->m_Pos) : 0;
            const bool bOverlap = LTWorldCollide_Ready() && LTGetWorldCollide()->Overlaps(p->m_Pos, 8.0f);
            const int nNow = (nLeaf < 0 ? 1 : 0) | (bOverlap ? 2 : 0);
            std::map<const SRec*, int>::iterator it = s_State.find(p);
            const int nWas = (it == s_State.end()) ? 0 : it->second;
            if (nNow != nWas)
            {
                const std::vector<float>& L = s_Last[p];
                printf("[AIWALL] t=%.2f %-14s %s at (%.0f %.0f %.0f)%s%s; was at (%.0f %.0f %.0f)\n",
                       g_fServerTime, p->m_sName.c_str(), nNow ? "INTO SOLID" : "back out",
                       p->m_Pos[0], p->m_Pos[1], p->m_Pos[2],
                       (nNow & 1) ? " outside every leaf" : "", (nNow & 2) ? " overlapping geometry" : "",
                       L.size() == 3 ? L[0] : 0.0f, L.size() == 3 ? L[1] : 0.0f, L.size() == 3 ? L[2] : 0.0f);
                fflush(stdout);
                s_State[p] = nNow;
            }
            s_Last[p] = std::vector<float>(p->m_Pos, p->m_Pos + 3);
        }
    }

    g_nLastStep = LTStub_Count() - n0;
}

bool LTServerHost_TakeWorldStarted()
{
    if (!g_bWorldStartedLatch) return false;
    g_bWorldStartedLatch = false;
    return true;
}

// ⚠️ MOVE THE SERVER'S PLAYER TOO. LT_CLIPOS moves the CLIENT's object, which
// is the authoritative one while the client is driving -- but on a path where
// the client is NOT driving (a cinematic camera holds player movement off, and
// `play.sh game` never leaves one) the server's copy simply stays where it was
// and a test that teleports only the client measures nothing.
//
// A test hook that works on one code path and silently does nothing on another
// is worse than no hook: it reports "no pickup" identically whether the pickup
// is broken or the player never arrived.
bool LTServerHost_PlacePlayer(const float* pPos3)
{
    if (!pPos3) return false;
    SRec* p = LTHost_FindPlayerRecord();
    if (!p) return false;
    for (int k = 0; k < 3; ++k) { p->m_Pos[k] = pPos3[k]; p->m_Vel[k] = 0.0f; }
    return true;
}

void LTServerHost_SetWorldClosingHook(LTServerWorldClosingFn fn) { g_pWorldClosing = fn; }

// The k-th SECONDARY tracker of a server object, in AddTracker order -- the
// index a client-side copy of the object's trackers uses (LithTech replicates
// an object's trackers to its client object by index). False when there is no
// such tracker.
bool LTServerHost_GetTrackerAnim(void* hServer, int k, int* pAnim)
{
    if (pAnim) *pAnim = 0;
    SRec* p = CLTServerObjects::FromHandle(hServer);
    if (!p || k < 0 || !g_Objects.Contains(p)) return false;
    std::vector<std::pair<unsigned, const LTTrackerState*> > v;
    for (std::map<LTAnimTracker*, LTTrackerState>::const_iterator it = g_Trackers.begin();
         it != g_Trackers.end(); ++it)
        if (!it->second.m_bMain && it->second.m_pRec == p)
            v.push_back(std::make_pair(it->second.m_nSeq, &it->second));
    std::sort(v.begin(), v.end());
    static const bool s_bTrace = (getenv("LT_TRACKTRACE") != 0);
    if (s_bTrace)
    {
        static float s_fLast = -1.0f;
        if (g_fServerTime - s_fLast >= 0.5f)
        {
            s_fLast = g_fServerTime;
            printf("[TRACK] t=%.2f %s main anim %d, %zu secondary:", g_fServerTime, p->m_sName.c_str(), p->m_nAnim, v.size());
            for (size_t i = 0; i < v.size(); ++i)
                printf(" [%zu seq %u anim %d%s]", i, v[i].first, v[i].second->m_nAnim, v[i].second->m_bPlaying ? "" : " stopped");
            printf("  (asked %d)\n", k);
            fflush(stdout);
        }
    }
    if ((size_t)k >= v.size()) return false;
    if (pAnim) *pAnim = v[(size_t)k].second->m_nAnim;
    return true;
}

void* LTServerHost_PlayerHandle()
{
    return g_Objects.IsLive(CLTServerObjects::FromHandle(g_hPlayer)) ? (void*)g_hPlayer : 0;
}

bool LTServerHost_GetPlayerTransform(float* pPos3, float* pQuat4)
{
    SRec* p = CLTServerObjects::FromHandle(g_hPlayer);
    if (!p) return false;

    if (pPos3)  { for (int i = 0; i < 3; ++i) pPos3[i]  = p->m_Pos[i]; }
    if (pQuat4) { for (int i = 0; i < 4; ++i) pQuat4[i] = p->m_Rot[i]; }
    return true;
}

unsigned int LTServerHost_ObjectCount()
{
    return g_Objects.ObjectCount();
}

void LTServerHost_ProbeCharacters(bool bReset, int* pAlive, int* pMoved)
{
    static std::map<const SRec*, std::vector<float> > s_Last;
    static std::map<const SRec*, int> s_Ever;      // moved at least once
    if (bReset) { s_Last.clear(); s_Ever.clear(); }
    int nAlive = 0, nMoved = 0;
    std::map<const SRec*, std::vector<float> > now;
    const unsigned int n = g_Objects.ObjectCount();
    for (unsigned int i = 0; i < n; ++i)
    {
        SRec* p = g_Objects.RecordAt(i);
        if (!p || p->m_bDead || !p->m_pClass || !ClassIsA(p->m_pClass, "CCharacter")) continue;
        if (ClassIsA(p->m_pClass, "CPlayerObj")) continue;
        ++nAlive;
        std::vector<float> v(p->m_Pos, p->m_Pos + 3);
        std::map<const SRec*, std::vector<float> >::const_iterator it = s_Last.find(p);
        if (it != s_Last.end())
        {
            const float dx = v[0]-it->second[0], dy = v[1]-it->second[1], dz = v[2]-it->second[2];
            if (dx*dx + dy*dy + dz*dz > 64.0f) s_Ever[p] = 1;
        }
        now[p] = v;
    }
    s_Last.swap(now);
    for (std::map<const SRec*, int>::const_iterator e = s_Ever.begin(); e != s_Ever.end(); ++e)
        if (s_Last.count(e->first)) ++nMoved;
    if (pAlive) *pAlive = nAlive;
    if (pMoved) *pMoved = nMoved;
}

// LT_AIMAT's target: the nearest live CHARACTER whose class or object name
// contains pSub (case-insensitive). Writes its position and name.
bool LTServerHost_FindCharacter(const char* pSub, const float* pFrom3,
                                float* pPos3, std::string* pName)
{
    if (!pSub || !*pSub) return false;
    // LT_AIMLIST=<frame-interval>: every character, distance, visible or not.
    static int s_nCall = 0;
    static const int s_nEvery = getenv("LT_AIMLIST") ? atoi(getenv("LT_AIMLIST")) : 0;
    if (s_nEvery > 0 && (s_nCall++ % s_nEvery) == 0)
    {
        printf("[AIMLIST] t=%.1f eye (%.0f %.0f %.0f)\n", g_fServerTime, pFrom3[0], pFrom3[1], pFrom3[2]);
        const unsigned int n = g_Objects.ObjectCount();
        for (unsigned int i = 0; i < n; ++i)
        {
            SRec* p = g_Objects.RecordAt(i);
            if (!p || p->m_bDead || !p->m_pClass || !ClassIsA(p->m_pClass, "CCharacter")) continue;
            const float dx = p->m_Pos[0]-pFrom3[0], dy = p->m_Pos[1]-pFrom3[1], dz = p->m_Pos[2]-pFrom3[2];
            const float dd = sqrtf(dx*dx + dy*dy + dz*dz);
            float pt[3], nm[3]; void* hHit = 0; float fHit = dd;
            if (LTServerPhys_RayTest(pFrom3, p->m_Pos, 0, 0, pt, nm, &hHit, false, true))
            { const float hx = pt[0]-pFrom3[0], hy = pt[1]-pFrom3[1], hz = pt[2]-pFrom3[2]; fHit = sqrtf(hx*hx+hy*hy+hz*hz); }
            printf("[AIMLIST]   %-28s %-14s at %5.0f units (%6.0f %5.0f %6.0f)  %s\n", p->m_pClass->m_ClassName,
                   p->m_sName.c_str(), dd, p->m_Pos[0], p->m_Pos[1], p->m_Pos[2],
                   fHit >= dd - 40.0f ? "VISIBLE" : "hidden");
        }
        fflush(stdout);
    }
    std::string sub(pSub);
    for (size_t i = 0; i < sub.size(); ++i) sub[i] = (char)toupper((unsigned char)sub[i]);
    float fBest = 3.4e38f; bool bFound = false;
    const unsigned int n = g_Objects.ObjectCount();
    for (unsigned int i = 0; i < n; ++i)
    {
        SRec* p = g_Objects.RecordAt(i);
        if (!p || p->m_bDead || !p->m_pClass || !ClassIsA(p->m_pClass, "CCharacter")) continue;
        if (ClassIsA(p->m_pClass, "CPlayerObj")) continue;
        std::string a = std::string(p->m_pClass->m_ClassName) + "|" + p->m_sName;
        for (size_t k = 0; k < a.size(); ++k) a[k] = (char)toupper((unsigned char)a[k]);
        if (a.find(sub) == std::string::npos) continue;
        const float dx = p->m_Pos[0]-pFrom3[0], dy = p->m_Pos[1]-pFrom3[1], dz = p->m_Pos[2]-pFrom3[2];
        const float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 >= fBest) continue;
        // Only one the player can SEE: a WORLD-only ray from the eye must
        // reach within 40 units of him. Objects are left out -- an invisible
        // blocker in a window (M01S01 `blocker1`) is not cover.
        // LT_AIMANY=1 skips it: a ray to a character's CENTRE clips a roof
        // parapet that does not hide his head.
        static const bool s_bAny = (getenv("LT_AIMANY") != 0);
        // ⚠️ THE HEAD FIRST, THEN THE CENTRE -- what a player aims at is what
        // is exposed. A rooftop assassin behind a parapet hides his centre and
        // shows his head, and a centre-only test skipped him altogether: the
        // harness killed one assassin and then aimed at nobody while the next
        // one shot the ambassador. Head = centre + 0.75 of the half-height.
        float aim[3] = { p->m_Pos[0], p->m_Pos[1], p->m_Pos[2] };
        if (!s_bAny)
        {
            // A BULLET'S line, not an eye's: objects included, and through
            // what the game shoots through (Invisible.dtx, surface 202), as
            // CProjectile continues past it. The target counts as reached when
            // the first real hit is him or a hit box within 64 units of him.
            bool bReach = false;
            for (int pass = 0; pass < 2 && !bReach; ++pass)
            {
                const float tgt[3] = { p->m_Pos[0],
                                       p->m_Pos[1] + (pass == 0 ? p->m_Dims[1] * 0.75f : 0.0f),
                                       p->m_Pos[2] };
                float from[3] = { pFrom3[0], pFrom3[1], pFrom3[2] };
                bool bDone = false;
                for (int step = 0; step < 6 && !bDone; ++step)
                {
                    float pt[3], nm[3]; void* hHit = 0; unsigned int nPoly = 0;
                    if (!LTServerPhys_RayTest(from, tgt, 0, 0, pt, nm, &hHit, true, true, &nPoly))
                    { bReach = true; break; }
                    const SRec* h = (const SRec*)hHit;
                    if (h == p) { bReach = true; break; }
                    if (h && h->m_pClass && ClassIsA(h->m_pClass, "CCharacterHitBox"))
                    {
                        const float ex = h->m_Pos[0]-p->m_Pos[0], ey = h->m_Pos[1]-p->m_Pos[1], ez = h->m_Pos[2]-p->m_Pos[2];
                        bReach = (ex*ex + ey*ey + ez*ez) < 64.0f*64.0f;
                        break;
                    }
                    uint32 nST = 0;
                    if (nPoly) LTHost_PolyTextureFlags(nPoly, &nST);
                    if (nST != 202) { bDone = true; break; }        // a wall, a wire...
                    float dd[3] = { tgt[0]-from[0], tgt[1]-from[1], tgt[2]-from[2] };
                    const float l = sqrtf(dd[0]*dd[0] + dd[1]*dd[1] + dd[2]*dd[2]);
                    for (int k = 0; k < 3; ++k) from[k] = pt[k] + dd[k] / (l > 0 ? l : 1) * 2.0f;
                }
                if (bReach) for (int k = 0; k < 3; ++k) aim[k] = tgt[k];
            }
            if (!bReach) continue;
        }
        fBest = d2; bFound = true;
        for (int k = 0; k < 3; ++k) pPos3[k] = aim[k];
        if (pName) *pName = std::string(p->m_pClass->m_ClassName) + " " + p->m_sName;
    }
    return bFound;
}

void LTServerHost_LiveObjects(std::vector<void*>& out)
{
    // The records in the list are ours by construction, so liveness is the
    // dead flag alone -- not IsLive's scan, which made a per-frame walk
    // quadratic.
    out.clear();
    const unsigned int n = g_Objects.ObjectCount();
    for (unsigned int i = 0; i < n; ++i)
    {
        SRec* p = g_Objects.RecordAt(i);
        if (p && !p->m_bDead) out.push_back((void*)p);
    }
}

const char* LTServerHost_ClassNameOf(void* hServer)
{
    SRec* p = CLTServerObjects::FromHandle(hServer);
    return (p && p->m_pClass && p->m_pClass->m_ClassName) ? p->m_pClass->m_ClassName : "?";
}

const char* LTServerHost_ObjectNameOf(void* hServer)
{
    SRec* p = CLTServerObjects::FromHandle(hServer);
    return p ? p->m_sName.c_str() : "";
}

// The object keeps its special-effect message (ltserverobjects.h m_SFXMsg):
// installed as the message module's capture hook. pObject is the BaseClass*.
void LTServerHost_CaptureSFX(void* pObject, const unsigned char* pBytes, unsigned int nBytes)
{
    SRec* p = g_Objects.RecordOf((BaseClass*)pObject);
    if (!p) return;
    p->m_SFXMsg.assign(pBytes, pBytes + nBytes);
}

// The box a RAY tests this object by (ltserverphysics.cpp): a brush object's
// own box where it is now, anything else pos +- dims.
bool LTServerHost_RayBoxOf(void* hServer, float* pMin3, float* pMax3)
{
    SRec* p = CLTServerObjects::FromHandle(hServer);
    if (!p || !pMin3 || !pMax3) return false;
    for (int k = 0; k < 3; ++k)
    {
        if (p->m_bHasBox)
        {
            const float d0 = p->m_Pos[k] - p->m_BoxRestPos[k];
            pMin3[k] = p->m_BoxMin[k] + d0; pMax3[k] = p->m_BoxMax[k] + d0;
        }
        else { pMin3[k] = p->m_Pos[k] - p->m_Dims[k]; pMax3[k] = p->m_Pos[k] + p->m_Dims[k]; }
    }
    return true;
}

void* LTServerHost_ObjectAt(unsigned int i)
{
    SRec* p = g_Objects.RecordAt(i);
    // ⚠️ A DEAD RECORD IS NOT AN OBJECT. RemoveObject only marks; SweepDead
    // frees later (see Record::m_bDead), so a walk that skipped this check
    // would integrate the physics of things the game has already destroyed.
    if (!p || !g_Objects.IsLive(p)) return 0;
    return (void*)p;
}

bool LTServerHost_GetLiveCameraTransform(float* pPos3, float* pQuat4)
{
    // USRFLG_CAMERA_LIVE, ClientServerShared.h:137. Spelled out rather than
    // included: this file must not depend on the game's headers.
    const unsigned int kCameraLive = (1u << 5);

    const unsigned int n = g_Objects.ObjectCount();
    for (unsigned int i = 0; i < n; ++i)
    {
        SRec* p = g_Objects.RecordAt(i);
        if (!p || !g_Objects.IsLive(p)) continue;
        if (!(p->m_UserFlags & kCameraLive)) continue;
        if (!p->m_pClass || !p->m_pClass->m_ClassName) continue;
        if (strcmp(p->m_pClass->m_ClassName, "Camera") != 0) continue;

        // ⚠️ NAMED, once per camera. "A camera is live" is not actionable;
        // "camera1_CAM4 is live and camera2 never went off" is.
        if (getenv("LT_CAMTRACE"))
        {
            static std::string sLast;
            if (sLast != p->m_sName)
            {
                sLast = p->m_sName;
                printf("[CAM] live camera is now '%s' (flags 0x%08x) at t=%.3f\n",
                       p->m_sName.empty() ? "(unnamed)" : p->m_sName.c_str(),
                       p->m_UserFlags, LTServerHost_Time());
                fflush(stdout);
            }
        }

        if (pPos3)  { for (int k = 0; k < 3; ++k) pPos3[k]  = p->m_Pos[k]; }
        if (pQuat4) { for (int k = 0; k < 4; ++k) pQuat4[k] = p->m_Rot[k]; }
        return true;
    }
    return false;
}

void LTServerHost_SetSoundPlayer(LTServerSoundFn fn) { g_pSoundFn = fn; }

void LTServerHost_SetSoundQueries(LTServerSoundDoneFn fnDone,
                                  LTServerSoundDurFn fnDur,
                                  LTServerSoundKillFn fnKill,
                                  LTServerSoundKillFn fnKillLoop)
{
    g_pSoundDoneFn     = fnDone;
    g_pSoundDurFn      = fnDur;
    g_pSoundKillFn     = fnKill;
    g_pSoundKillLoopFn = fnKillLoop;
}


void LTServerHost_DumpTouchCensus()
{
    printf("\n[TOUCH] %u overlapping pair(s), %u notification(s) sent\n",
           g_nTouchPairs, g_nTouchSent);
    printf("[SKEY] %u server model key(s) delivered (MID_MODELSTRINGKEY)\n", g_nServerKeys);
    printf("[DOOR] %u door(s) with a zero MoveDir given a unit direction and no distance\n", g_nZeroMoveDirDoors);
    if (!g_nTouchPairs)
        printf("        ⚠️  NONE -- nothing in this level can be triggered,\n"
               "            picked up, or walked into. See LTServerHost_TouchPass.\n");
    fflush(stdout);
}

void LTServerHost_DumpClassCensus() {
    // ⚠️ WHAT THE PLAYER CAN USE, asked at the END of the run rather than at
    // load: the game raises USRFLG_CAN_ACTIVATE from its own update, so a list
    // taken while the level is still being built says almost nothing.
    if (getenv("LT_WMTRACE"))
    {
        unsigned int n = 0;
        printf("\n[USE] objects flagged CAN_ACTIVATE:\n");
        for (unsigned int i = 0; i < g_Objects.ObjectCount(); ++i)
        {
            SRec* r = g_Objects.RecordAt(i);
            if (!r || !g_Objects.IsLive(r) || !(r->m_UserFlags & (1u << 21))) continue;
            printf("      %-22s %-26s (%.0f %.0f %.0f)\n",
                   r->m_pClass && r->m_pClass->m_ClassName ? r->m_pClass->m_ClassName : "?",
                   r->m_sName.c_str(), r->m_Pos[0], r->m_Pos[1], r->m_Pos[2]);
            ++n;
        }
        if (!n) printf("      ⚠️  NONE -- nothing in this level can be used\n");
        fflush(stdout);
    }
 g_Objects.DumpClassCensus(); }


bool LTServerHost_GetClientViewPos(float* pPos3)
{
    if (!g_bClientViewPosSet) return false;
    if (pPos3) { for (int i = 0; i < 3; ++i) pPos3[i] = g_ClientViewPos[i]; }
    return true;
}


// ⚠️ THE ENGINE'S `AutoDeactivate` VARIABLE, read where the game writes it.
// lithtech.exe keeps it as an engine console variable (default 0) and NOLF
// turns it on with RunGameConString("autodeactivate 1.0") as the server shell
// is made; this port's RunGameConString runs into the game console, so that
// is where it is looked up. LT_NOAUTODEACTIVATE=1 -- nothing ever falls
// asleep, every AI hears every shot (old).
bool CLTServerObjects::AutoDeactivateOn()
{
    static const bool bOff = []() {
        const bool b = (getenv("LT_NOAUTODEACTIVATE") != 0);
        if (b) { printf("[ACT] LT_NOAUTODEACTIVATE=1 -- objects never fall asleep; every AI hears every shot (old)\n"); fflush(stdout); }
        return b; }();
    if (bOff) return false;
    const LTConsoleVar* pVar = g_GameConsole.Find("AutoDeactivate");
    return pVar && g_GameConsole.GetFloat(pVar) != 0.0f;
}


void* LTServerHost_HandleOf(void* pGameObject)
{
    return pGameObject ? (void*)((BaseClass*)pGameObject)->m_hObject : 0;
}


bool LTServerHost_GetObjectState(void* hObj,
                                 unsigned int* pType,
                                 float* pPos3, float* pQuat4,
                                 unsigned int* pFlags,
                                 unsigned int* pUserFlags,
                                 const char** ppFilename)
{
    SRec* p = CLTServerObjects::FromHandle(hObj);

    // ⚠️ VALIDATED AGAINST THE LIST, not merely null-checked. The whole point
    // of this call is that the caller is holding a handle from an earlier
    // frame; `p != 0` only says the pointer is not null, which a freed record
    // also satisfies.
    if (!p || !g_Objects.IsLive(p)) return false;

    if (pType)      *pType      = (unsigned int)p->m_Type;
    if (pFlags)     *pFlags     = p->m_Flags;
    if (pUserFlags) *pUserFlags = p->m_UserFlags;
    if (pPos3)  { for (int i = 0; i < 3; ++i) pPos3[i]  = p->m_Pos[i]; }
    if (pQuat4) { for (int i = 0; i < 4; ++i) pQuat4[i] = p->m_Rot[i]; }

    // ⚠️ Points INTO the record, which outlives the call and is re-read every
    // sync. Copying it would be safer and would also mean allocating a string
    // per object per frame for a value that changes once, at Apply.
    if (ppFilename) *ppFilename = p->m_sFilename.c_str();
    return true;
}


unsigned int LTServerHost_ObjectsUpdated()  { return g_nLastUpdated; }
unsigned int LTServerHost_MessagesHandled() { return g_nMsgIn; }
float        LTServerHost_Time()           { return g_fServerTime; }


void LTServerHost_Stop()
{
    // ⚠️ THE QUEUES GO FIRST. A message still pending points at an object the
    // teardown below is about to destroy, and a pump after that is a
    // use-after-free that looks like a crash in the game.
    LTMessage_SetServerDeliver(0);
    LTMessage_Reset();

    if (g_pShell && g_pDelete) g_pDelete(g_pShell);
    g_pShell = 0;

    g_Objects.Term();

    delete g_pServer;
    g_pServer = 0;

    // ⚠️ The library is deliberately NOT dlclose()d after the shell is gone.
    // Static destructors in ObjectDLL run at unload and the engine they call
    // back into is this executable, which is still alive -- but the ORDER is
    // not something either side controls, and a crash during teardown would
    // look exactly like a crash in the game. Leaving it mapped costs a few MB
    // of address space in a process that is exiting.
    g_pDelete = 0;
}


void LTServerHost_SetCommandQuery(LTServerCommandFn fn)
{
    g_pCommandFn = fn;
}

// See LTWorldScene_SetLiveQuery: what the game has done to world-file object i.
int LTServerHost_WorldObjectLive(unsigned int nWorldIndex)
{
    return g_Objects.WorldState(nWorldIndex);
}

// See LTWorldScene_SetLivePoseQuery: where the game has world-file object i.
bool LTServerHost_WorldObjectPose(unsigned int nWorldIndex, float* pPos3, float* pQuat4)
{
    return g_Objects.WorldPose(nWorldIndex, pPos3, pQuat4);
}

// See LTWorldScene_SetLiveAnimQuery: what world-file object i is playing (5cg),
// on the same clock as the game-spawned models (LTHost_AnimElapsed).
bool LTServerHost_WorldObjectAnim(unsigned int nWorldIndex, int* pnAnim, float* pfSeconds, bool* pbLoop)
{
    float fStart = 0.0f;
    if (!g_Objects.WorldAnim(nWorldIndex, pnAnim, &fStart, pbLoop)) return false;
    *pfSeconds = LTHost_AnimElapsed(fStart);
    return true;
}

unsigned int LTServerHost_PointContainers(const float* pPoint3, void** pList, unsigned int nMax)
{
    if (!pPoint3 || !pList || !nMax) return 0;
    return LTHost_PointContainersRaw(pPoint3, pList, nMax);
}

bool LTServerHost_ContainerCode(void* hServer, unsigned short* pCode)
{
    uint16 c = 0;
    const bool b = LTHost_ServerContainerCode((HOBJECT)hServer, &c) != LTFALSE;
    if (pCode) *pCode = c;
    return b;
}

const char* LTServerHost_GameConVarString(const char* pName)
{
    if (!pName) return 0;
    const LTConsoleVar* v = g_GameConsole.Find(pName);
    return v ? v->m_sValue.c_str() : 0;
}

bool LTServerHost_GameConVarFloat(const char* pName, float* pValue)
{
    if (pValue) *pValue = 0.0f;
    if (!pName) return false;
    const LTConsoleVar* v = g_GameConsole.Find(pName);
    if (!v) return false;
    if (pValue) *pValue = v->m_bHasFloat ? v->m_fValue : (float)atof(v->m_sValue.c_str());
    return true;
}
