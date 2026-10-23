// mmsystem.h -- Win32 multimedia header.
// Only the millisecond timer is referenced by the original sources; audio is
// handled by the engine's own sound layer, not by this header.
#ifndef __LTCOMPAT_MMSYSTEM_H__
#define __LTCOMPAT_MMSYSTEM_H__
#include "windows.h"
#ifdef __cplusplus
extern "C" {
#endif
#define timeGetTime GetTickCount
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec;
                 WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
#ifdef __cplusplus
}
#endif
#endif // __LTCOMPAT_MMSYSTEM_H__
