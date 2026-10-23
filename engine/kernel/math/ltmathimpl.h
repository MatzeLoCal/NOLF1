// ----------------------------------------------------------------------- //
//
// MODULE  : ltmathimpl.h
//
// PURPOSE : The rotation maths the game asks the engine for.
//
// ⚠️ THESE ARE NOT A CONVENIENCE. THEY DECIDE WHERE EVERY MENU OBJECT GOES.
//
//   `CBaseFolder::CreateInterfaceSFX` (BaseFolder.cpp:2078) does
//
//       GetObjectPos(hCamera, &g_vPos);
//       GetObjectRotation(hCamera, &g_rRot);
//       GetRotationVectors(&g_rRot, &g_vU, &g_vR, &g_vF);
//
//   and then computes EVERY interface object's position from `g_vU`, `g_vR`
//   and `g_vF`. Those three are file-scope globals, so while
//   `GetRotationVectors` was a stub they held uninitialised memory and every
//   menu FX was placed at a garbage position -- x around 1e30, y and z NaN.
//   Nothing complained; the objects simply never landed on screen.
//
//   ⚠️ The same shape appears in `CBaseFolder::CreateScaleFX`:
//
//       LTVector vNewPos;                       // uninitialised
//       GetObjectPos(hSFX, &vNewPos);           // if this does nothing...
//       vNewPos.z *= GetXRatio();               // ...garbage is scaled...
//       SetObjectPos(hSFX, &vNewPos);           // ...and stored.
//
//   The game gives these calls NO fallback, so a silent no-op is not a
//   degraded result -- it is corruption.
//
// ⚠️ THE PARAMETER ORDER DIFFERS BETWEEN INTERFACES, AND THE SDK SAYS SO
//
//   iltcsbase.h:210 carries the warning in capitals: "NOTE: PARAMETER
//   ORDERINGS ARE DIFFERENT FOR SOME OF THE FUNCTIONS."
//
//       ILTCSBase::GetRotationVectors(rot, UP,    RIGHT, forward)
//       ILTMath  ::GetRotationVectors(rot, RIGHT, UP,    forward)
//
//   Up and right are swapped. The game calls the ILTCSBase one and names its
//   arguments g_vU, g_vR, g_vF, which matches. Implementing it to the ILTMath
//   order would mirror the entire menu layout about its diagonal -- a wrong
//   picture, not a crash.
//
// THE CONVENTION COMES FROM THE ORIGINAL SDK, NOT FROM US
//
//   `quat_GetVectors` in `LT2/sdk/inc/ltquatbase.cpp` is Monolith's own code
//   and is already in the build, so the basis extraction is taken from it
//   rather than re-derived. LTRotation's identity "faces down positive Z"
//   (ltrotation.h:42), and quat_GetVectors returns forward (0,0,1) for it --
//   the two agree, which is the check worth having.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_MATH_IMPL_H__
#define __LT_MATH_IMPL_H__

namespace LTMathImpl
{
    // All quaternions are four floats in (x, y, z, w) order -- LTRotation's
    // own layout, and quat_GetVectors' too.

    void    GetVectors(const float* pQuat, float* pRight, float* pUp, float* pForward);

    // Rotates pQuat about a WORLD-space axis by `amount` radians.
    //
    // ⚠️ Pre-multiplied (q = axis * q), which is what "around a world axis"
    // means; post-multiplying would rotate about the object's own axis. The
    // menu camera is at identity today so both give the same answer -- which
    // is precisely why this is written down rather than left to be discovered
    // when a camera finally has a rotation.
    void    RotateAroundAxis(float* pQuat, const float* pAxis3, float amount);

    void    SetupEuler(float* pQuat, float pitch, float yaw, float roll);

    // Builds a rotation whose forward is vDir and whose up is as close to vUp
    // as possible.
    void    AlignRotation(float* pQuat, const float* pDir3, const float* pUp3);

    void    Slerp(float* pDest, const float* pA, const float* pB, float t);
}

#endif // __LT_MATH_IMPL_H__
