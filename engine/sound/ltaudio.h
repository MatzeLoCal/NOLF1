// ----------------------------------------------------------------------- //
//
// MODULE  : ltaudio.h
//
// PURPOSE : The CoreAudio device, and MP3 decoding. Everything platform.
//
// ⚠️ THE DIVISION OF LABOUR IS THE POINT. CLTMixer decides what the samples
// ARE and is pure C++ with 21 checks over it; this file only gets them to the
// speakers and turns MP3 into PCM. Nothing here makes a decision that could be
// wrong in a way a test could catch, which is why there is so little of it.
//
// ⚠️ MP3 IS DECODED BY AUDIOTOOLBOX, deliberately. 3,662 of the game's 4,798
// sounds are MP3 in a RIFF wrapper (FORMATS.md §8), and macOS decodes MPEG in
// the system frameworks. Writing an MPEG-1 Layer III decoder to avoid a
// dependency that ships with the OS would be weeks of work to arrive somewhere
// slower and less correct.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_AUDIO_H__
#define __LT_AUDIO_H__

#include <vector>

class CLTMixer;

// Opens the default output device and starts pulling from pMixer. The mixer
// must outlive the device. Safe to call twice.
bool LTAudio_Start(CLTMixer* pMixer);
void LTAudio_Stop();
bool LTAudio_Running();

// Why the device did not start. Null when there is nothing to say.
const char* LTAudio_LastError();

// ⚠️ Decodes an MP3 BITSTREAM (the `data` chunk of a fmt-tag-85 WAV) to 16-bit
// mono/stereo PCM. Returns false and leaves the outputs untouched on anything
// it cannot do -- a caller must not treat failure as silence.
bool LTAudio_DecodeMP3(const void* pData, unsigned int nSize,
                       std::vector<short>& outSamples,
                       unsigned int& outRate, unsigned int& outChannels);

#endif // __LT_AUDIO_H__
