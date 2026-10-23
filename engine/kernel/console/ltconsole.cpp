// ----------------------------------------------------------------------- //
//
// MODULE  : ltconsole.cpp
//
// PURPOSE : Console variable store. See ltconsole.h for why it is load-bearing.
//
// ----------------------------------------------------------------------- //

#include "ltconsole.h"

#include <ctype.h>
#include <vector>

#include "windows.h"
#include "lithtech.h"

#include "ltfilemgr.h"
#include "ltstub.h"

#include "iltclient.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::string Upper(const char* p)
{
    std::string s;
    for (; p && *p; ++p) s += (char)toupper((unsigned char)*p);
    return s;
}

CLTConsole::CLTConsole() {}
CLTConsole::~CLTConsole() { Term(); }

void CLTConsole::Term()
{
    for (std::map<std::string, LTConsoleVar*>::iterator i = m_Vars.begin();
         i != m_Vars.end(); ++i) delete i->second;
    m_Vars.clear();
}

LTConsoleVar* CLTConsole::Find(const char* pName) const
{
    if (!pName || !pName[0]) return 0;
    std::map<std::string, LTConsoleVar*>::const_iterator i = m_Vars.find(Upper(pName));
    return (i == m_Vars.end()) ? 0 : i->second;
}

LTConsoleVar* CLTConsole::Set(const char* pName, const char* pValue)
{
    if (!pName || !pName[0]) return 0;

    std::string sKey = Upper(pName);
    LTConsoleVar* pVar = 0;

    std::map<std::string, LTConsoleVar*>::iterator i = m_Vars.find(sKey);
    if (i != m_Vars.end())
    {
        pVar = i->second;
    }
    else
    {
        pVar = new LTConsoleVar;
        pVar->m_sName = pName;
        pVar->m_nSave = 0;
        m_Vars[sKey] = pVar;
    }

    pVar->m_sValue = pValue ? pValue : "";

    // ⚠️ The float is parsed ONCE, here, and remembered -- including whether
    // the parse succeeded. GetVarValueFloat is read every frame by things like
    // the camera FOV, and re-running strtod per read would be both wasteful
    // and a place for a partial parse to differ between calls.
    const char* p = pVar->m_sValue.c_str();
    char* pEnd = 0;
    double d = strtod(p, &pEnd);
    pVar->m_bHasFloat = (pEnd != p);
    pVar->m_fValue = pVar->m_bHasFloat ? (float)d : 0.0f;

    return pVar;
}

float CLTConsole::GetFloat(const LTConsoleVar* pVar) const
{
    return pVar ? pVar->m_fValue : 0.0f;
}

const char* CLTConsole::GetString(const LTConsoleVar* pVar) const
{
    return pVar ? pVar->m_sValue.c_str() : "";
}

// Splits a console line into tokens, honouring double quotes.
static void Tokenise(const char* p, std::vector<std::string>& out)
{
    out.clear();
    while (p && *p)
    {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        // ⚠️ A comment. autoexec.cfg carries them and a token like "//" would
        // otherwise become a variable name.
        if (p[0] == '/' && p[1] == '/') break;
        if (p[0] == '#' || p[0] == ';') break;

        std::string tok;
        if (*p == '"')
        {
            ++p;
            while (*p && *p != '"') tok += *p++;
            if (*p == '"') ++p;
        }
        else
        {
            while (*p && !isspace((unsigned char)*p)) tok += *p++;
        }
        out.push_back(tok);
    }
}

LTConsoleVar* CLTConsole::RunString(const char* pString)
{
    if (!pString) return 0;

    std::vector<std::string> tok;
    Tokenise(pString, tok);
    if (tok.empty()) return 0;

    // ⚠️ A REGISTERED PROGRAM WINS, and it is checked FIRST -- before the
    // one-token query and before any assignment. `Record x y` must run the
    // game's RecordFn, not create a variable called Record; and `ExitLevel`
    // alone must run, not be answered as a variable lookup that finds nothing.
    if (ConsoleProgramFn fn = FindProgram(tok[0].c_str()))
    {
        // ⚠️ argv EXCLUDES the command name. NOLF's handlers index from zero:
        //     `Record <world name> <filename>` reads argv[0] and argv[1]
        //     (GameClientShell.cpp:4735-4742). Passing the name as argv[0]
        //     would hand every handler its own name as its first argument.
        std::vector<char*> argv;
        for (size_t i = 1; i < tok.size(); ++i)
            argv.push_back(const_cast<char*>(tok[i].c_str()));

        fn((int)argv.size(), argv.empty() ? 0 : &argv[0]);
        return 0;
    }

    // ⚠️ One token is a QUERY, not "set to empty". Setting it empty would
    // wipe a variable every time something merely asked for it.
    if (tok.size() == 1) return Find(tok[0].c_str());

    // Everything after the name joins back together, so `bind "##17" "Forward"`
    // keeps its argument list rather than losing all but the first word.
    std::string sValue = tok[1];
    for (size_t i = 2; i < tok.size(); ++i)
    {
        sValue += " ";
        sValue += tok[i];
    }

    // ⚠️ "+name value" SETS `name`, it does not create "+name" (HANDOVER 5bp).
    // The donor's console (concommand.cpp:224) reads a leading '+' as "mark
    // this variable for saving" and a leading '-' as "never save it", and
    // strips either before the lookup. NOLF's WriteConsoleInt/Float/String
    // (ClientUtilities.cpp) ALWAYS write the '+' form -- FarZ, Difficulty,
    // MouseSensitivity, the menu's and loading screen's FogEnable, 44 names in
    // all -- so without this every one of them landed in a variable nothing
    // reads. The mark is kept as m_nSave and decides what LTConfig_Write saves
    // to the player's config (HANDOVER 5cc). LT_CONKEEPPLUS=1 = the old
    // behaviour (says so).
    static const bool s_bKeepPlus = []() {
        const bool b = (getenv("LT_CONKEEPPLUS") != 0);
        if (b) { printf("[CONSOLE] LT_CONKEEPPLUS=1 -- '+name' / '-name' kept as literal names\n"); fflush(stdout); }
        return b; }();
    const char* pName = tok[0].c_str();
    int nMark = 0;
    if (!s_bKeepPlus && (pName[0] == '+' || pName[0] == '-') && pName[1])
    {
        nMark = (pName[0] == '+') ? 1 : -1;
        ++pName;
    }
    LTConsoleVar* pVar = Set(pName, sValue.c_str());
    // The save mark (LTConfig_Write): '+' saves, '-' never; a config file's
    // own lines save unless the variable was already forbidden.
    if (pVar)
    {
        if (nMark) pVar->m_nSave = nMark;
        else if (m_bSaveOnSet && pVar->m_nSave == 0) pVar->m_nSave = 1;
    }
    return pVar;
}

namespace
{
    std::string LowerOf(const char* p)
    {
        std::string s(p ? p : "");
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = (char)tolower((unsigned char)s[i]);
        return s;
    }
}

void CLTConsole::RegisterProgram(const char* pName, ConsoleProgramFn fn)
{
    if (!pName || !*pName || !fn) return;
    m_Programs[LowerOf(pName)] = fn;
}

ConsoleProgramFn CLTConsole::FindProgram(const char* pName) const
{
    if (!pName || !*pName) return 0;
    std::map<std::string, ConsoleProgramFn>::const_iterator i =
        m_Programs.find(LowerOf(pName));
    return (i == m_Programs.end()) ? 0 : i->second;
}

void CLTConsole::ListPrograms(std::string& sOut) const
{
    sOut.clear();
    for (std::map<std::string, ConsoleProgramFn>::const_iterator i = m_Programs.begin();
         i != m_Programs.end(); ++i)
    {
        if (!sOut.empty()) sOut += ", ";
        sOut += i->first;
    }
}


int CLTConsole::RunConfigFile(const char* pFilename)
{
    CLTFileMgr* pFM = LTGetFileMgr();
    if (!pFM || !pFilename) return 0;

    ILTStream* pStream = pFM->OpenFile(pFilename);
    if (!pStream) return 0;

    unsigned long nLen = 0;
    pStream->GetLen(&nLen);

    std::vector<char> buf((size_t)nLen + 1, 0);
    bool bOK = (nLen > 0) && (pStream->Read(&buf[0], nLen) == LT_OK);
    pStream->Release();
    if (!bOK) return 0;

    int nRun = 0;
    const char* p = &buf[0];
    std::string sLine;
    for (unsigned long i = 0; i <= nLen; ++i)
    {
        char c = (i < nLen) ? buf[i] : '\n';
        if (c == '\r' || c == '\n')
        {
            if (!sLine.empty())
            {
                if (RunString(sLine.c_str())) ++nRun;
                sLine.clear();
            }
        }
        else sLine += c;
    }
    (void)p;
    return nRun;
}

CLTConsole* LTGetConsole()
{
    static CLTConsole s_Console;
    return &s_Console;
}


// ----------------------------------------------------------------------- //

static HCONSOLEVAR CON_GetConsoleVar(char* pName)
{
    LTIMPL("ILTClient::GetConsoleVar");
    return (HCONSOLEVAR)LTGetConsole()->Find(pName);
}

static void CON_RunConsoleString(char* pString)
{
    LTIMPL("ILTClient::RunConsoleString");

    // ⚠️ LT_CONTRACE=1 -- what the GAME writes to its own console, which is
    // often the only externally visible trace of a decision it made. The
    // cinematic camera is the case that earned this: TurnOnAlternativeCamera
    // writes MaxModelShadows 0 and TurnOffAlternativeCamera writes it back
    // (GameClientShell.cpp:2526, :2558), so the pair says whether the game
    // entered a cinematic camera and whether it ever left -- and leaving is
    // what re-enables the player's weapon.
    if (pString && getenv("LT_CONTRACE"))
    {
        printf("[CON] game: %s\n", pString);
        fflush(stdout);
    }

    LTGetConsole()->RunString(pString);
}

static LTRESULT CON_RegisterConsoleProgram(char* pName, ConsoleProgramFn fn)
{
    LTIMPL("ILTClient::RegisterConsoleProgram");
    if (!pName || !fn) return LT_ERROR;
    LTGetConsole()->RegisterProgram(pName, fn);
    return LT_OK;
}

void LTConsole_Wire(ILTClient* p)
{
    if (!p) return;
    p->GetConsoleVar    = CON_GetConsoleVar;
    p->RunConsoleString = CON_RunConsoleString;
    p->RegisterConsoleProgram = CON_RegisterConsoleProgram;
}
