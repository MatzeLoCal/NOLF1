// ----------------------------------------------------------------------- //
//
// MODULE  : ltmodeldraw.h
//
// PURPOSE : Draws a loaded ABC model into an LT3DTarget.
//
//           This is the piece that makes the ABC loader and the rasteriser
//           into a renderer. Everything either side of it was already tested
//           in isolation; this is where they meet.
//
// ⚠️ IT DEPENDS ON ltabc AND lt3d, AND ON NOTHING ELSE
//
//   In particular it does NOT include the DTX loader. Skins arrive as plain
//   decoded LT3DTexture views, so this file cannot care where the pixels came
//   from -- and a test can hand it a synthetic 2x2 checker instead of a 4 MB
//   texture. The engine headers stay out for the same reason lt2d and lt3d
//   keep them out.
//
// TWO POSES, AND THE CALLER CHOOSES
//
//   Every ABC vertex carries BOTH a model-space `m_Pos` and a set of
//   node-space weights (`offset` per node, plus a bias).
//
//   * **Bind pose** -- `m_Pos` used directly. No hierarchy, no animation.
//     Exactly right for a static prop.
//   * **Skinned** -- walk the node tree into world matrices for a given
//     animation and keyframe, then
//     `pos = sum(bias * (world[node] * offset))`.
//
// ⚠️ THE BIND POSE IS NOT A KEYFRAME OF THE MODEL'S OWN ANIMATION
//
//   Tempting assumption, and it is false. `CHARS\MODELS\INTHERO_ACTION.ABC`
//   has exactly ONE animation ("interface4", 151 keyframes) and **no keyframe
//   reproduces `m_Pos`** -- the closest, frame 94, is still 7.5 units out on a
//   figure about 64 units tall. Character models inherit their skeleton and
//   animations from a child model (`basemodel.abc` and friends), so `m_Pos` is
//   an authored reference pose, not frame 0 of anything.
//
//   So "does skinning reproduce m_Pos?" is the WRONG check and would have sent
//   someone hunting a bug that is not there. What is verified instead:
//   composing the hierarchy from keyframe transforms gives the same answer as
//   the node's own stored `m_Matrix` (6.081427 vs 6.081412 on CATE.ABC) --
//   i.e. **`m_Matrix` is the composed world bind transform**, and the two
//   independent routes agree.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_MODEL_DRAW_H__
#define __LT_MODEL_DRAW_H__

#include "lt3d.h"

#include <vector>

class CLTABCModel;
struct LTABCVertex;

struct LTModelDrawStats
{
    int m_nTriangles;       // triangles submitted
    int m_nPixels;          // pixels actually written
    int m_nCulled;          // triangles that covered nothing
};

// One 4x4 per node, in the flattened depth-first order the loader produces.
struct LTModelPose
{
    std::vector<float>  m_Node;     // 16 floats per node

    const float*        Get(unsigned int i) const { return &m_Node[(size_t)i * 16]; }
    unsigned int        Count() const { return (unsigned int)(m_Node.size() / 16); }
};

namespace LTModelDraw
{
    // Composes the node hierarchy for one animation keyframe into world
    // matrices. Returns false if the model has no such animation or keyframe.
    //
    // ⚠️ The keyframe transforms are LOCAL to each node's parent, so this is a
    // parent-then-child compose. Verified against the models' own stored node
    // matrices, which are the composed result.
    bool    BuildPose(const CLTABCModel& model, unsigned int nAnim,
                      unsigned int nKeyframe, LTModelPose& out);

    // ⚠️ THE ENGINE'S NODE-CONTROL HOOK -- ILTClient::ModelNodeControl.
    //
    // NOLF animates FACES through it: CNodeController rotates the jaw from the
    // amplitude of the voice that is playing (lip sync) and turns the head to
    // follow a speaker. The game's callback receives each node's GLOBAL
    // (model-space) matrix and does `*pGlobal = *pGlobal * delta` -- so it has
    // to be called DURING the hierarchy walk, after a node is composed and
    // before its children are, or a turned head would leave the face behind.
    //
    // The matrix is this module's own row-major float[16], which is LTMatrix's
    // exact layout (`float m[4][4]`, translation in column 3) -- the hook can
    // hand it to the game unconverted.
    typedef void (*NodeHook)(unsigned int nNode, float* pGlobal16, void* pUser);
    bool    BuildPose(const CLTABCModel& model, unsigned int nAnim,
                      unsigned int nKeyframe, LTModelPose& out,
                      NodeHook pHook, void* pHookUser);

    // ⚠️ THE EXTRA TRACKERS -- ILTModel::AddTracker's blink, twitch and pitch.
    //
    // Each plays its own animation through a WEIGHT SET (one float per node),
    // blended onto the main animation at the LOCAL-transform stage, in the
    // order the trackers were added -- the donor's TransformMaker::
    // BlendTransform rule:
    //
    //     weight 0          -> the layer does not touch this node
    //     weight exactly 2  -> ADDITIVE: the layer's motion relative to its
    //                          own frame 0, q = q * (q0^-1 * q_now),
    //                          v = v + (v_now - v0)
    //     any other weight  -> slerp / lerp toward the layer by that weight
    //
    // MNODE_ROTATIONONLY still replaces the translation AFTER blending, as the
    // donor does. NOLF's blink is `Blink` (Eyelid = 2.00) on anim `blink`,
    // whose frame 0 is the open lid -- so the eyes are open except during the
    // four 100 ms closes per loop. LT_NOANIMLAYERS=1 ignores every layer.
    struct Layer
    {
        unsigned int    m_nAnim;
        unsigned int    m_nKeyframe;
        int             m_nWeightSet;   // index into the model's weight sets
    };
    bool    BuildPose(const CLTABCModel& model, unsigned int nAnim,
                      unsigned int nKeyframe, LTModelPose& out,
                      NodeHook pHook, void* pHookUser,
                      const Layer* pLayers, unsigned int nLayers);

    // The model's rest hierarchy, straight from the node matrices. Used when
    // there is no animation to play.
    bool    BuildBindPose(const CLTABCModel& model, LTModelPose& out);

    // ⚠️ Is the model-space X mirror on? DEFAULT TRUE -- NOLF's model data is
    // left-handed against this renderer's basis (see ltmodeldraw.cpp);
    // LT_NOMODELMIRROR=1 disables it.
    //
    // Exposed because a caller that applies anything else in MODEL space --
    // the origin offset in the world object pass -- has to mirror it too.
    // Model space cannot be half mirrored: doing that fixed the lever
    // direction and broke the plate alignment, which is how it was caught.
    bool    MirrorX();

    // The skinning switches, for a backend that skins elsewhere (the GPU,
    // HANDOVER 5cd) and must honour the same A/B instruments:
    // LT_NOSKINNORMAL, LT_NOBINDINVERSE, LT_NOBIASNORM.
    void    SkinSwitches(bool* pNoSkinNormal, bool* pNoBindInverse, bool* pNoBiasNorm);

    // True while an instrument that only the CPU path implements is on
    // (LT_HIDEPIECE, LT_LIGHTONLY, LT_SKINPERCORNER, LT_BIGTRI, LT_NOMIRRORU):
    // a GPU backend must then hand the model to Draw instead.
    bool    CpuOnlyInstrument();

    // ⚠️ DOES THIS ANIMATION PRODUCE A PHYSICALLY POSSIBLE SKELETON?
    //
    //   A bone cannot stretch. So for every keyframe, the distance between a
    //   node and its parent must match the bind pose. This is an INDEPENDENT
    //   invariant -- it does not care whether the transforms were read in the
    //   right order, only whether the result is a body.
    //
    //   It exists because the byte-level checks are blind here: node-major and
    //   keyframe-major storage occupy exactly the same
    //   `nNodes * nKeyframes * 28` bytes, so the chunk-boundary discipline
    //   that validated the entire loader cannot tell them apart. Measured:
    //
    //       CATE.ABC            (13 kf)    0.18%     SUNGLASSES_HH (2 kf)  0.00%
    //       INTHERO_ACTION.ABC (151 kf)  2382%       WALTHER_HH    (2 kf)  0.00%
    //
    //   Short animations are read correctly; long ones are not, and the cause
    //   is still open. Rather than disable animation everywhere, the scene
    //   asks this question per animation and falls back to the bind pose only
    //   where the answer is no.
    //
    //   ⚠️ Checks EVERY keyframe, not a sample -- the cost is trivial and this
    //   project has been bitten by sampling before (HANDOVER.md 5).
    bool    AnimationIsPlausible(const CLTABCModel& model, unsigned int nAnim,
                                 float fTolerance = 0.02f);

    // Computes the model's axis-aligned bounds from LOD 0 of every piece.
    // Returns false if there is no geometry.
    //
    // ⚠️ Derived from the vertices rather than trusting the header's
    // `internalRadius`, because a viewer that frames the model wrongly looks
    // exactly like a renderer that transforms it wrongly.
    bool    GetBounds(const CLTABCModel& model, float* pMin3, float* pMax3);

    // Draws one LOD of every piece.
    //
    //  pMVP     model->clip, the full transform chain
    //  pNormal  model->world rotation for lighting; may be null for flat shading
    //  pSkins   decoded textures indexed by each piece's texture index;
    //           a piece whose index is out of range draws untextured
    //
    // ⚠️ Two-sided by default. NOLF models include single-sided cards (hair,
    // foliage, cloth) whose winding is not reliable, and a first look at a
    // half-missing character is indistinguishable from a broken transform.
    // pPose may be null, which draws the BIND POSE from each vertex's m_Pos.
    // ⚠️ WHERE A MODEL'S TRIANGLES GO, so a GPU backend can receive finished
    // triangles instead of reimplementing skin selection, the per-FACE UVs and
    // the two-sided lighting. Same reason ILTWorldGeomSink exists for world
    // brushes: these rules have each been got wrong once already, and a second
    // copy is a second place to get them wrong again.
    //
    // pModel3x3 is the three vertices in MODEL space -- posed and skinned, but
    // before pMVP -- which is what a GPU wants since it applies the matrix
    // itself. pVerts3 is the same three already transformed and shaded.
    class ISink
    {
    public:
        virtual ~ISink() {}
        virtual void Tri(const LT3DTexture* pTex,
                         const float pModel3x3[3][3],
                         const LT3DVert* pVerts3) = 0;
    };

    // ⚠️ `bMirrorU` -- FLIP THE TEXTURE HORIZONTALLY, and it belongs to the
    // CALLER, not to this module. A renderer that mirrors its scene must flip
    // model U to keep text readable, exactly as the world pass flips U for its
    // own polygons; a renderer that does not mirror must not. LTModelDraw is
    // shared by the world (mirrored) and the MENU (not yet), so deciding it
    // here from a global would corrupt whichever caller disagreed.
    //
    // ⚠️ Measured: with the scene mirror on and this off, the "CLASSIFIED"
    // stamp on `LooseFiles_01` renders BACKWARDS in M01S01. That is every prop
    // skin in every level, and nothing but a legible texture would have shown
    // it -- the door handles and lamps this port had been staring at carry no
    // text at all.
    // ⚠️ THE NORMAL-SKINNING RULE, AS ONE FUNCTION SO IT CAN BE ASSERTED.
    //
    //     n = normalize( SUM bias_i * R_i * R_bind_i^-1 * n_file )
    //
    // It is here rather than inline in Draw because it has an ORACLE and an
    // oracle is worthless if nothing can call it: skinning at the BIND pose
    // must give the file's own normal back, for any weights, since the biases
    // sum to 1. `enginetest_abc` asserts exactly that over every weighted
    // vertex of every shipped character.
    //
    // ⚠️ A NORMAL AND A POSITION ARE NOT IN THE SAME FRAME. `m_Offset` is
    // already in the bone's frame, so a position needs no inverse; `m_Normal`
    // is in MODEL space and does. Leaving the inverse out put hard-edged pale
    // facets on every posed face in the game -- HANDOVER.md 5aq.
    //
    // Returns false and leaves `out3` untouched when the vertex has no
    // weights or the pose does not match the model, which is the caller's
    // signal to use the file's normal as it stands.
    bool SkinNormal(const CLTABCModel& model, const LTABCVertex& vt,
                    const LTModelPose& pose, float* out3);

    // ⚠️ A SOCKET's MODEL-SPACE TRANSFORM at a pose: node(pose) x socket.
    // Case-insensitive name match, as the game names them ("Head",
    // "RightHand"). Returns false when the model has no such socket.
    // `bMirror` conjugates by the X mirror (Mx * M * Mx) for a model drawn
    // mirrored -- see CLTScene::GetSocketTransform for why it must be a
    // conjugation and not a negated position.
    bool SocketTransform(const CLTABCModel& model, const LTModelPose& pose,
                         const char* pSocketName, bool bMirror, float* pOut16);

    // The keyframe the renderer shows for `fSeconds` into an animation --
    // the LAST key at or before that time, wrapped when looping and held at
    // the end otherwise. One rule, so an attachment can never sit on a
    // different frame from the character it is attached to.
    unsigned int KeyframeAt(const CLTABCModel& model, unsigned int nAnim,
                            float fSeconds, bool bLooping);

    // ⚠️ BETWEEN TWO KEYS (HANDOVER 5cg). The donor's tracker interpolates
    // (slerp the rotations, lerp the positions by the time between the keys);
    // KeyframeAt and BuildPose show the LAST key, which is what every
    // character in this port still does. KeyframeSpan gives the two keys and
    // the fraction for `fSeconds` (wrapped when looping, held at the end
    // otherwise); BuildPoseBetween poses there. Used by the level's animated
    // props -- a globe that turns 41 degrees per key cannot step.
    void KeyframeSpan(const CLTABCModel& model, unsigned int nAnim,
                      float fSeconds, bool bLooping,
                      unsigned int* pK0, unsigned int* pK1, float* pFrac);
    bool BuildPoseBetween(const CLTABCModel& model, unsigned int nAnim,
                          unsigned int nKey0, unsigned int nKey1, float fFrac,
                          LTModelPose& out);

    // ⚠️ THE POSITION-SKINNING RULE, for the same reason -- it has an oracle.
    //
    //     p = ( SUM R_i * offset_i  +  SUM bias_i * t_i ) / SUM bias_i
    //
    // ⚠️ The division is not decoration: 9,719 shipped vertices have biases
    // that do not sum to 1, and without it each one is scaled toward the
    // model origin by its bias sum. Returns false (out3 untouched) for an
    // unweighted vertex or a pose that does not match the model.
    bool SkinPosition(const CLTABCModel& model, const LTABCVertex& vt,
                      const LTModelPose& pose, float* out3);

    // ⚠️ THE ENGINE'S OWN MODEL LIGHT, when the caller has one -- the lineage's
    // rule (donor setupmodel.cpp + renderlight.cpp), in 0..1 units:
    //
    //     c = clamp( ambient + SUM colour_i * max(0, N . -dir_i) )
    //
    // `ambient` is the world's AmbientLight plus the console's ModelAdd; each
    // light is a Light / ObjectLight / DirLight from the LEVEL, sampled ONCE at
    // the object's position (distance and cone falloff already in `colour`) and
    // applied as a directional light pointing from the light to the object.
    // Normals must be in the same frame as `dir` -- the world frame -- which is
    // what `pNormalMatrix` produces in the world pass.
    //
    // ⚠️ ONE-SIDED, unlike the fallback. The fallback's |N.L| hid wrong normals;
    // with the normals now proven (enginetest_abc) the back of a face is dark,
    // as it is in the original.
    //
    // Null -- the menu and modelview -- keeps the fixed key light, so the
    // frame contract's menu frame is untouched by this.
    struct LTModelLighting
    {
        enum { kMaxLights = 8 };
        float   m_Ambient[3];
        int     m_nLights;
        float   m_Dir[kMaxLights][3];       // unit, light -> object
        float   m_Color[kMaxLights][3];     // 0..1, attenuated
    };

    LTModelDrawStats Draw(const LT3DTarget& target, const CLTABCModel& model,
                          const float* pMVP, const float* pNormalMatrix,
                          const LT3DTexture* pSkins, int nSkins,
                          int nLod = 0, bool bCull = false,
                          const LTModelPose* pPose = 0,
                          ISink* pSink = 0,
                          bool bMirrorU = false,
                          const LTModelLighting* pLighting = 0,
                          int nMirrorModel = -1);

    // ⚠️ `nMirrorModel` -- WHETHER THIS MODEL'S SPACE IS MIRRORED IN X, per
    // call: -1 = MirrorX() (the default every older caller gets), 0 = no,
    // 1 = yes. It is a parameter because props and characters need DIFFERENT
    // answers, and the Windows build's own frame of M01S01 says so for both:
    // its door lever points the way the MIRRORED prop does, and its Cate turns
    // and raises her hand the way the UNMIRRORED character does. See
    // HANDOVER.md 5as.
}

#endif // __LT_MODEL_DRAW_H__
