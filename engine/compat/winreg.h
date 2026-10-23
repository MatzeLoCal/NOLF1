// winreg.h -- Win32 registry API.
//
// NOLF's RegMgr stores player profile / config values here. There is no macOS
// equivalent and the port should NOT invent one silently: the registry calls
// below announce themselves and fail cleanly, so callers take their
// "no stored value" path rather than reading uninitialised data.
//
// When settings persistence is implemented it belongs behind this interface
// (a plist or a file under Application Support), not scattered through the
// game code.
#ifndef __LTCOMPAT_WINREG_H__
#define __LTCOMPAT_WINREG_H__

#include "windows.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HKEY_CLASSES_ROOT    ((HKEY)(uintptr_t)0x80000000)
#define HKEY_CURRENT_USER    ((HKEY)(uintptr_t)0x80000001)
#define HKEY_LOCAL_MACHINE   ((HKEY)(uintptr_t)0x80000002)
#define HKEY_USERS           ((HKEY)(uintptr_t)0x80000003)

#define ERROR_SUCCESS        0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_NO_MORE_ITEMS  259L

#define KEY_QUERY_VALUE      0x0001
#define KEY_SET_VALUE        0x0002
#define KEY_READ             0x20019
#define KEY_WRITE            0x20006
#define KEY_ALL_ACCESS       0xF003F

#define REG_NONE             0
#define REG_SZ               1
#define REG_BINARY           3
#define REG_DWORD            4

typedef LONG LSTATUS;

LSTATUS RegOpenKeyExA(HKEY key, const char* subKey, DWORD options,
                      DWORD desired, HKEY* result);
LSTATUS RegCreateKeyExA(HKEY key, const char* subKey, DWORD reserved,
                        char* cls, DWORD options, DWORD desired,
                        void* security, HKEY* result, DWORD* disposition);
LSTATUS RegQueryValueExA(HKEY key, const char* valueName, DWORD* reserved,
                         DWORD* type, BYTE* data, DWORD* cbData);
LSTATUS RegSetValueExA(HKEY key, const char* valueName, DWORD reserved,
                       DWORD type, const BYTE* data, DWORD cbData);
LSTATUS RegDeleteValueA(HKEY key, const char* valueName);
LSTATUS RegDeleteKeyA(HKEY key, const char* subKey);
LSTATUS RegEnumKeyExA(HKEY key, DWORD index, char* name, DWORD* cbName,
                      DWORD* reserved, char* cls, DWORD* cbClass, FILETIME* lastWrite);
LSTATUS RegCloseKey(HKEY key);

#define RegOpenKeyEx    RegOpenKeyExA
#define RegCreateKeyEx  RegCreateKeyExA
#define RegQueryValueEx RegQueryValueExA
#define RegSetValueEx   RegSetValueExA
#define RegDeleteValue  RegDeleteValueA
#define RegDeleteKey    RegDeleteKeyA
#define RegEnumKeyEx    RegEnumKeyExA

#ifdef __cplusplus
}
#endif
#endif // __LTCOMPAT_WINREG_H__
