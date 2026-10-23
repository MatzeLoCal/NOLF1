// ----------------------------------------------------------------------- //
//
// MODULE  : ltmathlt.cpp
//
// PURPOSE : ILTMath -- the out-of-line definitions of the interface itself.
//
// ⚠️ WHY THESE ARE BASE-CLASS METHODS AND NOT AN OVERRIDE
//
//   ILTCSBase holds `ILTMath m_MathLT` BY VALUE (iltcsbase.h:235) and
//   GetMathLT() returns its address. So the game's g_pMathLT is literally this
//   base object, on client and server alike, and no subclass can be swapped in.
//   Its methods were the generator's stubs.
//
// ⚠️ WHAT THAT COST: NPCs RENDERED AS FLAT DISCS
//
//   The AI build their facing through these calls into an UNINITIALISED
//   LTRotation. A stub writes nothing, so every character's rotation was stack
//   garbage -- and garbage is almost never a unit quaternion. A non-unit
//   quaternion does not merely turn a body, it scales and shears it. Measured:
//   every character skeleton was upright and correctly proportioned in model
//   space (Y span ~90), and on screen the receptionist was a horizontal disc
//   with her face smeared across it. The sixth instance of the uninitialised-
//   out-parameter bug in this port.
//
// ⚠️ ONE IMPLEMENTATION -- LTMathImpl, the code the client's ILTCSBase
// rotation calls already use -- so a yaw means the same thing on both halves.
//
// ⚠️ PARAMETER ORDER: ILTMath::GetRotationVectors is (RIGHT, UP, FORWARD).
// ILTCSBase and ILTCommon are (UP, RIGHT, FORWARD). iltcsbase.h:210 warns
// about this in capitals; it is spelled out below rather than shared.
//
// LTMatrix is m[row][col] with translation in column 3 (ltmatrix.h Apply) --
// the same layout LT3D uses, so the conversion is an index copy.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "iltmath.h"
#include "ltmathimpl.h"
#include "ltquatbase.h"
#include "ltstub.h"

#include <math.h>

namespace
{
    inline void Q(const LTRotation& r, float* q)
    { q[0] = r.m_Quat[0]; q[1] = r.m_Quat[1]; q[2] = r.m_Quat[2]; q[3] = r.m_Quat[3]; }

    inline void R(LTRotation& r, const float* q) { r.Init(q[0], q[1], q[2], q[3]); }

    // ⚠️ MONOLITH'S OWN CONVERSIONS (LT2/sdk/inc/ltquatbase.cpp), the same
    // source LTMathImpl::GetVectors uses, so matrix and vector forms of one
    // rotation cannot disagree about handedness.
    void ToMatrix(const float* q, const float* t, LTMatrix& m)
    {
        quat_ConvertToMatrix(q, m.m);
        m.m[0][3] = t[0]; m.m[1][3] = t[1]; m.m[2][3] = t[2];
    }

    void FromMatrix(const LTMatrix& mm, float* q)
    {
        quat_ConvertFromMatrix(q, mm.m);
    }

    const float kZero3[3] = { 0.0f, 0.0f, 0.0f };
}

LTRESULT ILTMath::GetRotationVectors(LTRotation &rot, LTVector &right, LTVector &up, LTVector &forward)
{
    LTIMPL("ILTMath::GetRotationVectors");
    float q[4], r[3], u[3], f[3];
    Q(rot, q);
    LTMathImpl::GetVectors(q, r, u, f);
    right.Init(r[0], r[1], r[2]);
    up.Init(u[0], u[1], u[2]);
    forward.Init(f[0], f[1], f[2]);
    return LT_OK;
}

LTRESULT ILTMath::InterpolateRotation(LTRotation &rDest, LTRotation &rot1, LTRotation &rot2, float t)
{
    LTIMPL("ILTMath::InterpolateRotation");
    float a[4], b[4], o[4];
    Q(rot1, a); Q(rot2, b);
    LTMathImpl::Slerp(o, a, b, t);
    R(rDest, o);
    return LT_OK;
}

LTRESULT ILTMath::SetupEuler(LTRotation &rRot, float pitch, float yaw, float roll)
{
    LTIMPL("ILTMath::SetupEuler");
    float q[4];
    LTMathImpl::SetupEuler(q, pitch, yaw, roll);
    R(rRot, q);
    return LT_OK;
}

LTRESULT ILTMath::SetupTransformationMatrix(LTMatrix &mMat, LTVector &vTranslation, LTRotation &rRotation)
{
    LTIMPL("ILTMath::SetupTransformationMatrix");
    float q[4], t[3] = { vTranslation.x, vTranslation.y, vTranslation.z };
    Q(rRotation, q);
    ToMatrix(q, t, mMat);
    return LT_OK;
}

LTRESULT ILTMath::SetupTranslationMatrix(LTMatrix &mMat, LTVector &vTranslation)
{
    LTIMPL("ILTMath::SetupTranslationMatrix");
    const float q[4] = { 0, 0, 0, 1 };
    float t[3] = { vTranslation.x, vTranslation.y, vTranslation.z };
    ToMatrix(q, t, mMat);
    return LT_OK;
}

LTRESULT ILTMath::SetupRotationMatrix(LTMatrix &mMat, LTRotation &rRot)
{
    LTIMPL("ILTMath::SetupRotationMatrix");
    float q[4];
    Q(rRot, q);
    ToMatrix(q, kZero3, mMat);
    return LT_OK;
}

LTRESULT ILTMath::SetupTranslationFromMatrix(LTVector &vTranslation, LTMatrix &mMat)
{
    LTIMPL("ILTMath::SetupTranslationFromMatrix");
    vTranslation.Init(mMat.m[0][3], mMat.m[1][3], mMat.m[2][3]);
    return LT_OK;
}

LTRESULT ILTMath::SetupRotationFromMatrix(LTRotation &rRot, LTMatrix &mMat)
{
    LTIMPL("ILTMath::SetupRotationFromMatrix");
    float q[4];
    FromMatrix(mMat, q);
    R(rRot, q);
    return LT_OK;
}

// Rotation about vPoint: translate to the origin, rotate, translate back.
LTRESULT ILTMath::SetupRotationAroundPoint(LTMatrix &mMat, LTRotation &rRot, LTVector &vPoint)
{
    LTIMPL("ILTMath::SetupRotationAroundPoint");
    float q[4];
    Q(rRot, q);
    ToMatrix(q, kZero3, mMat);
    const float p[3] = { vPoint.x, vPoint.y, vPoint.z };
    for (int r = 0; r < 3; ++r)
        mMat.m[r][3] = p[r] - (mMat.m[r][0]*p[0] + mMat.m[r][1]*p[1] + mMat.m[r][2]*p[2]);
    return LT_OK;
}

LTRESULT ILTMath::AlignRotation(LTRotation &rOutRot, LTVector &vVector, LTVector &vUp)
{
    LTIMPL("ILTMath::AlignRotation");
    float q[4], d[3] = { vVector.x, vVector.y, vVector.z }, u[3] = { vUp.x, vUp.y, vUp.z };
    LTMathImpl::AlignRotation(q, d, u);
    R(rOutRot, q);
    return LT_OK;
}

LTRESULT ILTMath::RotateAroundAxis(LTRotation &rRot, LTVector &vAxis, float amount)
{
    LTIMPL("ILTMath::RotateAroundAxis");
    float q[4], a[3] = { vAxis.x, vAxis.y, vAxis.z };
    Q(rRot, q);
    LTMathImpl::RotateAroundAxis(q, a, amount);
    R(rRot, q);
    return LT_OK;
}

LTRESULT ILTMath::EulerRotateX(LTRotation &rRot, float amount)
{ LTVector v(1.0f, 0.0f, 0.0f); return RotateAroundAxis(rRot, v, amount); }

LTRESULT ILTMath::EulerRotateY(LTRotation &rRot, float amount)
{ LTVector v(0.0f, 1.0f, 0.0f); return RotateAroundAxis(rRot, v, amount); }

LTRESULT ILTMath::EulerRotateZ(LTRotation &rRot, float amount)
{ LTVector v(0.0f, 0.0f, 1.0f); return RotateAroundAxis(rRot, v, amount); }

// ⚠️ (RIGHT, UP, FORWARD) are the matrix COLUMNS: column c is where the local
// axis c ends up, which is how LTMathImpl::GetVectors defines them too.
LTRESULT ILTMath::GetRotationVectorsFromMatrix(LTMatrix &mMat, LTVector &vRight, LTVector &vUp, LTVector &vForward)
{
    LTIMPL("ILTMath::GetRotationVectorsFromMatrix");
    vRight.Init  (mMat.m[0][0], mMat.m[1][0], mMat.m[2][0]);
    vUp.Init     (mMat.m[0][1], mMat.m[1][1], mMat.m[2][1]);
    vForward.Init(mMat.m[0][2], mMat.m[1][2], mMat.m[2][2]);
    return LT_OK;
}
