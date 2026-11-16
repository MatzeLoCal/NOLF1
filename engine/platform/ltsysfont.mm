// ----------------------------------------------------------------------- //
//
// MODULE  : ltsysfont.mm
//
// PURPOSE : PORT: one glyph of a macOS system font as an anti-aliased
//           coverage bitmap, for the smooth subtitle font (ltfont.cpp,
//           CLTBitmapFont::BuildSmooth). AppKit only -- no extra framework.
//
// ----------------------------------------------------------------------- //

#import <AppKit/AppKit.h>

#include <string.h>
#include <vector>

// Renders code point `cp` in the bold system font so that a line is
// `nPixelHeight` pixels tall. Fills `cov` (nWidth x nPixelHeight, one byte of
// coverage per pixel) and returns the advance in nWidth. False for a code
// point the font cannot draw.
bool LTSysFont_RasterGlyph(unsigned int cp, int nPixelHeight,
                           std::vector<unsigned char>& cov, int& nWidth)
{
    @autoreleasepool
    {
        if (nPixelHeight < 6) return false;

        // Line height = ascender - descender + leading ~ 1.2 x point size.
        const CGFloat fSize = (CGFloat)nPixelHeight / 1.22;
        NSFont* font = [NSFont boldSystemFontOfSize:fSize];
        if (!font) return false;

        unichar ch = (unichar)cp;
        NSString* s = [NSString stringWithCharacters:&ch length:1];
        NSDictionary* attrs = @{ NSFontAttributeName: font,
                                 NSForegroundColorAttributeName: [NSColor whiteColor] };
        NSSize sz = [s sizeWithAttributes:attrs];
        int w = (int)ceil(sz.width);
        if (w <= 0) return false;
        w += 1;                                 // a column of air between glyphs

        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:nPixelHeight
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:w * 4 bitsPerPixel:32];
        if (!rep) return false;
        memset([rep bitmapData], 0, (size_t)w * nPixelHeight * 4);

        NSGraphicsContext* ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ctx];
        [ctx setShouldAntialias:YES];
        // Baseline: the descender sits on the bottom edge.
        const CGFloat y = -[font descender] + ((CGFloat)nPixelHeight - ([font ascender] - [font descender])) / 2.0;
        [s drawAtPoint:NSMakePoint(0, y) withAttributes:attrs];
        [ctx flushGraphics];
        [NSGraphicsContext restoreGraphicsState];

        const unsigned char* px = [rep bitmapData];
        const NSInteger stride = [rep bytesPerRow];
        cov.assign((size_t)w * nPixelHeight, 0);
        for (int yy = 0; yy < nPixelHeight; ++yy)
            for (int xx = 0; xx < w; ++xx)
                cov[(size_t)yy * w + xx] = px[yy * stride + xx * 4 + 3];   // alpha

        nWidth = w;
        return true;
    }
}
