// ----------------------------------------------------------------------- //
//
// MODULE  : ltbindings.cpp
//
// See ltbindings.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltbindings.h"
#include "ltwindow.h"
#include <sys/stat.h>
#include <stdio.h>

// Windows OEM virtual keys -- the punctuation row. Not in compat/windows.h
// (nothing in the game names them); the values are winuser.h's.
#ifndef VK_OEM_1
#define VK_OEM_1        0xBA    // ;:
#define VK_OEM_PLUS     0xBB    // =+
#define VK_OEM_COMMA    0xBC    // ,<
#define VK_OEM_MINUS    0xBD    // -_
#define VK_OEM_PERIOD   0xBE    // .>
#define VK_OEM_2        0xBF    // /?
#define VK_OEM_3        0xC0    // `~
#define VK_OEM_4        0xDB    // [{
#define VK_OEM_5        0xDC    // \|
#define VK_OEM_6        0xDD    // ]}
#define VK_OEM_7        0xDE    // '"
#define VK_OEM_102      0xE2    // the ISO key left of 1 / next to left shift
#endif
#include "ltinput.h"
#include "ltconsole.h"
#include "ltfilemgr.h"
#include "iltstream.h"

#include <map>
#include <set>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace
{
    // action name (lowercased) -> command id, from `AddAction`
    std::map<std::string, int> g_Actions;
    std::map<int, std::string> g_ActionNames;   // code -> name as written (for the saved config)

    // ⚠️ Mouse button -> command id. Separate from the key table because the
    // two namespaces collide -- see Cmd_RangeBind.
    std::map<int, int> g_MouseToCommand;

    // DIK scan code -> command id, from `rangebind`
    std::map<int, int>         g_DikToCommand;

    // ⚠️ DIAGNOSTIC ONLY -- the action name each binding came from, so the
    // census can say "Shift -> NextWeapon -> 67" instead of "Shift=67". A
    // number alone cannot tell a wrong binding from a missing one.
    std::map<int, std::string> g_DikToAction;
    std::map<int, std::string> g_MouseToAction;

    // The wheel's two notches (command ids, -1 = unbound) and the commands a
    // notch turned on this frame -- see LTBind_OnWheel.
    int g_WheelFwd = -1, g_WheelBack = -1;
    std::vector<int> g_Pulses;

    // ⚠️ EVERY TRIGGER THAT WAS EVER BOUND THIS SESSION (5dv). The player's
    // config is LAYERED over the install's (5cc), so a key the player took
    // away must be written as an explicit unbind, or the install's
    // autoexec.cfg binds it again at the next start.
    std::set<int> g_DikEver, g_ButtonEver;
    bool          g_bWheelEver = false;

    unsigned g_nUnboundKeys = 0;
    unsigned g_nKeyHits     = 0;

    std::string Lower(const char* p)
    {
        std::string s(p ? p : "");
        for (size_t i = 0; i < s.size(); ++i)
            if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
        return s;
    }

    // ⚠️ Strips the `##` and any quotes autoexec.cfg wraps a token in.
    // `"##keyboard"` and `##17` both appear, and so do bare names.
    const char* Strip(const char* p, char* buf, size_t n)
    {
        if (!p) { buf[0] = 0; return buf; }
        while (*p == '"' || *p == ' ') ++p;
        if (p[0] == '#' && p[1] == '#') p += 2;

        size_t k = 0;
        while (*p && *p != '"' && k + 1 < n) buf[k++] = *p++;
        buf[k] = 0;
        return buf;
    }

    // ------------------------------------------------------------------ //
    // ⚠️ WINDOWS VIRTUAL KEY -> DIK SCAN CODE.
    //
    // This port's input events carry VK (ltinput.cpp explains at length why),
    // and `rangebind` speaks DIK. This table is the bridge, and it covers what
    // NOLF's shipped autoexec.cfg actually binds -- letters, digits, arrows,
    // the modifiers and the handful of named keys. A VK with no entry is
    // reported by the census rather than silently dropped.
    // ------------------------------------------------------------------ //
    int VKToDIK(int vk)
    {
        // Letters: DIK is a scan-code layout, not alphabetical.
        static const struct { int vk; int dik; } kLetters[] = {
            {'A',30},{'B',48},{'C',46},{'D',32},{'E',18},{'F',33},{'G',34},
            {'H',35},{'I',23},{'J',36},{'K',37},{'L',38},{'M',50},{'N',49},
            {'O',24},{'P',25},{'Q',16},{'R',19},{'S',31},{'T',20},{'U',22},
            {'V',47},{'W',17},{'X',45},{'Y',21},{'Z',44}
        };
        for (size_t i = 0; i < sizeof(kLetters)/sizeof(kLetters[0]); ++i)
            if (kLetters[i].vk == vk) return kLetters[i].dik;

        // Digits 1..9,0 are DIK 2..11 in order.
        if (vk >= '1' && vk <= '9') return 2 + (vk - '1');
        if (vk == '0') return 11;

        switch (vk)
        {
            case VK_ESCAPE:  return 1;
            case VK_BACK:    return 14;
            case VK_TAB:     return 15;
            case VK_RETURN:  return 28;
            case VK_CONTROL: return 29;
            case VK_LSHIFT:  case VK_SHIFT: return 42;
            case VK_RSHIFT:  return 54;
            case VK_MENU:    return 56;      // Alt
            case VK_SPACE:   return 57;
            case VK_CAPITAL: return 58;
            case VK_F1:      return 59;
            case VK_F2:      return 60;
            case VK_F3:      return 61;
            case VK_F4:      return 62;
            case VK_F5:      return 63;
            case VK_F6:      return 64;
            case VK_F7:      return 65;
            case VK_F8:      return 66;
            case VK_F9:      return 67;
            case VK_F10:     return 68;
            case VK_F11:     return 87;
            case VK_F12:     return 88;
            case VK_UP:      return 200;
            case VK_LEFT:    return 203;
            case VK_RIGHT:   return 205;
            case VK_DOWN:    return 208;
            case VK_PRIOR:   return 201;     // PageUp
            case VK_NEXT:    return 209;     // PageDown
            case VK_HOME:    return 199;
            case VK_END:     return 207;
            case VK_INSERT:  return 210;
            case VK_DELETE:  return 211;
            // Left/right modifiers and the punctuation row (see ltinput.cpp).
            case VK_LCONTROL: return 29;
            case VK_RCONTROL: return 157;
            case VK_LMENU:    return 56;
            case VK_RMENU:    return 184;
            case VK_OEM_MINUS:  return 12;
            case VK_OEM_PLUS:   return 13;
            case VK_OEM_4:      return 26;
            case VK_OEM_6:      return 27;
            case VK_OEM_1:      return 39;
            case VK_OEM_7:      return 40;
            case VK_OEM_3:      return 41;
            case VK_OEM_5:      return 43;
            case VK_OEM_COMMA:  return 51;
            case VK_OEM_PERIOD: return 52;
            case VK_OEM_2:      return 53;
            case VK_OEM_102:    return 86;
            case VK_NUMPAD7:  return 71;  case VK_NUMPAD8: return 72;  case VK_NUMPAD9: return 73;
            case VK_SUBTRACT: return 74;
            case VK_NUMPAD4:  return 75;  case VK_NUMPAD5: return 76;  case VK_NUMPAD6: return 77;
            case VK_ADD:      return 78;
            case VK_NUMPAD1:  return 79;  case VK_NUMPAD2: return 80;  case VK_NUMPAD3: return 81;
            case VK_NUMPAD0:  return 82;  case VK_DECIMAL: return 83;
            case VK_MULTIPLY: return 55;  case VK_DIVIDE:  return 181;
            default: break;
        }
        return -1;
    }


    bool                     g_bTracking = false;
    std::vector<DeviceInput> g_Tracked;
    size_t                   g_nTrackCap = 0;
    unsigned                 g_nTrackDevices = 0;    // StartDeviceTrack's DEVICETYPE_ mask

    // A mouse event for the key-assignment screen (5dv): DirectInput's names,
    // "Button N" and "Wheel" -- what GetDeviceObjects and GetDeviceBindings
    // answer, since the screen matches all three by name.
    void TrackMouse(unsigned nControlType, const char* pName, unsigned nValue)
    {
        if (!g_bTracking || !(g_nTrackDevices & DEVICETYPE_MOUSE) || g_Tracked.size() >= g_nTrackCap) return;
        DeviceInput di;
        memset(&di, 0, sizeof(di));
        di.m_DeviceType = DEVICETYPE_MOUSE;
        strncpy(di.m_DeviceName, "##mouse", sizeof(di.m_DeviceName) - 1);
        di.m_ControlType = nControlType;
        strncpy(di.m_ControlName, pName, sizeof(di.m_ControlName) - 1);
        di.m_InputValue = nValue;
        g_Tracked.push_back(di);
    }

    // The name a person should see on the Controls screen. Only used for
    // display -- the BINDING travels as the DIK code in byte 63.
    // ⚠️ ONLY LETTERS AND DIGITS ARE THEIR OWN CHARACTER (HANDOVER 5dv).
    // Every VK from 32 to 126 used to print as its ASCII code before the
    // names below were tried -- and VK_LEFT/UP/RIGHT/DOWN are 37..40, so the
    // Controls screen showed '%' '&' ''' '(' for the arrows, ' ' for Space,
    // '.' for Delete. A single character also goes through the game's
    // ConvertKeyToCurrentKeyboard, which must get the ENGLISH key.
    void VKName(int nVK, int nDik, char* pOut, size_t n)
    {
        if ((nVK >= '0' && nVK <= '9') || (nVK >= 'A' && nVK <= 'Z'))
        { snprintf(pOut, n, "%c", (char)nVK); return; }

        switch (nVK)
        {
            case VK_SPACE:   snprintf(pOut, n, "Space");   return;
            case VK_RETURN:  snprintf(pOut, n, "Enter");   return;
            case VK_ESCAPE:  snprintf(pOut, n, "Escape");  return;
            case VK_TAB:     snprintf(pOut, n, "Tab");     return;
            case VK_SHIFT:   snprintf(pOut, n, "Shift");   return;
            case VK_CONTROL: snprintf(pOut, n, "Ctrl");    return;
            case VK_MENU:    snprintf(pOut, n, "Alt");     return;
            case VK_UP:      snprintf(pOut, n, "Up");      return;
            case VK_DOWN:    snprintf(pOut, n, "Down");    return;
            case VK_LEFT:    snprintf(pOut, n, "Left");    return;
            case VK_RIGHT:   snprintf(pOut, n, "Right");   return;
            case VK_RSHIFT:   snprintf(pOut, n, "Right Shift"); return;
            case VK_RCONTROL: snprintf(pOut, n, "Right Ctrl");  return;
            case VK_RMENU:    snprintf(pOut, n, "Right Alt");   return;
            case VK_CAPITAL:  snprintf(pOut, n, "CapsLock");    return;
            case VK_BACK:     snprintf(pOut, n, "Backspace");   return;
            case VK_INSERT:   snprintf(pOut, n, "Insert");      return;
            case VK_DELETE:   snprintf(pOut, n, "Delete");      return;
            case VK_PRIOR:    snprintf(pOut, n, "PgUp");        return;
            case VK_NEXT:     snprintf(pOut, n, "PgDn");        return;
            case VK_HOME:     snprintf(pOut, n, "Home");        return;
            case VK_END:      snprintf(pOut, n, "End");         return;
            case VK_OEM_MINUS:  snprintf(pOut, n, "-");  return;
            case VK_OEM_PLUS:   snprintf(pOut, n, "=");  return;
            case VK_OEM_4:      snprintf(pOut, n, "[");  return;
            case VK_OEM_6:      snprintf(pOut, n, "]");  return;
            case VK_OEM_1:      snprintf(pOut, n, ";");  return;
            case VK_OEM_7:      snprintf(pOut, n, "'");  return;
            case VK_OEM_3:      snprintf(pOut, n, "`");  return;
            case VK_OEM_5:      snprintf(pOut, n, "\\"); return;
            case VK_OEM_COMMA:  snprintf(pOut, n, ",");  return;
            case VK_OEM_PERIOD: snprintf(pOut, n, ".");  return;
            case VK_OEM_2:      snprintf(pOut, n, "/");  return;
            default: break;
        }
        if (nVK >= VK_F1 && nVK <= VK_F12) { snprintf(pOut, n, "F%d", nVK - VK_F1 + 1); return; }
        if (nVK >= VK_NUMPAD0 && nVK <= VK_NUMPAD9) { snprintf(pOut, n, "Num %d", nVK - VK_NUMPAD0); return; }
        switch (nVK)
        {
            case VK_MULTIPLY: snprintf(pOut, n, "Num *"); return;
            case VK_ADD:      snprintf(pOut, n, "Num +"); return;
            case VK_SUBTRACT: snprintf(pOut, n, "Num -"); return;
            case VK_DECIMAL:  snprintf(pOut, n, "Num ."); return;
            case VK_DIVIDE:   snprintf(pOut, n, "Num /"); return;
            case VK_OEM_102:  snprintf(pOut, n, "ISO Key"); return;
            default: break;
        }
        snprintf(pOut, n, "Key %d", nDik);
    }

    // ⚠️ ONE NAME PER DIK, whatever VK the event carried. Shift arrives as
    // VK_SHIFT or VK_LSHIFT, both DIK 42; naming by VK gave one key two names,
    // and the screen matches bindings, tracked keys and device objects BY
    // NAME. The lowest VK that maps to the DIK names it.
    void KeyDisplayName(int nVK, int nDik, char* pOut, size_t n)
    {
        (void)nVK;
        for (int vk = 1; vk < 256; ++vk)
            if (VKToDIK(vk) == nDik) { VKName(vk, nDik, pOut, n); return; }
        snprintf(pOut, n, "Key %d", nDik);
    }

    // The other way: a key's NAME back to its DIK, case-insensitive, for
    // `rangebind "##keyboard" "W" ...` -- the game unbinds a key by the name
    // GetDeviceBindings gave it (FolderCustomControls.cpp:349).
    int DikForName(const char* pName)
    {
        if (!pName || !*pName) return -1;
        for (int dik = 1; dik < 256; ++dik)
        {
            char buf[64];
            buf[0] = 0;
            bool bAny = false;
            for (int vk = 1; vk < 256 && !bAny; ++vk) bAny = (VKToDIK(vk) == dik);
            if (!bAny) continue;
            KeyDisplayName(0, dik, buf, sizeof buf);
            if (strcasecmp(buf, pName) == 0) return dik;
        }
        return -1;
    }

    // ---- console commands --------------------------------------------- //

    // AddAction <name> <commandId>
    //
    // ⚠️ argv EXCLUDES the command name (ltconsole.cpp:139), so argv[0] is
    // <name>, not "AddAction". Getting this backwards silently registers
    // nothing: every line parses, every lookup fails, and the census reports a
    // keyboard with no keys.
    void Cmd_AddAction(int argc, char** argv)
    {
        if (argc < 2) return;
        char a[128], b[64];
        Strip(argv[0], a, sizeof a);
        Strip(argv[1], b, sizeof b);
        g_Actions[Lower(a)] = atoi(b);
        g_ActionNames[atoi(b)] = a;
    }

    // rangebind "##keyboard" "##<dik>" <min> <max> "<action>"
    // bind      "##keyboard" "##<dik>" "<action>"
    //
    // ⚠️ THE CONTROLS SCREEN SPEAKS IT TOO (5dv), in three ways the shipped
    // config never does:
    //   * the trigger by NAME -- `"W"`, not `"##17"` -- taken from
    //     GetDeviceBindings; `##` decides, so the name "1" is the 1 key and
    //     `##1` is DIK 1 (Escape);
    //   * an EMPTY action, `""`, which UNBINDS the trigger (UnBind,
    //     FolderCustomControls.cpp:349) -- it used to return, so a re-bound
    //     action kept its old key as well;
    //   * the wheel line with ONE range or none (RebindWheel): the line
    //     replaces the trigger's actions, so the other notch is cleared.
    void Cmd_RangeBind(int argc, char** argv)
    {
        if (argc < 2) return;

        char dev[64], key[64], act[128];
        Strip(argv[0], dev, sizeof dev);
        Strip(argv[1], key, sizeof key);
        const char* pRawKey = argv[1] ? argv[1] : "";
        while (*pRawKey == '"' || *pRawKey == ' ') ++pRawKey;
        const bool bCode = (pRawKey[0] == '#' && pRawKey[1] == '#');

        const std::string sDev = Lower(dev);

        // The wheel: "##z-axis" then (lo hi action) triples. A range below
        // zero is the backward notch, above zero the forward one.
        if (sDev == "mouse" && (Lower(key) == "z-axis" || Lower(key) == "wheel"))
        {
            g_WheelFwd = g_WheelBack = -1;
            g_bWheelEver = true;
            for (int a = 2; a + 2 < argc; a += 3)
            {
                char act2[128];
                Strip(argv[a + 2], act2, sizeof act2);
                std::map<std::string, int>::const_iterator jt = g_Actions.find(Lower(act2));
                if (jt == g_Actions.end()) continue;
                const float lo = (float)atof(argv[a]), hi = (float)atof(argv[a + 1]);
                if (hi <= 0.0f || lo < 0.0f) g_WheelBack = jt->second;
                else                         g_WheelFwd  = jt->second;
            }
            return;
        }

        // The action is the LAST argument for both forms, which is why this
        // takes it by position from the end rather than by index. Empty =
        // unbind; a name the game never added = the line is ignored.
        act[0] = 0;
        if (argc >= 3) Strip(argv[argc - 1], act, sizeof act);
        int nCmd = -1;
        if (act[0])
        {
            std::map<std::string, int>::const_iterator it = g_Actions.find(Lower(act));
            if (it == g_Actions.end()) return;
            nCmd = it->second;
        }

        // ⚠️ THE MOUSE IS A SEPARATE TABLE, NOT A SHARED ONE. Mouse button 1
        // and DIK 1 (Escape) are both spelled "1", so one map would have them
        // overwrite each other. This used to `return` here instead -- which
        // meant NO MOUSE BUTTON COULD EVER PRODUCE A COMMAND.
        //
        // ⚠️ AND THAT IS WHERE FIRING LIVES. The shipped autoexec.cfg binds:
        //     rangebind "##mouse" "Button 0" ... "Fire"
        //     rangebind "##mouse" "Button 1" ... "Activate"
        //     rangebind "##mouse" "Button 2" ... "Reload"
        // so dropping them silently removed the primary control for shooting
        // and one of the two ways to use anything in the world.
        //
        // ⚠️ The key is spelled "Button N", not "##N" -- a different syntax in
        // the same command, which is why this parses past the word rather than
        // calling atoi on the whole string.
        if (sDev == "mouse")
        {
            const char* p = key;
            while (*p && (*p < '0' || *p > '9')) ++p;   // skip "Button "
            if (!*p) return;

            const int nButton = atoi(p);
            if (nButton < 0 || nButton > 7) return;

            g_ButtonEver.insert(nButton);
            if (nCmd < 0) { g_MouseToCommand.erase(nButton); g_MouseToAction.erase(nButton); return; }
            g_MouseToCommand[nButton] = nCmd;
            g_MouseToAction[nButton]  = act;
            return;
        }

        if (sDev != "keyboard") return;

        const int nDik = bCode ? atoi(key) : DikForName(key);
        if (nDik <= 0 || nDik > 255) return;

        g_DikEver.insert(nDik);
        if (nCmd < 0) { g_DikToCommand.erase(nDik); g_DikToAction.erase(nDik); return; }
        g_DikToCommand[nDik] = nCmd;
        g_DikToAction[nDik]  = act;
    }
}


void LTBind_Register()
{
    LTGetConsole()->RegisterProgram("AddAction", Cmd_AddAction);
    LTGetConsole()->RegisterProgram("rangebind", Cmd_RangeBind);
    LTGetConsole()->RegisterProgram("bind",      Cmd_RangeBind);
}


int LTBind_LoadConfig(const char* pPath)
{
    if (!pPath || !*pPath) return 0;

    // ⚠️ THROUGH THE FILE MANAGER, READ-ONLY. The install is opened read-only
    // by construction (there is no write path), which is what keeps the rule
    // about never touching FULLGAME true by design rather than by care.
    ILTStream* pS = LTGetFileMgr()->OpenFile(pPath);
    if (!pS)
    {
        fprintf(stderr, "[BIND] could not read %s -- no key bindings.\n", pPath);
        return 0;
    }

    unsigned long nLen = 0;
    pS->GetLen(&nLen);
    if (!nLen) { pS->Release(); return 0; }

    std::vector<char> data((size_t)nLen);
    pS->Read(&data[0], nLen);
    pS->Release();

    std::string all(&data[0], (size_t)nLen);
    int nLines = 0;
    size_t from = 0;
    // Every variable a config file sets is the player's setting (the
    // reference runs it with VARFLAG_SAVE) -- see LTConfig_Write.
    LTGetConsole()->SetSaveOnSet(true);

    while (from <= all.size())
    {
        size_t end = all.find('\n', from);
        if (end == std::string::npos) end = all.size();

        std::string line = all.substr(from, end - from);
        from = end + 1;

        while (!line.empty() && (line[line.size()-1] == '\r' ||
                                 line[line.size()-1] == ' ')) line.erase(line.size()-1);
        if (line.empty()) continue;

        LTGetConsole()->RunString(line.c_str());
        ++nLines;
    }

    LTGetConsole()->SetSaveOnSet(false);
    return nLines;
}


// ----------------------------------------------------------------------- //
// The player's config -- see ltbindings.h.
// ----------------------------------------------------------------------- //

namespace
{
    bool g_bConfigCanSave = false;

    std::string ConfigPath(const char* pName)
    {
        // The NAME only: the game passes "autoexec.cfg"; anything with a path
        // is cut to its last component, so no write can leave the directory.
        std::string s(pName ? pName : "autoexec.cfg");
        const size_t k = s.find_last_of("/\\");
        if (k != std::string::npos) s = s.substr(k + 1);
        if (s.empty() || s == "." || s == "..") s = "autoexec.cfg";
        return std::string(LTConfig_Dir()) + "/" + s;
    }

    // The reference's input_SaveBindings, from what this binding table holds.
    void SaveBindings(FILE* fp)
    {
        for (std::map<int, std::string>::const_iterator it = g_ActionNames.begin();
             it != g_ActionNames.end(); ++it)
            fprintf(fp, "AddAction %s %d\n", it->second.c_str(), it->first);
        fprintf(fp, "\n");

        // ⚠️ A TRIGGER THE PLAYER UNBOUND IS WRITTEN AS AN UNBIND (5dv) --
        // `""` for its action, the Controls screen's own form -- because this
        // file is read AFTER the install's autoexec.cfg, which binds it again.
        if (!g_DikToAction.empty() || !g_DikEver.empty())
        {
            fprintf(fp, "enabledevice \"##keyboard\"\n");
            for (std::map<int, std::string>::const_iterator it = g_DikToAction.begin();
                 it != g_DikToAction.end(); ++it)
                fprintf(fp, "rangebind \"##keyboard\" \"##%d\" 0.000000 0.000000 \"%s\" \n",
                        it->first, it->second.c_str());
            for (std::set<int>::const_iterator it = g_DikEver.begin(); it != g_DikEver.end(); ++it)
                if (!g_DikToAction.count(*it))
                    fprintf(fp, "rangebind \"##keyboard\" \"##%d\" 0.000000 0.000000 \"\" \n", *it);
        }
        if (!g_MouseToAction.empty() || !g_ButtonEver.empty() || g_bWheelEver
            || g_WheelFwd >= 0 || g_WheelBack >= 0)
        {
            fprintf(fp, "enabledevice \"##mouse\"\n");
            // The wheel as ONE line, whatever is bound -- the line replaces
            // both notches, so an empty one clears them.
            fprintf(fp, "rangebind \"##mouse\" \"##z-axis\"");
            if (g_WheelBack >= 0 && g_ActionNames.count(g_WheelBack))
                fprintf(fp, " -255.000000 -0.100000 \"%s\"", g_ActionNames[g_WheelBack].c_str());
            if (g_WheelFwd >= 0 && g_ActionNames.count(g_WheelFwd))
                fprintf(fp, " 0.100000 255.000000 \"%s\"", g_ActionNames[g_WheelFwd].c_str());
            fprintf(fp, "\n");
            for (std::map<int, std::string>::const_iterator it = g_MouseToAction.begin();
                 it != g_MouseToAction.end(); ++it)
                fprintf(fp, "rangebind \"##mouse\" \"Button %d\" 0.000000 0.000000 \"%s\" \n",
                        it->first, it->second.c_str());
            for (std::set<int>::const_iterator it = g_ButtonEver.begin(); it != g_ButtonEver.end(); ++it)
                if (!g_MouseToAction.count(*it))
                    fprintf(fp, "rangebind \"##mouse\" \"Button %d\" 0.000000 0.000000 \"\" \n", *it);
        }
    }
}

bool LTConfig_Enabled()
{
    static const int s_n = []() {
        if (getenv("LT_NOUSERCONFIG"))
        {
            printf("[CONFIG] LT_NOUSERCONFIG=1 -- the player's config is neither read nor written\n");
            fflush(stdout);
            return 0;
        }
        return (getenv("LT_CONFIGDIR") || LTWindow_Wanted()) ? 1 : 0;
    }();
    return s_n != 0;
}

const char* LTConfig_Dir()
{
    static std::string s;
    if (s.empty())
    {
        if (const char* p = getenv("LT_CONFIGDIR")) s = p;
        else
        {
            const char* pHome = getenv("HOME");
            s = std::string(pHome ? pHome : ".") + "/Library/Application Support/NOLF1";
        }
    }
    return s.c_str();
}

void LTConfig_SetCanSave(bool b) { g_bConfigCanSave = b; }

int LTConfig_LoadUser(const char* pName)
{
    if (!LTConfig_Enabled()) return 0;
    const std::string sPath = ConfigPath(pName);
    FILE* fp = fopen(sPath.c_str(), "r");
    if (!fp)
    {
        printf("[CONFIG] no saved settings yet (%s) -- the install's defaults\n", sPath.c_str());
        fflush(stdout);
        return 0;
    }
    int nLines = 0;
    char line[1024];
    LTGetConsole()->SetSaveOnSet(true);
    while (fgets(line, sizeof line, fp))
    {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ')) line[--n] = 0;
        if (!n) continue;
        LTGetConsole()->RunString(line);
        ++nLines;
    }
    LTGetConsole()->SetSaveOnSet(false);
    fclose(fp);
    printf("[CONFIG] read %d line(s) of saved settings from %s\n", nLines, sPath.c_str());
    fflush(stdout);
    return nLines;
}

bool LTConfig_Write(const char* pName)
{
    if (!LTConfig_Enabled() || !g_bConfigCanSave) return false;

    // ~/Library/Application Support exists on every Mac; NOLF1 may not.
    mkdir(LTConfig_Dir(), 0755);

    const std::string sPath = ConfigPath(pName);
    const std::string sTmp  = sPath + ".tmp";
    FILE* fp = fopen(sTmp.c_str(), "w");
    if (!fp)
    {
        printf("[CONFIG] ⚠️  cannot write %s\n", sTmp.c_str());
        fflush(stdout);
        return false;
    }

    fprintf(fp, "// NOLF macOS port: your settings. Read after the game's own autoexec.cfg.\n");
    unsigned int nVars = 0;
    const std::map<std::string, LTConsoleVar*>& vars = LTGetConsole()->Vars();
    for (std::map<std::string, LTConsoleVar*>::const_iterator it = vars.begin(); it != vars.end(); ++it)
    {
        const LTConsoleVar* v = it->second;
        if (!v || v->m_nSave != 1) continue;
        fprintf(fp, "\"%s\" \"%s\"\n", v->m_sName.c_str(), v->m_sValue.c_str());
        ++nVars;
    }
    fprintf(fp, "\n");
    SaveBindings(fp);
    const bool bOK = (fclose(fp) == 0);

    // ⚠️ REPLACED IN ONE STEP: a crash mid-write leaves the old file whole.
    if (!bOK || rename(sTmp.c_str(), sPath.c_str()) != 0)
    {
        printf("[CONFIG] ⚠️  could not replace %s\n", sPath.c_str());
        fflush(stdout);
        remove(sTmp.c_str());
        return false;
    }
    printf("[CONFIG] saved %u setting(s) and %u key binding(s) to %s\n",
           nVars, (unsigned)(g_DikToAction.size() + g_MouseToAction.size()), sPath.c_str());
    fflush(stdout);
    return true;
}


int LTBind_CommandForVK(int nVK)
{
    const int nDik = VKToDIK(nVK);
    if (nDik < 0) return -1;

    std::map<int, int>::const_iterator it = g_DikToCommand.find(nDik);
    return (it == g_DikToCommand.end()) ? -1 : it->second;
}


bool LTBind_OnMouseButton(int nButton, bool bDown)
{
    std::map<int, int>::const_iterator it = g_MouseToCommand.find(nButton);

    if (nButton >= 0 && nButton <= 7)
    {
        char name[16];
        snprintf(name, sizeof name, "Button %d", nButton);
        TrackMouse(CONTROLTYPE_BUTTON, name, bDown ? 1u : 0u);
    }

    if (getenv("LT_KEYTRACE"))
    {
        printf("[KEY] mouse button %d -> %s%s\n", nButton,
               (it == g_MouseToCommand.end()) ? "unbound" : "command",
               (it == g_MouseToCommand.end()) ? "" : (bDown ? " ON" : " off"));
        if (it != g_MouseToCommand.end()) printf("      command id %d\n", it->second);
        fflush(stdout);
    }

    if (it == g_MouseToCommand.end()) return false;

    LTCommand_Set(it->second, bDown);
    return true;
}


bool LTBind_OnWheel(int nDir)
{
    const int nCmd = (nDir > 0) ? g_WheelFwd : (nDir < 0 ? g_WheelBack : -1);
    // The screen reads the sign: `(int)m_InputValue > 0` is a forward notch.
    if (nDir) TrackMouse(CONTROLTYPE_ZAXIS, "Wheel", (unsigned)(nDir > 0 ? 120 : -120));
    if (getenv("LT_KEYTRACE"))
    {
        printf("[KEY] wheel %s -> %s %d\n", nDir > 0 ? "forward" : "back",
               nCmd < 0 ? "unbound" : "command", nCmd);
        fflush(stdout);
    }
    if (nCmd < 0) return false;
    LTCommand_Set(nCmd, true);
    g_Pulses.push_back(nCmd);
    return true;
}

void LTBind_ReleasePulses()
{
    for (size_t i = 0; i < g_Pulses.size(); ++i) LTCommand_Set(g_Pulses[i], false);
    g_Pulses.clear();
}


bool LTBind_OnKey(int nVK, bool bDown)
{
    const int nCmd = LTBind_CommandForVK(nVK);

    // ⚠️ LT_KEYTRACE=1 -- every key the ENGINE receives, with what it resolved
    // to. This is the one diagnostic that separates the three ways a keyboard
    // can be dead: the window never delivered the key, the key is not bound,
    // or the command was set and the GAME declined to act on it.
    if (getenv("LT_KEYTRACE"))
    {
        printf("[KEY] vk=%d ('%c') dik=%d -> %s%s\n",
               nVK, (nVK >= 32 && nVK < 127) ? (char)nVK : '?',
               VKToDIK(nVK),
               (nCmd < 0) ? "unbound" : "command",
               (nCmd < 0) ? "" : (bDown ? " ON" : " off"));
        if (nCmd >= 0) printf("      command id %d\n", nCmd);
        fflush(stdout);
    }

    // ⚠️ RECORDED EVEN WHEN UNBOUND -- the whole point of the assignment
    // screen is to bind a key that currently is not.
    if (g_bTracking && (g_nTrackDevices & DEVICETYPE_KEYBOARD) && g_Tracked.size() < g_nTrackCap)
    {
        const int nDik = VKToDIK(nVK);
        if (nDik > 0)
        {
            DeviceInput di;
            memset(&di, 0, sizeof(di));
            di.m_DeviceType = DEVICETYPE_KEYBOARD;
            strncpy(di.m_DeviceName, "##keyboard", sizeof(di.m_DeviceName) - 1);
            di.m_ControlType = CONTROLTYPE_KEY;
            KeyDisplayName(nVK, nDik, di.m_ControlName,
                           sizeof(di.m_ControlName) - 1);

            // ⚠️ BYTE 63: LithTech's channel for the raw scan code, read back
            // as `(uint8)pInput->m_ControlName[63]`. Without it the game binds
            // by NAME and the name is not what `rangebind` wants.
            di.m_ControlName[63] = (char)(unsigned char)nDik;

            di.m_InputValue = bDown ? 1 : 0;
            g_Tracked.push_back(di);
        }
    }

    if (nCmd < 0) { if (bDown) ++g_nUnboundKeys; return false; }

    if (bDown) ++g_nKeyHits;
    LTCommand_Set(nCmd, bDown);
    return true;
}


// ----------------------------------------------------------------------- //
// Device tracking -- the key-assignment screen's path. See the header.
// ----------------------------------------------------------------------- //


void LTDevice_StartTrack(unsigned nDevices, unsigned nBufferSize)
{
    g_nTrackDevices = nDevices;
    g_bTracking = true;
    g_nTrackCap = nBufferSize ? nBufferSize : 32;
    g_Tracked.clear();
}

void LTDevice_EndTrack()
{
    g_bTracking = false;
    g_Tracked.clear();
}

bool LTDevice_Tracking() { return g_bTracking; }

void LTDevice_Track(void* pArray, unsigned* pnInOut)
{
    if (!pnInOut) return;

    const unsigned nRoom = *pnInOut;
    *pnInOut = 0;
    if (!pArray || !nRoom || g_Tracked.empty()) return;

    DeviceInput* pOut = (DeviceInput*)pArray;
    unsigned n = 0;
    for (size_t i = 0; i < g_Tracked.size() && n < nRoom; ++i, ++n)
        pOut[n] = g_Tracked[i];

    // ⚠️ DRAINED, not kept. The screen polls every frame and acts on the first
    // key-down it sees; leaving events in the buffer would re-assign the same
    // key on the next poll too.
    g_Tracked.clear();
    *pnInOut = n;
}

bool LTDevice_GetName(unsigned nDeviceType, char* pBuf, unsigned nBufLen)
{
    if (!pBuf || !nBufLen) return false;

    // ⚠️ THE `##` FORM, because the game feeds this straight back into
    // `rangebind "%s" ...` (FolderCustomControls.cpp:434) and the parser --
    // ours and the original's -- expects the marker.
    const char* p = (nDeviceType == DEVICETYPE_KEYBOARD) ? "##keyboard"
                  : (nDeviceType == DEVICETYPE_MOUSE)    ? "##mouse"
                                                         : "";
    if (!*p) { pBuf[0] = 0; return false; }

    strncpy(pBuf, p, nBufLen - 1);
    pBuf[nBufLen - 1] = 0;
    return true;
}


// ⚠️ BUILT FROM THE SAME TWO TABLES the bindings themselves live in, so what
// the screen shows is what the engine will actually do. Deriving the display
// from a second source is how a screen ends up confidently wrong.
namespace
{
    // One trigger's binding record with one action -- a second action (the
    // wheel's other notch) is chained by the caller.
    GameAction* NewAction(int nCmd, const std::map<int, std::string>& byCommand,
                          float fLow, float fHigh)
    {
        GameAction* pA = new GameAction;
        memset(pA, 0, sizeof(*pA));
        pA->nActionCode = nCmd;
        std::map<int, std::string>::const_iterator na = byCommand.find(nCmd);
        if (na != byCommand.end())
            strncpy(pA->strActionName, na->second.c_str(), sizeof(pA->strActionName) - 1);
        pA->nRangeLow  = fLow;
        pA->nRangeHigh = fHigh;
        pA->pNext = 0;
        return pA;
    }

    DeviceBinding* NewBinding(const char* pDevice, const char* pTrigger)
    {
        DeviceBinding* pB = new DeviceBinding;
        memset(pB, 0, sizeof(*pB));
        strncpy(pB->strDeviceName, pDevice, sizeof(pB->strDeviceName) - 1);
        strncpy(pB->strTriggerName, pTrigger, sizeof(pB->strTriggerName) - 1);
        pB->nScale = 1.0f;
        return pB;
    }
}

void* LTDevice_GetBindings(unsigned nDeviceFlags)
{
    if (!(nDeviceFlags & (DEVICETYPE_KEYBOARD | DEVICETYPE_MOUSE))) return 0;

    // command id -> action name, inverted from the AddAction table.
    std::map<int, std::string> byCommand;
    for (std::map<std::string, int>::const_iterator it = g_Actions.begin();
         it != g_Actions.end(); ++it)
    {
        // First name wins: several aliases can share a command id and the
        // screen only has room for one.
        if (byCommand.find(it->second) == byCommand.end())
            byCommand[it->second] = it->first;
    }

    DeviceBinding* pHead = 0;
    DeviceBinding* pTail = 0;

    // ⚠️ THE MOUSE (5dv): "Button N" per bound button, and the wheel as ONE
    // trigger, "Wheel", carrying both notches as ranged actions -- the screen
    // tells them apart by the sign of nRangeHigh (FolderCustomControls.cpp:
    // 917). Without these the mouse column was always empty and a click or a
    // notch could not be assigned.
    if (nDeviceFlags & DEVICETYPE_MOUSE)
    {
        for (std::map<int, int>::const_iterator it = g_MouseToCommand.begin();
             it != g_MouseToCommand.end(); ++it)
        {
            char name[16];
            snprintf(name, sizeof name, "Button %d", it->first);
            DeviceBinding* pB = NewBinding("##mouse", name);
            pB->pActionHead = NewAction(it->second, byCommand, 0.0f, 0.0f);
            if (!pHead) pHead = pTail = pB; else { pTail->pNext = pB; pTail = pB; }
        }
        if (g_WheelFwd >= 0 || g_WheelBack >= 0)
        {
            DeviceBinding* pB = NewBinding("##mouse", "Wheel");
            GameAction** ppA = &pB->pActionHead;
            if (g_WheelFwd >= 0)  { *ppA = NewAction(g_WheelFwd,  byCommand,  0.1f,  255.0f); ppA = &(*ppA)->pNext; }
            if (g_WheelBack >= 0) { *ppA = NewAction(g_WheelBack, byCommand, -0.1f, -255.0f); }
            if (!pHead) pHead = pTail = pB; else { pTail->pNext = pB; pTail = pB; }
        }
    }
    if (!(nDeviceFlags & DEVICETYPE_KEYBOARD)) return pHead;

    for (std::map<int, int>::const_iterator it = g_DikToCommand.begin();
         it != g_DikToCommand.end(); ++it)
    {
        const int nDik = it->first;
        const int nCmd = it->second;

        // The DIK has to be expressed as the key's DISPLAY name, because that
        // is what the screen compares against (see the header).
        int vk = -1;
        for (int k = 1; k < 256; ++k)
            if (VKToDIK(k) == nDik) { vk = k; break; }
        if (vk < 0) continue;

        DeviceBinding* pB = new DeviceBinding;
        memset(pB, 0, sizeof(*pB));
        strncpy(pB->strDeviceName, "##keyboard", sizeof(pB->strDeviceName) - 1);
        KeyDisplayName(vk, nDik, pB->strTriggerName,
                       sizeof(pB->strTriggerName) - 1);
        pB->nScale = 1.0f;
        pB->pNext  = 0;

        GameAction* pA = new GameAction;
        memset(pA, 0, sizeof(*pA));
        pA->nActionCode = nCmd;
        std::map<int, std::string>::const_iterator na = byCommand.find(nCmd);
        if (na != byCommand.end())
            strncpy(pA->strActionName, na->second.c_str(),
                    sizeof(pA->strActionName) - 1);
        pA->pNext = 0;
        pB->pActionHead = pA;

        if (!pHead) pHead = pTail = pB;
        else        { pTail->pNext = pB; pTail = pB; }
    }
    return pHead;
}

void LTDevice_FreeBindings(void* pList)
{
    DeviceBinding* p = (DeviceBinding*)pList;
    while (p)
    {
        GameAction* pA = p->pActionHead;
        while (pA) { GameAction* pN = pA->pNext; delete pA; pA = pN; }
        DeviceBinding* pNext = p->pNext;
        delete p;
        p = pNext;
    }
}


// ⚠️ THE KEYS THE DEVICE OFFERS, built from the SAME VK->DIK table the rest
// of this module uses -- so a key the screen lists is exactly a key the engine
// can deliver. Generating this from a second, hand-written list is how the two
// drift and a key becomes assignable but dead.
//
// `GetControlType` walks it comparing m_ObjectName against the name it got from
// TrackDevice (FolderCustomControls.cpp:850), so the names must match what
// KeyDisplayName produces.
void* LTDevice_GetObjects(unsigned nDeviceFlags)
{
    DeviceObject* pHead = 0;
    DeviceObject* pTail = 0;

    // The mouse's assignable controls (5dv), under the names tracking and
    // the binding list use: five buttons and the wheel.
    if (nDeviceFlags & DEVICETYPE_MOUSE)
    {
        for (int b = 0; b <= 5; ++b)
        {
            DeviceObject* p = new DeviceObject;
            memset(p, 0, sizeof(*p));
            p->m_DeviceType = DEVICETYPE_MOUSE;
            strncpy(p->m_DeviceName, "##mouse", sizeof(p->m_DeviceName) - 1);
            if (b < 5) { p->m_ObjectType = CONTROLTYPE_BUTTON; snprintf(p->m_ObjectName, sizeof(p->m_ObjectName), "Button %d", b); }
            else       { p->m_ObjectType = CONTROLTYPE_ZAXIS;  strncpy(p->m_ObjectName, "Wheel", sizeof(p->m_ObjectName) - 1); }
            p->m_RangeLow  = (b < 5) ? 0.0f : -255.0f;
            p->m_RangeHigh = (b < 5) ? 1.0f :  255.0f;
            if (!pHead) pHead = pTail = p; else { pTail->m_pNext = p; pTail = p; }
        }
    }
    if (!(nDeviceFlags & DEVICETYPE_KEYBOARD)) return pHead;

    // Every VK this port can map. 1..255 is small and runs once per screen.
    // One object per DIK: Shift and Left Shift are one key with one name.
    std::set<int> seen;
    for (int vk = 1; vk < 256; ++vk)
    {
        const int nDik = VKToDIK(vk);
        if (nDik <= 0 || !seen.insert(nDik).second) continue;

        DeviceObject* p = new DeviceObject;
        memset(p, 0, sizeof(*p));
        p->m_DeviceType = DEVICETYPE_KEYBOARD;
        strncpy(p->m_DeviceName, "##keyboard", sizeof(p->m_DeviceName) - 1);
        p->m_ObjectType = CONTROLTYPE_KEY;
        KeyDisplayName(vk, nDik, p->m_ObjectName, sizeof(p->m_ObjectName) - 1);
        p->m_RangeLow  = 0.0f;
        p->m_RangeHigh = 1.0f;
        p->m_pNext     = 0;

        if (!pHead) pHead = pTail = p;
        else        { pTail->m_pNext = p; pTail = p; }
    }
    return pHead;
}

void LTDevice_FreeObjects(void* pList)
{
    DeviceObject* p = (DeviceObject*)pList;
    while (p) { DeviceObject* pNext = p->m_pNext; delete p; p = pNext; }
}


unsigned LTBind_ActionCount()  { return (unsigned)g_Actions.size(); }
unsigned LTBind_BindingCount() { return (unsigned)g_DikToCommand.size(); }


// ⚠️ NOLF's OWN NAMES, TRANSCRIBED -- not #included.
//
// These are the ids in NOLF/Shared/CommandIDs.h. The engine must not depend on
// a game header (HANDOVER 4.3), so this table is a transcription and the
// citation is the contract: if the game's header changes, this is wrong and
// only a person reading both will know.
// LT_KEYCHECK=1 -- EVERY BOUND KEY, THROUGH THE REAL MAC PATH. For each
// rangebind'ed scan code: which physical Mac key reaches it (keyDown table, or
// a modifier through flagsChanged), or that none does. A bound key no Mac key
// can produce is a dead control -- and nothing at run time would say so.
void LTBind_CheckMacKeys()
{
    std::map<int, int> dikToMac;            // dik -> mac keycode
    for (int k = 0; k < 128; ++k)
    {
        const int vk = LTInput_MacKeyToVK(k);
        if (vk) { const int d = VKToDIK(vk); if (d > 0 && !dikToMac.count(d)) dikToMac[d] = k; }
        bool bDown = false, bToggle = false;
        const int mv = LTInput_MacModifierToVK(k, 0xFFFFFFFFul, &bDown, &bToggle);
        if (mv) { const int d = VKToDIK(mv); if (d > 0 && !dikToMac.count(d)) dikToMac[d] = k; }
    }
    unsigned nOk = 0, nDead = 0;
    for (std::map<int, int>::const_iterator it = g_DikToCommand.begin(); it != g_DikToCommand.end(); ++it)
    {
        std::map<int, int>::const_iterator m = dikToMac.find(it->first);
        if (m == dikToMac.end()) ++nDead; else ++nOk;
        printf("[KEYCHECK] dik %3d -> %-14s %s", it->first, LTBind_CommandName(it->second),
               m == dikToMac.end() ? "UNREACHABLE from a Mac keyboard\n" : "");
        if (m != dikToMac.end()) printf("mac key 0x%02X\n", m->second);
    }
    printf("[KEYCHECK] %u bound key(s) reachable, %u unreachable\n", nOk, nDead);
    fflush(stdout);
}

const char* LTBind_CommandName(int nCommand)
{
    switch (nCommand)
    {
        case  0: return "FORWARD";        case  1: return "REVERSE";
        case  2: return "ACTIVATE";       case  3: return "RELOAD";
        case  4: return "TURNAROUND";     case  5: return "DUCK";
        case  6: return "JUMP";           case  7: return "RUN";
        case  8: return "FIRING";         case  9: return "STRAFE";
        case 10: return "LEFT";           case 11: return "RIGHT";
        case 12: return "RUNLOCK";        case 13: return "STRAFE_LEFT";
        case 14: return "STRAFE_RIGHT";   case 15: return "ALT_FIRING";
        case 16: return "LOOKUP";         case 17: return "LOOKDOWN";
        case 60: return "NEXT_AMMO";      case 61: return "ZOOM_IN";
        case 62: return "ZOOM_OUT";       case 63: return "FLASHLIGHT";
        case 64: return "QUICKSAVE";      case 65: return "QUICKLOAD";
        case 66: return "PREV_WEAPON";    case 67: return "NEXT_WEAPON";
        case 68: return "FRAGCOUNT";      case 69: return "MISSION";
        case 70: return "MOUSEAIMTOGGLE"; case 71: return "CROSSHAIRTOGGLE";
        case 72: return "CENTERVIEW";     case 73: return "HOLSTER";
        case 74: return "MESSAGE";        case 75: return "NEXT_LAYOUT";
        case 76: return "PREV_LAYOUT";    case 77: return "TEAM_MESSAGE";
        case 78: return "INVENTORY";      case 79: return "LEFT_SHORT";
        case 99: return "UNASSIGNED";
        default: break;
    }
    if (nCommand >= 20 && nCommand <= 59) return "WEAPON";
    return "?";
}

void LTBind_DumpCensus()
{
    // ⚠️ MOUSE BINDINGS ARE COUNTED SEPARATELY AND PRINTED ALWAYS. A census
    // that only counted keys reported "46 key binding(s)" while every mouse
    // binding in the file was being discarded -- a full, confident number about
    // half the input system.
    printf("\n[BIND] %u mouse button binding(s)", (unsigned)g_MouseToCommand.size());
    if (g_MouseToCommand.empty())
        printf("  ⚠️  NONE -- nothing can fire");
    else
    {
        printf(":");
        for (std::map<int,int>::const_iterator mi = g_MouseToCommand.begin();
             mi != g_MouseToCommand.end(); ++mi)
            printf("  btn%d=%d", mi->first, mi->second);
    }
    printf("\n");

    printf("\n[BIND] %u action(s), %u key binding(s); %u key press(es) hit a "
           "binding, %u did not\n",
           (unsigned)g_Actions.size(), (unsigned)g_DikToCommand.size(),
           g_nKeyHits, g_nUnboundKeys);

    // ⚠️ THE KEYS A PERSON WILL ACTUALLY PRESS, resolved end to end: VK ->
    // DIK -> action -> command id. A table with 46 entries that maps none of
    // WASD is a table that parsed and is still useless, and the count alone
    // cannot tell the two apart.
    {
        static const struct { const char* pName; int vk; } kCheck[] = {
            {"W",     'W'}, {"A",     'A'}, {"S",     'S'}, {"D",     'D'},
            {"E",     'E'}, {"Space", VK_SPACE}, {"Shift", VK_SHIFT},
            {"Ctrl",  VK_CONTROL}
        };
        printf("       ");
        for (size_t i = 0; i < sizeof(kCheck)/sizeof(kCheck[0]); ++i)
        {
            const int c = LTBind_CommandForVK(kCheck[i].vk);
            printf("%s=", kCheck[i].pName);
            if (c < 0) printf("--  "); else printf("%d  ", c);
        }
        printf("\n");
    }

    if (g_DikToCommand.empty())
        printf("       ⚠️  NO BINDINGS. autoexec.cfg was not read, or its\n"
               "           AddAction lines never reached the console.\n");

    // ⚠️ LT_BINDDUMP=1 -- EVERY binding, end to end, with the game's own name
    // for the command. The spot-check above answers "did WASD survive"; this
    // answers "which control is missing", which is the question a person asks
    // when a key does nothing. Off by default because it is 46 lines.
    if (getenv("LT_BINDDUMP"))
    {
        printf("\n[BIND] every binding -- key -> action -> command\n");
        for (std::map<int,std::string>::const_iterator mi = g_MouseToAction.begin();
             mi != g_MouseToAction.end(); ++mi)
        {
            const int cmd = g_MouseToCommand[mi->first];
            printf("       mouse %-5d %-22s %3d  %s\n", mi->first,
                   mi->second.c_str(), cmd, LTBind_CommandName(cmd));
        }
        for (std::map<int,std::string>::const_iterator ki = g_DikToAction.begin();
             ki != g_DikToAction.end(); ++ki)
        {
            const int cmd = g_DikToCommand[ki->first];
            printf("       DIK   %-5d %-22s %3d  %s\n", ki->first,
                   ki->second.c_str(), cmd, LTBind_CommandName(cmd));
        }

        // ⚠️ AND THE ACTIONS WITH NO KEY AT ALL. An action the game declared
        // and the config never bound is a control that cannot be reached, and
        // it is invisible in a list of bindings.
        unsigned nUnbound = 0;
        for (std::map<std::string,int>::const_iterator ai = g_Actions.begin();
             ai != g_Actions.end(); ++ai)
        {
            bool bBound = false;
            for (std::map<int,int>::const_iterator ki = g_DikToCommand.begin();
                 !bBound && ki != g_DikToCommand.end(); ++ki)
                if (ki->second == ai->second) bBound = true;
            for (std::map<int,int>::const_iterator mi = g_MouseToCommand.begin();
                 !bBound && mi != g_MouseToCommand.end(); ++mi)
                if (mi->second == ai->second) bBound = true;
            if (!bBound)
            {
                if (!nUnbound) printf("       -- declared but bound to nothing --\n");
                ++nUnbound;
                printf("       (none)      %-22s %3d  %s\n",
                       ai->first.c_str(), ai->second,
                       LTBind_CommandName(ai->second));
            }
        }
    }
    fflush(stdout);
}


// ----------------------------------------------------------------------- //
// MapVirtualKeyEx -- the game's key LABELS (HANDOVER 5dv).
//
// ⚠️ THE CONTROLS SCREEN SHOWED NOTHING FOR W, A, S, D. A one-character key
// name goes through the game's ConvertKeyToCurrentKeyboard (KeyFixes.cpp:29):
// English char -> scan code (its own table) -> MapVirtualKeyEx(scan, 1) ->
// VK -> MapVirtualKeyEx(VK, 2) -> the character that key types in the
// player's layout. The compat stub returned its input unchanged, so 'W' came
// back as character 17, a control code, drawn as nothing.
//
// Answered from the same VK<->DIK table as the bindings. Type 2 asks macOS
// what the key types in the CURRENT keyboard layout -- bindings are by
// physical key, as DirectInput's were, so on a Swiss keyboard the key a US
// layout calls Y is labelled Z, which is what Windows showed there. Only a
// printable ASCII answer is used (the menu fonts have nothing else); otherwise
// the US character. LT_USKEYLABELS=1 = US labels always (says so).
// ----------------------------------------------------------------------- //

#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

namespace
{
    // What a US keyboard types on this VK, unshifted (MAPVK_VK_TO_CHAR).
    unsigned USChar(int vk)
    {
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return (unsigned)vk;
        switch (vk)
        {
            case VK_SPACE:      return ' ';
            case VK_OEM_1:      return ';';
            case VK_OEM_PLUS:   return '=';
            case VK_OEM_COMMA:  return ',';
            case VK_OEM_MINUS:  return '-';
            case VK_OEM_PERIOD: return '.';
            case VK_OEM_2:      return '/';
            case VK_OEM_3:      return '`';
            case VK_OEM_4:      return '[';
            case VK_OEM_5:      return '\\';
            case VK_OEM_6:      return ']';
            case VK_OEM_7:      return '\'';
            case VK_OEM_102:    return '\\';
            default:            return 0;
        }
    }

    // The character the Mac key types in the current layout, no modifiers;
    // 0 if it cannot be asked. HIToolbox is loaded by Cocoa; looked up at run
    // time so the project needs no Carbon link.
    unsigned LayoutCharForMacKey(int nMacKey)
    {
        typedef void*       (*CopySrcFn)();
        typedef void*       (*GetPropFn)(void*, CFStringRef);
        typedef int32_t     (*TranslateFn)(const void*, uint16_t, uint16_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t*, unsigned long, unsigned long*, uint16_t*);
        typedef uint8_t     (*KbdTypeFn)();
        static void* s_h = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY);
        if (!s_h) return 0;
        static CopySrcFn   pCopy  = (CopySrcFn)dlsym(s_h, "TISCopyCurrentKeyboardLayoutInputSource");
        static GetPropFn   pProp  = (GetPropFn)dlsym(s_h, "TISGetInputSourceProperty");
        static TranslateFn pTrans = (TranslateFn)dlsym(s_h, "UCKeyTranslate");
        static KbdTypeFn   pKbd   = (KbdTypeFn)dlsym(s_h, "LMGetKbdType");
        static CFStringRef* pKey  = (CFStringRef*)dlsym(s_h, "kTISPropertyUnicodeKeyLayoutData");
        if (!pCopy || !pProp || !pTrans || !pKbd || !pKey || !*pKey) return 0;

        void* src = pCopy();
        if (!src) return 0;
        unsigned c = 0;
        CFDataRef data = (CFDataRef)pProp(src, *pKey);
        if (data)
        {
            uint32_t dead = 0;
            unsigned long n = 0;
            uint16_t buf[4] = { 0, 0, 0, 0 };
            // kUCKeyActionDisplay = 3, kUCKeyTranslateNoDeadKeysMask = 1
            if (pTrans(CFDataGetBytePtr(data), (uint16_t)nMacKey, 3, 0, pKbd(), 1,
                       &dead, 4, &n, buf) == 0 && n == 1)
                c = buf[0];
        }
        CFRelease((CFTypeRef)src);
        return c;
    }

    unsigned LayoutChar(int vk)
    {
        static const bool s_bUS = []() {
            const bool b = (getenv("LT_USKEYLABELS") != 0);
            if (b) { printf("[BIND] LT_USKEYLABELS=1 -- key labels from a US layout\n"); fflush(stdout); }
            return b; }();
        if (s_bUS) return 0;
        for (int k = 0; k < 128; ++k)
        {
            if (LTInput_MacKeyToVK(k) != vk) continue;
            unsigned c = LayoutCharForMacKey(k);
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            return (c > 32 && c < 127) ? c : 0;
        }
        return 0;
    }
}

// Weakly referenced from compat/windows_compat.c (MapVirtualKeyEx).
extern "C" unsigned int LTPlatform_MapVirtualKey(unsigned int code, unsigned int mapType)
{
    switch (mapType)
    {
        case 0:                                     // VK -> scan code
        {
            const int d = VKToDIK((int)code);
            return d > 0 ? (unsigned)(d & 0x7f) : 0u;
        }
        case 1: case 3:                             // scan code -> VK
            for (int vk = 1; vk < 256; ++vk)
                if (VKToDIK(vk) == (int)code) return (unsigned)vk;
            return 0;
        case 2:                                     // VK -> character
        {
            const unsigned c = LayoutChar((int)code);
            return c ? c : USChar((int)code);
        }
        default:
            return 0;
    }
}
