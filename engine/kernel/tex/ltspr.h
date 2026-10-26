// ----------------------------------------------------------------------- //
//
// MODULE  : ltspr.h
//
// PURPOSE : `.spr` -- a LithTech SPRITE, which is not an image.
//
// ⚠️ THE SURPRISE, AND IT IS WHY THIS WAS CHEAP. "Sprite" suggests pixel data
// and a decoder to write. A `.spr` is 37 to 2,438 BYTES: a frame count, a frame
// rate, and a list of DTX filenames. All the pixels are in textures this port
// already reads. Measured before a line of it was written -- the whole format
// was settled by hex-dumping two files and then predicting the size of all 173.
//
// LAYOUT (verified on every shipped file)
//
//   uint32  nFrames                 1 .. 78
//   uint32  framesPerSecond         one of 1 2 3 5 6 8 10 12 15 24 30
//   uint32  bTransparent            the donor's names (sprite.cpp) -- and
//   uint32  bTranslucent            its renderer reads none of the three
//   uint32  colourKey
//   nFrames x { uint16 len; char name[len] }      no terminator
//
// ⚠️ THE THREE FLAGS ARE NOT ALWAYS ZERO (5dh). 169 sprites carry zeros; FOUR
// carry transparent = translucent = 1 -- SFX\Impact\Spr\Smoke.spr and the
// three SFX\Smoke\Spr\*Trail*.spr -- and they are the ones every smoke
// particle system is set up with. This reader used to REFUSE a non-zero flag
// as "header is not what this reader expects", so all smoke and steam had no
// texture and drew nothing. LT_OLDSPRHEADER=1 brings the refusal back.
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

    CLTSprite() : m_bLoaded(false), m_nFPS(0),
                  m_bTransparent(false), m_bTranslucent(false), m_nColourKey(0) {}

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

    // The header's three flags, as read (see LAYOUT). Nothing draws by them.
    bool                IsTransparent() const { return m_bTransparent; }
    bool                IsTranslucent() const { return m_bTranslucent; }
    unsigned int        GetColourKey() const { return m_nColourKey; }

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
    bool                        m_bTransparent;
    bool                        m_bTranslucent;
    unsigned int                m_nColourKey;
    std::vector<std::string>    m_Frames;
};

#endif // __LT_SPR_H__
