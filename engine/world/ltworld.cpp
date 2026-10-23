// ----------------------------------------------------------------------- //
//
// MODULE  : ltworld.cpp
//
// PURPOSE : v66 world reader -- header and object list. See ltworld.h.
//
// ----------------------------------------------------------------------- //

// ⚠️ Include order mirrors the game's own Stdafx.h -- HANDOVER.md §4.3.
#include "windows.h"
#include "lithtech.h"

#include "ltworld.h"
#include "ltfilemgr.h"

#include <cstdio>
#include <cstring>
#include <cmath>

namespace
{
    // ⚠️ v66: version + TWO positions + 8 words. v85 has four more positions;
    // using its layout starts the info string 16 bytes late.
    const unsigned int kInfoLenOffset = 44;
    const unsigned int kWorldVersion  = 66;

    class Reader
    {
    public:
        Reader(const unsigned char* p, unsigned int n, unsigned int o = 0)
            : m_p(p), m_n(n), m_o(o), m_bBad(false) {}

        bool Bad() const { return m_bBad; }
        unsigned int Tell() const { return m_o; }
        void Seek(unsigned int o) { m_o = o; if (m_o > m_n) m_bBad = true; }

        unsigned char U8()
        {
            if (!Want(1)) return 0;
            return m_p[m_o++];
        }
        unsigned short U16()
        {
            if (!Want(2)) return 0;
            unsigned short v = (unsigned short)(m_p[m_o] | (m_p[m_o+1] << 8));
            m_o += 2; return v;
        }
        unsigned int U32()
        {
            if (!Want(4)) return 0;
            unsigned int v = (unsigned int)m_p[m_o] | ((unsigned int)m_p[m_o+1] << 8)
                           | ((unsigned int)m_p[m_o+2] << 16) | ((unsigned int)m_p[m_o+3] << 24);
            m_o += 4; return v;
        }
        float F32()
        {
            // Assembled and memcpy'd: the file is packed and floats land at
            // arbitrary offsets.
            unsigned int u = U32(); float f;
            std::memcpy(&f, &u, sizeof(f)); return f;
        }
        // uint16 count + bytes -- the object list's string form.
        std::string Str()
        {
            unsigned int n = U16();
            if (!Want(n)) return std::string();
            std::string s((const char*)(m_p + m_o), n);
            m_o += n; return s;
        }

        // ⚠️ uint32 count + bytes -- the HEADER's info string uses a 32-bit
        // length while every string in the object list uses 16. Reading the
        // header one with Str() takes the low half (which happens to be the
        // right VALUE) and then starts the text two bytes early, on the
        // length's own high-order zero bytes. The result is a string that
        // begins with NUL, so it measures as non-empty but PRINTS as empty --
        // caught only by diffing against an independent reader.
        std::string Str32()
        {
            unsigned int n = U32();
            if (!Want(n)) return std::string();
            std::string s((const char*)(m_p + m_o), n);
            m_o += n; return s;
        }
        void Skip(unsigned int n) { if (Want(n)) m_o += n; }
        const unsigned char* At() const { return m_p + m_o; }

        bool CountFits(unsigned int nCount, unsigned int nMin) const
        {
            if (m_bBad || nMin == 0) return !m_bBad;
            unsigned int left = (m_o <= m_n) ? (m_n - m_o) : 0;
            return nCount <= left / nMin;
        }

    private:
        bool Want(unsigned int n)
        {
            if (m_bBad || m_o + n > m_n || m_o + n < m_o) { m_bBad = true; return false; }
            return true;
        }
        const unsigned char* m_p;
        unsigned int m_n, m_o;
        bool m_bBad;
    };
}


const LTWorldProperty* LTWorldObject::Find(const char* pName) const
{
    if (!pName) return 0;
    for (size_t i = 0; i < m_Props.size(); ++i)
        if (strcasecmp(m_Props[i].m_sName.c_str(), pName) == 0) return &m_Props[i];
    return 0;
}

bool LTWorldObject::GetPos(float* pOut) const
{
    const LTWorldProperty* p = Find("Pos");
    if (!p || p->m_nType != kLTWP_Vector) return false;
    for (int i = 0; i < 3; ++i) pOut[i] = p->m_Vec[i];
    return true;
}

bool LTWorldObject::GetEuler(float* pOut) const
{
    const LTWorldProperty* p = Find("Rotation");
    if (!p || p->m_nType != kLTWP_Rotation) return false;
    for (int i = 0; i < 3; ++i) pOut[i] = p->m_Euler[i];
    return true;
}

const char* LTWorldObject::GetName() const
{
    const LTWorldProperty* p = Find("Name");
    return (p && p->m_nType == kLTWP_String) ? p->m_sString.c_str() : "";
}


CLTWorld::CLTWorld() { Clear(); }

void CLTWorld::Clear()
{
    m_bLoaded = false;
    m_bGeometry = false;
    m_bModelsComplete = false;
    m_nBodiesParsed = 0;
    m_nBodiesSkipped = 0;
    m_nBodiesFullyAccounted = 0;
    m_Models.clear();
    std::memset(&m_Info, 0, sizeof(m_Info));
    m_sError.clear();
    m_nVersion = 0;
    m_nObjectDataPos = m_nBlindObjectDataPos = 0;
    m_sInfo.clear();
    m_Objects.clear();
}

bool CLTWorld::Fail(const char* pWhy)
{
    std::string keep = pWhy ? pWhy : "unknown";
    Clear();
    m_sError = keep;
    return false;
}

const LTWorldObject* CLTWorld::FindObjectOfType(const char* pType) const
{
    if (!pType) return 0;
    for (size_t i = 0; i < m_Objects.size(); ++i)
        if (strcasecmp(m_Objects[i].m_sType.c_str(), pType) == 0) return &m_Objects[i];
    return 0;
}


namespace
{
    // Walks the world tree's bit stream. Returns false on overrun; otherwise
    // leaves the cursor just past the stream and reports the subdivision count.
    //
    // ⚠️ Iterative, not recursive. A corrupt bit pattern in a 12 MB file can
    // otherwise recurse until the stack dies, and the depth is data-controlled.
    bool WalkTree(const unsigned char* p, unsigned int nSize, unsigned int& o,
                  unsigned int& nSubs)
    {
        unsigned char nByte = 0;
        unsigned int nBit = 8;
        unsigned int nPending = 1;       // the root
        nSubs = 0;

        while (nPending > 0)
        {
            if (nBit == 8)
            {
                if (o >= nSize) return false;
                nByte = p[o++];
                nBit = 0;
            }
            bool bSub = ((nByte >> nBit) & 1) != 0;
            ++nBit;
            --nPending;

            if (bSub)
            {
                ++nSubs;
                nPending += 4;           // MAX_WTNODE_CHILDREN
                if (nPending > (1u << 24)) return false;
            }
        }
        return true;
    }
}

// ----------------------------------------------------------------------- //
// One world model's BSP body.
//
// ⚠️ ALL-OR-NOTHING, ON PURPOSE. The parse must land EXACTLY on the model's
// declared end; anything else leaves m_bBodyParsed false and the caller skips
// the model. Because the models are a linked list, skipping one costs nothing
// -- the next model's position is known independently. Half-parsed geometry
// would be far worse than none.
// ----------------------------------------------------------------------- //

void CLTWorld::ParseBody(const unsigned char* p, LTWorldModelHeader& m)
{
    if (m.m_nBodyEnd <= m.m_nBodyStart) return;

    // ⚠️ Each polygon carries a VARIABLE-LENGTH block of 4-byte entries, and
    // `extra2` is the TOTAL number of entries across the model -- not a count
    // of polygons that have one. Most polygons have exactly one; some have
    // none and some have two. See the polygon loop for the rule that decides.

    Reader r(p, m.m_nBodyEnd, m.m_nBodyStart);

    // ---- polygon vertex counts: TWO uint8s per polygon ------------------ //
    //
    // ⚠️ NOT a uint16. The donor's v85 loader reads a byte here, and it was
    // right; reading the pair as one uint16 works only because the second byte
    // is zero in 19,189 of 20,183 models. In the other 994 -- every large lit
    // model, PhysicsBSP among them -- it is not, and a uint16 read yields
    // vertex counts like 1284 and 1796.
    //
    // The two bytes are separate counts. The cross-check that settles it:
    // summing ONLY the first byte reproduces the model's declared m_nVerts for
    // all 20,183 models, while summing both fails for exactly those 994. So
    // the first byte is the real vertex count and the second counts additional
    // 5-byte records appended to the same polygon.
    std::vector<unsigned int> vertCounts(m.m_nPolies), extraCounts(m.m_nPolies);
    unsigned int nVertTotal = 0;
    for (unsigned int i = 0; i < m.m_nPolies; ++i)
    {
        vertCounts[i]  = r.U8();
        extraCounts[i] = r.U8();
        if (vertCounts[i] == 0 || vertCounts[i] > 64) return;
        if (extraCounts[i] > 64) return;
        nVertTotal += vertCounts[i];
    }
    if (r.Bad() || nVertTotal != m.m_nVerts) return;

    // ---- leaf lists (VisBSP only) --------------------------------------- //
    //
    // The one block in this format that did NOT have to be reverse engineered:
    // the donor still walks it, at de_mainworld.cpp:503, to skip past it.
    //
    //     uint16 nLists
    //     nLists == 0xFFFF -> uint16   (this leaf shares another leaf's lists)
    //     otherwise        -> nLists x { uint16 portalID; uint16 nBytes;
    //                                    uint8 data[nBytes] }
    //
    // ⚠️ v66 then adds TWO fields the v85 loader does not read -- a uint32
    // count followed by that many 4-byte entries, and one more 4-byte value.
    // Without them the walk desynchronises on the second leaf.
    //
    // ⚠️ THE HEADER PAYS FOR ITSELF HERE. Three counts declared in the model
    // header are exactly reproduced by this walk, and they are checked:
    //
    //     nLeafLists        == number of lists actually read
    //     totalVisListSize  == total bytes of visibility data
    //     extra1            == total number of the 4-byte entries
    //
    // That last one is what `extra1` MEANS -- previously unidentified, and
    // nonzero in exactly the 102 models that have leaves, one per world.
    // Three independent counts agreeing is a far stronger check than landing
    // on the right offset, which any off-by-N-in-two-places error can fake.
    unsigned int nListsSeen = 0, nVisBytes = 0, nEntriesSeen = 0;
    std::vector<LTWorldLeaf> leaves(m.m_nLeafs);
    for (unsigned int i = 0; i < m.m_nLeafs; ++i)
    {
        LTWorldLeaf& lf = leaves[i];
        lf.m_nShare = -1;
        unsigned int nLists = r.U16();
        if (r.Bad()) return;

        if (nLists == 0xFFFF)
        {
            lf.m_nShare = (int)r.U16();          // shares another leaf's lists
        }
        else
        {
            if (nLists > 256) return;
            lf.m_Lists.resize(nLists);
            for (unsigned int k = 0; k < nLists; ++k)
            {
                lf.m_Lists[k].m_nPortal = (unsigned short)r.U16();
                unsigned int nBytes = r.U16();
                if (r.Bad() || r.Tell() + nBytes > m.m_nBodyEnd) return;
                lf.m_Lists[k].m_Data.assign(p + r.Tell(), p + r.Tell() + nBytes);
                r.Skip(nBytes);
                nVisBytes += nBytes;
            }
            nListsSeen += nLists;
        }

        unsigned int nEntries = r.U32();
        if (r.Bad() || nEntries > (1u << 20)) return;
        lf.m_Entries.resize(nEntries);
        for (unsigned int k = 0; k < nEntries; ++k) lf.m_Entries[k] = r.U32();
        lf.m_nTrailer = r.U32();
        nEntriesSeen += nEntries;
        if (r.Bad()) return;
    }
    if (nListsSeen   != m.m_nLeafLists)        return;
    if (nVisBytes    != m.m_nTotalVisListSize) return;
    if (nEntriesSeen != m.m_nExtra1)           return;
    for (unsigned int i = 0; i < m.m_nLeafs; ++i)
        if (leaves[i].m_nShare >= (int)m.m_nLeafs) return;

    // ---- planes -------------------------------------------------------- //
    m.m_Planes.resize(m.m_nPlanes);
    for (unsigned int i = 0; i < m.m_nPlanes; ++i)
    {
        for (int k = 0; k < 3; ++k) m.m_Planes[i].m_Normal[k] = r.F32();
        m.m_Planes[i].m_Dist = r.F32();
    }
    if (r.Bad()) { m.m_Planes.clear(); return; }

    // ---- surfaces ------------------------------------------------------ //
    m.m_Surfaces.resize(m.m_nSurfaces);
    for (unsigned int i = 0; i < m.m_nSurfaces; ++i)
    {
        LTWorldSurface& sf = m.m_Surfaces[i];
        for (int k = 0; k < 3; ++k) sf.m_O[k] = r.F32();
        for (int k = 0; k < 3; ++k) sf.m_P[k] = r.F32();
        for (int k = 0; k < 3; ++k) sf.m_Q[k] = r.F32();
        for (int k = 0; k < 14; ++k) sf.m_Fixed[k] = r.U8();
        sf.m_nTexture = (unsigned short)(sf.m_Fixed[0] | (sf.m_Fixed[1] << 8));
        sf.m_nFlags = (unsigned int)sf.m_Fixed[6]
                    | ((unsigned int)sf.m_Fixed[7] << 8)
                    | ((unsigned int)sf.m_Fixed[8] << 16)
                    | ((unsigned int)sf.m_Fixed[9] << 24);

        unsigned int nEffects = r.U8();
        if (r.Bad() || nEffects > 8) { m.m_Planes.clear(); m.m_Surfaces.clear(); return; }
        for (unsigned int e = 0; e < nEffects; ++e)
        {
            LTWorldSurfaceEffect fx;
            fx.m_sName  = r.Str();
            fx.m_sValue = r.Str();
            sf.m_Effects.push_back(fx);
        }
        sf.m_nTrailing = r.U16();
        if (r.Bad()) { m.m_Planes.clear(); m.m_Surfaces.clear(); return; }
    }

    // ---- polygons: 22 fixed + 5 per vertex ----------------------------- //
    m.m_Polys.resize(m.m_nPolies);
    unsigned int nExtraTotal = 0;
    for (unsigned int i = 0; i < m.m_nPolies; ++i)
    {
        LTWorldPoly& po = m.m_Polys[i];
        for (int k = 0; k < 3; ++k) po.m_Centre[k] = r.F32();
        po.m_nUnknown0 = r.U32();                 // two uint16s, both 0 or both set

        // ---- the variable block, and the rule that sizes it ------------- //
        //
        // ⚠️ TWO RULES IN ONE, and neither is guessable from byte totals:
        //
        //   1. The pair of uint16s above is zero EXACTLY when the polygon has
        //      no entries at all.
        //   2. Otherwise the FIRST entry doubles as the count -- the uint32
        //      here is how many 4-byte entries there are, itself included.
        //
        // ⚠️ Reading that uint32 unconditionally is wrong and looks right: it
        // is 1 for 6,978 polygons in models that declare `extra2`, which is
        // convincing, but it is >= 8 for 6,567 polygons in models declaring
        // `extra2 == 0`, where the true count must be zero. The zero-pair
        // test is what separates the two cases.
        //
        // How the rule was found: the block sizes were first recovered WITHOUT
        // a rule, by searching every assignment of per-polygon counts that
        // both lands exactly on the polygon block's end and gives every vertex
        // a valid point index. That search is far too slow to ship, but it
        // produced ground truth for one model (5,049 polygons), and the rule
        // above reproduces it exactly -- then reproduces every other model too.
        unsigned int nExtra = 0;
        if (po.m_nUnknown0 != 0)
        {
            nExtra = r.U32();
            if (r.Bad() || nExtra == 0 || nExtra > 64)
            { m.m_Planes.clear(); m.m_Surfaces.clear(); m.m_Polys.clear(); return; }
            r.Skip(4 * (nExtra - 1));
        }
        nExtraTotal += nExtra;

        // ⚠️ The low half of this word runs 0,1,2,... across the model's
        // polygons; the HIGH half is the surface index, and it is in range for
        // every polygon measured. Kept as one U32 for continuity, then split.
        po.m_nIndexWord = r.U32();
        po.m_nSurface   = (unsigned short)(po.m_nIndexWord >> 16);
        po.m_nUnknown1  = r.U16();

        if (po.m_nSurface >= m.m_nSurfaces)
        { m.m_Planes.clear(); m.m_Surfaces.clear(); m.m_Polys.clear(); return; }

        po.m_Verts.resize(vertCounts[i]);
        for (unsigned int v = 0; v < vertCounts[i]; ++v)
        {
            po.m_Verts[v].m_nPoint = r.U16();
            for (int k = 0; k < 3; ++k) po.m_Verts[v].m_RGB[k] = r.U8();
        }
        // ⚠️ The extra records are the same width as a vertex entry and sit
        // directly after them. Skipped rather than invented: their contents are
        // unread, but their SIZE is what makes the polygon block land exactly.
        r.Skip(5 * extraCounts[i]);

        if (r.Bad()) { m.m_Planes.clear(); m.m_Surfaces.clear(); m.m_Polys.clear(); return; }
    }

    // ---- nodes + the extra2 array, then the points ---------------------- //
    //
    // ⚠️ THIS OFFSET WAS MEASURED, NOT ASSUMED, and that is the only reason it
    // is right. Earlier attempts derived it from byte accounting and produced
    // two plausible-but-wrong answers (20-byte nodes, then 16) that both
    // balanced the totals and both failed the meaning checks.
    //
    // What settled it: locate the point array EMPIRICALLY in each model -- the
    // one place where nPoints consecutive 24-byte records all have a position
    // inside the model's own bounding box AND a unit normal -- then fit a
    // formula to the measured offsets:
    //
    //     pointsStart = endOfPolygons + 14*nNodes + 4*extra2
    //
    // ⚠️ 14 is precisely the v85 node size (uint32 poly + uint16 leaf + uint32
    // sides[2]) -- the node record did NOT change between versions, which is
    // why every guess that made it bigger to absorb `extra2` was wrong. The
    // 14-byte records are directly visible in the bytes before the points,
    // repeating on a 14-byte stride right up to pointsStart.
    //
    // ⚠️ An earlier version of this line carried a "-12" here and a matching
    // +12 in the trailing block, described as real and unexplained. It was
    // neither: the point RECORD is position-then-normal, and reading it
    // normal-first shifted the whole array by 12 bytes. The constant was the
    // shape of that mistake, not of the format. THE UNEXPLAINED CONSTANT WAS
    // THE BUG -- the arithmetic was covering for a wrong field order.
    // ⚠️ THE COUNTS MUST AGREE. The per-polygon entries are the whole of
    // `extra2`, so their sum is a free cross-check on the entire polygon walk:
    // if any polygon's block was mis-sized, this will not balance even when
    // the model still happens to land on its declared end.
    if (nExtraTotal != m.m_nExtra2)
    { m.m_Planes.clear(); m.m_Surfaces.clear(); m.m_Polys.clear(); return; }

    // ⚠️ Only the nodes. The `4 * extra2` that used to be added here is the
    // same storage as the per-polygon entries above -- counted once, now that
    // it is read where it actually lives.
    //
    // Kept only for models with leaves: ltworldvis walks the VisBSP's tree to
    // find the leaf a point is in. Everything else skips them as before.
    if (m.m_nLeafs != 0)
    {
        m.m_Nodes.resize(m.m_nNodes);
        for (unsigned int i = 0; i < m.m_nNodes; ++i)
        {
            LTWorldNode& nd = m.m_Nodes[i];
            nd.m_nPoly    = r.U32();
            nd.m_nLeaf    = (unsigned short)r.U16();
            nd.m_Sides[0] = (int)r.U32();
            nd.m_Sides[1] = (int)r.U32();
        }
        if (r.Bad()) { m.m_Planes.clear(); m.m_Surfaces.clear(); m.m_Polys.clear(); m.m_Nodes.clear(); return; }
    }
    else
        r.Skip(14 * m.m_nNodes);

    // ---- user portals --------------------------------------------------- //
    //
    //     uint16 nameLen; char name[nameLen]; uint8 payload[30]
    //
    // ⚠️ `nUserPortals` is the count, and it is the third header field this
    // pass has given a meaning. It is nonzero in exactly 29 of the 20,183
    // models -- and those 29 are EXACTLY the VisBSPs whose points did not sit
    // where the arithmetic predicted. No model without leaves has any.
    //
    // The names are real and readable: "Room2Portal", "bigdoorportal",
    // "portal1". ⚠️ That is what makes this block certain rather than merely
    // consistent -- a size that happens to fit proves nothing, but a length
    // prefix that lands exactly on a legible English name, 29 times, does.
    for (unsigned int i = 0; i < m.m_nUserPortals; ++i)
    {
        unsigned int nLen = r.U16();
        if (r.Bad() || nLen > 64)
        { m.m_Planes.clear(); m.m_Surfaces.clear(); m.m_Polys.clear(); return; }

        std::string sName;
        for (unsigned int k = 0; k < nLen; ++k) sName += (char)r.U8();
        m.m_PortalNames.push_back(sName);
        std::vector<unsigned char> pay(30);
        for (unsigned int k = 0; k < 30; ++k) pay[k] = r.U8();
        m.m_PortalData.push_back(pay);
        if (r.Bad())
        { m.m_Planes.clear(); m.m_Surfaces.clear(); m.m_Polys.clear(); return; }
    }
    if (r.Bad()) { m.m_Planes.clear(); m.m_Surfaces.clear(); m.m_Polys.clear(); return; }

    // ---- points: a POSITION and then a normal --------------------------- //
    //
    // ⚠️ Position first. Both orders "work" on a boundary check because the
    // record is 24 bytes either way, and both survive a unit-length test on a
    // cube whose corner coordinates happen to decode as small floats. The
    // order was fixed by dumping a model with a known bounding box and reading
    // the numbers: the first triple is the box corner, the second is unit.
    m.m_Points.resize(m.m_nPoints);
    for (unsigned int i = 0; i < m.m_nPoints; ++i)
    {
        for (int k = 0; k < 3; ++k) m.m_Points[i].m_Pos[k] = r.F32();
        for (int k = 0; k < 3; ++k) m.m_Points[i].m_Normal[k] = r.F32();
    }

    // ---- the section block ---------------------------------------------- //
    //
    // Everything after the points is ONE structure, and its first 36 bytes are
    // a header that IS decoded:
    //
    //     int32 nX, nY, nZ        a uniform grid laid over the model
    //     float sizeX, sizeY, sizeZ   cell size -- 500 or 1024 in the shipped data
    //     float originX, Y, Z     where the grid starts
    //
    // ⚠️ How that was confirmed, rather than guessed: the grid dimensions are
    // PREDICTABLE from geometry the model already declares. ceil(extent / cell
    // size), computed from the model's own bounding box, reproduces the stored
    // nX,nY,nZ for 19,360 of 19,473 models. A three-int field agreeing with an
    // independent geometric prediction that many times is not a coincidence.
    //
    // ⚠️ And every model WITH LEAVES has this header zeroed -- all 102 of them,
    // grid (0,0,0) and cell size (0,0,0) -- so a VisBSP carries no sections at
    // all. That is why their tail is a flat 44 bytes.
    //
    // The section COUNT is nX*nY*nZ. The payload is variable and NOT decoded;
    // for a model with a single section the whole tail comes to
    //
    //     tail = 20 + 6*nNodes + 28*nSections     (exact for 16,584 models)
    //
    // ⚠️ A CORRECTION TO WHAT THIS FILE USED TO SAY. The old comment here
    // claimed 48 constant bytes of `0, 1.0, 0, 1, 1, 1, 8192, 8192, 8192` and
    // "6 bytes per node, so the node record is split 14/6". The 48 bytes are
    // not constant -- they are this header -- and the trailing per-node array
    // is NOT exactly 6*nNodes: models exist whose tail is short by 6, 24 or 30
    // bytes, always a MULTIPLE OF SIX, so the array is 6*(nNodes - k) for a k
    // that has not been identified. The old figure fitted because k is zero in
    // the overwhelming majority.
    //
    // ⚠️ AND FOR 3,857 MODELS IT IS LONGER, because a SECTION BLOCK sits
    // between the points and this trailer. The donor names it: v85 reads a
    // root index and `nSections` immediately after the points and REFUSES any
    // file with `nSections > 0` (`de_mainworld.cpp:622`), so the structure was
    // deleted from the engine before the version this donor ships. v66 worlds
    // still carry it -- 512,760 bytes of it in `PhysicsBSP` alone -- and it
    // holds no strings and no stride that any count in the header divides.
    // It is NOT decoded here.
    //
    // ⚠️ SO THIS IS THE ONE PLACE THE PARSER READS LESS THAN THE WHOLE MODEL,
    // and it is deliberate. The section block is SKIPPED, not parsed, and the
    // number of bytes skipped is published in m_nUnparsedTail so a caller can
    // never mistake a partially-read model for a fully-read one. What makes
    // that safe rather than sloppy:
    //
    //   * the model's end is known INDEPENDENTLY, from the linked list, so the
    //     skip is to a known offset -- no length is being guessed;
    //   * everything the renderer consumes -- polygons, vertex indices, points
    //     -- lies BEFORE the block and is still checked exactly; and
    //   * the polygon walk has its own end-to-end proof that does not depend
    //     on the tail at all: the per-polygon entry counts must sum to the
    //     model's declared `extra2`.
    //
    // ⚠️ Models that DO account for their tail exactly are counted separately
    // (GetNumBodiesFullyAccounted). That number, not the parse count, is the
    // one to watch when the section block is eventually decoded.
    //
    // ⚠️ AND IT IS A DIFFERENT SIZE FOR MODELS WITH LEAVES -- a flat 44 bytes,
    // exact for all 102 VisBSPs, which is what a zeroed section header costs.
    unsigned int nTailHave = (m.m_nBodyEnd > r.Tell()) ? (m.m_nBodyEnd - r.Tell()) : 0;
    unsigned int nTailWant = (m.m_nLeafs != 0) ? 44u : (48 + 6 * m.m_nNodes);

    // ⚠️ A SHORT TAIL IS NOT A REASON TO THROW THE GEOMETRY AWAY, and treating
    // it as one refused 183 models across 12 worlds -- 7.3% of every polygon in
    // the game, including one entire level's `PhysicsBSP`.
    //
    // The size above is a PREDICTION about a block this parser never reads: the
    // very next statement skips the whole tail to the model's declared end.
    // Refusing on it meant refusing on the accuracy of a guess rather than on
    // the correctness of anything, and the guess is known to be imperfect --
    // FORMATS.md already records the trailing array as `6*(nNodes - k)` for a
    // `k` nobody has identified. Measured here, `k` ranges from 0 to nNodes
    // itself: `V2` declares 820 nodes and carries a 48-byte tail with no
    // per-node array at all, and one `PhysicsBSP` is short by exactly 78 = 6*13.
    //
    // ⚠️ WHAT STILL GUARDS THE GEOMETRY, and why this is safe: everything the
    // renderer consumes -- points, planes, surfaces, polygons -- is read BEFORE
    // the tail and is still checked exactly, by the boundary check below (the
    // parse must land on the model's declared end) AND by the meaning checks
    // (unit normals, points inside the model's own bounding box, every vertex
    // index addressing a real point). A misparse that left the tail short would
    // have had to consume the missing bytes as geometry, and geometry made of
    // the wrong bytes does not have unit normals.
    if (nTailHave >= 36)
    {
        for (int k = 0; k < 3; ++k) m.m_SectionGrid[k]   = (int)r.U32();
        for (int k = 0; k < 3; ++k) m.m_SectionSize[k]   = r.F32();
        for (int k = 0; k < 3; ++k) m.m_SectionOrigin[k] = r.F32();
        m.m_bSectionHeader = !r.Bad();
    }
    // A VisBSP's flat 44: that header (zeroed), then the ROOT NODE index and
    // nSections (0) -- the two fields v85 reads right after its points
    // (de_mainworld.cpp:612).
    m.m_nRootNode = -1;
    if (m.m_nLeafs != 0 && nTailHave == 44 && m.m_bSectionHeader)
    {
        int nRoot = (int)r.U32();
        unsigned int nSections = r.U32();
        if (!r.Bad() && nSections == 0 && nRoot >= 0 && nRoot < (int)m.m_Nodes.size())
            m.m_nRootNode = nRoot;
    }

    // ⚠️ A model whose tail is not the expected SHAPE counts as entirely
    // unaccounted, not as a small remainder. That keeps
    // GetNumBodiesFullyAccounted meaning what it always meant -- every byte of
    // the trailer explained -- so publishing this geometry cannot flatter the
    // one number that tracks how much of the format is genuinely understood.
    m.m_nUnparsedTail = (nTailHave >= nTailWant) ? (nTailHave - nTailWant)
                                                 : nTailHave;
    r.Skip(m.m_nBodyEnd - r.Tell());

    // ⚠️ BOTH GATES, AND BOTH ARE NECESSARY.
    //
    //   1. THE BOUNDARY CHECK -- land exactly on the model's declared end.
    //   2. THE MEANING CHECKS -- every point inside the model's own bounding
    //      box with a unit normal, every polygon vertex addressing a real
    //      point.
    //
    // ⚠️ Neither alone is sufficient, and this format proved it the hard way.
    // Byte accounting alone passed 78% of models whose geometry was nonsense,
    // because a body with PERMUTED sub-blocks still sums to the same total.
    // Meaning alone would accept a parse that quietly left bytes unread. Only
    // together do they say "the right things, in the right places".
    bool bOK = !r.Bad() && (r.Tell() == m.m_nBodyEnd);

    if (bOK)
    {
        for (size_t i = 0; i < m.m_Points.size() && bOK; ++i)
        {
            const float* nr = m.m_Points[i].m_Normal;
            // ⚠️ NaN first. `fabsf(NaN - 1) > 1e-2` is FALSE, so a NaN normal
            // sails through a plain tolerance test -- a check that passes on a
            // degenerate answer is not a check.
            if (!(nr[0] == nr[0] && nr[1] == nr[1] && nr[2] == nr[2])) { bOK = false; break; }
            float L = sqrtf(nr[0]*nr[0] + nr[1]*nr[1] + nr[2]*nr[2]);
            if (fabsf(L - 1.0f) > 1e-2f) { bOK = false; break; }
            for (int k = 0; k < 3; ++k)
            {
                float v = m.m_Points[i].m_Pos[k];
                if (v < m.m_Min[k] - 1.0f || v > m.m_Max[k] + 1.0f) { bOK = false; break; }
            }
        }
    }
    if (bOK)
    {
        for (size_t i = 0; i < m.m_Polys.size() && bOK; ++i)
            for (size_t v = 0; v < m.m_Polys[i].m_Verts.size(); ++v)
                if (m.m_Polys[i].m_Verts[v].m_nPoint >= m.m_Points.size()) { bOK = false; break; }
    }

    if (!bOK)
    {
        m.m_Planes.clear();
        m.m_Surfaces.clear();
        m.m_Polys.clear();
        m.m_Points.clear();
        m.m_Nodes.clear();
        m.m_nRootNode = -1;
        return;
    }
    m.m_bBodyParsed = true;
    if (m.m_nRootNode >= 0) m.m_Leaves.swap(leaves);
    else                    m.m_Nodes.clear();
    if (m.m_nUnparsedTail == 0) ++m_nBodiesFullyAccounted;
}


bool CLTWorld::ReadGeometryInfo(const unsigned char* p, unsigned int nSize,
                                unsigned int nStart)
{
    Reader r(p, nSize, nStart);

    m_Info.m_fUnknown = r.F32();
    for (int i = 0; i < 3; ++i) m_Info.m_ExtentsMin[i] = r.F32();
    for (int i = 0; i < 3; ++i) m_Info.m_ExtentsMax[i] = r.F32();
    for (int i = 0; i < 3; ++i) m_Info.m_TreeMin[i] = r.F32();
    for (int i = 0; i < 3; ++i) m_Info.m_TreeMax[i] = r.F32();
    m_Info.m_nTreeNodes = r.U32();
    m_Info.m_nTerrainDepth = r.U32();
    if (r.Bad()) return false;

    if (m_Info.m_nTreeNodes == 0) return false;

    unsigned int o = r.Tell();
    unsigned int nSubs = 0;
    if (!WalkTree(p, nSize, o, nSubs)) return false;

    // ⚠️ THE CHECK THAT VALIDATES THE WHOLE PREAMBLE. Each subdivision creates
    // exactly 4 children, so the walk must account for every node but the
    // root. Holds for all 102 shipped worlds; any earlier field being the
    // wrong size desynchronises the bit stream and breaks this immediately.
    if (nSubs * 4 != m_Info.m_nTreeNodes - 1) return false;

    m_Info.m_nSubdivisions = nSubs;
    m_Info.m_nGeometryEnd = o;

    Reader r2(p, nSize, o);
    m_Info.m_nWorldModels = r2.U32();
    if (r2.Bad() || m_Info.m_nWorldModels == 0 || m_Info.m_nWorldModels > 100000)
        return false;

    m_bGeometry = true;

    // ---- walk every world model by its link field --------------------- //
    //
    // ⚠️ The uint32 before each model is the absolute offset of the NEXT
    // model's link. The donor ignores it; it is what makes the models
    // iterable without parsing their bodies.
    unsigned int nAt = r2.Tell();
    m_Models.reserve(m_Info.m_nWorldModels);

    for (unsigned int i = 0; i < m_Info.m_nWorldModels; ++i)
    {
        Reader rm(p, nSize, nAt);
        unsigned int nNext = rm.U32();
        if (rm.Bad() || nNext <= nAt || nNext > nSize) return true;

        // 32 bytes, zero in every shipped world.
        unsigned int nZeroStart = rm.Tell();
        for (int k = 0; k < 8; ++k) (void)rm.U32();
        bool bZero = true;
        for (unsigned int k = 0; k < 32; ++k)
            if (p[nZeroStart + k]) { bZero = false; break; }
        if (!bZero) return true;

        LTWorldModelHeader m;
        m.m_nInfoFlags = rm.U32();       // 2 == WIF_MOVEABLE
        m.m_nUnknown   = rm.U32();
        m.m_sName      = rm.Str();
        if (rm.Bad() || m.m_sName.empty() || m.m_sName.size() > 64) return true;

        m.m_nPoints           = rm.U32();
        m.m_nPlanes           = rm.U32();
        m.m_nSurfaces         = rm.U32();
        m.m_nUserPortals      = rm.U32();
        m.m_nPolies           = rm.U32();
        m.m_nLeafs            = rm.U32();
        m.m_nVerts            = rm.U32();
        m.m_nTotalVisListSize = rm.U32();
        m.m_nLeafLists        = rm.U32();
        m.m_nNodes            = rm.U32();
        m.m_nExtra1           = rm.U32();
        m.m_nExtra2           = rm.U32();

        for (int k = 0; k < 3; ++k) m.m_Min[k] = rm.F32();
        for (int k = 0; k < 3; ++k) m.m_Max[k] = rm.F32();
        for (int k = 0; k < 3; ++k) m.m_Translation[k] = rm.F32();

        unsigned int nNamesLen = rm.U32();
        unsigned int nTextures = rm.U32();
        if (rm.Bad()) return true;

        if (nNamesLen > 0 && nTextures > 0 && nTextures <= 4096
            && nNamesLen <= (1u << 20) && rm.Tell() + nNamesLen <= nSize)
        {
            const unsigned char* pBlob = p + rm.Tell();

            // ⚠️ nTextures NUL-terminated names in exactly nNamesLen bytes.
            // That the two agree is what confirms this header layout.
            unsigned int nNuls = 0;
            for (unsigned int k = 0; k < nNamesLen; ++k) if (pBlob[k] == 0) ++nNuls;
            if (nNuls == nTextures && pBlob[nNamesLen - 1] == 0)
            {
                unsigned int nOff = 0;
                for (unsigned int k = 0; k < nTextures; ++k)
                {
                    m.m_Textures.push_back(std::string((const char*)(pBlob + nOff)));
                    nOff += (unsigned int)m.m_Textures.back().size() + 1;
                }
            }
            rm.Skip(nNamesLen);
        }

        m.m_bBodyParsed = false;
        m.m_bSectionHeader = false;
        m.m_nRootNode = -1;
        for (int k = 0; k < 3; ++k)
        {
            m.m_SectionGrid[k] = 0;
            m.m_SectionSize[k] = 0.0f;
            m.m_SectionOrigin[k] = 0.0f;
        }
        m.m_nBodyStart = rm.Tell();
        m.m_nBodyEnd   = nNext;
        if (m.m_nBodyStart > m.m_nBodyEnd) return true;

        ParseBody(p, m);
        if (m.m_bBodyParsed) ++m_nBodiesParsed; else ++m_nBodiesSkipped;

        m_Models.push_back(m);
        nAt = nNext;
    }

    // ⚠️ THE WHOLE-REGION CHECK. After the last world model the cursor must sit
    // exactly on objectDataPos -- the geometry region is then fully accounted
    // for, structurally, even though the BSP bodies are not yet parsed.
    m_bModelsComplete = (nAt == m_nObjectDataPos)
                     && (m_Models.size() == m_Info.m_nWorldModels);
    return true;
}


bool CLTWorld::Load(const void* pData, unsigned int nSize)
{
    Clear();
    if (!pData || nSize < kInfoLenOffset + 4)
        return Fail("not a world: too small to hold a header");

    const unsigned char* p = (const unsigned char*)pData;
    Reader r(p, nSize);

    m_nVersion = r.U32();
    if (m_nVersion != kWorldVersion)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "world version %u, expected %u",
                      m_nVersion, kWorldVersion);
        return Fail(buf);
    }

    m_nObjectDataPos      = r.U32();
    m_nBlindObjectDataPos = r.U32();

    // 8 words: packer type/version then six spares. Zero in every shipped
    // world; consumed because they are there, not interpreted.
    for (int i = 0; i < 8; ++i) (void)r.U32();

    m_sInfo = r.Str32();
    if (r.Bad()) return Fail("truncated world header");

    // ⚠️ Best-effort and NON-FATAL. The object list is what the game needs
    // first and it is fully understood; the geometry is only partly read, so a
    // world whose geometry this cannot follow must still load its objects.
    // Silence here would be wrong though -- HasGeometryInfo() reports it.
    ReadGeometryInfo(p, nSize, r.Tell());

    if (m_nObjectDataPos == 0 || m_nObjectDataPos >= nSize)
        return Fail("object data position is outside the file");
    if (m_nBlindObjectDataPos > nSize)
        return Fail("blind object data position is outside the file");

    // --- the object list --------------------------------------------- //
    Reader o(p, nSize, m_nObjectDataPos);
    unsigned int nObjects = o.U32();
    if (o.Bad()) return Fail("truncated object list");

    // Each object costs at least its length word plus an empty name and count.
    if (!o.CountFits(nObjects, 8))
        return Fail("object count cannot fit in the file");

    m_Objects.resize(nObjects);

    for (unsigned int i = 0; i < nObjects; ++i)
    {
        unsigned int nDeclared = o.U16();
        unsigned int nStart = o.Tell();
        if (o.Bad()) return Fail("truncated object record");

        if (nStart + nDeclared > nSize)
            return Fail("object record runs past the end of the file");

        LTWorldObject& obj = m_Objects[i];
        obj.m_sType = o.Str();

        unsigned int nProps = o.U32();
        if (o.Bad()) return Fail("truncated object header");
        if (!o.CountFits(nProps, 9))
            return Fail("property count cannot fit in the file");

        obj.m_Props.resize(nProps);
        for (unsigned int k = 0; k < nProps; ++k)
        {
            LTWorldProperty& pr = obj.m_Props[k];
            pr.m_sName  = o.Str();
            pr.m_nType  = o.U8();
            pr.m_nFlags = o.U32();
            unsigned int nLen = o.U16();
            if (o.Bad()) return Fail("truncated property header");

            unsigned int nAfter = o.Tell() + nLen;

            std::memset(pr.m_Vec, 0, sizeof(pr.m_Vec));
            std::memset(pr.m_Euler, 0, sizeof(pr.m_Euler));
            pr.m_fReal = 0.0f; pr.m_nInt = 0; pr.m_bBool = false;

            switch (pr.m_nType)
            {
                case kLTWP_String:
                    // ⚠️ The stored length INCLUDES the count word, so the
                    // string is read as counted text and the cursor is then
                    // placed by nLen -- the two agree on every shipped world,
                    // and seeking by nLen is what keeps them agreeing if one
                    // ever does not.
                    pr.m_sString = o.Str();
                    break;

                case kLTWP_Vector:
                case kLTWP_Color:
                    if (nLen >= 12) { pr.m_Vec[0] = o.F32(); pr.m_Vec[1] = o.F32(); pr.m_Vec[2] = o.F32(); }
                    break;

                case kLTWP_Rotation:
                    // ⚠️ EULER RADIANS + a padding float -- NOT a quaternion.
                    // See ltworld.h; this is the trap in this format.
                    if (nLen >= 12) { pr.m_Euler[0] = o.F32(); pr.m_Euler[1] = o.F32(); pr.m_Euler[2] = o.F32(); }
                    break;

                case kLTWP_Real:
                    if (nLen >= 4) pr.m_fReal = o.F32();
                    break;

                case kLTWP_Flags:
                case kLTWP_LongInt:
                    // ⚠️ Stored as a FLOAT and cast, because the level editor
                    // has no integer property type (donor s_object.cpp:485).
                    if (nLen >= 4) pr.m_nInt = (int)o.F32();
                    break;

                case kLTWP_Bool:
                    if (nLen >= 1) pr.m_bBool = (o.U8() != 0);
                    break;

                default:
                    break;
            }

            // Authoritative: the property's declared length places the cursor.
            o.Seek(nAfter);
            if (o.Bad()) return Fail("property runs past the end of the file");
        }

        // ⚠️ THE CHECK THAT MAKES THIS A READER RATHER THAN A GUESS. Every
        // object states its own byte length; a parse that lands anywhere else
        // means the model is wrong. Holds for 79,968 objects across all 102
        // shipped worlds.
        if (o.Tell() != nStart + nDeclared)
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "object %u ('%s') ended at %u but declares %u (off by %+d)",
                          i, obj.m_sType.c_str(), o.Tell() - nStart, nDeclared,
                          (int)(o.Tell() - nStart) - (int)nDeclared);
            return Fail(buf);
        }
    }

    m_bLoaded = true;
    return true;
}


bool CLTWorld::LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName)
{
    Clear();
    if (!pFileMgr || !pName || !pName[0])
        return Fail("no file manager or no name");

    ILTStream* pStream = pFileMgr->OpenFile(pName);
    if (!pStream)
    {
        std::string sTry(pName);
        sTry += ".dat";
        pStream = pFileMgr->OpenFile(sTry.c_str());
    }
    if (!pStream)
    {
        std::string sWhy = "no tree holds '";
        sWhy += pName; sWhy += "'";
        return Fail(sWhy.c_str());
    }

    unsigned long nLen = 0;
    pStream->GetLen(&nLen);

    std::vector<unsigned char> buf((size_t)nLen);
    bool bRead = (nLen > 0) && (pStream->Read(&buf[0], nLen) == LT_OK);
    pStream->Release();

    if (!bRead) return Fail("could not read the world out of its archive");
    return Load(&buf[0], (unsigned int)nLen);
}
