// ----------------------------------------------------------------------- //
//
// MODULE  : ltfilestream.h
//
// PURPOSE : ILTStream over a byte range of a file on disk.
//
// WHY ONE CLASS AND NOT TWO
//
//   The donor has two stream classes -- DosFileStream over a whole file and
//   RezFileStream over an item inside an archive (de_file.cpp:145 and :183).
//   They differ only in where offset zero is and how long the file is, so this
//   is one class parameterised by a base offset and a length. A loose file is
//   the range [0, filelen); a rez item is [item.pos, item.pos + item.size).
//
//   That collapses the donor's positional bookkeeping -- it keeps a
//   process-wide "last item and last position" pair (g_pDeFileLastRezItm /
//   g_nDeFileLastRezPos) to skip a seek when reads happen to be sequential.
//   Here every stream owns its own FILE*, so its position IS the stream's
//   position and no such cache is needed. The cost is one file descriptor per
//   open stream instead of one per archive; the benefit is that two streams
//   into the same archive cannot interfere, which in the donor requires a
//   critical section (LT_FILE_THREADSAFE).
//
// ⚠️ THE SIGNATURES ARE LT2's, NOT THE DONOR'S
//
//   LT2's ILTStream declares sizes as `unsigned long`; Jupiter's declares them
//   as uint32. On LP64 those are different types. Copying the donor's
//   signatures would produce overloads rather than overrides -- which here
//   fails loudly (the class stays abstract) rather than silently, but only
//   because every one of them is pure virtual. See HANDOVER.md §4.5.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_FILESTREAM_H__
#define __LT_FILESTREAM_H__

#include <stdio.h>

#include "iltstream.h"

class CLTFileStream : public ILTStream
{
public:

    // Takes ownership of pFile: Release() fcloses it.
    // nBase/nLen describe the visible range; the stream's own position 0 is
    // file offset nBase.
    static CLTFileStream*   Create(FILE* pFile, unsigned long nBase, unsigned long nLen);

    // --- ILTStream ---------------------------------------------------- //

    void        Release() override;

    LTRESULT    Read(void *pData, unsigned long size) override;
    LTRESULT    ReadString(char *pStr, unsigned long maxBytes) override;

    LTRESULT    ErrorStatus() override;

    LTRESULT    SeekTo(unsigned long offset) override;
    LTRESULT    GetPos(unsigned long *offset) override;
    LTRESULT    GetLen(unsigned long *len) override;

    LTRESULT    WriteStream(ILTStream &dsSource,
                            unsigned long dwMin = WRITESTREAM_DEFAULT,
                            unsigned long dwMax = WRITESTREAM_DEFAULT) override;

    // Read-only. Both of these report LT_ERROR rather than pretending, for the
    // reason gen_skeleton.py's header gives: "succeeded" when nothing happened
    // is the failure that costs weeks.
    LTRESULT    Write(const void *pData, unsigned long size) override;
    LTRESULT    WriteString(char *pStr) override;

private:

    CLTFileStream();
    ~CLTFileStream() override;

    FILE*           m_pFile;
    unsigned long   m_Base;     // file offset of this stream's position 0
    unsigned long   m_Len;      // visible length
    unsigned long   m_Pos;      // current position, relative to m_Base
    int             m_ErrorStatus;
};

#endif // __LT_FILESTREAM_H__
