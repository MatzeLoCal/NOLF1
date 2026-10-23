// ----------------------------------------------------------------------- //
//
// MODULE  : ltwindow.h
//
// PURPOSE : The macOS window -- the first platform dependency in the port, and
//           the step that makes everything already working VISIBLE.
//
// ⚠️ OFF BY DEFAULT, AND THAT IS NOT A CONVENIENCE
//
//   The frame contract depends on the run being headless, deterministic and
//   fast. A window brings an event loop, a compositor, a display refresh and a
//   user -- every one of which is a source of variation the contract cannot
//   absorb. So:
//
//       LT_WINDOW=1     opens a window and presents on FlipScreen
//       (unset)         no window, no AppKit, no event loop
//
//   `harness/` never sets it. A run with a window is for looking at; a run
//   without one is for measuring. Conflating those is how a reproducible gate
//   becomes a flaky one.
//
// WHAT IT IS
//
//   One NSWindow holding one view that blits the engine's screen surface --
//   already plain top-down RGBA8888 -- through CoreGraphics. No Metal, no
//   shaders, no texture upload path: at 640x480 a CPU blit costs nothing worth
//   optimising, and it keeps this file small enough to be obviously correct.
//   Metal belongs with the 3D renderer, not here.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WINDOW_H__
#define __LT_WINDOW_H__

// True if LT_WINDOW is set. Cheap; safe to call anywhere.
bool LTWindow_Wanted();

// Creates the window. Returns false if it could not be created, or if
// LT_WINDOW is not set -- in which case every other call here is a no-op.
bool LTWindow_Open(int nWidth, int nHeight, const char* pTitle);

// Uploads and displays one frame. pRGBA is nWidth*nHeight*4, top-down.
void LTWindow_Present(const unsigned char* pRGBA, int nWidth, int nHeight);

// Drains pending events. ⚠️ Must be called every frame: without it the window
// never draws, the beachball appears, and the app looks hung.
void LTWindow_PumpEvents();

// True once the user has asked to close the window.
bool LTWindow_ShouldClose();

void LTWindow_Close();

// ⚠️ MOUSE LOOK: hides the cursor and DETACHES it from the mouse, so the
// pointer stops moving while motion still arrives as deltas. That is what an
// FPS camera needs and it is also a state that must never be left behind --
// LTWindow_Close() releases it, and so does losing key focus, because a cursor
// that stays hidden after the game has gone is the user's whole desktop broken.
// ⚠️ The NSView, as an opaque pointer, so the Metal backend can attach its
// layer without this file importing Metal or that file importing AppKit's
// window management. Returns null when the window is not open.
void* LTWindow_GetContentView();

// Replaces the title bar text (the host shows renderer and FPS there).
// A no-op when the window is not open.
void LTWindow_SetTitle(const char* pTitle);

void LTWindow_SetMouseLook(bool bOn);
bool LTWindow_GetMouseLook();

// The main screen's width / height (its full frame), or 0 when there is none.
// Asked before the window opens: the compositor shapes the game's surface to
// it (HANDOVER 5bq).
float LTWindow_ScreenAspect();

// ⚠️ THE MOUSE THROUGH THE COMPOSITOR. With a mapper installed, a pointer
// position is handed over in BACKING PIXELS (points x scale, top-down) and the
// mapper returns the game-surface pixel -- the inverse of what the compositor
// last presented, so a click on a zoomed menu lands on the item under it.
// Null = the old linear bounds -> surface scale.
typedef bool (*LTWindowPointMapFn)(float fX, float fY, int* pX, int* pY);
void LTWindow_SetPointMapper(LTWindowPointMapFn fn);

// Resizes the window's CONTENT to w x h points, centred where it was. For the
// LT_WINRESIZE test switch: a headless harness cannot drag a window corner.
void LTWindow_SetContentSize(int nWidth, int nHeight);

#endif // __LT_WINDOW_H__
