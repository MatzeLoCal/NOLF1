// ----------------------------------------------------------------------- //
//
// MODULE  : ltmixer.h
//
// PURPOSE : Mixes NOLF's sounds into a stereo buffer. No CoreAudio, no
//           AudioToolbox, no Objective-C -- just samples in, samples out.
//
// ⚠️ THIS IS SPLIT FROM THE DEVICE ON PURPOSE, and it is the same split that
// made the Metal backend checkable: the part with the DECISIONS in it is pure
// C++ and can be run headlessly, so "does a sound 40 units to the left come out
// of the left speaker" is a test rather than a thing someone listens for.
//
// A mixer that only exists inside a CoreAudio render callback can only be
// tested by ear, and "it made a noise" is not a check anyone else can repeat.
//
// ⚠️ WHAT IT IS NOT: the game's sound engine. No reverb, no occlusion, no
// obstruction, no EAX filters, no doppler. Those are real entries in
// ILTClientSoundMgr and they are declined explicitly rather than faked -- see
// ltsoundmgr.cpp.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_MIXER_H__
#define __LT_MIXER_H__

#include <vector>

class CLTMixer
{
public:

    // ⚠️ One rate for the whole mix. Every voice resamples into it, because
    // mixing sources at different rates without resampling plays them at the
    // wrong pitch -- and the shipped data is 44100, 22050 and 11025, so two
    // thirds of the game would be wrong.
    enum { kRate = 44100, kChannels = 2 };

    // How a voice is positioned. Matches the PLAYSOUND_ flags the game sets.
    enum Mode
    {
        kLocal   = 0,       // inside the head: no distance, no panning
        kAmbient = 1,       // distance rolloff, no panning
        k3D      = 2        // distance rolloff and panning
    };

    struct Params
    {
        Mode    m_eMode;
        float   m_Pos[3];
        float   m_fInner;       // full volume within this radius
        float   m_fOuter;       // silent beyond it
        float   m_fVolume;      // 0..1
        float   m_fPitch;       // 1 = as recorded
        bool    m_bLoop;

        Params()
            : m_eMode(kLocal), m_fInner(0), m_fOuter(0),
              m_fVolume(1.0f), m_fPitch(1.0f), m_bLoop(false)
        { m_Pos[0] = m_Pos[1] = m_Pos[2] = 0.0f; }
    };

    CLTMixer();

    // Where the ears are. pRight3 must be the listener's RIGHT vector -- the
    // same `f x up` the camera uses, or sounds pan the wrong way.
    void            SetListener(const float* pPos3, const float* pRight3);

    // Starts a voice on 16-bit mono or stereo samples the CALLER owns and
    // keeps alive until the voice ends. Returns a handle, or 0.
    //
    // ⚠️ Not copied: the game holds thousands of sounds and a mixer that
    // duplicated every buffer would double the memory for nothing.
    unsigned int    Play(const short* pSamples, unsigned int nSamples,
                         unsigned int nRate, unsigned int nChannels,
                         const Params& p);

    void            Stop(unsigned int nVoice);
    void            StopAll();
    bool            IsDone(unsigned int nVoice) const;
    void            SetVoicePosition(unsigned int nVoice, const float* pPos3);
    bool            GetVoicePosition(unsigned int nVoice, float* pPos3) const;

    void            SetPaused(bool b) { m_bPaused = b; }
    bool            GetPaused() const { return m_bPaused; }

    // Master volume, 0..1.
    void            SetVolume(float f);
    float           GetVolume() const { return m_fMaster; }

    unsigned int    ActiveVoices() const;

    // How long a live voice runs, in seconds, at its current pitch. False for
    // a voice that has already ended -- its length is no longer known here,
    // and inventing one would be worse than saying so.
    bool            VoiceDuration(unsigned int nVoice, float& fSeconds) const;

    // ⚠️ THE LIVE VOICE'S OWN SAMPLES AND WHERE IT HAS GOT TO -- what NOLF's
    // lip sync reads (ILTClient::GetSoundData / GetSoundOffset). It averages
    // the amplitude just ahead of the play cursor and opens the jaw by it, so
    // both answers must describe the voice actually playing, not the file.
    // False once the voice has ended. The cursor is in FRAMES.
    bool            VoiceData(unsigned int nVoice, const short*& pSamples,
                              unsigned int& nSamples, unsigned int& nRate,
                              unsigned int& nChannels) const;
    bool            VoiceCursor(unsigned int nVoice, unsigned int& nFrame) const;

    // ⚠️ Fills nFrames INTERLEAVED stereo frames, replacing whatever is there.
    // Callable from a device thread; the only mutable state it touches is each
    // voice's own cursor.
    void            Render(short* pOut, unsigned int nFrames);

    // ⚠️ RETIRES VOICES WITHOUT MIXING, and this is not a test convenience.
    // A voice ends only when someone CONSUMES it -- that someone is normally
    // the CoreAudio pull. With no output device nothing pulls, so nothing ever
    // ends, so IsSoundDone answers false forever.
    //
    // That is not "no sound" but a HANG: NOLF's cinematic dialogue advances by
    // asking IsSoundDone about the line it is speaking (CinematicTrigger), so
    // a headless run stops after its FIRST line and waits for a sound that
    // cannot finish. Measured on M01S01: one Voice\10124.wav and then silence
    // for the remaining 394 updates.
    //
    // So when no device is running the host advances the mixer itself, by the
    // frame's worth of time. Same arithmetic Render does to the cursor, same
    // retirement rule, no output buffer.
    void            Advance(unsigned int nFrames);

    // The gain a voice at this distance would get. Exposed because the rolloff
    // is the one piece of real arithmetic here and deserves to be asserted
    // directly rather than inferred from mixed output.
    static float    DistanceGain(float fDistance, float fInner, float fOuter);

private:

    struct Voice
    {
        const short*    m_pSamples;
        unsigned int    m_nSamples;     // total shorts, not frames
        unsigned int    m_nRate;
        unsigned int    m_nChannels;
        double          m_fCursor;      // in FRAMES, fractional for resampling
        Params          m_Params;
        unsigned int    m_nId;
        bool            m_bActive;
    };

    Voice*          Find(unsigned int nId);
    const Voice*    Find(unsigned int nId) const;
    void            GainsFor(const Voice& v, float& fL, float& fR) const;

    std::vector<Voice>  m_Voices;
    unsigned int        m_nNextId;
    float               m_Listener[3];
    float               m_Right[3];
    float               m_fMaster;
    bool                m_bPaused;
};

// The music (HANDOVER 5cj): a source that ADDS nFrames of interleaved stereo
// float (full scale 1.0) to the buffer it is given. Called from Render -- the
// device thread -- so it must not block; the music manager's is a lock-free
// ring. Mixed BEFORE the mixer's own pause: DirectMusic was never under
// DirectSound's pause, the game pauses music itself. 0 = none.
typedef void (*LTMixerMusicFn)(float* pLR, unsigned int nFrames);
void LTMixer_SetMusicSource(LTMixerMusicFn fn);

#endif // __LT_MIXER_H__
