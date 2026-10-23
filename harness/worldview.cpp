// ----------------------------------------------------------------------- //
//
// worldview.cpp -- renders a NOLF level's world geometry to a PPM.
//
// ⚠️ THIS IS THE REAL TEST OF THE WORLD READER. Unit checks say the bytes are
// accounted for and the numbers are in range; only drawing it says the level
// is actually there. A wrong polygon winding, a transposed axis or a mixed-up
// point index all pass every check in enginetest_world.cpp and are obvious the
// moment you look at a picture.
//
// Only world models whose bodies passed BOTH gates are drawn. The rest are
// counted and reported -- a partial level must not masquerade as a whole one.
//
// USAGE
//   worldview <dirs> <world.dat> <out.ppm> [size] [yaw] [pitch] [modelName]
//
// Textured through each surface's O/P/Q projection, lit by the per-vertex RGB
// the file carries. Pass a model name to frame ONE model -- at whole-level zoom
// a pixel spans several texels and point sampling turns any texture into
// speckle, which reads as broken UVs but is not (there is no mip selection).
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltworld.h"
#include "lt3d.h"
#include "ltfilemgr.h"
#include "ltdtx.h"
#include "ltabc.h"
#include "ltmodeldraw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <ctype.h>

// ----------------------------------------------------------------------- //
//
// The texture cache. Worlds share textures heavily -- M01S01 names 47 distinct
// files across 144 models -- so decoding per model would decode the same DTX
// dozens of times.
//
// ⚠️ The RGBA must outlive every LT3DTexture that points into it, so the bytes
// live in the cache and never in a local. A vector that reallocates would
// dangle every texture handed out before it grew, which is why the store is a
// map of pointers rather than a vector of values.
//
// ----------------------------------------------------------------------- //

struct CachedTex
{
    // ⚠️ One decoded buffer PER MIP, each owned here for the same lifetime
    // reason as the base level: LT3DTexture holds raw pointers into them.
    std::vector<unsigned char>  m_RGBA;
    std::vector<unsigned char>  m_MipRGBA[LT3DTexture::kMaxMips];
    LT3DTexture                 m_Tex;
    bool                        m_bValid;
};

static std::map<std::string, CachedTex*> g_Textures;

static const LT3DTexture* GetTexture(CLTFileMgr& fm, const std::string& sName)
{
    std::map<std::string, CachedTex*>::iterator it = g_Textures.find(sName);
    if (it != g_Textures.end())
        return it->second->m_bValid ? &it->second->m_Tex : 0;

    CachedTex* pC = new CachedTex();
    pC->m_bValid = false;
    pC->m_Tex.m_pRGBA = 0;
    pC->m_Tex.m_Width = pC->m_Tex.m_Height = 0;
    pC->m_Tex.m_bAlphaTest = false;

    CLTDtx dtx;
    if (dtx.LoadThroughFileMgr(&fm, sName.c_str()) && dtx.DecodeMip(0, pC->m_RGBA)
        && !pC->m_RGBA.empty())
    {
        pC->m_Tex.m_pRGBA = &pC->m_RGBA[0];
        pC->m_Tex.m_Width = (int)dtx.GetMip(0)->m_nWidth;
        pC->m_Tex.m_Height = (int)dtx.GetMip(0)->m_nHeight;
        pC->m_Tex.m_bAlphaTest = dtx.AlphaIsMeaningful();
        pC->m_bValid = pC->m_Tex.m_Width > 0 && pC->m_Tex.m_Height > 0;

        // ---- the mip chain, which the DTX already carries ---------------- //
        //
        // ⚠️ Not generated -- SHIPPED. Every DTX stores its own chain, built by
        // the game's tools, so there is nothing to filter here and no chance of
        // a downsample that disagrees with what the game showed.
        //
        // ⚠️ Stops at the first level that is not a power of two. The sampler
        // wraps by masking, so a 3-wide level would fetch the wrong texel
        // rather than fail; refusing the tail of the chain is the safe way to
        // be wrong-free rather than approximately right.
        unsigned int nMips = dtx.GetNumMipmaps();
        if (nMips > (unsigned int)LT3DTexture::kMaxMips)
            nMips = (unsigned int)LT3DTexture::kMaxMips;

        unsigned int nGood = 0;
        for (unsigned int k = 0; k < nMips; ++k)
        {
            const CLTDtx::Mip* pM = dtx.GetMip(k);
            if (!pM || pM->m_nWidth == 0 || pM->m_nHeight == 0) break;
            if ((pM->m_nWidth & (pM->m_nWidth - 1)) != 0) break;
            if ((pM->m_nHeight & (pM->m_nHeight - 1)) != 0) break;
            if (!dtx.DecodeMip(k, pC->m_MipRGBA[k]) || pC->m_MipRGBA[k].empty()) break;

            pC->m_Tex.m_pMipRGBA[k] = &pC->m_MipRGBA[k][0];
            pC->m_Tex.m_MipW[k] = (int)pM->m_nWidth;
            pC->m_Tex.m_MipH[k] = (int)pM->m_nHeight;
            ++nGood;
        }
        // LT_WV_NOMIP=1 keeps the chain decoded but hides it from the sampler,
        // so the two paths can be measured on identical geometry.
        pC->m_Tex.m_nMips = getenv("LT_WV_NOMIP") ? 0 : (int)nGood;
    }

    g_Textures[sName] = pC;
    return pC->m_bValid ? &pC->m_Tex : 0;
}

// ----------------------------------------------------------------------- //
//
// Objects: the props the level places, as opposed to its architecture.
//
// ⚠️ The world file names both halves. An object carries `Filename` (an .abc)
// and `Skin` (a .dtx) as ordinary string properties -- in M01S01, 82 objects
// across 6 classes (PropType, DoorKnob, Intelligence, AmmoBox, Prop, ZipHook)
// referencing 20 distinct models. Nothing here is inferred from a class name.
//
// ⚠️ What is NOT here: characters. Cate, guards and any other AI are spawned
// by GAME CODE, not by a world property, so no amount of reading the .dat will
// place them. An empty-looking level is the correct result for this reader.
//
// ----------------------------------------------------------------------- //

// ⚠️ A model can have SEVERAL skins, and the property is one string. `Skin`
// holds a ";"-separated list -- "Props\Skins\Tree_04.dtx ; Tree_04t.dtx" --
// one entry per texture index the pieces use. Loading only the first draws
// every piece that indexes the second UNTEXTURED, which comes out pure white:
// the trees in M01S01 rendered as big white stars until this was split.
enum { kMaxSkins = 8 };

struct CachedModel
{
    CLTABCModel                 m_Model;
    std::vector<unsigned char>  m_SkinRGBA[kMaxSkins];
    LT3DTexture                 m_Skins[kMaxSkins];
    int                         m_nSkins;
    bool                        m_bValid;
};

static std::map<std::string, CachedModel*> g_Models;

static CachedModel* GetModel(CLTFileMgr& fm, const std::string& sModel,
                             const std::string& sSkin)
{
    std::string sKey = sModel + "|" + sSkin;
    std::map<std::string, CachedModel*>::iterator it = g_Models.find(sKey);
    if (it != g_Models.end()) return it->second->m_bValid ? it->second : 0;

    CachedModel* pC = new CachedModel();
    pC->m_nSkins = 0;
    pC->m_bValid = pC->m_Model.LoadThroughFileMgr(&fm, sModel.c_str());

    if (pC->m_bValid && !sSkin.empty())
    {
        // Split on ';' and trim -- the shipped strings carry spaces around it.
        size_t a = 0;
        while (a <= sSkin.size() && pC->m_nSkins < kMaxSkins)
        {
            size_t b = sSkin.find(';', a);
            if (b == std::string::npos) b = sSkin.size();
            std::string one = sSkin.substr(a, b - a);
            a = b + 1;

            size_t f = one.find_first_not_of(" \t");
            size_t l = one.find_last_not_of(" \t");
            if (f == std::string::npos) continue;
            one = one.substr(f, l - f + 1);
            if (one.empty()) continue;

            int k = pC->m_nSkins;
            CLTDtx dtx;
            if (dtx.LoadThroughFileMgr(&fm, one.c_str())
                && dtx.DecodeMip(0, pC->m_SkinRGBA[k]) && !pC->m_SkinRGBA[k].empty())
            {
                pC->m_Skins[k].m_pRGBA = &pC->m_SkinRGBA[k][0];
                pC->m_Skins[k].m_Width = (int)dtx.GetMip(0)->m_nWidth;
                pC->m_Skins[k].m_Height = (int)dtx.GetMip(0)->m_nHeight;
                pC->m_Skins[k].m_bAlphaTest = dtx.AlphaIsMeaningful();
            }
            // ⚠️ Count it either way. The skins are indexed BY POSITION, so a
            // failed load must still occupy its slot or every later piece
            // silently takes the wrong texture.
            ++pC->m_nSkins;
        }
    }

    g_Models[sKey] = pC;
    return pC->m_bValid ? pC : 0;
}

// Row-major, translation in column 3 -- the layout LT3D::Multiply and
// TransformPoint already use.
//
// ⚠️ Yaw is Euler[1], which is not a guess: the inside camera uses the same
// index on GameStartPoint's rotation and produces a view down the corridor
// rather than into a wall.
static void WorldMatrix(const float* pos, const float* eul, float* out)
{
    float cy = cosf(eul[1]), sy = sinf(eul[1]);
    float cx = cosf(eul[0]), sx = sinf(eul[0]);
    float cz = cosf(eul[2]), sz = sinf(eul[2]);

    // R = Ry * Rx * Rz
    float m00 =  cy*cz + sy*sx*sz, m01 = -cy*sz + sy*sx*cz, m02 = sy*cx;
    float m10 =  cx*sz,            m11 =  cx*cz,            m12 = -sx;
    float m20 = -sy*cz + cy*sx*sz, m21 =  sy*sz + cy*sx*cz, m22 = cy*cx;

    LT3D::Identity(out);
    out[0] = m00; out[1] = m01; out[2]  = m02; out[3]  = pos[0];
    out[4] = m10; out[5] = m11; out[6]  = m12; out[7]  = pos[1];
    out[8] = m20; out[9] = m21; out[10] = m22; out[11] = pos[2];
}

// ⚠️ Which world models the ENGINE would draw, and one of these two rules is
// a heuristic while the other is not. Said plainly rather than blurred:
//
//   * VisBSP is excluded BY FLAG -- WIF_VISBSP, a near-duplicate of the main
//     world kept for visibility, which z-fights it if both are drawn.
//
//   * AIVolume brushes are excluded BY NAME, and there is no flag to use.
//     They are ordinary world models -- flags 2, WIF_MOVEABLE, identical to a
//     door or a window -- textured with DEdit's placeholder `Tex\AI.dtx`,
//     whose bitmap literally reads "AI". The engine never draws them because
//     the server-side AIVolume object that owns each one has no visible client
//     representation, and that relationship is not in the world file's model
//     table at all. M01S01 has 78 of them and they fill the corridors.
//
// ⚠️ So this is a VIEWER heuristic standing in for object ownership the
// renderer does not model yet, not a fact about the format.
static bool Drawable(const LTWorldModelHeader& m, unsigned int nVisBspFlag)
{
    if (m.m_nInfoFlags & nVisBspFlag) return false;
    if (m.m_sName.compare(0, 8, "AIVolume") == 0) return false;
    return true;
}

// ----------------------------------------------------------------------- //
//
// Draw one world model. Split out so the SKY can be drawn as its own pass with
// its own view matrix -- the sky is the same kind of geometry as everything
// else, just seen from a different place.
//
// ----------------------------------------------------------------------- //

struct DrawStats { int nPix, nDrawn, nTextured, nUntextured, nSkySurf; };

static void DrawModel(const LT3DTarget& t, const float* MVP,
                      const LTWorldModelHeader& M,
                      const std::vector<const LT3DTexture*>& tex,
                      bool bNoLight, unsigned int kSURF_SKY,
                      unsigned int kSURF_INVISIBLE, unsigned int kSURF_BRIGHT,
                      DrawStats& st)
{
    const float L[3] = { -0.40824829f, 0.81649658f, 0.40824829f };
    for (size_t q = 0; q < M.m_Polys.size(); ++q)
    {
        const LTWorldPoly& po = M.m_Polys[q];
        if (po.m_Verts.size() < 3) continue;

        const LT3DTexture* pTex = 0;
        const LTWorldSurface* pSurf = 0;
        if (po.m_nSurface < M.m_Surfaces.size())
        {
            pSurf = &M.m_Surfaces[po.m_nSurface];

            // ⚠️ A SKY PORTAL IS NOT A TEXTURED SURFACE. The engine draws
            // the skybox through it; the texture on disk is DEdit's
            // placeholder, and it says so -- the bitmap literally reads
            // "SKY". Drawing it puts a cyan-and-yellow slab where the sky
            // belongs, which is what the first inside-the-level render
            // showed. Skipped, the same as the engine does.
            if (pSurf->m_nFlags & kSURF_SKY)   { ++st.nSkySurf; continue; }
            if (pSurf->m_nFlags & kSURF_INVISIBLE) { ++st.nSkySurf; continue; }

            if (pSurf->m_nTexture < tex.size()) pTex = tex[pSurf->m_nTexture];
        }
        if (pTex) ++st.nTextured; else ++st.nUntextured;

        // Fan-triangulate. World polygons are convex by construction.
        for (size_t k = 1; k + 1 < po.m_Verts.size(); ++k)
        {
            const size_t vi[3] = { 0, k, k + 1 };
            LT3DVert tri[3];
            bool bOK = true;
            for (int v = 0; v < 3; ++v)
            {
                const LTWorldPolyVert& pv = po.m_Verts[vi[v]];
                if (pv.m_nPoint >= M.m_Points.size()) { bOK = false; break; }
                const LTWorldPoint& pt = M.m_Points[pv.m_nPoint];
                LT3D::TransformPoint(MVP, pt.m_Pos, tri[v].m_Clip);

                if (pTex && pSurf)
                {
                    float d[3] = { pt.m_Pos[0] - pSurf->m_O[0],
                                   pt.m_Pos[1] - pSurf->m_O[1],
                                   pt.m_Pos[2] - pSurf->m_O[2] };
                    float u = d[0]*pSurf->m_P[0] + d[1]*pSurf->m_P[1] + d[2]*pSurf->m_P[2];
                    float vv = d[0]*pSurf->m_Q[0] + d[1]*pSurf->m_Q[1] + d[2]*pSurf->m_Q[2];
                    // ⚠️ Negated -- see ltworldrender.cpp. Only text reveals
                    // it; M01S01's door plate read "82" before this flip.
                    tri[v].m_U = -u / (float)pTex->m_Width;
                    tri[v].m_V = vv / (float)pTex->m_Height;

                    // ⚠️ SURF_BRIGHT (1<<5) means FULLBRIGHT -- the surface
                    // ignores lighting entirely. All six skybox faces carry
                    // it (flags 0x121), and honouring it is not optional: an
                    // unlit brush stores vertex RGB 255, which the 1/128
                    // scale below turns into 2.0 and saturates the texture to
                    // flat white. That is exactly what the first sky render
                    // produced -- a white sky over a blue-sky texture.
                    //
                    // ⚠️ LT_WV_NOLIGHT=1 drops the baked lighting so a
                    // suspect surface can be judged on its TEXTURE alone.
                    // The two are multiplied, so a wrong texture and a
                    // wrong light produce the same wrong pixel.
                    if (bNoLight || (pSurf->m_nFlags & kSURF_BRIGHT))
                    {
                        tri[v].m_R = tri[v].m_G = tri[v].m_B = 1.0f;
                    }
                    else
                    {
                        tri[v].m_R = pv.m_RGB[0] * (1.0f / 128.0f);
                        tri[v].m_G = pv.m_RGB[1] * (1.0f / 128.0f);
                        tri[v].m_B = pv.m_RGB[2] * (1.0f / 128.0f);
                    }
                }
                else
                {
                    // No texture resolved -- fall back to the shaded grey
                    // so a missing DTX reads as "untextured", not "missing".
                    float dd = pt.m_Normal[0]*L[0] + pt.m_Normal[1]*L[1] + pt.m_Normal[2]*L[2];
                    if (dd < 0) dd = -dd;
                    float s = 0.30f + 0.70f * dd;
                    tri[v].m_U = tri[v].m_V = 0.0f;
                    tri[v].m_R = s * 0.86f; tri[v].m_G = s * 0.89f; tri[v].m_B = s;
                }
            }
            if (!bOK) continue;
            st.nPix += LT3D::DrawTriangle(t, pTex, tri[0], tri[1], tri[2], false);
            ++st.nDrawn;
        }
    }
}

static bool WritePPM(const char* p, const unsigned char* rgba, int w, int h)
{
    FILE* f = fopen(p, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) fwrite(rgba + (size_t)i * 4, 1, 3, f);
    fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: worldview <dirs> <world> <out.ppm> [size] [yaw] [pitch]\n"); return 2; }

    std::vector<std::string> dirs;
    { std::string s(argv[1]); size_t a = 0;
      while (a <= s.size()) { size_t b = s.find(':', a); if (b == std::string::npos) b = s.size();
        if (b > a) dirs.push_back(s.substr(a, b - a)); a = b + 1; } }
    std::vector<const char*> pd;
    for (size_t i = 0; i < dirs.size(); ++i) pd.push_back(dirs[i].c_str());

    const char* pWorld = argv[2];
    const char* pOut   = argv[3];
    int   nSize  = (argc > 4) ? atoi(argv[4]) : 640;
    float fYaw   = (argc > 5) ? (float)atof(argv[5]) : 40.0f;
    float fPitch = (argc > 6) ? (float)atof(argv[6]) : 35.0f;

    // ⚠️ Optional model filter. Framing a whole level puts ~7 world units in
    // every pixel, and with roughly one texel per unit that point-samples a
    // texture at 7x its own frequency -- the result is speckle, and it looks
    // exactly like broken UVs. It is not: it is aliasing, and there is no mip
    // selection in the rasteriser yet. Drawing ONE model fills the frame and
    // shows whether the projection is actually right.
    const char* pOnly = (argc > 7) ? argv[7] : 0;

    // Stand at the player's spawn instead of orbiting the level from outside.
    const bool bInside = (pOnly && strcmp(pOnly, "-inside") == 0);
    const bool bPortal = (pOnly && strcmp(pOnly, "-skyportal") == 0);
    if (bInside || bPortal) pOnly = 0;

    // ⚠️ WIF_ bits, from the donor's header (`de_world.h:24`).
    //
    // ⚠️ AND THE NAMES IN THE FILE ARE MISLEADING -- READ THE FLAGS, NOT THE
    // NAME. The model called "PhysicsBSP" carries flags 20, which is
    // WIF_PHYSICSBSP | **WIF_MAINWORLD**: it is the main world, and rendering
    // it alone produces the level -- arches, wallpaper, carpet, doors. The
    // model called "VisBSP" carries flags 32, WIF_VISBSP alone, and holds a
    // near-duplicate of the same geometry for visibility work.
    //
    // Drawing both z-fights them against each other, so the VISIBILITY one is
    // dropped and the MAINWORLD one kept. An earlier version of this file did
    // the exact opposite, on the reasoning that anything called "PhysicsBSP"
    // must be collision -- the name was wrong and the flag was right.
    const unsigned int kWIF_MAINWORLD = (1u << 2);
    const unsigned int kWIF_VISBSP    = (1u << 5);

    // SURF_ bits, from the donor's de_world.h:59.
    const unsigned int kSURF_INVISIBLE = (1u << 2);
    const unsigned int kSURF_SKY       = (1u << 4);
    const unsigned int kSURF_BRIGHT    = (1u << 5);

    const bool bNoLight = (getenv("LT_WV_NOLIGHT") != 0);

    // LT_WV_ONLY=<modelName> restricts drawing while still using the inside
    // camera -- for answering "which of these two models is the world?".
    const char* pOnlyEnv = getenv("LT_WV_ONLY");

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&pd[0], (int)pd.size()) == 0) { fprintf(stderr, "no game data\n"); return 2; }

    CLTWorld w;
    if (!w.LoadThroughFileMgr(&fm, pWorld)) { fprintf(stderr, "world: %s\n", w.GetError()); return 1; }

    const std::vector<LTWorldModelHeader>& mdl = w.GetModels();
    printf("world  : %s\n", pWorld);
    printf("         %u models, %u bodies parsed, %u skipped\n",
           (unsigned)mdl.size(), w.GetNumBodiesParsed(), w.GetNumBodiesSkipped());

    // ---- which model is the sky ----------------------------------------- //
    //
    // ⚠️ Read from the world, not assumed from the name. A `SkyPointer` object
    // carries a `SkyObjectName` string property naming the object that is the
    // sky; matching a model called "SkyBox" would work in M01S01 and fail
    // anywhere the designer named it something else.
    const LTWorldModelHeader* pSky = 0;
    {
        const std::vector<LTWorldObject>& obj = w.GetObjects();
        std::string sSkyName;
        for (size_t i = 0; i < obj.size() && sSkyName.empty(); ++i)
        {
            if (obj[i].m_sType != "SkyPointer") continue;
            const LTWorldProperty* p = obj[i].Find("SkyObjectName");
            if (p && !p->m_sString.empty()) sSkyName = p->m_sString;
        }
        if (!sSkyName.empty())
            for (size_t i = 0; i < mdl.size(); ++i)
                if (mdl[i].m_sName == sSkyName) { pSky = &mdl[i]; break; }
        printf("sky    : SkyPointer names \"%s\"%s\n",
               sSkyName.empty() ? "(none)" : sSkyName.c_str(),
               (!sSkyName.empty() && !pSky) ? "  -- but no such world model" : "");
    }

    // Frame the geometry we can actually draw.
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    unsigned long long nTris = 0;
    for (size_t i = 0; i < mdl.size(); ++i)
    {
        if (!mdl[i].m_bBodyParsed) continue;
        if (pOnly && mdl[i].m_sName != pOnly) continue;
        if (pOnlyEnv && mdl[i].m_sName != pOnlyEnv) continue;
        if (!pOnly && !pOnlyEnv && !Drawable(mdl[i], kWIF_VISBSP)) continue;
        // ⚠️ The skybox must not frame the shot. It is a cube big enough to
        // hold the level, so including it doubles the radius and pushes the
        // camera far enough back that the level is a speck.
        if (&mdl[i] == pSky) continue;
        for (size_t p = 0; p < mdl[i].m_Points.size(); ++p)
            for (int k = 0; k < 3; ++k)
            {
                float v = mdl[i].m_Points[p].m_Pos[k];
                if (v < mn[k]) mn[k] = v;
                if (v > mx[k]) mx[k] = v;
            }
        for (size_t q = 0; q < mdl[i].m_Polys.size(); ++q)
            if (mdl[i].m_Polys[q].m_Verts.size() >= 3)
                nTris += mdl[i].m_Polys[q].m_Verts.size() - 2;
    }
    if (nTris == 0) { fprintf(stderr, "no drawable geometry\n"); return 1; }

    float c[3], fRadius = 0.0f;
    for (int k = 0; k < 3; ++k) { c[k] = (mn[k] + mx[k]) * 0.5f; float d = (mx[k] - mn[k]) * 0.5f; fRadius += d * d; }
    fRadius = sqrtf(fRadius);
    printf("bounds : (%.0f %.0f %.0f)..(%.0f %.0f %.0f)  radius %.0f  %llu triangles\n",
           mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], fRadius, nTris);

    const float kFov = 0.9f;
    float dist = fRadius / sinf(kFov * 0.5f) * 1.05f;
    float ry = fYaw * 3.14159265f / 180.0f, rp = fPitch * 3.14159265f / 180.0f;
    float eye[3] = { c[0] + sinf(ry) * cosf(rp) * dist,
                     c[1] + sinf(rp) * dist,
                     c[2] + cosf(ry) * cosf(rp) * dist };
    float at[3] = { c[0], c[1], c[2] };
    float up[3] = { 0, 1, 0 };
    float fNear = dist * 0.005f + 1.0f, fFar = dist + fRadius * 4.0f;

    // ---- the player's eye ----------------------------------------------- //
    //
    // ⚠️ Not an invented viewpoint: the world ships one. `GameStartPoint` is
    // where the game spawns Cate, and it carries both a Pos and a Rotation, so
    // the camera can stand exactly where the player does and face the way the
    // level designer pointed them.
    //
    // ⚠️ Yaw only. The start point's pitch and roll are the PLAYER's, and a
    // player spawns upright -- applying all three Eulers would tilt the
    // horizon for no reason.
    if (bInside)
    {
        const std::vector<LTWorldObject>& obj = w.GetObjects();
        const LTWorldObject* pStart = 0;
        for (size_t i = 0; i < obj.size() && !pStart; ++i)
            if (obj[i].m_sType == "GameStartPoint") pStart = &obj[i];

        float pos[3], eul[3];
        if (!pStart || !pStart->GetPos(pos))
        { fprintf(stderr, "no GameStartPoint with a Pos in this world\n"); return 1; }
        if (!pStart->GetEuler(eul)) { eul[0] = eul[1] = eul[2] = 0.0f; }

        eye[0] = pos[0]; eye[1] = pos[1] + 48.0f; eye[2] = pos[2];

        float yaw = eul[1] + fYaw * 3.14159265f / 180.0f;
        float pitch = fPitch * 3.14159265f / 180.0f;
        at[0] = eye[0] + sinf(yaw) * cosf(pitch);
        at[1] = eye[1] + sinf(pitch);
        at[2] = eye[2] + cosf(yaw) * cosf(pitch);

        // ⚠️ A near plane scaled to the whole level would clip the wall you
        // are standing next to. Standing inside, it has to be small.
        fNear = 4.0f;
        fFar  = fRadius * 4.0f;
        printf("camera : INSIDE at (%.0f %.0f %.0f) looking %.0f deg\n",
               eye[0], eye[1], eye[2], yaw * 180.0f / 3.14159265f);
    }

    // ---- stand in front of an actual sky portal ------------------------- //
    //
    // ⚠️ Because no spawn point in the levels tested has one in view. The four
    // GameStartPoints checked are all enclosed, and "0 pixels of sky survive"
    // is the correct answer there, not a broken sky pass. To SEE a portal work
    // the camera has to be put at one, so this finds the largest sky-flagged
    // polygon in the world and stands off it along its own normal.
    if (bPortal)
    {
        const LTWorldPoly* pBest = 0; const LTWorldModelHeader* pBestM = 0;
        float fBestArea = 0.0f;
        for (size_t i = 0; i < mdl.size(); ++i)
        {
            if (!mdl[i].m_bBodyParsed || &mdl[i] == pSky) continue;
            if (!Drawable(mdl[i], kWIF_VISBSP)) continue;
            for (size_t q = 0; q < mdl[i].m_Polys.size(); ++q)
            {
                const LTWorldPoly& po = mdl[i].m_Polys[q];
                if (po.m_nSurface >= mdl[i].m_Surfaces.size()) continue;
                if (!(mdl[i].m_Surfaces[po.m_nSurface].m_nFlags & kSURF_SKY)) continue;
                if (po.m_Verts.size() < 3) continue;

                float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                bool bOK = true;
                for (size_t v = 0; v < po.m_Verts.size() && bOK; ++v)
                {
                    if (po.m_Verts[v].m_nPoint >= mdl[i].m_Points.size()) { bOK = false; break; }
                    const float* p3 = mdl[i].m_Points[po.m_Verts[v].m_nPoint].m_Pos;
                    for (int k = 0; k < 3; ++k)
                    { if (p3[k] < lo[k]) lo[k] = p3[k]; if (p3[k] > hi[k]) hi[k] = p3[k]; }
                }
                if (!bOK) continue;
                float d0 = hi[0]-lo[0], d1 = hi[1]-lo[1], d2 = hi[2]-lo[2];
                float fArea = d0*d1 + d1*d2 + d0*d2;
                if (fArea > fBestArea) { fBestArea = fArea; pBest = &po; pBestM = &mdl[i]; }
            }
        }
        if (!pBest) { fprintf(stderr, "no sky portal in this world\n"); return 1; }

        float ctr[3] = {0,0,0}, nrm[3] = {0,0,0};
        for (size_t v = 0; v < pBest->m_Verts.size(); ++v)
        {
            const LTWorldPoint& pt = pBestM->m_Points[pBest->m_Verts[v].m_nPoint];
            for (int k = 0; k < 3; ++k) { ctr[k] += pt.m_Pos[k]; nrm[k] += pt.m_Normal[k]; }
        }
        float inv = 1.0f / (float)pBest->m_Verts.size();
        for (int k = 0; k < 3; ++k) { ctr[k] *= inv; nrm[k] *= inv; }
        float nl = sqrtf(nrm[0]*nrm[0]+nrm[1]*nrm[1]+nrm[2]*nrm[2]);
        if (nl > 1e-6f) for (int k = 0; k < 3; ++k) nrm[k] /= nl;

        float back = sqrtf(fBestArea) * 0.9f + 96.0f;
        for (int k = 0; k < 3; ++k) { at[k] = ctr[k]; eye[k] = ctr[k] + nrm[k] * back; }
        fNear = 4.0f; fFar = fRadius * 4.0f;
        printf("camera : AT A SKY PORTAL, %.0f units off (%.0f %.0f %.0f)\n",
               back, ctr[0], ctr[1], ctr[2]);
    }

    float P[16], V[16], MVP[16];
    LT3D::Perspective(kFov, 1.0f, fNear, fFar, P);
    LT3D::LookAt(eye, at, up, V);
    LT3D::Multiply(P, V, MVP);

    std::vector<unsigned char> rgba((size_t)nSize * nSize * 4, 0);
    std::vector<float> depth((size_t)nSize * nSize, 0.0f);
    LT3DTarget t; t.m_pRGBA = &rgba[0]; t.m_pDepth = &depth[0];
    t.m_Width = nSize; t.m_Height = nSize;
    LT3D::ClearColor(t, 0x0E1116);
    LT3D::ClearDepth(t);

    DrawStats st; st.nPix = st.nDrawn = st.nTextured = st.nUntextured = st.nSkySurf = 0;

    // ---- pass 1: the sky ------------------------------------------------ //
    //
    // ⚠️ The world says which object is the sky, so nothing here is guessed.
    // A `SkyPointer` object carries `SkyObjectName`, and in M01S01 it names
    // "SkyBox" -- a 36-polygon cube whose six faces are BlueSky_UP, _FR, _BK,
    // _LF, _RT and Snow_DN. The engine keeps a server-side list of such
    // objects (`ServerMgr::m_SkyObjects`) and sends it to the client.
    //
    // ⚠️ THE SKY DOES NOT TRANSLATE WITH THE CAMERA. It is drawn from the
    // skybox's own centre using the camera's ORIENTATION only, which is what
    // puts it at infinity: walk down the corridor and the sky must not slide
    // past the doorway. Then the depth buffer is CLEARED, so world geometry
    // always wins over it and the sky survives only where nothing was drawn --
    // which is exactly the holes the sky-portal surfaces leave behind.
    if (pSky && pSky->m_bBodyParsed)
    {
        float smn[3] = { 1e30f, 1e30f, 1e30f }, smx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t p = 0; p < pSky->m_Points.size(); ++p)
            for (int k = 0; k < 3; ++k)
            {
                float v = pSky->m_Points[p].m_Pos[k];
                if (v < smn[k]) smn[k] = v;
                if (v > smx[k]) smx[k] = v;
            }
        float sc[3], sr = 0.0f;
        for (int k = 0; k < 3; ++k)
        { sc[k] = (smn[k] + smx[k]) * 0.5f; float d = (smx[k] - smn[k]) * 0.5f; sr += d * d; }
        sr = sqrtf(sr);

        float dir[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
        float sat[3] = { sc[0] + dir[0], sc[1] + dir[1], sc[2] + dir[2] };

        float SP[16], SV[16], SMVP[16];
        LT3D::Perspective(kFov, 1.0f, sr * 0.001f + 0.1f, sr * 4.0f, SP);
        LT3D::LookAt(sc, sat, up, SV);
        LT3D::Multiply(SP, SV, SMVP);

        std::vector<const LT3DTexture*> stex(pSky->m_Textures.size(), (const LT3DTexture*)0);
        for (size_t k = 0; k < pSky->m_Textures.size(); ++k)
            stex[k] = GetTexture(fm, pSky->m_Textures[k]);

        DrawModel(t, SMVP, *pSky, stex, bNoLight, kSURF_SKY, kSURF_INVISIBLE, kSURF_BRIGHT, st);
        printf("sky    : \"%s\" drawn from its own centre, %d pixels\n",
               pSky->m_sName.c_str(), st.nPix);
        LT3D::ClearDepth(t);
    }

    // ---- pass 2: the world ---------------------------------------------- //
    for (size_t i = 0; i < mdl.size(); ++i)
    {
        if (!mdl[i].m_bBodyParsed) continue;
        if (pOnly && mdl[i].m_sName != pOnly) continue;
        if (pOnlyEnv && mdl[i].m_sName != pOnlyEnv) continue;
        if (!pOnly && !pOnlyEnv && !Drawable(mdl[i], kWIF_VISBSP)) continue;
        if (&mdl[i] == pSky) continue;          // already drawn, at infinity
        const LTWorldModelHeader& M = mdl[i];

        std::vector<const LT3DTexture*> tex(M.m_Textures.size(), (const LT3DTexture*)0);
        for (size_t k = 0; k < M.m_Textures.size(); ++k)
            tex[k] = GetTexture(fm, M.m_Textures[k]);

        DrawModel(t, MVP, M, tex, bNoLight, kSURF_SKY, kSURF_INVISIBLE, kSURF_BRIGHT, st);
    }
    int nPix = st.nPix, nDrawn = st.nDrawn;
    int nTextured = st.nTextured, nUntextured = st.nUntextured, nSky = st.nSkySurf;
    // ---- pass 3: the objects -------------------------------------------- //
    {
        int nObj = 0, nObjTris = 0, nObjPix = 0, nNoModel = 0;
        const std::vector<LTWorldObject>& obj = w.GetObjects();
        for (size_t i = 0; i < obj.size(); ++i)
        {
            // ⚠️ `Filename` is not always a model. Objects use the same
            // property name for sounds and textures, so the extension decides
            // -- otherwise every non-model Filename is counted as a failure
            // and the statistic lies about how well this works.
            const LTWorldProperty* pF = obj[i].Find("Filename");
            if (!pF || pF->m_sString.size() < 4) continue;
            std::string sLow = pF->m_sString;
            for (size_t c = 0; c < sLow.size(); ++c) sLow[c] = (char)tolower(sLow[c]);
            if (sLow.compare(sLow.size() - 4, 4, ".abc") != 0) continue;

            float pos[3], eul[3] = { 0, 0, 0 };
            if (!obj[i].GetPos(pos)) continue;
            obj[i].GetEuler(eul);

            const LTWorldProperty* pS = obj[i].Find("Skin");
            CachedModel* pM = GetModel(fm, pF->m_sString,
                                       pS ? pS->m_sString : std::string());
            if (!pM) { ++nNoModel; continue; }

            float World[16], ObjMVP[16];
            WorldMatrix(pos, eul, World);
            LT3D::Multiply(MVP, World, ObjMVP);

            // ⚠️ The NORMAL matrix is the object's world rotation, not the
            // full chain: lighting must not follow the camera. Translation in
            // column 3 is ignored by the shading, so World serves for both.
            LTModelDrawStats ms = LTModelDraw::Draw(
                t, pM->m_Model, ObjMVP, World,
                pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins, 0, false, 0);

            ++nObj; nObjTris += ms.m_nTriangles; nObjPix += ms.m_nPixels;
        }
        printf("objects: %d placed, %d triangles, %d pixels%s\n",
               nObj, nObjTris, nObjPix,
               nNoModel ? "  (some models failed to load)" : "");
    }

    // ⚠️ THE MEASUREMENT THAT ACTUALLY TESTS THE SKY PASS. Depth is cleared
    // between the two passes, so every pixel the world touched has a nonzero
    // depth and every pixel it did not still reads zero. Those untouched
    // pixels are precisely where the sky survives -- the holes the sky-portal
    // surfaces left behind. Counting them turns "the sky shows through the
    // portals" from something to squint at into a number.
    {
        int nSkyPix = 0;
        for (size_t i = 0; i < depth.size(); ++i) if (depth[i] == 0.0f) ++nSkyPix;
        printf("sky    : %d pixels survive the world pass (%.1f%% of frame)\n",
               nSkyPix, 100.0 * nSkyPix / (double)depth.size());
    }

    printf("texture: %d polys textured, %d not, %d sky/invisible skipped; %u DTX\n",
           nTextured, nUntextured, nSky, (unsigned)g_Textures.size());
    printf("drawn  : %d triangles, %d pixels\n", nDrawn, nPix);
    if (nPix == 0) { fprintf(stderr, "NOTHING DRAWN\n"); return 1; }
    if (!WritePPM(pOut, &rgba[0], nSize, nSize)) return 1;
    printf("wrote  : %s\n", pOut);
    return 0;
}
