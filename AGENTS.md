# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

ioEF is a fork of ioquake3 (open-source Quake 3 engine) extended with **Star Trek: Voyager - Elite Force** singleplayer and multiplayer support. It is a C game engine licensed under GPLv2.

When built with `BUILD_ELITEFORCE=1`, the engine defines `ELITEFORCE` and uses `baseEF/` as the game data directory, version 1.38, and the `STEF1` master server heartbeat. Without it, it builds as standard ioquake3 (`baseq3/`, version 1.36).

The SP game module (`efgamex86_64.dll`, built from the separate Elite-Force-VR repository — see Build Commands) uses a **Q2-style `GetGameAPI`** interface, not Q3 VM syscalls. A single `efgamex86_64.dll` contains **both** server game logic (via `GetGameAPI`) and client cgame rendering (via `dllEntry`/`vmMain`). The UI module (`efuix86_64.dll`) uses `GetUIAPI`.

## Documentation — keep it current

`README.md` is the project's front-door overview (what the port is, status, build/run quick start, the documentation index, and **licensing** — engine GPLv2 vs the proprietary STEF Game Source License governing the Elite-Force-VR SP game source). **It is maintained, not write-once.** Whenever a change alters something `README.md` states — supported platforms/arch, the build commands or flags, where the SP source lives, the directory layout, the licensing boundary, or project status — update `README.md` in the same change. The same applies to the rest of the doc set you should keep in sync: `AGENTS.md` (this file), `BUILD.md`, and `android/README.md`. If you add or move a top-level doc, update the documentation index table in `README.md`. Licensing: this repo's engine is GPLv2 (`COPYING.txt`); the EF SP game source (separate Elite-Force-VR repo) is under the proprietary STEF Game Source License, whose text lives in that repo — keep `README.md`'s Licensing section consistent with this split (no STEF license file is kept in this repo).

## Build Commands

The build system is GNU Make. On Windows, use the **MSYS2 MINGW64** shell — the project is **64-bit only** (`x86_64`); 32-bit is abandoned. This Makefile builds the **engine** with MinGW; the **SP game DLLs are built separately** from the Elite-Force-VR repo with Visual Studio (see "Elite Force SP DLLs" below).

```bash
# Release build — engine (64-bit; see flag notes below)
make ARCH=x86_64 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres USE_CODEC_MP3=1 -j4
# Add BUILD_VR=1 to also build the OpenXR VR engine (VR requires ARCH=x86_64).

# Debug build (with -ggdb -O0)
make debug ARCH=x86_64 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres USE_CODEC_MP3=1 -j4

# Clean all build artifacts (or clean-release ARCH=x86_64 to keep baseEF/ paks+DLLs)
make clean
```

> **64-bit only.** The project targets `x86_64` exclusively — the OpenXR VR build requires it and 32-bit is abandoned. Build from the **MSYS2 MINGW64** shell.

This produces the SP client `ioquake3.x86_64.exe` (+ `renderer_opengl1_x86_64.dll`) in `build/release-mingw64-x86_64/`. Flag rationale:
- `BUILD_ELITEFORCE=1` — **required for Elite Force.** Without it the engine builds as stock ioquake3 and looks for `baseq3/pak0.pk3` instead of `baseEF/`, failing at startup with `"pak0.pk3" is missing`. Defines `ELITEFORCE`, sets the game dir to `baseEF/`, version 1.38, and the `STEF1` master server.
- `BUILD_MISSIONPACK=0` — Team Arena's `bg_misc.c` references `entityState_t.generic1`, which the EF `entityState_t` doesn't have. Building missionpack under `ELITEFORCE` fails; EF doesn't use it.
- `BUILD_SERVER=0` — the dedicated server (`ioq3ded`) can't link: the SP bridge in `sv_game_sp.c`/`sv_init.c` references client-only functions (`CL_ShutdownCGame`, `CL_SP_GetStoredSaveComment`, `CL_SP_CopySaveScreenshot`). SP runs the client only.
- `WINDRES=windres` — forcing `ARCH` trips the Makefile's `CROSS_COMPILING` path, which hunts for a prefixed `x86_64-w64-mingw32-windres` that the toolchain doesn't ship (only plain `windres.exe` exists). Without this the resource compile fails (Error 127) and linking aborts with `cannot find .../win_resource.o`.
- `BUILD_GAME_QVM=0` — the LCC QVM compiler in `code/tools/` has a `constexpr` keyword conflict on GCC 15+ (the current MSYS2 gcc is 16.x). QVM bytecode isn't needed for SP.
- `USE_CODEC_MP3=1` — EF voice/dialogue files are `.mp3`; without it all scripted speech is silent.

**Gotcha:** Make does not track changes to command-line variables — after changing a flag (e.g. adding `BUILD_ELITEFORCE=1`), existing `.o` files look up to date and you get a stale binary. Run `make clean-release ARCH=x86_64` first (removes only objects + target binaries; leaves `baseEF/` intact), then rebuild.

Build output goes to `build/release-mingw64-x86_64/` (or `build/debug-mingw64-x86_64/`).

Override variables via command line or by creating a `Makefile.local` file (gitignored, not committed). Key variables: `BUILD_CLIENT`, `BUILD_SERVER`, `BUILD_GAME_SO`, `BUILD_GAME_QVM`, `BUILD_ELITEFORCE`, `BUILD_VR`, `BUILD_RENDERER_OPENGL2`, `USE_OPENAL`, `USE_CURL`, `ARCH`, `PLATFORM`.

Cross-compile for Windows from Linux:
```bash
make PLATFORM=mingw32 ARCH=x86_64 CC=x86_64-w64-mingw32-gcc WINDRES=x86_64-w64-mingw32-windres -j$(nproc)
```

### Elite Force SP DLLs (separate Elite-Force-VR repo)

The SP game/cgame/UI source is **not** in this repo and is **not** built by this Makefile — it lives in the separate **Elite-Force-VR** repository and is built with Visual Studio (MSVC), **64-bit** (`/p:Platform=x64`):

```bash
cd C:\DEV\GitHub\Public\Elite-Force-VR
msbuild EF_SPMod.sln /p:Configuration=Release /p:Platform=x64 \
        /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m
```

(Elite-Force-VR has git submodules under `speedrun/` — run `git submodule update --init --recursive` after a fresh clone. The projects ship targeting toolset v141 + SDK 10.0.15063.0; retarget to an installed toolset/SDK if those aren't present.) After building, copy `efgamex86_64.dll` and `efuix86_64.dll` (from `Elite-Force-VR/Release/`) into the build's `baseEF/` directory (e.g. `build/release-mingw64-x86_64/baseEF/`) — the engine loads them at runtime. The Android arm64 build compiles the same Elite-Force-VR source via its own ndk-build path (`android/jni/EFGame/`, sibling checkout — see `android/README.md`). See `BUILD.md` for full toolchain setup and deploy steps.

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
| `code/server/sv_game_sp.c` | Server-side bridge: loads `efgamex86_64.dll` via `GetGameAPI`, maintains shadow entity array, translates between SP `gentity_t` and engine `sharedEntity_t` |
| `code/client/cl_cgame_sp.c` | Cgame syscall dispatcher: 71 SP syscalls (different numbering from MP's ~50), snapshot builder |
| `code/client/cl_ui_sp.c` | UI bridge: loads `efuix86_64.dll` via `GetUIAPI` |
| `code/qcommon/sp_types.h` | Shared SP struct definitions (all SP-specific types defined here) |
| `code/qcommon/vm.c` | Fake VM support (`isFake` flag for bridge VMs that aren't real QVMs) |

**How SP loading works:**

1. **Server side** (`sv_game_sp.c`): Loads `efgamex86_64.dll`, calls `GetGameAPI` to exchange function pointer tables. A **shadow entity array** (`sv_sp_entities`) translates between the SP game's `gentity_t` layout and the engine's `sharedEntity_t`. `SV_SP_SyncToShared` / `SV_SP_SyncFromShared` do **field-by-field translation** (NOT memcpy, because struct layouts differ). `SV_SP_SyncPlayerState` translates SP playerState to ioEF playerState for the engine snapshot builder.

2. **Client cgame** (`cl_cgame_sp.c`): Gets the DLL handle from server side and calls `dllEntry`/`vmMain` for the cgame portion of the same DLL. `SPCG_GETSNAPSHOT` **bypasses engine delta compression** and builds SP-format snapshots directly from game module live data.

3. **Client UI** (`cl_ui_sp.c`): Loads `efuix86_64.dll` via `GetUIAPI` for the SP menu system.

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

The following SP features are currently stubbed (no-op or minimal implementations):

- **Save/load system:** `AppendToSaveGame`, `ReadFromSaveGame`, etc.
- **ICARUS scripting engine:** `WriteCam` and related hooks
- **Ambient sound sets:** `S_UpdateAmbientSet`, `S_AddLocalSet`, `AS_*`
- **Force feedback:** `FF_StartFX`, `FF_EnsureFX`, etc.
- **Renderer extras:** `R_GetLighting`, `R_DrawRotatePic`, `R_Scissor`, `R_DrawScreenshot`

## Running

```bash
cd build/release-mingw64-x86_64

# Singleplayer (requires efgamex86_64.dll and efuix86_64.dll in baseEF/)
./ioquake3.x86_64.exe +set sp_game 1 +set r_fullscreen 0 +map borg1

# Automated/headless-friendly launch (suppresses safe mode dialog)
./ioquake3.x86_64.exe +set sp_game 1 +set r_fullscreen 0 +set com_skipSafeDialog 1 +map borg1

# With console logging
./ioquake3.x86_64.exe +set sp_game 1 +set r_fullscreen 0 +set logfile 2 +map borg1

# Multiplayer
./ioquake3.x86_64.exe +set r_fullscreen 0
```

Immersive VR uses the shared PCVR/Android stereo replay path: the SP
cgame/render frontend runs once with a union stereo frustum, then the captured
renderer commands are replayed into the left and right OpenXR eye buffers with
per-eye projection and IPD. It is intended to reduce CPU/frontend work and is
not GL multiview. The old immersive two-pass renderer has been removed.

Motion-controlled VR weapons currently cover `WP_PHASER`,
`WP_COMPRESSION_RIFLE`, and `WP_IMOD`. The dominant controller pose is cached in
`vr.weaponposition` / `vr.weaponoffset` / `vr.weaponangles[ANGLES_ADJUSTED]` by
the engine, then consumed by the separate Elite-Force-VR cgame/game. Manual
alignment cvars live in the Elite-Force-VR cgame and follow the JKXR format
`scale,offsetX,offsetY,offsetZ,pitch,yaw,roll`: `vr_weapon_adjustment_1`
(Phaser), `vr_weapon_adjustment_2` (Compression Rifle), and
`vr_weapon_adjustment_3` (IMOD). The engine also owns `vr_weapon_pitchadjust`,
an archived JKXR-style global aim pitch bias defaulting to `-20.0`; it is applied
when deriving `vr.weaponangles[ANGLES_ADJUSTED]` from the raw controller pose.

## Debugging

```bash
# Build debug first, then:
cd build/debug-mingw64-x86_64
gdb ./ioquake3.x86_64.exe
(gdb) set args +set r_fullscreen 0 +set sp_game 1 +set logfile 2 +map borg1
(gdb) run
```

Console log output (`+set logfile 2`) goes to `baseEF/qconsole.log`.
