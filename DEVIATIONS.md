# Deviations from the original NOLF 1

This port compiles **the original game code** (the client shell and the object
DLL from the NOLF 1 v1.003 mod SDK) and runs it on a **new engine** behind
LithTech 2's own interface. The goal is to stay as close to the original as
possible. This file lists every place where it does not, in two parts:

1. **Behaviour differences** (E-001 to E-017): where the game looks or plays
   differently from the Windows version. Most of them are in the engine. Only
   three change the game's own source code (E-002, E-013, E-014). Where it
   makes sense, an environment variable switches back to the original behaviour.
2. **Source fixes** (D-001 to D-037): changes to the original sources that were
   needed to build 2000-era Win32/MSVC code with clang for 64-bit macOS. None
   of them is meant to change what the game does.

Changes in the game's own sources are marked in the code:
`// PORT:` for the build fixes, `// PORT-BEHAVIOUR:` for the three behaviour
changes, so `grep -rn 'PORT-BEHAVIOUR' NOLF/` finds exactly those.

---

## Part 1 — Behaviour differences

### E-017 — tinted text has soft edges
Windows tinted every font glyph by filling each pixel that was not exactly the
transparent colour, so the grey anti-aliased edges of the menu and HUD fonts
became solid: letters a pixel fatter, small letters closing up ("e" looks like
"a"). The port uses the glyph's grey levels as coverage, so letters keep the
shapes in the font files. Engine only.
Original: `LT_SOLIDFONTS=1`.

### E-016 — mirrors always reflect
On Windows, reflections follow the Mirrors option (off in the Low and Medium
presets and always off at 16-bit colour). The port renders in 32-bit and keeps
mirrors on. The menu option still shows and saves its value, but it no longer
changes anything. Engine only.
Original: `LT_MIRRORCONSOLE=1`.

### E-015 — flying outside the level with `mpclip` does not wake every AI
An AI the player cannot see sleeps. A view outside every part of the level
counts as seeing everything, so flying over the roofs with `mpclip` (E-014)
would wake every AI in the level. The player's own view outside the level now
keeps the last area it was in. Cinematic cameras keep the original rule.
Engine only.
Original: `LT_MPCLIPWAKES=1`.

### E-014 — the `mpclip` cheat works *(game source change)*
The shipped game is built with `_FINAL`, which compiles the no-clip cheat out:
typing `mpclip` did nothing. In the port it toggles the game's own spectator
mode (fly through walls), and in single player the message line can be opened
in spectator mode so the cheat can be turned off again. Every other `_FINAL`
block is unchanged.
Files: `NOLF/ClientShellDLL/MessageMgr.cpp`, `NOLF/ClientShellDLL/InterfaceMgr.cpp`.

### E-013 — Options > Display offers widescreen sizes *(game source change)*
The game skipped every display mode that was not 4:3. The port offers 4:3,
16:10 and 16:9 sizes that fit the screen. The game's own minimum of 640x480
stays.
File: `NOLF/ClientShellDLL/FolderDisplay.cpp`.

### E-012 — on a widescreen display the menu's 3D keeps its 640x480 proportions
The menu's 3D objects are laid out for 4:3. At other shapes Windows draws them
squashed or stretched against the menu's own 2D. The port scales them so they
match the 2D. At 4:3 nothing changes. Engine only.
Original: `LT_MENUSQUASH=1`.

### E-011 — crossbow bolts and speargun spears stick in wooden doors
The game sticks a bolt only in the world, in characters and in bodies; on a
door it always breaks. The port lets the game's own "stick" rule run on doors
too (wooden doors only), and the bolt moves with the door. Metal doors, glass
and the game's 10% random break are unchanged. A gameplay change, chosen on
purpose.
Original: `LT_NOBOLTSINDOORS=1`.

### E-010 — falling sparks and blood bounce off the floor
The engine supports bouncing particles, but NOLF 1 never asks for it, so
falling sparks and blood drops fell straight through the floor. In the port,
particles with gravity bounce off the floor below them. Smoke, steam, fire and
bubbles are unaffected.
Original: `LT_NOFLOORBOUNCE=1`, or launcher › Advanced › "Sparks bounce off floors".

### E-009 — aquarium bubbles appear at any frame rate
The placed bubble emitters add `floor(rate × frame time)` particles per frame
and drop the remainder. At 60 fps that is 0 every frame, so on a modern machine
the bubbles never appear. The port carries the dropped fraction over, so each
emitter produces its authored rate. Engine only.
Original: `LT_NOPSRATE=1`, or launcher › Advanced › "Bubbles in aquariums".

### E-008 — bullets pass through the power lines (M01S01, M01S03)
Five power-line models count as shoot-through wire mesh that deflects
bullets. From the M01S01 window this made the assassin on the far balcony
nearly impossible to hit while standing. In the port, objects named
`powerline*` are invisible to bullets and line-of-sight rays; collision is
unchanged. A gameplay change, chosen on purpose.
Original: `LT_WIRESCATCHSHOTS=1`.

### E-007 — the M01S02 hostage guards start shooting 5 seconds later
Crossing one of the hostage triggers made the guard shoot the hostage almost
at once (first shot about 0.85 s after). The port delays the guard's
`target`/`attack` messages by 5 seconds. The guard still turns and the
hostage still screams immediately. A gameplay change, chosen on purpose; only
M01S02's 14 hostage triggers are affected.
Original: `LT_HOSTAGEDELAY=0` (or another delay in seconds).

### E-006 — *(resolved)* animation blend-ins
Earlier versions held a new animation's first frame during the blend instead
of cross-fading. Characters now cross-fade into a new animation as the
original does. Kept here for the record.

### E-005 — doors with a zero move direction keep their position
In the original, a door with a zero `MoveDir` gets a NaN position on its first
move, and the x87 build treats that as "arrived". Such an object (for example
a disarmed M01S02 charge) then most likely vanished. The port gives such doors
a distance of 0: same outcome for the game's logic, but the object stays where
it is and stays visible. Engine only.
Original: `LT_NANDOORS=1` (such doors then never open on arm64).

### E-004 — client and server share one `rand()`
On Windows the two game DLLs each had their own C runtime random generator.
In the port they share the process's one, which is a different generator with
a different range. Nothing visible has been traced to this. Not fixed yet.

### E-003 — the static loading screen is used
The animated loading screen runs on a thread that the port does not provide.
The port switches to the game's own static loading screen
(`DynamicLoadScreen 0`, a shipped option). As a result, the level's name and
photo do not appear while loading. No game source changed.

### E-002 — a missing music theme no longer crashes *(game source change)*
Twelve shipped levels name a music theme (`urbanspy`) that does not exist, and
the music manager then reads far past an empty table. That is a bug in the
shipped game, and on this port it crashed. One added condition skips moods
that have no entries. Levels whose moods all have entries behave exactly as
before.
File: `NOLF/ObjectDLL/MusicMgr.cpp`.

### E-001 — standing on a breakable object does not break it
The game asks the engine what a character is standing on and breaks it if it
is a breakable object. The port's server answers "nothing": it has no
object-on-object contact. So a crate under a character's feet is never broken
by standing on it. Engine only.

---

## Part 2 — Source fixes for 64-bit macOS and a modern compiler

None of these is meant to change behaviour. Most are no-ops on the original
Win32 compiler. Files are relative to the repository root.

| ID | Where | What and why |
|---|---|---|
| D-001 | `LT2/sdk/inc/ltbasetypes.h` | `int32`/`uint32` were `long`, which is 64-bit on macOS; now `int` under LP64 (Win32 spelling kept) |
| D-002 | `LT2/sdk/inc/ltbasetypes.h` | handle typedefs (`HPOLY`, `HMODELANIM`, …) were `unsigned long`; now `uint32`, which also repairs the `INVALID_*` comparisons |
| D-003 | `LT2/sdk/inc/ltbasedefs.h` | stray leading `##` in `DEFINE_HANDLE_TYPE` |
| D-004 | `NOLF/Shared/Factory.h` | stray `##` around a macro parameter |
| D-005 | `LT2/sdk/inc/iltclient.h`, `iltserver.h` | default arguments on function-pointer members (no caller relied on them) |
| D-006 | `LT2/lithshared/incs/ztools.h` | redefined `size_t` |
| D-007 | `LT2/lithshared/incs/ztools.h` | pointer→`int` casts made explicit through `intptr_t` |
| D-008 | `LT2/sdk/inc/ltbasedefs.h` | `this->` for members of a dependent base class (two-phase lookup) |
| D-009 | `LT2/lithshared/incs/ztools.h` | identifiers that later became keywords (`or`, `export`, `nullptr`) and implicit `int` |
| D-010 | `LT2/lithshared/incs/ztools.h` | its own `istream`/`ostream` declarations shadowed the standard ones |
| D-011 | `LT2/lithshared/incs/ButeMgr.h` | `ios::nocreate` no longer exists (it was redundant for an `ifstream`) |
| D-012 | `NOLF/ObjectDLL/AnimationStd.h` | operators declared without a return type |
| D-013 | `NOLF/Shared/BankedList_impl.h` | a duplicate declaration and a missing `return` in a never-instantiated template |
| D-014 | `NOLF/ObjectDLL/AnimationMgr.h` | forward declaration of `CAnimationMgr` |
| D-015 | 6 files in `NOLF/ObjectDLL/` | 69 dangling `##` pastes |
| D-016 | `NOLF/Shared/Factory.h` | `template<>` on explicit specialisations (corrected by D-036) |
| D-017 | `LT2/lithshared/stdlith/…`, `lith/lithtypes.h`, `incs/rezmgr.h`, `NOLF/ClientShellDLL/WinUtil.h` | `DWORD`/`SDWORD` were 64-bit (a `#define` won over the typedef); now 32-bit |
| D-018 | `LT2/lithshared/stdlith/abstractio.h` | two overloads became identical after D-017 |
| D-019 | `LT2/sdk/inc/iltcsbase.h` + 5 writers in `NOLF/ObjectDLL/` | pointers passed through a 32-bit message field (trigger messages); widened to `uintptr_t` |
| D-020, D-029, D-030 | `GameServerShell.cpp`, 10+4 files in `NOLF/ClientShellDLL/` | MSVC's for-loop variable scope (variables hoisted; one is a shellsort that needs the leaked index) |
| D-021 | `NOLF/ObjectDLL/AICmd.cpp` | `template<>` on function specialisations |
| D-022 | `NOLF/ObjectDLL/AIGoalMgr.cpp` | `AIGoal.h` ships as a zero-byte file; include the header that has the class |
| D-023 | `LT2/lithshared/butemgr/avector.h` | a `const` class return type blocked assignment |
| D-024 | `NOLF/ObjectDLL/ClientWeaponSFX.cpp` | temporaries bound to non-const references (a named copy keeps the semantics) |
| D-025, D-026 | `Sparam.cpp`, `CinematicTrigger.cpp` | `strstr` const-correctness; a pointer truncated into a `BOOL` |
| D-027 | `lithfontdefs.h`, `rezmgr.h`, `JoystickAxis.cpp` | redundant class qualification on member declarations |
| D-028 | `NOLF/ClientShellDLL/LTWnd.h` | default arguments on function-pointer types |
| D-031 | `NOLF/ClientShellDLL/GameClientShell.cpp` | the window-procedure hook truncated a function pointer to 32 bits (`LONG_PTR`) |
| D-032 to D-034 | `SoundFX.cpp`, `LTMenuWnd.cpp`, the second `Sparam.cpp` | a handle truncated to a `BOOL`, a macro-vs-typedef cast, `strstr` again |
| D-035 | `LT2/lithshared/ltguimgr/ltguictrl.h`, `FolderJoin.cpp` | pointers stored in a UI control's 32-bit user-data slot; widened |
| D-036 | `NOLF/Shared/Factory.h` | D-016 made the factory statics declarations only; initialisers restore the definitions |
| D-037 | `LT2/lithshared/lith/baselistcounter.h` | a base-class constructor called like a function |

Not changed on purpose: original quirks that are harmless, such as
`INVALID_MODEL_SOCKET` being defined with the node type, are left as they are.
