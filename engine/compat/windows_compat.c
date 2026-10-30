// ----------------------------------------------------------------------- //
//
// MODULE  : windows_compat.c
//
// PURPOSE : Implementation of the Win32 surface declared in compat/windows.h.
//
// NOTE    : Functions that genuinely cannot work yet call
//           LTCOMPAT_UNIMPLEMENTED() and RETURN A DEFINED VALUE. They are never
//           silent. Anything here that returns a plausible-looking value
//           without doing the work is a liability, so it says so out loud the
//           first time it is reached.
//
// ----------------------------------------------------------------------- //

#include "windows.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>

// ----------------------------------------------------------------------- //
// Stub reporting
// ----------------------------------------------------------------------- //

void LTCompat_ReportUnimplemented(const char* fn, const char* file, int line)
{
    fprintf(stderr,
            "[LTCOMPAT] UNIMPLEMENTED: %s()  (%s:%d)  -- first call only\n",
            fn ? fn : "?", file ? file : "?", line);
    fflush(stderr);
}

// ----------------------------------------------------------------------- //
// Last error
// ----------------------------------------------------------------------- //

static DWORD g_LastError = 0;
DWORD GetLastError(void)      { return g_LastError; }
void  SetLastError(DWORD e)   { g_LastError = e; }

// ----------------------------------------------------------------------- //
// String helpers
// ----------------------------------------------------------------------- //

#undef strupr
#undef strlwr

char* _strupr(char* s)
{
    if (!s) return s;
    for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return s;
}

char* _strlwr(char* s)
{
    if (!s) return s;
    for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return s;
}

int wsprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    // Win32's wsprintf caps at 1024 bytes including the terminator. Matching
    // that bound is deliberate: code written against it may rely on the clamp.
    int n = vsnprintf(buffer, 1024, format, args);
    va_end(args);
    return n;
}

char* lstrcpy(char* dst, const char* src)   { return strcpy(dst, src); }
char* lstrcat(char* dst, const char* src)   { return strcat(dst, src); }
int   lstrlen(const char* s)                { return s ? (int)strlen(s) : 0; }
int   lstrcmp(const char* a, const char* b) { return strcmp(a, b); }
int   lstrcmpi(const char* a, const char* b){ return strcasecmp(a, b); }

int _mkdir(const char* path)
{
    return mkdir(path, 0755);
}

// Win32 path splitting. NOLF 1 uses this on asset paths, which are authored
// with backslashes, so BOTH separators must be recognised.
void _splitpath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
    if (drive) drive[0] = '\0';
    if (dir)   dir[0]   = '\0';
    if (fname) fname[0] = '\0';
    if (ext)   ext[0]   = '\0';
    if (!path) return;

    const char* p = path;

    // Drive letter, e.g. "C:"
    if (p[0] && p[1] == ':') {
        if (drive) { drive[0] = p[0]; drive[1] = ':'; drive[2] = '\0'; }
        p += 2;
    }

    // Last separator -- either flavour.
    const char* lastSlash = NULL;
    for (const char* q = p; *q; ++q)
        if (*q == '/' || *q == '\\') lastSlash = q;

    const char* nameStart = p;
    if (lastSlash) {
        size_t n = (size_t)(lastSlash - p) + 1;
        if (dir) { memcpy(dir, p, n); dir[n] = '\0'; }
        nameStart = lastSlash + 1;
    }

    const char* dot = strrchr(nameStart, '.');
    if (dot) {
        if (fname) {
            size_t n = (size_t)(dot - nameStart);
            memcpy(fname, nameStart, n);
            fname[n] = '\0';
        }
        if (ext) strcpy(ext, dot);
    } else {
        if (fname) strcpy(fname, nameStart);
    }
}

void _makepath(char* path, const char* drive, const char* dir,
               const char* fname, const char* ext)
{
    if (!path) return;
    path[0] = '\0';
    if (drive && drive[0]) strcat(path, drive);
    if (dir   && dir[0])   strcat(path, dir);
    if (fname && fname[0]) strcat(path, fname);
    if (ext   && ext[0])   strcat(path, ext);
}

// ----------------------------------------------------------------------- //
// Time
// ----------------------------------------------------------------------- //

// ⚠️ A SECOND CLOCK, AND IT HAD TO BE PUT UNDER THE ENGINE'S CONTROL.
//
//   The port replaced the engine clock with a FIXED TIMESTEP so runs are
//   reproducible (engine/kernel/time/). GetTickCount is a *different* clock,
//   and NOLF drives real animation from it: CWinUtil::GetTime() is
//   `GetTickCount() / 1000.0f` (WinUtil.cpp:128), and CBaseScaleFX uses that
//   for every interface effect's start time and phase (BaseScaleFX.cpp:230,
//   :261).
//
//   So while this returned wall time, the main menu's rotating flower and
//   panels sat at a DIFFERENT ANGLE on every run: six runs in fresh
//   directories produced three distinct frames, differing by up to 38% of
//   pixels. The rasteriser and the scene were entirely deterministic; the
//   clock underneath them was not.
//
//   On Win32 both clocks advanced together in real time, so having them
//   disagree is the unfaithful state -- this restores the pairing.
//   LT_REALTIME=1 leaves the hook unset and the real clock is used, exactly as
//   documented.
static DWORD (*g_pfnTickSource)(void) = 0;

void LTCompat_SetTickSource(DWORD (*pfn)(void))
{
    g_pfnTickSource = pfn;
}

DWORD GetTickCount(void)
{
    struct timeval tv;
    if (g_pfnTickSource) return g_pfnTickSource();
    gettimeofday(&tv, NULL);
    // Wraps at 2^32 ms just as Win32 does. Code comparing tick deltas with
    // unsigned arithmetic stays correct across the wrap; code that casts the
    // difference to a SIGNED wider type does not (this exact mistake kicked
    // every client off a NOLF 2 server after ~30 seconds).
    return (DWORD)((uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u);
}

void Sleep(DWORD ms)
{
    usleep((useconds_t)ms * 1000);
}

// ----------------------------------------------------------------------- //
// Debug output
// ----------------------------------------------------------------------- //

#undef OutputDebugString
void OutputDebugStringA(const char* s)
{
    if (s) { fputs(s, stderr); fflush(stderr); }
}

// ----------------------------------------------------------------------- //
// File I/O -- POSIX fd carried inside the HANDLE
// ----------------------------------------------------------------------- //

#undef CreateFile
#undef CreateDirectory
#undef GetFileAttributes
#undef DeleteFile

static HANDLE fd_to_handle(int fd)
{
    return (fd < 0) ? INVALID_HANDLE_VALUE : (HANDLE)(intptr_t)(fd + 1);
}
static int handle_to_fd(HANDLE h)
{
    return (h == INVALID_HANDLE_VALUE || h == NULL) ? -1 : (int)(intptr_t)h - 1;
}

HANDLE CreateFileA(const char* name, DWORD access, DWORD share,
                   void* security, DWORD creation, DWORD flags, HANDLE tmpl)
{
    (void)share; (void)security; (void)flags; (void)tmpl;

    int oflag = 0;
    if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) oflag = O_RDWR;
    else if (access & GENERIC_WRITE)                         oflag = O_WRONLY;
    else                                                     oflag = O_RDONLY;

    switch (creation) {
        case CREATE_NEW:        oflag |= O_CREAT | O_EXCL;  break;
        case CREATE_ALWAYS:     oflag |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS:       oflag |= O_CREAT;           break;
        case TRUNCATE_EXISTING: oflag |= O_TRUNC;           break;
        case OPEN_EXISTING:
        default:                                            break;
    }

    int fd = open(name, oflag, 0644);
    if (fd < 0) { SetLastError((DWORD)errno); return INVALID_HANDLE_VALUE; }
    return fd_to_handle(fd);
}

// NB: the out-params are NOT named `read`/`write` -- those would shadow the
// POSIX calls this function is built on.
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD toRead, DWORD* bytesRead, void* ov)
{
    (void)ov;
    int fd = handle_to_fd(h);
    if (fd < 0) return FALSE;
    ssize_t n = read(fd, buf, toRead);
    if (n < 0) { SetLastError((DWORD)errno); if (bytesRead) *bytesRead = 0; return FALSE; }
    if (bytesRead) *bytesRead = (DWORD)n;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD toWrite, DWORD* bytesWritten, void* ov)
{
    (void)ov;
    int fd = handle_to_fd(h);
    if (fd < 0) return FALSE;
    ssize_t n = write(fd, buf, toWrite);
    if (n < 0) { SetLastError((DWORD)errno); if (bytesWritten) *bytesWritten = 0; return FALSE; }
    if (bytesWritten) *bytesWritten = (DWORD)n;
    return TRUE;
}

BOOL CloseHandle(HANDLE h)
{
    int fd = handle_to_fd(h);
    if (fd < 0) return FALSE;
    return close(fd) == 0 ? TRUE : FALSE;
}

DWORD SetFilePointer(HANDLE h, LONG dist, LONG* distHigh, DWORD method)
{
    (void)distHigh;
    int fd = handle_to_fd(h);
    if (fd < 0) return (DWORD)-1;
    int whence = (method == FILE_CURRENT) ? SEEK_CUR
               : (method == FILE_END)     ? SEEK_END
                                          : SEEK_SET;
    off_t r = lseek(fd, dist, whence);
    return (r < 0) ? (DWORD)-1 : (DWORD)r;
}

DWORD GetFileSize(HANDLE h, DWORD* sizeHigh)
{
    if (sizeHigh) *sizeHigh = 0;
    int fd = handle_to_fd(h);
    if (fd < 0) return (DWORD)-1;
    struct stat st;
    if (fstat(fd, &st) != 0) return (DWORD)-1;
    return (DWORD)st.st_size;
}

BOOL CreateDirectoryA(const char* path, void* security)
{
    (void)security;
    return mkdir(path, 0755) == 0 ? TRUE : FALSE;
}

DWORD GetFileAttributesA(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL DeleteFileA(const char* path)
{
    return unlink(path) == 0 ? TRUE : FALSE;
}

// ----------------------------------------------------------------------- //
// INI files -- the Win32 private-profile API.
//
// ⚠️ NOLF'S SAVE-GAME LIST LIVES HERE, and with both calls stubbed no save
// could ever be loaded: CGameClientShell::SaveGame records each slot as
// `[No One Lives Forever] SaveGame00=<world>|<time>` in Save\Save1001.ini
// (SAVEGAMEINI_FILENAME), and QuickLoad / the Load folder read that entry
// back -- an empty answer means "no quick save" even with Save\Quick.sav on
// disk (HANDOVER 5bz).
//
// The path rule is the server's (ltserverhost.cpp LTHost_SaveObjects):
// backslashes become slashes, relative to the working directory, the parent
// directory made on write -- so the INI sits beside Save/Quick.sav. Sections
// and keys match case-insensitively; values are trimmed and one pair of
// surrounding quotes is dropped, as Windows does. NULL key = delete or list,
// NULL value = delete key, NULL section = list sections.
// LT_NOINI=1 = the old stubs (announces itself).
// ----------------------------------------------------------------------- //

#undef GetPrivateProfileString
#undef WritePrivateProfileString

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static int ini_off(void)
{
    static int s_n = -1;
    if (s_n < 0)
    {
        s_n = getenv("LT_NOINI") != 0;
        if (s_n) { fprintf(stdout, "[LTCOMPAT] LT_NOINI=1 -- INI files not read or written (old stubs)\n"); fflush(stdout); }
    }
    return s_n;
}

typedef struct { char** v; int n, cap; } IniLines;

static void ini_insert(IniLines* L, int at, const char* line)
{
    if (L->n == L->cap)
    {
        L->cap = L->cap ? L->cap * 2 : 32;
        L->v = (char**)realloc(L->v, sizeof(char*) * (size_t)L->cap);
    }
    memmove(L->v + at + 1, L->v + at, sizeof(char*) * (size_t)(L->n - at));
    L->v[at] = strdup(line);
    ++L->n;
}

static void ini_erase(IniLines* L, int at)
{
    free(L->v[at]);
    memmove(L->v + at, L->v + at + 1, sizeof(char*) * (size_t)(L->n - at - 1));
    --L->n;
}

static void ini_free(IniLines* L)
{
    for (int i = 0; i < L->n; ++i) free(L->v[i]);
    free(L->v);
    L->v = 0; L->n = L->cap = 0;
}

static void ini_path(const char* file, char* out, size_t size)
{
    size_t i = 0;
    for (; file[i] && i + 1 < size; ++i) out[i] = (file[i] == '\\') ? '/' : file[i];
    out[i] = '\0';
}

static void ini_load(const char* path, IniLines* L)
{
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f))
    {
        size_t n = strlen(buf);
        while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        ini_insert(L, L->n, buf);
    }
    fclose(f);
}

static int ini_save(const char* path, const IniLines* L)
{
    char dir[1024];
    strncpy(dir, path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
    char* slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0755); }     /* EEXIST is the normal answer */
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    for (int i = 0; i < L->n; ++i) fprintf(f, "%s\r\n", L->v[i]);
    fclose(f);
    return 1;
}

/* Trimmed copy of [b, e) into out. */
static void ini_copytrim(const char* b, const char* e, char* out, size_t size)
{
    while (b < e && isspace((unsigned char)*b)) ++b;
    while (e > b && isspace((unsigned char)e[-1])) --e;
    size_t n = (size_t)(e - b);
    if (n >= size) n = size - 1;
    memcpy(out, b, n); out[n] = '\0';
}

/* "[name]" -> 1 and the trimmed name. */
static int ini_section(const char* line, char* name, size_t size)
{
    while (isspace((unsigned char)*line)) ++line;
    if (*line != '[') return 0;
    const char* e = strchr(line, ']');
    if (!e) return 0;
    ini_copytrim(line + 1, e, name, size);
    return 1;
}

/* "key=value" -> 1, the trimmed key, and where the value starts. */
static int ini_keyval(const char* line, char* key, size_t size, const char** pVal)
{
    const char* b = line;
    while (isspace((unsigned char)*b)) ++b;
    if (!*b || *b == ';' || *b == '[') return 0;
    const char* eq = strchr(b, '=');
    if (!eq) return 0;
    ini_copytrim(b, eq, key, size);
    if (pVal) *pVal = eq + 1;
    return 1;
}

/* Index of section `app`'s header, or -1; *pEnd = the first line after it. */
static int ini_find_section(const IniLines* L, const char* app, int* pEnd)
{
    char name[512];
    int s = -1;
    for (int i = 0; i < L->n; ++i)
    {
        if (!ini_section(L->v[i], name, sizeof(name))) continue;
        if (s >= 0) { *pEnd = i; return s; }
        if (strcasecmp(name, app) == 0) s = i;
    }
    *pEnd = L->n;
    return s;
}

/* Windows' list form: each name NUL-terminated, a second NUL at the end. */
static DWORD ini_list_add(char* out, DWORD size, DWORD* pUsed, const char* name, int* pFull)
{
    const DWORD n = (DWORD)strlen(name);
    if (*pFull) return *pUsed;
    if (*pUsed + n + 2 > size) { *pFull = 1; return *pUsed; }
    memcpy(out + *pUsed, name, n + 1);
    *pUsed += n + 1;
    return *pUsed;
}

DWORD GetPrivateProfileStringA(const char* app, const char* key,
                               const char* def, char* out, DWORD size,
                               const char* file)
{
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (ini_off() || !file)
    {
        if (def) { strncpy(out, def, size - 1); out[size - 1] = '\0'; }
        return (DWORD)strlen(out);
    }

    char path[1024];
    ini_path(file, path, sizeof(path));
    IniLines L = { 0, 0, 0 };
    ini_load(path, &L);

    char name[512];
    DWORD used = 0;
    int full = 0;

    if (!app)                                   /* every section name */
    {
        for (int i = 0; i < L.n; ++i)
            if (ini_section(L.v[i], name, sizeof(name))) ini_list_add(out, size, &used, name, &full);
        if (size >= 2) out[used] = '\0';
        ini_free(&L);
        return full ? (size >= 2 ? size - 2 : 0) : used;
    }

    int e = 0;
    const int s = ini_find_section(&L, app, &e);

    if (!key)                                   /* every key of the section */
    {
        if (s >= 0)
            for (int i = s + 1; i < e; ++i)
                if (ini_keyval(L.v[i], name, sizeof(name), 0)) ini_list_add(out, size, &used, name, &full);
        if (size >= 2) out[used] = '\0';
        ini_free(&L);
        return full ? (size >= 2 ? size - 2 : 0) : used;
    }

    if (s >= 0)
        for (int i = s + 1; i < e; ++i)
        {
            const char* pVal = 0;
            if (!ini_keyval(L.v[i], name, sizeof(name), &pVal) || strcasecmp(name, key) != 0) continue;
            char val[4096];
            ini_copytrim(pVal, pVal + strlen(pVal), val, sizeof(val));
            size_t n = strlen(val);
            const char* v = val;
            if (n >= 2 && (val[0] == '"' || val[0] == '\'') && val[n-1] == val[0]) { val[n-1] = '\0'; ++v; }
            strncpy(out, v, size - 1); out[size - 1] = '\0';
            ini_free(&L);
            return (DWORD)strlen(out);
        }

    ini_free(&L);
    if (def)
    {
        char d[4096];
        ini_copytrim(def, def + strlen(def), d, sizeof(d));
        strncpy(out, d, size - 1); out[size - 1] = '\0';
    }
    return (DWORD)strlen(out);
}

BOOL WritePrivateProfileStringA(const char* app, const char* key,
                                const char* value, const char* file)
{
    if (ini_off() || !app || !file) return FALSE;

    char path[1024];
    ini_path(file, path, sizeof(path));
    IniLines L = { 0, 0, 0 };
    ini_load(path, &L);

    int e = 0;
    int s = ini_find_section(&L, app, &e);

    if (!key)                                   /* delete the whole section */
    {
        if (s >= 0) for (int i = e - 1; i >= s; --i) ini_erase(&L, i);
        const int ok = ini_save(path, &L);
        ini_free(&L);
        return ok ? TRUE : FALSE;
    }

    char line[4096];
    if (value) snprintf(line, sizeof(line), "%s=%s", key, value);

    if (s < 0)
    {
        if (!value) { ini_free(&L); return TRUE; }      /* nothing to delete */
        char hdr[600];
        snprintf(hdr, sizeof(hdr), "[%s]", app);
        ini_insert(&L, L.n, hdr);
        ini_insert(&L, L.n, line);
    }
    else
    {
        char name[512];
        int found = -1, lastKey = s;
        for (int i = s + 1; i < e; ++i)
        {
            if (!ini_keyval(L.v[i], name, sizeof(name), 0)) continue;
            lastKey = i;
            if (found < 0 && strcasecmp(name, key) == 0) found = i;
        }
        if (found >= 0)
        {
            if (value) { free(L.v[found]); L.v[found] = strdup(line); }
            else       ini_erase(&L, found);
        }
        else if (value)
            ini_insert(&L, lastKey + 1, line);
    }

    const int ok = ini_save(path, &L);
    ini_free(&L);
    return ok ? TRUE : FALSE;
}

// ----------------------------------------------------------------------- //
// Window / input -- owned by the Cocoa layer once it exists.
//
// ⚠️ Every one of these is on the path that makes menus work. When the Cocoa
// layer lands, these must be replaced, NOT left returning defaults -- a
// silently-defaulting GetCursorPos gives a game with working clicks and a
// cursor pinned at the origin.
// ----------------------------------------------------------------------- //

#undef SendMessage
#undef PostMessage
#undef DefWindowProc
#undef CallWindowProc
#undef SetWindowLong
#undef GetWindowLong
#undef MessageBox

BOOL GetWindowRect(HWND h, LPRECT r)
{
    (void)h;
    LTCOMPAT_UNIMPLEMENTED();
    if (r) { r->left = r->top = r->right = r->bottom = 0; }
    return FALSE;
}

BOOL GetClientRect(HWND h, LPRECT r)
{
    (void)h;
    LTCOMPAT_UNIMPLEMENTED();
    if (r) { r->left = r->top = r->right = r->bottom = 0; }
    return FALSE;
}

BOOL ClientToScreen(HWND h, LPPOINT p) { (void)h; (void)p; LTCOMPAT_UNIMPLEMENTED(); return FALSE; }
BOOL ScreenToClient(HWND h, LPPOINT p) { (void)h; (void)p; LTCOMPAT_UNIMPLEMENTED(); return FALSE; }

BOOL GetCursorPos(LPPOINT p)
{
    LTCOMPAT_UNIMPLEMENTED();
    if (p) { p->x = 0; p->y = 0; }
    return FALSE;
}

BOOL  SetCursorPos(int x, int y)  { (void)x; (void)y; LTCOMPAT_UNIMPLEMENTED(); return FALSE; }
int   ShowCursor(BOOL show)       { (void)show; LTCOMPAT_UNIMPLEMENTED(); return 0; }
short GetAsyncKeyState(int vk)    { (void)vk; LTCOMPAT_UNIMPLEMENTED(); return 0; }
short GetKeyState(int vk)         { (void)vk; LTCOMPAT_UNIMPLEMENTED(); return 0; }

LRESULT SendMessageA(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    (void)h; (void)msg; (void)w; (void)l;
    LTCOMPAT_UNIMPLEMENTED();
    return 0;
}

BOOL PostMessageA(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    (void)h; (void)msg; (void)w; (void)l;
    LTCOMPAT_UNIMPLEMENTED();
    return FALSE;
}

LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    (void)h; (void)msg; (void)w; (void)l;
    return 0;
}

LRESULT CallWindowProcA(WNDPROC prev, HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (prev) return prev(h, msg, w, l);
    return 0;
}

/* ⚠️ GWL_WNDPROC IS REAL STORAGE, and it has to be.
 *
 * NOLF's HookWindow() does GetWindowLong(GWL_WNDPROC), bails if the result is
 * NULL, then SetWindowLong()s its own procedure in. Both were stubs returning
 * 0, so the hook failed at the first line and the game silently had no mouse
 * input at all -- with no diagnostic, because failing that check is a normal
 * code path.
 *
 * One slot: there is exactly one window. Anything other than GWL_WNDPROC still
 * reports as unimplemented rather than pretending. */
static intptr_t g_WndProcSlot = 0;

intptr_t SetWindowLongA(HWND h, int index, intptr_t value)
{
    (void)h;
    if (index == GWL_WNDPROC)
    {
        intptr_t prev = g_WndProcSlot;
        g_WndProcSlot = value;
        /* Win32 returns the PREVIOUS value, and returning 0 would make the
         * caller think it failed -- HookWindow tests exactly that. */
        return prev ? prev : value;
    }
    LTCOMPAT_UNIMPLEMENTED();
    return 0;
}

intptr_t GetWindowLongA(HWND h, int index)
{
    (void)h;
    if (index == GWL_WNDPROC)
        return g_WndProcSlot;
    LTCOMPAT_UNIMPLEMENTED();
    return 0;
}

int MessageBoxA(HWND h, const char* text, const char* caption, UINT type)
{
    (void)h; (void)type;
    // Deliberately real, not a stub: this is how the original code reports
    // fatal problems, and losing that during bring-up would hide failures.
    fprintf(stderr, "[MessageBox] %s: %s\n",
            caption ? caption : "NOLF", text ? text : "");
    fflush(stderr);
    return IDOK;
}

// ----------------------------------------------------------------------- //
// Modules
// ----------------------------------------------------------------------- //

#undef GetModuleHandle
#undef LoadLibrary
#undef GetModuleFileName

HMODULE GetModuleHandleA(const char* name) { (void)name; return NULL; }
void*   GetProcAddress(HMODULE m, const char* n) { (void)m; (void)n; LTCOMPAT_UNIMPLEMENTED(); return NULL; }
HMODULE LoadLibraryA(const char* name) { (void)name; LTCOMPAT_UNIMPLEMENTED(); return NULL; }
BOOL    FreeLibrary(HMODULE m) { (void)m; return TRUE; }

DWORD GetModuleFileNameA(HMODULE m, char* out, DWORD size)
{
    (void)m;
    LTCOMPAT_UNIMPLEMENTED();
    if (out && size) out[0] = '\0';
    return 0;
}

// ----------------------------------------------------------------------- //
// Events / cursor clipping / misc.
//
// The event objects are a minimal manual-reset implementation over a heap flag.
// They are NOT thread-synchronisation primitives -- nothing waits on them
// across threads yet. If a real WaitForSingleObject with blocking semantics is
// ever needed, THIS is the thing to replace, and it says so when it is used.
// ----------------------------------------------------------------------- //

#undef CreateEvent
#undef GetWindowText

typedef struct { int signalled; int manualReset; } LTCompatEvent;

HANDLE CreateEventA(void* attrs, BOOL manualReset, BOOL initialState, const char* name)
{
    (void)attrs; (void)name;
    LTCompatEvent* e = (LTCompatEvent*)malloc(sizeof(LTCompatEvent));
    if (!e) return NULL;
    e->signalled  = initialState ? 1 : 0;
    e->manualReset = manualReset ? 1 : 0;
    return (HANDLE)e;
}

BOOL SetEvent(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    ((LTCompatEvent*)h)->signalled = 1;
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    ((LTCompatEvent*)h)->signalled = 0;
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    (void)ms;
    // Announces itself: a caller that genuinely needs to BLOCK will otherwise
    // silently spin or proceed early.
    LTCOMPAT_UNIMPLEMENTED();
    if (!h || h == INVALID_HANDLE_VALUE) return (DWORD)-1;
    return ((LTCompatEvent*)h)->signalled ? WAIT_OBJECT_0 : (DWORD)-1;
}

BOOL ClipCursor(const RECT* r)
{
    (void)r;
    LTCOMPAT_UNIMPLEMENTED();   // belongs to the Cocoa layer
    return FALSE;
}

int GetWindowTextA(HWND h, char* out, int maxCount)
{
    (void)h;
    LTCOMPAT_UNIMPLEMENTED();
    if (out && maxCount > 0) out[0] = '\0';
    return 0;
}

BOOL DeleteObject(HANDLE obj)
{
    (void)obj;
    return TRUE;    // GDI object lifetime has no meaning here
}

// ----------------------------------------------------------------------- //
// Remaining Win32 surface -- window/resource/error-mode helpers.
// ----------------------------------------------------------------------- //

#undef SetWindowText

BOOL UpdateWindow(HWND h)                    { (void)h; return TRUE; }
BOOL SetWindowTextA(HWND h, const char* t)   { (void)h; (void)t; LTCOMPAT_UNIMPLEMENTED(); return FALSE; }
UINT SetErrorMode(UINT mode)                 { (void)mode; return 0; }
void* LockResource(void* res)                { LTCOMPAT_UNIMPLEMENTED(); return res; }

HFILE OpenFile(const char* name, OFSTRUCT* rs, UINT style)
{
    if (rs) { rs->cBytes = sizeof(OFSTRUCT); rs->nErrCode = 0; rs->szPathName[0] = '\0'; }
    // OF_EXIST is the only style NOLF uses: a pure existence test.
    //
    // ⚠️ DOS SEPARATORS (HANDOVER 5db). The game asks for "Save\\Slot01.sav";
    // the ENGINE writes saves as Save/Slot01.sav, so the literal name was never
    // found: the Load game folder listed nothing (no quick save, no level
    // start, no slot) and Save never asked before overwriting. Some game files
    // are written AND read under the literal backslash name ("Save\\Summary.sav"
    // in the play dir), so the literal name is tried first, then with '/'.
    // LT_OLDFILEEXIST=1 = the literal name only (old, says so).
    if (style & OF_EXIST) {
        struct stat st;
        static int s_nOld = -1;
        if (s_nOld < 0) {
            s_nOld = getenv("LT_OLDFILEEXIST") ? 1 : 0;
            if (s_nOld) { printf("[COMPAT] LT_OLDFILEEXIST=1 -- OpenFile(OF_EXIST) tests the literal DOS path only (old)\n"); fflush(stdout); }
        }
        if (!name) return HFILE_ERROR;
        if (stat(name, &st) == 0) return 0;
        if (s_nOld) return HFILE_ERROR;
        {
            char buf[1024];
            size_t n = strlen(name), i;
            if (n >= sizeof(buf)) return HFILE_ERROR;
            for (i = 0; i <= n; ++i) buf[i] = (name[i] == '\\') ? '/' : name[i];
            return (stat(buf, &st) == 0) ? 0 : HFILE_ERROR;
        }
    }
    LTCOMPAT_UNIMPLEMENTED();
    return HFILE_ERROR;
}

// ----------------------------------------------------------------------- //
// Listbox / window placement / resources / threads.
//
// These belong to the Cocoa + platform layer. Every one that cannot work yet
// announces itself rather than quietly returning a plausible value.
// ----------------------------------------------------------------------- //

#undef FindResource

BOOL InvalidateRect(HWND h, const RECT* r, BOOL erase)
{ (void)h; (void)r; (void)erase; LTCOMPAT_UNIMPLEMENTED(); return FALSE; }

BOOL SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{ (void)h; (void)after; (void)x; (void)y; (void)cx; (void)cy; (void)flags;
  LTCOMPAT_UNIMPLEMENTED(); return FALSE; }

int MapWindowPoints(HWND from, HWND to, LPPOINT pts, UINT count)
{ (void)from; (void)to; (void)pts; (void)count; LTCOMPAT_UNIMPLEMENTED(); return 0; }

HRSRC FindResourceA(HMODULE m, const char* name, const char* type)
{ (void)m; (void)name; (void)type; LTCOMPAT_UNIMPLEMENTED(); return NULL; }

HGLOBAL LoadResource(HMODULE m, HRSRC res)
{ (void)m; (void)res; LTCOMPAT_UNIMPLEMENTED(); return NULL; }

// ⚠️ THE CONTROLS SCREEN'S KEY LABELS (HANDOVER 5dv). The game maps a key's
// English character to what the player's layout types on that key
// (KeyFixes.cpp ConvertKeyToCurrentKeyboard): scan -> VK (type 1), VK -> char
// (type 2). Returning the input unchanged turned 'W' into character 17 --
// nothing on screen. The platform layer answers (ltbindings.cpp, from the
// binding table and the Mac's current layout); weak, so a tool that links
// this file alone still links and gets 0 = "no translation", which sends the
// game to its fallback (the English name).
extern unsigned int LTPlatform_MapVirtualKey(unsigned int code, unsigned int mapType) __attribute__((weak));

HKL GetKeyboardLayout(DWORD threadId)
{ (void)threadId; return (HKL)1; }          // one layout: the Mac's current one

UINT MapVirtualKeyEx(UINT code, UINT mapType, HKL layout)
{
    (void)layout;
    if (LTPlatform_MapVirtualKey) return LTPlatform_MapVirtualKey(code, mapType);
    LTCOMPAT_UNIMPLEMENTED();
    return 0;
}

HANDLE CreateThread(void* attrs, size_t stackSize,
                    DWORD (*start)(void*), void* param, DWORD flags, DWORD* threadId)
{ (void)attrs; (void)stackSize; (void)start; (void)param; (void)flags;
  if (threadId) *threadId = 0;
  LTCOMPAT_UNIMPLEMENTED(); return NULL; }

// ----------------------------------------------------------------------- //
// Drive type / focus / debug break.
// ----------------------------------------------------------------------- //

#undef GetDriveType

// NOLF uses this solely to find the CD drive for the disc check / streaming
// audio. There is no such concept here; reporting FIXED is honest and lets the
// caller fall through to its non-CD path.
UINT GetDriveTypeA(const char* rootPath)
{
    (void)rootPath;
    return DRIVE_FIXED;
}

HWND GetFocus(void)             { LTCOMPAT_UNIMPLEMENTED(); return NULL; }
HWND GetForegroundWindow(void)  { LTCOMPAT_UNIMPLEMENTED(); return NULL; }

void DebugBreak(void)
{
#if defined(__aarch64__)
    __builtin_debugtrap();
#else
    __builtin_trap();
#endif
}

#undef FindWindow
HWND FindWindowA(const char* className, const char* windowName)
{ (void)className; (void)windowName; LTCOMPAT_UNIMPLEMENTED(); return NULL; }
BOOL ShowWindow(HWND h, int cmdShow)
{ (void)h; (void)cmdShow; LTCOMPAT_UNIMPLEMENTED(); return FALSE; }

// ----------------------------------------------------------------------- //
// Registry -- see compat/winreg.h. Every entry point announces itself, so a
// caller relying on stored settings cannot be misled by a silent success.
// ----------------------------------------------------------------------- //

#include "winreg.h"

#undef RegOpenKeyEx
#undef RegCreateKeyEx
#undef RegQueryValueEx
#undef RegSetValueEx
#undef RegDeleteValue
#undef RegDeleteKey
#undef RegEnumKeyEx

LSTATUS RegOpenKeyExA(HKEY key, const char* subKey, DWORD options,
                      DWORD desired, HKEY* result)
{ (void)key;(void)subKey;(void)options;(void)desired;
  LTCOMPAT_UNIMPLEMENTED(); if (result) *result = NULL; return ERROR_FILE_NOT_FOUND; }

LSTATUS RegCreateKeyExA(HKEY key, const char* subKey, DWORD reserved, char* cls,
                        DWORD options, DWORD desired, void* security,
                        HKEY* result, DWORD* disposition)
{ (void)key;(void)subKey;(void)reserved;(void)cls;(void)options;(void)desired;(void)security;
  LTCOMPAT_UNIMPLEMENTED(); if (result) *result = NULL; if (disposition) *disposition = 0;
  return ERROR_FILE_NOT_FOUND; }

LSTATUS RegQueryValueExA(HKEY key, const char* valueName, DWORD* reserved,
                         DWORD* type, BYTE* data, DWORD* cbData)
{ (void)key;(void)valueName;(void)reserved;(void)data;
  LTCOMPAT_UNIMPLEMENTED(); if (type) *type = REG_NONE; if (cbData) *cbData = 0;
  return ERROR_FILE_NOT_FOUND; }

LSTATUS RegSetValueExA(HKEY key, const char* valueName, DWORD reserved,
                       DWORD type, const BYTE* data, DWORD cbData)
{ (void)key;(void)valueName;(void)reserved;(void)type;(void)data;(void)cbData;
  LTCOMPAT_UNIMPLEMENTED(); return ERROR_FILE_NOT_FOUND; }

LSTATUS RegDeleteValueA(HKEY key, const char* valueName)
{ (void)key;(void)valueName; LTCOMPAT_UNIMPLEMENTED(); return ERROR_FILE_NOT_FOUND; }

LSTATUS RegDeleteKeyA(HKEY key, const char* subKey)
{ (void)key;(void)subKey; LTCOMPAT_UNIMPLEMENTED(); return ERROR_FILE_NOT_FOUND; }

LSTATUS RegEnumKeyExA(HKEY key, DWORD index, char* name, DWORD* cbName,
                      DWORD* reserved, char* cls, DWORD* cbClass, FILETIME* lastWrite)
{ (void)key;(void)index;(void)name;(void)reserved;(void)cls;(void)cbClass;(void)lastWrite;
  LTCOMPAT_UNIMPLEMENTED(); if (cbName) *cbName = 0; return ERROR_NO_MORE_ITEMS; }

LSTATUS RegCloseKey(HKEY key)
{ (void)key; return ERROR_SUCCESS; }

BOOL DestroyWindow(HWND h) { (void)h; LTCOMPAT_UNIMPLEMENTED(); return FALSE; }
