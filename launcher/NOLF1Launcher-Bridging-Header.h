// NOLF1Launcher-Bridging-Header.h -- the game, as the launcher calls it.
//
// The engine and the game's client code are linked INTO this app (the static
// library `nolf1core`, -force_load), so the game runs in the launcher's own
// process: one program, one Dock icon. GameRunner.swift drives it.

#ifndef NOLF1_LAUNCHER_BRIDGING_HEADER_H
#define NOLF1_LAUNCHER_BRIDGING_HEADER_H

// The whole game, as the `nolf1` executable's main() runs it: reads its
// settings from the environment (LT_*, NOLF_*), runs until its window closes
// (or, headless, for LT_UPDATES frames), tears down, returns 0.
// ⚠️ ONCE PER PROCESS. The engine's globals and the game module's state are
// not reset by a return -- a second call would start on the first one's
// leftovers. The launcher restarts itself after a game instead.
int NOLF1_Main(int argc, char** argv);

// Asks a running game to end as if its window had been closed (it saves the
// player's settings and tears down on its own). Safe to call at any time.
void NOLF1_RequestQuit(void);

#endif
