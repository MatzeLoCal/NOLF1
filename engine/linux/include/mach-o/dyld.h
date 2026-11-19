// Linux port: stand-in for <mach-o/dyld.h>. Only _NSGetExecutablePath is used.
#pragma once
#include <unistd.h>
#include <stdint.h>
static inline int _NSGetExecutablePath(char* buf, uint32_t* size)
{
    ssize_t n = readlink("/proc/self/exe", buf, *size ? *size - 1 : 0);
    if (n < 0) return -1;
    buf[n] = 0;
    return 0;
}
