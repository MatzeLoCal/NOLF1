// ----------------------------------------------------------------------- //
//
// MODULE  : ltinput.h
//
// PURPOSE : Turns macOS events into the two things NOLF actually listens to.
//
// ⚠️ NOLF TAKES MOUSE AND KEYBOARD THROUGH DIFFERENT DOORS
//
//   This is the trap the background doc flags as §6.3, and it is worse than
//   "mouse input is unusual" -- the two paths do not even resemble each other:
//
//   MOUSE -> a Win32 WINDOW PROCEDURE THE GAME INSTALLS ITSELF.
//       HookWindow() asks the engine for its HWND (GetEngineHook "HWND"),
//       saves the existing proc with GetWindowLong(GWL_WNDPROC), and puts
//       HookedWindowProc in its place. That proc handles WM_MOUSEMOVE,
//       WM_[LR]BUTTON{DOWN,UP,DBLCLK}, WM_CHAR and WM_SETCURSOR
//       (GameClientShell.cpp:8276). IClientShell has NO mouse callbacks at
//       all, so there is no other way in.
//
//   KEYBOARD -> IClientShell::OnKeyDown(key, rep) / OnKeyUp(key), which the
//       ENGINE calls directly (iclientshell.h:36). ⚠️ `key` is a WINDOWS
//       VIRTUAL KEY code -- the game compares it against VK_ESCAPE
//       (InterfaceMgr.cpp:2504) and VK_TOGGLE_SCREENSHOTMODE.
//
//   ⚠️ NOT DIK, despite appearances. autoexec.cfg's "##17" DIK codes belong to
//       a THIRD path -- the binding system (`rangebind "##keyboard" "##17"
//       ... "Forward"`), reached through GetDeviceBindings -- which is not
//       implemented. Mapping OnKeyDown to DIK produces a keyboard that is
//       silently, completely dead while the mouse works perfectly.
//
//   ⚠️ WM_CHAR is the exception that proves it: typed TEXT goes through the
//   window proc, while game KEYS go through OnKeyDown. Both are needed.
//
// ⚠️ AND THE ORDERING TRAP THAT HID ALL OF THIS
//
//   HookWindow() runs at the TOP of OnEngineInitialized, before the game calls
//   SetRenderMode -- and it begins with `if (!GetScreenSurface()) return FALSE`.
//   A renderer that refuses a screen surface until a mode is set therefore
//   makes the hook fail SILENTLY, and GetEngineHook is never even reached.
//
//   The real engine creates its window from screenwidth/screenheight in
//   autoexec.cfg before the shell runs, so the surface is already there. The
//   host now sets that initial mode at startup for the same reason.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_INPUT_H__
#define __LT_INPUT_H__

class IClientShell;

// One event, in a form both the Objective-C++ and C++ sides can hold.
struct LTInputEvent
{
    enum Kind { kMouseMove, kMouseDown, kMouseUp, kMouseDblClick,
                kKeyDown, kKeyUp, kChar,

                // ⚠️ THE WINDOW LOST FOCUS, and it is an INPUT event because
                // of what it means for held keys: a key that was down when
                // focus left sends its keyUp somewhere else, so every held
                // command must be dropped or the player walks forever.
                //
                // ⚠️ Reported as an EVENT rather than the window calling the
                // command layer directly -- the window layer knows about the
                // queue and nothing else, which is what lets the Metal test
                // link it without dragging in the input and binding modules.
                kFocusLost,

                // ⚠️ ONE MOUSE-WHEEL NOTCH: m_DY = +1 forward (away from the
                // user), -1 back. autoexec.cfg binds the wheel ("##z-axis")
                // to ZoomIn / ZoomOut, which is how a player zooms the scope.
                kWheel };

    int m_Kind;
    int m_X, m_Y;       // mouse: client coordinates, top-down

    // ⚠️ RELATIVE motion, and it is not derivable from m_X/m_Y. Mouse look
    // needs "how far did the mouse move", which stops being answerable from
    // absolute coordinates the moment the pointer reaches the window edge --
    // and under capture the pointer never moves at all while the mouse does.
    // Zero for every event that is not a move.
    int m_DX, m_DY;
    int m_Button;       // 0 = left, 1 = right
    int m_Code;         // keys: DIK scan code.  kChar: the character
    int m_Repeat;
};

// Called by the window layer. Drops events when full rather than blocking --
// a backed-up queue means the frame loop has stalled, and old input is worse
// than no input.
void LTInput_Push(const LTInputEvent& ev);

// ⚠️ An OBSERVER, not a filter. Set it and every event is shown to the callback
// as well as being delivered normally; leave it null -- the default -- and the
// pump behaves exactly as it always did, which is what the frame contract's
// recorded run depends on. Walk mode uses this to track which keys are HELD,
// because the queue only ever says "went down" and "went up".
typedef void (*LTInputObserver)(const LTInputEvent& ev);
void LTInput_SetObserver(LTInputObserver pFn);

// Drains the queue: mouse and WM_CHAR into the game's hooked window
// procedure, keys into the shell. Call once per frame, after pumping the
// platform event loop.
void LTInput_Pump(IClientShell* pShell);

// Maps a macOS virtual keycode to a Windows VIRTUAL KEY code, or 0.
int  LTInput_MacKeyToVK(int nMacCode);
// A modifier key's flagsChanged -> VK, down/up, and whether it is a toggle
// (Caps Lock: press and release in one). 0 for keys that are not game keys.
int LTInput_MacModifierToVK(int nKeyCode, unsigned long nFlags, bool* pDown, bool* pToggle);

// ⚠️ SCRIPTED INPUT -- background doc §3.2's `LT_TEST_KEYS`, and the reason it
// is worth having: without it, every menu bug needs a human at the keyboard and
// a screenshot round-trip. With it, any screen in the game is reachable from a
// headless run, so the text/2D traces can be pointed at it.
//
//   LT_TEST_KEYS="200,200,28"    DIK codes, pressed in order
//   LT_TEST_KEYS_INTERVAL=30     frames between presses (default 30)
//
// DIK codes worth knowing: 200 up, 208 down, 203 left, 205 right,
// 28 return, 1 escape.
void LTInput_InitScript();
void LTInput_ScriptTick(int nFrame);

// LT_TEST_CLICK="frame[,frame...]" -- scripted left clicks. Some windows only
// advance on a click; see the .cpp.
void LTInput_ClickTick(int nFrame);

// Diagnostics for the window-proc hook: the host registers its own default so
// the input layer can say whether the GAME ever replaced it.
void LTInput_SetHostDefaultProc(void* p);
bool LTInput_ProcIsHostDefault(void* p);

// ----------------------------------------------------------------------- //
// COMMANDS -- the layer between a key and the game's idea of "forward".
//
// ⚠️ EVERY MOVEMENT DECISION IN NOLF GOES THROUGH HERE. CMoveMgr asks
// `g_pLTClient->IsCommandOn(COMMAND_ID_FORWARD)` and nine siblings
// (CMoveMgr.cpp:384-470); the engine's `IsCommandOn` was a stub returning
// LTFALSE, so no movement command was EVER on and the player could not walk no
// matter how complete the physics underneath it was.
//
// ⚠️ THIS IS COMMAND STATE, NOT A BINDING SYSTEM. Turning a real key into a
// command needs `GetDeviceBindings`, which is still a stub -- the game reads
// the player's own bindings out of autoexec.cfg through it. Until that exists,
// commands are set by the host: scripted for a headless run, and later by a
// binding table. Keeping the two apart means the binding work lands in one
// place instead of being spread through the input queue.
//
// Command ids are the GAME's (NOLF/Shared/CommandIDs.h): 0 forward,
// 1 reverse, 2 left, 3 right, 4 duck, 5 jump, 6 run, ...
void LTCommand_Set(int nCommand, bool bOn);
bool LTCommand_IsOn(int nCommand);
void LTCommand_Clear();

// ⚠️ THE EDGE, AND IT IS A SEPARATE THING FROM THE STATE.
//
// `IsCommandOn` answers a LEVEL: "is forward held right now". Every
// CONTINUOUS action in NOLF reads it, and that is why walking worked as soon
// as the state existed. But every DISCRETE action -- activate, change weapon,
// reload, flashlight, zoom, run-lock -- is handled in
// `CGameClientShell::OnCommandOn` (GameClientShell.cpp:4075), a callback the
// ENGINE is required to make on the off->on TRANSITION. The port set the state
// and never made the call, so `OnCommandOn` had never run once.
//
// ⚠️ ONE MISSING CALL, AND IT LOOKED LIKE FIVE UNRELATED BUGS:
//     "E is not working"            -- COMMAND_ID_ACTIVATE -> DoActivate
//     "weapon keys do not work"     -- the weapon-change range, same switch
//     "the cutscene never ends"     -- the cinematic's ONLY designed exit is
//                                      the player pressing Activate, which
//                                      reaches CPlayerObj::Activate ->
//                                      PlayingCinematic(TRUE) -> "SKIP"
// Each was reported separately and each was this.
//
// ⚠️ THE INPUT LAYER MUST NOT CALL THE SHELL. It does not know what a shell
// is, and wiring one in here is the layering mistake that broke the Metal
// engine test once already (ltwindow.mm calling LTCommand_Clear directly).
// So the edges are QUEUED here and drained by the host, which owns the shell.
struct LTCommandEdge
{
    int  m_nCommand;
    bool m_bOn;
};

// Drains the queue: writes at most nMax transitions and returns how many.
// Overflow is counted and reported by the census rather than silently lost.
int LTCommand_TakeEdges(LTCommandEdge* pOut, int nMax);

// ⚠️ A TEST INSTRUMENT, and labelled as one. LT_HOLD_COMMANDS="0,6" holds
// FORWARD and RUN for a headless run, so "does the player walk" becomes a
// question a run can answer. LT_HOLD_FROM / LT_HOLD_TO bound it by frame.
void LTCommand_InitScript();
void LTCommand_ScriptTick(int nFrame);
unsigned LTCommand_Asked();
void     LTCommand_DumpCensus();

// ⚠️ MOUSE LOOK. `CGameClientShell::UpdatePlayerRotation` reads three floats
// and uses them DIRECTLY as yaw and pitch deltas (GameClientShell.cpp:2620):
//
//     g_pLTClient->GetAxisOffsets(offsets);
//     m_fYaw   += offsets[0] / fZoom;
//     m_fPitch -= offsets[1] / fZoom;
//
// so the gain lives here, and it is the SAME constant the walk viewer uses --
// two different mouse-look implementations that felt different would be a bug
// nobody could name.
//
// ⚠️ TAKE, NOT PEEK: the accumulator is zeroed by this call. The game asks once
// per frame and expects the movement SINCE the last ask; returning a running
// total would spin the camera faster the longer you played.
void LTInput_TakeAxisOffsets(float* pOffsets3);

// Adds yaw / pitch in RADIANS to the pending mouse offsets -- for LT_AIMAT.
void LTInput_AddAxisRadians(float fYaw, float fPitch);

unsigned LTInput_GetNumDelivered();
unsigned LTInput_GetNumDropped();
unsigned LTInput_GetNumUnmapped();

#endif // __LT_INPUT_H__
