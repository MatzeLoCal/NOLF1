#ifndef __WINUTIL_H
#define __WINUTIL_H

#ifndef BOOL
	typedef int		    BOOL;
	#define FALSE		    0
	#define TRUE		    1
#endif

#ifndef DWORD
	// PORT(macOS/LP64): `unsigned long` was 32-bit on Win32 -- and the authored
// intent here is explicitly 32 bits -- but it is 64-bit under LP64.
// NB the `#ifndef DWORD` guard around this NEVER fires: DWORD is a typedef,
// not a macro, so this redefines it even when a windows.h is in scope.
#if defined(__LP64__) || defined(_LP64)
typedef unsigned int	DWORD;
#else
typedef unsigned long	DWORD;
#endif
#endif

class CWinUtil
{
public:

	static BOOL GetMoviesPath (char* strPath);
	static BOOL DirExist (char* strPath);
	static BOOL CreateDir (char* strPath);
	static BOOL FileExist (char* strPath);

	static DWORD WinGetPrivateProfileString (char* lpAppName, char* lpKeyName, char* lpDefault, char* lpReturnedString, DWORD nSize, char* lpFileName);
	static DWORD WinWritePrivateProfileString (char* lpAppName, char* lpKeyName, char* lpString, char* lpFileName);

	static void DebugOut (char* str);
	static void DebugBreak();
	static float GetTime();

	static char* GetFocusWindow();

	static void WriteToDebugFile (char *strText);
};

#endif
