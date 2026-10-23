// ----------------------------------------------------------------------- //
//
// MODULE  : ltbindings.h
//
// PURPOSE : KEY -> ACTION -> COMMAND. The last link between a person at the
//           keyboard and the player moving.
//
// ⚠️ THE WHOLE BINDING SYSTEM IS DATA, and it ships with the game. autoexec.cfg
// carries it in two halves:
//
//     AddAction Forward 0                                  name -> command id
//     rangebind "##keyboard" "##17" 0 0 "Forward"           key  -> action name
//
// So this module implements two console commands and a lookup, and NOLF's own
// configuration supplies every key the player expects. Nothing here invents a
// layout; changing `autoexec.cfg` changes the controls, exactly as it does in
// the shipped game.
//
// ⚠️ TWO ENCODINGS, AND GETTING THEM BACKWARDS MAKES A DEAD KEYBOARD.
// `rangebind` speaks DIK scan codes (`##17` is DIK_W). `IClientShell::OnKeyDown`
// and this port's input events speak WINDOWS VIRTUAL KEY codes. ltinput.cpp
// documents the trap in full; this module owns the VK->DIK table that bridges
// it, because the binding system is the only thing that needs it.
//
// ⚠️ IT DRIVES `IsCommandOn`, NOT THE KEY QUEUE. Every movement decision in
// NOLF reads `g_pLTClient->IsCommandOn(COMMAND_ID_FORWARD)` and nine siblings;
// the queue is for menus and typed text. Keeping the two apart is why holding a
// key works at all -- the queue only reports transitions.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_BINDINGS_H__
#define __LT_BINDINGS_H__

// Registers `AddAction` and `rangebind`/`bind` with the console, so running
// autoexec.cfg installs the player's own controls. Call once, before the
// config is run.
void LTBind_Register();

// Runs the game's autoexec.cfg through the console. Read-only: the config is
// opened through the file manager and never written.
// Returns the number of lines applied, or 0 when it could not be read.
int  LTBind_LoadConfig(const char* pPath);

// ⚠️ THE PLAYER'S OWN CONFIG, OUTSIDE THE INSTALL (user decision 2026-10-17):
// ~/Library/Application Support/NOLF1/<name>. FULLGAME/ is never written.
//
// Read: the install's autoexec.cfg first (LTBind_LoadConfig), then this file
// on top. Write: what the reference's cc_SaveConfigFile writes -- every
// console variable marked for saving as `"name" "value"`, then the bindings
// (input_SaveBindings) -- on the game's WriteConfigFile and at shutdown
// (SaveAutoConfig).
//
// ⚠️ Only for a run with a WINDOW (a player), or with LT_CONFIGDIR=<dir> (a
// test, writing there instead): headless runs must neither read the player's
// settings (determinism, the frame contract) nor overwrite them.
// LT_NOUSERCONFIG=1 turns it off entirely (says so).
bool        LTConfig_Enabled();
const char* LTConfig_Dir();
int         LTConfig_LoadUser(const char* pName);   // lines applied, 0 if no file
bool        LTConfig_Write(const char* pName);
// Set once the install's config was read: no write before (a failed start
// must not replace the player's settings with an empty file).
void        LTConfig_SetCanSave(bool b);

// The command id bound to a Windows virtual key, or -1.
int  LTBind_CommandForVK(int nVK);

// Called by the input pump on every key transition; sets the command state.
// Returns true when the key was bound to something.
bool LTBind_OnKey(int nVK, bool bDown);

// ⚠️ THE MOUSE BUTTONS, AND THEY ARE WHERE FIRING LIVES. The shipped
// autoexec.cfg binds Fire to button 0, Activate to button 1 and Reload to
// button 2. Mouse events used to reach only the Win32 message proc -- good for
// menus, useless for the game -- so no mouse button could set a command and
// there was no way to shoot. Returns true when the button was bound.
bool LTBind_OnMouseButton(int nButton, bool bDown);

// ⚠️ THE MOUSE WHEEL -- `rangebind "##mouse" "##z-axis" -255 -0.1 "ZoomOut"
// 0.1 255 "ZoomIn"`. A notch is a PULSE: the command goes on for one frame
// (so OnCommandOn sees the edge) and LTBind_ReleasePulses turns it off at the
// next pump. nDir > 0 is forward.
bool LTBind_OnWheel(int nDir);
void LTBind_ReleasePulses();

// ---- device tracking --------------------------------------------------- //
//
// ⚠️ THE KEY-ASSIGNMENT SCREEN IS A THIRD INPUT PATH, and neither of the other
// two can serve it. Menu navigation uses `OnKeyDown` (VK); movement uses
// `IsCommandOn`; but Options -> Customize Controls waits on
// `StartDeviceTrack` / `TrackDevice` / `EndDeviceTrack`
// (FolderCustomControls.cpp:514-562) to see the RAW key a person pressed.
//
// With those stubbed the screen sees NOTHING -- so Enter and Escape navigate
// it perfectly while no key can ever be assigned, which reads as "letters are
// broken" and is really "this screen uses an API that does not exist yet".
//
// ⚠️ THE DIK CODE RIDES IN BYTE 63 OF m_ControlName. That is LithTech's
// convention, not ours, and the game reads it back as
// `uint8 diCode = (uint8)pInput->m_ControlName[63]`
// (FolderCustomControls.cpp:609) before emitting
// `rangebind "##keyboard" "##<diCode>" 0 0 "<Action>"` -- which is exactly the
// console command this module already parses. Fill byte 63 and the rebinding
// loop closes on itself.
void LTDevice_StartTrack(unsigned nDevices, unsigned nBufferSize);
void LTDevice_EndTrack();
bool LTDevice_Tracking();

// Fills pArray with the key transitions seen since the last call; *pnInOut is
// the array size in, and the number written out.
void LTDevice_Track(void* pArray, unsigned* pnInOut);

// "##keyboard" / "##mouse" -- the names autoexec.cfg and `rangebind` use.
bool LTDevice_GetName(unsigned nDeviceType, char* pBuf, unsigned nBufLen);

// ⚠️ WHAT IS CURRENTLY BOUND, as the linked list the Controls screen walks to
// DISPLAY each row. Without it the screen can assign a key and then shows
// nothing next to any action, which reads as the assignment having failed.
//
// `strTriggerName` is matched against the name TrackDevice reported
// (FolderCustomControls.cpp:687), so both must come from the same place.
void* LTDevice_GetBindings(unsigned nDeviceFlags);
void  LTDevice_FreeBindings(void* pList);

// The objects (keys) a device offers, as the linked list the game walks.
// The caller frees it with LTDevice_FreeObjects.
void* LTDevice_GetObjects(unsigned nDeviceFlags);
void  LTDevice_FreeObjects(void* pList);

// ---- census ------------------------------------------------------------ //
//
// ⚠️ Counted because "the controls work" is otherwise unverifiable from a
// headless run, and because a binding table that silently ends up EMPTY looks
// exactly like a keyboard that is not plugged in.
unsigned LTBind_ActionCount();
unsigned LTBind_BindingCount();
// NOLF's own name for a command id, for diagnostics (see the .cpp -- it is a
// transcription of NOLF/Shared/CommandIDs.h, not an include).
const char* LTBind_CommandName(int nCommand);
void        LTBind_CheckMacKeys();      // LT_KEYCHECK -- see ltbindings.cpp

void     LTBind_DumpCensus();

#endif // __LT_BINDINGS_H__
