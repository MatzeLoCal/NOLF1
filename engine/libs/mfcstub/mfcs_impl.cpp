// ----------------------------------------------------------------------- //
//
// MODULE  : mfcs_impl.cpp
//
// PURPOSE : Implementation of NOLF 1's MFC-stub classes -- CString, CPoint,
//           CRect -- declared in LT2/lithshared/mfcstub/.
//
// WHY THIS EXISTS
//
//   LT2/lithshared ships HEADERS ONLY. The original build linked a prebuilt
//   MFCStub.lib that is not part of the mod SDK, so these definitions have to
//   be supplied. Written against the SDK's own declarations; the headers are
//   not modified.
//
// ⚠️ LAYOUT CONTRACT
//
//   CString's header does its own pointer arithmetic inline:
//
//       GetData() { return ((CStringData *)m_pBuffer) - 1; }
//
//   so a CStringData block MUST sit immediately before the character data, and
//   m_pBuffer MUST point at the first character, not at the allocation. Every
//   allocation here is sizeof(CStringData) + bufferBytes for that reason. Get
//   this wrong and GetLength()/GetBufferSize() read whatever precedes the heap
//   block -- which would look like it works for short strings and corrupt
//   silently later.
//
// ----------------------------------------------------------------------- //

// ⚠️ windows.h MUST come first, exactly as the game's own Stdafx.h does. It
// defines _WINDOWS_, which makes mfcstub defer to windows.h's POINT and RECT
// instead of declaring its own. Including mfcstub.h alone would compile this
// file against a DIFFERENT POINT/RECT than every game translation unit sees --
// an ODR mismatch that links cleanly and misbehaves at runtime.
#include "windows.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>

#include "mfcstub.h"

// ----------------------------------------------------------------------- //
// CString
// ----------------------------------------------------------------------- //

// Allocate a buffer of `chars` bytes (INCLUDING the null terminator) with its
// CStringData header, returning a pointer to the character data.
static char* ltmfc_AllocBuffer(DWORD chars)
{
    if (chars == 0)
        return NULL;

    void* raw = malloc(sizeof(CStringData) + (size_t)chars);
    if (!raw)
        return NULL;

    CStringData* pData = (CStringData*)raw;
    pData->m_Length     = 0;
    pData->m_BufferSize = chars;

    char* pChars = (char*)(pData + 1);
    pChars[0] = '\0';
    return pChars;
}

static void ltmfc_FreeBuffer(char* pChars)
{
    if (!pChars)
        return;
    free(((CStringData*)pChars) - 1);
}

CString::CString()
    : m_pBuffer(NULL)
{
}

CString::CString(LPCTSTR pString)
    : m_pBuffer(NULL)
{
    CopyString(pString);
}

CString::CString(const CString& cString)
    : m_pBuffer(NULL)
{
    CopyString(cString.m_pBuffer);
}

CString::~CString()
{
    ltmfc_FreeBuffer(m_pBuffer);
    m_pBuffer = NULL;
}

// Ensure the buffer holds at least minLength bytes (including the terminator).
BOOL CString::ExpandBuffer(DWORD minLength)
{
    if (minLength == 0)
        return TRUE;

    if (m_pBuffer && GetBufferSize() >= minLength)
        return TRUE;

    // Grow generously so repeated Concat() is not quadratic.
    DWORD newSize = (minLength < 16) ? 16 : minLength;
    if (m_pBuffer && newSize < GetBufferSize() * 2)
        newSize = GetBufferSize() * 2;

    char* pNew = ltmfc_AllocBuffer(newSize);
    if (!pNew)
        return FALSE;

    DWORD oldLen = GetLength();
    if (m_pBuffer && oldLen)
    {
        memcpy(pNew, m_pBuffer, (size_t)oldLen);
        ((CStringData*)pNew)[-1].m_Length = oldLen;
    }
    pNew[oldLen] = '\0';

    ltmfc_FreeBuffer(m_pBuffer);
    m_pBuffer = pNew;
    return TRUE;
}

// Ensure the buffer is no larger than maxLength bytes (including terminator).
// maxLength == 0 releases the buffer entirely, which is what Empty() wants.
BOOL CString::ShrinkBuffer(DWORD maxLength)
{
    if (maxLength == 0)
    {
        ltmfc_FreeBuffer(m_pBuffer);
        m_pBuffer = NULL;
        return TRUE;
    }

    if (!m_pBuffer || GetBufferSize() <= maxLength)
        return TRUE;

    DWORD len = GetLength();
    if (len > maxLength - 1)
        len = maxLength - 1;

    char* pNew = ltmfc_AllocBuffer(maxLength);
    if (!pNew)
        return FALSE;

    if (len)
        memcpy(pNew, m_pBuffer, (size_t)len);
    pNew[len] = '\0';
    ((CStringData*)pNew)[-1].m_Length = len;

    ltmfc_FreeBuffer(m_pBuffer);
    m_pBuffer = pNew;
    return TRUE;
}

void CString::CopyString(LPCTSTR pString)
{
    if (!pString)
    {
        ShrinkBuffer(0);
        return;
    }

    // Self-assignment, including assigning from our own buffer.
    if (pString == m_pBuffer)
        return;

    DWORD len = (DWORD)strlen(pString);
    if (!ExpandBuffer(len + 1))
        return;

    memcpy(m_pBuffer, pString, (size_t)len + 1);
    SetLength(len);
}

LPSTR CString::GetBuffer(DWORD minLength)
{
    if (!ExpandBuffer(minLength + 1))
        return NULL;
    return m_pBuffer;
}

// Re-syncs the cached length after a caller has written through GetBuffer().
// length < 0 means "measure it".
void CString::ReleaseBuffer(SDWORD length)
{
    if (!m_pBuffer)
        return;

    DWORD len = (length < 0) ? (DWORD)strlen(m_pBuffer) : (DWORD)length;
    if (len > GetBufferSize() - 1)
        len = GetBufferSize() - 1;

    m_pBuffer[len] = '\0';
    SetLength(len);
}

void CString::Concat(LPCTSTR pString)
{
    if (!pString || !*pString)
        return;

    DWORD addLen = (DWORD)strlen(pString);
    DWORD oldLen = GetLength();

    // pString may point into our own buffer, so copy it before reallocating.
    char* pTmp = (char*)malloc((size_t)addLen + 1);
    if (!pTmp)
        return;
    memcpy(pTmp, pString, (size_t)addLen + 1);

    if (ExpandBuffer(oldLen + addLen + 1))
    {
        memcpy(m_pBuffer + oldLen, pTmp, (size_t)addLen + 1);
        SetLength(oldLen + addLen);
    }
    free(pTmp);
}

void CString::Concat(char ch)
{
    char tmp[2] = { ch, '\0' };
    Concat(tmp);
}

void CString::FormatV(LPCTSTR pFormat, va_list args)
{
    if (!pFormat)
        return;

    // Measure first, so arbitrarily long results are not truncated.
    va_list probe;
    va_copy(probe, args);
    int needed = vsnprintf(NULL, 0, pFormat, probe);
    va_end(probe);

    if (needed < 0)
        return;

    if (!ExpandBuffer((DWORD)needed + 1))
        return;

    vsnprintf(m_pBuffer, (size_t)needed + 1, pFormat, args);
    SetLength((DWORD)needed);
}

void CString::Format(LPCTSTR pFormat, ...)
{
    va_list args;
    va_start(args, pFormat);
    FormatV(pFormat, args);
    va_end(args);
}

int CString::Find(char ch, DWORD start) const
{
    if (!m_pBuffer || start >= GetLength())
        return -1;
    const char* p = strchr(m_pBuffer + start, ch);
    return p ? (int)(p - m_pBuffer) : -1;
}

int CString::Find(LPCTSTR pSub, DWORD start) const
{
    if (!m_pBuffer || !pSub || start > GetLength())
        return -1;
    const char* p = strstr(m_pBuffer + start, pSub);
    return p ? (int)(p - m_pBuffer) : -1;
}

int CString::Replace(LPCTSTR pOld, LPCTSTR pNew)
{
    if (!m_pBuffer || !pOld || !*pOld)
        return 0;

    const char* pRepl = pNew ? pNew : "";
    DWORD oldLen = (DWORD)strlen(pOld);
    DWORD newLen = (DWORD)strlen(pRepl);

    // Build the result separately -- editing in place would invalidate the
    // search position whenever the replacement changes length.
    CString result;
    int count = 0;
    DWORD pos = 0;

    for (;;)
    {
        int hit = Find(pOld, pos);
        if (hit < 0)
            break;

        for (DWORD i = pos; i < (DWORD)hit; ++i)
            result.Concat(m_pBuffer[i]);
        result.Concat(pRepl);

        pos = (DWORD)hit + oldLen;
        ++count;
    }

    if (count == 0)
        return 0;

    for (DWORD i = pos; i < GetLength(); ++i)
        result.Concat(m_pBuffer[i]);

    (void)newLen;
    CopyString(result.m_pBuffer ? result.m_pBuffer : "");
    return count;
}

int CString::Compare(LPCTSTR lpsz) const
{
    return strcmp(m_pBuffer ? m_pBuffer : "", lpsz ? lpsz : "");
}

int CString::CompareNoCase(LPCTSTR lpsz) const
{
    return strcasecmp(m_pBuffer ? m_pBuffer : "", lpsz ? lpsz : "");
}

CString CString::Mid(DWORD nFirst, DWORD nCount) const
{
    CString out;
    if (!m_pBuffer || nFirst >= GetLength())
        return out;

    DWORD avail = GetLength() - nFirst;
    if (nCount > avail)
        nCount = avail;

    if (out.ExpandBuffer(nCount + 1))
    {
        memcpy(out.m_pBuffer, m_pBuffer + nFirst, (size_t)nCount);
        out.m_pBuffer[nCount] = '\0';
        out.SetLength(nCount);
    }
    return out;
}

CString CString::Mid(DWORD nFirst) const
{
    if (!m_pBuffer || nFirst >= GetLength())
        return CString();
    return Mid(nFirst, GetLength() - nFirst);
}

CString CString::Left(DWORD nCount) const
{
    return Mid(0, nCount);
}

CString CString::Right(DWORD nCount) const
{
    if (!m_pBuffer)
        return CString();
    DWORD len = GetLength();
    if (nCount >= len)
        return Mid(0, len);
    return Mid(len - nCount, nCount);
}

// ----------------------------------------------------------------------- //
// CPoint
// ----------------------------------------------------------------------- //

CPoint::CPoint()                        { x = 0; y = 0; }
CPoint::CPoint(const CPoint& cPt)       { x = cPt.x; y = cPt.y; }
CPoint::CPoint(const POINT* pPt)        { if (pPt) { x = pPt->x; y = pPt->y; } else { x = 0; y = 0; } }
CPoint::CPoint(SDWORD newx, SDWORD newy){ x = newx; y = newy; }

// ----------------------------------------------------------------------- //
// CRect
//
// ⚠️ TopLeft()/BottomRight() reinterpret the RECT's two coordinate pairs as
// CPoints. That is the classic MFC trick and it is only valid because RECT is
// {left,top,right,bottom} and POINT is {x,y} with identical member types --
// which compat/windows.h guarantees (both LONG). If either layout changes,
// these break silently.
// ----------------------------------------------------------------------- //

CRect::CRect()                                      { left = top = right = bottom = 0; }
CRect::CRect(const RECT& cRect)                     { *(RECT*)this = cRect; }
CRect::CRect(const RECT* pRect)                     { if (pRect) *(RECT*)this = *pRect; else left = top = right = bottom = 0; }
CRect::CRect(SDWORD l, SDWORD t, SDWORD r, SDWORD b){ left = l; top = t; right = r; bottom = b; }
CRect::CRect(POINT topLeft, POINT bottomRight)      { left = topLeft.x; top = topLeft.y; right = bottomRight.x; bottom = bottomRight.y; }

void CRect::SetRect(SDWORD l, SDWORD t, SDWORD r, SDWORD b)
{
    left = l; top = t; right = r; bottom = b;
}

BOOL CRect::IntersectRect(const RECT* lpRect1, const RECT* lpRect2)
{
    if (!lpRect1 || !lpRect2)
    {
        left = top = right = bottom = 0;
        return FALSE;
    }

    LONG l = (lpRect1->left   > lpRect2->left)   ? lpRect1->left   : lpRect2->left;
    LONG t = (lpRect1->top    > lpRect2->top)    ? lpRect1->top    : lpRect2->top;
    LONG r = (lpRect1->right  < lpRect2->right)  ? lpRect1->right  : lpRect2->right;
    LONG b = (lpRect1->bottom < lpRect2->bottom) ? lpRect1->bottom : lpRect2->bottom;

    // Win32 zeroes the rect and reports FALSE when there is no overlap.
    if (l >= r || t >= b)
    {
        left = top = right = bottom = 0;
        return FALSE;
    }

    left = l; top = t; right = r; bottom = b;
    return TRUE;
}

CPoint&       CRect::TopLeft()           { return *((CPoint*)this); }
const CPoint& CRect::TopLeft() const     { return *((const CPoint*)this); }
CPoint&       CRect::BottomRight()       { return *(((CPoint*)this) + 1); }
const CPoint& CRect::BottomRight() const { return *(((const CPoint*)this) + 1); }

void CRect::NormalizeRect()
{
    if (left > right) { LONG t2 = left; left = right; right = t2; }
    if (top > bottom) { LONG t2 = top;  top  = bottom; bottom = t2; }
}
