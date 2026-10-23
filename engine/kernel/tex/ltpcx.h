// ----------------------------------------------------------------------- //
//
// MODULE  : ltpcx.h
//
// PURPOSE : Decodes a PCX image to RGBA8888.
//
//           This is what `ILTClient::CreateSurfaceFromBitmap` actually loads.
//
// ⚠️ IT IS PCX, NOT DTX -- AND THAT COST A FALSE START
//
//   `CreateSurfaceFromBitmap` looks like a texture entry point, and NOLF 1 has
//   4166 `.dtx` files, so the obvious move is to write a DTX loader. It is the
//   wrong move. `Layout.txt` says
//
//       Background = "interface\mb_back.pcx"
//
//   and `INTERFACE\MB_BACK.PCX` is what is in the archive. **The entire 2D
//   interface is PCX**; DTX is world and model textures. This was found while
//   already writing a DXT block decoder for a dialogue window that never wanted
//   one. See FORMATS.md §2a.
//
// ⚠️ NAMES ARRIVE WITHOUT AN EXTENSION TOO
//
//   `Frame = "Interface\DlgWnd\dialogueframe"`, and the game appends `TOP`,
//   `BOTTOMLEFT` and so on before anything appends `.pcx`. So a loader has to
//   try the name as given AND with `.pcx` added -- LoadThroughFileMgr does.
//
// THE SHAPE OF THE DATA (measured across all 289 files, FORMATS.md §2a)
//
//   version 5, encoding 1 (RLE), 8 bits per plane -- uniformly. Two variants:
//
//     214 files   1 plane   8-bit paletted, 256-colour palette in the last
//                           769 bytes behind a 0x0C marker
//      75 files   3 planes  24-bit RGB, no palette
//
//   Largest is INTERFACE\PAUSE.PCX at 640x480 -- exactly the screen.
//
// ⚠️ TWO TRAPS IN THE FORMAT ITSELF
//
//   * `BytesPerLine` is the STRIDE, and it is padded to an even number, so it
//     can exceed the image width. Using it as the width silently shifts every
//     row after the first by one pixel on any odd-width image.
//   * With 3 planes the scanlines are **plane-interleaved by ROW**, not by
//     pixel: a whole row of red, then a whole row of green, then blue, then
//     the next scanline. Treating it as RGB triplets produces a recognisable
//     but wrong image, which is the worst kind of wrong.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_PCX_H__
#define __LT_PCX_H__

#include <vector>

class CLTFileMgr;

class CLTPcx
{
public:

    CLTPcx();

    // Decodes a whole PCX held in memory. Returns false and stays empty on
    // anything that is not a version-5 RLE PCX this can represent.
    bool                    Load(const void* pData, unsigned int nSize);

    // Opens pName through the file manager -- trying the name as given first,
    // then with ".pcx" appended -- and decodes it.
    bool                    LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName);

    void                    Clear();

    bool                    IsLoaded() const { return !m_RGBA.empty(); }
    unsigned int            GetWidth() const  { return m_Width; }
    unsigned int            GetHeight() const { return m_Height; }
    unsigned int            GetPlanes() const { return m_Planes; }
    bool                    HasPalette() const { return m_bHasPalette; }

    // Tightly packed RGBA8888, m_Width * m_Height * 4 bytes.
    //
    // ⚠️ ALPHA IS ALWAYS 255. PCX has no alpha channel, and NOLF's transparency
    // is a COLOUR KEY chosen at DRAW time -- DrawSurfaceToSurfaceTransparent
    // takes the transparent colour as an argument, and the game's is
    // SETRGB_T(255,0,255) (GameClientShell.cpp:1081). Baking magenta into the
    // alpha here would move a draw-time decision into load time and quietly
    // break any image that legitimately contains that colour.
    const unsigned char*    GetRGBA() const { return m_RGBA.empty() ? 0 : &m_RGBA[0]; }
    unsigned int            GetRGBASize() const { return (unsigned int)m_RGBA.size(); }

private:

    // RLE-expands the body into m_Scratch. Returns false if the stream ends
    // early or overruns -- never a partial image presented as a whole one.
    bool                    Expand(const unsigned char* p, unsigned int nSize,
                                   unsigned int nTotal);

    unsigned int                m_Width;
    unsigned int                m_Height;
    unsigned int                m_Planes;
    unsigned int                m_BytesPerLine;
    bool                        m_bHasPalette;

    std::vector<unsigned char>  m_Scratch;   // planar, stride-padded
    std::vector<unsigned char>  m_RGBA;
};

#endif // __LT_PCX_H__
