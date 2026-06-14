# Building ioEF with Elite Force Singleplayer Support

This document covers building the ioEF engine (this repo) and the Elite Force SP
game/UI DLLs (the separate **Elite-Force-VR** repo) from source on Windows. The
engine is built with MSYS2 MinGW (64-bit); the SP DLLs are built with Visual
Studio and copied into the engine's `baseEF/`.

---

## Quick Start

For experienced developers who already have the toolchains installed:

```bash
# 1. Engine (MSYS2 MINGW64 shell)
cd /c/DEV/GitHub/Public/ioef-sp          # or wherever the repo is
make ARCH=x86_64 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres USE_CODEC_MP3=1 -j4
# add BUILD_VR=1 for the OpenXR VR engine; use `make debug ...` for a debug build.

# 2. SP game DLLs (Visual Studio / MSBuild, from the Elite-Force-VR repo, x64)
cd C:\DEV\GitHub\Public\Elite-Force-VR
msbuild EF_SPMod.sln /p:Configuration=Release /p:Platform=x64 ^
        /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m

# 3. Deploy the DLLs into the engine's baseEF/
cp /c/DEV/GitHub/Public/Elite-Force-VR/Release/ef{game,ui}x86_64.dll \
   build/release-mingw64-x86_64/baseEF/

# 4. Run
cd build/release-mingw64-x86_64
./ioquake3.x86_64.exe +set sp_game 1 +set r_fullscreen 0 +map borg1
```

---

## Overview

Two components, built with different toolchains; both produce **64-bit** binaries.
The engine loads the SP game/UI DLLs at runtime via `GetGameAPI` / `dllEntry` /
`vmMain` / `GetUIAPI` exports.

| Component | Toolchain / source | Output |
|-----------|--------------------|--------|
| **ioEF engine** | MSYS2 + MinGW, this repo (`code/`) | `ioquake3.x86_64.exe`, `renderer_opengl1_x86_64.dll` |
| **Elite Force SP DLLs** | Visual Studio 2017+, the **Elite-Force-VR** repo | `efgamex86_64.dll`, `efuix86_64.dll` |

> **64-bit only.** The project targets `x86_64` exclusively — the OpenXR VR build
> requires it and 32-bit is abandoned. The SP DLLs are built from the separate
> Elite-Force-VR repository with MSVC; both produce standard Win64 DLLs with C
> linkage, so they interoperate at runtime regardless of compiler. The Android
> arm64 build compiles the same Elite-Force-VR source via ndk-build — see
> `android/README.md`.

---

## Prerequisites

### MSYS2 (the only toolchain)

1. Install MSYS2 from <https://www.msys2.org/>
2. Open the **MSYS2 MINGW64** shell (not MINGW32 or MSYS).
3. Install the 64-bit toolchain and dependencies:

```bash
pacman -S --needed \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-SDL2 \
  mingw-w64-x86_64-openal \
  mingw-w64-x86_64-curl \
  mingw-w64-x86_64-libmad \
  make git
```

> The `mingw-w64-x86_64-toolchain` group provides `gcc` and `windres`. `libmad`
> is needed for `USE_CODEC_MP3=1` (EF voice/dialogue is `.mp3`); copy `libmad.a`
> into `code/libs/win64/` if the Makefile doesn't pick it up — see the engine notes.

> **Important:** use the **MINGW64** shell so `gcc` resolves to the `x86_64`
> compiler.

### Visual Studio (for the SP game DLLs)

The Elite Force SP DLLs are built from the separate **Elite-Force-VR** repository
with Visual Studio (that game source is C++ with MSVC project files):

- Visual Studio 2017 or later (2022 recommended), with **Desktop development with C++**.
- A platform toolset + Windows SDK. The projects ship targeting **v141 + SDK
  10.0.15063.0**; if those aren't installed, retarget the solution in VS or
  override at the command line with `/p:PlatformToolset=` and
  `/p:WindowsTargetPlatformVersion=` (see section 2). v143 + a current Win10/11
  SDK build cleanly.

---

## 1. Build the Engine

From the **MSYS2 MINGW64** shell (this builds the **engine**; the SP DLLs are a
separate Visual Studio build — see section 2):

```bash
cd /c/DEV/GitHub/Public/ioef-sp          # or wherever the repo is

# Release build (optimized) — the engine
make ARCH=x86_64 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres USE_CODEC_MP3=1 -j4

# --- OR --- Debug build (with symbols, no optimization): replace `make` with `make debug`
# --- VR --- add BUILD_VR=1 to also build the OpenXR VR engine.
```

Output goes to `build/release-mingw64-x86_64/` (or `build/debug-mingw64-x86_64/`).

#### Why these flags

`BUILD_ELITEFORCE=1`, `BUILD_MISSIONPACK=0`, and `BUILD_SERVER=0` are **mandatory**
for an EF SP engine build. `ARCH=x86_64`, `WINDRES`, `BUILD_GAME_QVM`, and
`USE_CODEC_MP3` round out a known-good invocation.

| Flag | Reason |
|------|--------|
| `BUILD_ELITEFORCE=1` | **Required.** Defines `ELITEFORCE` and switches the game dir to `baseEF/` (version 1.38, `STEF1` master server). Without it the engine builds as stock ioquake3, looks in `baseq3/`, and aborts at startup with `"pak0.pk3" is missing`. |
| `BUILD_MISSIONPACK=0` | Team Arena's `bg_misc.c` uses `entityState_t.generic1`, a field the EF `entityState_t` removed, so missionpack fails to compile under `ELITEFORCE`. EF doesn't use missionpack. |
| `BUILD_SERVER=0` | The dedicated server (`ioq3ded`) fails to link: the SP bridge in `sv_game_sp.c` / `sv_init.c` references client-only functions (`CL_ShutdownCGame`, `CL_SP_GetStoredSaveComment`, `CL_SP_CopySaveScreenshot`). SP runs the client binary only. |
| `ARCH=x86_64` | The project is 64-bit only (32-bit abandoned; the VR build requires x86_64). |
| `WINDRES=windres` | Forcing `ARCH` trips the Makefile's `CROSS_COMPILING` logic, which then looks for a prefixed `x86_64-w64-mingw32-windres` that the MINGW64 toolchain doesn't ship. Only plain `windres.exe` exists. Without this, the `.rc` resource compile silently fails (Error 127) and the final link aborts with `cannot find .../win_resource.o`. |
| `BUILD_GAME_QVM=0` | Skips QVM bytecode compilation, which fails on GCC 15+ (see the `constexpr` note under Troubleshooting) and is not needed for SP. |
| `USE_CODEC_MP3=1` | EF voice/dialogue files are `.mp3`; without it all scripted speech is silent (needs libmad). |

> **Gotcha:** Make does not track changes to command-line variables. If you
> built once and then add/change a flag like `BUILD_ELITEFORCE=1`, the existing
> `.o` files look up to date and make will *not* recompile them — you'll get a
> stale binary. Run `make clean-release ARCH=x86_64` first (this only removes
> object files and target binaries; it leaves `baseEF/` and its paks/DLLs
> intact), then rebuild.

### Key files produced

```
build/release-mingw64-x86_64/
  ioquake3.x86_64.exe           # Main client executable
  renderer_opengl1_x86_64.dll   # OpenGL renderer
  SDL264.dll                    # SDL2 runtime (copied from toolchain)
  baseEF/
    efgamex86_64.dll            # SP game + cgame (from the Elite-Force-VR MSVC build)
    efuix86_64.dll              # SP UI          (from the Elite-Force-VR MSVC build)
    pak0.pk3 ...                # Game data (added separately, see below)
```

### Useful make variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `BUILD_CLIENT` | 1 | Build the client binary |
| `BUILD_SERVER` | 1 | Build the dedicated server |
| `BUILD_GAME_SO` | 0 | Build baseq3 game DLLs (MP; not needed for SP) |
| `BUILD_GAME_QVM` | 1 | Build QVM bytecode (set to 0 to skip; required on GCC 15+, see note below) |
| `USE_OPENAL` | 1 | OpenAL sound backend |
| `USE_CURL` | 1 | HTTP/FTP download support |

Example building only the client with debug symbols:

```bash
make debug -j$(nproc) BUILD_SERVER=0
```

You can also create a `Makefile.local` file (gitignored) to persistently
override variables without passing them on every command line.

### Cross-compiling from Linux

If building the engine on a Linux host targeting Windows:

```bash
make PLATFORM=mingw32 ARCH=x86_64 \
     CC=x86_64-w64-mingw32-gcc \
     WINDRES=x86_64-w64-mingw32-windres \
     -j$(nproc)
```

### Editing & building from Visual Studio (NMake project)

The repo ships a Visual Studio **NMake (Makefile-type) project** so you can browse
the engine source, get IntelliSense, and trigger builds from the IDE. It is **not**
an MSVC build of the engine — it is a thin wrapper that shells out to the same
MSYS2/MinGW `make` command documented above. The compiler, flags, and output are
identical whether you build from the MINGW64 shell or from inside Visual Studio;
**nothing about the MinGW build changes.** This is purely a convenience layer for
people who prefer the IDE for editing and debugging.

Files (generated; checked in):

| File | Role |
|------|------|
| `ioef-sp.sln` | Solution to open in Visual Studio. |
| `ioef-sp.vcxproj` | NMake project — its `NMakeBuildCommandLine` etc. invoke `make` via `C:\msys64\usr\bin\bash.exe`. |
| `ioef-sp.vcxproj.filters` | Source-tree folder layout for the Solution Explorer. |
| `gen_vs_project.sh` | Regenerates the three files above (re-run after adding/removing source files). |

**Prerequisites:** the same MSYS2/MinGW toolchain as the command-line build,
installed at `C:\msys64` (the path is baked into the project's build commands),
plus Visual Studio 2022 with the **Desktop development with C++** workload (for the
IDE, IntelliSense, and the NMake project system). The engine is still compiled by
MinGW `gcc`, not MSVC.

> **Repo path is auto-derived.** The IDE build commands `cd` into the repo via an
> MSYS path baked into the project. `gen_vs_project.sh` derives this from its own
> location (`pwd`), so the generated `ioef-sp.vcxproj` is correct for whatever
> machine/checkout ran the script — **no manual path editing.** If your checkout
> differs from the committed project's path, just re-run `./gen_vs_project.sh` (see
> below) and the path is regenerated. To force a specific path, set
> `REPO_MSYS=/c/your/path ./gen_vs_project.sh`. The build also requires MSYS2 at
> `C:\msys64` (the `bash.exe` path is fixed in the project).

**Use the x64 configurations only.** The project defines `Debug|x64`, `Release|x64`,
`Debug|Win32`, and `Release|Win32`, but **32-bit is abandoned** — only the **x64**
configurations are supported. Ignore (or delete) the Win32 ones.

**Build-flag parity.** The NMake command lines use
`ARCH=x86_64 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres`
but **omit `USE_CODEC_MP3=1` and `BUILD_VR=1`**. A build driven from the IDE as-is
therefore has **no MP3 voice/dialogue and no VR** — fine for quick engine edits,
but not equal to the canonical build above. To match it, add those flags to the
`make` invocations (edit `gen_vs_project.sh` and regenerate, or edit
`ioef-sp.vcxproj` directly).

**IntelliSense classification.** `gen_vs_project.sh` reads the `.d` dependency files
under `build/` to decide which sources are actually compiled on Windows; those
become `<ClCompile>` (IntelliSense on) and everything else (Android/Unix/GL2/null
stubs) is listed as `<None>` (browsable, IntelliSense off, so VS doesn't error on
missing platform headers like `jni.h`). **Run a command-line build first**, then
regenerate, so the classification is accurate:

```bash
# from the MSYS2 MINGW64 shell, after at least one successful make:
./gen_vs_project.sh
```

> The SP **game/cgame/UI** code is not part of this project — it lives in the
> separate Elite-Force-VR repo and has its own MSVC solution (`EF_SPMod.sln`, see
> section 2). This NMake project covers the **engine** only.

---

## 2. Build the SP Game DLLs (Visual Studio, from the Elite-Force-VR repo)

The Elite Force SP game/cgame/UI source is **not** in this repo — it lives in the
separate **Elite-Force-VR** repository and is built with Visual Studio (MSVC),
**64-bit**. Open `EF_SPMod.sln` in VS (Release / x64), or build from a Developer
Command Prompt:

```cmd
cd C:\DEV\GitHub\Public\Elite-Force-VR
msbuild EF_SPMod.sln /p:Configuration=Release /p:Platform=x64 /m
:: If the shipped toolset/SDK (v141 / SDK 10.0.15063.0) isn't installed, override:
msbuild EF_SPMod.sln /p:Configuration=Release /p:Platform=x64 ^
        /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m
```

Output → `Elite-Force-VR/Release/efgamex86_64.dll` (SP server game + cgame, via
`GetGameAPI` / `dllEntry` / `vmMain`) and `efuix86_64.dll` (SP UI, via `GetUIAPI`).
Copy both into the engine's `baseEF/` (see section 3). Elite-Force-VR has git
submodules under `speedrun/` — run `git submodule update --init --recursive`
after a fresh clone.

> The Android arm64 build compiles the same Elite-Force-VR source via its own
> ndk-build path (`android/jni/EFGame/`, sibling checkout) — see `android/README.md`.

---

## 3. Set Up Game Data

The engine needs the original Elite Force game data (`.pk3` files) in `baseEF/`,
plus the SP DLLs from section 2.

### Directory layout

```
build/release-mingw64-x86_64/
  ioquake3.x86_64.exe
  renderer_opengl1_x86_64.dll
  SDL264.dll
  libgcc_s_seh-1.dll      # mingw64 GCC runtime — renderer DLL depends on it
  libwinpthread-1.dll     # mingw64 runtime (copy alongside)
  libstdc++-6.dll         # mingw64 runtime (copy alongside)
  baseEF/
    pak0.pk3               # From the original EF1 game disc/install
    pak1.pk3 ...           # (optional) Patch data
    efgamex86_64.dll       # From Elite-Force-VR/Release/ (section 2)
    efuix86_64.dll         # From Elite-Force-VR/Release/ (section 2)
```

### Copying the mingw64 runtime DLLs

`ioquake3.x86_64.exe` is statically linked, but `renderer_opengl1_x86_64.dll` imports
`libgcc_s_seh-1.dll`. Without it next to the exe, the renderer fails to load
with `The specified module could not be found` and the engine aborts at
`Initializing Renderer`. Copy the runtime DLLs from the MINGW64 toolchain:

```bash
BUILDDIR="build/release-mingw64-x86_64"
cp /c/msys64/mingw64/bin/libgcc_s_seh-1.dll  "$BUILDDIR/"
cp /c/msys64/mingw64/bin/libwinpthread-1.dll "$BUILDDIR/"
cp /c/msys64/mingw64/bin/libstdc++-6.dll     "$BUILDDIR/"
```

### Copying game data

```bash
# From the MSYS2 shell:
BUILDDIR="build/release-mingw64-x86_64/baseEF"

# Copy original EF1 pak files (adjust source path as needed)
cp "/c/Program Files (x86)/GOG Galaxy/Games/Star Trek Elite Force/baseEF/"*.pk3 "$BUILDDIR/"

# Copy the SP DLLs from the Elite-Force-VR build (section 2)
cp /c/DEV/GitHub/Public/Elite-Force-VR/Release/ef{game,ui}x86_64.dll "$BUILDDIR/"
```

---

## 4. Launch

### Singleplayer mode

```bash
cd build/release-mingw64-x86_64

# Launch the borg1 map in SP mode (windowed)
./ioquake3.x86_64.exe +set r_fullscreen 0 +set sp_game 1 +map borg1

# With logging enabled (writes qconsole.log to baseEF/)
./ioquake3.x86_64.exe +set r_fullscreen 0 +set sp_game 1 +set logfile 2 +map borg1

# Suppress the safe-mode dialog (useful for automated/headless launches)
./ioquake3.x86_64.exe +set r_fullscreen 0 +set sp_game 1 +set com_skipSafeDialog 1 +map borg1
```

Key launch cvars:

| Cvar | Value | Purpose |
|------|-------|---------|
| `sp_game` | 1 | Enable singleplayer game module loading |
| `r_fullscreen` | 0 | Windowed mode |
| `logfile` | 2 | Write console output to `baseEF/qconsole.log` |
| `com_hunkmegs` | 256 | Increase memory if needed |
| `com_skipSafeDialog` | 1 | Skip the safe-mode dialog on startup |
| `r_mode` | -1 | Custom resolution (use with r_customwidth/height) |

### Stereo Replay

Immersive VR uses the shared PCVR/Android stereo replay path. The engine still
uses the normal OpenXR per-eye swapchains, but the SP cgame and renderer frontend
run once per frame with a union stereo frustum; the captured renderer commands
are then replayed into the left and right eye buffers with per-eye projection and
IPD.

This is intended to reduce CPU/frontend work. It is not GL multiview and does
not reduce GPU draw submission to a single eye-independent draw. The old
immersive two-pass renderer has been removed; virtual-screen VR mode and flat
non-VR rendering remain separate paths.

### Motion-Controlled Weapons

In VR, the Phaser (`WP_PHASER`), Compression Rifle (`WP_COMPRESSION_RIFLE`) and
IMOD (`WP_IMOD`) are positioned from the dominant controller instead of being
fixed to the headset. Their traces/effects originate from the rendered muzzle
when available, falling back to the controller pose if the cgame has not updated
the muzzle that frame.

Manual weapon alignment is handled in the Elite-Force-VR cgame with archived
cvars:

| Cvar | Weapon | Format |
|------|--------|--------|
| `vr_weapon_adjustment_1` | Phaser | `scale,offsetX,offsetY,offsetZ,pitch,yaw,roll` |
| `vr_weapon_adjustment_2` | Compression Rifle | `scale,offsetX,offsetY,offsetZ,pitch,yaw,roll` |
| `vr_weapon_adjustment_3` | IMOD | `scale,offsetX,offsetY,offsetZ,pitch,yaw,roll` |

These are JKXR-style adjustment values. Current source defaults in
Elite-Force-VR are:

| Cvar | Default |
|------|---------|
| `vr_weapon_adjustment_1` | `1.000,-5.340,6.600,-15.480,0.000,2.300,0.000` |
| `vr_weapon_adjustment_2` | `1.215,-1.860,4.700,-5.780,0.000,0.000,0.000` |
| `vr_weapon_adjustment_3` | `1.960,-2.040,2.660,-5.660,0.000,1.100,0.000` |

The shared engine cvar `vr_weapon_pitchadjust` applies the JKXR-style global
controller aim pitch bias before the per-weapon adjustment is applied. Its
default is `-20.0`, matching JKXR. Use it to correct the natural controller aim
angle globally, then use the per-weapon cvars for model-specific scale,
position, and rotation.

For live in-headset tuning, set `vr_align_weapons 1` and press the off-hand
primary button to enter the JKXR-style alignment utility. In alignment mode,
dominant-stick left/right selects `scale`, `right`, `up`, `forward`, `pitch`,
`yaw`, or `roll`; dominant-stick up/down adjusts the selected value; dominant
stick click zeros it; A/B switches weapon; the off-hand primary button exits.
Every adjustment is written immediately to the active weapon's archived
`vr_weapon_adjustment_N` cvar, so the aligned pose remains after leaving the
mode.

### Multiplayer mode (standard ioEF)

```bash
./ioquake3.x86_64.exe +set r_fullscreen 0
```

---

## 5. Debugging

### GDB

```bash
cd build/release-mingw64-x86_64

# Build with debug symbols first (make debug)
gdb ./ioquake3.x86_64.exe

# In GDB:
(gdb) set args +set r_fullscreen 0 +set sp_game 1 +set logfile 2 +map borg1
(gdb) run
```

### Debug symbols for game DLLs

Build the SP DLLs in Elite-Force-VR with the **Debug** configuration to get
symbols. The MSVC `.pdb` files (next to the DLLs in `Elite-Force-VR/Release/` or
`Debug/`) are picked up by Visual Studio's debugger / WinDbg; place them beside
the DLLs in `baseEF/`. For GDB, build the engine with `make debug` for engine-side
symbols (the MSVC-built game DLLs won't symbolicate under GDB).

### Console log

Set `+set logfile 2` on the command line. Output goes to
`baseEF/qconsole.log`. This captures all `Com_Printf` output including SP
bridge diagnostics.

### MCP Inspector Server

The `mcp/` directory contains a Model Context Protocol (MCP) server for live
game engine inspection. It can query entity state, memory layouts, struct
validation, and other runtime diagnostics from a running ioEF instance.

```bash
# Build and run the MCP inspector server
cd mcp
npm install
npm run build
npm start        # or: npm run dev (for development with auto-reload)
```

The inspector communicates with the game engine over a local connection and
exposes tools for entity inspection, cvar queries, and struct validation.
See `mcp/package.json` for available scripts.

---

## Project Structure

### Engine source (`code/`)

| Directory | Contents |
|-----------|----------|
| `code/client/` | Client main loop, input handling, sound, cgame/UI module loading |
| `code/server/` | Server main loop, client connections, entity snapshots, game module loading |
| `code/qcommon/` | Shared engine core: console/cvars, filesystem, network, collision, VM, memory |
| `code/renderergl1/` | OpenGL 1.x fixed-function renderer (default) |
| `code/renderergl2/` | Modern OpenGL renderer (off by default; `BUILD_RENDERER_OPENGL2=1`) |
| `code/renderercommon/` | Shared renderer code (image loading, fonts) |
| `code/sys/` | Platform abstraction (`sys_win32.c`, `sys_unix.c`) |
| `code/sdl/` | SDL2 backend for windowing and input |
| `code/botlib/` | Bot AI with AAS pathfinding |
| `code/game/` | Server-side game rules (MP; SP uses external DLL) |
| `code/cgame/` | Client-side prediction, HUD, effects (MP) |
| `code/ui/`, `code/q3_ui/` | Menu system (MP) |
| `code/tools/` | QVM compilation tools (lcc compiler, q3asm assembler) |
| `code/jpeg-8c/`, `code/zlib/` | Bundled third-party libraries |

### SP bridge files (the core of the ioEF singleplayer support)

| File | Role |
|------|------|
| `code/server/sv_game_sp.c` | Server-side bridge: loads `efgamex86_64.dll` via `GetGameAPI`, maintains a shadow entity array, translates between SP `gentity_t` and engine `sharedEntity_t` |
| `code/client/cl_cgame_sp.c` | Cgame syscall dispatcher: 71 SP syscalls with different numbering from MP's ~50, plus snapshot builder |
| `code/client/cl_ui_sp.c` | UI bridge: loads `efuix86_64.dll` via `GetUIAPI`, provides save-game helpers |
| `code/qcommon/sp_types.h` | Shared SP struct definitions (`sp_entityState_t`, `sp_playerState_t`, etc.) |
| `code/qcommon/vm.c` | Fake VM support (`isFake` flag for bridge VMs that are not real QVMs) |

### Other key files

| File | Role |
|------|------|
| `Makefile` | GNU Make build system for the engine |
| `CLAUDE.md` | Development context and architecture notes |
| `mcp/` | MCP inspector server for live runtime diagnostics |

---

## Architecture Notes

### How SP loading works

The `sp_game` cvar triggers an alternate code path:

1. **Server side** (`sv_game_sp.c`): Loads `efgamex86_64.dll`, calls its
   `GetGameAPI` export to exchange function pointer tables (Q2-style API).
   A shadow entity array translates between the SP game's `gentity_t` layout
   and the engine's `sharedEntity_t`.

2. **Client cgame** (`cl_cgame_sp.c`): Gets the DLL handle from the server
   side and calls `dllEntry` / `vmMain` for the cgame portion of the same
   DLL. Translates SP cgame syscall numbers (which differ from MP) to engine
   functions.

3. **Client UI** (`cl_ui_sp.c`): Loads `efuix86_64.dll` and calls its
   `GetUIAPI` export for the SP menu system.

### Key differences from MP

- `sv_pure` is automatically set to 0 in SP mode.
- The SP game module uses different struct layouts for `playerState_t` and
  `entityState_t` (extra fields for phaser recharge, lean, scale, Borg
  adaptation, etc.).
- The SP cgame has 71 syscalls with different numbering from MP's ~50.
- The single `efgamex86_64.dll` contains both server-side game logic (via
  `GetGameAPI`) and client-side cgame rendering (via `dllEntry`/`vmMain`).

---

## Troubleshooting

### Common build errors

#### GCC 15+ `constexpr` keyword conflict

**Symptom:** Build fails in `code/tools/lcc/` with errors about `constexpr`
being a reserved keyword.

**Cause:** GCC 15 added `constexpr` as a C23 keyword (the issue persists on
GCC 16, the current MSYS2 x86_64 toolchain). The LCC tool (QVM compiler) uses
`constexpr` as an identifier in its own source code.

**Fix:** Disable QVM building, which is not needed for SP development:

```bash
make ARCH=x86_64 BUILD_ELITEFORCE=1 WINDRES=windres -j4 BUILD_GAME_QVM=0
```

#### JPEG struct redefinition / type mismatch

**Symptom:** Compiler errors about `jpeg_common_struct` or related JPEG types
being redefined or having incompatible types.

**Cause:** Conflict between the system-installed libjpeg headers and the
bundled `code/jpeg-8c/` source. This typically happens when a system libjpeg
development package is installed alongside the bundled copy.

**Fix:** The engine uses its bundled JPEG library by default. Ensure you are
not passing extra `-I` include paths that pull in system JPEG headers. If
you have `mingw-w64-x86_64-libjpeg-turbo` installed, consider removing it or
setting `USE_INTERNAL_JPEG=1` explicitly.

#### glconfig_t size check assertion

**Symptom:** Runtime error or assertion about `glconfig_t` size mismatch when
loading the SP game DLLs.

**Cause:** The SP game module includes a `clampToEdgeAvailable` field in its
`glconfig_t` that the original ioquake3 `glconfig_t` did not have. If the
engine and DLL disagree on the struct size, rendering initialization fails.

**Fix:** This is already patched in ioEF -- the engine's `glconfig_t` includes
the `clampToEdgeAvailable` field. If you see this error, make sure you are
building from the current ioEF source (not upstream ioquake3) and that both
the engine and game DLLs are freshly built.

#### Safe-mode dialog blocks automated launch

**Symptom:** The engine shows a "safe mode" dialog box on startup asking
whether to reset settings, which blocks headless or automated launches.

**Fix:** Pass `+set com_skipSafeDialog 1` on the command line:

```bash
./ioquake3.x86_64.exe +set sp_game 1 +set r_fullscreen 0 +set com_skipSafeDialog 1 +map borg1
```

### Runtime errors

| Problem | Solution |
|---------|----------|
| `"pak0.pk3" is missing ... baseq3 directory` | The engine was built **without** `BUILD_ELITEFORCE=1`, so it's looking in `baseq3/` instead of `baseEF/`. Rebuild with `BUILD_ELITEFORCE=1` (run `make clean-release ARCH=x86_64` first — see the gotcha under "Build the Engine"). |
| Compile error: `entityState_t ... has no member named 'generic1'` | A missionpack source file under `ELITEFORCE`. Add `BUILD_MISSIONPACK=0`. |
| Link error: undefined reference to `CL_ShutdownCGame` / `CL_SP_GetStoredSaveComment` / `CL_SP_CopySaveScreenshot` | The dedicated-server link pulling in client-only SP bridge symbols. Add `BUILD_SERVER=0` (SP uses the client binary only). |
| `failed to load efgamex86_64.dll` | Ensure the DLL (from the Elite-Force-VR x64 build) is in `baseEF/` and is 64-bit. |
| `Unpure client detected` | Should be auto-fixed by SP mode setting `sv_pure 0`. If not, add `+set sv_pure 0` to launch args. |
| `GetGameAPI returned NULL` | DLL loaded but export not found. Check it was built from Elite-Force-VR source (needs the `GetGameAPI` export) for the matching arch. |
| `game API version mismatch` | DLL API version doesn't match expected version 6. Rebuild DLLs. |
| `cc1.exe: sorry, unimplemented: 64-bit mode not compiled in` | You're in the 32-bit MINGW32 shell. Use the MSYS2 **MINGW64** shell (the project is 64-bit only). |
| Link fails: `cannot find .../win_resource.o` | The `windres` resource compile failed (often Error 127). Pass `WINDRES=windres` so it uses the unprefixed binary instead of a missing `x86_64-w64-mingw32-windres`. |
| `uname: command not found` | Run `make` from MSYS2 shell, not PowerShell or cmd. The Makefile requires Unix utilities. |
| Missing `SDL2.dll` | Install `mingw-w64-x86_64-SDL2` via pacman. The Makefile copies it to the build directory. |
| Renderer fails to load: `The specified module could not be found` | `renderer_opengl1_x86_64.dll` needs `libgcc_s_seh-1.dll` next to the exe. Copy the mingw64 runtime DLLs (see "Copying the mingw64 runtime DLLs"). |
