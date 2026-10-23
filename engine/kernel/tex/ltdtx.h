// ----------------------------------------------------------------------- //
//
// MODULE  : ltdtx.h
//
// PURPOSE : Reads NOLF 1's `.dtx` textures -- the world and model textures.
//           Decodes every pixel format the shipped data uses to RGBA8888, and
//           also hands out the raw blocks untouched for a GPU that can take
//           them directly.
//
// ⚠️ THIS IS NOT THE 2D INTERFACE. That is PCX (`ltpcx.h`), and confusing the
//   two already cost this port one false start in the other direction.
//
// THE SHAPE OF THE DATA (measured across all 4160 unique files, FORMATS.md §2)
//
//   Header is **164 bytes**, and all 4160 are `m_ResType` 0, version **-5** --
//   which is exactly what the donor's `dtxmgr` demands (`CURRENT_DTX_VERSION`
//   is an equality test), so the format itself transfers.
//
//   ⚠️ **2657 of 4166 are S3TC-COMPRESSED.** This is not a "read w*h*4 bytes"
//   job:
//
//     DXT1     2440      BPP_32   1502       DXT5   160
//     DXT3       57      BPP_32P     1
//
//   ⚠️ `m_Extra[2] == 0` means **BPP_32, not BPP_8P**, even though `BPP_8P = 0`
//   in the enum -- `GetBPPIdent()` special-cases zero (donor dtxmgr_lib.h:76).
//   Reading the enum value literally gets 605 files wrong.
//
// HOW THE DECODE IS CHECKED
//
//   * **Size prediction.** Every file's total length is predicted from its
//     header and compared against the archive's directory entry: **4160 of
//     4160 exact.** That is what caught an earlier census of this format
//     reading `m_Extra` at offset 28 instead of 24 and concluding "all 4166
//     are BPP_32, no compression" -- numbers that looked plausible and uniform
//     and were wrong.
//   * **An independent decoder.** Every S3TC mip level was decoded by this
//     code and by **Pillow's** BCn decoder and compared byte for byte:
//     **10,512 mip levels, 0 differing** (DXT1 9661, DXT5 623, DXT3 228).
//
// ⚠️ S3TC INTERPOLATION TRUNCATES -- IT DOES NOT ROUND
//
//   The endpoint blends are `(2*a + b) / 3`, the 3-colour midpoint is
//   `(a + b) >> 1`, and DXT5's alpha ramp is `(6*a0 + a1) / 7` -- all integer
//   division, no `+1`. That is not a style choice: it is what LithTech's own
//   software decoder does (donor `runtime/shared/src/pixelformat.cpp:892` and
//   `:947`), and Pillow agrees. Rounding instead is off by exactly +1 on about
//   13% of texels -- invisible to the eye and fatal to a byte-exact check.
//
// ⚠️ THE 3-COLOUR DXT1 BLOCK IS SELECTED ON THE RAW 565 WORDS
//
//   `c0 <= c1` (comparing the stored uint16s, not the expanded colours) means
//   the block has three colours and index 3 is **fully transparent**. Miss
//   this and every punch-through texture loses its holes.
//
// ⚠️ BPP_32 IS STORED **BGRA**
//
//   Established from the data rather than assumed: across the shipped set,
//   `SKY` textures are byte-0-dominant, `WOOD` byte-2-dominant and `FOLIAGE`
//   byte-1-dominant. Blue, red and green respectively -- three independent
//   semantic confirmations. This class always outputs **RGBA**.
//
// ⚠️ BPP_32P IS ONE FILE, AND IT IS THE ONLY FILE WITH A SECTION
//
//   `TEX\GLASS\GL01\GL0007.DTX` is 8-bit indices into a 256-entry 32-bit
//   palette, and the palette arrives in a trailing `"PALLETE32"` section. Its
//   `SectionHeader` is `char[15] + char[10] + uint32` = **32 bytes with
//   natural alignment**, not 29 -- the three padding bytes are what make the
//   file's size come out exact.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_DTX_H__
#define __LT_DTX_H__

#include <string>
#include <vector>

class CLTFileMgr;

class CLTDtx
{
public:

    // The formats the shipped data actually uses. Names follow the donor's
    // BPPIdent so the mapping is checkable against dtxmgr.
    enum EFormat
    {
        kUnknown = 0,
        kBPP_32,        // BPPIdent 3 -- and 0, which GetBPPIdent() folds to 32
        kDXT1,          // BPPIdent 4
        kDXT3,          // BPPIdent 5
        kDXT5,          // BPPIdent 6
        kBPP_32P        // BPPIdent 7 -- 8-bit indices + a PALLETE32 section
    };

    struct Mip
    {
        unsigned int    m_nWidth;
        unsigned int    m_nHeight;
        unsigned int    m_nOffset;      // into the file image
        unsigned int    m_nSize;        // bytes as stored (blocks, or w*h*n)
    };

    CLTDtx();

    // Reads a whole DTX held in memory. Returns false and stays empty on
    // anything it cannot represent exactly -- GetError() says why.
    bool                Load(const void* pData, unsigned int nSize);

    // Opens pName through the file manager, trying the name as given and then
    // with ".dtx" appended.
    bool                LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName);

    void                Clear();

    bool                IsLoaded() const { return m_bLoaded; }
    const char*         GetError() const { return m_sError.c_str(); }

    unsigned int        GetWidth() const { return m_nWidth; }
    unsigned int        GetHeight() const { return m_nHeight; }
    EFormat             GetFormat() const { return m_eFormat; }
    const char*         GetFormatName() const;
    bool                IsCompressed() const
                        { return m_eFormat == kDXT1 || m_eFormat == kDXT3 || m_eFormat == kDXT5; }

    // ⚠️ WHETHER THIS TEXTURE'S ALPHA CHANNEL CARRIES OPACITY. Measured, and
    // the answer is usually NO: **816 of the 1502 BPP_32 textures have alpha 0
    // in every texel**, and 317 more are mostly below 128. Alpha-testing them
    // makes over half of all uncompressed-textured geometry disappear.
    //
    // The compressed formats are the opposite: DXT1's 3-colour block encodes
    // real punch-through, and DXT3/DXT5 exist precisely to carry alpha, so an
    // artist choosing them is the signal. Uncompressed translucency is decided
    // per SURFACE by the game's own flags, not here.
    // ⚠️ CORRECTED 2026-09-09 -- the format was the WRONG QUESTION.
    //
    // "Compressed" is a statement about the artist's export setting, and the
    // counts above are real, but they hid a class this rule gets exactly
    // backwards: an UNCOMPRESSED texture that genuinely IS a cut-out. NOLF's
    // foliage is exactly that. `Props\Skins\Tree_04.dtx` is BPP_32 with 61% of
    // its texels at alpha 0 and 30% at 255 -- a palm frond -- and the old rule
    // declared its alpha meaningless, so every leaf drew as an opaque
    // dark-green slab. 21 plants and trees in M01S01 alone.
    //
    // The question that actually matters is whether the alpha channel
    // DISCRIMINATES: does it have both transparent and opaque texels? An
    // all-zero channel (the 816) does not -- alpha-testing it erases the
    // surface, which is the disaster the old rule was written to avoid. A
    // channel that varies is carrying a shape.
    //
    // ⚠️ So the all-zero case is still excluded, by measurement rather than by
    // proxy. See AlphaDiscriminates().
    // ⚠️ THE TEXTURE MUST SAY IT HAS ALPHA -- measured, not guessed.
    //
    //   This used to be `IsCompressed() || AlphaDiscriminates()`, i.e. "does
    //   the alpha channel happen to contain both transparent and opaque
    //   texels". That is a CONTENT heuristic standing in for an AUTHORED fact,
    //   and it was wrong in both directions across the shipped set:
    //
    //     408 textures DECLARE alpha (PREFER4444 / PREFER5551 / an alpharef)
    //         and were being drawn fully opaque because their content did not
    //         trip the heuristic.
    //      23 textures declare NOTHING and were being punched through anyway --
    //         and 19 of those were losing 40%+ of their texels. `DinnerTray`
    //         rendered as an empty rim with its whole surface and its food
    //         missing; `Flour` and `SpotLight` the same shape.
    //
    //   ⚠️ The foliage that the old rule was written for is unaffected: every
    //   plant and tree skin carries PREFER4444 AND an `AlphaRef 96`, so it is
    //   still tested. The heuristic was solving the right problem with the
    //   wrong evidence.
    //
    //   Compressed formats stay in: DXT1's 3-colour block IS punch-through,
    //   and DXT3/DXT5 are chosen by an artist precisely when alpha is wanted.
    bool                AlphaIsMeaningful() const;

    // ⚠️ Does the alpha channel contain BOTH transparent and opaque texels?
    // Decodes mip 0 once and caches the answer; false for a texture whose
    // alpha is uniform, whichever value it is uniform at.
    bool                AlphaDiscriminates() const;

    // ⚠️ THE AUTHORED ALPHA-TEST THRESHOLD, 0..255, or -1 when the texture
    // does not state one. It is written into the DTX's own COMMAND STRING as
    // `alpharef N` -- e.g. `Props\Skins\Lamp_02.dtx` says "alpharef 96".
    //
    // ⚠️ THE PORT USED A HARD-CODED 128 AND THAT DELETED GEOMETRY. 231 shipped
    // textures carry an alpharef and EVERY ONE of them is below 128 (min 10,
    // max 96, mean 94.8), so a 128 threshold punches out every texel the
    // artist intended to keep in the band between. On the wall lantern that is
    // the entire cage -- the bars vanish and the lamp renders as disconnected
    // fragments, which reads as a broken model rather than a wrong constant.
    //
    // ⚠️ Reading the authored value is not a refinement of the heuristic, it
    // REPLACES a guess with the data. The same shape as the PropTypes fix and
    // the origin offset: the answer was in the file all along.
    int                 GetAlphaRef() const;

    unsigned int        GetNumMipmaps() const { return (unsigned int)m_Mips.size(); }
    const Mip*          GetMip(unsigned int i) const;

    // The blocks/pixels exactly as stored, for handing straight to a GPU that
    // understands the format. Returns 0 for a bad index.
    const unsigned char* GetMipData(unsigned int i) const;

    // Decodes one mip to tightly packed RGBA8888 (w*h*4). Works for every
    // format, including the compressed ones.
    //
    // ⚠️ Decoding is ON DEMAND and writes to the caller's buffer rather than
    // caching internally. There are 4166 textures and up to 5 mips each; a
    // loader that eagerly decoded everything would cost far more memory than
    // the game ever needs at once, and the GPU path wants the blocks anyway.
    bool                DecodeMip(unsigned int i, std::vector<unsigned char>& outRGBA) const;

    // The authored `m_CommandString` -- how a texture declares things like
    // detail maps and environment mapping.
    const char*         GetCommandString() const { return m_sCommand.c_str(); }

    // m_Extra accessors, named as the donor names them (dtxmgr_lib.h:70-96).
    unsigned int        GetTextureGroup() const { return m_Extra[0]; }
    unsigned int        GetNumMipmapsToUse() const { return m_Extra[1]; }

    // ⚠️ How many mips to SKIP when the GPU cannot take S3TC. Whether that
    // applies here is a Metal question nobody has answered yet -- so this is
    // exposed rather than acted on.
    unsigned int        GetNonS3TCMipmapOffset() const { return m_Extra[3]; }
    unsigned int        GetUIMipmapOffset() const { return m_Extra[4]; }
    unsigned int        GetTexturePriority() const { return m_Extra[5]; }

    // ⚠️ The donor adds 1.0f "so all the old values of 0.0f return 1.0f", and
    // the four bytes are read out of an unaligned position in m_Extra.
    float               GetDetailTextureScale() const;
    int                 GetDetailTextureAngle() const;

    // ⚠️ The two that say "this texture has an alpha channel that matters".
    // Named here rather than left as bare bit tests at the use site, because
    // 1<<7 and 1<<8 are not self-describing and the donor's own header
    // (tools/shared/engine/dtxmgr.h:36,38) is the only place they are defined.
    enum { kPrefer4444 = (1 << 7),      // graded alpha if 16-bit
           kPrefer5551 = (1 << 8) };    // 1-bit alpha if 16-bit
    // ⚠️ THE TWO FLAGS DO NOT MEAN THE SAME THING, and collapsing them cost a
    // whole family of props. The donor's own header says it
    // (tools/shared/engine/dtxmgr.h:36,38):
    //
    //     PREFER5551  "use 5551 if 16-bit"   -> ONE BIT of alpha -> punch-through
    //     PREFER4444  "use a 4444 texture"   -> FOUR BITS        -> graded, for BLENDING
    //
    // A 1-bit texture IS a cutout, so testing it is right. A 4-bit one is a
    // gradient, and cutting it at a threshold throws away everything the extra
    // bits were for. Measured across the shipped set:
    //
    //     435 textures declare PREFER4444 and NOTHING else -- and 261 of those
    //         lose 40%+ of their texels to a 0.5 cut. `Lamp_05b` is 88.6%
    //         alpha-0, so its pendant lamp rendered as a small pale teardrop
    //         floating under the ceiling with its shade and cord deleted.
    //     220 declare PREFER4444 AND an alpharef -- the artist asking for a
    //         cutout at a specific value. That is the foliage, and it stays.
    //    1422 declare PREFER5551 alone. Punch-through, correctly.
    //
    // ⚠️ So 4444-alone is drawn OPAQUE, which is not right either -- it wants
    // the blended pass this renderer does not have yet (see lt3d.cpp). Opaque
    // gives the correct SHAPE with the wrong translucency; punching gives
    // neither. That is the trade until a blend pass exists, and it is a
    // deliberate choice rather than an oversight.
    bool                DeclaresAlpha() const
                        { return (m_nIFlags & kPrefer5551) != 0
                                 || GetAlphaRef() >= 0; }

    int                 GetIFlags() const { return m_nIFlags; }
    int                 GetUserFlags() const { return m_nUserFlags; }

    unsigned int        GetNumSections() const { return (unsigned int)m_Sections.size(); }
    const char*         GetSectionType(unsigned int i) const;

private:

    // Lazily computed by AlphaDiscriminates(); -1 = not yet asked.
    mutable int         m_nAlphaDiscriminates;

    bool                Fail(const char* pWhy);

    // Bytes one mip occupies as stored, or 0 if the format has no size rule.
    unsigned int        MipBytes(unsigned int w, unsigned int h) const;

    static void         DecodeBlocks(const unsigned char* pSrc, unsigned int w,
                                     unsigned int h, EFormat eFmt,
                                     unsigned char* pDest);

    struct Section
    {
        std::string                 m_sType;
        std::string                 m_sName;
        std::vector<unsigned char>  m_Data;
    };

    bool                        m_bLoaded;
    std::string                 m_sError;

    std::vector<unsigned char>  m_File;     // the whole image, mips index into it
    unsigned int                m_nWidth;
    unsigned int                m_nHeight;
    EFormat                     m_eFormat;
    int                         m_nIFlags;
    int                         m_nUserFlags;
    unsigned char               m_Extra[12];
    std::string                 m_sCommand;

    std::vector<Mip>            m_Mips;
    std::vector<Section>        m_Sections;
    const unsigned char*        m_pPalette;  // into m_Sections; 1024 bytes, BPP_32P only
};

#endif // __LT_DTX_H__
