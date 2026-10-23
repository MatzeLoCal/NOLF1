// ----------------------------------------------------------------------- //
//
// MODULE  : ltworlddoors.h
//
// PURPOSE : Doors that open. Reads the world's own door objects, matches each
//           to the brush it moves, and animates it.
//
// ⚠️ EVERY NUMBER HERE COMES FROM THE WORLD FILE. A `HingedDoor` object
// carries `RotationPoint` (the hinge), `RotationAngles` in DEGREES (0,90,0 for
// a normal door) and `Speed`; a sliding `Door` carries `MoveDir`, `MoveDist`
// and `Speed`. Nothing is invented, and a door whose properties do not say how
// it moves is left shut rather than guessed at.
//
// ⚠️ WHICH DOORS ACTUALLY OPEN, measured across three levels:
//
//     HingedDoor   23/23, 17/20, 6/6   carry a rotation angle
//     Door         1/25, 15/98, 1/12   carry a move direction AND distance
//
// So the hinged ones are nearly all openable and most objects of class `Door`
// are not doors that move at all -- the class is used for other brushes, the
// level's SkyBox among them. That is why this reports how many it accepted.
//
// ⚠️ WHAT IS NOT MODELLED: locks needing keys, triggers, AI opening doors,
// sounds, damage, or the wait-then-close cycle. `Locked` is honoured only to
// the extent of refusing to open.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WORLD_DOORS_H__
#define __LT_WORLD_DOORS_H__

#include <string>
#include <map>
#include <vector>

class CLTWorld;
struct LTWorldModelHeader;

class CLTWorldDoors
{
public:

    CLTWorldDoors();

    void            Build(const CLTWorld& world);

    unsigned int    Count() const   { return (unsigned int)m_Doors.size(); }
    unsigned int    Moving() const;

    // Opens or closes the nearest door whose brush centre is within fReach of
    // the position. Returns its index, or -1 when nothing is in range. A
    // locked door is reported through pbLocked rather than silently ignored.
    int             Activate(const float* pPos3, float fReach, bool* pbLocked);

    void            Update(float fDelta);

    // The current transform for a world model, or false when it is not a door.
    bool            GetTransform(const std::string& sModelName, float* pOut16) const;

    // ⚠️ True while ANY door is mid-swing. The collision cache only needs
    // rebuilding then, and asking every frame would throw away the saving.
    bool            Changed() const { return m_bChanged; }
    void            ClearChanged()  { m_bChanged = false; }

    const std::string& Name(unsigned int i) const { return m_Doors[i].m_sName; }

    // The sound the door makes, and where it is. Empty when the author set none.
    const std::string& OpenSound(unsigned int i) const   { return m_Doors[i].m_sOpen; }
    const std::string& CloseSound(unsigned int i) const  { return m_Doors[i].m_sClose; }
    const std::string& LockedSound(unsigned int i) const { return m_Doors[i].m_sLocked; }
    float              SoundRadius(unsigned int i) const { return m_Doors[i].m_fSoundRadius; }

    // ⚠️ Which sound to play: a door that has just been told to OPEN is going
    // +1. Asking m_fOpen instead would pick the wrong sound at the moment of
    // activation, because it has not started moving yet.
    bool               IsOpening(unsigned int i) const { return m_Doors[i].m_nGoing > 0; }
    void               Centre(unsigned int i, float* pOut3) const
                       { for (int k = 0; k < 3; ++k) pOut3[k] = m_Doors[i].m_Centre[k]; }

private:

    // Finds the props that ride on each door. Called by Build.
    void                AttachProps(const CLTWorld& world);

    struct Door;
    void                MatrixOf(const Door& d, float* pOut16) const;

    struct Door
    {
        std::string m_sName;
        bool        m_bHinged;
        bool        m_bLocked;

        float       m_Hinge[3];         // hinged: the pivot
        float       m_Angles[3];        // hinged: degrees, fully open
        float       m_Dir[3];           // sliding: unit direction
        float       m_fDist;            // sliding: units
        float       m_fSpeed;           // degrees/s or units/s

        // ⚠️ The door's OWN sounds, named by the level author -- `OpenSound`,
        // `CloseSound`, `LockedSound`. Playing these rather than something
        // chosen here is the difference between the game's audio and a guess.
        std::string m_sOpen, m_sClose, m_sLocked;
        float       m_fSoundRadius;

        float       m_Centre[3];        // brush centre, for "which door is near"
        float       m_Min[3], m_Max[3];  // brush bounds, for attaching props
        float       m_fOpen;            // 0 shut .. 1 open
        int         m_nGoing;           // +1 opening, -1 closing, 0 at rest
    };

    std::vector<Door>   m_Doors;

    // ⚠️ PROPS THAT RIDE ON A DOOR, by object name -> door index. A door handle
    // is not part of the door brush: it is a separate `DoorKnob` object with
    // its own .abc, placed at a fixed position, and NOTHING in the world file
    // links the two -- the knob has no property naming its door. In the real
    // game the ObjectDLL attaches it at runtime; with no ObjectDLL the same
    // association has to be recovered from geometry, which is what Build does.
    //
    // Without this the door swings open and leaves its handles hanging in the
    // air where the door used to be.
    std::map<std::string, int>  m_Attached;

    bool                m_bChanged;
};

#endif // __LT_WORLD_DOORS_H__
