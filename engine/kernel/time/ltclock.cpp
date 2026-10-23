// ----------------------------------------------------------------------- //
//
// MODULE  : ltclock.cpp
//
// PURPOSE : The engine clock. See ltclock.h for why it defaults to a fixed
//           timestep rather than the wall clock.
//
// ----------------------------------------------------------------------- //

#include "windows.h"

#include "ltclock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static double WallSeconds()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

CLTClock::CLTClock()
    : m_bInited(false), m_bRealTime(false), m_Step(1.0f / 60.0f)
    , m_Time(0.0f), m_FrameTime(0.0f), m_nFrames(0), m_StartWall(0.0)
{
}

static DWORD ClockTicks(void);

void CLTClock::Init()
{
    if (m_bInited) return;
    m_bInited = true;

    const char* pStep = getenv("LT_TIMESTEP");
    if (pStep && pStep[0])
    {
        float f = (float)atof(pStep);
        // A zero or negative step would freeze time again, which is the exact
        // failure this class exists to fix -- refuse it rather than honour it.
        if (f > 0.0f) m_Step = f;
        else fprintf(stderr, "[LTCLOCK] ⚠️  ignoring LT_TIMESTEP=%s\n", pStep);
    }

    // ⚠️ "LT_REALTIME=0" MEANS OFF. The harness has to be able to pin the
    // fixed step even when the launcher it runs asks for real time, and
    // `getenv() != 0` alone would read the string "0" as yes.
    const char* pReal = getenv("LT_REALTIME");
    m_bRealTime = pReal && pReal[0] && strcmp(pReal, "0") != 0;
    m_StartWall = WallSeconds();

    if (m_bRealTime)
        fprintf(stderr, "[LTCLOCK] ⚠️  REAL TIME -- this run is NOT reproducible\n");
    else
    {
        // ⚠️ THE SECOND CLOCK. GetTickCount is a separate wall clock that
        // NOLF drives real animation from -- CWinUtil::GetTime() is
        // GetTickCount()/1000 (WinUtil.cpp:128) and CBaseScaleFX phases every
        // interface effect on it (BaseScaleFX.cpp:230, :261). Leaving it on
        // wall time made the main menu's rotating flower land at a different
        // angle every run: three distinct frames out of six, up to 38% of
        // pixels differing, while the renderer itself was perfectly
        // deterministic. Point it at the fixed timestep too.
        LTCompat_SetTickSource(ClockTicks);
    }
}

// Milliseconds of SIMULATED time, for GetTickCount.
static DWORD ClockTicks(void)
{
    return (DWORD)(LTGetClock()->GetTime() * 1000.0f);
}

void CLTClock::Tick()
{
    ++m_nFrames;

    if (m_bRealTime)
    {
        float now = (float)(WallSeconds() - m_StartWall);
        m_FrameTime = now - m_Time;
        if (m_FrameTime < 0.0f) m_FrameTime = 0.0f;

        // ⚠️ THE LONG FRAME IS CAPPED, and the cap is not tuning.
        //
        // Loading a level, or the first frame after a window opens, takes
        // seconds. Handing that to the game as one frame's worth of time moves
        // the player several rooms in a single step, and a sweep that long
        // passes straight through walls. Cap it and let simulated time fall
        // behind the wall clock instead -- slow is recoverable, through a
        // wall is not.
        //
        // ⚠️ AND TIME IS THEN ADVANCED BY THE CAPPED AMOUNT, not to `now`.
        // Setting m_Time = now while reporting a smaller frame time would
        // make GetTime and the sum of GetFrameTime disagree, and the game
        // uses both -- CBaseScaleFX phases on absolute time while movement
        // integrates the delta.
        const float fMaxFrame = 0.1f;               // 10 fps floor
        if (m_FrameTime > fMaxFrame) m_FrameTime = fMaxFrame;
        m_Time += m_FrameTime;
        (void)now;
    }
    else
    {
        m_FrameTime = m_Step;
        m_Time += m_Step;
    }
}

static CLTClock g_Clock;
CLTClock* LTGetClock() { return &g_Clock; }

double LTClock_WallElapsed() { return WallSeconds() - g_Clock.StartWall(); }
