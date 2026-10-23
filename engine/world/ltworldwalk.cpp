// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldwalk.cpp
//
// See ltworldwalk.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltworldwalk.h"
#include "ltworldrender.h"
#include "ltworldscene.h"
#include "ltworldcollide.h"
#include "ltworlddoors.h"
#include "ltsurfaces.h"
#include "lt2d.h"
#include "ltrecord.h"
#include "ltfilemgr.h"
#include "iltstream.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

namespace
{
    // ⚠️ NOT OWNED HERE ANY MORE. The walk viewer used to own the world, which
    // was right while it was the only thing that drew one. The game's client
    // now draws the same level through ILTClient::RenderCamera, so the scene
    // moved to engine/world/ltworldscene.* and this is a reference to it.
    // Two copies would be two 2.3-million-triangle worlds and, worse, two
    // answers to "what surface is under this point".
    CLTWorldScene&  g_Scene = *LTGetWorldScene();
    bool            g_bActive   = false;
    char            g_Status[256] = { 0 };

    float           g_Eye[3]    = { 0, 0, 0 };
    float           g_Yaw       = 0.0f;
    float           g_Pitch     = 0.0f;

    // ⚠️ Indexed by Windows VK code, which is what the input layer delivers
    // (ltinput.cpp maps Mac scan codes to VK). 256 covers every code it emits.
    bool            g_Keys[256] = { false };

    std::vector<float>         g_Depth;
    std::vector<unsigned char> g_Colour;   // the supersampled target

    // Units per second. NOLF's world is roughly 1 unit = 1 inch, and a walking
    // pace of ~300 covers a corridor in a few seconds without overshooting a
    // doorway between frames.
    const float     kWalkSpeed  = 300.0f;
    const float     kFastSpeed  = 1100.0f;
    const float     kLookSpeed  = 1.6f;     // radians/second

    // ---- the body ------------------------------------------------------- //
    //
    // ⚠️ The camera is an EYE, the collision is a BODY, and they are not at the
    // same place. The sphere's centre sits a radius above the feet; the eye
    // sits higher still. Colliding the eye directly would let you push your
    // head through a doorway lintel and would stand you 48 units inside the
    // floor.
    const float     kRadius     = 24.0f;
    const float     kEyeAbove   = 34.0f;    // eye above the sphere centre
    const float     kGravity    = 1400.0f;  // units/s^2
    const float     kStepUp     = 22.0f;    // a stair riser, not a wall
    const float     kTermVel    = 3000.0f;

    CLTWorldCollide g_Collide;
    CLTWorldDoors   g_Doors;
    bool            g_bEDown    = false;    // edge-detect the activate key
    std::vector<float> g_DoorTris;

    // ⚠️ How close you must stand, measured from the BODY to the door brush's
    // centre. Not arm's length: a door is ~64 units wide and ~128 tall, so its
    // centre is already tens of units away when you are touching it. M01S01's
    // spawn faces HingedDoor0 whose centre is 130 units off -- with a 110-unit
    // reach the very first door in the game reported "nothing to open here",
    // which is how this number was found rather than guessed.
    const float     kReach      = 190.0f;

    // The transform source the renderer asks. It knows nothing about doors
    // beyond "this model has moved".
    struct DoorXform : public ILTWorldModelXform
    {
        virtual bool GetModelTransform(const std::string& sName, float* pOut16) const
        { return g_Doors.GetTransform(sName, pOut16); }
    };
    DoorXform       g_Xform;

    // Rebuilds the door triangles in world space. ⚠️ Only called when a door
    // actually moved -- doing it every frame would cost the same as a second
    // static build for no reason.
    void RebuildDoorCollision(const CLTWorld& world)
    {
        g_DoorTris.clear();
        const std::vector<LTWorldModelHeader>& mdl = world.GetModels();

        for (unsigned int d = 0; d < g_Doors.Count(); ++d)
        {
            float M[16];
            if (!g_Doors.GetTransform(g_Doors.Name(d), M)) continue;

            for (size_t i = 0; i < mdl.size(); ++i)
            {
                if (mdl[i].m_sName != g_Doors.Name(d) || !mdl[i].m_bBodyParsed) continue;
                const LTWorldModelHeader& W = mdl[i];

                for (size_t q = 0; q < W.m_Polys.size(); ++q)
                {
                    const LTWorldPoly& po = W.m_Polys[q];
                    if (po.m_Verts.size() < 3) continue;
                    if (po.m_nSurface >= W.m_Surfaces.size()) continue;
                    if (!(W.m_Surfaces[po.m_nSurface].m_nFlags & 1u)) continue;   // SOLID

                    for (size_t k = 1; k + 1 < po.m_Verts.size(); ++k)
                    {
                        const unsigned short idx[3] = { po.m_Verts[0].m_nPoint,
                                                        po.m_Verts[k].m_nPoint,
                                                        po.m_Verts[k+1].m_nPoint };
                        bool bOK = true;
                        for (int v = 0; v < 3 && bOK; ++v)
                            if (idx[v] >= W.m_Points.size()) bOK = false;
                        if (!bOK) continue;

                        for (int v = 0; v < 3; ++v)
                        {
                            float o[4];
                            LT3D::TransformPoint(M, W.m_Points[idx[v]].m_Pos, o);
                            g_DoorTris.push_back(o[0]);
                            g_DoorTris.push_back(o[1]);
                            g_DoorTris.push_back(o[2]);
                        }
                    }
                }
                break;
            }
        }
        g_Collide.SetDynamic(g_DoorTris);
    }
    bool            g_bNoClip   = false;
    bool            g_bGround   = false;
    float           g_VelY      = 0.0f;
    bool            g_bFDown    = false;    // edge-detect the noclip toggle

    // Mouse look. Deltas accumulate between frames; the frame consumes them.
    int             g_MouseDX   = 0;
    int             g_MouseDY   = 0;
    bool            g_bTabDown  = false;    // edge-detect the capture toggle

    // Radians per unit of mouse motion. ⚠️ Tuned so a comfortable hand movement
    // turns roughly 90 degrees, which is what makes a mouse better than the
    // arrow keys rather than merely different.
    const float     kMouseGain  = 0.0032f;

    // ⚠️ WALK MODE OWNS THIS FLAG, AND DOES NOT TOUCH THE WINDOW. Calling
    // LTWindow_SetMouseLook from here would drag Cocoa into every test that
    // links the camera -- and the walk suite links it WITHOUT the window layer.
    // That exact dependency mistake left this suite failing to build, unnoticed,
    // for a whole session (HANDOVER.md ss3a). The host reads the flag and drives
    // the window; the camera stays testable headlessly.
    bool            g_bMouseLook = true;

    LTWalkGpuRenderFn g_pGpuRender = 0;
    LTWalkSoundFn     g_pSound     = 0;

    // ---- footsteps ------------------------------------------------------ //
    //
    // ⚠️ THE SOUNDS AND THE SURFACE ARE BOTH THE GAME'S. `Attributes\\Surface.txt`
    // gives four footstep sounds per surface across 30 surfaces; the surface a
    // polygon has is its TEXTURE's m_UserFlags (see ltsurfaces.h for how that
    // was found and confirmed). So walking from carpet onto tile changes the
    // sound because the level says it should, not because anything here decides.
    CLTSurfaces     g_Surfaces;
    bool            g_bRightFoot = false;
    float           g_fSincePace = 0.0f;
    unsigned int    g_nFootVariant = 0;
    int             g_nLastSurface = -2;    // -2 = nothing reported yet

    // Units of travel between steps. A stride, not a tuning knob: NOLF's world
    // is roughly an inch per unit, so this is about a metre.
    const float     kStride     = 44.0f;

    bool Down(int vk) { return (vk >= 0 && vk < 256) ? g_Keys[vk] : false; }
}


bool LTWorldWalk_Init(CLTFileMgr* pFileMgr)
{
    const char* pWorld = getenv("LT_WORLD");
    if (!pWorld || !*pWorld) return false;

    // ⚠️ Loud, not silent. A world that will not load must not leave the host
    // quietly showing menus as though nothing had been asked for.
    // LTWorldScene_Load reports the reason; this adds the walk-mode framing.
    if (!LTWorldScene_Load(pFileMgr, pWorld))
    {
        snprintf(g_Status, sizeof(g_Status), "LT_WORLD=%s FAILED: %s",
                 pWorld, g_Scene.GetError());
        printf("[WALK] %s\n", g_Status);
        LTRecord_Event("walk", "load failed");
        return false;
    }

    // ⚠️ The up-front decode now happens inside LTWorldScene_Load, because the
    // game's client needs it just as much as the viewer does -- lazy decoding
    // costs 86 ms on M01S01's first frame against 20-36 ms after, and repeats
    // on entering any area whose textures are new.

    g_Doors.Build(g_Scene.GetWorld());

    // ⚠️ Doors come OUT of the static grid before it is built -- a grid keyed
    // on world-space cells cannot hold geometry that moves.
    {
        std::vector<std::string> names;
        for (unsigned int d = 0; d < g_Doors.Count(); ++d) names.push_back(g_Doors.Name(d));
        g_Collide.SetExcluded(names);
    }
    g_Collide.Build(g_Scene.GetWorld());
    RebuildDoorCollision(g_Scene.GetWorld());

    float pos[3], yaw = 0.0f;
    if (g_Scene.GetSpawn(pos, &yaw))
    {
        // The stored eye is the SPHERE CENTRE; the view is offset upward when
        // the frame is drawn.
        g_Eye[0] = pos[0];
        g_Eye[1] = pos[1] + kRadius;
        g_Eye[2] = pos[2];
        g_Yaw = yaw;

        // ⚠️ A spawn point is where the game puts the player's FEET, and a
        // sphere centred there is half buried. If the body starts inside
        // geometry the very first push-out throws it somewhere arbitrary, so
        // lift it until it is clear -- and say so if it never is, rather than
        // starting the session wedged in a wall.
        int nLift = 0;
        while (nLift < 16 && g_Collide.Overlaps(g_Eye, kRadius))
        { g_Eye[1] += kRadius * 0.5f; ++nLift; }
        if (nLift) printf("[WALK] spawn was inside geometry -- lifted %d step(s)\n", nLift);
    }
    g_Pitch = 0.0f;
    g_VelY = 0.0f;
    g_bGround = false;

    g_bActive = true;
    snprintf(g_Status, sizeof(g_Status),
             "%s -- spawn (%.0f %.0f %.0f)", pWorld, g_Eye[0], g_Eye[1], g_Eye[2]);

    printf("\n[WALK] %s\n", g_Status);
    printf("[WALK] W/A/S/D move, arrows look, Space jump, Shift run\n");
    printf("[WALK] E opens a door you are facing; F toggles noclip "
           "(then E/Q fly up/down)\n");
    printf("[WALK] mouse looks; TAB releases the pointer "
           "(you need it to move or close the window)\n");
    printf("[WALK] collision: %u static triangles in %u cells, %u door triangles\n",
           g_Collide.Triangles(), g_Collide.Cells(), g_Collide.DynamicTriangles());
    printf("[WALK] doors that open: %u  (E to open the nearest)\n", g_Doors.Count());
    fflush(stdout);

    LTRecord_Event("walk", "world loaded");
    return true;
}


bool LTWorldWalk_Active()
{
    return g_bActive;
}


void LTWorldWalk_SetGpuRenderer(LTWalkGpuRenderFn fn) { g_pGpuRender = fn; }
void LTWorldWalk_SetSoundPlayer(LTWalkSoundFn fn)     { g_pSound = fn; }


// ⚠️ The listener's RIGHT vector must be the SAME right the movement code uses
// -- the player's right in the WORLD, (cos yaw, 0, -sin yaw) for NOLF's
// left-handed axes. Deriving it independently here is how it inherited the
// camera's inverted right once already, and audio panning is the worst place
// for that: a sound on your left playing on your right is not obviously a SIGN
// error, it just sounds like the mix is wrong.
//
// ⚠️ Corrected 2026-09-09 with the camera; if one is ever flipped, flip both.
void LTWorldWalk_GetListener(float* pPos3, float* pRight3)
{
    if (pPos3)
    {
        pPos3[0] = g_Eye[0];
        pPos3[1] = g_Eye[1] + kEyeAbove;
        pPos3[2] = g_Eye[2];
    }
    if (pRight3)
    {
        pRight3[0] =  cosf(g_Yaw);
        pRight3[1] = 0.0f;
        pRight3[2] = -sinf(g_Yaw);
    }
}


void LTWorldWalk_SetMouseLook(bool b) { g_bMouseLook = b; }
bool LTWorldWalk_GetMouseLook()       { return g_bMouseLook; }


void LTWorldWalk_OnMouse(int nDX, int nDY)
{
    g_MouseDX += nDX;
    g_MouseDY += nDY;
}


void LTWorldWalk_OnKey(int nVK, bool bDown)
{
    if (nVK >= 0 && nVK < 256) g_Keys[nVK] = bDown;
}


void LTWorldWalk_GetPos(float* pPos3, float* pYaw)
{
    if (pPos3) { pPos3[0] = g_Eye[0]; pPos3[1] = g_Eye[1]; pPos3[2] = g_Eye[2]; }
    if (pYaw)  *pYaw = g_Yaw;
}


const char* LTWorldWalk_Status()
{
    return g_Status[0] ? g_Status : 0;
}


void LTWorldWalk_Frame(const LT2DSurf& surf, float fDelta)
{
    if (!g_bActive || !surf.m_pRGBA || surf.m_Width <= 0 || surf.m_Height <= 0)
        return;

    // ⚠️ Clamp the step. A frame that stalled (a texture decode, a breakpoint)
    // would otherwise teleport the camera across the level in one update.
    if (fDelta < 0.0f)   fDelta = 0.0f;
    if (fDelta > 0.10f)  fDelta = 0.10f;

    // ---- mouse look ------------------------------------------------------ //
    //
    // ⚠️ Tab releases the pointer, and it is not a nicety. While mouse look is
    // on the cursor is hidden and detached from the mouse, so without a way
    // back the window cannot be moved, resized or closed by hand.
    if (Down(VK_TAB) && !g_bTabDown)
    {
        g_bMouseLook = !g_bMouseLook;
        printf("[WALK] mouse look %s\n", g_bMouseLook ? "ON" : "OFF (Tab to grab)");
        fflush(stdout);
        g_MouseDX = g_MouseDY = 0;      // drop whatever the toggle itself moved
    }
    g_bTabDown = Down(VK_TAB);

    if (g_bMouseLook)
    {
        // ⚠️ Yaw uses the SAME sign convention as the arrow keys: moving the
        // mouse right must turn right, which is INCREASING yaw (see the `right`
        // vector below -- increasing yaw swings toward +right).
        g_Yaw   += (float)g_MouseDX * kMouseGain;
        g_Pitch -= (float)g_MouseDY * kMouseGain;
    }
    g_MouseDX = g_MouseDY = 0;

    // ---- look ----------------------------------------------------------- //
    //
    // ⚠️ THE SIGN HERE IS SET BY NOLF'S WORLD, NOT BY TASTE, AND IT WAS WRONG
    // FOR A REASON WORTH KEEPING (corrected 2026-09-09).
    //
    // NOLF's world is LEFT-handed: X right, Y up, Z forward. Facing yaw, `fwd`
    // is (sin, 0, cos) and the player's right hand points at (cos, 0, -sin) --
    // so increasing yaw turns RIGHT.
    //
    // ⚠️ These lines previously read the other way, and the comment that
    // justified it was confident and wrong. The cause was that the scene is
    // RENDERED MIRRORED (see LT_MIRROR): with the picture flipped, a correct
    // camera looks inverted to whoever is watching, so "fixing" the controls to
    // match the picture pushed the sign error into the CAMERA instead. Two
    // wrongs that agreed on screen and disagreed with the world.
    //
    // ⚠️ The direction tests could not catch it, and that is the general
    // lesson: they assert a SCREEN-space outcome, which is (camera sign) x
    // (mirror). Flipping both leaves them green. They are still worth having --
    // they catch one sign flipped alone -- but a screen-space test can never
    // separate a mirrored camera from a mirrored image. Only the WORLD can:
    // the level's own layout, checked against the real game.
    if (Down(VK_LEFT))  g_Yaw   -= kLookSpeed * fDelta;
    if (Down(VK_RIGHT)) g_Yaw   += kLookSpeed * fDelta;
    if (Down(VK_UP))    g_Pitch += kLookSpeed * fDelta;
    if (Down(VK_DOWN))  g_Pitch -= kLookSpeed * fDelta;

    // ⚠️ Stop just short of straight up. At exactly +/-90 degrees the view
    // direction becomes parallel to the up vector and LookAt's cross product
    // collapses, which does not crash -- it silently produces a degenerate
    // matrix and a black screen.
    const float kLimit = 1.5533f;           // 89 degrees
    if (g_Pitch >  kLimit) g_Pitch =  kLimit;
    if (g_Pitch < -kLimit) g_Pitch = -kLimit;

    // ---- move ----------------------------------------------------------- //
    float cp = cosf(g_Pitch), sp = sinf(g_Pitch);
    float fwd[3]   = { sinf(g_Yaw) * cp, sp, cosf(g_Yaw) * cp };

    // ⚠️ RIGHT IS THE PLAYER'S RIGHT IN THE WORLD, not whatever matches the
    // picture. NOLF's world is LEFT-handed (X right, Y up, Z forward), so
    // facing `fwd` = (sin, 0, cos) the right hand points at (cos, 0, -sin).
    // At yaw 0 that is +X, and +X is what the level calls right.
    //
    // ⚠️ This was `{ -cos, 0, sin }` -- the exact negation -- and it was
    // introduced to make D strafe toward the right of the SCREEN while the
    // screen was mirrored. It is the same error as the yaw sign above and has
    // the same cause; see the note there. Nothing local could catch it: both
    // vectors are unit, horizontal and perpendicular to fwd, so every property
    // except the sign is identical.
    float right[3] = {  cosf(g_Yaw),     0,  -sinf(g_Yaw)     };

    // F toggles noclip, on the PRESS rather than while held -- a held key
    // would flip it sixty times a second.
    if (Down('F') && !g_bFDown) { g_bNoClip = !g_bNoClip; g_VelY = 0.0f; }
    g_bFDown = Down('F');

    // ---- E opens the nearest door --------------------------------------- //
    if (Down('E') && !g_bEDown && !g_bNoClip)
    {
        bool bLocked = false;
        int n = g_Doors.Activate(g_Eye, kReach, &bLocked);
        if (n < 0)          printf("[WALK] nothing to open here\n");
        else if (bLocked)   printf("[WALK] \"%s\" is LOCKED\n", g_Doors.Name(n).c_str());
        else                printf("[WALK] \"%s\"\n", g_Doors.Name(n).c_str());

        // ⚠️ THE DOOR'S OWN SOUND, named by the level author -- OpenSound,
        // CloseSound and LockedSound are properties on the object. Choosing a
        // sound here instead would be inventing audio the game does not have.
        if (n >= 0 && g_pSound)
        {
            const std::string& s = bLocked
                ? g_Doors.LockedSound((unsigned int)n)
                : (g_Doors.IsOpening((unsigned int)n)
                       ? g_Doors.OpenSound((unsigned int)n)
                       : g_Doors.CloseSound((unsigned int)n));

            if (!s.empty())
            {
                float c[3]; g_Doors.Centre((unsigned int)n, c);
                float fOuter = g_Doors.SoundRadius((unsigned int)n);
                if (fOuter <= 0.0f) fOuter = 1500.0f;   // author left it unset
                g_pSound(s.c_str(), c, fOuter * 0.15f, fOuter);
            }
        }
        fflush(stdout);
    }
    g_bEDown = Down('E');

    g_Doors.Update(fDelta);
    if (g_Doors.Changed())
    {
        RebuildDoorCollision(g_Scene.GetWorld());
        g_Doors.ClearChanged();
    }

    // Where we were before this frame's move resolved -- the footstep pacing
    // below measures how far we actually got, not how far we asked to go.
    const float fPrevX = g_Eye[0], fPrevZ = g_Eye[2];

    float fSpeed = (Down(VK_SHIFT) ? kFastSpeed : kWalkSpeed) * fDelta;

    float wish[3] = { 0, 0, 0 };
    if (Down('W')) for (int k = 0; k < 3; ++k) wish[k] += fwd[k];
    if (Down('S')) for (int k = 0; k < 3; ++k) wish[k] -= fwd[k];
    if (Down('D')) for (int k = 0; k < 3; ++k) wish[k] += right[k];
    if (Down('A')) for (int k = 0; k < 3; ++k) wish[k] -= right[k];

    if (g_bNoClip)
    {
        if (Down('E')) wish[1] += 1.0f;
        if (Down('Q')) wish[1] -= 1.0f;
        for (int k = 0; k < 3; ++k) g_Eye[k] += wish[k] * fSpeed;
    }
    else
    {
        // ⚠️ Walking is HORIZONTAL. Looking up must not launch you into the
        // ceiling, so the movement vector is flattened and renormalised --
        // otherwise "forward" while looking at the floor drives you into it and
        // the collision spends every frame pushing you back out.
        wish[1] = 0.0f;
        float wl = sqrtf(wish[0]*wish[0] + wish[2]*wish[2]);
        if (wl > 1e-4f) { wish[0] /= wl; wish[2] /= wl; }

        // Jump, only with both feet down.
        if (Down(VK_SPACE) && g_bGround) { g_VelY = 520.0f; g_bGround = false; }

        // ⚠️ GRAVITY ONLY WHILE AIRBORNE, and this is not an optimisation --
        // it is what stops the body vibrating for ever. Accumulating gravity
        // while resting sinks the sphere ~0.4 units each frame, the resolver
        // pushes it back out, and the eye oscillates within half a unit
        // permanently. The test that watches twenty settled frames caught it;
        // a single frame-to-frame comparison would have passed by luck.
        if (g_bGround && g_VelY <= 0.0f)
        {
            g_VelY = 0.0f;
        }
        else
        {
            g_VelY -= kGravity * fDelta;
            if (g_VelY < -kTermVel) g_VelY = -kTermVel;
        }

        // ---- horizontal, with a step up ---------------------------------- //
        //
        // ⚠️ Without this a 6-inch kerb stops you dead: the sphere hits the
        // riser, slides along it and never rises. The trick is to try the move
        // lifted by a step height and drop back down; if that lands higher and
        // still clear, take it.
        float flat[3] = { wish[0] * fSpeed, 0.0f, wish[2] * fSpeed };

        float ground[3] = { g_Eye[0], g_Eye[1], g_Eye[2] };
        bool bG1 = false;
        g_Collide.MoveSphere(ground, flat, kRadius, &bG1);

        bool bUsedStep = false;
        if (g_bGround && (fabsf(flat[0]) > 1e-4f || fabsf(flat[2]) > 1e-4f))
        {
            float stepped[3] = { g_Eye[0], g_Eye[1] + kStepUp, g_Eye[2] };
            if (!g_Collide.Overlaps(stepped, kRadius))
            {
                bool bG2 = false;
                g_Collide.MoveSphere(stepped, flat, kRadius, &bG2);
                float drop[3] = { 0.0f, -kStepUp, 0.0f };
                g_Collide.MoveSphere(stepped, drop, kRadius, &bG2);

                // Only if it actually got further along the wish direction.
                float a = (ground[0]-g_Eye[0])*wish[0] + (ground[2]-g_Eye[2])*wish[2];
                float b = (stepped[0]-g_Eye[0])*wish[0] + (stepped[2]-g_Eye[2])*wish[2];
                if (b > a + 0.5f)
                {
                    memcpy(g_Eye, stepped, sizeof(g_Eye));
                    g_bGround = bG2 || g_bGround;
                    bUsedStep = true;
                }
            }
        }
        if (!bUsedStep) { memcpy(g_Eye, ground, sizeof(g_Eye)); g_bGround = bG1; }

        // ---- vertical ----------------------------------------------------- //
        //
        // ⚠️ Skipped entirely while resting. A zero-length move still costs a
        // grid query, and more importantly a body at rest must not be asked to
        // move at all -- every such move is another chance to be nudged.
        if (g_bGround && g_VelY == 0.0f)
        {
            // Standing. Walking off an edge clears g_bGround in the horizontal
            // move above, and gravity resumes on the next frame.
        }
        else
        {
        float fall[3] = { 0.0f, g_VelY * fDelta, 0.0f };
        bool bG3 = false;
        g_Collide.MoveSphere(g_Eye, fall, kRadius, &bG3);
        if (bG3)
        {
            // ⚠️ Landing must ZERO the velocity, not merely stop the move. A
            // body that keeps accumulating downward speed while resting will
            // punch through the floor the moment a frame runs long.
            if (g_VelY < 0.0f) g_VelY = 0.0f;
            g_bGround = true;
        }
        else if (g_VelY > 0.0f)
        {
            g_bGround = false;
        }
        }   // airborne
    }

    // ⚠️ The EYE is above the body's centre -- see kEyeAbove.
    // ---- footsteps ------------------------------------------------------ //
    //
    // ⚠️ PACED BY DISTANCE, not by time. A time-based step keeps ticking while
    // you walk into a wall and speeds up when you sprint by exactly the wrong
    // amount; distance gives a stride that is right at any speed and stops the
    // instant you actually stop.
    //
    // ⚠️ Only with both feet on the ground: no footsteps mid-jump or noclipping.
    if (g_pSound && g_Surfaces.IsLoaded() && g_bGround && !g_bNoClip)
    {
        const float dx = g_Eye[0] - fPrevX;
        const float dz = g_Eye[2] - fPrevZ;
        g_fSincePace += sqrtf(dx*dx + dz*dz);

        if (g_fSincePace >= kStride)
        {
            g_fSincePace = 0.0f;
            g_bRightFoot = !g_bRightFoot;
            if (g_bRightFoot) ++g_nFootVariant;      // alternate the pair

            // ⚠️ Asked PER STEP, not per frame: the ray walks every drawn
            // polygon, which is fine twice a second and not sixty times.
            const int nSurf = g_Scene.SurfaceUnder(g_Eye[0], g_Eye[2],
                                                   g_Eye[1], 256.0f);

            if (nSurf != g_nLastSurface)
            {
                g_nLastSurface = nSurf;
                const CLTSurfaces::Surface* p = g_Surfaces.Find(nSurf);
                printf("[WALK] now standing on: %s (id %d)\n",
                       p && !p->m_sName.empty() ? p->m_sName.c_str() : "Unknown", nSurf);
                fflush(stdout);
            }

            const char* pSnd = g_Surfaces.Footstep(nSurf, g_bRightFoot, g_nFootVariant);

            // ⚠️ LOCAL, not 3D. Your own feet are not somewhere else in the
            // room -- positioning them makes them pan as you turn.
            if (pSnd && *pSnd) g_pSound(pSnd, 0, 0.0f, 0.0f);
        }
    }

    float eye[3] = { g_Eye[0], g_Eye[1] + kEyeAbove, g_Eye[2] };
    float at[3]  = { eye[0] + fwd[0], eye[1] + fwd[1], eye[2] + fwd[2] };

    // ---- supersampling, and WHY ----------------------------------------- //
    //
    // ⚠️ The corridor floor tore into a harsh white stipple at grazing angles,
    // and it was diagnosed by elimination rather than guessed at:
    //
    //   not a hole      -- painting the background magenta left 0 magenta pixels
    //   not mip choice  -- identical with the chain disabled
    //   not two models  -- identical with only the MAINWORLD model drawn
    //   not backfaces   -- identical with culling on (which removed real walls)
    //   not z-fighting  -- a relative depth bias changed nothing
    //   not the light scale -- 1/255 instead of 1/128 changed nothing
    //
    // ⚠️ What DID change it: drawing every surface in one flat colour made the
    // pattern almost vanish. So the geometry and depth are right and the
    // artefact is ALIASING -- the floor is tessellated into many small
    // polygons whose baked vertex lighting differs sharply (48 and 227 both
    // occur on the corridor floor), and at a grazing angle each covers a
    // pixel or two. One sample per pixel cannot represent that.
    //
    // So the fix is more samples, not a different formula. Rendering at twice
    // the linear size and box-filtering down costs 4x the fill and removes it.
    //
    // ⚠️ This lives HERE and not in lt3d, because the rasteriser is what the
    // frame contract measures. The walk path supersamples; the game's own
    // frames are produced exactly as before.
    // ⚠️ ONE sample per pixel by default, and that is a REVERSAL. Supersampling
    // was added when the corridor floor was tearing, before the cause was
    // found; the real fault was perspective-correct interpolation of the
    // vertex colour, and fixing that dropped the floor's high-frequency energy
    // from 35.23 to 14.05 on its own. 2x costs 3.5x the frame time -- 12 fps
    // against 44 -- to take that 14.05 down to 9.50, which is not a trade
    // worth making for a camera you are walking around with.
    //
    // ⚠️ Kept as a switch rather than deleted: it is the honest way to see how
    // much shimmer is left. LT_WALK_SS=2 restores it.
    // ⚠️ THE GPU PATH IS OPT-IN AND RETURNS EARLY. It presents its own frame
    // through its own layer, so nothing below runs: no supersample buffer, no
    // downsample, and no LTWindow_Present. Leaving the software blit to also
    // run would fight the GPU for the same view.
    if (g_pGpuRender &&
        g_pGpuRender(g_Scene, surf.m_Width, surf.m_Height, eye, at, 1.4f, &g_Xform))
        return;

    static const int SS = getenv("LT_WALK_SS") ? atoi(getenv("LT_WALK_SS")) : 1;
    const int RW = surf.m_Width * SS, RH = surf.m_Height * SS;

    size_t nBig = (size_t)RW * (size_t)RH;
    if (g_Depth.size() != nBig) g_Depth.assign(nBig, 0.0f);
    if (g_Colour.size() != nBig * 4) g_Colour.assign(nBig * 4, 0);

    g_Scene.Render(&g_Colour[0], &g_Depth[0], RW, RH, eye, at, 1.4f, &g_Xform);

    for (int y = 0; y < surf.m_Height; ++y)
    {
        unsigned char* pDst = surf.m_pRGBA + (size_t)y * surf.m_Width * 4;
        for (int x = 0; x < surf.m_Width; ++x)
        {
            int acc[4] = { 0, 0, 0, 0 };
            for (int sy = 0; sy < SS; ++sy)
            {
                const unsigned char* pSrc =
                    &g_Colour[(((size_t)(y*SS + sy) * RW) + (size_t)x*SS) * 4];
                for (int sx = 0; sx < SS; ++sx, pSrc += 4)
                    for (int k = 0; k < 4; ++k) acc[k] += pSrc[k];
            }
            for (int k = 0; k < 4; ++k) pDst[k] = (unsigned char)(acc[k] / (SS*SS));
            pDst += 4;
        }
    }
}
