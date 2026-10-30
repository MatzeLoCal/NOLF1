// ----------------------------------------------------------------------- //
//
// ltworldvis.h -- THE LEVEL'S VISIBILITY (PVS), read the way the shipped
// engine reads it.
//
// Every NOLF level has one world model with leaves, `VisBSP`, and each leaf
// carries a list saying what can be seen from it. The server uses it for one
// thing that matters to the game: objects the player cannot see are put to
// sleep (LT2 auto-deactivation), and a sleeping AI hears nothing. Without it
// every AI in a level stayed awake and heard every shot -- the whole hotel
// stormed out at a pistol shot from the gate (HANDOVER 5cp).
//
// ⚠️ A LIST IS NOT A LEAF BITMAP. It is a BIT STREAM THAT STEERS A WALK of
// the VisBSP's node tree (lithtech.exe 0x48b8e0), depth-first from the root:
//
//     at a node that owns a leaf     1 bit -- that leaf is visible
//     then for sides[0], if a node   1 bit -- descend (its whole subtree first)
//     then for sides[1], if a node   1 bit -- descend
//
// Objects are pushed down the same walk, split by each node's plane, and are
// seen when they reach a node whose leaf bit was set. Read as a bitmap the
// data looks like nonsense (lists of up to 1,804 bits for 616 leaves); walked,
// all 616 of M01S01's lists end inside their last byte, every leaf sees
// itself, and 99.2% of leaf pairs see each other both ways.
//
// ⚠️ A leaf's FIRST list is always used. Further lists belong to a named
// portal (a door) and count only while it is open -- ILTServer::SetPortalFlags,
// which Door.cpp calls as it opens and closes.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WORLD_VIS_H__
#define __LT_WORLD_VIS_H__

#include <string>
#include <vector>

class CLTWorld;

class CLTWorldVis
{
public:

    CLTWorldVis();

    // From the world's VisBSP. False, and IsValid() false, when it has none
    // (or it did not parse) -- callers then treat everything as visible.
    bool                Build(const CLTWorld& world);
    void                Clear();
    bool                IsValid() const { return !m_Nodes.empty(); }

    unsigned int        NumLeaves() const { return (unsigned int)m_Leaves.size(); }
    unsigned int        NumNodes() const { return (unsigned int)m_Nodes.size(); }
    unsigned int        NumPortals() const { return (unsigned int)m_Portals.size(); }

    // The leaf a point is in, or -1 outside the level.
    int                 LeafAt(const float* pPos3) const;

    // Fixes the viewpoint for BoxVisible. -1 (outside every leaf) and a leaf
    // with no list both mean EVERYTHING is visible -- the shipped engine's
    // own fallback. Decoded once per leaf and portal state, then cached.
    void                SetViewLeaf(int nLeaf);

    // Would the walk from the view leaf reach this box?
    bool                BoxVisible(const float* pMin3, const float* pMax3) const;

    // The leaves the current view sees (for traces and tests).
    unsigned int        VisibleLeafCount() const;

    // ⚠️ A VIEW OF ITS OWN (HANDOVER 5ea) -- the renderer's camera, which is
    // not where the server's player is (a cinematic camera is often far out
    // in the void). Same walk and same fallback as SetViewLeaf/BoxVisible,
    // into the caller's state, so neither re-decodes the other's leaf.
    struct View
    {
        int                         m_nLeaf = -2;
        unsigned int                m_nGen = 0;
        bool                        m_bAll = true;
        std::vector<unsigned char>  m_State;
    };
    // Recomputed only when the leaf or a portal changed; true when it was.
    bool                ComputeView(int nLeaf, View& v) const;
    bool                BoxVisibleIn(const View& v, const float* pMin3, const float* pMax3) const;
    unsigned int        LeafCountIn(const View& v) const;

    // The decoder's own check, for tests: walks every leaf's lists and counts
    // the ones that ran past their data or left a whole byte unread.
    void                SelfCheck(unsigned int* pnLists, unsigned int* pnBad,
                                  unsigned int* pnSelfVisible) const;

    // ILTServer::Get/SetPortalFlags. False for a name this level does not have.
    bool                GetPortalFlags(const char* pName, unsigned int* pFlags) const;
    bool                SetPortalFlags(const char* pName, unsigned int nFlags);
    // By index, in the level's order -- for saving and restoring them.
    unsigned int        PortalFlagsAt(unsigned int i) const
                        { return i < m_Portals.size() ? m_Portals[i].m_nFlags : 0; }
    void                SetPortalFlagsAt(unsigned int i, unsigned int nFlags)
                        { if (i < m_Portals.size() && m_Portals[i].m_nFlags != nFlags)
                          { m_Portals[i].m_nFlags = nFlags; ++m_nPortalGen; } }

private:

    struct Node
    {
        float           m_Normal[3];
        float           m_Dist;
        int             m_Sides[2];     // [0] back, [1] front; -1 IN, -2 OUT
        int             m_nLeaf;        // -1 when the node owns no leaf
    };
    struct List
    {
        int                         m_nPortal;  // -1 for the leaf's base list
        std::vector<unsigned char>  m_Bits;
    };
    struct Portal
    {
        std::string     m_sName;
        unsigned int    m_nFlags;
    };

    enum { kVisited = 1, kLeafSeen = 2 };

    void                Walk(const std::vector<unsigned char>& bits, std::vector<unsigned char>& state,
                             bool* pRanPast, size_t* pBitsUsed) const;

    std::vector<Node>                   m_Nodes;
    int                                 m_nRoot;
    std::vector<std::vector<List> >     m_Leaves;   // the lists, shares resolved
    std::vector<Portal>                 m_Portals;
    unsigned int                        m_nPortalGen;

    // the current view
    int                                 m_nViewLeaf;
    unsigned int                        m_nViewGen;
    bool                                m_bAll;
    std::vector<unsigned char>          m_State;    // per node: kVisited | kLeafSeen
};

// The one the server uses, built per level.
CLTWorldVis*    LTGetWorldVis();
void            LTWorldVis_BuildFor(const CLTWorld* pWorld);

#endif // __LT_WORLD_VIS_H__
