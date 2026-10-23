// ----------------------------------------------------------------------- //
//
// MODULE  : ztools_impl.cpp
//
// PURPOSE : Out-of-line definitions for the ztools support classes that
//           NOLF 1's ButeMgr is built on: zErrHandler, zErrHandling, zPtrColl
//           and zPTree (via zSymTab).
//
// WHY THIS EXISTS
//
//   LT2/lithshared ships headers only. ButeMgr declares its storage as direct
//   members --
//
//       typedef zSymTab<CSymTabItem>  TableOfItems;
//       typedef zSymTab<TableOfItems> TableOfTags;
//       TableOfTags m_tagTab;
//
//   -- so this layer has to work before a single ButeMgr accessor can.
//
// ⚠️ A DELIBERATE IMPLEMENTATION CHOICE, RECORDED
//
//   zPTree is declared as a Patricia trie: it carries `sbits`, a `preview`
//   index, per-node `index` bit positions, and private helpers bit()/diffpos().
//   This implementation does NOT reproduce the trie. It stores the same
//   key -> pointer mapping in an ordered binary search tree built from the
//   declared zPTreeNode fields.
//
//   That is safe because the trie is entirely private:
//     * zPTreeNode, zPTree, draw() and dump() are referenced by ZERO files in
//       the game code and ZERO in butemgr -- measured, not assumed.
//     * Everything reaches the table through zSymTab<T>'s insert / lookup /
//       add / traverse, which only require correct associative behaviour.
//     * The node layout is unchanged, so the header's inline clear() and
//       ~zPTree() (which call cleanup() and reset root/preview) still work.
//
//   The trade is that draw()/dump() can no longer print a trie shape. They are
//   debug-only, uncalled, and say so when used rather than lying.
//
// ----------------------------------------------------------------------- //

#include "windows.h"        // first, so _WINDOWS_ is set (see mfcs_impl.cpp)

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "ztools.h"

// ----------------------------------------------------------------------- //
// zErrHandler
//
// The error machinery is never exercised by NOLF 1 -- no game code calls
// handle(), geterr() or the mode setters. It must therefore be correct and
// SILENT, not elaborate. handle() records the code so geterr() can report it.
// ----------------------------------------------------------------------- //

_dhandler zErrHandler::dl[MAX_DEDICATED];
int       zErrHandler::ndh = 0;

zErrHandler::zErrHandler(const char* pId)
{
    default_ef = 0;
    slot       = -1;
    evalue     = 0;
    mode       = logging;
    prevmode   = logging;
    id         = pId;
}

int zErrHandler::srch(void* pObj)
{
    for (int i = 0; i < ndh; ++i)
    {
        if (dl[i].object == pObj)
            return i;
    }
    return -1;
}

void zErrHandler::handle(void* pObj, const char* s, int e)
{
    evalue = (short)e;

    // Only the "call a function" mode has an observable effect here. Logging
    // deliberately stays quiet: ButeMgr reports its own parse errors through
    // GetErrorString(), and duplicating them on stderr would be noise.
    if (mode == fcall)
    {
        int i = srch(pObj);
        erf_t f = (i >= 0 && dl[i].handler) ? dl[i].handler : default_ef;
        if (f)
            f(s, e);
    }
}

int zErrHandler::geterr(void* pObj, int n)
{
    (void)pObj;
    int ret = evalue;
    evalue  = (short)n;     // reading an error clears it (n defaults to 0)
    return ret;
}

erf_t zErrHandler::set_ef(void* pObj, erf_t f)
{
    int i = srch(pObj);
    if (i < 0)
    {
        if (ndh >= MAX_DEDICATED)
            return 0;
        i = ndh++;
        dl[i].object  = pObj;
        dl[i].handler = 0;
        dl[i].lasterr = 0;
    }
    erf_t prev = dl[i].handler;
    dl[i].handler = f;
    return prev;
}

erf_t zErrHandler::set_default_ef(erf_t f)
{
    erf_t prev = default_ef;
    default_ef = f;
    return prev;
}

// ----------------------------------------------------------------------- //
// zErrHandling
// ----------------------------------------------------------------------- //

// The shared fallback handler, used when a subclass passes none.
static zErrHandler g_ltDefaultErrHandler("lt-default");

char* zErrHandling::_nomem     = (char*)"out of memory";
char* zErrHandling::_inval     = (char*)"invalid";
char* zErrHandling::_overflow  = (char*)"overflow";
char* zErrHandling::_nosuch    = (char*)"no such element";
char* zErrHandling::_range     = (char*)"out of range";
char* zErrHandling::_exists    = (char*)"already exists";
char* zErrHandling::_nullparg  = (char*)"null pointer argument";
char* zErrHandling::_badarg    = (char*)"bad argument";

zErrHandling::zErrHandling(zErrHandler* p)
{
    hp = p ? p : &g_ltDefaultErrHandler;
}

zErrHandling::~zErrHandling()
{
}

// ----------------------------------------------------------------------- //
// zPtrColl
// ----------------------------------------------------------------------- //

zPtrColl::zPtrColl(cleanup_behaviour cb, dtorf_t d)
{
    dtor   = d;
    flags  = (int)cb;
    _count = 0;
}

zPtrColl::~zPtrColl()
{
}

// NB: zPtrColl::same() is already defined INLINE in ztools.h -- do not define
// it here as well.

// ----------------------------------------------------------------------- //
// zPTree -- ordered BST over the declared zPTreeNode fields.
//
//   left / right : subtree links
//   symbol       : owned copy of the key
//   body         : the stored pointer
//   index        : unused by this implementation (kept zero)
// ----------------------------------------------------------------------- //

zErrHandler zPTree::ceh("zPTree");

zPTree::zPTree(dtorf_t d, cleanup_behaviour cb)
    : zErrHandling(&ceh), zPtrColl(cb, d)
{
    root    = 0;
    p       = 0;
    q       = 0;
    sbits   = 0;
    preview = 0;
}

// ⚠️ zPTreeNode's fields are private with `friend class zPTree`, so only
// zPTree's own members may touch them. This helper therefore returns RAW
// storage and the calling member does the initialisation.
static zPTreeNode* ztree_AllocNode()
{
    return (zPTreeNode*)malloc(sizeof(zPTreeNode));
}

static char* ztree_DupKey(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// Recursive teardown. Honours zPtrColl's cleanup_behaviour: `none` suppresses
// destruction of the stored objects, `active` runs the registered destructor
// function on each. The nodes themselves always go.
void zPTree::cleanup(zPTreeNode* pNode)
{
    if (!pNode)
    {
        pNode = root;
        if (!pNode)
            return;
        root = 0;
    }

    cleanup(pNode->left);
    cleanup(pNode->right);

    if (pNode->body && !leave() && purge())
        destroy(pNode->body);

    free(pNode->symbol);
    free(pNode);
}

// Returns the stored body for `s`, or 0.
void* zPTree::lookup(const char* s)
{
    if (!s)
        return 0;

    zPTreeNode* pNode = root;
    while (pNode)
    {
        int c = strcmp(s, pNode->symbol);
        if (c == 0)
            return pNode->body;
        pNode = (c < 0) ? pNode->left : pNode->right;
    }
    return 0;
}

// Insert if absent. If the key already exists the EXISTING body is returned
// and the tree is left alone -- callers distinguish "inserted" from "already
// present" by comparing the result against what they passed in.
void* zPTree::insert(const char* s, void* body)
{
    if (!s)
        return 0;

    if (!root)
    {
        root = ztree_AllocNode();
        if (!root)
            return 0;
        root->symbol = ztree_DupKey(s);
        root->left = root->right = 0;
        root->index = 0;
        root->body  = body;
        incc();
        return body;
    }

    zPTreeNode* pNode = root;
    for (;;)
    {
        int c = strcmp(s, pNode->symbol);
        if (c == 0)
            return pNode->body;             // already present

        zPTreeNode*& slot = pNode->ptr(c > 0 ? 1 : 0);
        if (!slot)
        {
            slot = ztree_AllocNode();
            if (!slot)
                return 0;
            slot->symbol = ztree_DupKey(s);
            slot->left = slot->right = 0;
            slot->index = 0;
            slot->body  = body;
            incc();
            return body;
        }
        pNode = slot;
    }
}

// Insert or REPLACE. The previous body (if any) is destroyed under the same
// cleanup_behaviour rules as cleanup().
void* zPTree::add(const char* s, void* body)
{
    if (!s)
        return 0;

    zPTreeNode* pNode = root;
    while (pNode)
    {
        int c = strcmp(s, pNode->symbol);
        if (c == 0)
        {
            if (pNode->body && pNode->body != body && !leave() && purge())
                destroy(pNode->body);
            pNode->body = body;
            return body;
        }
        pNode = (c < 0) ? pNode->left : pNode->right;
    }
    return insert(s, body);
}

// In-order traversal, so callers see keys in a stable sorted order rather than
// whatever order they were inserted.
void zPTree::_trav(stvf_t vf, void* supplementary, zPTreeNode* pNode)
{
    if (!vf)
        return;

    if (!pNode)
    {
        pNode = root;
        if (!pNode)
            return;
    }

    if (pNode->left)
        _trav(vf, supplementary, pNode->left);

    vf(pNode->symbol, pNode->body, supplementary);

    if (pNode->right)
        _trav(vf, supplementary, pNode->right);
}

int zPTree::diffpos(const char* a, const char* b)
{
    // Retained for interface completeness: the first differing BIT position,
    // which is what a Patricia trie keys on. This implementation does not use
    // it, but the signature is part of the declared class.
    if (!a || !b)
        return 0;

    int i = 0;
    while (a[i] && a[i] == b[i])
        ++i;

    unsigned char x = (unsigned char)(a[i] ^ b[i]);
    int bitpos = i << 3;
    for (int k = 0; k < 8; ++k)
    {
        if (x & (1 << k))
            return bitpos + k;
    }
    return bitpos;
}

// Debug-only structure printers. This implementation is a BST rather than the
// original trie, so these do not print a trie shape -- and they SAY SO instead
// of printing something misleading. Neither is called by NOLF 1.
void zPTree::draw(zPTreeNode* pNode, int a, int b, int c) const
{
    (void)pNode; (void)a; (void)b; (void)c;
    fprintf(stderr,
            "[ztools] zPTree::draw() -- this build stores the symbol table as a "
            "BST, not a Patricia trie; there is no trie shape to draw.\n");
}

void zPTree::dump(zPTreeNode* pNode)
{
    if (!pNode)
    {
        fprintf(stderr, "[ztools] zPTree::dump: %lu entries\n",
                (unsigned long)count());
        pNode = root;
        if (!pNode)
            return;
    }

    if (pNode->left)
        dump(pNode->left);
    fprintf(stderr, "  %s\n", pNode->symbol ? pNode->symbol : "(null)");
    if (pNode->right)
        dump(pNode->right);
}
