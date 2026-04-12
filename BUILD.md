# Building ioEF with Elite Force Singleplayer Support

This document covers building the ioEF engine (with SP bridge code) and the
Elite Reinforce game DLLs from source on Windows.

## Overview

There are two components that must be built separately:

| Component | Toolchain | Output |
|-----------|-----------|--------|
| **ioEF engine** | MSYS2 + MinGW (32-bit GCC) | `ioquake3.x86.exe`, `renderer_opengl1_x86.dll` |
| **Elite Reinforce SP DLLs** | Visual Studio 2017+ | `efgamex86.dll`, `efuix86.dll` |

Both produce **32-bit x86** binaries. The engine loads the SP game DLLs at
runtime via `GetGameAPI` / `dllEntry` / `vmMain` exports.

---

## Prerequisites

### MSYS2 (for the engine)

1. Install MSYS2 from <https://www.msys2.org/>
2. Open the **MSYS2 MINGW32** shell (not MINGW64 or MSYS).
3. Install the 32-bit toolchain and dependencies:

```bash
pacman -S --needed \
  mingw-w64-i686-gcc \
  mingw-w64-i686-SDL2 \
  mingw-w64-i686-openal \
  mingw-w64-i686-curl \
  make git
```

> **Important:** You must use the MINGW32 shell so that `gcc` resolves to the
> i686 (32-bit) compiler. If you run from MINGW64, the Makefile will detect
> `x86_64` and build a 64-bit binary that cannot load the 32-bit game DLLs.

### Visual Studio (for the game DLLs)

- Visual Studio 2017 or later (2022 recommended).
- The **Desktop development with C++** workload.
- Platform toolset v141 or v143.

---

## 1. Build the Engine

From the **MSYS2 MINGW32** shell:

```bash
cd /e/Github/ioef          # or wherever the repo is

# Release build (optimized)
make -j$(nproc)

# --- OR --- Debug build (with symbols, no optimization)
make debug -j$(nproc)
```

Output goes to `build/release-mingw32-x86/` (or `build/debug-mingw32-x86/`).

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
| `USE_OPENAL` | 1 | OpenAL sound backend |
| `USE_CURL` | 1 | HTTP/FTP download support |

Example building only the client with debug symbols:

```bash
make debug -j$(nproc) BUILD_SERVER=0
```

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

1. Open `E:\Github\Elite-Reinforce\EF_SPMod.sln` in Visual Studio.
2. Set the solution configuration to **Release** and platform to **Win32**.
3. Build the solution (Ctrl+Shift+B) or build each project individually:
   - `game` &rarr; produces `Release/efgamex86.dll`
   - `ui` &rarr; produces `Release/efuix86.dll`

### Using the command line

From a **Developer Command Prompt for VS**:

```cmd
cd E:\Github\Elite-Reinforce

msbuild EF_SPMod.sln /p:Configuration=Release /p:Platform=Win32 /m

:: Or build individual projects:
msbuild game\game.vcxproj /p:Configuration=Release /p:Platform=Win32
msbuild ui\ui.vcxproj     /p:Configuration=Release /p:Platform=Win32
```

### Output

```
Elite-Reinforce/Release/
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
  baseEF/
    pak0.pk3               # From the original EF1 game disc/install
    pak1.pk3               # (optional) Patch data
    efgamex86.dll          # From Elite-Reinforce/Release/
    efuix86.dll            # From Elite-Reinforce/Release/
```

### Copying game data

```bash
# From the MSYS2 shell:
BUILDDIR="build/release-mingw32-x86/baseEF"

# Copy original EF1 pak files (adjust source path as needed)
cp "/e/Downloads/Star Trek - Voyager - Elite Force (USA) (Rerelease)/extracted/Setup/baseEF/pak0.pk3" "$BUILDDIR/"

# Copy built SP DLLs
cp /e/Github/Elite-Reinforce/Release/efgamex86.dll "$BUILDDIR/"
cp /e/Github/Elite-Reinforce/Release/efuix86.dll   "$BUILDDIR/"
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
```

Key launch cvars:

| Cvar | Value | Purpose |
|------|-------|---------|
| `sp_game` | 1 | Enable singleplayer game module loading |
| `r_fullscreen` | 0 | Windowed mode |
| `logfile` | 2 | Write console output to `baseEF/qconsole.log` |
| `com_hunkmegs` | 256 | Increase memory if needed |
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

The `.pdb` files from the Elite-Reinforce Release build contain full debug
symbols. Place them next to the DLLs in `baseEF/` for Visual Studio's debugger
or WinDbg to pick up. For GDB, the DWARF info is embedded in the DLL if built
with the Debug configuration.

### Console log

Set `+set logfile 2` on the command line. Output goes to
`baseEF/qconsole.log`. This captures all `Com_Printf` output including SP
bridge diagnostics.

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

| Problem | Solution |
|---------|----------|
| `failed to load efgamex86.dll` | Ensure the DLL is in `baseEF/` and is 32-bit. |
| `Unpure client detected` | Should be auto-fixed by SP mode setting `sv_pure 0`. If not, add `+set sv_pure 0` to launch args. |
| `GetGameAPI returned NULL` | DLL loaded but export not found. Check DLL was built from Elite-Reinforce source (needs `GetGameAPI` export). |
| `game API version mismatch` | DLL API version doesn't match expected version 6. Rebuild DLLs. |
| Engine builds as 64-bit | Use the MSYS2 MINGW32 shell, not MINGW64. Or set `ARCH=x86` explicitly. |
| `uname: command not found` | Run `make` from MSYS2 shell, not PowerShell or cmd. The Makefile requires Unix utilities. |
| Missing `SDL2.dll` | Install `mingw-w64-i686-SDL2` via pacman. The Makefile copies it to the build directory. |
