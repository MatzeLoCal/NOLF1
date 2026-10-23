// ------------------------------------------------------------------------- //
//
// ltdmmgr.h -- ILTDirectMusicMgr: LithTech's music manager over the port's
// own performance (HANDOVER 5cj).
//
// The manager is the donor's CLTDirectMusicMgr (Jupiter EX
// ltdirectmusic_impl.cpp) with DirectMusic replaced by CLTDMPerformance:
//   * a level's CONTROL FILE (Music\<dir>\<name>.txt) lists styles, bands,
//     intensities (segments, loop count, the intensity to go to after), the
//     transition matrix, secondary segments and motifs;
//   * ChangeIntensity plays the transition segment (or the intensity's first
//     segment) at the transition's boundary and QUEUES the rest; each time
//     the newest primary is about to end, the queue advances -- next segment
//     at the segment end, loop back, or change intensity;
//   * motifs and secondary segments play over the top.
//
// The music thread renders ahead into a ring the mixer drains; the mixer's
// own pause does not stop music (DirectMusic was never under DirectSound's
// pause either -- the game pauses music itself).
//
// ⚠️ Every entry point takes the PERFORMANCE's lock -- one lock, see
// ltdmperf.h.
//
// ------------------------------------------------------------------------- //

#ifndef __LTDMMGR_H__
#define __LTDMMGR_H__

#include "windows.h"
#include "lithtech.h"
#include "iltdirectmusic.h"
#include "ltdmperf.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

class CLTDMMusicMgr : public ILTDirectMusicMgr
{
public:
    CLTDMMusicMgr();
    virtual ~CLTDMMusicMgr();

    // ---- ILTDirectMusicMgr --------------------------------------------- //
    virtual LTRESULT Init();
    virtual LTRESULT Term();
    virtual LTRESULT InitLevel(const char* sWorkingDirectory, const char* sControlFileName,
                               const char* sDefine1 = NULL, const char* sDefine2 = NULL,
                               const char* sDefine3 = NULL);
    virtual LTRESULT TermLevel();
    virtual LTRESULT Play();
    virtual LTRESULT Stop(const LTDMEnactTypes nStart = LTDMEnactDefault);
    virtual LTRESULT Pause(const LTDMEnactTypes nStart = LTDMEnactDefault);
    virtual LTRESULT UnPause();
    virtual LTRESULT SetVolume(const long nVolume);
    virtual LTRESULT ChangeIntensity(const int nNewIntensity, const LTDMEnactTypes nStart = LTDMEnactInvalid);
    virtual LTRESULT PlaySecondary(const char* sSecondarySegment, const LTDMEnactTypes nStart = LTDMEnactDefault);
    virtual LTRESULT StopSecondary(const char* sSecondarySegment = NULL, const LTDMEnactTypes nStart = LTDMEnactDefault);
    virtual LTRESULT PlayMotif(const char* sStyleName, const char* sMotifName, const LTDMEnactTypes nStart = LTDMEnactDefault);
    virtual LTRESULT StopMotif(const char* sStyleName, const char* sMotifName = NULL, const LTDMEnactTypes nStart = LTDMEnactDefault);
    virtual int GetCurIntensity();
    virtual LTDMEnactTypes StringToEnactType(const char* sName);
    virtual void EnactTypeToString(LTDMEnactTypes nType, char* sName);
    virtual int GetNumIntensities();
    virtual int GetInitialIntensity();
    virtual long GetInitialVolume();

    // ---- host ---------------------------------------------------------- //
    CLTDMPerformance& Performance()     { return m_Perf; }
    // Renders straight from the performance (offline tools).
    void    Render(float* pLR, unsigned int nFrames)  { m_Perf.Render(pLR, nFrames); }
    // The music thread: renders ahead into the ring while pfnActive says
    // someone is draining it. Pull adds up to nFrames from the ring to pLR
    // (the mixer's callback; lock-free, never blocks, silence on underrun).
    bool    StartThread(bool (*pfnActive)());
    void    StopThread();
    void    Pull(float* pLR, unsigned int nFrames);

    // Diagnostics.
    unsigned int NumCommands() const    { return (unsigned int)m_Cmds.size(); }
    int     PrevIntensity() const       { return m_nPrevIntensity; }

private:
    struct Intensity
    {
        int m_nLoops, m_nNext;
        std::vector<const LTDMSegment*> m_Segs;
        Intensity() : m_nLoops(0), m_nNext(0) {}
    };
    struct Transition
    {
        LTDMEnactTypes      m_eEnact;
        bool                m_bManual;
        const LTDMSegment*  m_pSeg;
    };
    struct Named            // a loaded segment or a motif, with its default enact
    {
        std::string         m_sName;
        const LTDMSegment*  m_pSeg;
        const LTDMStyle*    m_pStyle;
        const LTDMPattern*  m_pPattern;
        LTDMEnactTypes      m_eEnact;
    };
    enum CmdKind { kCmdPlaySegment, kCmdLoopToStart, kCmdChangeIntensity, kCmdPlaySecondary, kCmdPlayMotif };
    struct Command
    {
        CmdKind             m_eKind;
        const LTDMSegment*  m_pSeg;
        int                 m_n;        // loops left, or the intensity
        const Named*        m_pNamed;
    };
    struct Playing { unsigned int m_nId; const Named* m_pNamed; };

    static void NotifyThunk(void* pUser, CLTDMPerformance::Notify e, unsigned int nId);
    void    OnNotify(CLTDMPerformance::Notify e, unsigned int nId);
    void    OnAlmostEnd(unsigned int nId);
    CLTDMPerformance::Enact ToEnact(LTDMEnactTypes e) const;
    const Transition& GetTransition(int nFrom, int nTo) const;
    Named*  FindSegment(const char* pName);
    Named*  FindMotif(const char* pName);
    void    ClearCommands();
    void    ThreadMain();

    CLTDMPerformance                    m_Perf;
    bool                                m_bInitialized, m_bLevelInitialized;
    int                                 m_nNumIntensities, m_nInitialIntensity;
    long                                m_nInitialVolume, m_nVolumeOffset;
    int                                 m_nCurIntensity, m_nPrevIntensity;
    int                                 m_nPaused;
    std::vector<Intensity>              m_Intensities;      // [0] unused
    std::vector<Transition>             m_Transitions;      // (from, to) -> index from * (n+1) + to
    Transition                          m_DefaultTransition;
    std::vector<Named*>                 m_Segments;         // every segment the file named
    std::vector<Named*>                 m_MotifList;
    std::vector<Command>                m_Cmds;
    int                                 m_nLast;            // the command last executed, -1 = none
    std::vector<Command>                m_Cmds2;            // played when the next segment starts
    std::vector<unsigned int>           m_Primaries;        // newest LAST
    std::vector<Playing>                m_Secondaries, m_Motifs;

    // ---- the music thread and its ring --------------------------------- //
    enum { kRingFrames = 4096, kBlockFrames = 512 };
    std::thread                         m_Thread;
    std::atomic<bool>                   m_bRun;
    std::atomic<bool>                   m_bPulling;
    bool                                (*m_pfnActive)();
    float                               m_Ring[kRingFrames * 2];
    std::atomic<uint32_t>               m_nWrite, m_nRead;  // frame counters
};

// The engine's one manager (skeleton wiring, the mixer's pull).
CLTDMMusicMgr* LTDM_GetMusicMgr();
// How the manager reads music files (the engine: through the file manager).
void LTDM_SetDefaultReader(CLTDMPerformance::ReadFn fn);

#endif // __LTDMMGR_H__
