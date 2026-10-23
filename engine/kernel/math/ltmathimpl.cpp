// ----------------------------------------------------------------------- //
//
// MODULE  : ltmathimpl.cpp
//
// PURPOSE : Rotation maths. See ltmathimpl.h for why it is load-bearing.
//
// ----------------------------------------------------------------------- //

#include "ltmathimpl.h"

#include "ltquatbase.h"

#include <cmath>

namespace
{
    // q = a * b, both (x, y, z, w).
    void QuatMul(const float* a, const float* b, float* out)
    {
        float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
        float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
        float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
        float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
        out[0] = x; out[1] = y; out[2] = z; out[3] = w;
    }

    void Normalise(float* q)
    {
        float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (n > 1e-12f) { for (int i = 0; i < 4; ++i) q[i] /= n; }
        else { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; }
    }
}

namespace LTMathImpl
{

void GetVectors(const float* pQuat, float* pRight, float* pUp, float* pForward)
{
    // ⚠️ Monolith's own function, from LT2/sdk/inc/ltquatbase.cpp. Using it
    // rather than re-deriving the basis means the handedness and the "identity
    // faces down +Z" convention cannot drift from the original.
    quat_GetVectors(pQuat, pRight, pUp, pForward);
}

void RotateAroundAxis(float* pQuat, const float* pAxis, float amount)
{
    float ax[3] = { pAxis[0], pAxis[1], pAxis[2] };
    float len = std::sqrt(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
    if (len < 1e-12f) return;
    for (int i = 0; i < 3; ++i) ax[i] /= len;

    float h = amount * 0.5f;
    float s = std::sin(h);
    float q[4] = { ax[0]*s, ax[1]*s, ax[2]*s, std::cos(h) };

    float out[4];
    QuatMul(q, pQuat, out);          // world axis -> pre-multiply
    Normalise(out);
    for (int i = 0; i < 4; ++i) pQuat[i] = out[i];
}

void SetupEuler(float* pQuat, float pitch, float yaw, float roll)
{
    // pitch about X, yaw about Y, roll about Z -- the order iltmath.h states.
    float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    float cy = std::cos(yaw   * 0.5f), sy = std::sin(yaw   * 0.5f);
    float cr = std::cos(roll  * 0.5f), sr = std::sin(roll  * 0.5f);

    float qx[4] = { sp, 0, 0, cp };
    float qy[4] = { 0, sy, 0, cy };
    float qz[4] = { 0, 0, sr, cr };

    float t[4];
    QuatMul(qy, qx, t);
    QuatMul(t, qz, pQuat);
    Normalise(pQuat);
}

void AlignRotation(float* pQuat, const float* pDir, const float* pUp)
{
    float f[3] = { pDir[0], pDir[1], pDir[2] };
    float fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (fl < 1e-12f) { pQuat[0]=pQuat[1]=pQuat[2]=0.0f; pQuat[3]=1.0f; return; }
    for (int i = 0; i < 3; ++i) f[i] /= fl;

    float u[3] = { pUp[0], pUp[1], pUp[2] };

    // right = up x forward, then up = forward x right -- re-orthogonalised so
    // a caller's approximate up does not shear the basis.
    float r[3] = { u[1]*f[2] - u[2]*f[1],
                   u[2]*f[0] - u[0]*f[2],
                   u[0]*f[1] - u[1]*f[0] };
    float rl = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (rl < 1e-12f) { r[0] = 1.0f; r[1] = r[2] = 0.0f; rl = 1.0f; }
    for (int i = 0; i < 3; ++i) r[i] /= rl;

    u[0] = f[1]*r[2] - f[2]*r[1];
    u[1] = f[2]*r[0] - f[0]*r[2];
    u[2] = f[0]*r[1] - f[1]*r[0];

    // Columns are right, up, forward -- the inverse of quat_GetVectors, so the
    // two round-trip.
    float m[4][4];
    m[0][0] = r[0]; m[0][1] = u[0]; m[0][2] = f[0]; m[0][3] = 0.0f;
    m[1][0] = r[1]; m[1][1] = u[1]; m[1][2] = f[1]; m[1][3] = 0.0f;
    m[2][0] = r[2]; m[2][1] = u[2]; m[2][2] = f[2]; m[2][3] = 0.0f;
    m[3][0] = m[3][1] = m[3][2] = 0.0f; m[3][3] = 1.0f;

    quat_ConvertFromMatrix(pQuat, m);
    Normalise(pQuat);
}

void Slerp(float* pDest, const float* pA, const float* pB, float t)
{
    quat_Slerp(pDest, pA, pB, t);
}

} // namespace LTMathImpl
