// ----------------------------------------------------------------------- //
//
// MODULE  : ltconsole.h
//
// PURPOSE : Console variables -- the game's tuning store.
//
// ⚠️ THIS IS NOT A DEBUG CONVENIENCE. IT IS LOAD-BEARING FOR THE 3D MENU.
//
//   The main menu's camera and its object layout are read from console
//   variables, not from constants. `GameClientShell.cpp:902` does
//
//       g_vtInterfceFOVX.Init(g_pLTClient, "FovXInterface", NULL, 90.0f);
//
//   and line 1153 then does
//
//       SetCameraFOV(m_hInterfaceCamera, DEG2RAD(g_vtInterfceFOVX.GetFloat()), ...)
//
//   ⚠️ `VarTrack::GetFloat(float defVal = 0.0f)` is called with NO ARGUMENT
//   there. So when the console store is a stub, the "default" is not the 90.0f
//   passed to Init -- it is **zero**. The camera gets a field of view of zero
//   degrees, `tan(0/2)` is 0, the aspect ratio becomes 0/0, and every vertex
//   in the scene transforms to NaN.
//
//   That is exactly what happened: the whole menu scene loaded correctly --
//   Cate's interface model with both DXT1 skins, the sunglasses, the Walther,
//   six menu FX -- and drew **0 pixels**, with no error anywhere. The loaders
//   were never at fault. Found by printing the camera's FOV and one vertex's
//   clip coordinate, not by reading the rasteriser.
//
// THE CONTRACT, from VarTrack.h:24 -- this is what a caller expects
//
//   1. `GetConsoleVar(name)` -> a handle, or NULL if the variable is unknown.
//   2. If NULL, the caller builds `"name" "value"` and hands it to
//      `RunConsoleString`, then asks again. If it is STILL null the caller
//      gives up and every later read silently returns 0.
//   3. `GetVarValueFloat` / `GetVarValueString` read it.
//
//   ⚠️ So step 2 must genuinely CREATE the variable. A RunConsoleString that
//   parses but does not store leaves every VarTrack un-initted, and the
//   failure is silent and total.
//
// ⚠️ HANDLES MUST BE STABLE
//
//   A VarTrack caches its HCONSOLEVAR once, at Init, and reads through it for
//   the rest of the run. So a handle has to stay valid and keep pointing at
//   the same variable no matter how many variables are added afterwards --
//   which is why the store hands out pointers to individually allocated
//   entries rather than into a vector that reallocates.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_CONSOLE_H__
#define __LT_CONSOLE_H__

#include <map>
#include <string>

// ⚠️ For ConsoleProgramFn (ltbasetypes.h:148). This header deliberately does
// NOT pull in the whole SDK -- the console is used by tools that link no game
// code -- so it takes the one typedef it needs and nothing else.
typedef void (*ConsoleProgramFn)(int argc, char **argv);

struct LTConsoleVar
{
    std::string     m_sName;        // as first seen
    std::string     m_sValue;
    float           m_fValue;
    bool            m_bHasFloat;
    // ⚠️ WRITTEN TO THE PLAYER'S CONFIG? 1 = yes, -1 = never, 0 = not marked.
    // The reference: '+name' sets VARFLAG_SAVE, '-name' forbids it, and every
    // variable a config file sets is marked (RunAutoConfig, clientmgr.cpp).
    int             m_nSave;
};

class CLTConsole
{
public:

    CLTConsole();
    ~CLTConsole();

    void            Term();

    // Case-insensitive, like the real console. Returns 0 if unknown.
    LTConsoleVar*   Find(const char* pName) const;

    // Creates or updates. Always returns the entry.
    LTConsoleVar*   Set(const char* pName, const char* pValue);

    // Parses one console line and applies it.
    //
    // Accepts the quoted form VarTrack builds (`"name" "value"`), the bare
    // form autoexec.cfg uses (`name value`), and mixtures. A line with no
    // value is a query and changes nothing.
    //
    // Returns the variable it touched, or 0.
    LTConsoleVar*   RunString(const char* pString);

    // ⚠️ CONSOLE PROGRAMS -- the game's own commands, and until now there was
    // nowhere to put them. NOLF registers FIFTEEN
    // (`RegisterConsoleProgram("Record", RecordFn)` and friends,
    // GameClientShell.cpp:996-1010) and not one of them could ever be invoked,
    // because RegisterConsoleProgram was a stub and RunString only ever set
    // variables. They are how the game exposes its own entry points -- and
    // `Record <world> <file>` is a real, game-owned way into
    // CGameClientShell::DoLoadWorld, which is the path that starts a level.
    //
    // ⚠️ A PROGRAM BEATS A VARIABLE OF THE SAME NAME. Otherwise `Record x y`
    // would silently create a console variable called Record and set it to
    // "x y", which succeeds, changes nothing, and reports nothing.
    void            RegisterProgram(const char* pName, ConsoleProgramFn fn);
    ConsoleProgramFn FindProgram(const char* pName) const;
    unsigned int    GetNumPrograms() const { return (unsigned int)m_Programs.size(); }

    // Every registered command name, for the diagnostic when one is not found.
    void            ListPrograms(std::string& sOut) const;

    float           GetFloat(const LTConsoleVar* pVar) const;
    const char*     GetString(const LTConsoleVar* pVar) const;

    unsigned int    GetNumVars() const { return (unsigned int)m_Vars.size(); }

    // Every variable, for the config writer (LTConfig_Write). Keyed upper-case.
    const std::map<std::string, LTConsoleVar*>& Vars() const { return m_Vars; }

    // While true, every variable RunString sets is marked for saving -- what
    // the reference does for each line of a config file it runs.
    void            SetSaveOnSet(bool b) { m_bSaveOnSet = b; }

    // Loads a config file through the file manager, one console line per line.
    //
    // ⚠️ READ-ONLY, and it has to stay that way: `autoexec.cfg` lives inside
    // the user's real install. PORT_PLAN.md §5. Nothing here writes.
    int             RunConfigFile(const char* pFilename);

private:

    // ⚠️ Pointers to individually allocated entries, never indices into a
    // vector -- see the header comment on handle stability.
    std::map<std::string, LTConsoleVar*>    m_Vars;
    bool                                    m_bSaveOnSet = false;

    // ⚠️ Case-insensitive by construction: the key is lowercased on both
    // insert and lookup. The game registers "Record" and a person types
    // "record", and a console that cared would be wrong in the way that is
    // hardest to argue with -- it looks like the command does not exist.
    std::map<std::string, ConsoleProgramFn>  m_Programs;
};

CLTConsole* LTGetConsole();

// Assigns GetConsoleVar and RunConsoleString, which are function POINTERS.
// ⚠️ GetVarValueFloat/String are VIRTUALS and live on the host class instead --
// see skeleton_host.cpp. Splitting them is not a style choice; ILTClient is a
// hybrid and there is no single place both can be installed.
class ILTClient;
void LTConsole_Wire(ILTClient* p);

#endif // __LT_CONSOLE_H__
