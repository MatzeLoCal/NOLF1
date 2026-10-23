// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldscene.cpp
//
// See ltworldscene.h for why the world is owned here rather than by whoever
// happened to load it first.
//
// ----------------------------------------------------------------------- //

#include "ltworldscene.h"
#include "ltworldrender.h"
#include "ltrecord.h"

#include <stdio.h>
#include <string>

namespace
{
    // ⚠️ FUNCTION-LOCAL STATICS, so they are CONSTRUCTED ON FIRST USE. Other
    // translation units bind a reference to this scene during their own static
    // initialisation (ltworldwalk.cpp does), and a namespace-scope object here
    // would be racing them -- static initialisation order across translation
    // units is not defined, and the failure would be a scene that works or
    // does not depending on link order.
    CLTWorldScene& Scene() { static CLTWorldScene s; return s; }
    std::string&   Name()  { static std::string s;   return s; }
}


CLTWorldScene* LTGetWorldScene() { return &Scene(); }

bool LTWorldScene_IsLoaded()  { return Scene().IsLoaded(); }
const char* LTWorldScene_Name() { return Name().c_str(); }


bool LTWorldScene_Load(CLTFileMgr* pFileMgr, const char* pWorldName)
{
    if (!pFileMgr || !pWorldName || !*pWorldName) return false;

    // ⚠️ Asking for the world that is already loaded is a no-op, not a
    // reload. The game's client and the server both name the level, and
    // reloading a 2.3-million-triangle world because two callers agreed about
    // it would be a very expensive way to change nothing.
    if (Scene().IsLoaded() && Name() == pWorldName) return true;

    if (!Scene().Load(pFileMgr, pWorldName))
    {
        // ⚠️ Loud, never silent. A world that will not load must not leave the
        // engine quietly drawing an empty scene as though nothing had been
        // asked for.
        printf("[WORLD] %s FAILED: %s\n", pWorldName, Scene().GetError());
        fflush(stdout);
        LTRecord_Event("world", "load failed");
        Name().clear();
        return false;
    }

    Name() = pWorldName;

    int nTex = 0, nMdl = 0;
    Scene().PreloadAll(&nTex, &nMdl);
    printf("[WORLD] %s loaded -- preloaded %d texture references and %d prop models\n",
           pWorldName, nTex, nMdl);
    fflush(stdout);

    LTRecord_Event("world", "loaded");
    return true;
}
