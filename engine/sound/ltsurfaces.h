// ----------------------------------------------------------------------- //
//
// MODULE  : ltsurfaces.h
//
// PURPOSE : NOLF's surface table -- what each material sounds like underfoot.
//
// ⚠️ THE SURFACE TYPE IS THE DTX's m_UserFlags, and that took finding. The game
// asks the ENGINE for it (`GetPolyTextureFlags`, NOLF/Shared/SurfaceFunctions
// .cpp:358) so it is not in the game code, and it is not in the world file
// either -- every byte and every uint16 of the surface record was eliminated.
// It is a field on the TEXTURE, at offset 20 of the DTX header, which this port
// already read and never used.
//
// ⚠️ HOW IT WAS CONFIRMED, rather than assumed: the values are predictable from
// something they did not come from. Across 4,166 textures, WOOD\* files carry
// 30, STONE\* carry 10 and 11, METAL\* carry 20, SKY\* carry 110 and WATER\*
// carry 201 -- and `Surface.txt` independently documents 30 = Wood,
// 10 = Stone Smooth, 11 = Stone Rough, 20 = Metal, 110 = Sky, 201 = Liquid.
//
// ⚠️ The block INDEX is not the id. `[Surface12]` is "Wood" and its `Id` field
// says 30; indexing by block would map every material to the wrong sounds while
// looking entirely reasonable.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SURFACES_H__
#define __LT_SURFACES_H__

#include <map>
#include <string>
#include <vector>

class CLTFileMgr;

class CLTSurfaces
{
public:

    struct Surface
    {
        std::string                 m_sName;
        std::vector<std::string>    m_Right;    // RtFootSnd1..2
        std::vector<std::string>    m_Left;     // LtFootSnd1..2
    };

    CLTSurfaces();

    // Reads Attributes\Surface.txt. Safe to call twice.
    bool                Load(CLTFileMgr* pFileMgr);
    bool                IsLoaded() const { return m_bLoaded; }
    unsigned int        Count() const { return (unsigned int)m_ById.size(); }

    // The surface with this id, or null.
    const Surface*      Find(int nId) const;

    // A footstep for this surface and foot, or an empty string. Falls back to
    // surface 0 ("Unknown") -- which is the game's own answer for a surface it
    // cannot identify -- rather than to silence.
    const char*         Footstep(int nId, bool bRight, unsigned int nVariant) const;

private:
    std::map<int, Surface>  m_ById;
    bool                    m_bLoaded;
};

#endif // __LT_SURFACES_H__
