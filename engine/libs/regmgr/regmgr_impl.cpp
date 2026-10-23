// ----------------------------------------------------------------------- //
//
// MODULE  : regmgr_impl.cpp
//
// PURPOSE : CRegMgr -- NOLF 1's settings store, declared in
//           LT2/lithshared/lith/regmgr.h.
//
// WHY THIS IS A REAL STORE AND NOT A STUB
//
//   On Windows this class wraps the registry. There is no registry here, and
//   the tempting move is to stub it -- but a stub that always reports "no
//   value" means every setting the player changes is silently discarded. That
//   failure is invisible until someone notices their key bindings never stick,
//   which is a bad way to find out.
//
//   So this is backed by a plain key/value file instead. It is small, the file
//   is human-readable, and settings actually persist.
//
// ⚠️ WHERE IT WRITES -- deliberately NOT the game directory.
//
//   The port's standing rule is that the engine must never write into the
//   user's retail install. This store lives under
//
//       ~/Library/Application Support/NOLF1/<Company>/<App>/settings.cfg
//
//   which is both the macOS-correct location and safely outside the game data.
//
// ----------------------------------------------------------------------- //

#include "windows.h"        // first, so _WINDOWS_ is set (see mfcs_impl.cpp)
#include "winreg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "regmgr.h"

// ----------------------------------------------------------------------- //
// Backing store: a flat key/value table persisted as text.
// ----------------------------------------------------------------------- //

namespace
{
    struct RegEntry
    {
        char*     key;
        char*     value;      // stored as text; binary is hex-encoded
        RegEntry* next;
    };

    RegEntry* g_head    = 0;
    char      g_path[1024] = {0};
    bool      g_loaded  = false;
    bool      g_dirty   = false;

    void FreeAll()
    {
        RegEntry* p = g_head;
        while (p) { RegEntry* n = p->next; free(p->key); free(p->value); free(p); p = n; }
        g_head = 0;
    }

    RegEntry* Find(const char* key)
    {
        for (RegEntry* p = g_head; p; p = p->next)
            if (!strcasecmp(p->key, key)) return p;
        return 0;
    }

    void Put(const char* key, const char* value)
    {
        RegEntry* p = Find(key);
        if (p) { free(p->value); p->value = strdup(value); }
        else
        {
            p = (RegEntry*)malloc(sizeof(RegEntry));
            if (!p) return;
            p->key = strdup(key); p->value = strdup(value);
            p->next = g_head; g_head = p;
        }
        g_dirty = true;
    }

    // Create every directory along a path, like mkdir -p.
    void MakeDirs(const char* path)
    {
        char tmp[1024];
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        for (char* p = tmp + 1; *p; ++p)
        {
            if (*p != '/') continue;
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
        mkdir(tmp, 0755);
    }

    void Load()
    {
        if (g_loaded || !g_path[0]) return;
        g_loaded = true;

        FILE* fp = fopen(g_path, "r");
        if (!fp) return;                    // no file yet is normal, not an error

        char line[4096];
        while (fgets(line, sizeof(line), fp))
        {
            char* nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (!line[0] || line[0] == '#') continue;

            char* tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            Put(line, tab + 1);
        }
        fclose(fp);
        g_dirty = false;
    }

    void Save()
    {
        if (!g_dirty || !g_path[0]) return;

        // Ensure the containing directory exists.
        char dir[1024];
        strncpy(dir, g_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char* slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; MakeDirs(dir); }

        FILE* fp = fopen(g_path, "w");
        if (!fp) return;
        fprintf(fp, "# NOLF 1 settings -- written by the macOS port's CRegMgr\n");
        for (RegEntry* p = g_head; p; p = p->next)
            fprintf(fp, "%s\t%s\n", p->key, p->value);
        fclose(fp);
        g_dirty = false;
    }

    // Keys are namespaced by the current subkey so different sections cannot
    // collide, matching how the registry's key hierarchy behaved.
    char g_subKey[256] = {0};

    const char* FullKey(const char* sKey, char* buf, size_t n)
    {
        if (g_subKey[0]) snprintf(buf, n, "%s/%s", g_subKey, sKey ? sKey : "");
        else             snprintf(buf, n, "%s", sKey ? sKey : "");
        return buf;
    }
}

// ----------------------------------------------------------------------- //
// Lifecycle
// ----------------------------------------------------------------------- //

BOOL CRegMgr::Init(const char* sCompany, const char* sApp, const char* sVersion,
                   const char* sSubKey, HANDLE hRootKey, char* sRoot2)
{
    (void)hRootKey; (void)sRoot2; (void)sVersion;

    const char* home = getenv("HOME");
    if (!home) home = ".";

    snprintf(g_path, sizeof(g_path),
             "%s/Library/Application Support/NOLF1/%s/%s/settings.cfg",
             home,
             (sCompany && *sCompany) ? sCompany : "Monolith",
             (sApp && *sApp)         ? sApp     : "NOLF");

    g_subKey[0] = '\0';
    if (sSubKey && *sSubKey)
    {
        strncpy(g_subKey, sSubKey, sizeof(g_subKey) - 1);
        g_subKey[sizeof(g_subKey) - 1] = '\0';
    }

    g_loaded = false;
    Load();

    m_bInitialized = TRUE;
    return TRUE;
}

void CRegMgr::Term()
{
    if (!m_bInitialized)
        return;
    Save();
    FreeAll();
    g_loaded       = false;
    m_bInitialized = FALSE;
}

BOOL CRegMgr::SetSubKey(const char* sSubKey)
{
    if (!sSubKey) { g_subKey[0] = '\0'; return TRUE; }
    strncpy(g_subKey, sSubKey, sizeof(g_subKey) - 1);
    g_subKey[sizeof(g_subKey) - 1] = '\0';
    return TRUE;
}

// ----------------------------------------------------------------------- //
// Set
// ----------------------------------------------------------------------- //

BOOL CRegMgr::Set(const char* sKey, const char* sValue)
{
    if (!sKey) return FALSE;
    char full[512]; FullKey(sKey, full, sizeof(full));
    Put(full, sValue ? sValue : "");
    Save();
    return TRUE;
}

BOOL CRegMgr::Set(const char* sKey, DWORD nValue)
{
    if (!sKey) return FALSE;
    char full[512]; FullKey(sKey, full, sizeof(full));
    char buf[32]; snprintf(buf, sizeof(buf), "%u", (unsigned)nValue);
    Put(full, buf);
    Save();
    return TRUE;
}

// Binary values are hex-encoded so the store stays a readable text file.
BOOL CRegMgr::Set(const char* sKey, void* pValue, int nLen)
{
    if (!sKey || !pValue || nLen <= 0) return FALSE;
    char full[512]; FullKey(sKey, full, sizeof(full));

    char* hex = (char*)malloc((size_t)nLen * 2 + 3);
    if (!hex) return FALSE;
    hex[0] = '0'; hex[1] = 'x';
    const unsigned char* src = (const unsigned char*)pValue;
    for (int i = 0; i < nLen; ++i)
        snprintf(hex + 2 + i * 2, 3, "%02x", src[i]);

    Put(full, hex);
    free(hex);
    Save();
    return TRUE;
}

// ----------------------------------------------------------------------- //
// Get
// ----------------------------------------------------------------------- //

char* CRegMgr::Get(const char* sKey, char* sBuf, UINT32& nBufSize, const char* sDef)
{
    if (!sBuf || nBufSize == 0) return sBuf;

    char full[512]; FullKey(sKey, full, sizeof(full));
    RegEntry* p = sKey ? Find(full) : 0;
    const char* src = p ? p->value : (sDef ? sDef : "");

    strncpy(sBuf, src, nBufSize - 1);
    sBuf[nBufSize - 1] = '\0';
    nBufSize = (UINT32)strlen(sBuf);
    return sBuf;
}

DWORD CRegMgr::Get(const char* sKey, DWORD nDef)
{
    char full[512]; FullKey(sKey, full, sizeof(full));
    RegEntry* p = sKey ? Find(full) : 0;
    if (!p) return nDef;
    return (DWORD)strtoul(p->value, 0, 0);
}

void* CRegMgr::Get(const char* sKey, void* pBuf, UINT32& nBufSize,
                   void* pDef, UINT32 nDefSize)
{
    if (!pBuf || nBufSize == 0) return pBuf;

    char full[512]; FullKey(sKey, full, sizeof(full));
    RegEntry* p = sKey ? Find(full) : 0;

    if (!p || strncmp(p->value, "0x", 2) != 0)
    {
        if (pDef && nDefSize)
        {
            UINT32 n = (nDefSize < nBufSize) ? nDefSize : nBufSize;
            memcpy(pBuf, pDef, n);
            nBufSize = n;
        }
        else nBufSize = 0;
        return pBuf;
    }

    const char* hex = p->value + 2;
    size_t bytes = strlen(hex) / 2;
    if (bytes > nBufSize) bytes = nBufSize;

    unsigned char* dst = (unsigned char*)pBuf;
    for (size_t i = 0; i < bytes; ++i)
    {
        char octet[3] = { hex[i*2], hex[i*2+1], '\0' };
        dst[i] = (unsigned char)strtoul(octet, 0, 16);
    }
    nBufSize = (UINT32)bytes;
    return pBuf;
}

// ----------------------------------------------------------------------- //
// Delete
// ----------------------------------------------------------------------- //

BOOL CRegMgr::Delete(const char* sKey)
{
    if (!sKey) return FALSE;
    char full[512]; FullKey(sKey, full, sizeof(full));

    RegEntry* prev = 0;
    for (RegEntry* p = g_head; p; prev = p, p = p->next)
    {
        if (strcasecmp(p->key, full) != 0) continue;
        if (prev) prev->next = p->next; else g_head = p->next;
        free(p->key); free(p->value); free(p);
        g_dirty = true;
        Save();
        return TRUE;
    }
    return FALSE;
}

// Removes every key beneath the current subkey.
BOOL CRegMgr::DeleteSubKey()
{
    if (!g_subKey[0]) return FALSE;

    char prefix[300];
    snprintf(prefix, sizeof(prefix), "%s/", g_subKey);
    size_t n = strlen(prefix);

    RegEntry* prev = 0; RegEntry* p = g_head; BOOL any = FALSE;
    while (p)
    {
        if (strncasecmp(p->key, prefix, n) == 0)
        {
            RegEntry* dead = p;
            if (prev) prev->next = p->next; else g_head = p->next;
            p = p->next;
            free(dead->key); free(dead->value); free(dead);
            any = TRUE; g_dirty = true;
        }
        else { prev = p; p = p->next; }
    }
    if (any) Save();
    return any;
}

BOOL CRegMgr::DeleteUnderSubKey(const char* sKey)
{
    return Delete(sKey);
}

// Drops the entire store for this application.
BOOL CRegMgr::DeleteApp()
{
    FreeAll();
    g_dirty = true;
    Save();
    return TRUE;
}

BOOL CRegMgr::CreateKey(HKEY hKey, const char* sSubkey, HKEY& hNewKey)
{
    (void)hKey; (void)sSubkey;
    hNewKey = 0;
    return TRUE;    // no key hierarchy to create in a flat file store
}
