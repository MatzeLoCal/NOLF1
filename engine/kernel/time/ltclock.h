// ----------------------------------------------------------------------- //
//
// MODULE  : ltclock.h
//
// PURPOSE : The engine clock behind ILTCSBase::GetTime and GetFrameTime.
//
// WHY THIS TURNED UP WHERE IT DID
//
//   The 2D compositor was finished, correct, and unit-tested, and the screen
//   still dumped as pure black. The compositor was not at fault: the interface
//   manager fades in by scaling a 2x2 surface over the whole screen with a
//   colour key of white, and at t=0 that surface is black. With GetTime stuck
//   at 0 the fade never advances, so every frame is a full-screen black
//   overlay -- drawn perfectly.
//
//   ⚠️ A renderer can be entirely right and still produce a black screen. It
//   took an instrument that reported WHICH surfaces each draw touched, the key
//   colour, and how much of the screen was lit before each op, to tell the two
//   apart. Guessing would have gone straight into the blitter.
//
// ⚠️ FIXED TIMESTEP BY DEFAULT, AND THAT IS A CONTRACT DECISION
//
//   A wall clock makes every run different, which would destroy the frame
//   contract the moment time reached anything visible -- fades, animations,
//   anything eased. So the default is a FIXED step advanced once per frame by
//   the host: run N frames and you get exactly the same pixels every time.
//
//   LT_TIMESTEP=<seconds>  changes the step (default 1/60).
//   LT_REALTIME=1          uses the wall clock instead. ⚠️ This makes runs
//                          non-reproducible; the harness must never set it.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_CLOCK_H__
#define __LT_CLOCK_H__

class CLTClock
{
public:

    CLTClock();

    // Reads LT_TIMESTEP / LT_REALTIME. Safe to call more than once.
    void        Init();

    // Advances one frame. In fixed mode this is the only thing that moves time,
    // which is what makes a run reproducible.
    void        Tick();

    float       GetTime() const      { return m_Time; }
    float       GetFrameTime() const { return m_FrameTime; }

    bool        IsRealTime() const   { return m_bRealTime; }
    float       GetStep() const      { return m_Step; }
    unsigned    GetFrames() const    { return m_nFrames; }
    double      StartWall() const    { return m_StartWall; }

private:

    bool        m_bInited;
    bool        m_bRealTime;
    float       m_Step;
    float       m_Time;
    float       m_FrameTime;
    unsigned    m_nFrames;
    double      m_StartWall;
};

CLTClock* LTGetClock();

// Seconds of WALL time since the clock was initialised, whichever mode it is
// in. The point of comparison for GetTime: see the [CLOCK] census.
double LTClock_WallElapsed();

#endif // __LT_CLOCK_H__
