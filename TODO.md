# ioEF VR — engineering backlog

Open work for this port. (The extensionless `TODO` file is upstream ioquake3's and
just points at their wiki roadmap — unrelated to this list.)

---

## Startup self-check for the shared engine↔module struct layouts

**Why.** The engine and the SP modules (`efgame*` / `efui*`, built from the
separate Elite-Force-VR repo, by a *different compiler*) each compile their own
copy of several structs that are then passed between them raw, with no
marshalling. If the two copies disagree by so much as one field, the data is
silently misread — no compiler error, no crash, just wrong values. Three of these
were found in a single day (2026-07-22), each after a long symptom-driven hunt:

| Divergence | Symptom |
|---|---|
| `refEntityType_t` — engine used Raven's `ELITEFORCE` enum, module still had the stock Q3 one | Every `reType` except `RT_MODEL` misread: cgame `RT_SPRITE` arrived as `RT_ORIENTEDSPRITE` and drew nothing; `RT_BEAM` as `RT_ALPHAVERTPOLY`; `RT_PORTALSURFACE` as `RT_LIGHTNING` |
| `refEntity_t` — module has `vec3_t lightDir` before `radius`, engine did not | Engine read `radius` 12 bytes early (off the zeroed `lightDir`), so **every sprite the SP cgame ever submitted had radius 0 and was invisible** |
| `glconfig_t.clampToEdgeAvailable` (fixed earlier, see the comment in `code/renderercommon/tr_types.h`) | `vidWidth`/`vidHeight` and everything after read at a 4-byte offset → garbage viewport |

Each was invisible by construction and cost hours to find from the symptom end.
A mismatch should announce itself at startup instead.

**What.** At SP module init, have the module report its own `sizeof`/`offsetof`
for the shared structs and have the engine compare against its own, logging a
loud warning (or `Com_Error` in a debug build) naming the offending struct and
field on any mismatch.

Structs to cover, at minimum:

- `code/qcommon/sp_types.h` — `sp_playerState_t`, `sp_entityState_t`,
  `sp_snapshot_t`, and the `gclient_t`/`gentity_t` prefixes the bridge relies on
- `code/renderercommon/tr_types.h` — `refEntity_t`, `refEntityType_t`,
  `glconfig_t`, `refdef_t`
- `code/vr/VrClientInfo.h` — `vr_client_info_t` (already documented as needing to
  stay byte-identical, but nothing enforces it)

**Notes for whoever picks this up.**

- A compile-time `static_assert` can't span the two repos, so this has to be a
  runtime exchange. The natural channels already exist: the `GetGameAPI` table
  swap (`sv_game_sp.c`) and the cgame's `CG_INIT` args (`cl_cgame_sp.c`), which
  already carries the `VR_CGINIT_SENTINEL`.
- Checking whole-struct `sizeof` alone is not enough — `sp_playerState_t` and the
  module's `playerState_t` had *identical* sizes and offsets on x86_64 while a
  different bug was in play, and the `refEntity_t` divergence would have been
  caught by size, but `refEntityType_t` (an enum, same size) would not. Field
  offsets matter, not just totals.
- Verify on **both** ABIs. Windows x64 is LLP64 and Android arm64 is LP64; this
  port has already had two LP64-specific bugs (the ICARUS `.ibi` `*(long*)` read
  and the `RAND_MAX` assumption).
