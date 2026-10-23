// ----------------------------------------------------------------------- //
//
// MODULE  : ltsoundmgr.h
//
// PURPOSE : ILTClientSoundMgr, the interface the GAME calls, mapped onto
//           CLTMixer and the WAV reader.
//
// ⚠️ THIS IS THE JOIN, AND IT DECIDES NOTHING. The data tier (ltwav) knows the
// formats, the mixer knows the arithmetic, the device knows CoreAudio; this
// class translates PLAYSOUND_ flags into mixer parameters and hands out
// handles. Every line of it should be traceable to the header it implements.
//
// ⚠️ WHAT IS DECLINED, LOUDLY RATHER THAN FAKED. Eleven of the interface's
// twenty-four methods are reverb, occlusion, obstruction and EAX filter
// controls -- a hardware-era API that no macOS output has an equivalent for.
// They return LT_UNSUPPORTED and say so ONCE, in the same spirit as the GameSpy
// stub (HANDOVER.md §6): a caller that takes the "unavailable" path is correct,
// and a caller that gets LT_OK from a filter that did nothing is not.
//
// ⚠️ SOUNDS ARE CACHED BY NAME AND DECODED ONCE. NOLF plays the same footstep
// hundreds of times; decoding an MP3 per play would be an audible stall on the
// game thread. The cache owns the samples, and it must outlive every voice
// pointing into it -- which is why the mixer never copies and never frees.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SOUND_MGR_H__
#define __LT_SOUND_MGR_H__

#include "iltsoundmgr.h"
#include "ltmixer.h"

#include <map>
#include <string>
#include <vector>

class CLTFileMgr;

class CLTSoundMgr : public ILTClientSoundMgr
{
public:

    CLTSoundMgr();
    ~CLTSoundMgr();

    // ⚠️ Not in the interface. The host wires the file system and starts the
    // device; the game only ever sees ILTClientSoundMgr.
    void        Attach(CLTFileMgr* pFileMgr);
    void        Shutdown();
    CLTMixer&   GetMixer() { return m_Mixer; }

    // Where the ears are, pushed from the camera each frame.
    void        UpdateListener(const float* pPos3, const float* pRight3);

    // ⚠️ ILTClient::Set/GetListener -- the GAME's idea of where the ears are
    // (donor clientde_impl.cpp ci_GetListener, soundmgr.cpp:1319). "In the
    // client" means the ears follow the client's own object, pushed every
    // frame by UpdateClientListener; otherwise they stay where a cinematic
    // camera put them. GetListener writes EVERY output (see LTHost_GetListener).
    void        UpdateClientListener(const float* pPos3, const float* pQuat4);
    void        GetListenerState(bool* pInClient, float* pPos3, float* pQuat4) const;

    unsigned int CachedSounds() const { return (unsigned int)m_Cache.size(); }
    unsigned int Decoded() const { return m_nDecoded; }
    unsigned int Missing() const { return m_nMissing; }
    unsigned int Played() const  { return m_nPlayed; }

    // ⚠️ WHAT THE GAME ASKED FOR AND WHAT IT GOT. A headless run cannot be
    // listened to, so "the sound path works" is otherwise an untestable claim
    // -- and the sound path was silently doing nothing for months behind an
    // unwired function pointer. Names the last few misses, because "17
    // missing" is not something anyone can act on.
    void         DumpCensus() const;

    // ---- ILTSoundMgr ------------------------------------------------- //
    LTRESULT    PlaySound(PlaySoundInfo* pInfo, HLTSOUND& hResult) override;
    LTRESULT    GetSoundDuration(HLTSOUND hSound, LTFLOAT& fDuration) override;
    LTRESULT    IsSoundDone(HLTSOUND hSound, LTBOOL& bDone) override;

    // ⚠️ LIP SYNC'S TWO READS -- see CLTMixer::VoiceData. Offsets and sizes
    // are in SAMPLES (the game indexes the int16 array with them), and only
    // 16-bit data exists here: every WAV is decoded to shorts, so the 8-bit
    // pointer is always null, which the game handles.
    LTRESULT    GetSoundData(HLTSOUND hSound, short*& p16, signed char*& p8,
                             unsigned int* pRate, unsigned int* pChannels);
    LTRESULT    GetSoundOffset(HLTSOUND hSound, unsigned int* pOffset, unsigned int* pSize);
    LTRESULT    KillSound(HLTSOUND hSound) override;
    LTRESULT    KillSoundLoop(HLTSOUND hSound) override;

    // ---- ILTClientSoundMgr ------------------------------------------- //
    LTRESULT    GetSound3DProviderLists(Sound3DProvider*& pList, LTBOOL bVerify) override;
    LTRESULT    ReleaseSound3DProviderList(Sound3DProvider* pList) override;
    LTRESULT    InitSound(InitSoundInfo* pSoundInfo) override;
    LTRESULT    GetVolume(uint16& nVolume) override;
    LTRESULT    SetVolume(uint16 nVolume) override;
    LTRESULT    SetReverbProperties(ReverbProperties* p) override;
    LTRESULT    GetReverbProperties(ReverbProperties* p) override;
    LTRESULT    SetSoundOcclusion(HLTSOUND hSound, LTFLOAT fLevel) override;
    LTRESULT    GetSoundOcclusion(HLTSOUND hSound, LTFLOAT* pLevel) override;
    LTRESULT    SetSoundObstruction(HLTSOUND hSound, LTFLOAT fLevel) override;
    LTRESULT    GetSoundObstruction(HLTSOUND hSound, LTFLOAT* pLevel) override;
    LTRESULT    SetSoundFilter(HLTSOUND hSound, const char* pFilter) override;
    LTRESULT    SetSoundFilterParam(HLTSOUND hSound, const char* pParam, float f) override;
    LTRESULT    GetSoundFilterParam(HLTSOUND hSound, const char* pParam, float* pf) override;
    LTRESULT    GetFilterName(uint32 nIndex, const char** pFilter) override;
    LTRESULT    GetFilterParamName(uint32 nIndex, const char* pFilter, const char** pParam) override;
    LTRESULT    SetSoundPosition(HLTSOUND hSound, LTVector* pPos) override;
    LTRESULT    GetSoundPosition(HLTSOUND hSound, LTVector* pPos) override;
    LTRESULT    PauseSounds() override;
    LTRESULT    ResumeSounds() override;
    LTRESULT    SetListener(LTBOOL bListenerInClient, LTVector* pPos,
                            LTRotation* pRot, LTBOOL bTeleport) override;
    LTRESULT    ReleaseSoundHandle(HLTSOUND hSound) override;

private:

    struct Cached
    {
        std::vector<short>  m_Samples;
        unsigned int        m_nRate;
        unsigned int        m_nChannels;
    };

    const Cached*   Get(const char* pName);

    CLTFileMgr*                     m_pFileMgr;
    CLTMixer                        m_Mixer;
    std::map<std::string, Cached*>  m_Cache;
    unsigned int                    m_nDecoded;
    unsigned int                    m_nMissing;
    unsigned int                    m_nPlayed;
    unsigned int                    m_nDoneAsks;
    unsigned int                    m_nDoneYes;
    std::vector<std::string>        m_MissingNames;
    std::map<std::string, unsigned int> m_Played;
    bool                            m_bSaidUnsupported;
    bool                            m_bListenerInClient;
    float                           m_ListenerPos[3];
    float                           m_ListenerQuat[4];
};

#endif // __LT_SOUND_MGR_H__
