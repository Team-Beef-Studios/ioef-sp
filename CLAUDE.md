# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ioEF is a fork of ioquake3 (open-source Quake 3 engine) extended with **Star Trek: Voyager - Elite Force** singleplayer and multiplayer support. It is a C game engine licensed under GPLv2.

When built with `BUILD_ELITEFORCE=1`, the engine defines `ELITEFORCE` and uses `baseEF/` as the game data directory, version 1.38, and the `STEF1` master server heartbeat. Without it, it builds as standard ioquake3 (`baseq3/`, version 1.36).

The SP game module (`efgamex86.dll`, built from the separate Elite-Reinforce repository) uses a **Q2-style `GetGameAPI`** interface, not Q3 VM syscalls. A single `efgamex86.dll` contains **both** server game logic (via `GetGameAPI`) and client cgame rendering (via `dllEntry`/`vmMain`). The UI module (`efuix86.dll`) uses `GetUIAPI`.

## Build Commands

The build system is GNU Make. On Windows, use the **MSYS2 MINGW32** shell (not MINGW64) to produce 32-bit binaries compatible with the SP game DLLs.

```bash
# Release build (default)
make -j$(nproc)

# Debug build (with -ggdb -O0)
make debug -j$(nproc)

# Client only, no dedicated server
make -j$(nproc) BUILD_SERVER=0

# Clean all build artifacts
make clean
```

Build output goes to `build/release-<platform>-<arch>/` or `build/debug-<platform>-<arch>/`. On Windows with MINGW32, that's `build/release-mingw32-x86/`.

Override variables via command line or by creating a `Makefile.local` file (gitignored, not committed). Key variables: `BUILD_CLIENT`, `BUILD_SERVER`, `BUILD_GAME_SO`, `BUILD_GAME_QVM`, `BUILD_ELITEFORCE`, `BUILD_RENDERER_OPENGL2`, `USE_OPENAL`, `USE_CURL`, `ARCH`, `PLATFORM`.

Cross-compile for Windows from Linux:
```bash
make PLATFORM=mingw32 ARCH=x86 CC=i686-w64-mingw32-gcc WINDRES=i686-w64-mingw32-windres -j$(nproc)
```

**GCC 15 note:** The LCC tool (QVM compiler in `code/tools/`) has a `constexpr` keyword conflict with GCC 15. Disable QVM building (`BUILD_GAME_QVM=0`) to work around this.

SP game DLLs are built separately with Visual Studio from the Elite-Reinforce source (`E:\Github\Elite-Reinforce\EF_SPMod.sln`), not from this Makefile.

There is no automated test suite. CI is Travis CI with GCC/Clang builds and Coverity static analysis.

## Architecture

### Engine/Game Module Boundary

The engine communicates with game logic through a **virtual machine (VM) interface**. Game code runs either as QVM bytecode (sandboxed) or native DLLs (faster). Three independent VM modules exist:

| Module | Directory | Role |
|--------|-----------|------|
| **qagame** | `code/game/` | Server-side game rules, entities, weapons, scoring |
| **cgame** | `code/cgame/` | Client-side prediction, HUD, effects, entity rendering |
| **ui** | `code/ui/`, `code/q3_ui/` | Menu system, server browser |

Each module communicates with the engine exclusively through numbered **syscalls** (`*_syscalls.c` files). The engine calls into modules via `vmMain()` and modules call engine functions via `syscall()`.

### Elite Force SP Bridge

The `sp_game` cvar activates an alternate loading path for Elite Force singleplayer. This is the main ioEF-specific code.

**Key bridge files:**

| File | Role |
|------|------|
| `code/server/sv_game_sp.c` | Server-side bridge: loads `efgamex86.dll` via `GetGameAPI`, maintains shadow entity array, translates between SP `gentity_t` and engine `sharedEntity_t` |
| `code/client/cl_cgame_sp.c` | Cgame syscall dispatcher: 71 SP syscalls (different numbering from MP's ~50), snapshot builder |
| `code/client/cl_ui_sp.c` | UI bridge: loads `efuix86.dll` via `GetUIAPI` |
| `code/qcommon/sp_types.h` | Shared SP struct definitions (all SP-specific types defined here) |
| `code/qcommon/vm.c` | Fake VM support (`isFake` flag for bridge VMs that aren't real QVMs) |

**How SP loading works:**

1. **Server side** (`sv_game_sp.c`): Loads `efgamex86.dll`, calls `GetGameAPI` to exchange function pointer tables. A **shadow entity array** (`sv_sp_entities`) translates between the SP game's `gentity_t` layout and the engine's `sharedEntity_t`. `SV_SP_SyncToShared` / `SV_SP_SyncFromShared` do **field-by-field translation** (NOT memcpy, because struct layouts differ). `SV_SP_SyncPlayerState` translates SP playerState to ioEF playerState for the engine snapshot builder.

2. **Client cgame** (`cl_cgame_sp.c`): Gets the DLL handle from server side and calls `dllEntry`/`vmMain` for the cgame portion of the same DLL. `SPCG_GETSNAPSHOT` **bypasses engine delta compression** and builds SP-format snapshots directly from game module live data.

3. **Client UI** (`cl_ui_sp.c`): Loads `efuix86.dll` via `GetUIAPI` for the SP menu system.

**SP mode auto-sets `sv_pure` to 0.**

### Critical Struct Layout Differences

The SP game module uses different struct layouts from ioEF/ioquake3. All SP types are defined in `code/qcommon/sp_types.h`.

**`entityState_t`:** SP version is **28 bytes larger** than ioEF's. Extra fields: `modelindex3`, `legsAnimTimer`, `torsoAnimTimer`, `scale`, `pushVec`.

**`playerState_t`:** Significant differences from ioEF's:
- No `introTime`
- Has `leanofs`, `friction`
- `ammo[4]` instead of `ammo[16]`
- Has `borgAdaptHits[32]`
- `events[2]` instead of `events[4]`
- No `damageShieldCount`

**`snapshot_t`:** SP version has `cmdNum` before `ps`, uses SP-sized `ps` and entities.

**`glconfig_t`:** SP version has `clampToEdgeAvailable` field that ioEF originally lacked (now added to the engine).

### Major Engine Subsystems

- **`code/qcommon/`** -- Shared engine core: console/cvars (`cmd.c`, `cvar.c`), filesystem/PK3 loading (`files.c`), network (`net_chan.c`), collision/trace (`cm_*.c`), VM interpreter (`vm.c`), memory (`z_malloc.c`)
- **`code/client/`** -- Client main loop, input, network parsing, sound system (OpenAL + SDL backends), cgame/UI module loading
- **`code/server/`** -- Server main loop, client connection management, entity snapshots, game module loading, bot integration
- **`code/renderergl1/`** -- OpenGL 1.x fixed-function renderer (default)
- **`code/renderergl2/`** -- Modern OpenGL renderer (off by default, `BUILD_RENDERER_OPENGL2=1`)
- **`code/renderercommon/`** -- Shared renderer code (image loading, font)
- **`code/sys/`** -- Platform abstraction (`sys_win32.c`, `sys_unix.c`)
- **`code/sdl/`** -- SDL2 backend for windowing and input
- **`code/botlib/`** -- Bot AI with AAS pathfinding
- **`code/game/bg_*.c`** -- Shared game/cgame code (player movement in `bg_pmove.c`, item/weapon definitions in `bg_misc.c`)
- **`code/tools/`** -- QVM compilation tools (lcc compiler, q3asm assembler)

### Conditional Compilation

`#ifdef ELITEFORCE` guards appear throughout the codebase for EF-specific behavior (different protocol versions, game data paths, master server). The `STANDALONE` define strips Quake 3 dependencies for standalone games. Both are set via Makefile variables.

## Known Issues / Stubs

The following SP features still need attention:

- **Ambient sound sets:** now minimally bridged via `sound/sound.txt`, but the original Ritual ambient system likely did more than loop/select sounds (for example richer blending/volume behavior).
- **Speech override fidelity:** `gi.S_Override` is now driven by active voice playback, but only as a boolean speech-active signal, not the original per-frame mouth/volume visualization.
- **Force feedback:** `FF_StartFX`, `FF_EnsureFX`, etc. remain no-ops.
- **UI scissor pic fallback:** `cl_ui_sp.c` still falls back to stretch-pic behavior where the original UI used a scissored draw helper.

## Running

```bash
cd build/release-mingw32-x86

# Singleplayer (requires efgamex86.dll and efuix86.dll in baseEF/)
./ioquake3.x86.exe +set sp_game 1 +set r_fullscreen 0 +map borg1

# Automated/headless-friendly launch (suppresses safe mode dialog)
./ioquake3.x86.exe +set sp_game 1 +set r_fullscreen 0 +set com_skipSafeDialog 1 +map borg1

# With console logging
./ioquake3.x86.exe +set sp_game 1 +set r_fullscreen 0 +set logfile 2 +map borg1

# Multiplayer
./ioquake3.x86.exe +set r_fullscreen 0
```

## Debugging

```bash
# Build debug first, then:
cd build/debug-mingw32-x86
gdb ./ioquake3.x86.exe
(gdb) set args +set r_fullscreen 0 +set sp_game 1 +set logfile 2 +map borg1
(gdb) run
```

Console log output (`+set logfile 2`) goes to `baseEF/qconsole.log`.
