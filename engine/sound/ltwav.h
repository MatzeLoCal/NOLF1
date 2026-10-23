// ----------------------------------------------------------------------- //
//
// MODULE  : ltwav.h
//
// PURPOSE : Reads NOLF's `.wav` files into 16-bit PCM.
//
// ⚠️ THE EXTENSION LIES, AND THE MEASUREMENT IS THE WHOLE REASON THIS HEADER
// EXISTS. Of the 4,798 WAVs the game ships (FORMATS.md §8):
//
//     3,662 (76%)  are an MP3 BITSTREAM in a RIFF container   (fmt tag 85)
//       886        are PCM                                    (fmt tag 1)
//       250        are IMA / DVI ADPCM                        (fmt tag 17)
//
// A loader written for "WAV" handles a quarter of this game's audio, and the
// failure mode of the other three quarters is not a refusal -- a PCM reader
// will play an MP3 frame header as samples and produce noise. So the format tag
// is checked before a single byte is interpreted, and an unsupported one is
// REPORTED, never guessed at.
//
// ⚠️ MP3 IS DELIBERATELY NOT DECODED HERE. macOS decodes MP3 in the system
// frameworks, and writing a bit-exact MPEG decoder to avoid using it would be
// weeks of work to arrive somewhere worse. This module identifies the format
// and hands out the raw payload; the platform layer turns it into samples.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WAV_H__
#define __LT_WAV_H__

#include <vector>

class CLTFileMgr;

class CLTWav
{
public:

    enum Format
    {
        kUnknown = 0,
        kPCM     = 1,       // fmt tag 1
        kADPCM   = 17,      // fmt tag 17, IMA/DVI
        kMP3     = 85       // fmt tag 85, MPEG layer 3
    };

    CLTWav();

    bool                Load(const void* pData, unsigned int nSize);
    bool                LoadThroughFileMgr(CLTFileMgr* pFileMgr, const char* pName);

    bool                IsLoaded() const  { return m_bLoaded; }
    const char*         GetError() const  { return m_sError; }

    Format              GetFormat() const   { return m_eFormat; }
    unsigned int        GetRate() const     { return m_nRate; }
    unsigned int        GetChannels() const { return m_nChannels; }
    unsigned int        GetBits() const     { return m_nBits; }

    // ⚠️ PCM and ADPCM are decoded to 16-bit here; MP3 is NOT, and
    // GetSamples() is empty for it. Ask GetFormat() first -- a caller that
    // assumes samples exist gets silence, which is the hardest bug to see.
    const std::vector<short>& GetSamples() const { return m_Samples; }

    // The undecoded `data` chunk. For MP3 this is the bitstream to hand to the
    // platform decoder.
    const std::vector<unsigned char>& GetRaw() const { return m_Raw; }

    // Seconds, or 0 when it cannot be known without decoding.
    float               GetDuration() const;

private:

    bool                Fail(const char* p);

    bool                m_bLoaded;
    const char*         m_sError;
    Format              m_eFormat;
    unsigned int        m_nRate, m_nChannels, m_nBits, m_nBlockAlign;
    std::vector<short>          m_Samples;
    std::vector<unsigned char>  m_Raw;
};

#endif // __LT_WAV_H__
