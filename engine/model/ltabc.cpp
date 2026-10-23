// ----------------------------------------------------------------------- //
//
// MODULE  : ltabc.cpp
//
// PURPOSE : NOLF 1 `.abc` model reader. See ltabc.h for the format, how it was
//           measured, and why each version gate exists.
//
// ----------------------------------------------------------------------- //

// ⚠️ Include order mirrors the game's own Stdafx.h -- HANDOVER.md §4.3.
// ltfilemgr.h pulls in iltstream.h -> ltbasetypes.h -> ltvector.h, and
// ltvector.h declares LTStream_Read(ILTStream*) without declaring ILTStream.
// It compiles only behind lithtech.h, exactly as every game translation unit
// gets it. Reaching for a forward declaration instead would compile this file
// against different types than the rest of the port -- the ODR mismatch that
// links cleanly and misbehaves at runtime.
#include "windows.h"
#include "lithtech.h"

#include "ltabc.h"
#include "ltfilemgr.h"

#include <cstdio>
#include <cstring>
#include <ctype.h>
#include <map>

// The chunk list terminator. Uniform across all 695 shipped files.
static const unsigned int kEndOfChunks = 0xFFFFFFFFu;

// A node tree deeper than this is a malformed file, not a model. The deepest
// shipped skeleton is nowhere near it; the cap exists so a corrupt nChildren
// cannot recurse the stack away.
static const unsigned int kMaxNodeDepth = 256;


// ----------------------------------------------------------------------- //
// A bounds-checked cursor.
//
// ⚠️ Every read goes through this, and once it overruns it STAYS overrun and
// returns zeroes. That matters more than it looks: the alternative -- letting
// a short read produce a plausible value -- is how a truncated file turns into
// a model with wrong geometry instead of a refusal. The caller checks Bad()
// once per chunk rather than after every field, which keeps the parse readable
// without letting an overrun escape.
// ----------------------------------------------------------------------- //
class CLTABCModel::Reader
{
public:
    Reader(const unsigned char* p, unsigned int n)
        : m_p(p), m_n(n), m_o(0), m_bBad(false) {}

    bool            Bad() const { return m_bBad; }
    unsigned int    Tell() const { return m_o; }
    void            Seek(unsigned int o) { m_o = o; if (m_o > m_n) m_bBad = true; }

    unsigned char U8()
    {
        if (!Want(1)) return 0;
        return m_p[m_o++];
    }

    unsigned short U16()
    {
        if (!Want(2)) return 0;
        unsigned short v = (unsigned short)(m_p[m_o] | (m_p[m_o + 1] << 8));
        m_o += 2;
        return v;
    }

    unsigned int U32()
    {
        if (!Want(4)) return 0;
        unsigned int v = (unsigned int)m_p[m_o]
                       | ((unsigned int)m_p[m_o + 1] << 8)
                       | ((unsigned int)m_p[m_o + 2] << 16)
                       | ((unsigned int)m_p[m_o + 3] << 24);
        m_o += 4;
        return v;
    }

    float F32()
    {
        // ⚠️ Assembled from bytes and memcpy'd, not reinterpret_cast from the
        // buffer: the file is packed, so a float can sit at ANY offset. In the
        // shipped data it routinely does -- a command string of odd length
        // leaves internalRadius on an odd address. An aligned load there is
        // undefined behaviour and traps on some targets.
        unsigned int u = U32();
        float f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    }

    void FN(float* pOut, unsigned int n)
    {
        for (unsigned int i = 0; i < n; ++i) pOut[i] = F32();
    }

    // uint16 length followed by that many bytes. Not NUL-terminated on disk.
    std::string Str()
    {
        unsigned int n = U16();
        if (!Want(n)) return std::string();
        std::string s((const char*)(m_p + m_o), n);
        m_o += n;
        return s;
    }

    // Cheap guard before a loop that would otherwise allocate on a corrupt
    // count. Each element is at least nMinBytes, so a count that could not
    // possibly fit in the remaining bytes is refused before reserving.
    bool CountFits(unsigned int nCount, unsigned int nMinBytes) const
    {
        if (m_bBad) return false;
        if (nMinBytes == 0) return true;
        unsigned int nLeft = (m_o <= m_n) ? (m_n - m_o) : 0;
        return nCount <= nLeft / nMinBytes;
    }

private:
    bool Want(unsigned int n)
    {
        if (m_bBad || m_o + n > m_n || m_o + n < m_o)
        {
            m_bBad = true;
            return false;
        }
        return true;
    }

    const unsigned char*    m_p;
    unsigned int            m_n;
    unsigned int            m_o;
    bool                    m_bBad;
};


// ----------------------------------------------------------------------- //

CLTABCModel::CLTABCModel()
{
    Clear();
}

void CLTABCModel::Clear()
{
    m_bLoaded = false;
    m_sError.clear();

    m_nVersion = 0;
    m_nKeyframesHdr = m_nAnimsHdr = m_nNodesHdr = m_nPiecesHdr = 0;
    m_nChildModelsHdr = m_nTris = m_nVerts = m_nVertexWeights = 0;
    m_nLODsHdr = m_nSocketsHdr = m_nWeightSetsHdr = 0;
    m_nStrings = m_nStringLengths = 0;

    m_sCommand.clear();
    m_fInternalRadius = 0.0f;
    m_LodDistances.clear();

    m_Pieces.clear();
    m_Nodes.clear();
    m_ChildModels.clear();
    m_Anims.clear();
    m_Sockets.clear();
    m_WeightSets.clear();
    m_AnimBindings.clear();
}

bool CLTABCModel::Fail(const char* pWhy)
{
    std::string sKeep = pWhy ? pWhy : "unknown";
    Clear();
    m_sError = sKeep;
    return false;
}

bool CLTABCModel::FailChunk(const char* pChunk, unsigned int nGot,
                            unsigned int nWant)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "chunk '%s' ended at %u but declares its end at %u "
                  "(off by %+d)", pChunk, nGot, nWant, (int)(nGot - nWant));
    return Fail(buf);
}

float CLTABCModel::GetLODDistance(unsigned int i) const
{
    return (i < m_LodDistances.size()) ? m_LodDistances[i] : 0.0f;
}

unsigned int CLTABCModel::CountTris() const
{
    unsigned int n = 0;
    for (size_t p = 0; p < m_Pieces.size(); ++p)
        for (size_t l = 0; l < m_Pieces[p].m_Lods.size(); ++l)
            n += (unsigned int)m_Pieces[p].m_Lods[l].m_Faces.size();
    return n;
}

unsigned int CLTABCModel::CountVerts() const
{
    unsigned int n = 0;
    for (size_t p = 0; p < m_Pieces.size(); ++p)
        for (size_t l = 0; l < m_Pieces[p].m_Lods.size(); ++l)
            n += (unsigned int)m_Pieces[p].m_Lods[l].m_Verts.size();
    return n;
}


// ----------------------------------------------------------------------- //
// Load
// ----------------------------------------------------------------------- //

bool CLTABCModel::Load(const void* pData, unsigned int nSize)
{
    Clear();

    if (!pData || nSize < 16)
        return Fail("not an ABC: too small to hold one chunk header");

    const unsigned char* p = (const unsigned char*)pData;

    // --- walk the chunk directory ------------------------------------- //
    //
    // Collected first, so every chunk's declared end is known before any
    // payload is parsed. That is what lets each chunk be checked against an
    // independent statement of where it stops.
    struct Chunk { std::string m_sName; unsigned int m_nStart, m_nEnd; };
    std::vector<Chunk> chunks;

    unsigned int o = 0;
    for (;;)
    {
        Reader dir(p, nSize);
        dir.Seek(o);
        std::string sName = dir.Str();
        unsigned int nNext = dir.U32();
        if (dir.Bad())
            return Fail("truncated chunk directory");
        if (sName.empty())
            return Fail("chunk with an empty name");

        Chunk c;
        c.m_sName = sName;
        c.m_nStart = dir.Tell();
        c.m_nEnd = (nNext == kEndOfChunks) ? nSize : nNext;

        if (c.m_nEnd > nSize || c.m_nEnd < c.m_nStart)
            return Fail("chunk declares an end outside the file");

        chunks.push_back(c);

        if (nNext == kEndOfChunks)
            break;
        if (nNext <= o)
            return Fail("chunk offsets do not advance");
        o = nNext;

        if (chunks.size() > 64)
            return Fail("absurd number of chunks");
    }

    // --- the seven, in order ------------------------------------------ //
    //
    // ⚠️ Required by NAME and by ORDER. All 695 shipped files agree on both,
    // and Header must be first because every later chunk's size depends on
    // counts it carries.
    static const char* kWant[] = { "Header", "Pieces", "Nodes", "ChildModels",
                                   "Animation", "Sockets", "AnimBindings" };
    const unsigned int kNumWant = sizeof(kWant) / sizeof(kWant[0]);

    if (chunks.size() != kNumWant)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "expected %u chunks, found %u",
                      kNumWant, (unsigned int)chunks.size());
        return Fail(buf);
    }
    for (unsigned int i = 0; i < kNumWant; ++i)
    {
        if (chunks[i].m_sName != kWant[i])
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "chunk %u is '%s', expected '%s'",
                          i, chunks[i].m_sName.c_str(), kWant[i]);
            return Fail(buf);
        }
    }

    typedef bool (CLTABCModel::*ChunkFn)(Reader&);
    ChunkFn fns[kNumWant] = { &CLTABCModel::ReadHeader,
                              &CLTABCModel::ReadPieces,
                              &CLTABCModel::ReadNodes,
                              &CLTABCModel::ReadChildModels,
                              &CLTABCModel::ReadAnimation,
                              &CLTABCModel::ReadSockets,
                              &CLTABCModel::ReadAnimBindings };

    for (unsigned int i = 0; i < kNumWant; ++i)
    {
        // Each chunk reads within its own declared extent, so an overrun is
        // caught at the boundary that declared it rather than by running into
        // the next chunk and parsing it as data.
        Reader r(p, chunks[i].m_nEnd);
        r.Seek(chunks[i].m_nStart);

        if (!(this->*fns[i])(r))
            return false;                       // the callee set the message

        if (r.Bad())
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "chunk '%s' ran past its end",
                          kWant[i]);
            return Fail(buf);
        }

        // THE CHECK THAT MAKES THIS A READER RATHER THAN A GUESS.
        if (r.Tell() != chunks[i].m_nEnd)
            return FailChunk(kWant[i], r.Tell(), chunks[i].m_nEnd);
    }

    m_bLoaded = true;
    return true;
}


// ----------------------------------------------------------------------- //
// Header
// ----------------------------------------------------------------------- //

bool CLTABCModel::ReadHeader(Reader& r)
{
    m_nVersion        = r.U32();

    // ⚠️ Refuse an unknown version LOUDLY rather than assume it resembles v12.
    // Only 9, 11 and 12 exist in the shipped data, and 9 and 11 each differ
    // from 12 in a DIFFERENT chunk. Guessing at a v10 or v13 would produce a
    // model with wrong geometry rather than an error, which is the failure
    // mode this whole file is built to avoid.
    if (m_nVersion != 9 && m_nVersion != 11 && m_nVersion != 12)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "unsupported ABC version %u (known: 9, 11, 12)",
                      m_nVersion);
        return Fail(buf);
    }

    m_nKeyframesHdr   = r.U32();
    m_nAnimsHdr       = r.U32();
    m_nNodesHdr       = r.U32();
    m_nPiecesHdr      = r.U32();
    m_nChildModelsHdr = r.U32();
    m_nTris           = r.U32();
    m_nVerts          = r.U32();
    m_nVertexWeights  = r.U32();
    m_nLODsHdr        = r.U32();
    m_nSocketsHdr     = r.U32();
    m_nWeightSetsHdr  = r.U32();
    m_nStrings        = r.U32();
    m_nStringLengths  = r.U32();

    m_sCommand        = r.Str();
    m_fInternalRadius = r.F32();

    // The count of LOD distances beyond the base. It equals nLODs-1 in all 695
    // shipped files -- but the ARRAY below is sized by nLODs, which is what
    // makes every header end predict exactly, so this word is read and checked
    // rather than used.
    unsigned int nLODDistances = r.U32();

    // 56 bytes that are zero in every shipped file. Consumed because they are
    // there; not interpreted, because nothing has ever put a value in them.
    for (unsigned int i = 0; i < 14; ++i)
        (void)r.U32();

    if (r.Bad())
        return Fail("truncated Header chunk");

    if (m_nLODsHdr == 0 || m_nLODsHdr > 64)
        return Fail("Header declares an implausible LOD count");
    if (nLODDistances + 1 != m_nLODsHdr)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Header LOD-distance count %u does not match nLODs %u",
                      nLODDistances, m_nLODsHdr);
        return Fail(buf);
    }

    m_LodDistances.resize(m_nLODsHdr);
    r.FN(&m_LodDistances[0], m_nLODsHdr);

    return !r.Bad() || Fail("truncated LOD distance table");
}


// ----------------------------------------------------------------------- //
// Pieces
// ----------------------------------------------------------------------- //

bool CLTABCModel::ReadPieces(Reader& r)
{
    // Leading word. It matches the header's nVertexWeights on the shipped
    // data; it is read and ignored rather than trusted, because the per-vertex
    // counts below are authoritative for how much is actually stored.
    (void)r.U32();

    unsigned int nPieces = r.U32();
    if (r.Bad()) return Fail("truncated Pieces chunk");

    if (nPieces != m_nPiecesHdr)
        return Fail("Pieces count disagrees with the Header");
    if (!r.CountFits(nPieces, 16))
        return Fail("Pieces count cannot fit in the chunk");

    m_Pieces.resize(nPieces);

    for (unsigned int i = 0; i < nPieces; ++i)
    {
        LTABCPiece& pc = m_Pieces[i];

        pc.m_nTexture       = r.U16();
        pc.m_fSpecularPower = r.F32();
        pc.m_fSpecularScale = r.F32();

        // ⚠️ v9 DOES NOT HAVE THIS FIELD. Verified necessary: reading it on
        // the two v9 files overruns their Pieces chunk outright.
        pc.m_fLODWeight     = (m_nVersion > 9) ? r.F32() : 0.0f;

        pc.m_nUnused        = r.U16();
        pc.m_sName          = r.Str();

        if (r.Bad()) return Fail("truncated piece record");

        pc.m_Lods.resize(m_nLODsHdr);

        for (unsigned int l = 0; l < m_nLODsHdr; ++l)
        {
            LTABCLod& lod = pc.m_Lods[l];

            unsigned int nFaces = r.U32();
            if (r.Bad()) return Fail("truncated face count");
            if (!r.CountFits(nFaces, 30))
                return Fail("face count cannot fit in the chunk");

            lod.m_Faces.resize(nFaces);
            for (unsigned int f = 0; f < nFaces; ++f)
            {
                for (unsigned int v = 0; v < 3; ++v)
                {
                    LTABCFaceVert& fv = lod.m_Faces[f].m_Verts[v];
                    fv.m_fU     = r.F32();
                    fv.m_fV     = r.F32();
                    fv.m_nIndex = r.U16();
                }
            }

            unsigned int nVerts = r.U32();
            if (r.Bad()) return Fail("truncated vertex count");
            if (!r.CountFits(nVerts, 28))
                return Fail("vertex count cannot fit in the chunk");

            lod.m_Verts.resize(nVerts);
            for (unsigned int v = 0; v < nVerts; ++v)
            {
                LTABCVertex& vt = lod.m_Verts[v];

                unsigned int nWeights = r.U16();
                vt.m_nUnused = r.U16();

                if (r.Bad()) return Fail("truncated vertex record");
                if (!r.CountFits(nWeights, 20))
                    return Fail("vertex weight count cannot fit in the chunk");

                vt.m_Weights.resize(nWeights);
                for (unsigned int w = 0; w < nWeights; ++w)
                {
                    LTABCWeight& wt = vt.m_Weights[w];
                    wt.m_nNode = r.U32();
                    r.FN(wt.m_Offset, 3);
                    wt.m_fBias = r.F32();
                }

                r.FN(vt.m_Pos, 3);
                r.FN(vt.m_Normal, 3);
            }

            // Face indices address this LOD's own vertex array. Checked here
            // because an out-of-range index is a silent out-of-bounds read in
            // whatever draws it later.
            for (unsigned int f = 0; f < nFaces; ++f)
            {
                for (unsigned int v = 0; v < 3; ++v)
                {
                    if (lod.m_Faces[f].m_Verts[v].m_nIndex >= nVerts)
                        return Fail("face references a vertex that does not exist");
                }
            }
        }
    }

    return !r.Bad() || Fail("truncated Pieces chunk");
}


// ----------------------------------------------------------------------- //
// Nodes
// ----------------------------------------------------------------------- //

bool CLTABCModel::ReadNodeRecursive(Reader& r, unsigned int nDepth)
{
    if (nDepth > kMaxNodeDepth)
        return Fail("node tree is implausibly deep");
    if (m_Nodes.size() >= m_nNodesHdr && m_nNodesHdr != 0)
        return Fail("node tree holds more nodes than the Header declares");

    LTABCNode nd;
    nd.m_sName  = r.Str();
    nd.m_nIndex = r.U16();
    nd.m_nFlags = r.U8();
    r.FN(nd.m_Matrix, 16);
    nd.m_nChildren = r.U32();

    if (r.Bad()) return Fail("truncated node record");

    unsigned int nChildren = nd.m_nChildren;
    m_Nodes.push_back(nd);

    if (!r.CountFits(nChildren, 27))
        return Fail("node child count cannot fit in the chunk");

    for (unsigned int i = 0; i < nChildren; ++i)
    {
        if (!ReadNodeRecursive(r, nDepth + 1))
            return false;
    }
    return true;
}

bool CLTABCModel::ReadNodes(Reader& r)
{
    m_Nodes.clear();
    m_Nodes.reserve(m_nNodesHdr);

    if (m_nNodesHdr == 0)
        return Fail("a model with no nodes");

    if (!ReadNodeRecursive(r, 0))
        return false;

    if (m_Nodes.size() != m_nNodesHdr)
        return Fail("node tree holds fewer nodes than the Header declares");

    // Every node's stored index is its depth-first ordinal -- true for all 695
    // shipped files. Checked, because the weights and sockets below index into
    // this flattened array and would silently address the wrong bone.
    for (unsigned int i = 0; i < m_Nodes.size(); ++i)
    {
        if (m_Nodes[i].m_nIndex != i)
            return Fail("node index is not its depth-first position");
    }

    // ⚠️ WEIGHT SETS LIVE IN THE 'Nodes' CHUNK, after the tree -- not in a
    // chunk of their own. Each one carries its OWN node count; omitting that
    // field overruns every character model (142 of them) and lands exactly
    // nowhere.
    unsigned int nSets = r.U32();
    if (r.Bad()) return Fail("truncated weight-set count");

    if (nSets != m_nWeightSetsHdr)
        return Fail("weight-set count disagrees with the Header");
    if (!r.CountFits(nSets, 6))
        return Fail("weight-set count cannot fit in the chunk");

    m_WeightSets.resize(nSets);
    for (unsigned int i = 0; i < nSets; ++i)
    {
        LTABCWeightSet& ws = m_WeightSets[i];
        ws.m_sName = r.Str();

        unsigned int nWeights = r.U32();
        if (r.Bad()) return Fail("truncated weight set");
        if (nWeights != m_nNodesHdr)
            return Fail("weight set does not carry one weight per node");

        ws.m_Weights.resize(nWeights);
        if (nWeights)
            r.FN(&ws.m_Weights[0], nWeights);
    }

    return !r.Bad() || Fail("truncated Nodes chunk");
}


// ----------------------------------------------------------------------- //
// ChildModels
// ----------------------------------------------------------------------- //

bool CLTABCModel::ReadChildModels(Reader& r)
{
    unsigned int n = r.U32();
    if (r.Bad()) return Fail("truncated ChildModels chunk");

    if (n != m_nChildModelsHdr)
        return Fail("child-model count disagrees with the Header");
    if (n == 0)
        return Fail("a model must list at least itself as a child model");
    if (!r.CountFits(n, 4))
        return Fail("child-model count cannot fit in the chunk");

    m_ChildModels.resize(n);
    for (unsigned int i = 0; i < n; ++i)
    {
        LTABCChildModel& cm = m_ChildModels[i];

        // ⚠️ Entry 0 is the model itself and carries NO NAME FIELD at all --
        // not an empty string, no length word. Reading one would swallow four
        // bytes of the build number.
        if (i > 0)
            cm.m_sName = r.Str();

        cm.m_nBuildNumber = r.U32();
        if (r.Bad()) return Fail("truncated child-model record");

        if (!r.CountFits(m_nNodesHdr, 28))
            return Fail("child-model transforms cannot fit in the chunk");

        cm.m_Transforms.resize(m_nNodesHdr);
        for (unsigned int t = 0; t < m_nNodesHdr; ++t)
        {
            r.FN(cm.m_Transforms[t].m_Pos, 3);
            r.FN(cm.m_Transforms[t].m_Quat, 4);
        }
    }

    return !r.Bad() || Fail("truncated ChildModels chunk");
}


// ----------------------------------------------------------------------- //
// Animation
// ----------------------------------------------------------------------- //

bool CLTABCModel::ReadAnimation(Reader& r)
{
    unsigned int n = r.U32();
    if (r.Bad()) return Fail("truncated Animation chunk");

    if (n != m_nAnimsHdr)
        return Fail("animation count disagrees with the Header");
    if (!r.CountFits(n, 20))
        return Fail("animation count cannot fit in the chunk");

    m_Anims.resize(n);
    for (unsigned int i = 0; i < n; ++i)
    {
        LTABCAnim& a = m_Anims[i];

        r.FN(a.m_Dims, 3);
        a.m_sName = r.Str();

        // ⚠️ THE VERSION DRIFT, AND IT IS TWO SEPARATE FIELDS.
        //   v9  : neither      v11 : weight set      v12 : weight set + blend
        // Verified necessary in both directions -- the wrong branch fails to
        // land on this chunk's declared end for the off-version files.
        // A file without the blend field keeps what the shipped constructor
        // gives every animation: 200 ms (see LTABCAnim).
        a.m_nEditWeightSet = (m_nVersion >= 11) ? r.U32() : 0xFFFFFFFFu;
        a.m_nInterpMS      = (m_nVersion >= 12) ? r.U32() : 200u;

        unsigned int nKeyframes = r.U32();
        if (r.Bad()) return Fail("truncated animation record");
        if (!r.CountFits(nKeyframes, 6))
            return Fail("keyframe count cannot fit in the chunk");

        a.m_Keyframes.resize(nKeyframes);
        for (unsigned int k = 0; k < nKeyframes; ++k)
        {
            a.m_Keyframes[k].m_nTime    = r.U32();
            a.m_Keyframes[k].m_sCommand = r.Str();
        }
        if (r.Bad()) return Fail("truncated keyframe list");

        // One transform per node per keyframe, node-major.
        if (nKeyframes && !r.CountFits(m_nNodesHdr, 28 * nKeyframes))
            return Fail("animation transforms cannot fit in the chunk");

        a.m_Transforms.resize(m_nNodesHdr);
        for (unsigned int nd = 0; nd < m_nNodesHdr; ++nd)
            a.m_Transforms[nd].resize(nKeyframes);

        // ⚠️ EXPERIMENT (LT_ABC_KFMAJOR): node-major vs keyframe-major. Both
        // occupy exactly nNodes*nKeyframes*28 bytes, so the chunk-boundary
        // discipline that validated the rest of this loader cannot tell them
        // apart. The bone-stretch oracle can.
        static const bool bKfMajor = (getenv("LT_ABC_KFMAJOR") != 0);
        static const bool bQuatFirst = (getenv("LT_ABC_QUATFIRST") != 0);

        for (unsigned int i0 = 0; i0 < (bKfMajor ? nKeyframes : m_nNodesHdr); ++i0)
            for (unsigned int i1 = 0; i1 < (bKfMajor ? m_nNodesHdr : nKeyframes); ++i1)
            {
                const unsigned int nd = bKfMajor ? i1 : i0;
                const unsigned int k  = bKfMajor ? i0 : i1;
                if (bQuatFirst)
                {
                    r.FN(a.m_Transforms[nd][k].m_Quat, 4);
                    r.FN(a.m_Transforms[nd][k].m_Pos, 3);
                }
                else
                {
                    r.FN(a.m_Transforms[nd][k].m_Pos, 3);
                    r.FN(a.m_Transforms[nd][k].m_Quat, 4);
                }
            }
    }

    return !r.Bad() || Fail("truncated Animation chunk");
}


// ----------------------------------------------------------------------- //
// Sockets
// ----------------------------------------------------------------------- //

bool CLTABCModel::ReadSockets(Reader& r)
{
    unsigned int n = r.U32();
    if (r.Bad()) return Fail("truncated Sockets chunk");

    if (n != m_nSocketsHdr)
        return Fail("socket count disagrees with the Header");
    if (!r.CountFits(n, 34))
        return Fail("socket count cannot fit in the chunk");

    m_Sockets.resize(n);
    for (unsigned int i = 0; i < n; ++i)
    {
        LTABCSocket& s = m_Sockets[i];
        s.m_nNode = r.U32();
        s.m_sName = r.Str();
        r.FN(s.m_Rot, 4);
        r.FN(s.m_Pos, 3);

        if (r.Bad()) return Fail("truncated socket record");
        if (s.m_nNode >= m_nNodesHdr)
            return Fail("socket is attached to a node that does not exist");
    }

    return !r.Bad() || Fail("truncated Sockets chunk");
}


// ----------------------------------------------------------------------- //
// AnimBindings
// ----------------------------------------------------------------------- //

bool CLTABCModel::ReadAnimBindings(Reader& r)
{
    // ⚠️ ONE BLOCK PER CHILD MODEL, each with its own count -- not a single
    // flat list. A single-list reading works on the 554 files that have no
    // child models and under-reads the other 141 by thousands of bytes.
    m_AnimBindings.resize(m_nChildModelsHdr);

    for (unsigned int c = 0; c < m_nChildModelsHdr; ++c)
    {
        unsigned int n = r.U32();
        if (r.Bad()) return Fail("truncated anim-binding count");
        if (!r.CountFits(n, 26))
            return Fail("anim-binding count cannot fit in the chunk");

        m_AnimBindings[c].resize(n);
        for (unsigned int i = 0; i < n; ++i)
        {
            LTABCAnimBinding& b = m_AnimBindings[c][i];
            b.m_sName = r.Str();
            r.FN(b.m_Dims, 3);
            r.FN(b.m_Translation, 3);
        }
        if (r.Bad()) return Fail("truncated anim-binding record");
    }

    return !r.Bad() || Fail("truncated AnimBindings chunk");
}


// ----------------------------------------------------------------------- //

bool CLTABCModel::LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName)
{
    Clear();
    if (!pFileMgr || !pName || !pName[0])
        return Fail("no file manager or no name");

    // As given first, so a name that already ends in .abc is not turned into
    // "....abc.abc" -- the same rule ltpcx.cpp follows for the same reason.
    ILTStream* pStream = pFileMgr->OpenFile(pName);
    if (!pStream)
    {
        std::string sTry(pName);
        sTry += ".abc";
        pStream = pFileMgr->OpenFile(sTry.c_str());
    }
    if (!pStream)
    {
        std::string sWhy = "no tree holds '";
        sWhy += pName;
        sWhy += "'";
        return Fail(sWhy.c_str());
    }

    unsigned long nLen = 0;
    pStream->GetLen(&nLen);

    std::vector<unsigned char> buf((size_t)nLen);
    bool bRead = (nLen > 0) && (pStream->Read(&buf[0], nLen) == LT_OK);
    pStream->Release();

    if (!bRead)
        return Fail("could not read the model out of its archive");

    return Load(&buf[0], (unsigned int)nLen);
}

// ----------------------------------------------------------------------- //
//
// GetOriginOffset
//
// ⚠️ Child model 0 is the model itself (the file stores no name for it), so
// its AnimBinding list is the model's own. Anything else would be reading a
// DIFFERENT model's declaration.
//
// ----------------------------------------------------------------------- //

bool CLTABCModel::GetOriginOffset(unsigned int nAnim, float* pOut3) const
{
    if (!pOut3) return false;
    if (m_AnimBindings.empty()) return false;
    if (nAnim >= m_AnimBindings[0].size()) return false;

    const float* t = m_AnimBindings[0][nAnim].m_Translation;
    pOut3[0] = t[0]; pOut3[1] = t[1]; pOut3[2] = t[2];
    return true;
}


// ----------------------------------------------------------------------- //
// Child-model animations -- see the header.
// ----------------------------------------------------------------------- //

unsigned int CLTABCModel::MergeChildAnimations(CLTFileMgr* pFileMgr, const char* pSelfName)
{
    if (!m_bLoaded || !pFileMgr || m_ChildModels.size() < 2 || m_Nodes.empty()) return 0;

    // The directory the parent came from -- children are named relative to it.
    std::string sDir;
    if (pSelfName)
    {
        std::string s(pSelfName);
        const size_t n = s.find_last_of("\\/");
        if (n != std::string::npos) sDir = s.substr(0, n + 1);
    }

    std::map<std::string, unsigned int> parentIndex;
    for (size_t i = 0; i < m_Nodes.size(); ++i)
    {
        std::string k = m_Nodes[i].m_sName;
        for (size_t c = 0; c < k.size(); ++c) k[c] = (char)tolower((unsigned char)k[c]);
        parentIndex[k] = (unsigned int)i;
    }

    // The fallback local transform for a node the child does not animate: the
    // parent's own first frame, which is its authored pose for that node.
    std::vector<LTABCTransform> fallback(m_Nodes.size());
    for (size_t i = 0; i < m_Nodes.size(); ++i)
    {
        LTABCTransform t = { { 0, 0, 0 }, { 0, 0, 0, 1 } };
        if (!m_Anims.empty() && i < m_Anims[0].m_Transforms.size()
            && !m_Anims[0].m_Transforms[i].empty())
            t = m_Anims[0].m_Transforms[i][0];
        fallback[i] = t;
    }

    unsigned int nAdded = 0;
    for (size_t c = 1; c < m_ChildModels.size(); ++c)
    {
        const std::string& sChild = m_ChildModels[c].m_sName;
        if (sChild.empty()) continue;

        CLTABCModel child;
        if (!child.LoadThroughFileMgr(pFileMgr, (sDir + sChild).c_str()) &&
            !child.LoadThroughFileMgr(pFileMgr, sChild.c_str()))
            continue;

        // child node index -> parent node index, or -1
        std::vector<int> map(child.m_Nodes.size(), -1);
        for (size_t j = 0; j < child.m_Nodes.size(); ++j)
        {
            std::string k = child.m_Nodes[j].m_sName;
            for (size_t q = 0; q < k.size(); ++q) k[q] = (char)tolower((unsigned char)k[q]);
            std::map<std::string, unsigned int>::const_iterator it = parentIndex.find(k);
            if (it != parentIndex.end()) map[j] = (int)it->second;
        }

        // ⚠️ THE EXPORTER RENAMES THE ROOT NULL, AND MATCHING BY NAME ALONE
        // THEREFORE DROPS EVERY MERGED ANIMATION'S ROOT MOTION.
        //
        // Measured on Hero_CasualM1 against its child basemodel.abc:
        //
        //     parent node 0  'null1_3_1_1_1_1'
        //     child  node 0  'null1_1_1_3_1'     <- the same null
        //     ...and all 24 other nodes match exactly.
        //
        // Those suffixes are the modelling tool's uniquifier, not meaning. So
        // 24 of 25 nodes took the child's animation while the root kept the
        // PARENT's rest pose, for all 337 merged animations.
        //
        // ⚠️ THE RULE IS DELIBERATELY NARROW: index-matching is used only when
        // the two skeletons have the SAME NUMBER OF NODES and both the child's
        // node and the parent's node at that index are otherwise unmatched.
        // Two skeletons that genuinely differ are left alone -- inventing a
        // mapping there would pose a character with another character's bones,
        // which is far worse than a dropped animation.
        // LT_NOROOTMATCH=1 restores the name-only behaviour, so the fix can be
        // measured rather than asserted. It says so when it engages.
        static const bool bNoIndex = (getenv("LT_NOROOTMATCH") != 0);
        if (bNoIndex)
        {
            static bool bSaid = false;
            if (!bSaid) { bSaid = true; printf("[MERGE] index matching disabled (LT_NOROOTMATCH)\n"); fflush(stdout); }
        }
        if (!bNoIndex && child.m_Nodes.size() == m_Nodes.size())
        {
            std::vector<bool> taken(m_Nodes.size(), false);
            for (size_t j = 0; j < map.size(); ++j)
                if (map[j] >= 0) taken[(size_t)map[j]] = true;

            unsigned nByIndex = 0;
            for (size_t j = 0; j < map.size(); ++j)
            {
                if (map[j] >= 0 || taken[j]) continue;
                map[j] = (int)j;
                taken[j] = true;
                ++nByIndex;
                if (getenv("LT_MERGETRACE"))
                    printf("[MERGE] '%s' -> '%s' BY INDEX (%u): the names differ, "
                           "the skeletons do not\n",
                           child.m_Nodes[j].m_sName.c_str(),
                           m_Nodes[j].m_sName.c_str(), (unsigned)j);
            }
            (void)nByIndex;
        }

        // ⚠️ LT_MERGETRACE=1 -- WHICH PARENT NODES THIS CHILD ANIMATES, and
        // which are left at the parent's rest pose. A child that animates the
        // body but not the head leaves the head nodes behind; whether that is
        // correct depends entirely on WHICH nodes they are, and no other
        // instrument shows it.
        if (getenv("LT_MERGETRACE"))
        {
            unsigned nMapped = 0;
            for (size_t j = 0; j < map.size(); ++j) if (map[j] >= 0) ++nMapped;

            printf("[MERGE] %-34s %u of its %u node(s) match the parent's %u; "
                   "%u animation(s)\n", sChild.c_str(), nMapped,
                   (unsigned)child.m_Nodes.size(), (unsigned)m_Nodes.size(),
                   (unsigned)child.m_Anims.size());

            std::vector<bool> covered(m_Nodes.size(), false);
            for (size_t j = 0; j < map.size(); ++j)
                if (map[j] >= 0) covered[(size_t)map[j]] = true;
            std::string sLeft;
            for (size_t n = 0; n < covered.size(); ++n)
                if (!covered[n]) { sLeft += " "; sLeft += m_Nodes[n].m_sName; }
            if (!sLeft.empty())
                printf("        left at the parent's rest pose:%s\n", sLeft.c_str());

            // And the child's own names, so a MISMATCH can be told from an
            // absence: a node the child simply does not have is a different
            // problem from one it has under another name.
            printf("        the child's node 0..3:");
            for (size_t j = 0; j < child.m_Nodes.size() && j < 4; ++j)
                printf(" '%s'%s", child.m_Nodes[j].m_sName.c_str(),
                       (map[j] < 0) ? "(UNMATCHED)" : "");
            printf("\n        the parent's node 0..3:");
            for (size_t n = 0; n < m_Nodes.size() && n < 4; ++n)
                printf(" '%s'", m_Nodes[n].m_sName.c_str());
            printf("\n");
            fflush(stdout);
        }

        for (size_t a = 0; a < child.m_Anims.size(); ++a)
        {
            const LTABCAnim& src = child.m_Anims[a];
            LTABCAnim dst;
            for (int k = 0; k < 3; ++k) dst.m_Dims[k] = src.m_Dims[k];
            dst.m_sName       = src.m_sName;
            dst.m_nEditWeightSet = src.m_nEditWeightSet;
            dst.m_nInterpMS      = src.m_nInterpMS;
            dst.m_Keyframes   = src.m_Keyframes;

            const size_t nKf = src.m_Keyframes.size();
            dst.m_Transforms.assign(m_Nodes.size(),
                                    std::vector<LTABCTransform>());
            for (size_t n = 0; n < m_Nodes.size(); ++n)
                dst.m_Transforms[n].assign(nKf, fallback[n]);

            for (size_t j = 0; j < map.size(); ++j)
            {
                if (map[j] < 0 || j >= src.m_Transforms.size()) continue;
                const std::vector<LTABCTransform>& v = src.m_Transforms[j];
                for (size_t k = 0; k < nKf && k < v.size(); ++k)
                    dst.m_Transforms[(size_t)map[j]][k] = v[k];
            }

            m_Anims.push_back(dst);
            ++nAdded;
        }
    }
    return nAdded;
}
