// malloc.h -- MSVC/glibc header the original LT2 sources include.
// macOS has no top-level <malloc.h>; the allocation entry points live in
// <stdlib.h>, and alloca in <alloca.h>.
#ifndef __LTCOMPAT_MALLOC_H__
#define __LTCOMPAT_MALLOC_H__
#include <stdlib.h>
#include <alloca.h>
#endif // __LTCOMPAT_MALLOC_H__
