// ----------------------------------------------------------------------- //
//
// MODULE  : stdlith_impl.cpp
//
// PURPOSE : Out-of-line definitions for NOLF 1's StdLith support library.
//
// WHY THIS EXISTS
//
//   LT2/lithshared ships headers only; the original build linked a prebuilt
//   StdLith.lib that is not part of the mod SDK. Everything here is written
//   against StdLith's own declarations -- the headers are not modified.
//
//   Only the entry points the game actually references are defined. The rest
//   of StdLith is inline in its headers and needs nothing from here.
//
// ----------------------------------------------------------------------- //

#include "windows.h"        // first, so _WINDOWS_ is set (see mfcs_impl.cpp)

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "stdlithdefs.h"
#include "l_allocator.h"
#include "struct_bank.h"
#include "dynarray.h"
#include "linklist.h"

// ----------------------------------------------------------------------- //
// Default allocator
// ----------------------------------------------------------------------- //

void* DefStdlithAlloc(DWORD size)
{
    return malloc((size_t)size);
}

void DefStdlithFree(void* ptr)
{
    free(ptr);
}

// The one shared allocator instance the containers default to.
LAlloc g_DefAlloc;

// ----------------------------------------------------------------------- //
// StructBank -- a pooling allocator.
//
// ⚠️ CONTRACT WITH THE HEADER. sb_Allocate/sb_Allocate_z/sb_Free are INLINE in
// struct_bank.h and this file must agree with them exactly:
//
//   * The free list is threaded THROUGH THE OBJECT STORAGE ITSELF: sb_Free
//     casts the object pointer straight to StructLink*. So every chunk must be
//     at least sizeof(StructLink) bytes and suitably aligned for a pointer.
//   * sb_Allocate pops m_FreeListHead and, when empty, calls
//     sb_AllocateNewStructPage(pBank, pBank->m_CacheSize) and retries once.
//     So that function MUST leave m_FreeListHead non-NULL on success.
//
// ⚠️ LP64: the header comment describes 4-byte alignment, which was right when
// a pointer was 4 bytes. Here a StructLink is 8 bytes and wants 8-byte
// alignment, so chunks are aligned to sizeof(void*). Aligning to 4 would give
// misaligned pointer writes in the free list.
// ----------------------------------------------------------------------- //

static unsigned long sb_AlignedSize(unsigned long structSize)
{
    const unsigned long align = (unsigned long)sizeof(void*);

    unsigned long size = structSize;
    if (size < (unsigned long)sizeof(StructLink))
        size = (unsigned long)sizeof(StructLink);

    return (size + (align - 1)) & ~(align - 1);
}

void sb_Init(StructBank* pBank, unsigned long structSize, unsigned long cacheSize)
{
    if (!pBank)
        return;

    if (cacheSize == 0)
        cacheSize = 16;             // a zero cache would never hand anything out

    pBank->m_StructSize        = structSize;
    pBank->m_AlignedStructSize = sb_AlignedSize(structSize);
    pBank->m_CacheSize         = cacheSize;
    pBank->m_nPages            = 0;
    pBank->m_nTotalObjects     = 0;
    pBank->m_PageHead          = NULL;
    pBank->m_FreeListHead      = NULL;
}

int sb_Init2(StructBank* pBank, unsigned long structSize, unsigned long cacheSize,
             unsigned long nPreallocations)
{
    sb_Init(pBank, structSize, cacheSize);
    if (nPreallocations == 0)
        return 1;
    return sb_AllocateNewStructPage(pBank, nPreallocations);
}

int sb_AllocateNewStructPage(StructBank* pBank, unsigned long nAllocations)
{
    if (!pBank || nAllocations == 0)
        return 0;

    if (pBank->m_AlignedStructSize == 0)
        pBank->m_AlignedStructSize = sb_AlignedSize(pBank->m_StructSize);

    // Header, then nAllocations chunks. offsetof() rather than sizeof() because
    // StructBankPage declares m_Data[1] as a flexible-array stand-in.
    size_t headerBytes = offsetof(StructBankPage, m_Data);
    size_t dataBytes   = (size_t)nAllocations * (size_t)pBank->m_AlignedStructSize;

    StructBankPage* pPage = (StructBankPage*)malloc(headerBytes + dataBytes);
    if (!pPage)
        return 0;

    pPage->m_nObjects = nAllocations;
    pPage->m_pNext    = pBank->m_PageHead;
    pBank->m_PageHead = pPage;
    ++pBank->m_nPages;
    pBank->m_nTotalObjects += nAllocations;

    // Thread every chunk onto the free list.
    char* pChunk = ((char*)pPage) + headerBytes;
    for (unsigned long i = 0; i < nAllocations; ++i)
    {
        StructLink* pLink = (StructLink*)pChunk;
        pLink->m_pSLNext      = pBank->m_FreeListHead;
        pBank->m_FreeListHead = pLink;
        pChunk += pBank->m_AlignedStructSize;
    }

    return 1;
}

void sb_FreeAll(StructBank* pBank)
{
    if (!pBank)
        return;

    // Rebuild the free list across every page; no memory is released.
    pBank->m_FreeListHead = NULL;

    size_t headerBytes = offsetof(StructBankPage, m_Data);
    for (StructBankPage* pPage = pBank->m_PageHead; pPage; pPage = pPage->m_pNext)
    {
        char* pChunk = ((char*)pPage) + headerBytes;
        for (unsigned long i = 0; i < pPage->m_nObjects; ++i)
        {
            StructLink* pLink = (StructLink*)pChunk;
            pLink->m_pSLNext      = pBank->m_FreeListHead;
            pBank->m_FreeListHead = pLink;
            pChunk += pBank->m_AlignedStructSize;
        }
    }
}

void sb_Term2(StructBank* pBank, int bCheckObjects)
{
    if (!pBank)
        return;

    (void)bCheckObjects;    // leak-checking hook; nothing to report yet

    StructBankPage* pPage = pBank->m_PageHead;
    while (pPage)
    {
        StructBankPage* pNext = pPage->m_pNext;
        free(pPage);
        pPage = pNext;
    }

    pBank->m_PageHead      = NULL;
    pBank->m_FreeListHead  = NULL;
    pBank->m_nPages        = 0;
    pBank->m_nTotalObjects = 0;
}

void sb_Term(StructBank* pBank)
{
    sb_Term2(pBank, 0);
}

// ----------------------------------------------------------------------- //
// Type-erased container searches.
//
// These exist so the templates do not emit a copy of the search loop for every
// element type -- see the comments in dynarray.h / linklist.h.
// ----------------------------------------------------------------------- //

DWORD MoArray_FindElementMemcmp(const void* pToFind, const void* pArray,
                                DWORD nElements, DWORD elementSize)
{
    if (!pToFind || !pArray || elementSize == 0)
        return BAD_INDEX;

    const char* pCur = (const char*)pArray;
    for (DWORD i = 0; i < nElements; ++i)
    {
        if (memcmp(pCur, pToFind, (size_t)elementSize) == 0)
            return i;
        pCur += elementSize;
    }
    return BAD_INDEX;   // GenFindElement tests against exactly this
}

// Returns the matching node (usable as an LPOS), or NULL. The caller tests the
// result for truth, so NULL is the only "not found" signal available.
//
// ⚠️ This relies on the layout contract linklist.h states explicitly: CLLNode<T>
// and CGenLLNode must agree on m_pPrev/m_pNext, so the element data begins at
// offsetof(CGenLLNode, m_Data) in both. Deriving the offset rather than
// hard-coding two pointer widths keeps that true under LP64.
LPOS LinkList_FindElementMemcmp(const void* pHead, const void* pToFind,
                                DWORD elementSize)
{
    if (!pHead || !pToFind || elementSize == 0)
        return NULL;

    const size_t dataOffset = offsetof(CGenLLNode, m_Data);

    const CGenLLNode* pCur = (const CGenLLNode*)pHead;
    while (pCur)
    {
        const char* pData = ((const char*)pCur) + dataOffset;
        if (memcmp(pData, pToFind, (size_t)elementSize) == 0)
            return (LPOS)pCur;
        pCur = pCur->m_pNext;
    }
    return NULL;
}
