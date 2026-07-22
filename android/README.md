# EFXR — Elite Force VR (Meta Quest / Pico standalone)

Standalone Android/OpenXR build of the ioEF singleplayer engine. The engine
(`libefxr.so`) runs natively on the headset; the SP game logic loads at runtime
from `libefgameaarch64.so` / `libefuiaarch64.so` (built from the **sibling**
Elite-Force-VR checkout — see `jni/EFGame/`).

## Toolchain
- Android Studio with **NDK r26 (26.1.10909125)** and CMake (any; native build
  is ndk-build), **SDK 34**, build via **Gradle 8.5 / AGP 8.2.2**.
- Native build is **ndk-build** (`jni/Android.mk` + `jni/Application.mk`);
  arm64-v8a only.

## One-time setup
1. **OpenXR loader** — the single standard Khronos loader
   (`jni/AndroidPrebuilt/jni/libopenxr_loader.so`) is already vendored and is
   packaged into the APK; it resolves the active runtime via the system OpenXR
   broker on both Quest and Pico (matches OpenJKDF2). No per-vendor loader.
2. **Elite-Force-VR** — clone it as a sibling of this repo
   (`C:\DEV\GitHub\Public\Elite-Force-VR`); `jni/EFGame/` references it by
   relative path. Do **not** add it as a submodule.
3. `local.properties` with `sdk.dir=...` (Android Studio writes this).

## Build
The gradle **wrapper script/jar is not checked in** (only
`gradle/wrapper/gradle-wrapper.properties`), so build from Android Studio, or
invoke a local Gradle 8.5 directly:
```
cd android
gradle assembleDebug          # or: Build > Make Project in Android Studio
adb install -r build/outputs/apk/debug/efxr-debug.apk
```
`assembleDebug` runs ndk-build for all five `.so` (engine + gl4es + OpenXR loader
+ the two SP modules from the sibling Elite-Force-VR checkout).

The debug APK is signed with the auto-generated `~/.android/debug.keystore`, so
it **cannot update a release-signed install in place** — `adb install` fails with
`INSTALL_FAILED_UPDATE_INCOMPATIBLE`. Either `adb uninstall com.teambeefvr.efxr`
first (game data and saves live in `/sdcard/EFXR/` and survive), or sign the
release build with the same key as the installed one.

For a native-only iteration loop (no Android Studio rebuild), run `ndk-build`
directly and then `tools/repack-install.ps1`, which swaps the fresh `.so` into
the last debug APK, re-signs and installs it.

## Game data (on device)
Sideload the retail paks to `/sdcard/EFXR/baseEF/` (`pak0.pk3`, `pak3.pk3`, …).
Optional `/sdcard/EFXR/commandline.txt`, e.g.:
```
+set fs_basepath /sdcard/EFXR +set fs_game baseEF +set com_basegame baseEF +map borg1
```
Saves go to `/sdcard/EFXR/baseEF/save/`.

## Runtime rendering

Immersive VR uses the shared PCVR/Android stereo replay path: the SP
cgame/render frontend is built once per frame, then the captured renderer
backend commands are replayed into both OpenXR eye buffers with per-eye
projection and IPD. This is intended to reduce CPU/frontend work; it is not GL
multiview and still draws each eye separately.

Every weapon the player carries is motion-controlled, plus the tricorder and
hypos (`FIRST_WEAPON`..`LAST_VR_WEAPON`). The Android build uses the same
Elite-Force-VR source and the same archived alignment cvars as PCVR, one per
weapon index:

```
vr_weapon_adjustment_1   # WP_PHASER
vr_weapon_adjustment_2   # WP_COMPRESSION_RIFLE
vr_weapon_adjustment_3   # WP_IMOD
...
vr_weapon_adjustment_14  # WP_RED_HYPO
```

Each value is `scale,offsetX,offsetY,offsetZ,pitch,yaw,roll`. Defaults are
tuned in Elite-Force-VR and can be overridden from
`/sdcard/EFXR/commandline.txt` while tuning.
The shared engine cvar `vr_weapon_pitchadjust` is also archived and defaults to
`-20.0`, matching JKXR; use it for global controller aim pitch correction before
tuning per-weapon model alignment.
Set `vr_align_weapons 1` to enable the shared JKXR-style alignment utility on
standalone as well as PCVR. Enter/exit with the off-hand primary button; use
dominant stick left/right to select a field, dominant stick up/down to adjust,
dominant stick click to zero, and A/B to switch weapon. Values are written
immediately to `vr_weapon_adjustment_N`.

## Controls
Identical to PCVR — see [`../CONTROLS.md`](../CONTROLS.md). Note the grips are
the **selector wheels** (dominant = weapons, off-hand = inventory/quick menu),
so **crouch is the off-hand thumbstick click**, not the grip.

## Status
- **M3a** (this): gradle/ndk scaffolding + engine `.so` build files + engine
  edits (`sys_loadlib.h` Android dlopen, `sys_main.c` `main()` guard).
- **M3b**: OpenXR-on-Android VR layer in `code/vr/android/` (in progress).
- **M3c**: SP game/UI `.so` from Elite-Force-VR + in-headset run.
