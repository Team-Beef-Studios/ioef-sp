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
  X button (hold) ...... Mission-objectives info screen   B button ............ Use
  Stick (move) ......... Forward / strafe                 Stick X ............. Turn (snap-turn by default, 45°)
  Stick click ........... Crouch (toggle)                 Stick click ......... unused
  Menu/≡ button ......... Open in-game menu (Esc)          Trigger ............. Primary fire
  Trigger ............... Alt fire                         Grip (hold) ......... WEAPON WHEEL
  Grip (hold) ........... INVENTORY WHEEL                  Menu button ......... n/a on Touch (no physical button)
```

**Selector wheels.** Hold either grip to bring up a radial wheel in front of that
hand; point with the wheel hand and release the grip to take the highlighted
slot, or release with nothing highlighted to cancel. Time slows to 0.22× while a
wheel is open, and no other input reaches the game.

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
| Right (dominant) | Trigger | Primary fire | `VrInputCommon.c:751-756` |
| Right (dominant) | Grip, hold | **Weapon wheel** — release on a slot to equip it | `VrInputCommon.c:644-693` |
| Right (dominant) | A button | Jump (or skip cutscene, if `vr.cin_camera`) | `VrInputCommon.c:765-776` |
| Right (dominant) | B button | Use | `VrInputCommon.c:778-782` |
| Right (dominant) | Stick X | Turn (snap by default, `vr_turn_mode`/`vr_turn_angle`) | `VrInputCommon.c:710-749` |
| Right (dominant) | Stick Y | unused (weapon cycling moved to the wheel) | — |
| Right (dominant) | Stick click | unused | — |
| Left (off-hand) | Trigger | Alt fire | `VrInputCommon.c:758-763` |
| Left (off-hand) | Grip, hold | **Inventory wheel** — items + quick save/load/objectives | `VrInputCommon.c:644-693` |
| Left (off-hand) | Stick click | Crouch — **toggle**; jumping cancels it | `VrInputCommon.c:784-806` |
| Left (off-hand) | X button, hold | Mission-objectives info screen | `VrInputCommon.c:808-816` |
| Left (off-hand) | Y button | Open in-game menu (Esc) | `VrInputCommon.c:818-837` |
| Left (off-hand) | Stick | Movement (forward/strafe) | `VrInputCommon.c:695-708` |
| Either hand | Menu/≡ button | Open in-game menu (Esc) | `VrInputCommon.c:828-837` |
| Both hands | Both triggers + both grips, hold 3s | Full loadout — all weapons, max ammo, full health/armour (`vr_cheat_chord`, on by default) | `VrInputCommon.c:601-642` |

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
- **Left-handed mode** (`vr_control_scheme 10`) mirrors this exactly: the left
  controller becomes the weapon/dominant hand (trigger = fire, X = jump,
  Y = use, grip = weapon wheel) and the right becomes the off-hand (trigger =
  alt-fire, grip = inventory wheel, stick click = crouch, A = mission info,
  B = menu).
- **Menu button**: on Meta/Oculus Touch controllers only the left has a
  physical menu button; on Vive/Pico/the Khronos-simple fallback profile it's
  bound on both hands. Either way it opens the same in-game menu as the
  right-hand-default's Y button.
- **HTC Vive wands** have no thumbstick and no A/B/X/Y face buttons bound in
  the OpenXR action set (`OpenXrInput.c:341-368`) — on Vive hardware,
  movement, turning, weapon-switch, jump, use, and crouch have no input
  source. Only trigger (fire/alt-fire), grip-click, and the menu button work.
  The wheels do open on the grip-click. This is a real gap, not an intentional
  Vive-specific scheme.
- The right thumbstick **click** has no bound action in normal gameplay.
- The old **HUD/gun toggle** on the off-hand stick click is gone — that click is
  now crouch. Set `cg_draw2D` / `cg_drawGun` from the console instead.
- **Full-loadout chord**: holding both triggers *and* both grips together for
  3 seconds runs the game's own `give all` cheat (all weapons, ammo to max,
  full health and armour). VR has no console, so this is the in-headset route
  to it. `spmap` starts a level with `sv_cheats 0`, so the chord enables cheats
  first — they stay enabled until the next map load. Fire and crouch are
  suppressed while the four inputs are held together, so the chord doesn't
  empty a clip. Set `vr_cheat_chord 0` to disable it.
- A separate developer-only **weapon alignment mode** (`vr_align_weapons 1`)
  temporarily repurposes the dominant hand's stick/buttons to nudge the
  weapon model's position/rotation. It's not part of normal play and is
  already documented in [`README.md`](README.md#running) and
  [`BUILD.md`](BUILD.md).
