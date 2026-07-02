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
  Stick click ........... HUD/gun toggle (OFF by default,  Stick Y (flick) ..... Next / prev weapon
                           needs vr_hud_toggle 1)          Stick click ......... unused
  Menu/≡ button ......... Open in-game menu (Esc)          Trigger ............. Primary fire
  Trigger ............... Alt fire                         Grip ................ (holds weapon model — no action)
  Grip/squeeze .......... Crouch (only if not jumping)     Menu button ......... n/a on Touch (no physical button)
```

## Quick reference table

| Hand | Input | Action | Source |
|---|---|---|---|
| Right (dominant) | Trigger | Primary fire | `VrInputCommon.c:662-664` |
| Right (dominant) | A button | Jump (or skip cutscene, if `vr.cin_camera`) | `VrInputCommon.c:679-685` |
| Right (dominant) | B button | Use | `VrInputCommon.c:688-690` |
| Right (dominant) | Stick X | Turn (snap by default, `vr_turn_mode`/`vr_turn_angle`) | `VrInputCommon.c:597-636` |
| Right (dominant) | Stick Y flick | Next / prev weapon | `VrInputCommon.c:638-658` |
| Right (dominant) | Stick click | unused | — |
| Left (off-hand) | Trigger | Alt fire | `VrInputCommon.c:669-672` |
| Left (off-hand) | Grip/squeeze | Crouch (if not already jumping) | `VrInputCommon.c:695-698` |
| Left (off-hand) | X button, hold | Mission-objectives info screen | `VrInputCommon.c:700-708` |
| Left (off-hand) | Y button | Open in-game menu (Esc) | `VrInputCommon.c:710-729` |
| Left (off-hand) | Stick | Movement (forward/strafe) | `VrInputCommon.c:582-595` |
| Left (off-hand) | Stick click | HUD/gun-model toggle — only if `vr_hud_toggle 1` (off by default) | `VrInputCommon.c:731-752` |
| Either hand | Menu/≡ button | Open in-game menu (Esc) | `VrInputCommon.c:720-729` |

## Notes

- **Left-handed mode** (`vr_control_scheme 10`) mirrors this exactly: the left
  controller becomes the weapon/dominant hand (trigger = fire, X = jump,
  Y = use) and the right becomes the off-hand (trigger = alt-fire,
  grip = crouch, A = mission info, B = menu).
- **Menu button**: on Meta/Oculus Touch controllers only the left has a
  physical menu button; on Vive/Pico/the Khronos-simple fallback profile it's
  bound on both hands. Either way it opens the same in-game menu as the
  right-hand-default's Y button.
- **HTC Vive wands** have no thumbstick and no A/B/X/Y face buttons bound in
  the OpenXR action set (`OpenXrInput.c:341-368`) — on Vive hardware,
  movement, turning, weapon-switch, jump, use, and crouch have no input
  source. Only trigger (fire/alt-fire), grip-click, and the menu button work.
  This is a real gap, not an intentional Vive-specific scheme.
- The right thumbstick **click** has no bound action in normal gameplay.
- A separate developer-only **weapon alignment mode** (`vr_align_weapons 1`)
  temporarily repurposes the dominant hand's stick/buttons to nudge the
  weapon model's position/rotation. It's not part of normal play and is
  already documented in [`README.md`](README.md#running) and
  [`BUILD.md`](BUILD.md).
