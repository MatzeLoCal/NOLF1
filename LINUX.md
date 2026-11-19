# NOLF1 — Linux port

A Linux build of the macOS port, sharing the same engine source. Developed and
tested on Ubuntu (aarch64, Snapdragon X / Adreno X1-45, Mesa 26) with clang 21
and SDL3. Platform-specific code is under `#ifdef __APPLE__` / `engine/linux/`,
so the macOS build is unchanged.

## What is different from the macOS build

| macOS | Linux (`engine/linux/`) |
|---|---|
| AppKit window, input | `ltwindow_sdl.cpp` — SDL3 window and input (scancode → Mac keycode → VK, so bindings are shared) |
| Metal renderer (`ltmetal.mm`) | `ltgl.cpp` — the `ltmetal.h` API on OpenGL 4.6 core (`glClipControl` reproduces Metal's clip/row conventions); compositor, MSAA, skinning, reflections |
| CoreAudio | `ltaudio_sdl.cpp` — SDL3 audio stream driving the same `CLTMixer`; MP3 via `third_party/dr_mp3.h` |
| Apple's `gs_instruments.dls` GM fallback | `ltsf2.cpp` — loads a SoundFont 2 bank as the GM set (see below) |
| `~/Library/Application Support/NOLF1` | `~/.local/share/NOLF1` |
| `libnolfobject.dylib` | `libnolfobject.so` (linked with `-Wl,-Bsymbolic`, or game/shared symbols interpose onto the executable's copies and crash) |

The fixes below are shared with macOS:

- **Menu sounds.** `PauseSounds` now holds the voices that are playing, not the whole mixer, as LithTech did. Before this, the menu's own clicks were silent.
- **Software rasteriser.** Large triangles are split across worker threads by rows. The output is pixel-identical. Set the number of threads with `LT_SWTHREADS`; the default is cores − 2.

## Building

Dependencies (Debian/Ubuntu): `clang`, `pkg-config`, `python3`, `libsdl3-dev`, `libfreetype-dev`, `fonts-dejavu-core` (the smooth menu and subtitle font), Mesa GL drivers. `./forditas.sh ellenoriz` checks them.

```sh
python3 engine/linux_case_links.py          # once: case-exact include symlinks
NOLF_OBJ=$PWD/build-linux/obj NOLF_BIN=$PWD/build-linux/nolf1 ./engine/build_linux.sh
```

The script produces `build-linux/nolf1` and `build-linux/libnolfobject.so`.

## Running

You need the game data from an installed copy of NOLF GOTY. Point `NOLF_GAME_DIR` at it, then run:

```sh
NOLF_GAME_DIR="$HOME/No One Lives Forever" ./play_linux.sh [WxH]
```

`play_linux.sh` uses OpenGL by default. Set `LT_SOFTWARE=1` for the software renderer. Alt+Enter toggles fullscreen.

**Music.** NOLF's DirectMusic songs rely on a General MIDI bank for some instruments. Put any GM SoundFont at one of these paths:

- `~/.local/share/NOLF1/GeneralUser-GS.sf2`, or `gm.sf2`, or `gm.dls`
- `/usr/share/sounds/sf2/FluidR3_GM.sf2`, which is the `fluid-soundfont-gm` package

You can also name the bank directly with `LT_GMBANK=<file>`.

### Useful switches

| Variable | Effect |
|---|---|
| `LT_NOVSYNC=1` | No vsync |
| `LT_MSAA=n` | 3D anti-aliasing (default 4) |
| `LT_ANISO=n` | Anisotropic filtering |
| `LT_SWTHREADS=n` | Software rasteriser threads (1 = single-threaded) |
| `LT_PACETRACE=1` | FPS / frame pacing log |
| `LT_GLSHOT=<dir>` with `LT_GLSHOT_EVERY=n` | Dump presented GL frames as PPM |
| `LT_MIXWAV=<file>` | Dump the mixer output (raw s16 stereo 44100) |
| `LT_AUTOCLOSE=<s>` | Close the window after s seconds (tests) |
| `LT_TEST_KEYS=vk,...` with `LT_TEST_KEYS_INTERVAL=frames` | Scripted key presses (tests) |
