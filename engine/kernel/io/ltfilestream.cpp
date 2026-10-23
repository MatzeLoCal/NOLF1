// ----------------------------------------------------------------------- //
//
// MODULE  : ltfilestream.cpp
//
// PURPOSE : ILTStream over a byte range of a file. See ltfilestream.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltfilestream.h"

#include <string.h>

// CGenLTStream::WriteStream's block size (genltstream.cpp:6).
#define WRITESTREAM_BLOCKSIZE   512


CLTFileStream::CLTFileStream()
    : m_pFile(0), m_Base(0), m_Len(0), m_Pos(0), m_ErrorStatus(0)
{
}

CLTFileStream::~CLTFileStream()
{
    if (m_pFile)
    {
        fclose(m_pFile);
        m_pFile = 0;
    }
}

CLTFileStream* CLTFileStream::Create(FILE* pFile, unsigned long nBase, unsigned long nLen)
{
    if (!pFile) return 0;

    CLTFileStream* pStream = new CLTFileStream;
    pStream->m_pFile = pFile;
    pStream->m_Base  = nBase;
    pStream->m_Len   = nLen;
    pStream->m_Pos   = 0;

    // Position the handle at the start of the visible range so a caller that
    // reads without seeking first gets the right bytes.
    if (fseek(pFile, (long)nBase, SEEK_SET) != 0)
        pStream->m_ErrorStatus = 1;

    return pStream;
}

void CLTFileStream::Release()
{
    // "Call this when you're done with it (same as deleting it)" -- iltstream.h.
    delete this;
}


// ⚠️ ALWAYS ZERO-FILLS ON FAILURE. iltstream.h:26 promises exactly this:
// "ALWAYS fills in pData with 0's if it returns an error so you can safely read
// everything in without checking the return value every time and check
// ErrorStatus() at the end." Game code relies on that -- GameButeMgr.cpp:144
// reads a whole file and never looks at the result.
LTRESULT CLTFileStream::Read(void *pData, unsigned long size)
{
    if (!pData) return LT_ERROR;
    if (size == 0) return LT_OK;

    if (m_ErrorStatus || !m_pFile)
    {
        memset(pData, 0, size);
        return LT_ERROR;
    }

    // Reading past the end of the visible range is an error even when the
    // underlying file has more bytes -- for a rez item those bytes belong to
    // the next resource.
    if (m_Pos > m_Len || size > m_Len - m_Pos)
    {
        memset(pData, 0, size);
        m_ErrorStatus = 1;
        return LT_ERROR;
    }

    if (fseek(m_pFile, (long)(m_Base + m_Pos), SEEK_SET) != 0 ||
        fread(pData, 1, (size_t)size, m_pFile) != (size_t)size)
    {
        memset(pData, 0, size);
        m_ErrorStatus = 1;
        return LT_ERROR;
    }

    m_Pos += size;
    return LT_OK;
}


// Faithful to CGenLTStream::ReadString (genltstream.cpp:12): a uint16 length
// prefix, then that many bytes, with the tail consumed but discarded when the
// caller's buffer is too small.
LTRESULT CLTFileStream::ReadString(char *pStr, unsigned long maxBytes)
{
    uint16 len = 0;
    if (Read(&len, sizeof(len)) != LT_OK)
    {
        if (pStr && maxBytes > 0) pStr[0] = 0;
        return LT_ERROR;
    }

    if (len == 0)
    {
        if (pStr && maxBytes > 0) pStr[0] = 0;
        return LT_OK;
    }

    if (maxBytes == 0 || !pStr)
    {
        // Still consume the string, or every later read is misaligned.
        char dummy;
        for (uint16 i = 0; i < len; ++i)
            Read(&dummy, 1);
        return LT_OK;
    }

    unsigned long maxChars = maxBytes - 1;
    LTRESULT result;

    if ((unsigned long)len > maxChars)
    {
        result = Read(pStr, maxChars);
        pStr[maxChars] = 0;

        unsigned long remaining = (unsigned long)len - maxChars;
        char dummy;
        while (remaining--)
            Read(&dummy, 1);
    }
    else
    {
        result = Read(pStr, len);
        pStr[len] = 0;
    }

    return result;
}


LTRESULT CLTFileStream::ErrorStatus()
{
    return m_ErrorStatus ? LT_ERROR : LT_OK;
}


LTRESULT CLTFileStream::SeekTo(unsigned long offset)
{
    // Seeking exactly to the end is legal; reading there is not.
    if (offset > m_Len)
    {
        m_ErrorStatus = 1;
        return LT_ERROR;
    }

    m_Pos = offset;

    if (m_pFile && fseek(m_pFile, (long)(m_Base + m_Pos), SEEK_SET) != 0)
    {
        m_ErrorStatus = 1;
        return LT_ERROR;
    }

    return LT_OK;
}

LTRESULT CLTFileStream::GetPos(unsigned long *offset)
{
    if (!offset) return LT_ERROR;
    *offset = m_Pos;
    return LT_OK;
}

LTRESULT CLTFileStream::GetLen(unsigned long *len)
{
    if (!len) return LT_ERROR;
    *len = m_Len;
    return LT_OK;
}


LTRESULT CLTFileStream::WriteStream(ILTStream & /*dsSource*/,
                                    unsigned long /*dwMin*/, unsigned long /*dwMax*/)
{
    // WriteStream writes INTO this stream, and this stream is read-only.
    return LT_ERROR;
}

LTRESULT CLTFileStream::Write(const void * /*pData*/, unsigned long /*size*/)
{
    return LT_ERROR;
}

LTRESULT CLTFileStream::WriteString(char * /*pStr*/)
{
    return LT_ERROR;
}
