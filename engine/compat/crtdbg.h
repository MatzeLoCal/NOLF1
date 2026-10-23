// crtdbg.h -- MSVC debug-CRT header the original LT2 sources include.
// The debug heap has no macOS equivalent; the assert macros map onto <assert.h>
// and the heap-tracking calls become no-ops.
#ifndef __LTCOMPAT_CRTDBG_H__
#define __LTCOMPAT_CRTDBG_H__

#include <assert.h>

#ifndef _ASSERT
  #define _ASSERT(expr)          assert(expr)
#endif
#ifndef _ASSERTE
  #define _ASSERTE(expr)         assert(expr)
#endif

// Debug-heap flags and calls: accepted and ignored.
#define _CRTDBG_ALLOC_MEM_DF        0x01
#define _CRTDBG_CHECK_ALWAYS_DF     0x04
#define _CRTDBG_LEAK_CHECK_DF       0x20
#define _CRTDBG_REPORT_FLAG         (-1)

#define _CrtSetDbgFlag(f)           ((void)(f), 0)
#define _CrtCheckMemory()           (1)
#define _CrtDumpMemoryLeaks()       (0)

#define _malloc_dbg(sz, t, f, l)    malloc(sz)
#define _free_dbg(p, t)             free(p)


// MSVC's debug-report hook type. NOLF's AssertMgr stores one of these.
#ifndef _CRT_REPORT_HOOK_DEFINED
#define _CRT_REPORT_HOOK_DEFINED
typedef int (*_CRT_REPORT_HOOK)(int reportType, char* message, int* returnValue);
#endif

#define _CrtSetReportHook(h)        ((_CRT_REPORT_HOOK)0)
#define _CRT_WARN   0
#define _CRT_ERROR  1
#define _CRT_ASSERT 2

#endif // __LTCOMPAT_CRTDBG_H__
