// ----------------------------------------------------------------------- //
//
// MODULE  : nolf1_main.cpp
//
// PURPOSE : THE COMMAND-LINE GAME -- `nolf1`, the executable the harness, the
//           gates and NOLF1/build run. Everything it does is NOLF1_Main
//           (engine/skeleton/skeleton_host.cpp), which lives in the static
//           library `nolf1core` together with the whole engine and the game's
//           client code.
//
// ⚠️ ONE LIBRARY, TWO HOSTS. The launcher (NOLF1.app) links the same library
// and calls NOLF1_Main inside its own process, so the app is ONE program with
// one Dock icon -- the game used to be a second executable in
// Contents/Helpers. Both link it with -force_load: every object file, as the
// executable had them, including ones only reached by a static initialiser.
//
// ----------------------------------------------------------------------- //

extern "C" int NOLF1_Main(int argc, char** argv);

int main(int argc, char** argv)
{
    return NOLF1_Main(argc, argv);
}
