// ----------------------------------------------------------------------- //
//
// MODULE  : ltabc.h
//
// PURPOSE : Reads NOLF 1's `.abc` models. Written from scratch -- see below.
//
// ⚠️ THERE WAS NO DONOR CODE FOR THIS, AND THAT WAS MEASURED, NOT ASSUMED
//
//   The GPL Jupiter EX tree has ZERO real ABC code. The only ABC in its whole
//   runtime is a Linux stub that prints " --- ABC FORMAT NOT SUPPORTED -- ",
//   next to a comment at model_load.cpp:742 claiming the loader handles "both
//   ltb and abc". Jupiter's LTB starts `uint8 filetype; uint16 version;` and
//   ABC starts with a named chunk, so NOLF 1 models fail its "not of .ltb
//   type" check before any version test runs. PORT_PLAN.md 1.
//
// THE CONTAINER
//
//   A flat list of named chunks, each:
//
//       uint16 nameLen;  char name[nameLen];  uint32 nextChunkOffset;  payload
//
//   ⚠️ `nextChunkOffset` is an ABSOLUTE FILE OFFSET, not a size. The list ends
//   when it reads 0xFFFFFFFF -- uniformly, in all 695 shipped files.
//
//   All 695 files carry exactly the same seven chunks in the same order:
//
//       Header -> Pieces -> Nodes -> ChildModels -> Animation -> Sockets
//              -> AnimBindings
//
// ⚠️ NEVER ASSUME v12 -- AND THE DRIFT IS IN TWO SEPARATE CHUNKS
//
//   Measured across all 695: v12 x692, v11 x1, v9 x2. The three off-version
//   files are FORMATS.md 3's drift trap in the flesh, and they differ in two
//   places, so fixing only one still fails:
//
//     Pieces     lodWeight       present in v11 and v12, ABSENT in v9
//     Animation  interpTime      present in v11 and v12, ABSENT in v9
//     Animation  length          present in v12 only
//
//   Both gates were verified NECESSARY by parsing each off-version file both
//   ways: the wrong branch does not merely mis-read, it fails to land on the
//   chunk's declared end. A conditional nobody has seen fail is not a
//   conditional -- HANDOVER.md 5.
//
// HOW THIS LOADER IS CHECKED, AND WHY THAT WORDING MATTERS
//
//   The container hands us an independent statement of where every chunk ends.
//   So the parser does not merely read -- it PREDICTS. After each chunk it
//   asserts that the cursor landed EXACTLY on the declared next offset, and
//   refuses the file otherwise. That is FORMATS.md 2's DTX discipline (predict
//   every file's size from its header) available per chunk rather than per
//   file, and it holds for 695 of 695 shipped models, all seven boundaries.
//
//   ⚠️ Exact byte accounting proves LAYOUT, not MEANING. These were checked
//   separately, and they are what pins the interpretation down:
//
//     * every vertex normal is unit length -- min and max both 1.000000 across
//       all 695 files. A normal read at the wrong offset is not unit length.
//     * face indices, weight node indices and socket node indices are all in
//       range; node `index` fields are exactly their depth-first ordinal.
//     * the sprite case cross-checks two chunks against each other: its node
//       matrix is diag(-1,-1,1,1) and its keyframe quaternion is (0,0,1,0),
//       which is the same 180-degree rotation about Z expressed two ways. The
//       vertex `offset` is likewise the node-space image of `pos` under that
//       matrix. Two independent encodings agreeing is evidence; one parse not
//       crashing is not.
//
// ⚠️ nTris AND nVerts ARE SUMS OVER *ALL* LODs
//
//   Not LOD 0. Reading them as LOD 0 totals passes on 534 files and breaks on
//   exactly the 161 multi-LOD ones -- which are the character models, i.e. the
//   ones that matter. Measured 695/695 the correct way.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_ABC_H__
#define __LT_ABC_H__

#include <string>
#include <vector>

class CLTFileMgr;

// A vertex's attachment to one node. `offset` is in that NODE's space; `pos`
// on the vertex is the same point in MODEL space.
struct LTABCWeight
{
    unsigned int    m_nNode;
    float           m_Offset[3];
    float           m_fBias;
};

struct LTABCVertex
{
    std::vector<LTABCWeight>    m_Weights;
    float                       m_Pos[3];
    float                       m_Normal[3];    // unit length in all shipped data

    // ⚠️ Kept because it is REAL FILE CONTENT, not padding we may invent. In
    // the shipped files it is usually 0xCDCD -- MSVC's uninitialised-heap fill
    // -- so Monolith's exporter wrote out a field it never set. We must still
    // consume it, and pretending it is zero would be a guess.
    unsigned short              m_nUnused;
};

struct LTABCFaceVert
{
    float           m_fU, m_fV;
    unsigned short  m_nIndex;       // into the vertex array of the SAME LOD
};

struct LTABCFace
{
    LTABCFaceVert   m_Verts[3];
};

struct LTABCLod
{
    std::vector<LTABCFace>      m_Faces;
    std::vector<LTABCVertex>    m_Verts;
};

struct LTABCPiece
{
    unsigned short              m_nTexture;
    float                       m_fSpecularPower;
    float                       m_fSpecularScale;
    float                       m_fLODWeight;       // v11+ only; 0 on v9
    unsigned short              m_nUnused;
    std::string                 m_sName;
    std::vector<LTABCLod>       m_Lods;             // exactly nLODs of them
};

// The node tree, FLATTENED depth-first -- which is the order it is stored in
// and, as it happens, exactly what each node's `m_nIndex` field says it is.
struct LTABCNode
{
    std::string     m_sName;
    unsigned short  m_nIndex;
    unsigned char   m_nFlags;
    float           m_Matrix[16];
    unsigned int    m_nChildren;
};

struct LTABCTransform
{
    float           m_Pos[3];
    float           m_Quat[4];      // x, y, z, w
};

struct LTABCChildModel
{
    // Empty for entry 0, which is the model itself -- the file stores no name
    // for it. Entries 1.. are filenames like "basemodel.abc".
    std::string     m_sName;

    // ⚠️ Identified by evidence, not by guessing: a monotonic BUILD NUMBER.
    // A parent records the child's build number as of when the PARENT was
    // exported, so the same child file carries different values in different
    // parents (basemodel.abc appears as 233..820 across 91 parents). The test
    // that settled it: for all ten child models that are themselves shipped,
    // the child's own entry-0 value is >= every value any parent recorded, and
    // usually exactly the maximum -- which is what a monotonic counter does.
    // It is NOT the animation count; that was measured and refuted.
    unsigned int    m_nBuildNumber;

    std::vector<LTABCTransform>  m_Transforms;      // one per node
};

struct LTABCKeyframe
{
    unsigned int    m_nTime;
    std::string     m_sCommand;
};

struct LTABCAnim
{
    float                       m_Dims[3];
    std::string                 m_sName;
    // ⚠️ NEITHER IS WHAT ITS FIRST NAME SAID ("interpTime", "length"). The
    // shipped lithtech.exe settles it: its ModelAnim constructor (0x4459c0)
    // sets +0x18 = -1 and +0x1c = 200, which are the source's
    // `m_ModelEditWeightSet = INVALID_MODEL_WEIGHTSET; m_InterpolationMS =
    // 200;`, and ModelAnim::Load (0x44cf50) reads the v11 field into +0x18
    // and the v12 field into +0x1c. Census over all 695 models: the v11 field
    // is -1 on 2756 animations and 18 on the player's split-body layers (a
    // weight-set index); the v12 field is 200 on 2223, else 0..1500 ms.
    unsigned int                m_nEditWeightSet;   // v11+; ModelEdit only, unused at run time
    unsigned int                m_nInterpMS;        // v12; below v12 the constructor's 200
    std::vector<LTABCKeyframe>  m_Keyframes;

    // [node][keyframe]. Sized nNodes x m_Keyframes.size().
    std::vector<std::vector<LTABCTransform> >  m_Transforms;
};

struct LTABCSocket
{
    unsigned int    m_nNode;
    std::string     m_sName;
    float           m_Rot[4];
    float           m_Pos[3];
};

struct LTABCAnimBinding
{
    std::string     m_sName;
    float           m_Dims[3];
    float           m_Translation[3];
};

struct LTABCWeightSet
{
    std::string         m_sName;
    std::vector<float>  m_Weights;      // one per node
};


class CLTABCModel
{
public:

    CLTABCModel();

    // Reads a whole ABC held in memory. Returns false and leaves the object
    // empty on anything it cannot represent EXACTLY -- see GetError().
    bool                Load(const void* pData, unsigned int nSize);

    // Opens pName through the file manager, trying the name as given and then
    // with ".abc" appended, and reads it.
    bool                LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName);

    // ⚠️ APPEND THE CHILD MODELS' ANIMATIONS -- which is where a character's
    // animations actually are.
    //
    // `FemaleBystander_Lowrank.abc` owns ONE animation: "base", one frame.
    // Standing, idling, walking all live in its children `basemodel.abc` and
    // `fembase.abc`. LithTech appends each child's animations to the parent's
    // table (parent first, then children in file order), and the game looks
    // them up BY NAME through that combined table. Without this, GetAnimIndex
    // found nothing, the character got no animation, and it was drawn in its
    // authored reference pose -- lying flat in mid-air.
    //
    // ⚠️ NODES ARE MATCHED BY NAME, not by index. A child's node order is its
    // own; a node the child lacks takes the parent's own frame-0 transform.
    //
    // ⚠️ A SEPARATE CALL, not part of Load, on purpose: Load is a byte-exact
    // reader checked against every shipped file, and what it reports must stay
    // what the FILE says. The renderer and the server call this; the format
    // tests do not. Children are merged ONE level deep.
    //
    // Returns how many animations were appended.
    unsigned int        MergeChildAnimations(CLTFileMgr* pFileMgr, const char* pSelfName);

    void                Clear();

    bool                IsLoaded() const { return m_bLoaded; }

    // ⚠️ Says WHY, not just that it failed. HANDOVER.md 5: a stub that is
    // handed the reason and throws it away turns a sentence into an
    // investigation.
    const char*         GetError() const { return m_sError.c_str(); }

    unsigned int        GetVersion() const { return m_nVersion; }
    const char*         GetCommandString() const { return m_sCommand.c_str(); }
    float               GetInternalRadius() const { return m_fInternalRadius; }

    unsigned int        GetNumLODs() const { return (unsigned int)m_LodDistances.size(); }
    float               GetLODDistance(unsigned int i) const;

    const std::vector<LTABCPiece>&       GetPieces() const { return m_Pieces; }
    const std::vector<LTABCNode>&        GetNodes() const { return m_Nodes; }
    const std::vector<LTABCChildModel>&  GetChildModels() const { return m_ChildModels; }
    const std::vector<LTABCAnim>&        GetAnims() const { return m_Anims; }
    const std::vector<LTABCSocket>&      GetSockets() const { return m_Sockets; }
    const std::vector<LTABCWeightSet>&   GetWeightSets() const { return m_WeightSets; }

    // One list per child model, index-parallel to GetChildModels().
    const std::vector<std::vector<LTABCAnimBinding> >& GetAnimBindings() const
                                                     { return m_AnimBindings; }

    // ⚠️ THE MODEL'S OWN ORIGIN OFFSET -- the vector from the mesh origin to
    // the centre of the box the model declares for itself. It lives in the
    // AnimBinding for `nAnim` of child model 0, beside the dims, and it is
    // NOT always zero:
    //
    //   Doorknob_01L / _01R    (-5.000,  0.000,  0.000)
    //   SunGlasses_hh "interface" ( 3.500,  0.000, -3.000)
    //   Walther_hh "Hand"      ( 0.000,  0.000,  0.000)
    //   Lamp_02                ( 0.000,  0.000,  0.000)
    //
    // ⚠️ The last two are the CONTROL and they are why this is believed: the
    // models that declare a zero offset are exactly the ones that already
    // looked right (HANDOVER.md -- "the P38 is correct"), and the ones that
    // declare a non-zero offset are exactly the ones reported as wrong. A
    // field that predicts which props look broken is not a coincidence.
    //
    // Returns false and leaves pOut3 ALONE when the model declares none --
    // never zeroes it, because a caller that seeds a default and has it
    // silently overwritten is the bug family HANDOVER.md 2 keeps finding.
    bool                GetOriginOffset(unsigned int nAnim, float* pOut3) const;

    // Totals as the header declares them -- both are sums over ALL LODs.
    unsigned int        GetNumTris() const { return m_nTris; }
    unsigned int        GetNumVerts() const { return m_nVerts; }

    // Counts every triangle and vertex actually read, so a caller can compare
    // against the header rather than trust it.
    unsigned int        CountTris() const;
    unsigned int        CountVerts() const;

private:

    class Reader;

    bool                ReadHeader(Reader& r);
    bool                ReadPieces(Reader& r);
    bool                ReadNodes(Reader& r);
    bool                ReadChildModels(Reader& r);
    bool                ReadAnimation(Reader& r);
    bool                ReadSockets(Reader& r);
    bool                ReadAnimBindings(Reader& r);

    bool                ReadNodeRecursive(Reader& r, unsigned int nDepth);

    bool                Fail(const char* pWhy);
    bool                FailChunk(const char* pChunk, unsigned int nGot,
                                  unsigned int nWant);

    bool                        m_bLoaded;
    std::string                 m_sError;

    unsigned int                m_nVersion;
    unsigned int                m_nKeyframesHdr;
    unsigned int                m_nAnimsHdr;
    unsigned int                m_nNodesHdr;
    unsigned int                m_nPiecesHdr;
    unsigned int                m_nChildModelsHdr;
    unsigned int                m_nTris;
    unsigned int                m_nVerts;
    unsigned int                m_nVertexWeights;
    unsigned int                m_nLODsHdr;
    unsigned int                m_nSocketsHdr;
    unsigned int                m_nWeightSetsHdr;
    unsigned int                m_nStrings;
    unsigned int                m_nStringLengths;

    std::string                 m_sCommand;
    float                       m_fInternalRadius;
    std::vector<float>          m_LodDistances;

    std::vector<LTABCPiece>       m_Pieces;
    std::vector<LTABCNode>        m_Nodes;
    std::vector<LTABCChildModel>  m_ChildModels;
    std::vector<LTABCAnim>        m_Anims;
    std::vector<LTABCSocket>      m_Sockets;
    std::vector<LTABCWeightSet>   m_WeightSets;
    std::vector<std::vector<LTABCAnimBinding> > m_AnimBindings;
};

#endif // __LT_ABC_H__
