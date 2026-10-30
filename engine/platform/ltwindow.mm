// ----------------------------------------------------------------------- //
//
// MODULE  : ltwindow.mm
//
// PURPOSE : The macOS window. See ltwindow.h for why it is off by default.
//
// ⚠️ OBJECTIVE-C++, AND THE ONLY SUCH FILE IN THE PORT
//
//   It is `.mm` because AppKit needs Objective-C. Everything it touches on the
//   engine side is a plain byte buffer and two ints, so the Objective-C stays
//   sealed inside this file -- nothing else in the engine includes an AppKit
//   header, and the C++ side never learns that this platform exists.
//
// ----------------------------------------------------------------------- //

#include "ltwindow.h"
#include "ltinput.h"

#import <Cocoa/Cocoa.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// ----------------------------------------------------------------------- //

static LTWindowPointMapFn g_fnPointMap = 0;

@interface LTFrameView : NSView
{
@public
    unsigned char*  m_pPixels;      // owned; RGBA8888, top-down
    int             m_Width;
    int             m_Height;
}
@end

@implementation LTFrameView

- (BOOL)isFlipped { return YES; }    // top-down, matching the engine's buffers

// ⚠️ The view must be able to take key events, or the keyboard silently does
// nothing while the mouse works -- which reads as "keys are broken" rather
// than "the responder chain is wrong".
- (BOOL)acceptsFirstResponder { return YES; }

// ⚠️ YES, so a click that focuses the window is ALSO delivered. Without it the
// first click after switching apps is swallowed, which feels like a dropped
// input rather than a focus rule.
- (BOOL)acceptsFirstMouse:(NSEvent*)e { (void)e; return YES; }

// Window point -> client point, top-down. isFlipped already gives top-down
// view coordinates, so this is just the conversion.
- (void)pointFor:(NSEvent*)e outX:(int*)px outY:(int*)py
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];

    // ⚠️ THROUGH THE COMPOSITOR when it presents (HANDOVER 5bq): the picture is
    // no longer the surface stretched over the view -- menus are a zoomed box,
    // the game may sit between bars -- so only its own inverse is right.
    if (g_fnPointMap)
    {
        const CGFloat sc = [[self window] backingScaleFactor] > 0 ? [[self window] backingScaleFactor] : 1.0;
        int x = 0, y = 0;
        if (g_fnPointMap((float)(p.x * sc), (float)(p.y * sc), &x, &y)) { *px = x; *py = y; return; }
    }

    // Scale back to the ENGINE's resolution: the window is resizable, and the
    // game reasons entirely in 640x480 screen coordinates.
    CGFloat vw = [self bounds].size.width;
    CGFloat vh = [self bounds].size.height;
    if (vw > 0 && vh > 0 && m_Width > 0 && m_Height > 0)
    {
        p.x = p.x * (CGFloat)m_Width  / vw;
        p.y = p.y * (CGFloat)m_Height / vh;
    }

    *px = (int)p.x;
    *py = (int)p.y;
}

- (void)push:(int)kind event:(NSEvent*)e button:(int)btn
{
    LTInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.m_Kind = kind;
    ev.m_Button = btn;
    [self pointFor:e outX:&ev.m_X outY:&ev.m_Y];
    LTInput_Push(ev);
}

// ⚠️ deltaX/deltaY, NOT a difference of positions. Under mouse look the cursor
// is detached and its POSITION never changes, so a position difference would be
// permanently zero while the mouse is being moved.
- (void)pushMove:(NSEvent*)e button:(int)btn
{
    LTInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.m_Kind = LTInputEvent::kMouseMove;
    ev.m_Button = btn;
    [self pointFor:e outX:&ev.m_X outY:&ev.m_Y];
    ev.m_DX = (int)[e deltaX];
    ev.m_DY = (int)[e deltaY];
    LTInput_Push(ev);
}

- (void)mouseMoved:(NSEvent*)e   { [self pushMove:e button:0]; }
- (void)mouseDragged:(NSEvent*)e { [self pushMove:e button:0]; }
- (void)rightMouseDragged:(NSEvent*)e { [self pushMove:e button:1]; }

- (void)mouseDown:(NSEvent*)e
{
    if ([e clickCount] >= 2) [self push:LTInputEvent::kMouseDblClick event:e button:0];
    else                     [self push:LTInputEvent::kMouseDown     event:e button:0];
}
- (void)mouseUp:(NSEvent*)e { [self push:LTInputEvent::kMouseUp event:e button:0]; }

- (void)rightMouseDown:(NSEvent*)e
{
    if ([e clickCount] >= 2) [self push:LTInputEvent::kMouseDblClick event:e button:1];
    else                     [self push:LTInputEvent::kMouseDown     event:e button:1];
}
- (void)rightMouseUp:(NSEvent*)e { [self push:LTInputEvent::kMouseUp event:e button:1]; }

// ⚠️ THE MIDDLE BUTTON is "Button 2" in autoexec.cfg -- Reload. AppKit calls
// every button past the second an "other" mouse button.
- (void)otherMouseDown:(NSEvent*)e
{
    if ([e buttonNumber] == 2) [self push:LTInputEvent::kMouseDown event:e button:2];
}
- (void)otherMouseUp:(NSEvent*)e
{
    if ([e buttonNumber] == 2) [self push:LTInputEvent::kMouseUp event:e button:2];
}
- (void)otherMouseDragged:(NSEvent*)e { [self pushMove:e button:2]; }

// ⚠️ THE WHEEL, AS NOTCHES. DirectInput's z-axis is the wheel and the shipped
// autoexec.cfg binds it to ZoomIn (forward) / ZoomOut (back). A mouse wheel
// gives whole lines per event; a trackpad gives a stream of small pixel
// deltas, so those are ACCUMULATED and one notch is sent per 24 points --
// without that, one swipe would zoom through every level at once.
// ⚠️ Natural scrolling inverts deltaY; that is undone so that pushing the
// wheel AWAY always zooms in, as on Windows.
- (void)scrollWheel:(NSEvent*)e
{
    static float s_fAcc = 0.0f;
    float dy = [e hasPreciseScrollingDeltas] ? (float)[e scrollingDeltaY] / 24.0f
                                             : (float)[e deltaY];
    if ([e isDirectionInvertedFromDevice]) dy = -dy;
    if ([e phase] == NSEventPhaseBegan) s_fAcc = 0.0f;
    s_fAcc += dy;
    while (s_fAcc >= 1.0f || s_fAcc <= -1.0f)
    {
        LTInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.m_Kind = LTInputEvent::kWheel;
        ev.m_DY = (s_fAcc > 0.0f) ? 1 : -1;
        s_fAcc -= (float)ev.m_DY;
        LTInput_Push(ev);
    }
    // A mouse wheel's fractional deltas (some report 0.1) still count once.
    if (![e hasPreciseScrollingDeltas] && dy != 0.0f && s_fAcc != 0.0f && fabsf(dy) < 1.0f)
    {
        LTInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.m_Kind = LTInputEvent::kWheel;
        ev.m_DY = (dy > 0.0f) ? 1 : -1;
        s_fAcc = 0.0f;
        LTInput_Push(ev);
    }
}

// ⚠️ KEYS AND TEXT ARE TWO SEPARATE EVENTS, because NOLF listens on two
// separate doors (ltinput.h). A key press produces BOTH: a DIK scan code for
// OnKeyDown, and -- if it is printable -- a character for WM_CHAR.
- (void)keyDown:(NSEvent*)e
{
    // ⚠️ CTRL-CMD-F IS FULLSCREEN, the macOS shortcut, taken BEFORE the game
    // sees the key. There is no menu bar to carry it, so without this the
    // green button would be the only way in -- and with the pointer captured
    // for mouse look, no way out but Esc to a menu first.
    const NSEventModifierFlags mods = [e modifierFlags];
    if ((mods & NSEventModifierFlagCommand) && (mods & NSEventModifierFlagControl) &&
        [e keyCode] == 3 /* kVK_ANSI_F */)
    {
        [[self window] toggleFullScreen:nil];
        return;
    }

    LTInputEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.m_Kind   = LTInputEvent::kKeyDown;
    ev.m_Code   = LTInput_MacKeyToVK((int)[e keyCode]);
    ev.m_Repeat = [e isARepeat] ? 1 : 0;
    if (ev.m_Code) LTInput_Push(ev);

    NSString* chars = [e characters];
    if ([chars length] > 0)
    {
        unichar c = [chars characterAtIndex:0];
        if (c >= 32 && c < 127)
        {
            LTInputEvent ce;
            memset(&ce, 0, sizeof(ce));
            ce.m_Kind = LTInputEvent::kChar;
            ce.m_Code = (int)c;
            LTInput_Push(ce);
        }
    }
}

// ⚠️ THE MODIFIERS ARRIVE HERE, NOT IN keyDown/keyUp -- see
// LTInput_MacModifierToVK. Shift, Ctrl, Option and Caps Lock were dead keys.
- (void)flagsChanged:(NSEvent*)e
{
    bool bDown = false, bToggle = false;
    const int vk = LTInput_MacModifierToVK((int)[e keyCode], (unsigned long)[e modifierFlags],
                                           &bDown, &bToggle);
    if (!vk) return;
    LTInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.m_Code = vk;
    if (bToggle)
    {
        ev.m_Kind = LTInputEvent::kKeyDown; LTInput_Push(ev);
        ev.m_Kind = LTInputEvent::kKeyUp;   LTInput_Push(ev);
        return;
    }
    ev.m_Kind = bDown ? LTInputEvent::kKeyDown : LTInputEvent::kKeyUp;
    LTInput_Push(ev);
}

- (void)keyUp:(NSEvent*)e
{
    LTInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.m_Kind = LTInputEvent::kKeyUp;
    ev.m_Code = LTInput_MacKeyToVK((int)[e keyCode]);
    if (ev.m_Code) LTInput_Push(ev);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (!m_pPixels || m_Width <= 0 || m_Height <= 0) return;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        m_pPixels, m_Width, m_Height, 8, (size_t)m_Width * 4, cs,
        kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);

    if (ctx)
    {
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        if (img)
        {
            CGContextRef dst = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
            CGContextSetInterpolationQuality(dst, kCGInterpolationNone);

            // ⚠️ THE FRAME ARRIVES UPSIDE DOWN WITHOUT THIS, and the cause is
            // that `isFlipped = YES` buys two different things at once:
            //
            //   * top-down MOUSE coordinates, which is what the engine wants
            //     and why the view is flipped at all;
            //   * a flipped CTM, which CGContextDrawImage then applies to the
            //     image -- so a top-down buffer is drawn bottom-up.
            //
            // Un-flipping the CTM for the image only keeps both correct.
            // Making the view non-flipped instead would fix the picture and
            // silently invert every mouse Y, which is far harder to spot: the
            // menu would highlight the wrong item rather than look wrong.
            NSRect b = [self bounds];
            CGContextSaveGState(dst);
            CGContextTranslateCTM(dst, 0, b.size.height);
            CGContextScaleCTM(dst, 1.0, -1.0);
            CGContextDrawImage(dst, b, img);
            CGContextRestoreGState(dst);

            CGImageRelease(img);
        }
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);
}
@end


@interface LTWindowDelegate : NSObject <NSWindowDelegate>
@end

static bool g_bShouldClose = false;

static void LTWindow_ApplyMouseLook(bool bFocused);

@implementation LTWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    (void)sender;
    g_bShouldClose = true;
    return NO;      // the engine decides when to tear down, not AppKit
}

// ⚠️ Cmd-Tab away and you get your cursor back. Without this the pointer stays
// hidden and detached over every other application, which looks exactly like a
// frozen machine and is the user's problem, not the game's.
- (void)windowDidResignKey:(NSNotification*)n
{
    (void)n;
    // Release the pointer but REMEMBER whether the game wants it, so coming
    // back re-captures (see windowDidBecomeKey).
    LTWindow_ApplyMouseLook(false);

    // ⚠️ AND TELL THE INPUT LAYER TO DROP EVERY HELD KEY. A key that was down
    // when focus left sends its keyUp to whatever took focus, never to us --
    // so the command stays ON and the player walks forever with nothing on
    // screen explaining why. Cmd-Tab, a notification, or a click elsewhere.
    //
    // ⚠️ THIS IS NOT A TIDY-UP, IT IS THE FIX FOR A STUCK CONTROL: held state
    // ends only on an event, and focus loss guarantees the ending event never
    // arrives.
    LTInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.m_Kind = LTInputEvent::kFocusLost;
    LTInput_Push(ev);
}

// ⚠️ FOCUS BACK -> CAPTURE BACK, if the game is in a mouse-look state. Before
// this, Cmd-Tab away and back (or clicking into the window) left the pointer
// free over a game that was flying its camera (HANDOVER 5bb).
- (void)windowDidBecomeKey:(NSNotification*)n
{
    (void)n;
    LTWindow_ApplyMouseLook(true);
}
@end


// ----------------------------------------------------------------------- //

namespace
{
    NSWindow*           g_pWindow = nil;
    LTFrameView*        g_pView   = nil;
    LTWindowDelegate*   g_pDelegate = nil;
    bool                g_bOpen = false;
}


bool LTWindow_Wanted()
{
    static int s = -1;
    if (s < 0)
    {
        const char* p = getenv("LT_WINDOW");
        s = (p && p[0] && p[0] != '0') ? 1 : 0;
    }
    return s != 0;
}


bool LTWindow_Open(int nWidth, int nHeight, const char* pTitle)
{
    if (!LTWindow_Wanted()) return false;
    if (g_bOpen) return true;
    if (nWidth <= 0 || nHeight <= 0) return false;

    @autoreleasepool
    {
        [NSApplication sharedApplication];

        // ⚠️ Regular, not Accessory. Without this the window opens behind
        // everything, cannot take focus, and receives no keyboard -- which
        // looks like broken input rather than a missing activation policy.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // ⚠️ finishLaunching BEFORE THE WINDOW EXISTS, and that order is the
        // whole bug. AppKit sets up event routing and the key-window machinery
        // during finishLaunching; a window made key BEFORE it runs is ordered
        // front and drawn, and never becomes the keyboard's destination.
        //
        // The symptom is exact and misleading: the game renders, presents,
        // animates and responds to the mouse, and `nextEventMatchingMask:
        // NSEventMaskAny` returns not one key event -- so it reads as a
        // keyboard/binding problem rather than an application-startup one.
        // Measured: zero NSEventTypeKeyDown in a windowed run, with the
        // bindings verified correct headlessly.
        [NSApp finishLaunching];

        // ⚠️ FITTED TO THE SCREEN, keeping the surface's shape. The surface is
        // sized in pixels for the game; as a window in points it can be taller
        // than the screen (1024x768 points on a 1280x832 Retina panel leaves
        // no room for the title bar) -- and a window the system has to shove
        // around opens off-centre and clipped.
        int nWinW = nWidth, nWinH = nHeight;
        if (NSScreen* scr = [NSScreen mainScreen])
        {
            const NSRect vis = [scr visibleFrame];
            const CGFloat maxW = vis.size.width * 0.92, maxH = (vis.size.height - 32.0) * 0.92;
            const CGFloat k = fmin(1.0, fmin(maxW / nWidth, maxH / nHeight));
            nWinW = (int)(nWidth * k + 0.5); nWinH = (int)(nHeight * k + 0.5);
        }
        NSRect frame = NSMakeRect(0, 0, nWinW, nWinH);

        g_pWindow = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable |
                                 NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (!g_pWindow) return false;

        // ⚠️ ARC OWNS THIS WINDOW, SO CLOSING MUST NOT RELEASE IT.
        //
        // NSWindow's `releasedWhenClosed` defaults to YES for a window created
        // with -initWithContentRect:. This file is built with -fobjc-arc and
        // `g_pWindow` is a strong global, so LTWindow_Close's
        //
        //     [g_pWindow close];     <- AppKit releases it here
        //     g_pWindow = nil;       <- ARC releases it AGAIN here
        //
        // over-released the window. The crash does not happen at either line:
        // it happens when the enclosing @autoreleasepool pops and touches the
        // dead object, which is why the backtrace named objc_release inside
        // objc_autoreleasePoolPop and not this file at all.
        //
        // ⚠️ IT ONLY EVER HAPPENED WITH A WINDOW, AT EXIT -- so every headless
        // run was clean (12 levels, 8 repeats, 2000-update runs: zero) while
        // `./play.sh game` died 3 times in 3. That asymmetry is what made it
        // look like a gameplay bug worth chasing for days.
        [g_pWindow setReleasedWhenClosed:NO];

        // ⚠️ NATIVE FULLSCREEN (the green button, Ctrl-Cmd-F): its own Space,
        // menu bar and Dock hidden, any panel's shape -- the compositor fills
        // whatever it is given (HANDOVER 5bq).
        [g_pWindow setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];

        [g_pWindow setTitle:[NSString stringWithUTF8String:(pTitle ? pTitle : "NOLF")]];
        [g_pWindow center];

        g_pView = [[LTFrameView alloc] initWithFrame:frame];
        g_pView->m_pPixels = 0;
        g_pView->m_Width = 0;
        g_pView->m_Height = 0;

        [g_pWindow setContentView:g_pView];

        g_pDelegate = [[LTWindowDelegate alloc] init];
        [g_pWindow setDelegate:g_pDelegate];

        // ⚠️ THE ONE THE BACKGROUND DOC WARNS ABOUT (§6.3). Without this,
        // mouse MOVEMENT is never delivered -- clicks still work, so the
        // symptom is that menu rollover/highlight is dead while clicking is
        // fine, which reads as a UI bug rather than an input one.
        [g_pWindow setAcceptsMouseMovedEvents:YES];

        // ⚠️ The view is the keyboard's destination before the window is shown,
        // so the FIRST key needs no click to land.
        [g_pWindow setInitialFirstResponder:g_pView];

        [g_pWindow makeKeyAndOrderFront:nil];
        [g_pWindow makeFirstResponder:g_pView];

        // ⚠️ `activateIgnoringOtherApps:` IS DEPRECATED AND INCREASINGLY
        // IGNORED. macOS 14 added `-[NSApplication activate]` and newer
        // systems quietly refuse the old call, which leaves the process
        // running, drawing, and never the active application -- so every key
        // goes to whatever terminal launched it. Prefer the new one when the
        // running system has it; keep the old as the fallback.
        if ([NSApp respondsToSelector:@selector(activate)])
            [NSApp performSelector:@selector(activate)];
        else
            [NSApp activateIgnoringOtherApps:YES];
    }

    g_bOpen = true;
    g_bShouldClose = false;
    fprintf(stderr, "[LTWINDOW] %dx%d open\n", nWidth, nHeight);
    return true;
}


void LTWindow_Present(const unsigned char* pRGBA, int nWidth, int nHeight)
{
    if (!g_bOpen || !pRGBA || nWidth <= 0 || nHeight <= 0) return;

    @autoreleasepool
    {
        size_t nBytes = (size_t)nWidth * nHeight * 4;

        // ⚠️ COPIED, not referenced. drawRect: runs when AppKit decides, which
        // is not necessarily inside this call -- handing it the engine's live
        // buffer would race with the next frame's compositing and tear.
        if (g_pView->m_Width != nWidth || g_pView->m_Height != nHeight)
        {
            free(g_pView->m_pPixels);
            g_pView->m_pPixels = (unsigned char*)malloc(nBytes);
            g_pView->m_Width  = nWidth;
            g_pView->m_Height = nHeight;
        }
        if (g_pView->m_pPixels)
            memcpy(g_pView->m_pPixels, pRGBA, nBytes);

        [g_pView setNeedsDisplay:YES];
        [g_pView displayIfNeeded];
    }
}


void LTWindow_SetTitle(const char* pTitle)
{
    if (!g_bOpen || !g_pWindow || !pTitle) return;
    @autoreleasepool
    {
        [g_pWindow setTitle:[NSString stringWithUTF8String:pTitle]];

        // LT_TITLETRACE=1 -- what the WINDOW now says, read back from AppKit,
        // so a check does not depend on screen-recording permission.
        static const bool bTrace = (getenv("LT_TITLETRACE") != 0);
        if (bTrace)
        {
            printf("[WINDOW] title: %s\n", [[g_pWindow title] UTF8String]);
            fflush(stdout);
        }
    }
}


void LTWindow_PumpEvents()
{
    if (!g_bOpen) return;

    @autoreleasepool
    {
        // Drain everything pending without blocking. distantPast means "do not
        // wait" -- the engine owns the frame loop, not AppKit.
        for (;;)
        {
            NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;

            // ⚠️ LT_KEYTRACE=1 -- what AppKit actually hands us, BEFORE
            // dispatch. This separates "the OS never sent a key event" from
            // "it did and the responder chain swallowed it" -- two completely
            // different bugs that both look like a dead keyboard.
            if (getenv("LT_KEYTRACE"))
            {
                const NSEventType t = [ev type];
                if (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp ||
                    t == NSEventTypeFlagsChanged)
                {
                    static int n = 0;
                    if (n++ < 20)
                    {
                        NSResponder* fr = [g_pWindow firstResponder];
                        printf("[WIN] key NSEvent type=%d keyCode=%d  "
                               "windowIsKey=%d appActive=%d firstResponder=%s\n",
                               (int)t,
                               (t == NSEventTypeFlagsChanged) ? -1 : (int)[ev keyCode],
                               (int)[g_pWindow isKeyWindow],
                               (int)[NSApp isActive],
                               fr ? [NSStringFromClass([fr class]) UTF8String] : "(none)");
                        fflush(stdout);
                    }
                }
            }

            [NSApp sendEvent:ev];
        }
    }
}


bool LTWindow_ShouldClose()
{
    return g_bShouldClose;
}


void* LTWindow_GetContentView()
{
    return g_bOpen ? (__bridge void*)g_pView : 0;
}


static bool g_bMouseLook = false;      // the pointer IS captured
static bool g_bWantLook  = false;      // the game WANTS it (CM_None)

static void LTWindow_CaptureNow(bool bOn);

// Captures only while the window has focus; the wish survives focus changes.
static void LTWindow_ApplyMouseLook(bool bFocused)
{
    LTWindow_CaptureNow(g_bWantLook && bFocused && g_bOpen);
}

void LTWindow_SetMouseLook(bool bOn)
{
    g_bWantLook = bOn;
    const bool bKey = g_bOpen && g_pWindow && [g_pWindow isKeyWindow] && [NSApp isActive];
    LTWindow_ApplyMouseLook(bKey);
}

static void LTWindow_CaptureNow(bool bOn)
{
    if (bOn == g_bMouseLook) return;
    g_bMouseLook = bOn;
    if (getenv("LT_LOOKTRACE"))
    {
        printf("[LOOK] pointer %s (game wants %s)\n", bOn ? "CAPTURED" : "released",
               g_bWantLook ? "it" : "a cursor");
        fflush(stdout);
    }

    if (bOn)
    {
        CGDisplayHideCursor(kCGDirectMainDisplay);
        // Detach the cursor from the mouse: motion keeps arriving as deltas
        // while the pointer stays where it is, so it can never leave the
        // window or hit a screen edge mid-turn.
        CGAssociateMouseAndMouseCursorPosition(false);
    }
    else
    {
        CGAssociateMouseAndMouseCursorPosition(true);
        CGDisplayShowCursor(kCGDirectMainDisplay);
    }
}

bool LTWindow_GetMouseLook() { return g_bWantLook; }


float LTWindow_ScreenAspect()
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        NSScreen* scr = [NSScreen mainScreen];
        if (!scr) return 0.0f;
        const NSRect f = [scr frame];
        return f.size.height > 0 ? (float)(f.size.width / f.size.height) : 0.0f;
    }
}


bool LTWindow_ScreenSize(int* pW, int* pH)
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        NSScreen* scr = [NSScreen mainScreen];
        if (!scr || !pW || !pH) return false;
        const NSRect f = [scr frame];
        *pW = (int)f.size.width;
        *pH = (int)f.size.height;
        return *pW > 0 && *pH > 0;
    }
}


bool LTWindow_IsFullScreen()
{
    if (!g_bOpen || !g_pWindow) return false;
    return ([g_pWindow styleMask] & NSWindowStyleMaskFullScreen) != 0;
}


void LTWindow_SetPointMapper(LTWindowPointMapFn fn) { g_fnPointMap = fn; }


void LTWindow_SetContentSize(int nWidth, int nHeight)
{
    if (!g_bOpen || !g_pWindow || nWidth <= 0 || nHeight <= 0) return;
    @autoreleasepool
    {
        const NSRect old = [g_pWindow frame];
        [g_pWindow setContentSize:NSMakeSize(nWidth, nHeight)];
        NSRect f = [g_pWindow frame];
        f.origin.x = old.origin.x + (old.size.width - f.size.width) * 0.5;
        f.origin.y = old.origin.y + (old.size.height - f.size.height) * 0.5;
        [g_pWindow setFrame:f display:YES];
    }
}


void LTWindow_Close()
{
    // ⚠️ FIRST, before anything can fail below. A hidden, detached cursor that
    // outlives the process is a broken desktop.
    LTWindow_SetMouseLook(false);

    if (!g_bOpen) return;

    @autoreleasepool
    {
        [g_pWindow setDelegate:nil];
        [g_pWindow close];
        g_pWindow = nil;

        if (g_pView)
        {
            free(g_pView->m_pPixels);
            g_pView->m_pPixels = 0;
            g_pView = nil;
        }
        g_pDelegate = nil;
    }

    g_bOpen = false;
    fprintf(stderr, "[LTWINDOW] closed\n");
}
