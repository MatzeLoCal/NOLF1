// ----------------------------------------------------------------------- //
//
// MODULE  : ltspr.h
//
// PURPOSE : `.spr` -- a LithTech SPRITE, which is not an image.
//
// ⚠️ THE SURPRISE, AND IT IS WHY THIS WAS CHEAP. "Sprite" suggests pixel data
// and a decoder to write. A `.spr` is 51 to 268 BYTES: a frame count, a frame
// rate, and a list of DTX filenames. All the pixels are in textures this port
// already reads. Measured before a line of it was written -- the whole format
// was settled by hex-dumping two files and then predicting the size of all 173.
//
// LAYOUT (verified on every shipped file)
//
//   uint32  nFrames                 1 .. 78
//   uint32  framesPerSecond         one of 1 2 3 5 6 8 10 12 15 24 30
//   uint32  x3                      always zero
//   nFrames x { uint16 len; char name[len] }      no terminator
//
// ⚠️ The size prediction is the test, exactly as it was for ABC chunks and v66
// world models: `20 + sum(2 + len)` must equal the file length. **173 of 173
// land exactly**, 1,106 frames in total, and only ONE named frame texture is
// missing from the archives.
//
// WHY IT MATTERS: a world surface may name a `.spr` instead of a `.dtx`. Until
// this existed those surfaces had no texture at all and rendered flat shaded --
// 63 references across the shipped worlds, and the same gap is why wall sconces
// have brackets but no flame.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SPR_H__
#define __LT_SPR_H__

#include <string>
#include <vector>

class CLTFileMgr;

class CLTSprite
{
public:

    CLTSprite() : m_bLoaded(false), m_nFPS(0) {}

    // Reads a whole .spr held in memory. Returns false and stays empty on
    // anything it cannot account for EXACTLY -- see GetError().
    bool                Load(const void* pData, unsigned int nSize);

    // Opens pName through the file manager, trying the name as given and then
    // with ".spr" appended.
    bool                LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName);

    bool                IsLoaded() const { return m_bLoaded; }
    const char*         GetError() const { return m_sError.c_str(); }

    unsigned int        GetNumFrames() const { return (unsigned int)m_Frames.size(); }
    unsigned int        GetFPS() const { return m_nFPS; }

    // The DTX filename for frame i, or "" out of range.
    const char*         GetFrame(unsigned int i) const;

    // ⚠️ The frame to show at a given time, wrapping. Returns 0 when the
    // sprite has no frames, and NEVER divides by a zero frame rate -- a
    // sprite declaring 0 fps is a still, not a crash.
    unsigned int        FrameAt(float fSeconds) const;

private:

    bool                Fail(const char* pWhy);

    bool                        m_bLoaded;
    std::string                 m_sError;
    unsigned int                m_nFPS;
    std::vector<std::string>    m_Frames;
};

#endif // __LT_SPR_H__
