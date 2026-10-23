// ----------------------------------------------------------------------- //
//
// MODULE  : ltfilemgr.h
//
// PURPOSE : The engine's file system: a stack of file trees searched in order,
//           where a tree is either a `.rez` archive or a directory on disk.
//
//           This is what ILTCSBase::OpenFile is implemented against.
//
// PROVENANCE
//
//   Shape and semantics from the donor's two layers:
//     * de_file.cpp -- df_OpenTree / df_Open / df_GetFileInfo, and the
//       FileTree struct that is "either a CRezMgr or a base directory";
//     * client_filemgr.cpp -- CClientFileMgr::OpenFile, which walks the tree
//       list and returns the first tree that can produce a stream.
//
// ⚠️ SEARCH ORDER IS THE REVERSE OF ADD ORDER, AND THAT IS NOT ARBITRARY
//
//   The donor adds each tree with dl_Insert(&m_FileTrees, &pTree->m_Link)
//   (client_filemgr.cpp:287). dl_Insert splices the new link immediately after
//   the list head, so the list reads back LAST-ADDED FIRST -- and OpenFile
//   walks it from m_pNext. So the tree added last wins.
//
//   That is what makes the shipped ordering correct. NOLF 1's own startup.txt
//   lists trees oldest-first:
//
//       Rez0 = "nolf"          <- a loose directory
//       Rez1 = "nolf.rez"      <- base game, disc 1
//       Rez2 = "nolf2.rez"     <- base game, disc 2
//       ...
//       Rez5 = "nolfu003.rez"  <- the v1.003 patch
//       Rez8 = "nolfgoty.rez"  <- Game of the Year content
//
//   so reversing on lookup means the GOTY archive is consulted before the
//   patch, and the patch before the base game. Add them in file order and the
//   override chain comes out right by construction.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_FILEMGR_H__
#define __LT_FILEMGR_H__

#include <string>
#include <vector>

#include "iltstream.h"

class CLTRezArchive;

class CLTFileMgr
{
public:

    enum ETreeType { kRezTree, kDosTree };

    CLTFileMgr();
    ~CLTFileMgr();

    // Adds one tree. pPath may name a .rez archive or a directory; which it is
    // determines the tree type, exactly as df_OpenTree decides it from the
    // _A_SUBDIR attribute. Returns false if it does not exist or will not open.
    //
    // ⚠️ Later calls take PRIORITY over earlier ones -- see the header comment.
    bool            AddTree(const char* pPath);

    // Builds the tree stack the way the shipped game does: find startup.txt in
    // one of pDirs, read its [Rez] section, and add each named tree in file
    // order, resolving each name against every directory in pDirs.
    //
    // Each directory in pDirs is itself added as a tree FIRST, so it sits at
    // the BOTTOM of the search order. That is a port convenience -- these
    // trees are not in startup.txt -- and it is deliberately the lowest
    // priority, so it can only supply files no archive has.
    //
    // Returns the number of trees added. Reports every name it could not
    // resolve; a missing archive is never silent.
    int             InitFromGameDirs(const char* const* pDirs, int nDirs);

    void            Term();

    // The OpenFile contract: pFilename is relative and DOS-style
    // ("Attributes\\Weapons.txt"). Case and separator style do not matter.
    // Returns 0 if no tree has it.
    ILTStream*      OpenFile(const char* pFilename);

    bool            FileExists(const char* pFilename);

    // Copies a file out of whatever tree holds it to a real path on disk --
    // what ILTCSBase::CopyFile is for ("useful for copying files out of the rez
    // file to a temporary file"). Returns false on any failure.
    bool            CopyFileTo(const char* pSrcName, const char* pDestPath);

    // Diagnostics. The port's method note is that a silent stub loses weeks;
    // the same applies to a file system that silently finds nothing.
    int             GetNumTrees() const { return (int)m_Trees.size(); }
    const char*     GetTreePath(int i) const;
    ETreeType       GetTreeType(int i) const;
    unsigned int    GetTreeNumEntries(int i) const;

    // Counted for the same reason: "0 files opened" and "300 files opened"
    // look identical in a census that only records first calls.
    unsigned int    GetNumOpened() const  { return m_nOpened; }
    unsigned int    GetNumNotFound() const { return m_nNotFound; }

private:

    struct Tree
    {
        ETreeType       m_Type;
        std::string     m_Path;
        CLTRezArchive*  m_pRez;     // kRezTree only
    };

    // Opens pName within one tree, or returns 0.
    ILTStream*      OpenInTree(const Tree& tree, const char* pNormalisedName);

    std::vector<Tree>   m_Trees;
    unsigned int        m_nOpened;
    unsigned int        m_nNotFound;
};

// The engine has one. Declared here rather than passed around because
// ILTCSBase::OpenFile is a method on an interface the game already holds.
CLTFileMgr* LTGetFileMgr();

#endif // __LT_FILEMGR_H__
