// ----------------------------------------------------------------------- //
//
// MODULE  : ltsurfaces.cpp
//
// See ltsurfaces.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltsurfaces.h"
#include "ltfilemgr.h"
#include "iltstream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    // Value of `Key = "..."` or `Key = 12` within one block, or empty.
    std::string ValueOf(const std::string& block, const char* pKey)
    {
        size_t p = block.find(pKey);
        if (p == std::string::npos) return std::string();

        // ⚠️ The key must be followed by whitespace or '=', or "RtFootSnd1"
        // matches inside "RtFootSnd10" the moment a file has more than nine.
        size_t after = p + strlen(pKey);
        if (after < block.size())
        {
            char c = block[after];
            if (c != ' ' && c != '\t' && c != '=' && c != '\r' && c != '\n')
                return std::string();
        }

        size_t eq = block.find('=', after);
        if (eq == std::string::npos) return std::string();
        size_t nl = block.find('\n', eq);
        if (nl == std::string::npos) nl = block.size();

        std::string v = block.substr(eq + 1, nl - eq - 1);

        size_t q1 = v.find('"');
        if (q1 != std::string::npos)
        {
            size_t q2 = v.find('"', q1 + 1);
            if (q2 != std::string::npos) return v.substr(q1 + 1, q2 - q1 - 1);
            return std::string();
        }

        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
        while (!v.empty() && (v[v.size()-1] == ' ' || v[v.size()-1] == '\t' ||
                              v[v.size()-1] == '\r')) v.erase(v.size()-1);
        return v;
    }
}


CLTSurfaces::CLTSurfaces() : m_bLoaded(false) {}


bool CLTSurfaces::Load(CLTFileMgr* pFileMgr)
{
    if (m_bLoaded) return true;
    if (!pFileMgr) return false;

    ILTStream* pS = pFileMgr->OpenFile("Attributes\\Surface.txt");
    if (!pS) return false;

    unsigned long n = 0;
    pS->GetLen(&n);
    if (!n) { pS->Release(); return false; }
    std::vector<char> buf((size_t)n + 1);
    pS->Read(&buf[0], n);
    buf[n] = 0;
    pS->Release();

    const std::string all(&buf[0]);

    // Walk [SurfaceN] blocks in order; each ends where the next begins.
    size_t at = all.find("[Surface");
    while (at != std::string::npos)
    {
        size_t next = all.find("[Surface", at + 8);
        const std::string block = all.substr(at, (next == std::string::npos)
                                                 ? std::string::npos : next - at);

        const std::string sId = ValueOf(block, "Id");
        if (!sId.empty())
        {
            // ⚠️ THE ID, NOT THE BLOCK NUMBER. [Surface12] is "Wood" and its Id
            // is 30; keying on the block index maps every material to another
            // material's sounds and still looks sane.
            const int nId = atoi(sId.c_str());

            Surface s;
            s.m_sName = ValueOf(block, "Name");
            for (int v = 1; v <= 2; ++v)
            {
                char key[32];
                snprintf(key, sizeof key, "RtFootSnd%d", v);
                std::string r = ValueOf(block, key);
                if (!r.empty()) s.m_Right.push_back(r);

                snprintf(key, sizeof key, "LtFootSnd%d", v);
                std::string l = ValueOf(block, key);
                if (!l.empty()) s.m_Left.push_back(l);
            }
            m_ById[nId] = s;
        }
        at = next;
    }

    m_bLoaded = !m_ById.empty();
    return m_bLoaded;
}


const CLTSurfaces::Surface* CLTSurfaces::Find(int nId) const
{
    std::map<int, Surface>::const_iterator it = m_ById.find(nId);
    return (it == m_ById.end()) ? 0 : &it->second;
}


const char* CLTSurfaces::Footstep(int nId, bool bRight, unsigned int nVariant) const
{
    const Surface* p = Find(nId);

    // ⚠️ Fall back to surface 0, "Unknown", which is the GAME's own answer for
    // a surface it cannot identify -- not to silence, which would make a
    // missing id indistinguishable from a correct one on a silent floor.
    if (!p || (bRight ? p->m_Right.empty() : p->m_Left.empty()))
        p = Find(0);
    if (!p) return "";

    const std::vector<std::string>& v = bRight ? p->m_Right : p->m_Left;
    if (v.empty()) return "";
    return v[nVariant % v.size()].c_str();
}
