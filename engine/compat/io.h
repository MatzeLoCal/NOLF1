// io.h -- MSVC low-level I/O header.
// The POSIX equivalents live in <unistd.h> / <fcntl.h>; the underscore-prefixed
// MSVC spellings are mapped in compat/windows.h.
#ifndef __LTCOMPAT_IO_H__
#define __LTCOMPAT_IO_H__
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "windows.h"

#ifdef __cplusplus
extern "C" {
#endif

// MSVC's <io.h> file-search record. Declared here because LT2's ztools.h
// references it; the _findfirst/_findnext API itself is not implemented, and
// nothing in NOLF 1 calls it (the referencing class has 0 uses).
#ifndef _FINDDATA_T_DEFINED
#define _FINDDATA_T_DEFINED
struct _finddata_t {
    unsigned    attrib;
    long        time_create;
    long        time_access;
    long        time_write;
    long        size;
    char        name[260];
};
#endif

#ifdef __cplusplus
}
#endif
#endif // __LTCOMPAT_IO_H__
