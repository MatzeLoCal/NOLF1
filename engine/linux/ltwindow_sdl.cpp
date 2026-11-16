// Linux port: ltwindow.h on SDL3 (replaces platform/ltwindow.mm).
// The frame is a top-down RGBA8888 buffer, uploaded to a streaming texture.

#include "ltwindow.h"
#include "ltinput.h"
#include "ltglwin.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    SDL_Window*   g_pWin = 0;
    SDL_GLContext g_Ctx  = 0;
    SDL_Window*   g_pHidden = 0;     // headless GPU runs
    SDL_GLContext g_HiddenCtx = 0;
    int  g_TexW = 0, g_TexH = 0;
    bool g_bOpen = false, g_bClose = false;
    bool g_bWantLook = false, g_bFocused = true;
    LTWindowPointMapFn g_fnMap = 0;
    Uint64 g_nLastTick = 0;
    double g_fPeriod = 0.0;

    bool InitVideo()
    {
        if (SDL_WasInit(SDL_INIT_VIDEO)) return true;
        return SDL_InitSubSystem(SDL_INIT_VIDEO);
    }

    // SDL scancode -> macOS virtual keycode, so LTInput_MacKeyToVK keeps
    // being the single table that knows the game's keys (physical positions).
    int ScanToMac(SDL_Scancode s)
    {
        switch (s)
        {
        case SDL_SCANCODE_A: return 0x00; case SDL_SCANCODE_B: return 0x0B; case SDL_SCANCODE_C: return 0x08;
        case SDL_SCANCODE_D: return 0x02; case SDL_SCANCODE_E: return 0x0E; case SDL_SCANCODE_F: return 0x03;
        case SDL_SCANCODE_G: return 0x05; case SDL_SCANCODE_H: return 0x04; case SDL_SCANCODE_I: return 0x22;
        case SDL_SCANCODE_J: return 0x26; case SDL_SCANCODE_K: return 0x28; case SDL_SCANCODE_L: return 0x25;
        case SDL_SCANCODE_M: return 0x2E; case SDL_SCANCODE_N: return 0x2D; case SDL_SCANCODE_O: return 0x1F;
        case SDL_SCANCODE_P: return 0x23; case SDL_SCANCODE_Q: return 0x0C; case SDL_SCANCODE_R: return 0x0F;
        case SDL_SCANCODE_S: return 0x01; case SDL_SCANCODE_T: return 0x11; case SDL_SCANCODE_U: return 0x20;
        case SDL_SCANCODE_V: return 0x09; case SDL_SCANCODE_W: return 0x0D; case SDL_SCANCODE_X: return 0x07;
        case SDL_SCANCODE_Y: return 0x10; case SDL_SCANCODE_Z: return 0x06;
        case SDL_SCANCODE_0: return 0x1D; case SDL_SCANCODE_1: return 0x12; case SDL_SCANCODE_2: return 0x13;
        case SDL_SCANCODE_3: return 0x14; case SDL_SCANCODE_4: return 0x15; case SDL_SCANCODE_5: return 0x17;
        case SDL_SCANCODE_6: return 0x16; case SDL_SCANCODE_7: return 0x1A; case SDL_SCANCODE_8: return 0x1C;
        case SDL_SCANCODE_9: return 0x19;
        case SDL_SCANCODE_RETURN: return 0x24; case SDL_SCANCODE_TAB: return 0x30;
        case SDL_SCANCODE_SPACE: return 0x31; case SDL_SCANCODE_BACKSPACE: return 0x33;
        case SDL_SCANCODE_ESCAPE: return 0x35;
        case SDL_SCANCODE_UP: return 0x7E; case SDL_SCANCODE_DOWN: return 0x7D;
        case SDL_SCANCODE_LEFT: return 0x7B; case SDL_SCANCODE_RIGHT: return 0x7C;
        case SDL_SCANCODE_PAGEUP: return 0x74; case SDL_SCANCODE_PAGEDOWN: return 0x79;
        case SDL_SCANCODE_HOME: return 0x73; case SDL_SCANCODE_END: return 0x77;
        case SDL_SCANCODE_F1: return 0x7A; case SDL_SCANCODE_F2: return 0x78; case SDL_SCANCODE_F3: return 0x63;
        case SDL_SCANCODE_F4: return 0x76; case SDL_SCANCODE_F5: return 0x60; case SDL_SCANCODE_F6: return 0x61;
        case SDL_SCANCODE_F7: return 0x62; case SDL_SCANCODE_F8: return 0x64; case SDL_SCANCODE_F9: return 0x65;
        case SDL_SCANCODE_F10: return 0x6D; case SDL_SCANCODE_F11: return 0x67; case SDL_SCANCODE_F12: return 0x6F;
        case SDL_SCANCODE_MINUS: return 0x1B; case SDL_SCANCODE_EQUALS: return 0x18;
        case SDL_SCANCODE_LEFTBRACKET: return 0x21; case SDL_SCANCODE_RIGHTBRACKET: return 0x1E;
        case SDL_SCANCODE_BACKSLASH: return 0x2A; case SDL_SCANCODE_SEMICOLON: return 0x29;
        case SDL_SCANCODE_APOSTROPHE: return 0x27; case SDL_SCANCODE_COMMA: return 0x2B;
        case SDL_SCANCODE_PERIOD: return 0x2F; case SDL_SCANCODE_SLASH: return 0x2C;
        case SDL_SCANCODE_GRAVE: return 0x32; case SDL_SCANCODE_NONUSBACKSLASH: return 0x0A;
        case SDL_SCANCODE_DELETE: return 0x75; case SDL_SCANCODE_INSERT: return 0x72;
        case SDL_SCANCODE_KP_0: return 0x52; case SDL_SCANCODE_KP_1: return 0x53; case SDL_SCANCODE_KP_2: return 0x54;
        case SDL_SCANCODE_KP_3: return 0x55; case SDL_SCANCODE_KP_4: return 0x56; case SDL_SCANCODE_KP_5: return 0x57;
        case SDL_SCANCODE_KP_6: return 0x58; case SDL_SCANCODE_KP_7: return 0x59; case SDL_SCANCODE_KP_8: return 0x5B;
        case SDL_SCANCODE_KP_9: return 0x5C; case SDL_SCANCODE_KP_PERIOD: return 0x41;
        case SDL_SCANCODE_KP_MULTIPLY: return 0x43; case SDL_SCANCODE_KP_PLUS: return 0x45;
        case SDL_SCANCODE_KP_MINUS: return 0x4E; case SDL_SCANCODE_KP_DIVIDE: return 0x4B;
        case SDL_SCANCODE_KP_ENTER: return 0x4C;
        default: return -1;
        }
    }

    // Modifiers through LTInput_MacModifierToVK, with the Mac keycode and the
    // device-dependent flag bit it tests (set when the key went down).
    int ModToVK(SDL_Scancode s, bool bDown, bool* pToggle)
    {
        int code = 0; unsigned long bit = 0;
        switch (s)
        {
        case SDL_SCANCODE_LSHIFT: code = 0x38; bit = 0x0002; break;
        case SDL_SCANCODE_RSHIFT: code = 0x3C; bit = 0x0004; break;
        case SDL_SCANCODE_LCTRL:  code = 0x3B; bit = 0x0001; break;
        case SDL_SCANCODE_RCTRL:  code = 0x3E; bit = 0x2000; break;
        case SDL_SCANCODE_LALT:   code = 0x3A; bit = 0x0020; break;
        case SDL_SCANCODE_RALT:   code = 0x3D; bit = 0x0040; break;
        case SDL_SCANCODE_CAPSLOCK: code = 0x39; break;
        default: return 0;
        }
        bool d = false;
        return LTInput_MacModifierToVK(code, bDown ? bit : 0, &d, pToggle);
    }

    void ApplyLook()
    {
        if (!g_pWin) return;
        SDL_SetWindowRelativeMouseMode(g_pWin, g_bWantLook && g_bFocused);
    }

    // Window coordinates (points) -> game surface pixels.
    void MapPoint(float fx, float fy, int* px, int* py)
    {
        int ww = 0, wh = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(g_pWin, &ww, &wh);
        SDL_GetWindowSizeInPixels(g_pWin, &pw, &ph);
        const float sc = (ww > 0) ? (float)pw / (float)ww : 1.0f;
        if (g_fnMap && g_fnMap(fx * sc, fy * sc, px, py)) return;
        if (ww > 0 && wh > 0 && g_TexW > 0 && g_TexH > 0)
        { fx = fx * g_TexW / ww; fy = fy * g_TexH / wh; }
        *px = (int)fx; *py = (int)fy;
    }

    void PushMouse(int kind, float x, float y, int btn, float dx = 0, float dy = 0)
    {
        LTInputEvent ev; memset(&ev, 0, sizeof(ev));
        ev.m_Kind = kind; ev.m_Button = btn;
        MapPoint(x, y, &ev.m_X, &ev.m_Y);
        ev.m_DX = (int)dx; ev.m_DY = (int)dy;
        LTInput_Push(ev);
    }

    int SdlBtn(Uint8 b) { return b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_RIGHT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : -1; }
}

bool LTWindow_Wanted()
{
    static int s = -1;
    if (s < 0) { const char* p = getenv("LT_WINDOW"); s = (p && p[0] && p[0] != '0') ? 1 : 0; }
    return s != 0;
}

bool LTWindow_Open(int w, int h, const char* pTitle)
{
    if (!LTWindow_Wanted()) return false;
    if (g_bOpen) return true;
    if (w <= 0 || h <= 0 || !InitVideo()) { fprintf(stderr, "[LTWINDOW] SDL: %s\n", SDL_GetError()); return false; }

    int ww = w, wh = h;
    if (SDL_DisplayID d = SDL_GetPrimaryDisplay())
    {
        SDL_Rect r;
        if (SDL_GetDisplayUsableBounds(d, &r) && r.w > 0 && r.h > 0)
        {
            float k = SDL_min(1.0f, SDL_min(r.w * 0.92f / w, (r.h - 32) * 0.92f / h));
            ww = (int)(w * k + 0.5f); wh = (int)(h * k + 0.5f);
        }
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);       // every 3D pass has its own target
    g_pWin = SDL_CreateWindow(pTitle ? pTitle : "NOLF", ww, wh,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_OPENGL);
    if (!g_pWin) { fprintf(stderr, "[LTWINDOW] SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
    g_Ctx = SDL_GL_CreateContext(g_pWin);
    if (!g_Ctx) { fprintf(stderr, "[LTWINDOW] OpenGL 4.6 context: %s\n", SDL_GetError());
                  SDL_DestroyWindow(g_pWin); g_pWin = 0; return false; }
    SDL_GL_MakeCurrent(g_pWin, g_Ctx);
    // Adaptive vsync: a frame that missed the refresh is shown at once instead
    // of waiting a whole period more (60 -> 30 -> 20 FPS steps otherwise).
    if (getenv("LT_NOVSYNC")) SDL_GL_SetSwapInterval(0);
    else if (!SDL_GL_SetSwapInterval(-1))
    { fprintf(stderr, "[LTWINDOW] no adaptive vsync (%s) -- plain vsync\n", SDL_GetError()); SDL_GL_SetSwapInterval(1); }
    SDL_StartTextInput(g_pWin);

    g_bOpen = true; g_bClose = false;
    fprintf(stderr, "[LTWINDOW] %dx%d open (SDL3, OpenGL 4.6 core)\n", w, h);
    return true;
}

// LT_SWAPTRACE=1: swaps per second, by site (two swaps a frame halve the rate under vsync).
static void SwapTrace(int site)
{
    static const bool b = getenv("LT_SWAPTRACE") != 0;
    if (!b) return;
    static int n[2] = {0, 0}; static Uint64 t0 = 0;
    ++n[site];
    const Uint64 t = SDL_GetTicksNS();
    if (!t0) t0 = t;
    if (t - t0 > 2000000000ull)
    { fprintf(stderr, "[SWAP] %.1f/s present(blit) %.1f/s gl\n", n[0] / ((t - t0) * 1e-9), n[1] / ((t - t0) * 1e-9)); n[0] = n[1] = 0; t0 = t; }
}

// ⚠️ ONE SWAP A FRAME. On the Mac the GPU frame is its own layer above the
// software view, so a CPU present underneath it is harmless. Here both go to
// the same back buffer: the walk view's GL frame was swapped and then the
// (empty) software surface blitted and swapped over it -- two vblank waits a
// frame, 30 fps under vsync. A GL frame since the last present wins.
static bool g_bGLSwapped = false;

void LTWindow_Present(const unsigned char* p, int w, int h)
{
    if (g_bGLSwapped) { g_bGLSwapped = false; return; }
    SwapTrace(0);
    if (!g_bOpen || !p || w <= 0 || h <= 0) return;
    g_TexW = w; g_TexH = h;                 // the mouse maps to this surface
    SDL_GL_MakeCurrent(g_pWin, g_Ctx);
    int ow = 0, oh = 0;
    SDL_GetWindowSizeInPixels(g_pWin, &ow, &oh);
    if (LTGL_BlitRGBA(p, w, h, ow, oh)) SDL_GL_SwapWindow(g_pWin);   // with vsync, the pacing
    const Uint64 t = SDL_GetTicksNS();
    if (g_nLastTick) { double d = (t - g_nLastTick) * 1e-9; if (d > 0.004 && d < 0.05) g_fPeriod = d; }
    g_nLastTick = t;
}

// ---- the GL context (ltglwin.h) ------------------------------------------ //

bool LTGLWin_HasWindow() { return g_bOpen && g_Ctx; }

bool LTGLWin_EnsureContext(bool bAllowHidden)
{
    if (g_bOpen && g_Ctx) return SDL_GL_MakeCurrent(g_pWin, g_Ctx);
    if (g_HiddenCtx) return SDL_GL_MakeCurrent(g_pHidden, g_HiddenCtx);
    if (!bAllowHidden || !InitVideo()) return false;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    g_pHidden = SDL_CreateWindow("NOLF (GL offscreen)", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g_pHidden) return false;
    g_HiddenCtx = SDL_GL_CreateContext(g_pHidden);
    if (!g_HiddenCtx) { SDL_DestroyWindow(g_pHidden); g_pHidden = 0; return false; }
    return SDL_GL_MakeCurrent(g_pHidden, g_HiddenCtx);
}

void LTGLWin_Swap()
{
    if (!g_bOpen || !g_Ctx) return;
    SwapTrace(1);
    SDL_GL_SwapWindow(g_pWin);
    g_bGLSwapped = true;
    const Uint64 t = SDL_GetTicksNS();
    if (g_nLastTick) { double d = (t - g_nLastTick) * 1e-9; if (d > 0.004 && d < 0.05) g_fPeriod = d; }
    g_nLastTick = t;
}

bool LTGLWin_DrawableSize(int* pW, int* pH)
{
    if (!g_bOpen || !pW || !pH) return false;
    SDL_GetWindowSizeInPixels(g_pWin, pW, pH);
    return *pW > 0 && *pH > 0;
}

void* LTGLWin_GetProc(const char* pName) { return (void*)SDL_GL_GetProcAddress(pName); }

void LTWindow_PumpEvents()
{
    if (!g_bOpen) return;
    // LT_AUTOCLOSE=<seconds>: the close button, pressed by the clock (tests).
    static const double s_fAuto = getenv("LT_AUTOCLOSE") ? atof(getenv("LT_AUTOCLOSE")) : 0.0;
    static const Uint64 s_t0 = SDL_GetTicksNS();
    if (s_fAuto > 0.0 && (SDL_GetTicksNS() - s_t0) * 1e-9 > s_fAuto) g_bClose = true;
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        switch (e.type)
        {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: g_bClose = true; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        {
            g_bFocused = false; ApplyLook();
            LTInputEvent ev; memset(&ev, 0, sizeof(ev)); ev.m_Kind = LTInputEvent::kFocusLost; LTInput_Push(ev);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED: g_bFocused = true; ApplyLook(); break;
        case SDL_EVENT_MOUSE_MOTION:
        {
            const SDL_MouseButtonFlags b = e.motion.state;
            const int btn = (b & SDL_BUTTON_RMASK) ? 1 : (b & SDL_BUTTON_MMASK) ? 2 : 0;
            PushMouse(LTInputEvent::kMouseMove, e.motion.x, e.motion.y, btn, e.motion.xrel, e.motion.yrel);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            const int btn = SdlBtn(e.button.button);
            if (btn < 0) break;
            int kind = (e.type == SDL_EVENT_MOUSE_BUTTON_UP) ? LTInputEvent::kMouseUp
                     : (e.button.clicks >= 2 && btn < 2) ? LTInputEvent::kMouseDblClick : LTInputEvent::kMouseDown;
            PushMouse(kind, e.button.x, e.button.y, btn);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
        {
            static float acc = 0.0f;
            float dy = e.wheel.y;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
            acc += dy;
            if (dy != 0 && acc > -1 && acc < 1) acc = dy > 0 ? 1.0f : -1.0f;  // a small step still counts once
            while (acc >= 1.0f || acc <= -1.0f)
            {
                LTInputEvent ev; memset(&ev, 0, sizeof(ev));
                ev.m_Kind = LTInputEvent::kWheel; ev.m_DY = acc > 0 ? 1 : -1;
                acc -= (float)ev.m_DY; LTInput_Push(ev);
            }
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            const bool bDown = (e.type == SDL_EVENT_KEY_DOWN);
            // Alt+Enter / F11-less fullscreen toggle, taken before the game.
            if (bDown && e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))
            { SDL_SetWindowFullscreen(g_pWin, !LTWindow_IsFullScreen()); break; }
            LTInputEvent ev; memset(&ev, 0, sizeof(ev));
            ev.m_Kind = bDown ? LTInputEvent::kKeyDown : LTInputEvent::kKeyUp;
            bool bToggle = false;
            if (int vk = ModToVK(e.key.scancode, bDown, &bToggle))
            {
                if (e.key.repeat || (bToggle && !bDown)) break;
                ev.m_Code = vk;
                if (bToggle) { ev.m_Kind = LTInputEvent::kKeyDown; LTInput_Push(ev); ev.m_Kind = LTInputEvent::kKeyUp; }
                LTInput_Push(ev);
                break;
            }
            const int mac = ScanToMac(e.key.scancode);
            if (mac < 0) break;
            ev.m_Code = LTInput_MacKeyToVK(mac);
            ev.m_Repeat = e.key.repeat ? 1 : 0;
            if (ev.m_Code) LTInput_Push(ev);
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            for (const unsigned char* c = (const unsigned char*)e.text.text; c && *c; ++c)
                if (*c >= 32 && *c < 127)
                {
                    LTInputEvent ce; memset(&ce, 0, sizeof(ce));
                    ce.m_Kind = LTInputEvent::kChar; ce.m_Code = *c; LTInput_Push(ce);
                }
            break;
        default: break;
        }
    }
}

bool LTWindow_ShouldClose() { return g_bClose; }
void* LTWindow_GetContentView() { return g_bOpen ? (void*)g_pWin : 0; }

void LTWindow_SetTitle(const char* p)
{
    if (!g_bOpen || !p) return;
    SDL_SetWindowTitle(g_pWin, p);
    static const bool bTrace = getenv("LT_TITLETRACE") != 0;
    if (bTrace) { printf("[WINDOW] title: %s\n", SDL_GetWindowTitle(g_pWin)); fflush(stdout); }
}

void LTWindow_SetMouseLook(bool bOn)
{
    g_bWantLook = bOn;
    ApplyLook();
    if (getenv("LT_LOOKTRACE")) { printf("[LOOK] game wants %s\n", bOn ? "it" : "a cursor"); fflush(stdout); }
}
bool LTWindow_GetMouseLook() { return g_bWantLook; }

float LTWindow_ScreenAspect()
{
    int w = 0, h = 0;
    return (LTWindow_ScreenSize(&w, &h) && h > 0) ? (float)w / (float)h : 0.0f;
}

bool LTWindow_ScreenSize(int* pW, int* pH)
{
    if (!pW || !pH || !InitVideo()) return false;
    SDL_DisplayID d = g_pWin ? SDL_GetDisplayForWindow(g_pWin) : SDL_GetPrimaryDisplay();
    SDL_Rect r;
    if (!d || !SDL_GetDisplayBounds(d, &r)) return false;
    *pW = r.w; *pH = r.h;
    return r.w > 0 && r.h > 0;
}

bool LTWindow_IsFullScreen()
{
    return g_bOpen && (SDL_GetWindowFlags(g_pWin) & SDL_WINDOW_FULLSCREEN) != 0;
}

void LTWindow_SetPointMapper(LTWindowPointMapFn fn) { g_fnMap = fn; }

void LTWindow_SetContentSize(int w, int h)
{
    if (!g_bOpen || w <= 0 || h <= 0) return;
    SDL_SetWindowSize(g_pWin, w, h);
    SDL_SetWindowPosition(g_pWin, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

// Pacing comes from the vsynced SDL_RenderPresent, so nothing to wait for here:
// false tells the host to pace itself only when vsync is off.
bool LTWindow_WaitForRefresh() { return g_bOpen && !getenv("LT_NOVSYNC"); }
double LTWindow_RefreshInterval() { return g_fPeriod; }

void LTWindow_Close()
{
    LTWindow_SetMouseLook(false);
    if (!g_bOpen) return;
    if (g_Ctx) SDL_GL_DestroyContext(g_Ctx);
    SDL_DestroyWindow(g_pWin);
    g_Ctx = 0; g_pWin = 0; g_TexW = g_TexH = 0;
    g_bOpen = false;
    fprintf(stderr, "[LTWINDOW] closed\n");
}
