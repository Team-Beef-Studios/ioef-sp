# Investigation Task: ioEF Elite Force SP — scripted-gameplay pipeline (ICARUS + server→cgame state)

## Your mission
In this ioEF Elite Force singleplayer build, **scripted gameplay doesn't work**. Find the root cause(s) in the engine↔game bridge and fix them. Two confirmed symptoms, believed to share one root:
1. **Level intro cinematics don't play.** When you start a new game there's a dead period (static pre-spawn view) then gameplay just begins. EF level intros are **in-engine ICARUS-driven camera sequences** (the cgame's `CGCam`, gated by the `in_camera` flag), NOT RoQ video files.
2. **Script/target-controlled entity state isn't applied.** Example: a force field that should block a doorway until an associated "Distribution Node" is destroyed is **non-solid from the start** — the player walks straight through it.

Hypothesis: the **ICARUS level scripts aren't executing** and/or the **server→client communication of scripted state is broken/incomplete** in the bridge. Confirm which, then fix it without regressing anything.

## Project / architecture
- Engine repo: `C:\DEV\GitHub\Public\ioef-sp` — a fork of ioquake3 with Elite Force SP support. **Read `CLAUDE.md` and the memory files first** (see below); they document the build and every fix made so far.
- The SP game logic AND the client cgame both live in one precompiled DLL, **`efgamex86.dll`**, built from `C:\DEV\GitHub\Public\Elite-Force-VR` (Visual Studio). The SP menu is `efuix86.dll`. The ICARUS engine source is in `Elite-Force-VR\icarus\`.
- That one DLL exposes BOTH a Q2-style `GetGameAPI` (server game; function-pointer tables `gi.*` import / `ge->*` export) AND a Q3-style `dllEntry`/`vmMain` + numbered syscalls (client cgame). It has **one shared global `syscall` pointer** used by cgame-side code.
- Engine↔module "bridge" files:
  - `code/server/sv_game_sp.c` — server bridge: loads the DLL, shadow entity array `sv_sp_entities` translating SP `gentity_t` ↔ engine `sharedEntity_t`, `SV_SP_GameVmMain` command dispatch, `SV_SP_SyncToShared`/`SyncFromShared`, `SV_SP_SyncPlayerState`, the `gi.*` import table.
  - `code/client/cl_cgame_sp.c` — cgame syscall dispatcher (`SPCG_*` numbers; `SPCG_GETSNAPSHOT` builds SP snapshots directly).
  - `code/client/cl_ui_sp.c` — SP UI bridge.
  - `code/qcommon/sp_types.h` — ALL SP struct layouts (entityState/playerState/snapshot/usercmd/gentity).
  - `code/server/sv_game.c` — `SV_GameSystemCalls` (engine game syscall handler) and `SV_SP_GameSystemCalls` (SP wrapper guarding misrouted cgame traps).

## THE central bug pattern (internalize this)
The SP DLL and the engine share many enum/flag/struct **names** with **different values or layouts**. The bridge must **translate field-by-field**; **raw-copying across the bridge is the recurring bug.** Confirmed this session:
- `usercmd_t`: engine `byte buttons`, SP `int buttons` → every later field shifts. (Fixed by translating in `GAME_CLIENT_THINK` and `SPCG_GETUSERCMD`.)
- `svFlags`: SP `SVF_NPC`=0x4 == engine `SVF_CLIENTMASK`=0x4; SP `SVF_TRIMODEL`=0x100 == engine `SVF_SINGLECLIENT`; etc. Raw copy made NPCs invisible. (Fixed: forward only matching bits.)
- cgame trap numbers misrouting through the game syscall handler when shared DLL code calls them during server `ClientThink`/`RunFrame` (FF traps 34–37, `IN_PVS` 26) → crashes. (Guarded by arg validation in `SV_SP_GameSystemCalls`.)
- `entityState_t`/`playerState_t`/`snapshot_t` differ (documented in `sp_types.h`).
Note: `CONTENTS_*` values were checked and **do match** between SP and engine, so the force field is NOT a contents mismatch.

## Already working — do NOT re-investigate
Build works; boots to SP menu; loads borg1; no crashes; controls correct; enemies spawn and are visible. Engine/bridge **blockers are resolved**. Your work is the gameplay-scripting layer on top.

## Strong leads — start here
1. **`SPCG_GETSNAPSHOT` zeroes scripted-state delivery.** In `cl_cgame_sp.c`, the snapshot builder hard-codes `spSnap->numServerCommands = 0` and `numConfigstringChanges = 0` every frame. Server commands and configstring changes are how the server pushes scripted state to the client. Verify whether server commands (`trap_GetServerCommand` → `SPCG_GETSERVERCOMMAND` → `CL_GetServerCommand`) and configstring updates actually reach the SP cgame. If not, client-side scripted things (cinematics, objectives, HUD) can't work.
2. **Does ICARUS run level scripts at all?** ICARUS initializes ("ICARUS version 1.33" appears in the log), but confirm it actually executes the level/worldspawn/entity scripts (spawnscripts, the intro camera script, the node→force-field logic). ICARUS runs inside the DLL; it needs `gi.*` services (file reads for compiled `.IBI` scripts, cvar/entity access). Check `sv_game_sp.c`'s `gi.*` import table for missing/stubbed entries ICARUS depends on. Add server-side `Com_Printf` diagnostics at ICARUS run / `Q3_Interface` command dispatch points if needed.
3. **The cgame camera path.** `Elite-Force-VR\cgame\cg_camera.cpp` `CGCam_Enable`/`CGCam_Update` set `in_camera`; `cg_view.cpp` renders `CGCam_RenderScene()` when `in_camera`. The normal trigger is NOT the `cam_enable` console command (that's debug). `cg_servercmds.cpp CG_ServerCommand` only handles `cp/cs/print/chat/st/ct/cts/gt/lt/clientLevelShot/vmsg` — no camera command — so the cinematic trigger comes from elsewhere (playerState fields? a server command the bridge drops? a configstring?). Trace how the SERVER (ICARUS `SET_CAMERA`) is supposed to communicate camera state to the cgame, and find where that breaks in the bridge.
4. **Entity targeting / "use" chains.** Confirm whether triggering/destroying an entity fires its `target` and runs the target's use function server-side (pure game-DLL logic via `gi.*`). If targeting works, the node→force-field link should work; if the force field is script-spawned/controlled, it ties back to ICARUS (lead 2).

## Build / run / debug (machine-specific — use exactly)
Build (release): `$env:MSYSTEM='MINGW32'; & C:\msys64\usr\bin\bash.exe -lc "cd /c/DEV/GitHub/Public/ioef-sp && make ARCH=x86 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres -j$(nproc)"`
- All six flags required. Output: `build/release-mingw32-x86/ioquake3.x86.exe`.
- After changing a make FLAG (not just code) run `make clean-release ARCH=x86` first.
- Debug build for symbols: `make debug ARCH=x86 ...` → `build/debug-mingw32-x86/`. Run it with `+set fs_basepath C:/DEV/GitHub/Public/ioef-sp/build/release-mingw32-x86` so it finds `baseEF` (paks + DLLs). Copy `libgcc_s_dw2-1.dll`, `libwinpthread-1.dll`, `libstdc++-6.dll` from `C:\msys64\mingw32\bin` next to the debug exe (the renderer dll needs libgcc).

Run SP (release): from `build/release-mingw32-x86`: `./ioquake3.x86.exe +set sp_game 1 +set r_fullscreen 0 +set com_skipSafeDialog 1 +set logfile 2 +map borg1`. Log: `C:\Users\simon\AppData\Roaming\STVEF\baseEF\qconsole.log`. (`baseEF/autoexec.cfg` already sets `seta sp_game 1` + `seta logfile 2`.)

Rebuild the SP DLLs only if you change DLL source: `& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" C:\DEV\GitHub\Public\Elite-Force-VR\EF_SPMod.sln /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m` then copy `Elite-Force-VR\Release\ef{game,ui}x86.dll` into `build/release-mingw32-x86/baseEF/`.

Crash backtrace (gdb at `C:\msys64\mingw32\bin\gdb.exe`; can't read the MSVC PDB so DLL frames show as `efgamex86!...`, but engine frames + the syscall arg number are usually enough). Run it via `run_in_background` and Monitor the output file:
`$env:MSYSTEM='MINGW32'; & C:\msys64\usr\bin\bash.exe -lc "cd /c/DEV/GitHub/Public/ioef-sp/build/debug-mingw32-x86 && gdb -batch -ex 'set pagination off' -ex run -ex bt --args ./ioquake3.x86.exe +set com_skipSafeDialog 1 +set sp_game 1 +set fs_basepath 'C:/DEV/GitHub/Public/ioef-sp/build/release-mingw32-x86' +map borg1"`

## Pitfalls (learned the hard way — don't repeat)
- **DO NOT** bind the cgame stub on the DLL's shared syscall pointer during server game calls (`SV_SP_BindCgameStub`/`RestoreCgameSyscall`). It **hangs loading at the "COMPLETING SCAN" phase** (player never spawns). Tried twice, abandoned. For misrouted cgame traps use **per-syscall argument validation** in `SV_SP_GameSystemCalls`.
- Runtime `map borg1` (from console/autoexec after the menu) loads the **server** but the **client stays on the menu** — screenshots via that path show the menu, not gameplay. Server-side diagnostics are still valid. To get the client in-game, put `+map borg1` on the **launch command line**.
- `"SP entity data located: 2 entities"` is a transient first-link count, NOT the final count (borg1 ≈ 634 entities / 560 in-use / 22 NPCs).
- `SV_AreaEntities: MAXCOUNT` spam is a separate non-fatal symptom (now rate-limited), likely another misroute. Not your focus.
- "Process still alive" ≠ "working" — a loading hang looks identical. Verify with a screenshot or the log/diagnostics, never assume.
- The PowerShell tool sometimes says the safety classifier is "temporarily unavailable" — just retry.

## Diagnostic techniques that worked
- Temporary rate-limited `Com_Printf` in the bridge to dump struct/flag/arg values; rebuild; run; read `qconsole.log`. (This nailed the controls swap and invisible-enemies bugs.)
- To read SP gentities server-side after `ge->Init`: iterate `(sp_gentity_t*)((byte*)ge->gentities + i*ge->gentitySize)` for `i < ge->num_entities`; fields per `sp_types.h` (`s`, `client`, `inuse`, `linked`, `svFlags`, `contents`, `currentOrigin`, `mins`/`maxs`, `owner`, ...).
- gdb `-batch ... -ex run -ex bt` for crashes.

## Memory files (read these — they have full context on prior fixes)
`C:\Users\simon\.claude\projects\C--DEV-GitHub-Public-ioef-sp\memory\` — `MEMORY.md` index plus: build-environment, sp-snapshot-not-active-fix, sp-ui-menu-fixes, sp-cgame-syscall-stub-fix, sp-controls-usercmd-translation-fix, sp-svflags-translation-fix.

## Deliverables
1. A clear root-cause explanation for why scripted gameplay (cinematics + scripted entity state) doesn't work.
2. Engine-side bridge fix(es) that make at least the intro cinematic play and/or the force-field-by-node-destruction work — **without breaking loading**. Prefer wiring/translating the missing server→client communication over raw copies. **Verify loading still completes and the level is playable after every change.**
3. Update the memory files with findings.

## Method
Read `CLAUDE.md` + memory first. Form a hypothesis → add targeted diagnostics → rebuild → run → read the log → iterate. Make minimal, well-understood changes; verify loading + playability after each. Never claim success without visual or log confirmation. Note there are uncommitted changes from the prior session (engine `ioef-sp` and `Elite-Force-VR/cgame/cg_snapshot.cpp`) — do not discard them.
