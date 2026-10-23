// windowsx.h -- Win32 helper-macro header.
#ifndef __LTCOMPAT_WINDOWSX_H__
#define __LTCOMPAT_WINDOWSX_H__
#include "windows.h"

#define GET_X_LPARAM(lp) ((int)(short)((lp) & 0xffff))
#define GET_Y_LPARAM(lp) ((int)(short)(((lp) >> 16) & 0xffff))

// Message crackers. NOLF uses HANDLE_MSG to dispatch window messages to
// HANDLE_WM_* handlers it defines itself.
#define HANDLE_MSG(hwnd, message, fn) \
    case (message): return HANDLE_##message((hwnd), (wParam), (lParam), (fn))

#define HANDLE_WM_SETCURSOR(hwnd, wParam, lParam, fn) \
    (LRESULT)(BOOL)(fn)((hwnd), (HWND)(wParam), (UINT)((lParam) & 0xffff), (UINT)(((lParam) >> 16) & 0xffff))
#define HANDLE_WM_MOUSEMOVE(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_LBUTTONDOWN(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), FALSE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_LBUTTONUP(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_RBUTTONDOWN(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), FALSE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_RBUTTONUP(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)

#define HANDLE_WM_LBUTTONDBLCLK(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), TRUE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_RBUTTONDBLCLK(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), TRUE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_CHAR(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), (char)(wParam), (int)(short)((lParam) & 0xffff)), 0L)

#endif // __LTCOMPAT_WINDOWSX_H__
