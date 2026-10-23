// ----------------------------------------------------------------------- //
//
// MODULE  : windows.h  (macOS compatibility)
//
// PURPOSE : Enough of the Win32 API for the original NOLF 1 sources to compile
//           unmodified on macOS / arm64.
//
// SCOPE   : Deliberately NOT a general Win32 emulation. Every declaration here
//           corresponds to something the NOLF 1 game code or LT2 lithshared
//           actually references, established by census rather than by guessing.
//
// RULE    : Anything that cannot be implemented honestly must announce itself
//           when it runs -- see LTCOMPAT_UNIMPLEMENTED below. A stub that fails
//           silently is how a port loses weeks to an invisible dead path.
//
// ----------------------------------------------------------------------- //

#ifndef __LTCOMPAT_WINDOWS_H__
#define __LTCOMPAT_WINDOWS_H__

// The real Win32 windows.h defines _WINDOWS_, and the original LT2 sources
// test it -- e.g. mfcstub's mfcs_point.h / mfcs_rect.h define their own POINT
// and RECT only `#ifndef _WINDOWS_`. Defining it here reproduces the exact
// contract those sources were written against, so they defer to the
// definitions below just as they would on Windows.
#ifndef _WINDOWS_
#define _WINDOWS_
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------- //
// Loud stub reporting.
//
// Fires once per call site, so a hot path cannot flood the log while still
// making the FIRST occurrence impossible to miss.
// ----------------------------------------------------------------------- //

void LTCompat_ReportUnimplemented(const char* fn, const char* file, int line);

#define LTCOMPAT_UNIMPLEMENTED()                                        \
    do {                                                                \
        static int _ltc_reported = 0;                                   \
        if (!_ltc_reported) {                                           \
            _ltc_reported = 1;                                          \
            LTCompat_ReportUnimplemented(__func__, __FILE__, __LINE__); \
        }                                                               \
    } while (0)

// ----------------------------------------------------------------------- //
// Fixed-width base types.
//
// These use exact-width types deliberately. On Win32 DWORD is 32-bit because
// `unsigned long` was 32-bit there; spelling it uint32_t preserves the authored
// width under LP64, which is the same reasoning applied to int32/uint32 in
// LT2/sdk/inc/ltbasetypes.h (see Docs/DEVIATIONS.md, D-001).
// ----------------------------------------------------------------------- //

typedef uint8_t             BYTE;
typedef uint16_t            WORD;
typedef uint32_t            DWORD;
typedef int32_t             LONG;
typedef uint32_t            ULONG;
typedef int                 BOOL;           // Win32 BOOL is `int`, not bool
typedef unsigned int        UINT;
typedef int                 INT;
typedef float               FLOAT;
typedef void*               LPVOID;
typedef const void*         LPCVOID;

typedef char                CHAR;
typedef char*               LPSTR;
typedef const char*         LPCSTR;
typedef char*               LPTSTR;
typedef const char*         LPCTSTR;

typedef intptr_t            LONG_PTR;
typedef uintptr_t           ULONG_PTR;
typedef intptr_t            DWORD_PTR;

typedef intptr_t            INT_PTR;
typedef uintptr_t           UINT_PTR;

// ⚠️ WPARAM/LPARAM/LRESULT are POINTER-WIDTH on Win64 and are used to carry
// pointers through the window procedure. Making them 32-bit here would
// reproduce exactly the truncated-pointer class of bug that cost the NOLF 2
// port real time. Keep them pointer-width.
typedef uintptr_t           WPARAM;
typedef intptr_t            LPARAM;
typedef intptr_t            LRESULT;

#ifndef TRUE
  #define TRUE  1
  #define FALSE 0
#endif

#ifndef NULL
  #define NULL 0
#endif

#define MAX_PATH        260
#define _MAX_PATH       260   // MSVC spelling; the originals use both
#define WINAPI
#define APIENTRY
#define CALLBACK
#define WINAPIV
#define __cdecl
#define far
#define near
#define FAR
#define NEAR

// ----------------------------------------------------------------------- //
// Handles. Opaque pointers -- never integers, so truncation is impossible.
// ----------------------------------------------------------------------- //

typedef void*               HANDLE;
typedef void*               HWND;
typedef void*               HINSTANCE;
typedef void*               HMODULE;
typedef void*               HDC;
typedef void*               HKEY;
typedef void*               HFONT;
typedef void*               HBITMAP;
typedef void*               HICON;
typedef void*               HCURSOR;
typedef void*               HMENU;
typedef void*               HGLOBAL;

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

// ----------------------------------------------------------------------- //
// Simple structures.
// ----------------------------------------------------------------------- //

typedef struct tagPOINT {
    LONG x;
    LONG y;
} POINT, *LPPOINT;

typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *LPRECT;

typedef struct tagMSG {
    HWND    hwnd;
    UINT    message;
    WPARAM  wParam;
    LPARAM  lParam;
    DWORD   time;
    POINT   pt;
} MSG, *LPMSG;

typedef struct _GUID {
    DWORD  Data1;
    WORD   Data2;
    WORD   Data3;
    BYTE   Data4[8];
} GUID;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

// ----------------------------------------------------------------------- //
// Window messages.
//
// ⚠️ NOLF takes ALL mouse input through a WndProc it installs itself --
// IClientShell has no mouse callbacks at all. WM_MOUSEMOVE drives the cursor
// and every rollover; WM_LBUTTONDOWN clicks menu items. The macOS window layer
// must SYNTHESISE these from NSEvent into the game's proc, and must also set
// setAcceptsMouseMovedEvents:YES -- AppKit suppresses plain mouse-moved events
// without it, which yields working clicks and dead rollover.
// ----------------------------------------------------------------------- //

#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_MOVE             0x0003
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_PAINT            0x000F
#define WM_CLOSE            0x0010
#define WM_QUIT             0x0012
#define WM_ACTIVATEAPP      0x001C
#define WM_SETCURSOR        0x0020
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_SYSKEYDOWN       0x0104
#define WM_SYSKEYUP         0x0105
#define WM_COMMAND          0x0111
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_LBUTTONDBLCLK    0x0203
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_MBUTTONDOWN      0x0207
#define WM_MBUTTONUP        0x0208
#define WM_MOUSEWHEEL       0x020A

#define GWL_WNDPROC         (-4)
#define GWL_USERDATA        (-21)

#define MB_OK               0x00000000
#define MB_OKCANCEL         0x00000001
#define MB_YESNO            0x00000004
#define MB_ICONERROR        0x00000010
#define MB_ICONQUESTION     0x00000020
#define MB_ICONWARNING      0x00000030
#define MB_ICONINFORMATION  0x00000040
#define IDOK                1
#define IDCANCEL            2
#define IDYES               6
#define IDNO                7

// ----------------------------------------------------------------------- //
// String / CRT compatibility.
//
// This is by far the largest slice of the measured surface: _stricmp and
// stricmp alone account for 647 of the call sites, and both are plain
// strcasecmp.
// ----------------------------------------------------------------------- //

#define _stricmp    strcasecmp
#define stricmp     strcasecmp
#define _strcmpi    strcasecmp
#define strcmpi     strcasecmp
#define _strnicmp   strncasecmp
#define strnicmp    strncasecmp

#define _snprintf   snprintf
#define _vsnprintf  vsnprintf
#define _unlink     unlink
#define _getcwd     getcwd
#define _chdir      chdir
#define _access     access

char* _strupr(char* s);
char* _strlwr(char* s);
#define strupr _strupr
#define strlwr _strlwr

int   _mkdir(const char* path);
void  _splitpath(const char* path, char* drive, char* dir, char* fname, char* ext);
void  _makepath(char* path, const char* drive, const char* dir,
                const char* fname, const char* ext);

#define _MAX_DRIVE  3
#define _MAX_DIR    256
#define _MAX_FNAME  256
#define _MAX_EXT    256

int   wsprintf(char* buffer, const char* format, ...);
char* lstrcpy(char* dst, const char* src);
char* lstrcat(char* dst, const char* src);
int   lstrlen(const char* s);
int   lstrcmp(const char* a, const char* b);
int   lstrcmpi(const char* a, const char* b);

#define ZeroMemory(dst, len)        memset((dst), 0, (len))
#define CopyMemory(dst, src, len)   memcpy((dst), (src), (len))
#define FillMemory(dst, len, fill)  memset((dst), (fill), (len))

// ----------------------------------------------------------------------- //
// Time.
// ----------------------------------------------------------------------- //

DWORD GetTickCount(void);

// Installs an engine-supplied millisecond source for GetTickCount, so the
// fixed-timestep clock governs BOTH clocks. See windows_compat.c.
void  LTCompat_SetTickSource(DWORD (*pfn)(void));
void  Sleep(DWORD milliseconds);

// ----------------------------------------------------------------------- //
// Debug output.
// ----------------------------------------------------------------------- //

void OutputDebugStringA(const char* s);
#define OutputDebugString OutputDebugStringA

// ----------------------------------------------------------------------- //
// File I/O.
//
// The game code uses the Win32 handle API in only a handful of places; these
// map onto POSIX file descriptors carried inside the HANDLE.
// ----------------------------------------------------------------------- //

#define GENERIC_READ        0x80000000
#define GENERIC_WRITE       0x40000000
#define FILE_SHARE_READ     0x00000001
#define FILE_SHARE_WRITE    0x00000002
#define CREATE_NEW          1
#define CREATE_ALWAYS       2
#define OPEN_EXISTING       3
#define OPEN_ALWAYS         4
#define TRUNCATE_EXISTING   5
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_BEGIN          0
#define FILE_CURRENT        1
#define FILE_END            2

HANDLE CreateFileA(const char* name, DWORD access, DWORD share,
                   void* security, DWORD creation, DWORD flags, HANDLE tmpl);
BOOL   ReadFile(HANDLE h, LPVOID buf, DWORD toRead, DWORD* bytesRead, void* ov);
BOOL   WriteFile(HANDLE h, LPCVOID buf, DWORD toWrite, DWORD* bytesWritten, void* ov);
BOOL   CloseHandle(HANDLE h);
DWORD  SetFilePointer(HANDLE h, LONG dist, LONG* distHigh, DWORD method);
DWORD  GetFileSize(HANDLE h, DWORD* sizeHigh);
BOOL   CreateDirectoryA(const char* path, void* security);
DWORD  GetFileAttributesA(const char* path);
BOOL   DeleteFileA(const char* path);

#define CreateFile          CreateFileA
#define CreateDirectory     CreateDirectoryA
#define GetFileAttributes   GetFileAttributesA
#define DeleteFile          DeleteFileA

#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010

// ----------------------------------------------------------------------- //
// INI files.  NOLF 1 touches these in exactly two places.
// ----------------------------------------------------------------------- //

DWORD GetPrivateProfileStringA(const char* app, const char* key,
                               const char* def, char* out, DWORD size,
                               const char* file);
BOOL  WritePrivateProfileStringA(const char* app, const char* key,
                                 const char* value, const char* file);

#define GetPrivateProfileString   GetPrivateProfileStringA
#define WritePrivateProfileString WritePrivateProfileStringA

// ----------------------------------------------------------------------- //
// Window / input.
//
// Declared here, implemented by the Cocoa layer. Until that exists these are
// loud stubs -- see the WndProc note above.
// ----------------------------------------------------------------------- //

BOOL    GetWindowRect(HWND h, LPRECT r);
BOOL    GetClientRect(HWND h, LPRECT r);
BOOL    ClientToScreen(HWND h, LPPOINT p);
BOOL    ScreenToClient(HWND h, LPPOINT p);
BOOL    GetCursorPos(LPPOINT p);
BOOL    SetCursorPos(int x, int y);
int     ShowCursor(BOOL show);
short   GetAsyncKeyState(int vk);
short   GetKeyState(int vk);
LRESULT SendMessageA(HWND h, UINT msg, WPARAM w, LPARAM l);
BOOL    PostMessageA(HWND h, UINT msg, WPARAM w, LPARAM l);
LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM w, LPARAM l);
LRESULT CallWindowProcA(WNDPROC prev, HWND h, UINT msg, WPARAM w, LPARAM l);
intptr_t SetWindowLongA(HWND h, int index, intptr_t value);
intptr_t GetWindowLongA(HWND h, int index);
int     MessageBoxA(HWND h, const char* text, const char* caption, UINT type);

#define SendMessage     SendMessageA
#define PostMessage     PostMessageA
#define DefWindowProc   DefWindowProcA
#define CallWindowProc  CallWindowProcA
#define SetWindowLong   SetWindowLongA
#define GetWindowLong   GetWindowLongA
#define MessageBox      MessageBoxA

// ----------------------------------------------------------------------- //
// Modules.
// ----------------------------------------------------------------------- //

HMODULE GetModuleHandleA(const char* name);
void*   GetProcAddress(HMODULE m, const char* name);
HMODULE LoadLibraryA(const char* name);
BOOL    FreeLibrary(HMODULE m);
DWORD   GetModuleFileNameA(HMODULE m, char* out, DWORD size);

#define GetModuleHandle     GetModuleHandleA
#define LoadLibrary         LoadLibraryA
#define GetModuleFileName   GetModuleFileNameA

DWORD GetLastError(void);
void  SetLastError(DWORD err);


// ----------------------------------------------------------------------- //
// Virtual key codes.
//
// The real Win32 windows.h supplies these, which is why NOLF's own VKDefs.h
// hides its copies behind `#ifdef DEFINE_WINDOWS_VIRTUAL_KEYS` -- that block
// stayed OFF on Windows. Providing them here reproduces that exactly, so the
// escape hatch can stay off and every file gets one consistent set.
// Values are the genuine Win32 ones.
// ----------------------------------------------------------------------- //

#define VK_LBUTTON   0x01
#define VK_RBUTTON   0x02
#define VK_CANCEL    0x03
#define VK_MBUTTON   0x04
#define VK_BACK      0x08
#define VK_TAB       0x09
#define VK_CLEAR     0x0C
#define VK_RETURN    0x0D
#define VK_SHIFT     0x10
#define VK_CONTROL   0x11
#define VK_MENU      0x12
#define VK_PAUSE     0x13
#define VK_CAPITAL   0x14
#define VK_ESCAPE    0x1B
#define VK_SPACE     0x20
#define VK_PRIOR     0x21
#define VK_NEXT      0x22
#define VK_END       0x23
#define VK_HOME      0x24
#define VK_LEFT      0x25
#define VK_UP        0x26
#define VK_RIGHT     0x27
#define VK_DOWN      0x28
#define VK_SELECT    0x29
#define VK_PRINT     0x2A
#define VK_EXECUTE   0x2B
#define VK_SNAPSHOT  0x2C
#define VK_INSERT    0x2D
#define VK_DELETE    0x2E
#define VK_HELP      0x2F

#define VK_0 0x30
#define VK_1 0x31
#define VK_2 0x32
#define VK_3 0x33
#define VK_4 0x34
#define VK_5 0x35
#define VK_6 0x36
#define VK_7 0x37
#define VK_8 0x38
#define VK_9 0x39

#define VK_A 0x41
#define VK_B 0x42
#define VK_C 0x43
#define VK_D 0x44
#define VK_E 0x45
#define VK_F 0x46
#define VK_G 0x47
#define VK_H 0x48
#define VK_I 0x49
#define VK_J 0x4A
#define VK_K 0x4B
#define VK_L 0x4C
#define VK_M 0x4D
#define VK_N 0x4E
#define VK_O 0x4F
#define VK_P 0x50
#define VK_Q 0x51
#define VK_R 0x52
#define VK_S 0x53
#define VK_T 0x54
#define VK_U 0x55
#define VK_V 0x56
#define VK_W 0x57
#define VK_X 0x58
#define VK_Y 0x59
#define VK_Z 0x5A

#define VK_LWIN      0x5B
#define VK_RWIN      0x5C
#define VK_APPS      0x5D
#define VK_NUMPAD0   0x60
#define VK_NUMPAD1   0x61
#define VK_NUMPAD2   0x62
#define VK_NUMPAD3   0x63
#define VK_NUMPAD4   0x64
#define VK_NUMPAD5   0x65
#define VK_NUMPAD6   0x66
#define VK_NUMPAD7   0x67
#define VK_NUMPAD8   0x68
#define VK_NUMPAD9   0x69
#define VK_MULTIPLY  0x6A
#define VK_ADD       0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT  0x6D
#define VK_DECIMAL   0x6E
#define VK_DIVIDE    0x6F

#define VK_F1  0x70
#define VK_F2  0x71
#define VK_F3  0x72
#define VK_F4  0x73
#define VK_F5  0x74
#define VK_F6  0x75
#define VK_F7  0x76
#define VK_F8  0x77
#define VK_F9  0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87

#define VK_NUMLOCK   0x90
#define VK_SCROLL    0x91
#define VK_LSHIFT    0xA0
#define VK_RSHIFT    0xA1
#define VK_LCONTROL  0xA2
#define VK_RCONTROL  0xA3
#define VK_LMENU     0xA4
#define VK_RMENU     0xA5
#define VK_PROCESSKEY 0xE5
#define VK_ATTN      0xF6
#define VK_CRSEL     0xF7
#define VK_EXSEL     0xF8
#define VK_EREOF     0xF9
#define VK_PLAY      0xFA
#define VK_ZOOM      0xFB
#define VK_NONAME    0xFC
#define VK_PA1       0xFD
#define VK_OEM_CLEAR 0xFE

// Listbox / misc return codes referenced by the UI code.
#define LB_ERR       (-1)
#define WM_SETREDRAW 0x000B


// ----------------------------------------------------------------------- //
// Misc Win32 surface referenced by the client UI / input code.
// ----------------------------------------------------------------------- //

#define INFINITE  0xFFFFFFFF
#define WAIT_OBJECT_0 0

BOOL   ClipCursor(const RECT* r);
HANDLE CreateEventA(void* attrs, BOOL manualReset, BOOL initialState, const char* name);
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
int    GetWindowTextA(HWND h, char* out, int maxCount);
BOOL   DeleteObject(HANDLE obj);

#define CreateEvent    CreateEventA
#define GetWindowText  GetWindowTextA


// ----------------------------------------------------------------------- //
// Remaining Win32 surface referenced by the client shell.
// ----------------------------------------------------------------------- //

typedef short               SHORT;
typedef unsigned short      USHORT;
typedef void*               HRSRC;
typedef void*               HKL;
typedef void*               HGDIOBJ;
typedef int                 HFILE;
typedef int (*FARPROC)(void);

typedef struct _OFSTRUCT {
    BYTE cBytes;
    BYTE fFixedDisk;
    WORD nErrCode;
    WORD Reserved1;
    WORD Reserved2;
    char szPathName[128];
} OFSTRUCT;

#define OF_EXIST                    0x00004000
#define HFILE_ERROR                 (-1)

#define WAIT_TIMEOUT                0x00000102
#define SEM_FAILCRITICALERRORS      0x0001
#define SEM_NOOPENFILEERRORBOX      0x8000

#define WS_VISIBLE                  0x10000000
#define SWP_FRAMECHANGED            0x0020
#define WM_RBUTTONDBLCLK            0x0206

#define LB_RESETCONTENT             0x0184

#define MAKEINTRESOURCE(i)          ((const char*)(uintptr_t)((WORD)(i)))

BOOL   UpdateWindow(HWND h);
BOOL   SetWindowTextA(HWND h, const char* text);
UINT   SetErrorMode(UINT mode);
void*  LockResource(void* res);
HFILE  OpenFile(const char* name, OFSTRUCT* rs, UINT style);

#define SetWindowText SetWindowTextA


// Listbox messages, window styles, resource + thread + keyboard-layout API.
#define LB_ADDSTRING                0x0180
#define LB_DELETESTRING             0x0182
#define LB_GETTEXT                  0x0189
#define LB_GETCURSEL                0x0188
#define LB_SETCURSEL                0x0186
#define LB_GETCOUNT                 0x018B

#define GWL_STYLE                   (-16)
#define GWL_EXSTYLE                 (-20)
#define HWND_TOPMOST                ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST              ((HWND)(intptr_t)-2)
#define SWP_NOSIZE                  0x0001
#define SWP_NOMOVE                  0x0002
#define SWP_NOZORDER                0x0004
#define SWP_SHOWWINDOW              0x0040

BOOL   InvalidateRect(HWND h, const RECT* r, BOOL erase);
BOOL   SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags);
int    MapWindowPoints(HWND from, HWND to, LPPOINT pts, UINT count);
HRSRC  FindResourceA(HMODULE m, const char* name, const char* type);
HGLOBAL LoadResource(HMODULE m, HRSRC res);
HKL    GetKeyboardLayout(DWORD threadId);
UINT   MapVirtualKeyEx(UINT code, UINT mapType, HKL layout);
HANDLE CreateThread(void* attrs, size_t stackSize,
                    DWORD (*start)(void*), void* param, DWORD flags, DWORD* threadId);

#define FindResource FindResourceA


// Drive / focus / debug-break helpers used by WinUtil.
#define DRIVE_UNKNOWN     0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE   2
#define DRIVE_FIXED       3
#define DRIVE_REMOTE      4
#define DRIVE_CDROM       5
#define DRIVE_RAMDISK     6

UINT  GetDriveTypeA(const char* rootPath);
HWND  GetFocus(void);
HWND  GetForegroundWindow(void);
void  DebugBreak(void);

#define GetDriveType GetDriveTypeA


// Window show/find helpers used by AssertMgr.
#define SW_HIDE      0
#define SW_SHOWNORMAL 1
#define SW_MINIMIZE  6
#define SW_RESTORE   9
#define SW_SHOW      5

HWND FindWindowA(const char* className, const char* windowName);
BOOL ShowWindow(HWND h, int cmdShow);

#define FindWindow FindWindowA


#define MB_ABORTRETRYIGNORE  0x00000002
#define MB_YESNOCANCEL       0x00000003
#define MB_RETRYCANCEL       0x00000005
#define IDABORT              3
#define IDRETRY              4
#define IDIGNORE             5

#define SW_MAXIMIZE          3
BOOL DestroyWindow(HWND h);

#ifdef __cplusplus
}
#endif

#endif // __LTCOMPAT_WINDOWS_H__
