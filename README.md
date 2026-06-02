# ioEF VR — Star Trek: Voyager – Elite Force in VR

A virtual-reality port of the **Star Trek: Voyager – Elite Force** single-player
campaign, built on a fork of the open-source **[ioquake3](https://ioquake3.org/)**
engine. It runs the original Raven single-player game — cinematics, ICARUS
scripting, NPCs, weapons and all — rendered stereoscopically through **OpenXR**,
on both PC VR headsets and standalone Android headsets (Meta Quest / Pico).

> This repository is the **VR engine**. The Elite Force single-player game code
> lives in the companion **Elite-Force-VR** repository, and all game assets are
> the property of their respective rights holders. **Nothing here ships game
> data.** You must own a copy of Elite Force and supply your own `baseEF/` `.pk3`
> files to play. See [Licensing](#licensing).

---

## What this is

Stock Elite Force single-player is a Quake 3 (id Tech 3) game whose logic lives
in a Q2-style game module. ioEF VR re-hosts it on a modernised ioquake3 engine
and adds:

- **OpenXR stereoscopic rendering** — per-eye projection, 6DoF head tracking, a
  virtual screen for 2D menus and cinematics.
- **Motion-controller input** — thumbstick locomotion, snap/smooth turn, trigger
  fire, grip crouch, face-button jump/use, and a laser-pointer for menus.
- **Two delivery targets:**
  - **PC VR** (SteamVR / Oculus / WMR / Pico via OpenXR) — native Windows x86_64.
  - **Standalone Android** (Meta Quest, Pico) — arm64 `.apk`.

It is a fork of ioquake3 with an **Elite Force single-player bridge** (the
`sp_game` path) plus a self-contained VR layer (`code/vr/`). The single-player
game/cgame/UI itself is a **separate repository** (Elite-Force-VR) that builds
into the `efgame*` / `efui*` modules the engine loads at runtime. The full
upstream ioquake3 readme — including its cvar/command reference — is preserved in
[`README-ioquake3.md`](README-ioquake3.md).

---

## Status

| Target | State |
|--------|-------|
| **PC VR (OpenXR, x86_64)** | Engine builds with MinGW; SP DLLs build from the Elite-Force-VR repo (MSVC). SP campaign loads and runs, with in-headset VR rendering (per-eye, head-tracked). |
| **Standalone Android (Quest/Pico, arm64)** | Builds to an `.apk` (engine `.so` + the SP `.so` compiled from the Elite-Force-VR source via ndk-build); loads and renders the campaign in-headset. |

This is active R&D — expect rough edges. See the per-area docs below for the
current detail of each subsystem.

---

## Repository layout

```
code/            ioquake3 engine (C) — client, server, renderers, qcommon, sys, sdl, botlib
code/vr/         OpenXR VR layer (shared + windows/ + android/ platform glue)
code/qcommon/sp_types.h   Shared SP struct layouts for the engine↔game bridge
android/         Standalone Quest/Pico build (Gradle + ndk-build; jni/, EFGame/, EFXR/)
Makefile         GNU Make build for the PC engine
baseEF/          (in a build dir) game-data + SP DLLs the engine loads at runtime
```

The SP game/cgame/UI/ICARUS C++ source is **not** in this repo — it lives in the
companion **Elite-Force-VR** repository (checked out as a sibling) and is built
separately (MSVC for PC VR, ndk-build for Android). Key engine↔game bridge files:
`code/server/sv_game_sp.c`, `code/client/cl_cgame_sp.c`, `code/client/cl_ui_sp.c`,
`code/qcommon/sp_types.h`. The SP game module exposes a Q2-style `GetGameAPI`
(server game) **and** Q3-style `dllEntry`/`vmMain` (cgame) from a single DLL; the
UI module uses `GetUIAPI`.

---

## Building

The project is **64-bit only** (`x86_64` / `arm64`). Full details are in
[`BUILD.md`](BUILD.md) (PC VR) and [`android/README.md`](android/README.md)
(standalone). In brief:

### PC VR (Windows)

```bash
# 1. Engine — MSYS2 MINGW64 shell
make ARCH=x86_64 BUILD_ELITEFORCE=1 BUILD_VR=1 \
     BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 \
     WINDRES=windres USE_CODEC_MP3=1 -j4

# 2. SP game DLLs — Visual Studio (from the Elite-Force-VR repo, x64), then copy
#    Elite-Force-VR/Release/ef{game,ui}x86_64.dll into build/.../baseEF/
```

The engine build produces `ioquake3.x86_64.exe`; the Elite-Force-VR build produces
`efgamex86_64.dll` + `efuix86_64.dll`, which you copy into the engine's `baseEF/`.

### Standalone Android (Quest / Pico)

```bash
cd android
gradle assembleDebug          # or ./gradlew assembleDebug
# -> build/outputs/apk/debug/efxr-debug.apk
```

The Android build (ndk-build, NDK r26) compiles the Elite-Force-VR source (sibling
checkout) into `libefgameaarch64.so` / `libefuiaarch64.so` and packages them with
the engine `libefxr.so`.

---

## Running

Supply your own retail Elite Force game data (`baseEF/pak0.pk3`, …) and the SP
DLLs (built above).

**PC VR:**
```bash
cd build/release-mingw64-x86_64
./ioquake3.x86_64.exe +set sp_game 1 +map borg1
```

**Standalone:** sideload paks to `/sdcard/EFXR/baseEF/`, install the APK, launch
from the headset. See [`android/README.md`](android/README.md).

A flat (non-VR) build runs by omitting `BUILD_VR=1` and `vr_enable` — useful for
debugging gameplay without a headset.

---

## Documentation

| Doc | Covers |
|-----|--------|
| [`CLAUDE.md`](CLAUDE.md) | Architecture, the SP bridge, struct-layout differences, build flags, known stubs. **The primary engineering reference.** |
| [`BUILD.md`](BUILD.md) | Full PC VR build/deploy/debug walkthrough and troubleshooting. |
| [`android/README.md`](android/README.md) | Standalone Quest/Pico build, OpenXR loader, sideloading. |
| [`README-ioquake3.md`](README-ioquake3.md) | Upstream ioquake3 readme (cvar/command reference, modding, lineage). |
| [`opengl2-readme.md`](opengl2-readme.md) | The optional modern GL2 renderer. |

---

## Licensing

This project spans code under **two different licenses** — read both before
distributing anything.

### 1. Engine (this repo) — GNU GPL v2

The ioquake3-derived engine (everything under `code/`, the `Makefile`, the VR
layer, and the build glue) is licensed under the **GNU General Public License,
version 2**, inherited from Quake III Arena / ioquake3. The full text is in
[`COPYING.txt`](COPYING.txt). The original id Software release notes are in
[`id-readme.txt`](id-readme.txt).

### 2. Elite Force SP game source — STEF Game Source License (proprietary)

The Elite Force single-player game/cgame/UI/ICARUS source (in the companion
**Elite-Force-VR** repository, built into the `efgame*`/`efui*` modules) derives
from Raven Software's Star Trek: Voyager – Elite Force SDK and is governed by the
**STEF Game Source License** — a *proprietary* license, separate from the GPL,
which ties that code to the retail Elite Force product. Its full text lives with
that source in the **Elite-Force-VR** repository (`STEF Game Source License.doc`).
Keep the SP game source under its own terms, distinct from the GPLv2 engine.

### 3. Game assets — not included

Maps, models, textures, sounds and other data (`baseEF/*.pk3`) are the property
of their rights holders (Raven Software / Activision / CBS) and are **not part of
this repository**. You must own Elite Force and provide your own copy.

> The GPLv2 engine and the STEF-licensed game source are kept in **separate
> repositories with separate licenses on purpose.** If you fork or redistribute,
> keep those license boundaries intact, and never commit game data.

---

## Credits & lineage

- **id Software** — Quake III Arena and its GPL source release.
- **Raven Software** — Star Trek: Voyager – Elite Force and its single-player SDK.
- **The ioquake3 team** — the maintained engine base this fork builds on.
- **Team Beef** and the VR community — the OpenXR porting patterns this VR layer
  draws on (RealRTCWXR, JKXR, RTCWQuest, OpenJKDF2).
