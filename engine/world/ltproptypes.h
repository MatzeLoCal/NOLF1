// ----------------------------------------------------------------------- //
//
// MODULE  : ltproptypes.h
//
// PURPOSE : `Attributes\PropTypes.txt` -- the model and skins a PropType
//           object is ACTUALLY drawn with.
//
// ⚠️ WHY THIS EXISTS, AND IT IS NOT AN OPTIMISATION. A `PropType` object in a
// world file carries `Filename` and `Skin` properties, and BOTH CAN BE WRONG.
// The game does not read them: `PropType::ReadProp` (ObjectDLL/PropType.cpp:133)
// looks the object's `Type` string up in this attribute file and copies the
// model and skin list from THERE, unconditionally. Those two world properties
// are declared `PF_HIDDEN` in the class and default to "" -- they are editor
// bookkeeping, not the answer.
//
// Measured consequences of believing the world file instead:
//
//   * `Type = "Tree Fir Tall"` stores `Tree_06.abc` and one skin. The attribute
//     file says `Tree_06c.abc` and TWO skins -- leaf and trunk. So the wrong
//     model was drawn, and its trunk had no texture at all: flat shaded, the
//     same failure the foliage alpha bug produced, from a different cause.
//   * 23 placements across 7 worlds were drawn with fewer skins than their
//     model has texture slots.
//
// ⚠️ It is a READER, not a manager. Nothing here evaluates prop behaviour; the
// engine only needs to know what to draw.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_PROP_TYPES_H__
#define __LT_PROP_TYPES_H__

#include <string>
#include <map>

class CLTFileMgr;

class CLTPropTypes
{
public:

    // ⚠️ m_bLoaded must be false before Load runs: a caller that asks
    // IsLoaded() first would otherwise read uninitialised memory and take
    // the wrong branch -- the bug family HANDOVER.md 2 keeps finding.
    CLTPropTypes() : m_bLoaded(false) {}

    struct Entry
    {
        std::string m_sFilename;    // "Props\Models\Tree_06c.abc"
        std::string m_sSkin;        // ";"-separated, as the attribute file writes it

        // ⚠️ WHETHER THIS PROP USES ITS TEXTURE'S TRANSPARENCY, and it is the
        // ONLY thing that answers the question. Two props can share a texture
        // format, a flag set and even a skin and still need opposite
        // treatment:
        //
        //     Cattail               Chromakey = 1   -> a cut-out reed
        //     Hanging Lamp Orange   Chromakey = 0   -> a solid lamp shade
        //
        // Both skins are PREFER4444 with no alpharef, so nothing in the DTX
        // distinguishes them -- and the object's own `Chromakey` property is 0
        // on BOTH, so the world file does not either. The answer is here, three
        // lines below the Filename and Skin this file was already being read
        // for.
        //
        // ⚠️ It predicts every case that was established independently by
        // LOOKING: cattail, lamp_02, lamp_03, plant_04b and tree_09a are all 1
        // (verified cut-outs); lamp_05b, dinnertray and flour are all 0
        // (verified solid). Eight for eight, decided before the renders.
        //
        // Default FALSE: a prop type that does not say is opaque, which is what
        // 219 of the 301 entries say explicitly.
        bool        m_bChromakey;

        // ⚠️ AND `Alpha` IS A SECOND WAY OF SAYING IT. 18 of the 301 entries
        // set Alpha to 0.99 or 0.999 instead of 1.0, which marks the object
        // TRANSLUCENT -- and a translucent object is the one case where
        // LithTech uses the texture's alpha channel regardless of Chromakey.
        // Their DebrisType says what they are: 8 "Glass small" and 5
        // "Foliage".
        //
        // ⚠️ `Plant Long Grass 1` is why this is here. It is Chromakey=0 with
        // Alpha=0.999 and DebrisType "Foliage" -- long grass, 23 placements,
        // which Chromakey alone would draw as a solid slab. The same shape as
        // the Cattail regression, reached by a different route.
        //
        // ⚠️ This renderer has no blended pass, so a translucent prop is
        // alpha-TESTED rather than blended. For the grass that is right; for
        // the glass it punches the fully-transparent texels and leaves the
        // rest opaque, which is closer than either extreme. Both want the
        // blend pass (lt3d.cpp).
        float       m_fAlpha;

        // ⚠️ AND TRANSLUCENCY HAS THREE MODES, NOT ONE. `Additive` and
        // `Multiply` sit beside `Alpha` in the same block and PropType.cpp
        // turns them into FLAG2_ADDITIVE / FLAG2_MULTIPLY. A model with either
        // is composited whatever its Alpha says, and reading only Alpha draws
        // NOLF's light beams as solid black cones -- their skin is dark
        // because it is meant to be ADDED to what is behind it.
        bool        m_bAdditive;
        bool        m_bMultiply;

        // 0 ordinary source-alpha, 1 additive, 2 multiply.
        int         BlendOp() const
                    { return m_bAdditive ? 1 : (m_bMultiply ? 2 : 0); }

        bool        UsesTextureAlpha() const
                    { return m_bChromakey || m_fAlpha < 1.0f; }

        // ⚠️ THE TYPE'S OWN PLACEMENT, which the GAME uses (HANDOVER 5bt):
        // PropType::ReadProp takes `m_bMoveToFloor = pPropType->bMoveToFloor`
        // UNCONDITIONALLY (the world file's MoveToFloor is PF_HIDDEN), and the
        // type's Scale whenever the level left Scale at 1,1,1 (PropType.cpp:
        // 166-174). A missing MoveToFloor reads 0 -- CButeMgr::GetInt's
        // default -- and a missing Scale is taken as 1,1,1.
        bool        m_bMoveToFloor;
        float       m_Scale[3];

        Entry() : m_bChromakey(false), m_fAlpha(1.0f),
                  m_bAdditive(false), m_bMultiply(false), m_bMoveToFloor(false)
        { m_Scale[0] = m_Scale[1] = m_Scale[2] = 1.0f; }
    };

    // Reads Attributes\PropTypes.txt through the file manager. Returns false
    // and stays empty when it is absent -- callers then fall back to the
    // world's own properties, which is what this port did before and is wrong
    // only for the objects listed above.
    bool                Load(CLTFileMgr* pFileMgr);

    bool                IsLoaded() const { return m_bLoaded; }
    unsigned int        Count() const { return (unsigned int)m_Types.size(); }

    // The record for a `Type` string, or null. ⚠️ The attribute file says its
    // definition names ARE case sensitive; the Type VALUES are matched exactly
    // as the game's map does.
    const Entry*        Find(const std::string& sType) const;

private:

    bool                            m_bLoaded;
    std::map<std::string, Entry>    m_Types;
};

#endif // __LT_PROP_TYPES_H__
