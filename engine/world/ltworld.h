// ----------------------------------------------------------------------- //
//
// MODULE  : ltworld.h
//
// PURPOSE : Reads NOLF 1's `.dat` worlds -- version 66.
//
//           This first pass reads the HEADER and the OBJECT LIST: every
//           entity the level places, with its typed properties. That is what
//           the server spawns a level from, and it is the part gameplay needs
//           first. World GEOMETRY (BSP, render data) is a separate, larger
//           piece and is not read yet.
//
// ⚠️ THE DONOR CANNOT READ THESE, AND THAT WAS MEASURED
//
//   Jupiter tests `version != CURRENT_WORLD_VERSION` and rejects -- an
//   EQUALITY test against 85. All 102 shipped worlds are version **66**. So
//   this is written from scratch, like the ABC loader. What the donor DOES
//   give is the shape of the newer header and the object-list grammar
//   (`runtime/world/src/world_shared_bsp.cpp:360`,
//   `runtime/server/src/s_object.cpp:337`), which is where the field ORDER
//   below comes from rather than from guessing.
//
// ⚠️ v66's HEADER IS SHORTER THAN v85's, AND THAT IS THE FIRST TRAP
//
//   v85: version, objectDataPos, blindObjectDataPos, lightGridPos,
//        collisionDataPos, particleBlockerDataPos, renderDataPos, then 8 words.
//   v66: version, objectDataPos, blindObjectDataPos, then 8 words.
//
//   The four extra position fields do not exist in v66 -- the info string
//   starts at byte **44**, not 60. Reading v85's layout puts the string length
//   16 bytes late and everything after it is garbage. Verified the other way
//   round: with this layout, all **102 of 102** worlds produce a plausible
//   header and a printable info string.
//
// HOW THE OBJECT LIST IS CHECKED
//
//   Every object declares its own byte length before its data, so the parse
//   PREDICTS that boundary and refuses anything that lands elsewhere -- the
//   same lever that solved ABC, available per object.
//
//       **79,968 objects across all 102 worlds, every one exact.**
//
//   The classes that come out are unmistakably NOLF: Light (21321),
//   AIVolume (8238), TranslucentWorldModel (6605), ScaleSprite (5191),
//   AINode (4737), Door, Trigger, Key, Prop...
//
// ⚠️ LT_PT_ROTATION IS EULER ANGLES, NOT A QUATERNION
//
//   This is the trap most likely to cost someone a day. `LTRotation` is a
//   quaternion everywhere else in this engine (ltrotation.h:42), but a
//   ROTATION *property* is 16 bytes whose 4th float is **never** non-zero and
//   whose magnitude is **not** unit -- measured across 9,246 of them. They are
//   (pitch, yaw, roll) in RADIANS with a padding float.
//
//   Feeding them into a quaternion path yields a normalised-garbage
//   orientation for every object in the level, which looks like a physics bug.
//   `GameStartPoint` in `WORLDS\M01S01.DAT` reads (0, 4.714, 0, 0) -- a yaw of
//   270 degrees, not a quaternion.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WORLD_H__
#define __LT_WORLD_H__

#include <string>
#include <vector>

class CLTFileMgr;

// Matches LT_PT_* in the SDK's ltproperty.h.
enum ELTWorldPropType
{
    kLTWP_String   = 0,
    kLTWP_Vector   = 1,
    kLTWP_Color    = 2,
    kLTWP_Real     = 3,
    kLTWP_Flags    = 4,
    kLTWP_Bool     = 5,
    kLTWP_LongInt  = 6,
    kLTWP_Rotation = 7
};

struct LTWorldProperty
{
    std::string     m_sName;
    unsigned char   m_nType;        // ELTWorldPropType
    unsigned int    m_nFlags;
    std::string     m_sString;      // kLTWP_String
    float           m_Vec[3];       // Vector / Color
    float           m_Euler[3];     // ⚠️ Rotation -- EULER RADIANS, see above
    float           m_fReal;        // Real
    int             m_nInt;         // LongInt / Flags  (stored as a float!)
    bool            m_bBool;

    const char*     GetString() const { return m_sString.c_str(); }
};

struct LTWorldObject
{
    std::string                     m_sType;    // "GameStartPoint", "Door", ...
    std::vector<LTWorldProperty>    m_Props;

    // Convenience: the two properties nearly every object carries.
    const LTWorldProperty*  Find(const char* pName) const;
    bool                    GetPos(float* pOut3) const;
    bool                    GetEuler(float* pOut3) const;
    const char*             GetName() const;
};


// The world-info block that follows the header, and the world tree's shape.
//
// ⚠️ The tree is stored as a BIT STREAM -- one bit per node, 1 meaning
// "subdivide into 4 children" (MAX_WTNODE_CHILDREN is 4, a quadtree, not an
// octree). Walking it is self-checking: the number of subdivisions must come
// to exactly (nNodes-1)/4, and it does for all 102 shipped worlds. A wrong
// offset anywhere earlier desynchronises the walk immediately, so this one
// check validates the whole preamble.
struct LTWorldInfo
{
    float           m_fUnknown;         // 16/20/24/32 across the shipped set
    float           m_ExtentsMin[3];
    float           m_ExtentsMax[3];
    float           m_TreeMin[3];       // the world tree's root box
    float           m_TreeMax[3];
    unsigned int    m_nTreeNodes;
    unsigned int    m_nTerrainDepth;
    unsigned int    m_nSubdivisions;    // counted while walking the bit stream
    unsigned int    m_nWorldModels;
    unsigned int    m_nGeometryEnd;     // offset just past the tree bit stream
};

// ---- the BSP body ------------------------------------------------------ //
//
// ⚠️ EVERY MODEL IS CHECKED AGAINST ITS OWN DECLARED END. The world models form
// a linked list (see below), so a model whose body does not account for its
// bytes exactly is marked unparsed and SKIPPED -- it cannot desynchronise the
// ones after it. That is what makes it safe to land a parser that understands
// most of the format rather than all of it: the loader never guesses, and it
// reports what it could not read.
//
// As of 2026-09-06 the accounting is exact for **89%** of world models; the
// rest carry one more optional field, still unidentified. FORMATS.md §4.

struct LTWorldPlane
{
    float           m_Normal[3];        // unit length in the shipped data
    float           m_Dist;
};

struct LTWorldSurfaceEffect
{
    std::string     m_sName;            // "pan"
    std::string     m_sValue;           // "8 12"
};

struct LTWorldSurface
{
    // The texture-space triple. ⚠️ O is a point; P and Q are axis vectors that
    // carry the TEXEL SCALE, so they are NOT unit length in general (clouds'
    // are 0.5). Assuming otherwise produces nonsense.
    float           m_O[3], m_P[3], m_Q[3];

    // ⚠️ Index into the MODEL's texture-name list, and it is the FIRST field
    // here -- not where the donor puts it. v85's SDiskSurface is
    // `uint32 m_nFlags; uint16 m_nTexture; uint16 m_nTextureFlags`, which
    // would place it at offset 4, and offset 4 also passes a range check
    // because it is nearly always 0. The coverage test separates them: across
    // 166 multi-texture models offset 0 uses EXACTLY the full range
    // [0, nTextures) -- mean distinct/nTextures 1.000 -- while offset 4 is a
    // constant 0. Following the donor here would have been wrong in a way
    // that still looked right.
    unsigned short  m_nTexture;

    // ⚠️ SURF_* flags, at offset 6 of the fixed block. The donor names the bits
    // (`de_world.h:59`): SOLID 1<<0, INVISIBLE 1<<2, TRANSPARENT 1<<3,
    // SKY 1<<4, BRIGHT 1<<5, LIGHTMAP 1<<7 ...
    //
    // ⚠️ Found by TESTING A CONSEQUENCE, not by matching a struct: sky portals
    // should carry SURF_SKY, so bit 4 was checked against "this surface's
    // texture name contains sky". Offset 6 gives precision 1.000 -- every
    // surface with the bit set is sky-textured, zero false positives across
    // 26,401 surfaces in three levels.
    //
    // ⚠️ Most other offsets score 99.3% "agreement" and mean nothing: 99.28% of
    // surfaces are not sky, so a field that is always zero scores 99.3% by
    // doing nothing. Accuracy was the wrong measure; precision and recall are
    // the ones that separate a real field from a constant.
    unsigned int    m_nFlags;
    unsigned char   m_Fixed[14];
    std::vector<LTWorldSurfaceEffect> m_Effects;
    unsigned short  m_nTrailing;
};

// ⚠️ Per-VERTEX colour -- this is VERTEX LIGHTING, not a lightmap. Unlit
// brushes are all 255,255,255, which is exactly why "lightmap" was a
// believable wrong guess.
struct LTWorldPolyVert
{
    unsigned short  m_nPoint;           // index into the model's point array
    unsigned char   m_RGB[3];
};

struct LTWorldPoly
{
    float           m_Centre[3];
    unsigned int    m_nUnknown0;
    unsigned int    m_nIndexWord;       // (polygon index << 16) | ?

    // ⚠️ Index into the model's surface array, and every one of 185,634
    // polygons across 12 worlds is in range -- none had to be clamped.
    unsigned short  m_nSurface;
    unsigned short  m_nUnknown1;
    std::vector<LTWorldPolyVert> m_Verts;
};

// One BSP node -- 14 bytes, the v85 record unchanged (de_mainworld.cpp:577).
// A side is another node's index, or -1 (NODE_IN) / -2 (NODE_OUT).
struct LTWorldNode
{
    unsigned int    m_nPoly;            // the polygon whose plane splits
    unsigned short  m_nLeaf;            // the leaf (VisBSP) this node's cell is in
    int             m_Sides[2];         // [0] back, [1] front
};

// One leaf's visibility lists (VisBSP only) -- see ltworldvis.h for what the
// bytes mean. A leaf either owns its lists or shares another leaf's.
struct LTWorldVisList
{
    unsigned short              m_nPortal;
    std::vector<unsigned char>  m_Data;
};
struct LTWorldLeaf
{
    int                         m_nShare;   // -1, or the leaf whose lists these are
    std::vector<LTWorldVisList> m_Lists;
    std::vector<unsigned int>   m_Entries;  // v66's extra block (extra1 in total)
    unsigned int                m_nTrailer; // and the 4-byte value after it
};

// ⚠️ 24 bytes: a NORMAL and a POSITION, in that order. Verified because the
// positions land exactly on the model's bounding-box corners.
struct LTWorldPoint
{
    float           m_Normal[3];
    float           m_Pos[3];
};

// The header of ONE world model's BSP.
//
// ⚠️ ALL of them are readable, and the reason is a field the donor throws away.
// Each world model is preceded by a uint32 the donor reads as `nDummy` and
// never uses (`world_shared_bsp.cpp:223` -- it does NOT seek by it). It is in
// fact the **absolute file offset of the next world model's own nDummy**, so
// the models form a linked list exactly like ABC's chunks.
//
// That turns an all-or-nothing problem into an incremental one: models can be
// walked, and each body is a known byte range, WITHOUT understanding the body.
// Walking all 20,183 world models across the 102 shipped worlds lands exactly
// on `objectDataPos` every time.
struct LTWorldModelHeader
{
    unsigned int    m_nBodyStart;       // first byte after this header
    unsigned int    m_nBodyEnd;         // first byte of the next model's link
    unsigned int    m_nInfoFlags;       // 2 = WIF_MOVEABLE (97 of 102 firsts)
    unsigned int    m_nUnknown;
    std::string     m_sName;            // "TranslucentWorldModel5", "Terrain0"
    unsigned int    m_nPoints, m_nPlanes, m_nSurfaces, m_nUserPortals;
    unsigned int    m_nPolies, m_nLeafs, m_nVerts;
    unsigned int    m_nTotalVisListSize, m_nLeafLists, m_nNodes;
    unsigned int    m_nExtra1, m_nExtra2;
    float           m_Min[3], m_Max[3], m_Translation[3];
    std::vector<std::string> m_Textures;

    // ---- body, present only when m_bBodyParsed ------------------------ //
    bool                         m_bBodyParsed;
    unsigned int                 m_nUnparsedTail;   // section payload, not read
    std::vector<std::string>     m_PortalNames;     // nUserPortals of them
    std::vector<std::vector<unsigned char> > m_PortalData;  // each one's 30 bytes

    // ---- the section block's header, read; its payload is not ---------- //
    //
    // A uniform grid laid over the model. ⚠️ m_SectionGrid is zero for every
    // model with leaves, and its product is the section count elsewhere.
    bool                         m_bSectionHeader;  // false when the tail is too short
    int                          m_SectionGrid[3];  // cells in x, y, z
    float                        m_SectionSize[3];  // cell size, 500 or 1024
    float                        m_SectionOrigin[3];
    std::vector<LTWorldPlane>    m_Planes;
    std::vector<LTWorldSurface>  m_Surfaces;
    std::vector<LTWorldPoly>     m_Polys;
    std::vector<LTWorldPoint>    m_Points;

    // ---- the BSP and its visibility (kept for ltworldvis) -------------- //
    std::vector<LTWorldNode>     m_Nodes;
    int                          m_nRootNode;       // -1 when not read
    std::vector<LTWorldLeaf>     m_Leaves;          // nLeafs of them, VisBSP only
};

class CLTWorld
{
public:

    CLTWorld();

    bool                Load(const void* pData, unsigned int nSize);
    bool                LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName);
    void                Clear();

    bool                IsLoaded() const { return m_bLoaded; }
    const char*         GetError() const { return m_sError.c_str(); }

    unsigned int        GetVersion() const { return m_nVersion; }

    // The authored world properties -- "AmbientLight 32 32 32;LightTableRes
    // 1024;TerrainSubDivSize 20480;PBlockSize 1024;"
    const char*         GetInfoString() const { return m_sInfo.c_str(); }

    unsigned int        GetObjectDataPos() const { return m_nObjectDataPos; }
    unsigned int        GetBlindObjectDataPos() const { return m_nBlindObjectDataPos; }

    const std::vector<LTWorldObject>& GetObjects() const { return m_Objects; }

    // ⚠️ GEOMETRY IS ONLY PARTLY READ. The world-info block, the world tree's
    // shape and the FIRST world model's header are available; the BSP bodies
    // (points, planes, polygons, surfaces, nodes) are NOT parsed. So there is
    // no walkable level yet -- this is the foundation, not the finish.
    bool                    HasGeometryInfo() const { return m_bGeometry; }
    const LTWorldInfo&      GetWorldInfo() const { return m_Info; }
    // Every world model's header, in file order. Empty if the walk failed.
    const std::vector<LTWorldModelHeader>& GetModels() const { return m_Models; }

    // ⚠️ True only when the walk consumed every model AND landed exactly on
    // objectDataPos -- the whole-region check.
    bool                    ModelsAreComplete() const { return m_bModelsComplete; }

    // How many world models' bodies accounted for their bytes exactly, and how
    // many were skipped. ⚠️ Reported rather than hidden: "0 parsed" and "all
    // parsed" must never look alike.
    unsigned int            GetNumBodiesParsed() const { return m_nBodiesParsed; }
    unsigned int            GetNumBodiesSkipped() const { return m_nBodiesSkipped; }

    // ⚠️ Bodies that also accounted for their TRAILING block, i.e. carried no
    // undecoded section data. Always <= GetNumBodiesParsed().
    unsigned int            GetNumBodiesFullyAccounted() const { return m_nBodiesFullyAccounted; }

    // First object of a given class, or 0.
    const LTWorldObject* FindObjectOfType(const char* pType) const;

private:

    bool                Fail(const char* pWhy);

    bool                ReadGeometryInfo(const unsigned char* p, unsigned int nSize,
                                         unsigned int nStart);
    void                ParseBody(const unsigned char* p, LTWorldModelHeader& m);

    bool                        m_bLoaded;
    bool                        m_bGeometry;
    bool                        m_bModelsComplete;
    LTWorldInfo                 m_Info;
    std::vector<LTWorldModelHeader> m_Models;
    std::string                 m_sError;

    unsigned int                m_nVersion;
    unsigned int                m_nBodiesParsed;
    unsigned int                m_nBodiesSkipped;
    unsigned int                m_nBodiesFullyAccounted;
    unsigned int                m_nObjectDataPos;
    unsigned int                m_nBlindObjectDataPos;
    std::string                 m_sInfo;
    std::vector<LTWorldObject>  m_Objects;
};

#endif // __LT_WORLD_H__
