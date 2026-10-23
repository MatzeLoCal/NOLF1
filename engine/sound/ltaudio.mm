// ----------------------------------------------------------------------- //
//
// MODULE  : ltaudio.mm
//
// See ltaudio.h.
//
// ----------------------------------------------------------------------- //

#import <AudioToolbox/AudioToolbox.h>

#include "ltaudio.h"
#include "ltmixer.h"

#include <string.h>
#include <stdio.h>
#include <vector>

namespace
{
    AudioUnit   g_Unit     = 0;
    CLTMixer*   g_pMixer   = 0;
    bool        g_bRunning = false;
    char        g_Err[256] = { 0 };

    void Fail(const char* p) { snprintf(g_Err, sizeof(g_Err), "%s", p ? p : "?"); }

    // ⚠️ THIS RUNS ON THE AUDIO THREAD. No allocation, no locks, no logging --
    // a render callback that blocks does not sound like a slow callback, it
    // sounds like a click, because the device gets no samples in time.
    OSStatus RenderCB(void* /*pRef*/, AudioUnitRenderActionFlags* /*pFlags*/,
                      const AudioTimeStamp* /*pTime*/, UInt32 /*nBus*/,
                      UInt32 nFrames, AudioBufferList* pData)
    {
        if (!pData || pData->mNumberBuffers < 1) return noErr;

        short* pOut = (short*)pData->mBuffers[0].mData;
        if (!pOut) return noErr;

        if (g_pMixer) g_pMixer->Render(pOut, (unsigned int)nFrames);
        else          memset(pOut, 0, (size_t)nFrames * 2 * sizeof(short));

        return noErr;
    }

    // ---- reading an in-memory file for AudioToolbox --------------------- //
    struct MemFile { const unsigned char* m_p; SInt64 m_n; };

    OSStatus MemRead(void* pRef, SInt64 nPos, UInt32 nReq, void* pBuf, UInt32* pGot)
    {
        MemFile* f = (MemFile*)pRef;
        if (nPos >= f->m_n) { *pGot = 0; return noErr; }
        SInt64 n = (SInt64)nReq;
        if (nPos + n > f->m_n) n = f->m_n - nPos;
        memcpy(pBuf, f->m_p + nPos, (size_t)n);
        *pGot = (UInt32)n;
        return noErr;
    }

    SInt64 MemSize(void* pRef) { return ((MemFile*)pRef)->m_n; }
}


bool LTAudio_Running() { return g_bRunning; }
const char* LTAudio_LastError() { return g_Err[0] ? g_Err : 0; }


bool LTAudio_Start(CLTMixer* pMixer)
{
    if (g_bRunning) return true;
    if (!pMixer) { Fail("no mixer"); return false; }
    g_pMixer = pMixer;
    g_Err[0] = 0;

    AudioComponentDescription desc;
    memset(&desc, 0, sizeof(desc));
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(0, &desc);
    if (!comp) { Fail("no default output component"); return false; }

    if (AudioComponentInstanceNew(comp, &g_Unit) != noErr || !g_Unit)
    { Fail("could not create the output unit"); return false; }

    // ⚠️ Interleaved 16-bit stereo at the mixer's own rate, so the device
    // never resamples behind our back -- the mixer already resampled every
    // voice into this rate and a second conversion would be pure loss.
    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate       = (Float64)CLTMixer::kRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = CLTMixer::kChannels;
    fmt.mBitsPerChannel   = 16;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = fmt.mChannelsPerFrame * fmt.mBitsPerChannel / 8;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;

    if (AudioUnitSetProperty(g_Unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr)
    { Fail("output does not accept 16-bit stereo"); AudioComponentInstanceDispose(g_Unit); g_Unit = 0; return false; }

    AURenderCallbackStruct cb;
    cb.inputProc = &RenderCB;
    cb.inputProcRefCon = 0;
    if (AudioUnitSetProperty(g_Unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr)
    { Fail("could not install the render callback"); AudioComponentInstanceDispose(g_Unit); g_Unit = 0; return false; }

    if (AudioUnitInitialize(g_Unit) != noErr)
    { Fail("the output unit would not initialise"); AudioComponentInstanceDispose(g_Unit); g_Unit = 0; return false; }

    if (AudioOutputUnitStart(g_Unit) != noErr)
    { Fail("the output unit would not start"); AudioUnitUninitialize(g_Unit);
      AudioComponentInstanceDispose(g_Unit); g_Unit = 0; return false; }

    g_bRunning = true;
    return true;
}


void LTAudio_Stop()
{
    if (!g_bRunning) return;

    // ⚠️ Stop the device BEFORE dropping the mixer pointer: the callback can be
    // mid-flight on the audio thread, and clearing it first is a use-after-free
    // that only shows up under load.
    AudioOutputUnitStop(g_Unit);
    AudioUnitUninitialize(g_Unit);
    AudioComponentInstanceDispose(g_Unit);
    g_Unit = 0;
    g_pMixer = 0;
    g_bRunning = false;
}


bool LTAudio_DecodeMP3(const void* pData, unsigned int nSize,
                       std::vector<short>& outSamples,
                       unsigned int& outRate, unsigned int& outChannels)
{
    if (!pData || nSize < 4) return false;

    MemFile mf; mf.m_p = (const unsigned char*)pData; mf.m_n = (SInt64)nSize;

    AudioFileID af = 0;
    if (AudioFileOpenWithCallbacks(&mf, &MemRead, 0, &MemSize, 0,
                                   kAudioFileMP3Type, &af) != noErr || !af)
        return false;

    ExtAudioFileRef ext = 0;
    if (ExtAudioFileWrapAudioFileID(af, false, &ext) != noErr || !ext)
    { AudioFileClose(af); return false; }

    AudioStreamBasicDescription src;
    UInt32 n = sizeof(src);
    if (ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileDataFormat, &n, &src) != noErr)
    { ExtAudioFileDispose(ext); AudioFileClose(af); return false; }

    const UInt32 nCh = src.mChannelsPerFrame ? src.mChannelsPerFrame : 1;

    // Ask for 16-bit PCM at the source's own rate; the MIXER does the rate
    // conversion, in one place, for every format.
    AudioStreamBasicDescription dst;
    memset(&dst, 0, sizeof(dst));
    dst.mSampleRate       = src.mSampleRate;
    dst.mFormatID         = kAudioFormatLinearPCM;
    dst.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    dst.mChannelsPerFrame = nCh;
    dst.mBitsPerChannel   = 16;
    dst.mFramesPerPacket  = 1;
    dst.mBytesPerFrame    = nCh * 2;
    dst.mBytesPerPacket   = dst.mBytesPerFrame;

    if (ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ClientDataFormat,
                                sizeof(dst), &dst) != noErr)
    { ExtAudioFileDispose(ext); AudioFileClose(af); return false; }

    outSamples.clear();
    std::vector<short> chunk(4096 * nCh);

    for (;;)
    {
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = nCh;
        abl.mBuffers[0].mDataByteSize   = (UInt32)(chunk.size() * sizeof(short));
        abl.mBuffers[0].mData           = &chunk[0];

        UInt32 nFrames = 4096;
        if (ExtAudioFileRead(ext, &nFrames, &abl) != noErr) break;
        if (nFrames == 0) break;        // end of stream

        outSamples.insert(outSamples.end(), chunk.begin(),
                          chunk.begin() + (size_t)nFrames * nCh);
    }

    ExtAudioFileDispose(ext);
    AudioFileClose(af);

    if (outSamples.empty()) return false;

    outRate     = (unsigned int)src.mSampleRate;
    outChannels = (unsigned int)nCh;
    return true;
}
