# Building ioEF with Elite Force Singleplayer Support

This document covers building the ioEF engine (with SP bridge code) and the
Elite Force VR game DLLs from source on Windows.

---

## Quick Start

For experienced developers who already have the toolchains installed:

```bash
# 1. Engine (MSYS2 MINGW32 shell)
cd /c/DEV/GitHub/Public/ioef-sp          # or wherever the repo is
make ARCH=x86 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres -j$(nproc)        # release
# or replace `make` with `make debug` for a debug build.
# (See "Build the Engine" below for why these flags are needed.)

# 2. Game DLLs (VS Developer Command Prompt, or full path to MSBuild.exe)
cd C:\DEV\GitHub\Public\Elite-Force-VR
msbuild EF_SPMod.sln /p:Configuration=Release /p:Platform=x86 ^
        /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m

# 3. Deploy
cp /c/DEV/GitHub/Public/Elite-Force-VR/Release/ef{game,ui}x86.dll \
   build/release-mingw32-x86/baseEF/

# 4. Run
cd build/release-mingw32-x86
./ioquake3.x86.exe +set sp_game 1 +set r_fullscreen 0 +map borg1
```

---

## Overview

There are two components that must be built separately with different
toolchains. Both produce **32-bit x86** binaries. The engine loads the SP game
DLLs at runtime via `GetGameAPI` / `dllEntry` / `vmMain` exports.

| Component | Toolchain | Output |
|-----------|-----------|--------|
| **ioEF engine** | MSYS2 + MinGW (32-bit GCC) | `ioquake3.x86.exe`, `renderer_opengl1_x86.dll` |
| **Elite Force VR SP DLLs** | Visual Studio 2017+ | `efgamex86.dll`, `efuix86.dll` |

> **Why two toolchains?** The ioEF engine inherits ioquake3's GNU Make build
> system, which targets GCC/MinGW. The Elite Force VR game DLLs were developed
> with Visual Studio and rely on MSVC-specific project files. Both produce
> standard Win32 DLLs with C linkage, so they interoperate at runtime regardless
> of which compiler built them.

---

## Prerequisites

### MSYS2 (for the engine)

1. Install MSYS2 from <https://www.msys2.org/>
2. Open the **MSYS2 MINGW32** shell (not MINGW64 or MSYS).
3. Install the 32-bit toolchain and dependencies:

```bash
pacman -S --needed \
  mingw-w64-i686-toolchain \
  mingw-w64-i686-SDL2 \
  mingw-w64-i686-openal \
  mingw-w64-i686-curl \
  make git
```

> The `mingw-w64-i686-toolchain` group provides the 32-bit `gcc` **and**
> `windres` (from binutils), both of which the build needs.

> **Important:** Use the MINGW32 shell so that `gcc` resolves to the i686
> (32-bit) compiler. If you run from MINGW64, the Makefile detects `x86_64` and
> builds a 64-bit binary that cannot load the 32-bit game DLLs. Even from a
> MINGW32 shell, on some MSYS2 setups `uname -m` still reports `x86_64`; the
> recommended `ARCH=x86 ... WINDRES=windres` flags (see below) make the build
> robust against this.

### Visual Studio (for the game DLLs)

- Visual Studio 2017 or later (2022 recommended).
- The **Desktop development with C++** workload.
- A platform toolset and Windows SDK. The projects ship targeting **v141 +
  SDK 10.0.15063.0**; if you don't have those exact versions installed, either
  retarget the solution in VS (right-click solution &rarr; *Retarget solution*)
  or override at the command line with `/p:PlatformToolset=` and
  `/p:WindowsTargetPlatformVersion=` (see section 2). v143 + a current Win10/11
  SDK build cleanly.

---

## 1. Build the Engine

From the **MSYS2 MINGW32** shell:

```bash
cd /c/DEV/GitHub/Public/ioef-sp          # or wherever the repo is

# Release build (optimized) — produces the SP client ioquake3.x86.exe
make ARCH=x86 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres -j$(nproc)

# --- OR --- Debug build (with symbols, no optimization): replace `make` with `make debug`
```

Output goes to `build/release-mingw32-x86/` (or `build/debug-mingw32-x86/`).

#### Why these flags

`BUILD_ELITEFORCE=1`, `BUILD_MISSIONPACK=0`, and `BUILD_SERVER=0` are **mandatory**
for an EF SP build (they reflect the current state of the SP source). `ARCH`,
`WINDRES`, and `BUILD_GAME_QVM` make the build reliable across MSYS2 setups; in
an ideal MINGW32 shell those three are harmless no-ops.

| Flag | Reason |
|------|--------|
| `BUILD_ELITEFORCE=1` | **Required.** Defines `ELITEFORCE` and switches the game dir to `baseEF/` (version 1.38, `STEF1` master server). Without it the engine builds as stock ioquake3, looks in `baseq3/`, and aborts at startup with `"pak0.pk3" is missing`. |
| `BUILD_MISSIONPACK=0` | Team Arena's `bg_misc.c` uses `entityState_t.generic1`, a field the EF `entityState_t` removed, so missionpack fails to compile under `ELITEFORCE`. EF doesn't use missionpack. |
| `BUILD_SERVER=0` | The dedicated server (`ioq3ded`) fails to link: the SP bridge in `sv_game_sp.c` / `sv_init.c` references client-only functions (`CL_ShutdownCGame`, `CL_SP_GetStoredSaveComment`, `CL_SP_CopySaveScreenshot`). SP runs the client binary only. |
| `ARCH=x86` | Forces a 32-bit build. On some MSYS2 installs `uname -m` reports `x86_64` even from the MINGW32 shell, so without this the Makefile builds 64-bit and fails with `cc1.exe: sorry, unimplemented: 64-bit mode not compiled in`. |
| `WINDRES=windres` | Forcing `ARCH=x86` trips the Makefile's `CROSS_COMPILING` logic, which then looks for a prefixed `i686-w64-mingw32-windres` that the MINGW32 toolchain doesn't ship. Only plain `windres.exe` exists. Without this, the `.rc` resource compile silently fails (Error 127) and the final link aborts with `cannot find .../win_resource.o`. |
| `BUILD_GAME_QVM=0` | Skips QVM bytecode compilation, which fails on GCC 15+ (see the `constexpr` note under Troubleshooting) and is not needed for SP. |

> **Gotcha:** Make does not track changes to command-line variables. If you
> built once and then add/change a flag like `BUILD_ELITEFORCE=1`, the existing
> `.o` files look up to date and make will *not* recompile them — you'll get a
> stale binary. Run `make clean-release ARCH=x86` first (this only removes
> object files and target binaries; it leaves `baseEF/` and its paks/DLLs
> intact), then rebuild.

### Key files produced

```
build/release-mingw32-x86/
  ioquake3.x86.exe           # Main client executable
  renderer_opengl1_x86.dll   # OpenGL renderer
  SDL2.dll                    # SDL2 runtime (copied from toolchain)
  baseEF/                     # Game data directory (populated separately)
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

If building on a Linux host targeting Windows:

```bash
make PLATFORM=mingw32 ARCH=x86 \
     CC=i686-w64-mingw32-gcc \
     WINDRES=i686-w64-mingw32-windres \
     -j$(nproc)
```

---

## 2. Build the Game DLLs

### Using Visual Studio GUI

1. Open `C:\DEV\GitHub\Public\Elite-Force-VR\EF_SPMod.sln` in Visual Studio.
2. Set the solution configuration to **Release** and platform to **x86**.
3. If prompted that the toolset/SDK (v141 / SDK 10.0.15063.0) is missing,
   right-click the solution &rarr; *Retarget solution* and pick an installed
   toolset (e.g. v143) and SDK.
4. Build the solution (Ctrl+Shift+B) or build each project individually:
   - `game` &rarr; produces `Release/efgamex86.dll`
   - `ui` &rarr; produces `Release/efuix86.dll`

### Using the command line

From a **Developer Command Prompt for VS** (or invoke `MSBuild.exe` by full
path). The solution platform is named **x86** (it maps to `Win32` internally),
so pass `/p:Platform=x86`:

```cmd
cd C:\DEV\GitHub\Public\Elite-Force-VR

msbuild EF_SPMod.sln /p:Configuration=Release /p:Platform=x86 /m

:: If the shipped toolset/SDK is not installed, retarget on the command line
:: (non-persistent; leaves the .vcxproj files unchanged):
msbuild EF_SPMod.sln /p:Configuration=Release /p:Platform=x86 ^
        /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m
```

> The full path to MSBuild for VS 2022 Community is
> `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe`.

### Output

```
Elite-Force-VR/Release/
  efgamex86.dll    # SP game module (contains both game + cgame code)
  efgamex86.pdb    # Debug symbols
  efuix86.dll      # SP UI module
  efuix86.pdb      # Debug symbols
```

---

## 3. Set Up Game Data

The engine needs the original Elite Force game data (`.pk3` files) and the
freshly built DLLs in a `baseEF/` directory.

### Directory layout

```
build/release-mingw32-x86/
  ioquake3.x86.exe
  renderer_opengl1_x86.dll
  SDL2.dll
  libgcc_s_dw2-1.dll      # mingw32 GCC runtime — renderer DLL depends on it
  libwinpthread-1.dll     # mingw32 runtime (copy alongside)
  libstdc++-6.dll         # mingw32 runtime (copy alongside)
  baseEF/
    pak0.pk3               # From the original EF1 game disc/install
    pak1.pk3               # (optional) Patch data
    efgamex86.dll          # From Elite-Force-VR/Release/
    efuix86.dll            # From Elite-Force-VR/Release/
```

### Copying the mingw32 runtime DLLs

`ioquake3.x86.exe` is statically linked, but `renderer_opengl1_x86.dll` imports
`libgcc_s_dw2-1.dll`. Without it next to the exe, the renderer fails to load
with `The specified module could not be found` and the engine aborts at
`Initializing Renderer`. Copy the runtime DLLs from the MINGW32 toolchain:

```bash
BUILDDIR="build/release-mingw32-x86"
cp /c/msys64/mingw32/bin/libgcc_s_dw2-1.dll  "$BUILDDIR/"
cp /c/msys64/mingw32/bin/libwinpthread-1.dll "$BUILDDIR/"
cp /c/msys64/mingw32/bin/libstdc++-6.dll     "$BUILDDIR/"
```

### Copying game data

```bash
# From the MSYS2 shell:
BUILDDIR="build/release-mingw32-x86/baseEF"

# Copy original EF1 pak files (adjust source path as needed)
cp "/c/Program Files (x86)/GOG Galaxy/Games/Star Trek Elite Force/baseEF/"*.pk3 "$BUILDDIR/"

# Copy built SP DLLs
cp /c/DEV/GitHub/Public/Elite-Force-VR/Release/efgamex86.dll "$BUILDDIR/"
cp /c/DEV/GitHub/Public/Elite-Force-VR/Release/efuix86.dll   "$BUILDDIR/"
```

---

## 4. Launch

### Singleplayer mode

```bash
cd build/release-mingw32-x86

# Launch the borg1 map in SP mode (windowed)
./ioquake3.x86.exe +set r_fullscreen 0 +set sp_game 1 +map borg1

# With logging enabled (writes qconsole.log to baseEF/)
./ioquake3.x86.exe +set r_fullscreen 0 +set sp_game 1 +set logfile 2 +map borg1

# Suppress the safe-mode dialog (useful for automated/headless launches)
./ioquake3.x86.exe +set r_fullscreen 0 +set sp_game 1 +set com_skipSafeDialog 1 +map borg1
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

### Multiplayer mode (standard ioEF)

```bash
./ioquake3.x86.exe +set r_fullscreen 0
```

---

## 5. Debugging

### GDB

```bash
cd build/release-mingw32-x86

# Build with debug symbols first (make debug)
gdb ./ioquake3.x86.exe

# In GDB:
(gdb) set args +set r_fullscreen 0 +set sp_game 1 +set logfile 2 +map borg1
(gdb) run
```

### Debug symbols for game DLLs

The `.pdb` files from the Elite-Force-VR Release build contain full debug
symbols. Place them next to the DLLs in `baseEF/` for Visual Studio's debugger
or WinDbg to pick up. For GDB, the DWARF info is embedded in the DLL if built
with the Debug configuration.

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
| `code/server/sv_game_sp.c` | Server-side bridge: loads `efgamex86.dll` via `GetGameAPI`, maintains a shadow entity array, translates between SP `gentity_t` and engine `sharedEntity_t` |
| `code/client/cl_cgame_sp.c` | Cgame syscall dispatcher: 71 SP syscalls with different numbering from MP's ~50, plus snapshot builder |
| `code/client/cl_ui_sp.c` | UI bridge: loads `efuix86.dll` via `GetUIAPI`, provides save-game helpers |
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

1. **Server side** (`sv_game_sp.c`): Loads `efgamex86.dll`, calls its
   `GetGameAPI` export to exchange function pointer tables (Q2-style API).
   A shadow entity array translates between the SP game's `gentity_t` layout
   and the engine's `sharedEntity_t`.

2. **Client cgame** (`cl_cgame_sp.c`): Gets the DLL handle from the server
   side and calls `dllEntry` / `vmMain` for the cgame portion of the same
   DLL. Translates SP cgame syscall numbers (which differ from MP) to engine
   functions.

3. **Client UI** (`cl_ui_sp.c`): Loads `efuix86.dll` and calls its
   `GetUIAPI` export for the SP menu system.

### Key differences from MP

- `sv_pure` is automatically set to 0 in SP mode.
- The SP game module uses different struct layouts for `playerState_t` and
  `entityState_t` (extra fields for phaser recharge, lean, scale, Borg
  adaptation, etc.).
- The SP cgame has 71 syscalls with different numbering from MP's ~50.
- The single `efgamex86.dll` contains both server-side game logic (via
  `GetGameAPI`) and client-side cgame rendering (via `dllEntry`/`vmMain`).

---

## Troubleshooting

### Common build errors

#### GCC 15+ `constexpr` keyword conflict

**Symptom:** Build fails in `code/tools/lcc/` with errors about `constexpr`
being a reserved keyword.

**Cause:** GCC 15 added `constexpr` as a C23 keyword (the issue persists on
GCC 16, the current MSYS2 i686 toolchain). The LCC tool (QVM compiler) uses
`constexpr` as an identifier in its own source code.

**Fix:** Disable QVM building, which is not needed for SP development:

```bash
make ARCH=x86 BUILD_ELITEFORCE=1 WINDRES=windres -j$(nproc) BUILD_GAME_QVM=0
```

#### JPEG struct redefinition / type mismatch

**Symptom:** Compiler errors about `jpeg_common_struct` or related JPEG types
being redefined or having incompatible types.

**Cause:** Conflict between the system-installed libjpeg headers and the
bundled `code/jpeg-8c/` source. This typically happens when a system libjpeg
development package is installed alongside the bundled copy.

**Fix:** The engine uses its bundled JPEG library by default. Ensure you are
not passing extra `-I` include paths that pull in system JPEG headers. If
you have `mingw-w64-i686-libjpeg-turbo` installed, consider removing it or
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
./ioquake3.x86.exe +set sp_game 1 +set r_fullscreen 0 +set com_skipSafeDialog 1 +map borg1
```

### Runtime errors

| Problem | Solution |
|---------|----------|
| `"pak0.pk3" is missing ... baseq3 directory` | The engine was built **without** `BUILD_ELITEFORCE=1`, so it's looking in `baseq3/` instead of `baseEF/`. Rebuild with `BUILD_ELITEFORCE=1` (run `make clean-release ARCH=x86` first — see the gotcha under "Build the Engine"). |
| Compile error: `entityState_t ... has no member named 'generic1'` | A missionpack source file under `ELITEFORCE`. Add `BUILD_MISSIONPACK=0`. |
| Link error: undefined reference to `CL_ShutdownCGame` / `CL_SP_GetStoredSaveComment` / `CL_SP_CopySaveScreenshot` | The dedicated-server link pulling in client-only SP bridge symbols. Add `BUILD_SERVER=0` (SP uses the client binary only). |
| `failed to load efgamex86.dll` | Ensure the DLL is in `baseEF/` and is 32-bit. |
| `Unpure client detected` | Should be auto-fixed by SP mode setting `sv_pure 0`. If not, add `+set sv_pure 0` to launch args. |
| `GetGameAPI returned NULL` | DLL loaded but export not found. Check DLL was built from Elite-Force-VR source (needs `GetGameAPI` export). |
| `game API version mismatch` | DLL API version doesn't match expected version 6. Rebuild DLLs. |
| Engine builds as 64-bit (`64-bit mode not compiled in`) | Use the MSYS2 MINGW32 shell, not MINGW64. If `uname -m` still reports `x86_64`, set `ARCH=x86` explicitly. |
| Link fails: `cannot find .../win_resource.o` | The `windres` resource compile failed (often Error 127). Pass `WINDRES=windres` so it uses the unprefixed binary instead of a missing `i686-w64-mingw32-windres`. |
| `uname: command not found` | Run `make` from MSYS2 shell, not PowerShell or cmd. The Makefile requires Unix utilities. |
| Missing `SDL2.dll` | Install `mingw-w64-i686-SDL2` via pacman. The Makefile copies it to the build directory. |
| Renderer fails to load: `The specified module could not be found` | `renderer_opengl1_x86.dll` needs `libgcc_s_dw2-1.dll` next to the exe. Copy the mingw32 runtime DLLs (see "Copying the mingw32 runtime DLLs"). |
