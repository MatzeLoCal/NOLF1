// ----------------------------------------------------------------------- //
//
// MODULE  : ltmodeldraw.cpp
//
// PURPOSE : Draws a loaded ABC model into an LT3DTarget. See ltmodeldraw.h.
//
// ----------------------------------------------------------------------- //

#include "ltmodeldraw.h"

#include <stdlib.h>
#include <stdio.h>
#include "ltabc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <strings.h>
#include <vector>

namespace
{
    // See the use site: an off switch for the texture-U flip.
    bool NoMirrorU()
    {
        static const bool b = (getenv("LT_NOMIRRORU") != 0);
        return b;
    }

    // LT_NOSKINNORMAL=1 restores the old behaviour -- normals straight from
    // the file, unskinned -- so the fix below can be measured rather than
    // asserted.
    //
    // ⚠️ IT ANNOUNCES ITSELF. An A/B whose two arms cannot be told apart in
    // the output is how four runs of the same binary once read as a clean
    // result; a switch that says nothing is indistinguishable from a switch
    // that did not take.
    bool NoSkinNormal()
    {
        static const bool b = (getenv("LT_NOSKINNORMAL") != 0);
        static const bool bSaid = (b ? (printf("[NORMAL] LT_NOSKINNORMAL=1 -- "
                                               "normals taken UNSKINNED from the file\n"),
                                        true) : true);
        (void)bSaid;
        return b;
    }

    // ⚠️ LT_NOBINDINVERSE=1 -- skin the normal WITHOUT the inverse bind
    // rotation, which is what this did when normal skinning first landed.
    // Kept as a switch rather than deleted, because it is the arm that
    // produced the artefact and the difference has to stay measurable.
    bool NoBindInverse()
    {
        static const bool b = (getenv("LT_NOBINDINVERSE") != 0);
        static const bool bSaid = (b ? (printf("[NORMAL] LT_NOBINDINVERSE=1 -- "
                                               "normals skinned WITHOUT the inverse "
                                               "bind rotation\n"), true) : true);
        (void)bSaid;
        return b;
    }

    // ⚠️ LT_NOBIASNORM=1 -- skin positions WITHOUT dividing by the bias sum,
    // which is what this did until 2026-09-30. The arm that put the pale
    // wedges on Cate's cheeks; kept so the difference stays measurable, and
    // it announces itself like the others.
    bool NoBiasNorm()
    {
        static const bool b = (getenv("LT_NOBIASNORM") != 0);
        static const bool bSaid = (b ? (printf("[SKIN] LT_NOBIASNORM=1 -- positions skinned "
                                               "WITHOUT dividing by the bias sum\n"), true)
                                     : true);
        (void)bSaid;
        return b;
    }

    // A fixed key light, in MODEL space when no normal matrix is supplied.
    // Normalised at compile time by construction.
    const float kLightDir[3] = { -0.40824829f, 0.81649658f, 0.40824829f };
    const float kAmbient = 0.35f;

    inline float Dot3(const float* a, const float* b)
    {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    }

    // Rotates a direction by the upper 3x3 only -- a normal must not pick up
    // the translation, which is what TransformPoint would give it.
    void RotateNormal(const float* m, const float* n, float* out)
    {
        if (!m)
        {
            out[0] = n[0]; out[1] = n[1]; out[2] = n[2];
            return;
        }
        for (int r = 0; r < 3; ++r)
            out[r] = m[r*4+0]*n[0] + m[r*4+1]*n[1] + m[r*4+2]*n[2];
    }
}

// ⚠️ MODEL SPACE IS MIRRORED IN X, AND THIS UNDOES IT (default ON, 2026-09-09).
//
// NOLF's model data is left-handed against the basis this renderer builds, so
// every .abc came out reflected about its own X. It shipped as an opt-in
// experiment for one day and is now the default, on this evidence:
//
//   TEXTURE -- `WINEBOTTLE_01`'s label is authored with the grapes LEFT and the
//   "1960 / SPARKLING / MERLOT" block RIGHT. Drawn from OUTSIDE the bottle it
//   came out the other way round; with the mirror it matches the texture.
//   ⚠️ The world's own signage (the "28" door plate) reads correctly either
//   way, so this was never the camera and never the world mirror -- see
//   LT_MIRROR, which is a DIFFERENT and still-unsettled question.
//
//   GEOMETRY, measured in WORLD space with no camera involved:
//     * the two knobs on one door face put their plates 0.4 units apart;
//     * a lever sits 6.9 units INSIDE its leaf instead of 7.5 units past it;
//     * a double door's two handles stop interpenetrating (9.4 units of
//       overlap becomes a 9.9 gap) and land SYMMETRIC about that door's own
//       centre line to 0.4 units.
//
// ⚠️ That last figure is why this is the default. Two handles landing
// symmetric about their door's centre, from data nobody tuned, is not
// something a wrong transform produces.
//
// `LT_NOMODELMIRROR=1` turns it off -- named negatively, like LT_NOFILTER and
// LT_NOSORT, because the flag now describes a departure from correct output.
//
// ⚠️ MODEL SPACE CANNOT BE HALF MIRRORED. Anything else expressed in model
// space has to be mirrored with the mesh: the object pass mirrors the model's
// declared origin offset through MirrorX() for exactly this reason. Mirroring
// the mesh alone fixed the levers and silently broke the plate alignment
// (0.1 units apart, then 19.6), which is how the omission was caught.
//
// ⚠️ Culling is `false` for every current caller, so negating X does not flip
// any face away. A caller that ever culls must swap two corners as well.
namespace LTModelDraw
{

bool MirrorX()
{
    static const bool b = (getenv("LT_NOMODELMIRROR") == 0);
    return b;
}


namespace
{
    // Walks the depth-first node list, composing parent * local. Returns the
    // index just past this node's subtree, which is what makes a flat array
    // walkable as a tree without building one.
    unsigned int ComposeNode(const std::vector<LTABCNode>& nodes,
                             unsigned int idx, const float* pParent,
                             const float* pLocals, LTModelPose& out,
                             NodeHook pHook = 0, void* pHookUser = 0)
    {
        if (idx >= nodes.size()) return idx;

        const float* local = pLocals + (size_t)idx * 16;
        float* world = &out.m_Node[(size_t)idx * 16];

        if (pParent) LT3D::Multiply(pParent, local, world);
        else         std::memcpy(world, local, sizeof(float) * 16);

        // ⚠️ HERE, between this node and its children -- see NodeHook.
        if (pHook) pHook(idx, world, pHookUser);

        unsigned int next = idx + 1;
        for (unsigned int c = 0; c < nodes[idx].m_nChildren; ++c)
            next = ComposeNode(nodes, next, world, pLocals, out, pHook, pHookUser);
        return next;
    }
}

bool BuildPose(const CLTABCModel& model, unsigned int nAnim,
               unsigned int nKeyframe, LTModelPose& out)
{
    return BuildPose(model, nAnim, nKeyframe, out, 0, 0);
}

bool BuildPose(const CLTABCModel& model, unsigned int nAnim,
               unsigned int nKeyframe, LTModelPose& out,
               NodeHook pHook, void* pHookUser)
{
    return BuildPose(model, nAnim, nKeyframe, out, pHook, pHookUser, 0, 0);
}

namespace
{
    // The donor's quat_Slerp (ltquatbase.cpp), shortest arc, linear when the
    // two are nearly equal.
    void QuatSlerp(float* out, const float* a, const float* b, float t)
    {
        float c = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
        float bb[4] = { b[0], b[1], b[2], b[3] };
        if (c < 0.0f) { c = -c; for (int k = 0; k < 4; ++k) bb[k] = -bb[k]; }
        float s0, s1;
        if ((1.0f - c) > 0.0001f)
        {
            const float om = acosf(c), inv = 1.0f / sinf(om);
            s0 = sinf((1.0f - t) * om) * inv;
            s1 = sinf(t * om) * inv;
        }
        else { s0 = 1.0f - t; s1 = t; }
        for (int k = 0; k < 4; ++k) out[k] = s0 * a[k] + s1 * bb[k];
    }

    // The donor's quat_Mul: matrix(a*b) == matrix(a) * matrix(b).
    void QuatMul(float* q, const float* a, const float* b)
    {
        q[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
        q[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
        q[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
        q[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    }
}

namespace
{
    bool BuildPoseImpl(const CLTABCModel& model, unsigned int nAnim,
                       unsigned int nKeyframe, LTModelPose& out,
                       NodeHook pHook, void* pHookUser,
                       const Layer* pLayers, unsigned int nLayers,
                       unsigned int nKey1, float fFrac);
}

bool BuildPose(const CLTABCModel& model, unsigned int nAnim,
               unsigned int nKeyframe, LTModelPose& out,
               NodeHook pHook, void* pHookUser,
               const Layer* pLayers, unsigned int nLayers)
{
    return BuildPoseImpl(model, nAnim, nKeyframe, out, pHook, pHookUser,
                         pLayers, nLayers, nKeyframe, 0.0f);
}

bool BuildPoseBetween(const CLTABCModel& model, unsigned int nAnim,
                      unsigned int nKey0, unsigned int nKey1, float fFrac,
                      LTModelPose& out)
{
    return BuildPoseImpl(model, nAnim, nKey0, out, 0, 0, 0, 0, nKey1, fFrac);
}

namespace
{
bool BuildPoseImpl(const CLTABCModel& model, unsigned int nAnim,
                   unsigned int nKeyframe, LTModelPose& out,
                   NodeHook pHook, void* pHookUser,
                   const Layer* pLayers, unsigned int nLayers,
                   unsigned int nKey1, float fFrac)
{
    const std::vector<LTABCAnim>& anims = model.GetAnims();
    const std::vector<LTABCNode>& nodes = model.GetNodes();
    if (nAnim >= anims.size() || nodes.empty()) return false;

    const LTABCAnim& a = anims[nAnim];
    if (a.m_Transforms.size() != nodes.size()) return false;
    if (nKeyframe >= a.m_Keyframes.size()) return false;

    // ⚠️ MNODE_ROTATIONONLY -- A BONE TAKES ITS LENGTH FROM ITS OWN SKELETON.
    //
    // Node flag bit 1 says "only use ROTATION from the animation"; the
    // translation is the node's bind offset from its parent (donor
    // transformmaker.cpp:279, model.cpp SetNodeParentOffsets). Every bone of
    // NOLF's characters carries it -- the root and two control nodes do not --
    // because most of a character's animations are MERGED from a shared
    // skeleton (basemodel.abc) with different proportions. Taking the shared
    // skeleton's position keys put Cate's Head_node 0.73 units lower on her
    // neck while her Eyelid node landed where it was: the face slid away from
    // the lashes by about the height of an eye, and the lash pieces hung over
    // the iris as black wedges (M12S03; HANDOVER.md 5at).
    //
    // LT_NOROTONLY=1 uses the position keys for every node, as before.
    static const bool s_bNoRotOnly = (getenv("LT_NOROTONLY") != 0);
    static const bool s_bSaid = (s_bNoRotOnly
        ? (printf("[POSE] LT_NOROTONLY=1 -- ROTATIONONLY nodes take the ANIMATION's "
                  "position keys\n"), true) : true);
    (void)s_bSaid;

    // LT_NOANIMLAYERS=1 -- only the main animation, as before.
    static const bool s_bNoLayers = (getenv("LT_NOANIMLAYERS") != 0);
    static const bool s_bSaidL = (s_bNoLayers
        ? (printf("[POSE] LT_NOANIMLAYERS=1 -- extra trackers (blink, twitch, pitch) "
                  "are NOT blended\n"), true) : true);
    (void)s_bSaidL;
    if (s_bNoLayers) nLayers = 0;
    const std::vector<LTABCWeightSet>& sets = model.GetWeightSets();

    // Parents, for the bind offsets -- popped BEFORE reading, the order
    // AnimationIsPlausible learned the hard way.
    std::vector<int> parent(nodes.size(), -1);
    if (!s_bNoRotOnly)
    {
        std::vector<unsigned int> st, rem;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            while (!rem.empty() && rem.back() == 0) { rem.pop_back(); st.pop_back(); }
            if (!st.empty()) parent[i] = (int)st.back();
            if (!rem.empty()) --rem.back();
            if (nodes[i].m_nChildren) { st.push_back((unsigned int)i); rem.push_back(nodes[i].m_nChildren); }
        }
    }

    std::vector<float> locals((size_t)nodes.size() * 16);
    for (size_t n = 0; n < nodes.size(); ++n)
    {
        if (nKeyframe >= a.m_Transforms[n].size()) return false;
        const LTABCTransform& t = a.m_Transforms[n][nKeyframe];

        float quat[4] = { t.m_Quat[0], t.m_Quat[1], t.m_Quat[2], t.m_Quat[3] };
        float pos[3] = { t.m_Pos[0], t.m_Pos[1], t.m_Pos[2] };

        // ⚠️ BETWEEN TWO KEYS (BuildPoseBetween, HANDOVER 5cg) -- the donor's
        // tracker interpolates, slerp and lerp by the time between them.
        // Skipped entirely at fraction 0, so every existing caller is exact.
        if (fFrac > 0.0f && nKey1 != nKeyframe && nKey1 < a.m_Transforms[n].size())
        {
            const LTABCTransform& t1 = a.m_Transforms[n][nKey1];
            float q[4];
            QuatSlerp(q, quat, t1.m_Quat, fFrac);
            for (int k = 0; k < 4; ++k) quat[k] = q[k];
            for (int k = 0; k < 3; ++k) pos[k] += (t1.m_Pos[k] - pos[k]) * fFrac;
        }

        // The extra trackers, in the order they were added (see the header).
        for (unsigned int l = 0; l < nLayers; ++l)
        {
            const Layer& L = pLayers[l];
            if (L.m_nAnim >= anims.size() || L.m_nWeightSet < 0
                || (size_t)L.m_nWeightSet >= sets.size()) continue;
            const LTABCAnim& la = anims[L.m_nAnim];
            const std::vector<float>& wt = sets[(size_t)L.m_nWeightSet].m_Weights;
            if (la.m_Transforms.size() != nodes.size() || wt.size() != nodes.size()) continue;
            if (L.m_nKeyframe >= la.m_Transforms[n].size()) continue;

            const float w = wt[n];
            if (w == 0.0f) continue;
            // An additive layer AT its own frame 0 adds exactly nothing --
            // skipped rather than multiplied by q0^-1 * q0, whose rounding
            // nudged 52 pixels of every character by 1 level (the twitch
            // tracker sits on `base`, one key, for the whole game).
            if (w == 2.0f && L.m_nKeyframe == 0) continue;
            const LTABCTransform& now = la.m_Transforms[n][L.m_nKeyframe];
            if (w == 2.0f)
            {
                // ADDITIVE -- relative to the layer's own frame 0.
                const LTABCTransform& base = la.m_Transforms[n][0];
                const float inv0[4] = { -base.m_Quat[0], -base.m_Quat[1], -base.m_Quat[2], base.m_Quat[3] };
                float d[4], q[4];
                QuatMul(d, inv0, now.m_Quat);
                QuatMul(q, quat, d);
                for (int k = 0; k < 4; ++k) quat[k] = q[k];
                for (int k = 0; k < 3; ++k) pos[k] += now.m_Pos[k] - base.m_Pos[k];
            }
            else
            {
                float q[4];
                QuatSlerp(q, quat, now.m_Quat, w);
                for (int k = 0; k < 4; ++k) quat[k] = q[k];
                for (int k = 0; k < 3; ++k) pos[k] += (now.m_Pos[k] - pos[k]) * w;
            }
        }

        if (!s_bNoRotOnly && (nodes[n].m_nFlags & 2) && parent[n] >= 0)
        {
            // offset = translation of inverse(parentBind) * nodeBind; the bind
            // matrices are rigid (swept: orthonormal to 3e-07), so the inverse
            // is the transpose with the translation rotated back.
            const float* P = nodes[(size_t)parent[n]].m_Matrix;
            const float* N = nodes[n].m_Matrix;
            const float d[3] = { N[3] - P[3], N[7] - P[7], N[11] - P[11] };
            for (int k = 0; k < 3; ++k)
                pos[k] = P[0*4+k]*d[0] + P[1*4+k]*d[1] + P[2*4+k]*d[2];
        }
        LT3D::FromQuatPos(quat, pos, &locals[n * 16]);
    }

    out.m_Node.assign((size_t)nodes.size() * 16, 0.0f);

    // ⚠️ EXPERIMENT (LT_ABC_ABSOLUTE): are keyframe transforms LOCAL to the
    // parent, or already absolute in model space? Composing absolute
    // transforms applies every parent twice, which stretches bones -- the
    // exact symptom. The bone-length oracle decides.
    static const bool bAbsolute = (getenv("LT_ABC_ABSOLUTE") != 0);
    if (bAbsolute)
    {
        for (size_t n = 0; n < nodes.size(); ++n)
            std::memcpy(&out.m_Node[n * 16], &locals[n * 16], sizeof(float) * 16);
        return true;
    }

    ComposeNode(nodes, 0, 0, &locals[0], out, pHook, pHookUser);
    return true;
}
}   // namespace (BuildPoseImpl)

bool BuildBindPose(const CLTABCModel& model, LTModelPose& out)
{
    const std::vector<LTABCNode>& nodes = model.GetNodes();
    if (nodes.empty()) return false;

    // ⚠️ The stored node matrices are ALREADY the composed world transforms --
    // that is what the cross-check against keyframe composition established.
    // Composing them again would apply every parent twice.
    out.m_Node.assign((size_t)nodes.size() * 16, 0.0f);
    for (size_t n = 0; n < nodes.size(); ++n)
        std::memcpy(&out.m_Node[n * 16], nodes[n].m_Matrix, sizeof(float) * 16);
    return true;
}

bool AnimationIsPlausible(const CLTABCModel& model, unsigned int nAnim,
                          float fTolerance)
{
    const std::vector<LTABCNode>& nodes = model.GetNodes();
    if (nodes.size() < 2) return true;          // no bones to stretch

    if (nAnim >= model.GetAnims().size()) return false;
    const LTABCAnim& a = model.GetAnims()[nAnim];
    if (a.m_Keyframes.empty()) return false;

    // Parent of each node, from the depth-first child counts.
    //
    // ⚠️ POP EXHAUSTED SUBTREES **BEFORE** READING THE PARENT. This loop used
    // to assign `parent[i]` first and pop afterwards, so the first node after
    // any completed subtree was parented to the last node OF that subtree.
    //
    // ⚠️ IT ONLY GOES WRONG ON A BRANCHING SKELETON -- which is every
    // character and no doorknob. Measured on ARCH_BASE.ABC (25 nodes):
    //
    //     Right_armu_shoulder_node  parent=Left_arml_node    <- the left hand
    //     Left_legu_pelvis_node     parent=Right_arml_node
    //     Left_armu_shoulder_node   parent=Head_node
    //
    // and exactly those mis-parented nodes reported bone "stretch" of 68x,
    // 0.79 and 0.03 while every correctly-parented chain reported 0.0000.
    //
    // ⚠️ SO THIS FUNCTION WAS THE BUG IT WAS REPORTING. `ltscene.cpp` asks
    // AnimationIsPlausible whether to animate at all and falls back to the
    // bind pose when the answer is no -- so a broken parent walk here switched
    // off character animation for the whole game, and the evidence it produced
    // ("long animations are read incorrectly", HANDOVER.md) sent the
    // investigation to the ABC loader, which was correct all along.
    //
    // ⚠️ ComposeNode does NOT share this bug: it recurses over child counts
    // and never needed a parent array. The renderer's pose was always right.
    std::vector<int> parent(nodes.size(), -1);
    {
        std::vector<unsigned int> stack;
        std::vector<unsigned int> remaining;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            while (!remaining.empty() && remaining.back() == 0)
            { remaining.pop_back(); stack.pop_back(); }

            if (!stack.empty()) parent[i] = (int)stack.back();
            if (!remaining.empty()) --remaining.back();

            if (nodes[i].m_nChildren)
            { stack.push_back((unsigned int)i); remaining.push_back(nodes[i].m_nChildren); }
        }
    }

    LTModelPose bind;
    if (!BuildBindPose(model, bind)) return false;

    // ⚠️ WHAT COUNTS AS A BONE. A node that sits almost on top of its parent
    // is a CONTROL node, not a limb -- LithTech's "_zN_" nodes are exactly
    // this, and they carry the character's ROOT MOTION. Dividing a 100-unit
    // walk by a 1.4-unit rest length reports 68x "stretch" for data that is
    // perfectly correct, which is how ARCH_BASE.ABC came to look broken.
    //
    // Measured on that model with the parent walk fixed: EVERY limb reads
    // 0.0000 and the single offender is `_zN_LowerBody`, rest length 1.462.
    //
    // The floor is relative to the model's own size, because an absolute one
    // would mean something different for a doorknob and for a person.
    float fMin[3], fMax[3], fFloor = 0.0f;
    if (GetBounds(model, fMin, fMax))
    {
        const float dx = fMax[0]-fMin[0], dy = fMax[1]-fMin[1], dz = fMax[2]-fMin[2];
        fFloor = 0.04f * std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    std::vector<float> bindLen(nodes.size(), 0.0f);
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        if (parent[i] < 0) continue;
        const float* c = bind.Get((unsigned int)i);
        const float* p = bind.Get((unsigned int)parent[i]);
        float dx = c[3]-p[3], dy = c[7]-p[7], dz = c[11]-p[11];
        float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        bindLen[i] = (len >= fFloor) ? len : 0.0f;   // 0 => skipped below
    }

    LTModelPose pose;
    for (size_t k = 0; k < a.m_Keyframes.size(); ++k)
    {
        if (!BuildPose(model, nAnim, (unsigned int)k, pose)) return false;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            if (parent[i] < 0) continue;
            if (bindLen[i] < 1e-4f) continue;       // coincident nodes prove nothing
            const float* c = pose.Get((unsigned int)i);
            const float* p = pose.Get((unsigned int)parent[i]);
            float dx = c[3]-p[3], dy = c[7]-p[7], dz = c[11]-p[11];
            float len = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (std::fabs(len - bindLen[i]) / bindLen[i] > fTolerance) return false;
        }
    }
    return true;
}

bool GetBounds(const CLTABCModel& model, float* pMin, float* pMax)
{
    bool bAny = false;
    const std::vector<LTABCPiece>& pieces = model.GetPieces();
    for (size_t p = 0; p < pieces.size(); ++p)
    {
        if (pieces[p].m_Lods.empty()) continue;
        const std::vector<LTABCVertex>& vs = pieces[p].m_Lods[0].m_Verts;
        for (size_t v = 0; v < vs.size(); ++v)
        {
            const float* q = vs[v].m_Pos;
            if (!bAny)
            {
                for (int i = 0; i < 3; ++i) { pMin[i] = pMax[i] = q[i]; }
                bAny = true;
            }
            else
            {
                for (int i = 0; i < 3; ++i)
                {
                    if (q[i] < pMin[i]) pMin[i] = q[i];
                    if (q[i] > pMax[i]) pMax[i] = q[i];
                }
            }
        }
    }
    return bAny;
}

bool SocketTransform(const CLTABCModel& model, const LTModelPose& pose,
                     const char* pSocketName, bool bMirror, float* pOut16)
{
    if (!pSocketName || !pOut16) return false;
    const std::vector<LTABCSocket>& sk = model.GetSockets();
    for (size_t i = 0; i < sk.size(); ++i)
    {
        if (strcasecmp(sk[i].m_sName.c_str(), pSocketName) != 0) continue;

        float S[16];
        LT3D::FromQuatPos(sk[i].m_Rot, sk[i].m_Pos, S);
        const float* N = (sk[i].m_nNode < pose.Count()) ? pose.Get(sk[i].m_nNode) : 0;
        if (N) LT3D::Multiply(N, S, pOut16);
        else   std::memcpy(pOut16, S, sizeof(S));

        if (bMirror)
        {
            pOut16[1] = -pOut16[1]; pOut16[2] = -pOut16[2]; pOut16[3] = -pOut16[3];
            pOut16[4] = -pOut16[4]; pOut16[8] = -pOut16[8];
        }
        return true;
    }
    return false;
}

unsigned int KeyframeAt(const CLTABCModel& model, unsigned int nAnim,
                        float fSeconds, bool bLooping)
{
    const std::vector<LTABCAnim>& anims = model.GetAnims();
    if (nAnim >= anims.size() || anims[nAnim].m_Keyframes.empty()) return 0;
    const LTABCAnim& a = anims[nAnim];
    const unsigned int nLen = a.m_Keyframes.back().m_nTime;
    unsigned int t = (fSeconds > 0.0f) ? (unsigned int)(fSeconds * 1000.0f) : 0u;
    if (nLen > 0) t = bLooping ? (t % nLen) : (t > nLen ? nLen : t);
    unsigned int kf = 0;
    for (size_t k = 0; k < a.m_Keyframes.size(); ++k)
        if (a.m_Keyframes[k].m_nTime <= t) kf = (unsigned int)k;
    return kf;
}

void KeyframeSpan(const CLTABCModel& model, unsigned int nAnim,
                  float fSeconds, bool bLooping,
                  unsigned int* pK0, unsigned int* pK1, float* pFrac)
{
    *pK0 = *pK1 = 0; *pFrac = 0.0f;         // written on every path
    const std::vector<LTABCAnim>& anims = model.GetAnims();
    if (nAnim >= anims.size() || anims[nAnim].m_Keyframes.empty()) return;
    const LTABCAnim& a = anims[nAnim];
    const float fLen = (float)a.m_Keyframes.back().m_nTime;
    float t = (fSeconds > 0.0f) ? fSeconds * 1000.0f : 0.0f;
    if (fLen > 0.0f) t = bLooping ? fmodf(t, fLen) : (t > fLen ? fLen : t);
    unsigned int k0 = 0;
    for (size_t k = 0; k < a.m_Keyframes.size(); ++k)
        if ((float)a.m_Keyframes[k].m_nTime <= t) k0 = (unsigned int)k;
    unsigned int k1 = k0;
    float f = 0.0f;
    if ((size_t)k0 + 1 < a.m_Keyframes.size())
    {
        k1 = k0 + 1;
        const float t0 = (float)a.m_Keyframes[k0].m_nTime;
        const float t1 = (float)a.m_Keyframes[k1].m_nTime;
        if (t1 > t0) f = (t - t0) / (t1 - t0);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
    }
    *pK0 = k0; *pK1 = k1; *pFrac = f;
}

// See the header -- the position rule, with its own oracle.
bool SkinPosition(const CLTABCModel& model, const LTABCVertex& vt,
                  const LTModelPose& pose, float* out3)
{
    if (vt.m_Weights.empty() || pose.Count() != model.GetNodes().size()) return false;

    float p[3] = { 0.0f, 0.0f, 0.0f };
    float fSum = 0.0f;
    for (size_t w = 0; w < vt.m_Weights.size(); ++w)
    {
        const LTABCWeight& wt = vt.m_Weights[w];
        if (wt.m_nNode >= pose.Count()) continue;
        const float* M = pose.Get(wt.m_nNode);
        const float* o = wt.m_Offset;
        for (int k = 0; k < 3; ++k)
            p[k] += M[k*4+0]*o[0] + M[k*4+1]*o[1] + M[k*4+2]*o[2] + M[k*4+3] * wt.m_fBias;
        fSum += wt.m_fBias;
    }

    // ⚠️ DIVIDE BY THE SUM OF THE BIASES -- they do not always add up to 1.
    // 9,719 shipped vertices in 197 models do not; Cate's face has single-
    // weight vertices at Head_node/0.95 and nothing else. The offset is
    // pre-scaled by the bias, so an undivided sum is 0.95 * p: the vertex is
    // pulled 5% of its distance toward the model origin -- about 2 units at
    // head height, on triangles 1-2 units across. That stretched under-eye
    // texture down over the cheek and is the pale wedge (HANDOVER.md 5ar).
    // The bind-pose oracle decides it: undivided, 94 of Cate's vertices miss
    // their stored position by >0.1 (worst 2.083); divided, 0 (worst 0.038).
    if (!NoBiasNorm() && fSum > 1e-6f)
    { const float fInv = 1.0f / fSum; for (int k = 0; k < 3; ++k) p[k] *= fInv; }

    for (int k = 0; k < 3; ++k) out3[k] = p[k];
    return true;
}

// See the header. The whole reason this is a function is that it has an
// oracle; see HANDOVER.md 5aq for the 92-degree mean error it was hiding.
bool SkinNormal(const CLTABCModel& model, const LTABCVertex& vt,
                const LTModelPose& pose, float* out3)
{
    const std::vector<LTABCNode>& nodes = model.GetNodes();
    if (vt.m_Weights.empty() || pose.Count() != nodes.size()) return false;

    const bool bNoInv = NoBindInverse();
    float ns[3] = { 0.0f, 0.0f, 0.0f };
    for (size_t w = 0; w < vt.m_Weights.size(); ++w)
    {
        const LTABCWeight& wt = vt.m_Weights[w];
        if (wt.m_nNode >= pose.Count()) continue;
        const float* M = pose.Get(wt.m_nNode);

        // Into the bone's own frame first: R_bind^T * n. ⚠️ The transpose is
        // the inverse only because these are rotations -- swept over all 695
        // shipped models and all 8,770 node matrices, worst departure from
        // orthonormal 3.03e-07.
        float b3[3];
        if (bNoInv)
        { b3[0] = vt.m_Normal[0]; b3[1] = vt.m_Normal[1]; b3[2] = vt.m_Normal[2]; }
        else
        {
            const float* B = nodes[wt.m_nNode].m_Matrix;
            for (int k = 0; k < 3; ++k)
                b3[k] = B[0*4+k]*vt.m_Normal[0] + B[1*4+k]*vt.m_Normal[1]
                      + B[2*4+k]*vt.m_Normal[2];
        }

        for (int k = 0; k < 3; ++k)
            ns[k] += (M[k*4+0]*b3[0] + M[k*4+1]*b3[1] + M[k*4+2]*b3[2]) * wt.m_fBias;
    }

    const float len = sqrtf(ns[0]*ns[0] + ns[1]*ns[1] + ns[2]*ns[2]);
    if (len <= 1e-6f) return false;
    for (int k = 0; k < 3; ++k) out3[k] = ns[k] / len;
    return true;
}

void SkinSwitches(bool* pNoSkinNormal, bool* pNoBindInverse, bool* pNoBiasNorm)
{
    if (pNoSkinNormal)  *pNoSkinNormal  = NoSkinNormal();
    if (pNoBindInverse) *pNoBindInverse = NoBindInverse();
    if (pNoBiasNorm)    *pNoBiasNorm    = NoBiasNorm();
}

bool CpuOnlyInstrument()
{
    static const bool b = (getenv("LT_HIDEPIECE") && *getenv("LT_HIDEPIECE"))
                       || getenv("LT_LIGHTONLY") || getenv("LT_SKINPERCORNER")
                       || getenv("LT_BIGTRI") || getenv("LT_NOMIRRORU");
    return b;
}

LTModelDrawStats Draw(const LT3DTarget& target, const CLTABCModel& model,
                      const float* pMVP, const float* pNormalMatrix,
                      const LT3DTexture* pSkins, int nSkins,
                      int nLod, bool bCull, const LTModelPose* pPose,
                      ISink* pSink, bool bMirrorU,
                      const LTModelLighting* pLighting,
                      int nMirrorModel)
{
    const bool bMirror = (nMirrorModel < 0) ? MirrorX() : (nMirrorModel != 0);

    LTModelDrawStats st;
    st.m_nTriangles = st.m_nPixels = st.m_nCulled = 0;

    const std::vector<LTABCPiece>& pieces = model.GetPieces();

    for (size_t p = 0; p < pieces.size(); ++p)
    {
        const LTABCPiece& pc = pieces[p];
        if (nLod < 0 || (size_t)nLod >= pc.m_Lods.size()) continue;

        // ⚠️ LT_HIDEPIECE="<substring>" -- draw the model without the pieces
        // whose name contains it. The way to find out WHICH geometry a wrong
        // shape on screen belongs to: hide a candidate and see whether the
        // shape goes with it. Guessing from a screenshot is how a texture bug
        // and a skinning bug get confused for each other.
        {
            static const char* pHide = getenv("LT_HIDEPIECE");
            if (pHide && *pHide && pc.m_sName.find(pHide) != std::string::npos)
                continue;
        }

        const LTABCLod& lod = pc.m_Lods[nLod];

        // ⚠️ LT_LIGHTONLY=1 -- draw with NO TEXTURE, so the image is nothing
        // but the light value. It separates the two explanations for a flat
        // patch on a model that no amount of staring at a lit render can:
        // if the patch is still there, it is the LIGHTING; if it vanishes, it
        // is the TEXTURE.
        static const bool s_bLightOnly = (getenv("LT_LIGHTONLY") != 0);

        const LT3DTexture* pTex = 0;
        if (!s_bLightOnly && pSkins && pc.m_nTexture < (unsigned short)nSkins)
        {
            const LT3DTexture* pCand = &pSkins[pc.m_nTexture];
            if (pCand->m_pRGBA && pCand->m_Width > 0) pTex = pCand;
        }

        // ⚠️ EACH VERTEX ONCE (2026-10-12, HANDOVER 5bu). Everything a corner
        // needs except its UV -- the skinned position, the mirror, the clip
        // transform, the skinned normal, the light -- depends on the VERTEX
        // alone, and it was computed per FACE CORNER: 137,532 corners for
        // 23,661 vertices across M02S01's 66 game models per frame, every
        // vertex 5.8 times, 10.1 ms. Now once, kept for this call only, by the
        // same code in the same order, so the result is bit-identical.
        // LT_SKINPERCORNER=1 = per corner again (says so).
        static const bool s_bPerCorner = []() {
            const bool b = getenv("LT_SKINPERCORNER") != 0;
            if (b) { printf("[MODEL] LT_SKINPERCORNER=1 -- every face corner skinned and lit on its own\n"); fflush(stdout); }
            return b; }();
        struct VCache { float m_Clip[4]; float m_Pos[3]; float m_RGB[3]; };
        thread_local std::vector<VCache>       s_V;
        thread_local std::vector<unsigned int> s_Stamp;
        thread_local unsigned int              s_Gen = 0;
        if (s_Stamp.size() < lod.m_Verts.size())
        {
            s_Stamp.resize(lod.m_Verts.size(), 0);
            s_V.resize(lod.m_Verts.size());
        }
        if (++s_Gen == 0) { std::fill(s_Stamp.begin(), s_Stamp.end(), 0u); s_Gen = 1; }

        auto vertex = [&](unsigned short idx, VCache& vc)
        {
            const LTABCVertex& vt = lod.m_Verts[idx];

            // Bind pose unless a pose was supplied.
            const float* pPos = vt.m_Pos;
            float skinned[3];
            // ⚠️ THE BIAS WEIGHTS ONLY THE BONE'S TRANSLATION, and the sum
            // is divided by the total bias -- see SkinPosition, which is a
            // function so the bind-pose oracle can assert it.
            //
            //     pos = ( SUM R_i * offset_i  +  SUM bias_i * t_i ) / SUM bias_i
            //
            // The exporter baked each weight's bias into its offset. This
            // once multiplied the WHOLE transformed point by the bias --
            // identical for a vertex on one bone, which is why every rigid
            // part looked right and every JOINT did not (IntHero_Action
            // mean 3.255 -> 0.013 at the bind pose). The division is the
            // second half of the same rule and fixed the rest.
            if (pPose && SkinPosition(model, vt, *pPose, skinned))
                pPos = skinned;

            float mirrored[3];
            if (bMirror)
            {
                mirrored[0] = -pPos[0];
                mirrored[1] =  pPos[1];
                mirrored[2] =  pPos[2];
                pPos = mirrored;
            }

            LT3D::TransformPoint(pMVP, pPos, vc.m_Clip);
            for (int q2 = 0; q2 < 3; ++q2) vc.m_Pos[q2] = pPos[q2];


            // ⚠️ THE NORMAL IS SKINNED TOO, AND IT WAS NOT.
            //
            // The POSITION above is moved by the bone matrices; the normal
            // used to go straight from the file into the light equation
            // with only the object matrix applied. So on any posed model
            // every vertex was lit as though the skeleton were still in
            // its bind pose. Wherever a bone had turned -- the head in a
            // crouch-and-aim, say -- the geometry faced one way and its
            // normals another, and the shading came out as flat bands of
            // the wrong brightness with hard edges along the triangles.
            //
            // Measured on Hero_CasualM1 animation 151 keyframe 11: a pale
            // slab across the eyes and cheeks, ~35% brighter than the skin
            // beside it AT THE SAME HUE -- which is what says "lighting"
            // rather than "texture". It is fixed to the surface and does
            // not move with the camera (0.6% of model pixels at 0, 0.5 and
            // 2 degrees of yaw), which is what ruled out z-fighting.
            //
            // ⚠️ ROTATION ONLY -- no translation column -- and weighted by
            // the bias, because m_Offset is pre-scaled by the bias while a
            // normal is not.
            //
            // ⚠️⚠️ AND THE INVERSE BIND ROTATION, WHICH THE FIRST VERSION
            // OF THIS LEFT OUT. A position and a normal are stored in
            // DIFFERENT FRAMES, and the sum that is right for one is wrong
            // for the other:
            //
            //   m_Offset  is the vertex in the BONE's own frame -- the
            //             exporter already divided out the bind pose --
            //             so `SUM(R_i * offset_i)` is correct as it
            //             stands.
            //   m_Normal  is in MODEL space, like m_Pos. Multiplying it by
            //             the bone's world matrix applies the bind
            //             rotation A SECOND TIME.
            //
            // Measured on Hero_CasualM1 with the bind-pose oracle in
            // `harness/modelview` (skinning at the bind pose must give the
            // file's own normal back, over 840 weighted vertices):
            //
            //     R_i * n                  mean 92.807 deg, worst 172.286
            //     R_i * R_bind^-1 * n      mean  0.007 deg, worst   0.031
            //
            // ⚠️ THE TRANSPOSE IS THE INVERSE ONLY BECAUSE THESE MATRICES
            // ARE ROTATIONS, and that was swept rather than assumed: over
            // all 695 shipped models and all 8,770 node matrices the worst
            // departure from orthonormal is 3.03e-07, and none exceeds
            // 1e-3. If a model ever carries scale in a node matrix this
            // needs the inverse TRANSPOSE instead, and the transpose here
            // would silently drop it.
            float nSrc[3] = { vt.m_Normal[0], vt.m_Normal[1], vt.m_Normal[2] };
            if (pPose && !NoSkinNormal())
                SkinNormal(model, vt, *pPose, nSrc);

            // ⚠️ A MIRRORED MODEL MIRRORS ITS NORMALS. The position above
            // flips X; a normal that did not would light the mirrored half
            // as though it faced the other way.
            if (bMirror) nSrc[0] = -nSrc[0];

            float n[3];
            RotateNormal(pNormalMatrix, nSrc, n);

            if (pLighting)
            {
                // The engine's rule -- see LTModelLighting in the header.
                const float nl = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                if (nl > 1e-6f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
                float c[3] = { pLighting->m_Ambient[0], pLighting->m_Ambient[1],
                               pLighting->m_Ambient[2] };
                for (int li = 0; li < pLighting->m_nLights; ++li)
                {
                    const float* L = pLighting->m_Dir[li];
                    const float dd = -(n[0]*L[0] + n[1]*L[1] + n[2]*L[2]);
                    if (dd <= 0.0f) continue;
                    for (int k = 0; k < 3; ++k) c[k] += pLighting->m_Color[li][k] * dd;
                }
                for (int k = 0; k < 3; ++k) if (c[k] > 1.0f) c[k] = 1.0f;
                vc.m_RGB[0] = c[0]; vc.m_RGB[1] = c[1]; vc.m_RGB[2] = c[2];
            }
            else
            {
                float d = Dot3(n, kLightDir);
                if (d < 0.0f) d = -d;          // two-sided lighting
                float l = kAmbient + (1.0f - kAmbient) * d;
                if (l > 1.0f) l = 1.0f;

                vc.m_RGB[0] = vc.m_RGB[1] = vc.m_RGB[2] = l;
            }
        };

        for (size_t f = 0; f < lod.m_Faces.size(); ++f)
        {
            const LTABCFace& face = lod.m_Faces[f];
            LT3DVert tri[3];
            float    mpos[3][3];        // model space, for the GPU backend
            bool bOK = true;

            for (int i = 0; i < 3; ++i)
            {
                unsigned short idx = face.m_Verts[i].m_nIndex;
                if (idx >= lod.m_Verts.size()) { bOK = false; break; }

                VCache  vcOne;
                VCache* pVC = &vcOne;
                if (s_bPerCorner) vertex(idx, vcOne);
                else
                {
                    pVC = &s_V[idx];
                    if (s_Stamp[idx] != s_Gen) { vertex(idx, *pVC); s_Stamp[idx] = s_Gen; }
                }
                for (int k = 0; k < 4; ++k) tri[i].m_Clip[k] = pVC->m_Clip[k];
                for (int k = 0; k < 3; ++k) mpos[i][k] = pVC->m_Pos[k];
                tri[i].m_R = pVC->m_RGB[0]; tri[i].m_G = pVC->m_RGB[1]; tri[i].m_B = pVC->m_RGB[2];

                // ⚠️ UVs come from the FACE, not the vertex. ABC stores them
                // per face-corner, so one vertex can carry different texture
                // coordinates in different faces -- which is exactly how a
                // texture seam is expressed. Reading them from the vertex
                // would weld every seam and smear the skin around it.
                // ⚠️ LT_NOMIRRORU=1 forces the U flip OFF, as an instrument.
                // The flip is tied to the scene mirror in the world pass and
                // hard-false in the menu pass, and that asymmetry has never
                // been explained (HANDOVER §5a). A switch makes "is this model
                // sampling the wrong half of its texture?" a one-run question
                // instead of a rebuild.
                const bool bU = bMirrorU && !NoMirrorU();
                tri[i].m_U = bU ? -face.m_Verts[i].m_fU
                                :  face.m_Verts[i].m_fU;
                tri[i].m_V = face.m_Verts[i].m_fV;
            }

            if (!bOK) continue;

            // ⚠️ One decision path, two backends -- see ISink in the header.
            if (pSink)
            {
                pSink->Tri(pTex, mpos, tri);
                ++st.m_nTriangles;
                continue;
            }

            ++st.m_nTriangles;

            // ⚠️ NAME THE TRIANGLE BEFORE DRAWING IT -- see LT3DTarget's probe
            // buffers. The rasteriser stamps this id at every pixel the
            // triangle wins, so a patch on a finished render can be traced
            // back to the faces that painted it and to the UVs they carry.
            // +1 so that 0 stays "nothing drew here".
            if (!LT3D::g_bProbeHold)
                LT3D::g_nProbeID = (unsigned)(((p & 0xFFFu) << 20)
                                            | ((f + 1) & 0xFFFFFu));

            int n = LT3D::DrawTriangle(target, pTex, tri[0], tri[1], tri[2], bCull);
            st.m_nPixels += n;
            if (n == 0) ++st.m_nCulled;

            // ⚠️ THE WEDGE, DETECTED AS A NUMBER. A character triangle that
            // paints a large share of the whole screen is the screen-wide
            // white wedge -- one triangle with a corner in the wrong place.
            // This needs no screenshot and no eyes, which is the point: it can
            // be swept across every level headlessly, and it names the model,
            // the piece and the triangle's clip-space corners so the cause is
            // visible in the same line.
            {
                static const float s_fShare = getenv("LT_BIGTRI")
                    ? (float)atof(getenv("LT_BIGTRI")) : 0.0f;
                if (s_fShare > 0.0f && target.m_Width > 0 && target.m_Height > 0)
                {
                    const double area = (double)target.m_Width * target.m_Height;
                    if ((double)n > area * s_fShare)
                    {
                        // ⚠️ The cap is high because the MENU's own boxes are
                        // legitimately screen-filling and would otherwise use
                        // up a small budget before the level is even loaded.
                        static unsigned s_nSaid = 0;
                        if (s_nSaid < 400)
                        {
                            ++s_nSaid;
                            printf("[BIGTRI] %.0f%% of the screen from ONE triangle"
                                   "  piece %-20s  clip w: %.2f %.2f %.2f   z+w: %.2f %.2f %.2f\n",
                                   100.0 * (double)n / area, pc.m_sName.c_str(),
                                   tri[0].m_Clip[3], tri[1].m_Clip[3], tri[2].m_Clip[3],
                                   tri[0].m_Clip[2] + tri[0].m_Clip[3],
                                   tri[1].m_Clip[2] + tri[1].m_Clip[3],
                                   tri[2].m_Clip[2] + tri[2].m_Clip[3]);
                            fflush(stdout);
                        }
                    }
                }
            }
        }
    }

    return st;
}

} // namespace LTModelDraw
