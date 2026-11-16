// Linux port: the GL context shared by the window (ltwindow_sdl.cpp) and the
// OpenGL backend (ltgl.cpp, which implements ltmetal.h).
#pragma once

// Makes a GL 4.6 core context current: the window's when it is open, else
// (bAllowHidden) one on a hidden 16x16 window, for headless GPU runs.
bool  LTGLWin_EnsureContext(bool bAllowHidden);
bool  LTGLWin_HasWindow();                  // the context belongs to the visible window
void  LTGLWin_Swap();                       // presents the window's back buffer
bool  LTGLWin_DrawableSize(int* pW, int* pH);
void* LTGLWin_GetProc(const char* pName);

// ltgl.cpp: draws a top-down RGBA8 frame over the whole window (the software
// path's present); the caller swaps.
bool  LTGL_BlitRGBA(const unsigned char* pRGBA, int nWidth, int nHeight, int nOutW, int nOutH);
