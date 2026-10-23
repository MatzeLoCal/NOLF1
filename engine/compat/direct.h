// direct.h -- MSVC directory-manipulation header.
// The entry points the original sources use (_mkdir, _getcwd, _chdir) are
// declared in compat/windows.h; this forwards so <direct.h> resolves.
#ifndef __LTCOMPAT_DIRECT_H__
#define __LTCOMPAT_DIRECT_H__
#include <unistd.h>
#include <sys/stat.h>
#include "windows.h"
#endif // __LTCOMPAT_DIRECT_H__
