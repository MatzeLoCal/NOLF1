// ----------------------------------------------------------------------- //
//
// MODULE  : ltworlddoors.cpp
//
// See ltworlddoors.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltworlddoors.h"
#include "ltworld.h"
#include "lt3d.h"

#include <math.h>
#include <string.h>

namespace
{
    const float kDegToRad = 3.14159265f / 180.0f;

    // Rotation about X, then Y, then Z, by the given radians, around the
    // origin. Row-major, translation in column 3 -- LT3D's layout.
    void RotationMatrix(const float* pRad, float* out)
    {
        float cx = cosf(pRad[0]), sx = sinf(pRad[0]);
        float cy = cosf(pRad[1]), sy = sinf(pRad[1]);
        float cz = cosf(pRad[2]), sz = sinf(pRad[2]);

        float m00 =  cy*cz + sy*sx*sz, m01 = -cy*sz + sy*sx*cz, m02 = sy*cx;
        float m10 =  cx*sz,            m11 =  cx*cz,            m12 = -sx;
        float m20 = -sy*cz + cy*sx*sz, m21 =  sy*sz + cy*sx*cz, m22 = cy*cx;

        LT3D::Identity(out);
        out[0] = m00; out[1] = m01; out[2]  = m02;
        out[4] = m10; out[5] = m11; out[6]  = m12;
        out[8] = m20; out[9] = m21; out[10] = m22;
    }
}


CLTWorldDoors::CLTWorldDoors() : m_bChanged(false) {}


unsigned int CLTWorldDoors::Moving() const
{
    unsigned int n = 0;
    for (size_t i = 0; i < m_Doors.size(); ++i) if (m_Doors[i].m_nGoing) ++n;
    return n;
}


void CLTWorldDoors::Build(const CLTWorld& world)
{
    m_Doors.clear();
    m_bChanged = true;

    const std::vector<LTWorldObject>&      obj = world.GetObjects();
    const std::vector<LTWorldModelHeader>& mdl = world.GetModels();

    for (size_t i = 0; i < obj.size(); ++i)
    {
        const bool bHinged = (obj[i].m_sType == "HingedDoor");
        const bool bSlide  = (obj[i].m_sType == "Door");
        if (!bHinged && !bSlide) continue;

        const char* pName = obj[i].GetName();
        if (!pName || !*pName) continue;

        // ⚠️ The object names the BRUSH it moves, and they are matched by name
        // because the world file has no other link between them. A door object
        // with no matching world model is not a door that can be drawn moving.
        const LTWorldModelHeader* pModel = 0;
        for (size_t k = 0; k < mdl.size(); ++k)
            if (mdl[k].m_sName == pName && mdl[k].m_bBodyParsed)
            { pModel = &mdl[k]; break; }
        if (!pModel || pModel->m_Points.empty()) continue;

        // ⚠️ NOT memset: the struct now holds std::strings, and zeroing one is
        // undefined behaviour that happens to work until it does not.
        Door d;
        d.m_bHinged = d.m_bLocked = false;
        d.m_fSpeed = d.m_fDist = d.m_fOpen = 0.0f;
        d.m_nGoing = 0;
        d.m_fSoundRadius = 0.0f;
        for (int k = 0; k < 3; ++k)
        { d.m_Hinge[k] = d.m_Angles[k] = d.m_Dir[k] = d.m_Centre[k] = 0.0f;
          d.m_Min[k] = d.m_Max[k] = 0.0f; }
        d.m_sName   = pName;

        {
            const LTWorldProperty* p1 = obj[i].Find("OpenSound");
            const LTWorldProperty* p2 = obj[i].Find("CloseSound");
            const LTWorldProperty* p3 = obj[i].Find("LockedSound");
            const LTWorldProperty* p4 = obj[i].Find("SoundRadius");
            if (p1) d.m_sOpen   = p1->m_sString;
            if (p2) d.m_sClose  = p2->m_sString;
            if (p3) d.m_sLocked = p3->m_sString;
            if (p4) d.m_fSoundRadius = p4->m_fReal;
        }
        d.m_bHinged = bHinged;
        d.m_fOpen   = 0.0f;
        d.m_nGoing  = 0;

        const LTWorldProperty* pLocked = obj[i].Find("Locked");
        d.m_bLocked = pLocked && pLocked->m_bBool;

        const LTWorldProperty* pSpeed = obj[i].Find("Speed");
        d.m_fSpeed = pSpeed ? pSpeed->m_fReal : 0.0f;

        bool bUsable = false;

        if (bHinged)
        {
            const LTWorldProperty* pPt  = obj[i].Find("RotationPoint");
            const LTWorldProperty* pAng = obj[i].Find("RotationAngles");
            if (pPt && pAng)
            {
                for (int k = 0; k < 3; ++k)
                {
                    d.m_Hinge[k]  = pPt->m_Vec[k];
                    d.m_Angles[k] = pAng->m_Vec[k];
                }
                bUsable = (fabsf(d.m_Angles[0]) + fabsf(d.m_Angles[1])
                         + fabsf(d.m_Angles[2])) > 0.01f;
            }
            if (d.m_fSpeed <= 0.0f) d.m_fSpeed = 65.0f;     // degrees/s
        }
        else
        {
            const LTWorldProperty* pDir  = obj[i].Find("MoveDir");
            const LTWorldProperty* pDist = obj[i].Find("MoveDist");
            if (pDir && pDist)
            {
                float len = sqrtf(pDir->m_Vec[0]*pDir->m_Vec[0]
                                + pDir->m_Vec[1]*pDir->m_Vec[1]
                                + pDir->m_Vec[2]*pDir->m_Vec[2]);
                d.m_fDist = pDist->m_fReal;
                if (len > 1e-4f && fabsf(d.m_fDist) > 0.01f)
                {
                    for (int k = 0; k < 3; ++k) d.m_Dir[k] = pDir->m_Vec[k] / len;
                    bUsable = true;
                }
            }
            if (d.m_fSpeed <= 0.0f) d.m_fSpeed = 200.0f;    // units/s
        }

        // ⚠️ Refused, not faked. Most objects of class `Door` carry no move
        // direction at all -- the class is reused for brushes that never move,
        // including the level's SkyBox -- and inventing an opening for them
        // would swing pieces of scenery around.
        if (!bUsable) continue;

        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t p = 0; p < pModel->m_Points.size(); ++p)
            for (int k = 0; k < 3; ++k)
            {
                float v = pModel->m_Points[p].m_Pos[k];
                if (v < mn[k]) mn[k] = v;
                if (v > mx[k]) mx[k] = v;
            }
        for (int k = 0; k < 3; ++k)
        {
            d.m_Centre[k] = (mn[k] + mx[k]) * 0.5f;
            d.m_Min[k] = mn[k];
            d.m_Max[k] = mx[k];
        }

        m_Doors.push_back(d);
    }

    AttachProps(world);
}


// ⚠️ A DOOR HANDLE IS NOT PART OF THE DOOR. It is a separate `DoorKnob` object
// with its own .abc model and a fixed Pos, and the world file contains NO link
// between the two -- every property of a knob was dumped and none names a door.
// The real game's ObjectDLL makes the attachment at runtime; this port has no
// ObjectDLL, so the association has to be recovered from where things ARE.
//
// ⚠️ The rule is DISTANCE TO THE BRUSH, not to its centre. A door is a wide flat
// slab and a knob sits near one edge of it, so centre distance ranks a knob
// against the wrong door of a double pair about as often as the right one.
// Distance to the box is what "this knob is on that door" actually means.
//
// ⚠️ And it is a bounded search: a knob further than kGrab from every door brush
// is left alone rather than attached to the least-bad candidate. Handles on
// fixed frames and cupboards exist, and dragging one around when a door across
// the room opens would be a worse bug than the one being fixed.
void CLTWorldDoors::AttachProps(const CLTWorld& world)
{
    m_Attached.clear();

    // Knobs sit ON the door face; the brush is only ~10 units thick, so this is
    // "touching it", not "near it".
    const float kGrab = 24.0f;

    const std::vector<LTWorldObject>& obj = world.GetObjects();
    for (size_t i = 0; i < obj.size(); ++i)
    {
        if (obj[i].m_sType != "DoorKnob") continue;

        const char* pName = obj[i].GetName();
        if (!pName || !*pName) continue;

        float pos[3];
        if (!obj[i].GetPos(pos)) continue;

        int   nBest = -1;
        float fBest = kGrab;
        for (size_t k = 0; k < m_Doors.size(); ++k)
        {
            const Door& d = m_Doors[k];
            float f = 0.0f;
            for (int a = 0; a < 3; ++a)
            {
                float v = pos[a];
                if (v < d.m_Min[a]) { float e = d.m_Min[a] - v; f += e * e; }
                if (v > d.m_Max[a]) { float e = v - d.m_Max[a]; f += e * e; }
            }
            f = sqrtf(f);
            if (f < fBest) { fBest = f; nBest = (int)k; }
        }

        if (nBest >= 0) m_Attached[pName] = nBest;
    }
}


int CLTWorldDoors::Activate(const float* pPos3, float fReach, bool* pbLocked)
{
    if (pbLocked) *pbLocked = false;

    int   nBest = -1;
    float fBest = fReach * fReach;

    for (size_t i = 0; i < m_Doors.size(); ++i)
    {
        const Door& d = m_Doors[i];
        float dx = d.m_Centre[0] - pPos3[0];
        float dy = d.m_Centre[1] - pPos3[1];
        float dz = d.m_Centre[2] - pPos3[2];
        float f = dx*dx + dy*dy + dz*dz;
        if (f < fBest) { fBest = f; nBest = (int)i; }
    }

    if (nBest < 0) return -1;

    Door& d = m_Doors[nBest];
    if (d.m_bLocked) { if (pbLocked) *pbLocked = true; return nBest; }

    // ⚠️ Reverse whatever it is doing. A door caught mid-swing must turn
    // around from where it IS, not snap to an end -- which is why the state is
    // a fraction rather than a boolean.
    if (d.m_nGoing != 0)      d.m_nGoing = -d.m_nGoing;
    else if (d.m_fOpen < 0.5f) d.m_nGoing = +1;
    else                       d.m_nGoing = -1;

    return nBest;
}


void CLTWorldDoors::Update(float fDelta)
{
    if (fDelta <= 0.0f) return;

    for (size_t i = 0; i < m_Doors.size(); ++i)
    {
        Door& d = m_Doors[i];
        if (!d.m_nGoing) continue;

        // How long the full travel takes, from the door's own speed.
        float fTravel = d.m_bHinged
            ? (fabsf(d.m_Angles[0]) + fabsf(d.m_Angles[1]) + fabsf(d.m_Angles[2]))
            : fabsf(d.m_fDist);
        float fSeconds = (d.m_fSpeed > 1e-4f) ? (fTravel / d.m_fSpeed) : 1.0f;
        if (fSeconds < 0.05f) fSeconds = 0.05f;

        d.m_fOpen += (float)d.m_nGoing * (fDelta / fSeconds);
        m_bChanged = true;

        if (d.m_fOpen >= 1.0f) { d.m_fOpen = 1.0f; d.m_nGoing = 0; }
        if (d.m_fOpen <= 0.0f) { d.m_fOpen = 0.0f; d.m_nGoing = 0; }
    }
}


// The matrix for ONE door at its current openness. Split out of GetTransform so
// that a prop riding on the door can be given the identical transform rather
// than a second implementation of it.
void CLTWorldDoors::MatrixOf(const Door& d, float* pOut16) const
{
    if (d.m_fOpen <= 0.0f) { LT3D::Identity(pOut16); return; }

    if (d.m_bHinged)
    {
        // ⚠️ Rotate ABOUT THE HINGE, which means translating the hinge to
        // the origin, rotating, and translating back. Rotating in place
        // would spin the door about its own centre and leave the frame.
        float rad[3] = { d.m_Angles[0] * kDegToRad * d.m_fOpen,
                         d.m_Angles[1] * kDegToRad * d.m_fOpen,
                         d.m_Angles[2] * kDegToRad * d.m_fOpen };
        float R[16];
        RotationMatrix(rad, R);

        // out = T(+hinge) * R * T(-hinge), written directly: the rotation
        // stays in the upper 3x3 and the translation becomes
        // hinge - R*hinge.
        LT3D::Identity(pOut16);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) pOut16[r*4 + c] = R[r*4 + c];

        for (int r = 0; r < 3; ++r)
        {
            float v = 0.0f;
            for (int c = 0; c < 3; ++c) v += R[r*4 + c] * d.m_Hinge[c];
            pOut16[r*4 + 3] = d.m_Hinge[r] - v;
        }
    }
    else
    {
        LT3D::Identity(pOut16);
        for (int k = 0; k < 3; ++k)
            pOut16[k*4 + 3] = d.m_Dir[k] * d.m_fDist * d.m_fOpen;
    }
}


bool CLTWorldDoors::GetTransform(const std::string& sName, float* pOut16) const
{
    for (size_t i = 0; i < m_Doors.size(); ++i)
    {
        if (m_Doors[i].m_sName != sName) continue;
        MatrixOf(m_Doors[i], pOut16);
        return true;
    }

    // ⚠️ A NAME THAT IS NOT A DOOR MAY STILL BE CARRIED BY ONE. Door handles are
    // separate objects (see AttachProps), and they move only because they answer
    // here with their door's matrix. Keying both off the same call is what lets
    // the renderer stay ignorant of doors entirely -- it asks every model and
    // every object "have you moved?" and does not care why the answer is yes.
    std::map<std::string, int>::const_iterator it = m_Attached.find(sName);
    if (it != m_Attached.end() && it->second >= 0 && it->second < (int)m_Doors.size())
    {
        MatrixOf(m_Doors[it->second], pOut16);
        return true;
    }

    return false;
}
