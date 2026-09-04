# VR Controller Scheme

This documents the current OpenXR controller mapping as implemented in
`code/vr/VrInputCommon.c` (shared gameplay-input logic) and
`code/vr/windows/OpenXrInput.c` / `code/vr/android/OpenXrInput.c` (per-profile
action bindings). The PCVR and standalone Android/Quest builds share the exact
same mapping — the two `OpenXrInput.c` files are byte-for-byte identical.

Default scheme shown below is **right-handed** (`vr_control_scheme 0`, the
shipped default): the right controller is the dominant/weapon hand, the left
is the off-hand.

```
      LEFT CONTROLLER (off-hand)                          RIGHT CONTROLLER (dominant / weapon hand)
      ───────────────────────────                         ──────────────────────────────────────────

              ___________                                          ___________
             /           \                                        /           \
            /   [Y]  [X]  \                                      /   [B]  [A]  \
           /               \                                    /               \
          |                 |                                  |                 |
          |    ,-------.    |                                  |    ,-------.    |
          |   ( STICK   )   |                                  |   ( STICK   )   |
          |    `-------'    |                                  |    `-------'    |
          |                 |                                  |                 |
           \   [≡ Menu]    /                                    \               /
            \             /                                      \             /
             \  TRIGGER  /                                        \  TRIGGER  /
              \____|____/                                          \____|____/
                   |                                                    |
               [ GRIP ]                                             [ GRIP ]

  Y button ............ Open in-game menu (Esc)          A button ............ Jump  (Skip cutscene, if one's playing)
  X button (hold) ...... Mission-objectives info screen   B button ............ Alt fire
  Stick (move) ......... Forward / strafe                 Stick X ............. Turn (snap-turn by default, 45°)
  Stick click ........... unused                          Stick Y (pull down) . Crouch (toggle)
  Menu/≡ button ......... Open in-game menu (Esc)          Stick click ......... Use
  Trigger ............... unused                           Trigger ............. Primary fire
  Grip (hold) ........... INVENTORY WHEEL                  Grip (hold) ......... WEAPON WHEEL
```

Both fire controls live on the weapon hand: trigger for primary, **B for alt
fire**. The off-hand trigger is deliberately unbound — you should never fire the
weapon you are aiming with using your other hand.

**Selector wheels.** Hold either grip to bring up a radial wheel in front of that
hand; point with the wheel hand and release the grip to take the highlighted
slot, or release with nothing highlighted to cancel. Time slows to 0.22× while a
wheel is open, and no other input reaches the game. The wheel hand blips each
time the highlight moves to a new slot.

```
        DOMINANT grip -> WEAPON WHEEL          OFF-HAND grip -> INVENTORY WHEEL
        ────────────────────────────           ─────────────────────────────────
        Phaser, Compression Rifle,             Tricorder, Voyager Hypo,
        IMOD, Scavenger Rifle, Stasis,         Blue Hypo, Red Hypo,
        Grenade Launcher, Tetrion,             Quick Save, Quick Load,
        Quantum Burst, Dreadnought,            Mission Objectives
        Proton Gun
```

A weapon you own but have no ammo for stays on the ring, greyed out and not
selectable, rather than disappearing.

## Quick reference table

| Hand | Input | Action | Source |
|---|---|---|---|
| Right (dominant) | Trigger | Primary fire | `VrInputCommon.c:793` |
| Right (dominant) | B button | **Alt fire** | `VrInputCommon.c:812` |
| Right (dominant) | Grip, hold | **Weapon wheel** — release on a slot to equip it | `VrInputCommon.c:656` |
| Right (dominant) | A button | Jump (or skip cutscene, if `vr.cin_camera`) | `VrInputCommon.c:799` |
| Right (dominant) | Stick X | Turn (snap by default, `vr_turn_mode`/`vr_turn_angle`) | `VrInputCommon.c:740` |
| Right (dominant) | Stick Y, pull down | **Crouch** — toggle; jumping cancels it | `VrInputCommon.c:826` |
| Right (dominant) | Stick click | **Use** | `VrInputCommon.c:820` |
| Left (off-hand) | Trigger | unused | — |
| Left (off-hand) | Grip, hold | **Inventory wheel** — items + quick save/load/objectives | `VrInputCommon.c:656` |
| Left (off-hand) | Stick | Movement (forward/strafe) | `VrInputCommon.c:707` |
| Left (off-hand) | Stick click | unused | — |
| Left (off-hand) | X button, hold | Mission-objectives info screen | `VrInputCommon.c:858` |
| Left (off-hand) | Y button | Open in-game menu (Esc) | `VrInputCommon.c:868` |
| Either hand | Menu/≡ button | Open in-game menu (Esc) | `VrInputCommon.c:868` |
| Both hands | Both triggers + both grips, hold 3s | Full loadout — all weapons, max ammo, full health/armour (`vr_cheat_chord`, on by default) | `VrInputCommon.c:613` |

## Movement orientation

`vr_movement_orientation` (archived, default `0`):

* `0` — **head-relative.** You walk where you look. The engine applies
  `forwardmove`/`rightmove` relative to `cl.viewangles[YAW]`, which in VR is the
  HMD yaw.
* `1` — **controller-relative.** The move-stick vector is pre-rotated by
  (off-hand yaw − HMD yaw), so the off-hand controller steers movement and your
  head is free to look around independently. Point the off-hand where you want
  to go.

## Haptics

Controller rumble is driven by `vr_haptic_intensity` (archived, default `1.0`;
set `0` to disable all rumble).

The plumbing follows JKXR. The cgame calls `vr->HapticEvent(...)` with a name
describing how something should **feel**; the engine owns the timings, so the
engine stays game-agnostic:

| Layer | File | Role |
|---|---|---|
| Call sites | `Elite-Force-VR/cgame/cg_weapons.cpp`, `cg_event.cpp` | Decide *what* happened (this weapon fired, we took a hit) |
| Event → timing | `code/vr/VrInputCommon.c` `VR_HapticEvent` | Map the effect name to a duration + hand + amplitude |
| OpenXR output | `code/vr/*/OpenXrInput.c` `TBXR_Vibrate` / `TBXR_ProcessHaptics` | Arm a per-hand effect; apply and count it down each frame |

Effect names and their timings:

| Event | Duration | Hand | Used by |
|---|---|---|---|
| `fire_beam` | 50 ms, re-armed | weapon | Phaser, Dreadnought primary (continuous beams) |
| `fire_light` | 60 ms | weapon | Tetrion, Scavenger primary |
| `fire_medium` | 110 ms | weapon | Compression Rifle, IMOD, Stasis, Proton primary |
| `fire_heavy` | 260 ms | weapon | Grenade Launcher, Quantum primary, heavy alts |
| `fire_charged` | 400 ms | weapon | Compression Rifle alt (sniper), Quantum alt |
| `weapon_switch` | 250 ms | weapon | `EV_CHANGE_WEAPON` |
| `pickup_item` | 100 ms | both | `EV_ITEM_PICKUP` |
| `damage` | 200 ms | both | `EV_PAIN`, amplitude scaled by how hard the hit was |
| `selector_icon` | 50 ms | wheel hand | Selector-wheel highlight moving to a new slot |
| `use_button` | 50 ms | named | Hypos / tricorder |

`TBXR_Vibrate` lets a running effect finish rather than restarting it, so a held
trigger produces a steady pulse rather than a constant re-trigger. Only the
local player's own weapon rumbles — nearby NPC fire does not.

## Notes

- **Selector wheels** (`vr_wheels 1`, on by default). The dominant grip opens the
  weapon wheel, the off-hand grip the inventory wheel; point with the wheel hand
  and release to take the highlighted slot, or release with nothing highlighted
  to cancel. Slots show the weapon's own model, auto-sized so every weapon reads
  at the same size (`vr_wheel_modelscale` multiplies it) and turning slowly so
  you can read the silhouette from every angle. The wheel hangs in front of the
  wheel hand, as in JKXR.
  Time slows to `vr_wheel_timescale` (0.22) while a wheel is up, and the engine
  hard-clears the usercmd (`VR_InputSuppressed`) so nothing — stick, 6DoF lean or
  a held key — can move, crouch or fire you while you choose. Releasing on a
  **hypo** equips *and* fires it — they are one-shot heals; the tricorder is only
  equipped, since it's a hold-and-scan tool. Ported from JKXR
  (`codeJK2/cgame/cg_weapons.cpp CG_DrawItemSelector`); the cgame half lives in
  `Elite-Force-VR/cgame/cg_weapons.cpp`. The Quick Save / Quick Load icons come
  from JKXR (`z_vr_assets_base/gfx/`), shipped in `z_vr_assets_base.pk3`.
- **Crouch** is on the *turn* stick's Y axis, not a stick click, because that
  axis is otherwise unused (the move stick owns its own Y). It engages past
  −0.7 and releases above −0.5, so a diagonal turn cannot chatter the toggle.
  With `vr_switch_sticks 1` it follows the turn stick to the other hand.
- **Smooth turn** (`vr_turn_mode 1`) is scaled by real elapsed time, so the turn
  rate is identical on a 72 Hz standalone headset and a 120 Hz PC one. The rate
  matches the pre-fix per-frame step at 90 Hz, so that refresh rate's feel is
  unchanged. Snap turn (`vr_turn_mode 0`, the default) is unaffected.
- **Left-handed mode** (`vr_control_scheme 10`) mirrors this exactly: the left
  controller becomes the weapon/dominant hand (trigger = fire, X = jump,
  Y = alt fire, stick click = use, grip = weapon wheel) and the right becomes the
  off-hand (grip = inventory wheel, A = mission info, B = menu).
- **Menu button**: on Meta/Oculus Touch controllers only the left has a
  physical menu button; on Vive/Pico/the Khronos-simple fallback profile it's
  bound on both hands. Either way it opens the same in-game menu as the
  right-hand-default's Y button.
- **HTC Vive wands** have no thumbstick and no A/B/X/Y face buttons bound in
  the OpenXR action set (`OpenXrInput.c:341-368`) — on Vive hardware,
  movement, turning, jump, use, and crouch have no input source. Only trigger
  (primary fire), grip-click, and the menu button work. The wheels do open on
  the grip-click. This is a real gap, not an intentional Vive-specific scheme.
- Free inputs, if something new needs a home: the **off-hand trigger** and the
  **off-hand thumbstick click**.
- The old **HUD/gun toggle** on the off-hand stick click is gone. Set
  `cg_draw2D` / `cg_drawGun` from the console instead.
- **Full-loadout chord**: holding both triggers *and* both grips together for
  3 seconds runs the game's own `give all` cheat (all weapons, ammo to max,
  full health and armour). VR has no console, so this is the in-headset route
  to it. `spmap` starts a level with `sv_cheats 0`, so the chord enables cheats
  first; they stay enabled until the next map load. Fire and crouch are
  suppressed while the four inputs are held together, so the chord doesn't
  empty a clip. Set `vr_cheat_chord 0` to disable it.
- A separate developer-only **weapon alignment mode** (`vr_align_weapons 1`)
  temporarily repurposes the dominant hand's stick/buttons to nudge the
  weapon model's position/rotation. It's not part of normal play and is
  already documented in [`README.md`](README.md#running) and
  [`BUILD.md`](BUILD.md).
