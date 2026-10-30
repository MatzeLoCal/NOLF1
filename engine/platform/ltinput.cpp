// ----------------------------------------------------------------------- //
//
// MODULE  : ltinput.cpp
//
// PURPOSE : Event translation. See ltinput.h for the two-doors problem and the
//           ordering trap that hid it.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltinput.h"
#include "ltbindings.h"

#include "iclientshell.h"

#include <stdio.h>
#include <string.h>
#include <vector>
// Windows OEM virtual keys -- the punctuation row. Not in compat/windows.h
// (nothing in the game names them); the values are winuser.h's.
#ifndef VK_OEM_1
#define VK_OEM_1        0xBA    // ;:
#define VK_OEM_PLUS     0xBB    // =+
#define VK_OEM_COMMA    0xBC    // ,<
#define VK_OEM_MINUS    0xBD    // -_
#define VK_OEM_PERIOD   0xBE    // .>
#define VK_OEM_2        0xBF    // /?
#define VK_OEM_3        0xC0    // `~
#define VK_OEM_4        0xDB    // [{
#define VK_OEM_5        0xDC    // \|
#define VK_OEM_6        0xDD    // ]}
#define VK_OEM_7        0xDE    // '"
#define VK_OEM_102      0xE2    // the ISO key left of 1 / next to left shift
#endif

#include <stdlib.h>


// The window procedure the game installed, kept by the compat layer.
extern "C" intptr_t GetWindowLongA(HWND h, int index);

namespace
{
    // ⚠️ Fixed size. An input queue that grows without bound turns a stalled
    // frame loop into a memory leak, and the useful behaviour when it fills is
    // to DROP -- stale input is worse than none, and the drop count says so.
    const int kQueueSize = 256;

    LTInputEvent g_Queue[kQueueSize];
    int          g_nHead = 0;
    int          g_nTail = 0;

    unsigned g_nDelivered = 0;
    unsigned g_nDropped   = 0;
    unsigned g_nUnmapped  = 0;

    // Win32 message ids, from the compat header's own values.
    inline LPARAM PackXY(int x, int y)
    {
        // ⚠️ GET_X_LPARAM/GET_Y_LPARAM read SIGNED 16-bit halves, so negative
        // coordinates -- which happen when the cursor leaves the window during
        // a drag -- must be packed as 16-bit two's complement, not clamped.
        return (LPARAM)(((unsigned)(y & 0xFFFF) << 16) | (unsigned)(x & 0xFFFF));
    }
}


namespace
{
    // Same constant as the walk viewer's kMouseGain, deliberately.
    const float kAxisGain = 0.0032f;
    float g_AxisDX = 0.0f, g_AxisDY = 0.0f;
}

// Set by the host so the input layer can tell "the game hooked the window"
// from "nobody did". See the click trace below.
static void* g_pHostDefaultProc = 0;
void LTInput_SetHostDefaultProc(void* p) { g_pHostDefaultProc = p; }
bool LTInput_ProcIsHostDefault(void* p)  { return p && p == g_pHostDefaultProc; }

void LTInput_Push(const LTInputEvent& ev)
{
    int nNext = (g_nTail + 1) % kQueueSize;
    if (nNext == g_nHead) { ++g_nDropped; return; }

    g_Queue[g_nTail] = ev;
    g_nTail = nNext;
}


static LTInputObserver g_pObserver = 0;

void LTInput_SetObserver(LTInputObserver pFn)
{
    g_pObserver = pFn;
}


void LTInput_Pump(IClientShell* pShell)
{
    // A wheel notch held its command for exactly one frame; release it now.
    LTBind_ReleasePulses();

    // The game's hooked procedure, if HookWindow succeeded. ⚠️ Read every pump
    // rather than cached: the game re-hooks after a mode change
    // (GameClientShell.cpp:1389), and a cached pointer would go stale exactly
    // when the player changes resolution.
    WNDPROC pProc = (WNDPROC)GetWindowLongA((HWND)1, GWL_WNDPROC);

    while (g_nHead != g_nTail)
    {
        const LTInputEvent& ev = g_Queue[g_nHead];
        g_nHead = (g_nHead + 1) % kQueueSize;
        ++g_nDelivered;

        if (g_pObserver) g_pObserver(ev);

        // ⚠️ ACCUMULATED FOR EVERY MOUSE MOVE, whatever else consumes the
        // event. The game polls once a frame and several moves can arrive
        // between polls; keeping only the last would throw away most of a
        // fast flick.
        if (ev.m_Kind == LTInputEvent::kMouseMove)
        {
            g_AxisDX += (float)ev.m_DX;
            g_AxisDY += (float)ev.m_DY;
        }

        switch (ev.m_Kind)
        {
            case LTInputEvent::kKeyDown:
                // ⚠️ THE BINDING FIRST, AND IT IS NOT A FILTER -- the shell
                // still sees every key. The two paths are separate on purpose:
                // OnKeyDown drives menus and typed text and only reports
                // TRANSITIONS, while movement asks IsCommandOn about a key that
                // is HELD. Routing movement through the queue would make the
                // player step once per press.
                LTBind_OnKey(ev.m_Code, true);
                if (pShell) pShell->OnKeyDown(ev.m_Code, ev.m_Repeat);
                break;

            case LTInputEvent::kKeyUp:
                LTBind_OnKey(ev.m_Code, false);
                if (pShell) pShell->OnKeyUp(ev.m_Code);
                break;

            // ⚠️ Every held command released. See kFocusLost in the header.
            case LTInputEvent::kFocusLost:
                LTCommand_Clear();
                break;

            case LTInputEvent::kChar:
                if (pProc) pProc((HWND)1, WM_CHAR, (WPARAM)ev.m_Code, 1);
                break;

            case LTInputEvent::kMouseMove:
                if (pProc) pProc((HWND)1, WM_MOUSEMOVE, 0, PackXY(ev.m_X, ev.m_Y));
                break;

            case LTInputEvent::kMouseDown:
                // ⚠️ IS ANYONE LISTENING? The GAME receives all mouse input
                // through a window proc it installs itself (HookWindow,
                // GameClientShell.cpp:8364). If that never ran, the slot still
                // holds the HOST's no-op default and every click is delivered
                // to a function that returns 0 -- which looks exactly like a
                // window that ignores clicks.
                if (getenv("LT_CLICKTRACE"))
                {
                    static bool bSaid = false;
                    if (!bSaid)
                    {
                        bSaid = true;
                        printf("[CLICK] window proc is %p%s\n", (void*)pProc,
                               LTInput_ProcIsHostDefault((void*)pProc)
                                   ? "   ⚠️  STILL THE HOST'S DEFAULT -- the game never hooked the window"
                                   : "   (the game's own)");
                        fflush(stdout);
                    }
                }
                // ⚠️ BOTH PATHS, exactly like a key. The message proc drives
                // the MENUS (clicking a button) and the binding drives the
                // GAME (firing). Sending a mouse button to only the proc is
                // what left the player unable to shoot -- see
                // LTBind_OnMouseButton.
                LTBind_OnMouseButton(ev.m_Button, true);
                if (pProc)
                    pProc((HWND)1,
                          ev.m_Button == 2 ? WM_MBUTTONDOWN :
                          ev.m_Button ? WM_RBUTTONDOWN : WM_LBUTTONDOWN,
                          0, PackXY(ev.m_X, ev.m_Y));
                break;

            case LTInputEvent::kMouseUp:
                LTBind_OnMouseButton(ev.m_Button, false);
                if (pProc)
                    pProc((HWND)1,
                          ev.m_Button == 2 ? WM_MBUTTONUP :
                          ev.m_Button ? WM_RBUTTONUP : WM_LBUTTONUP,
                          0, PackXY(ev.m_X, ev.m_Y));
                break;

            case LTInputEvent::kWheel:
                LTBind_OnWheel(ev.m_DY);
                break;

            case LTInputEvent::kMouseDblClick:
                if (pProc)
                    pProc((HWND)1,
                          ev.m_Button ? WM_RBUTTONDBLCLK : WM_LBUTTONDBLCLK,
                          0, PackXY(ev.m_X, ev.m_Y));
                break;

            default:
                break;
        }
    }
}


// ----------------------------------------------------------------------- //
// macOS virtual keycode -> WINDOWS VIRTUAL KEY code.
//
// ⚠️ VK, NOT DIK -- AND THE FIRST VERSION OF THIS FILE HAD IT WRONG.
//
// The reasoning that produced DIK was: autoexec.cfg binds "##17" to Forward and
// DIK_W is 0x11 = 17, therefore key codes are DIK. That is true of the BINDING
// system -- `rangebind "##keyboard" "##17" ... "Forward"` -- which is reached
// through GetDeviceBindings and device polling, and is NOT implemented here.
//
// `IClientShell::OnKeyDown` is a different door and takes VIRTUAL KEY codes.
// The game's own code proves it:
//
//     if (key == VK_TOGGLE_SCREENSHOTMODE)      GameClientShell.cpp:4335
//     if (key != VK_ESCAPE || !g_bLockFolder)   InterfaceMgr.cpp:2504
//
// The symptom of getting this wrong is exactly what was observed: the mouse
// drives the menus perfectly and the keyboard does nothing at all, with no
// error anywhere -- because DIK 208 (DIK_DOWN) is simply not a VK the
// interface manager recognises.
//
// ⚠️ Two systems, two encodings, one `int`. When a game has more than one input
// path, confirm which encoding each one speaks before mapping either.

int LTInput_MacKeyToVK(int k)
{
    // Letters: VK_A..VK_Z are 'A'..'Z'.
    switch (k)
    {
        case 0x00: return 'A';  case 0x0B: return 'B';  case 0x08: return 'C';
        case 0x02: return 'D';  case 0x0E: return 'E';  case 0x03: return 'F';
        case 0x05: return 'G';  case 0x04: return 'H';  case 0x22: return 'I';
        case 0x26: return 'J';  case 0x28: return 'K';  case 0x25: return 'L';
        case 0x2E: return 'M';  case 0x2D: return 'N';  case 0x1F: return 'O';
        case 0x23: return 'P';  case 0x0C: return 'Q';  case 0x0F: return 'R';
        case 0x01: return 'S';  case 0x11: return 'T';  case 0x20: return 'U';
        case 0x09: return 'V';  case 0x0D: return 'W';  case 0x07: return 'X';
        case 0x10: return 'Y';  case 0x06: return 'Z';

        // Digits: VK_0..VK_9 are '0'..'9'.
        case 0x1D: return '0';  case 0x12: return '1';  case 0x13: return '2';
        case 0x14: return '3';  case 0x15: return '4';  case 0x17: return '5';
        case 0x16: return '6';  case 0x1A: return '7';  case 0x1C: return '8';
        case 0x19: return '9';

        // What a menu actually needs.
        case 0x24: return VK_RETURN;
        case 0x30: return VK_TAB;
        case 0x31: return VK_SPACE;
        case 0x33: return VK_BACK;
        case 0x35: return VK_ESCAPE;
        case 0x7E: return VK_UP;
        case 0x7D: return VK_DOWN;
        case 0x7B: return VK_LEFT;
        case 0x7C: return VK_RIGHT;
        case 0x74: return VK_PRIOR;
        case 0x79: return VK_NEXT;
        case 0x73: return VK_HOME;
        case 0x77: return VK_END;

        // Function keys are contiguous from VK_F1.
        case 0x7A: return VK_F1 + 0;   case 0x78: return VK_F1 + 1;
        case 0x63: return VK_F1 + 2;   case 0x76: return VK_F1 + 3;
        case 0x60: return VK_F1 + 4;   case 0x61: return VK_F1 + 5;
        case 0x62: return VK_F1 + 6;   case 0x64: return VK_F1 + 7;
        case 0x65: return VK_F1 + 8;   case 0x6D: return VK_F1 + 9;
        case 0x67: return VK_F11;      case 0x6F: return VK_F12;

        // ⚠️ THE PUNCTUATION ROW, which this table did not have -- and
        // NOLF binds it: PrevWeapon is "]" (DIK 27), MouseAim "\" (43),
        // IncLayout/DecLayout "-" and "=" (12, 13). A key with no entry here
        // never reaches the game at all (HANDOVER 5bi). Physical positions,
        // like the letters: DirectInput bindings are scan codes, so a Swiss or
        // German layout binds the same KEY a US one does.
        case 0x1B: return VK_OEM_MINUS;   case 0x18: return VK_OEM_PLUS;
        case 0x21: return VK_OEM_4;       case 0x1E: return VK_OEM_6;
        case 0x2A: return VK_OEM_5;       case 0x29: return VK_OEM_1;
        case 0x27: return VK_OEM_7;       case 0x2B: return VK_OEM_COMMA;
        case 0x2F: return VK_OEM_PERIOD;  case 0x2C: return VK_OEM_2;
        case 0x32: return VK_OEM_3;       case 0x0A: return VK_OEM_102;

        // Editing block and keypad.
        case 0x75: return VK_DELETE;      case 0x72: return VK_INSERT;
        case 0x52: return VK_NUMPAD0;     case 0x53: return VK_NUMPAD1;
        case 0x54: return VK_NUMPAD2;     case 0x55: return VK_NUMPAD3;
        case 0x56: return VK_NUMPAD4;     case 0x57: return VK_NUMPAD5;
        case 0x58: return VK_NUMPAD6;     case 0x59: return VK_NUMPAD7;
        case 0x5B: return VK_NUMPAD8;     case 0x5C: return VK_NUMPAD9;
        case 0x41: return VK_DECIMAL;     case 0x43: return VK_MULTIPLY;
        case 0x45: return VK_ADD;         case 0x4E: return VK_SUBTRACT;
        case 0x4B: return VK_DIVIDE;      case 0x4C: return VK_RETURN;

        default:
            ++g_nUnmapped;
            return 0;
    }
}


// ⚠️ THE MODIFIER KEYS, which macOS never sends as keyDown/keyUp -- only as
// flagsChanged. Without this NOLF's Shift (NextWeapon), Ctrl (NextAmmo),
// Alt (Strafe) and Caps Lock (RunLock) were dead keys on a Mac (HANDOVER 5bi).
// pDown says whether the key went down; the device-dependent bits of the
// modifier flags tell left from right (IOLLEvent.h NX_DEVICE*KEYMASK).
// The LEFT keys report the generic VK the game itself tests (VK_SHIFT --
// MessageMgr.cpp:759); the right ones their own. Caps Lock returns *pToggle:
// one flagsChanged per press, so it is a press-and-release.
int LTInput_MacModifierToVK(int nKeyCode, unsigned long nFlags, bool* pDown, bool* pToggle)
{
    if (pToggle) *pToggle = false;
    bool bDown = false;
    int  vk = 0;
    switch (nKeyCode)
    {
        case 0x38: vk = VK_SHIFT;    bDown = (nFlags & 0x00000002) != 0; break;
        case 0x3C: vk = VK_RSHIFT;   bDown = (nFlags & 0x00000004) != 0; break;
        case 0x3B: vk = VK_CONTROL;  bDown = (nFlags & 0x00000001) != 0; break;
        case 0x3E: vk = VK_RCONTROL; bDown = (nFlags & 0x00002000) != 0; break;
        case 0x3A: vk = VK_MENU;     bDown = (nFlags & 0x00000020) != 0; break;
        case 0x3D: vk = VK_RMENU;    bDown = (nFlags & 0x00000040) != 0; break;
        case 0x39: vk = VK_CAPITAL;  bDown = true; if (pToggle) *pToggle = true; break;
        default: return 0;      // Command, Fn: not game keys
    }
    if (pDown) *pDown = bDown;
    return vk;
}


// ----------------------------------------------------------------------- //
// Scripted input. See ltinput.h.
// ----------------------------------------------------------------------- //

namespace
{
    int g_ScriptKeys[64];
    int g_nScriptKeys = 0;
    int g_nScriptInterval = 30;
    int g_nScriptNext = 0;
}

void LTInput_InitScript()
{
    const char* p = getenv("LT_TEST_KEYS");
    if (!p || !p[0]) return;

    const char* pI = getenv("LT_TEST_KEYS_INTERVAL");
    if (pI && atoi(pI) > 0) g_nScriptInterval = atoi(pI);

    while (*p && g_nScriptKeys < 64)
    {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        g_ScriptKeys[g_nScriptKeys++] = atoi(p);   // a VK code -- see the trace below
        while (*p && *p != ',') ++p;
    }

    fprintf(stderr, "[LTKEYS] %d scripted key(s), every %d frame(s)\n",
            g_nScriptKeys, g_nScriptInterval);
}

// ⚠️ LT_KEYHOLD="vk,from,to[;vk,from,to...]" -- HOLD A KEY LIKE A PLAYER DOES.
// LT_TEST_KEYS presses and releases in the same frame, which is right for a
// menu and wrong for anything the game POLLS: Duck and Jump are read as "is
// it down this frame", and a press that is up again before the game looks
// never happened. Down at `from`, up at `to`, through the same queue the
// window feeds -- so the binding layer is tested, not bypassed.
static void LTInput_KeyHoldTick(int nFrame)
{
    struct Hold { int vk, from, to; };
    static std::vector<Hold> s_Holds;
    static bool s_bParsed = false;
    if (!s_bParsed)
    {
        s_bParsed = true;
        if (const char* p = getenv("LT_KEYHOLD"))
        {
            while (*p)
            {
                Hold h = { 0, 0, 0 };
                if (sscanf(p, "%d,%d,%d", &h.vk, &h.from, &h.to) == 3) s_Holds.push_back(h);
                while (*p && *p != ';') ++p;
                if (*p == ';') ++p;
            }
            fprintf(stderr, "[LTKEYS] %d scripted key hold(s)\n", (int)s_Holds.size());
        }
    }
    for (size_t i = 0; i < s_Holds.size(); ++i)
    {
        const Hold& h = s_Holds[i];
        if (nFrame != h.from && nFrame != h.to) continue;
        LTInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.m_Kind = (nFrame == h.from) ? LTInputEvent::kKeyDown : LTInputEvent::kKeyUp;
        ev.m_Code = h.vk;
        LTInput_Push(ev);
        fprintf(stderr, "[LTKEYS] frame %d: VK %d %s\n", nFrame, h.vk, nFrame == h.from ? "DOWN" : "UP");
    }
}

void LTInput_ScriptTick(int nFrame)
{
    LTInput_KeyHoldTick(nFrame);
    if (g_nScriptNext >= g_nScriptKeys) return;
    if (nFrame < (g_nScriptNext + 1) * g_nScriptInterval) return;

    int dik = g_ScriptKeys[g_nScriptNext++];

    // ⚠️ Down AND up. A game that tracks key state would otherwise believe the
    // key is still held for the rest of the run.
    LTInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.m_Kind = LTInputEvent::kKeyDown; ev.m_Code = dik;
    LTInput_Push(ev);
    ev.m_Kind = LTInputEvent::kKeyUp;
    LTInput_Push(ev);

    // ⚠️ THESE ARE **VK** CODES, NOT DIK. The label said DIK for a long time
    // and cost a test run: LT_TEST_KEYS=42 is not Left Shift (DIK 42), it is
    // VK 42. The queue speaks Windows virtual keys and the binding layer maps
    // VK -> DIK itself (ltbindings.h). Shift is 16, '1' is 49, 'E' is 69.
    fprintf(stderr, "[LTKEYS] frame %d: VK %d\n", nFrame, dik);
}

// ⚠️ LT_TEST_CLICK="frame[,frame...]" -- A SCRIPTED MOUSE CLICK, and it exists
// because some things in this game can ONLY be advanced by clicking.
//
// CLTDialogueWnd::OnLButtonDown calls Skip(), which is what tells the server a
// line of dialogue is finished (CSM_DIALOGUE_DONE). A cinematic whose
// CinematicTrigger is not skippable waits for exactly that, so a headless test
// that can only press KEYS reports a hang that a person with a mouse would
// never see. A harness that cannot do what the player does measures the
// harness.
// LT_TEST_WHEEL="<frame>:<+1|-1>[,...]" -- scripted wheel notches.
static void LTInput_WheelTick(int nFrame)
{
    static const char* pWant = getenv("LT_TEST_WHEEL");
    if (!pWant || !pWant[0]) return;
    for (const char* p = pWant; *p; )
    {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        const int f = atoi(p);
        const char* c = strchr(p, ':');
        const int d = c ? atoi(c + 1) : 1;
        if (f == nFrame)
        {
            LTInputEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.m_Kind = LTInputEvent::kWheel;
            ev.m_DY = d > 0 ? 1 : -1;
            LTInput_Push(ev);
        }
        while (*p && *p != ',') ++p;
    }
}

void LTInput_ClickTick(int nFrame)
{
    LTInput_WheelTick(nFrame);
    static const char* pWant = getenv("LT_TEST_CLICK");
    if (!pWant || !pWant[0]) return;

    // "frame[:button],..." -- button 0 (left, the default), 1 right, 2 middle.
    static int  nAt[32];
    static int  nBtn[32];
    static int  nCount = -1;
    static int  nNext  = 0;
    if (nCount < 0)
    {
        nCount = 0;
        for (const char* p = pWant; *p && nCount < 32; )
        {
            while (*p == ' ' || *p == ',') ++p;
            if (!*p) break;
            nAt[nCount] = atoi(p);
            nBtn[nCount] = 0;
            while (*p && *p != ',' && *p != ':') ++p;
            if (*p == ':') { ++p; nBtn[nCount] = atoi(p); while (*p && *p != ',') ++p; }
            ++nCount;
        }
        fprintf(stderr, "[LTCLICK] %d scripted click(s)\n", nCount);
    }

    if (nNext >= nCount || nFrame < nAt[nNext]) return;
    const int nButton = nBtn[nNext];
    ++nNext;

    LTInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.m_Kind = LTInputEvent::kMouseDown; ev.m_Button = nButton;
    ev.m_X = 320; ev.m_Y = 240;
    LTInput_Push(ev);
    ev.m_Kind = LTInputEvent::kMouseUp;
    LTInput_Push(ev);

    fprintf(stderr, "[LTCLICK] frame %d: %s click\n", nFrame,
            nButton == 1 ? "right" : (nButton == 2 ? "middle" : "left"));
}



// The aim instrument's way in: radians the game will add to yaw / pitch,
// exactly as a mouse movement of that size would deliver them.
void LTInput_AddAxisRadians(float fYaw, float fPitch)
{
    g_AxisDX += fYaw / kAxisGain;
    g_AxisDY += fPitch / kAxisGain;
}

void LTInput_TakeAxisOffsets(float* p3)
{
    if (!p3) return;
    p3[0] = g_AxisDX * kAxisGain;
    p3[1] = g_AxisDY * kAxisGain;
    p3[2] = 0.0f;
    g_AxisDX = g_AxisDY = 0.0f;
}

unsigned LTInput_GetNumDelivered() { return g_nDelivered; }
unsigned LTInput_GetNumDropped()   { return g_nDropped; }
unsigned LTInput_GetNumUnmapped()  { return g_nUnmapped; }


// ----------------------------------------------------------------------- //
// Command state -- see the header for why this is separate from bindings.
// ----------------------------------------------------------------------- //

namespace
{
    // ⚠️ 100, NOT 64, AND THE OLD 64 SILENTLY THREW AWAY A THIRD OF THE
    // GAME'S CONTROLS.
    //
    // NOLF's command ids run 0..79 with 99 reserved as "unassigned"
    // (NOLF/Shared/CommandIDs.h). 64 covered movement, fire and activate --
    // which is why the port felt playable -- and discarded everything from
    // QUICKSAVE (64) upward: PREV_WEAPON (66), NEXT_WEAPON (67), MISSION (69),
    // CENTERVIEW (72), HOLSTER (73), INVENTORY (78). The shipped autoexec.cfg
    // binds Shift to NEXT_WEAPON, so "weapon switching does not work" was this
    // bound check and nothing else.
    //
    // ⚠️ The id is a NOLF number, so this array is sized from NOLF's header
    // rather than from anything the engine chose. It is deliberately not
    // #included -- the engine must not depend on game headers (HANDOVER 4.3)
    // -- so the citation above is the contract.
    const int kMaxCommands = 100;
    bool     g_CmdOn[kMaxCommands] = { false };
    unsigned g_nCmdAsked = 0;

    // ⚠️ COUNTED, NEVER SILENT. An id this layer refuses is a control the
    // player pressed and the game never heard, and the whole reason the old
    // limit survived this long is that it made no noise.
    unsigned g_nCmdRejected = 0;
    int      g_nCmdRejectedMax = -1;

    // ⚠️ THE EDGE QUEUE. See ltinput.h for why the transition is a separate
    // thing from the state, and why it is queued rather than dispatched here.
    //
    // ⚠️ SIZED FOR A FRAME, NOT FOR A SESSION. The host drains it every frame;
    // 64 is more transitions than a person can produce in one, and an overflow
    // is COUNTED so a full queue cannot look like a quiet one.
    const int      kMaxEdges = 64;
    LTCommandEdge  g_Edges[kMaxEdges];
    int            g_nEdges     = 0;
    unsigned       g_nEdgesLost = 0;
    unsigned       g_nEdgesMade = 0;

    int  g_HoldCmds[16];
    int  g_nHoldCmds = 0;
    int  g_nHoldFrom = 0;
    int  g_nHoldTo   = 0;        // 0 = no limit
    bool g_bHoldOn   = false;
}

void LTCommand_Set(int nCommand, bool bOn)
{
    if (nCommand < 0 || nCommand >= kMaxCommands)
    {
        ++g_nCmdRejected;
        if (nCommand > g_nCmdRejectedMax) g_nCmdRejectedMax = nCommand;
        return;
    }

    // ⚠️ ONLY A CHANGE IS AN EDGE. A key held down arrives as a repeat on
    // every frame it is held; queueing those would fire DoActivate sixty times
    // a second and change weapon continuously while the key is down.
    if (g_CmdOn[nCommand] == bOn) return;

    g_CmdOn[nCommand] = bOn;

    ++g_nEdgesMade;
    if (g_nEdges < kMaxEdges)
    {
        g_Edges[g_nEdges].m_nCommand = nCommand;
        g_Edges[g_nEdges].m_bOn      = bOn;
        ++g_nEdges;
    }
    else
    {
        ++g_nEdgesLost;
    }
}

int LTCommand_TakeEdges(LTCommandEdge* pOut, int nMax)
{
    if (!pOut || nMax <= 0) { g_nEdges = 0; return 0; }

    int n = g_nEdges < nMax ? g_nEdges : nMax;
    for (int i = 0; i < n; ++i) pOut[i] = g_Edges[i];

    // ⚠️ Anything that did not fit is DROPPED, not carried over. A transition
    // delivered a frame late is worse than one never delivered: the pair it
    // belongs to (on/off) would arrive out of order and leave the game
    // believing a key is still held.
    if (g_nEdges > n) g_nEdgesLost += (unsigned)(g_nEdges - n);
    g_nEdges = 0;
    return n;
}

bool LTCommand_IsOn(int nCommand)
{
    ++g_nCmdAsked;

    // LT_CMDASK=<id> -- did the game ASK about this command, and what did it
    // get? "The key works" and "the game read it" are different claims.
    {
        static const char* pW = getenv("LT_CMDASK");
        static const int   nW = pW ? atoi(pW) : -1;
        if (nCommand == nW)
        {
            static int nLast = -1;
            const int  nNow  = (nCommand >= 0 && nCommand < kMaxCommands &&
                                g_CmdOn[nCommand]) ? 1 : 0;
            if (nNow != nLast)
            {
                nLast = nNow;
                printf("[LTCMD] the game asked about command %d -> %s\n",
                       nCommand, nNow ? "ON" : "off");
                fflush(stdout);
            }
        }
    }

    if (nCommand < 0 || nCommand >= kMaxCommands)
    {
        ++g_nCmdRejected;
        if (nCommand > g_nCmdRejectedMax) g_nCmdRejectedMax = nCommand;
        return false;
    }
    return g_CmdOn[nCommand];
}

void LTCommand_Clear()
{
    // ⚠️ THROUGH LTCommand_Set, so losing focus with keys held produces the
    // OFF edges the game needs. Zeroing the array directly -- which is what
    // this used to do -- leaves CGameClientShell believing STRAFE is still
    // held, and it stays held until the key is pressed and released again.
    for (int i = 0; i < kMaxCommands; ++i) LTCommand_Set(i, false);
}

unsigned LTCommand_Asked() { return g_nCmdAsked; }

// ⚠️ THE COUNT IS THE DIAGNOSTIC. `CMoveMgr::UpdateControlFlags` returns
// before asking anything when the interface forbids camera movement
// (InterfaceMgr.cpp:358), so ZERO asks means the GATE is shut and a non-zero
// count means the gate is open and the problem is further down. Those are
// different bugs and they look identical from the outside.
void LTCommand_DumpCensus()
{
    printf("\n[LTCMD] IsCommandOn asked %u time(s)\n", g_nCmdAsked);
    printf("        %u command transition(s) queued", g_nEdgesMade);
    if (g_nEdgesLost) printf(", ⚠️  %u LOST (queue overflow)", g_nEdgesLost);
    printf("\n");
    if (!g_nEdgesMade)
        printf("        ⚠️  NO TRANSITIONS -- CGameClientShell::OnCommandOn\n"
               "            never ran, so activate, weapon change, reload and\n"
               "            the cinematic SKIP are all unreachable.\n");
    if (!g_nCmdAsked)
        printf("        ⚠️  NEVER ASKED -- CMoveMgr::UpdateControlFlags is\n"
               "            returning early; the interface state forbids\n"
               "            camera movement. Nothing below this can run.\n");
    if (g_nCmdRejected)
        printf("        ⚠️  %u command(s) OUT OF RANGE (highest id %d, limit %d)\n"
               "            -- a control the player pressed that the game never\n"
               "            heard. See kMaxCommands.\n",
               g_nCmdRejected, g_nCmdRejectedMax, kMaxCommands);
    fflush(stdout);
}

void LTCommand_InitScript()
{
    const char* p = getenv("LT_HOLD_COMMANDS");
    if (!p || !p[0]) return;

    while (*p && g_nHoldCmds < 16)
    {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        g_HoldCmds[g_nHoldCmds++] = atoi(p);
        while (*p && *p != ',') ++p;
    }

    if (const char* pF = getenv("LT_HOLD_FROM")) g_nHoldFrom = atoi(pF);
    if (const char* pT = getenv("LT_HOLD_TO"))   g_nHoldTo   = atoi(pT);

    fprintf(stderr, "[LTCMD] holding %d command(s) from frame %d",
            g_nHoldCmds, g_nHoldFrom);
    if (g_nHoldTo) fprintf(stderr, " to %d", g_nHoldTo);
    fprintf(stderr, "\n");
}

void LTCommand_ScriptTick(int nFrame)
{
    if (!g_nHoldCmds) return;

    const bool bWant = (nFrame >= g_nHoldFrom) &&
                       (g_nHoldTo == 0 || nFrame <= g_nHoldTo);
    if (bWant == g_bHoldOn) return;

    g_bHoldOn = bWant;
    for (int i = 0; i < g_nHoldCmds; ++i)
        LTCommand_Set(g_HoldCmds[i], bWant);

    fprintf(stderr, "[LTCMD] frame %d: scripted commands %s\n",
            nFrame, bWant ? "ON" : "OFF");
}
