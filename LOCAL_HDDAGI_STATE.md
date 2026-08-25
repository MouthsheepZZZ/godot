# Local Dynamic HDDAGI V0 — Current State

## Current Task

**Task 4 — Global → Local Lighting** (parked)

Task 3 is complete, including human visual PASS on GI mode and dynamic geometry. **Do not start Task 4 until the human explicitly asks.**

---

## Last Completed Commit

```text
local-hddagi: support GI mode and dynamic contributors
```

Baseline before Local V0:

```text
ab154bfd170dce1047ec4b2842c0fc1be31a90ff
HDDAGI: increase LIGHTPROBE_OCT_SIZE to 6, remove redundant barrier, pack occlusion shared memory as half2x16
```

---

## Completed

- Task 0 Global baseline remains accepted.
- Task 1 Local scene plumbing remains accepted.
- Task 2 single static Local loop remains accepted.
- Task 3 GI mode + dynamic geometry remains accepted.
- One active Local slot reuses existing HDDAGI resources and passes.
- Local is forced to one cascade with no camera-centered scrolling.
- Local voxelization uses Local-domain view/transform and descendant contributors only.
- `GI_MODE_DISABLED` receives Local GI and is excluded from Local voxel contributors.
- `GI_MODE_STATIC` contributes to Local voxelization. Local-space motion inside unchanged bounds does not rebuild.
- `GI_MODE_DYNAMIC` contributes to Local voxelization. A Local-space transform change requests `DIRTY_ALL` and a full Local voxel rebuild.
- Local root translation/rotation updates transform only and does not bump `data_version` or trigger Local voxel rebuild, even when DYNAMIC descendants exist.
- Descendant light motion refreshes Direct lighting only and does not bump `data_version`.
- Local descendant lights are used first (world lights are still Task 4).
- Final shading samples Local inside bounds and leaves Global pixels unchanged outside bounds.
- Local final sampling is **volume-centered**. The Global camera-centered cascade-edge fade must not run in `local_mode`.

---

## Current Implementation State

```text
LocalDynamicGI3D
  → collect descendant GeometryInstance3D / Light3D
  → DISABLED = receive only; STATIC/DYNAMIC = Local voxel contributors
  → DYNAMIC Local-space transform change → request_rebuild → data_version++
  → RS::local_dynamic_gi_* RID (bounds / transform / instances / data_version)
  → RendererSceneCull selects one active Local
  → RB_SCOPE_LOCAL_HDDAGI (independent Ref<HDDAGI>, sampling_ubo)
  → voxelize Local contributors in Local space (1 cascade, no scroll)
  → data_version change → Local DIRTY_ALL → full Local voxel rebuild
  → Direct / Probe / filter reuse existing HDDAGI
  → process_gi(local_mode): sample Local inside bounds, keep Global outside
  → local_mode skips camera-centered cascade-edge fade (volume-centered)
```

Runtime runs `NOTIFICATION_INTERNAL_PROCESS` so DYNAMIC descendant motion is detected without a script calling `update_local_data()`.

Global `hddagi_ubo` and the per-view Global solver are unchanged. `hddagi_preprocess` / HDDA / direct-light / probe filter / voxel format were not rewritten.

---

## Dedicated Validation Project

Status: **CREATED** (local-only)

```text
_local_hddagi_validation/
  project.godot
  scenes/00_global_baseline.tscn
  scenes/01_local_static.tscn
  scenes/02_local_root_motion.tscn
  scenes/03_gi_mode_dynamic.tscn
  scripts/test_overlay.gd
  scripts/compare_room_cameras.gd
  scripts/local_root_motion.gd
  scripts/gi_mode_dynamic_motion.gd
  scripts/task1_plumbing_test.gd
  scripts/task2_runtime_test.gd
  scripts/task3_gi_mode_test.gd
```

Excluded by `.git/info/exclude`, not `.gitignore`.

Editor to use:

```text
F:\godot\bin\godot.windows.editor.x86_64.exe
```

### Scene design rule (mandatory for later tasks)

Every new visual test scene must be designed so the human can **compare Global and Local in the same case**, not only look at a Local interior.

Required, unless a later task explicitly forbids it:

- Keep at least one closed Global-only Cornell (or equivalent bounce box) that is **not** a `LocalDynamicGI3D` descendant.
- Keep at least one closed Local Cornell under `LocalDynamicGI3D`.
- Closed rooms stay closed so bounce/bleeding stay readable. Do not delete the front wall just to frame both rooms.
- If both interiors cannot be on screen at once, use in-room cameras and Tab (or an equivalent switcher). Overlay must say which view is active.
- Do not use a tiny exterior slab, an off-camera prop, or “the room is Local so Global cannot be checked” as the Global control.
- `00_global_baseline.tscn` remains the Global-only reference.
- Give in-room cameras WASD fly + right-mouse look when the human needs to inspect bounce/contribution.

`01_local_static.tscn`: two closed Cornell boxes. `LocalRoom` is Local HDDAGI. `GlobalRoom` at x=9 is Global only. Tab switches in-room cameras.

`02_local_root_motion.tscn`: Local room motion case. When this scene is touched again, add the same Global-only closed Cornell + view switcher so motion can be judged against a static Global control.

`03_gi_mode_dynamic.tscn`: two closed Cornell boxes. Local dark-blue center box is `GI_MODE_DYNAMIC` and slides through the room. Back-left white block is `GI_MODE_STATIC`. Magenta floor cube is `GI_MODE_DISABLED`. Global center box stays still. Tab switches rooms. WASD + RMB look.

---

## Automated Validation

```text
Editor build: PASS
  Incremental Windows editor scons succeeded.
HDDAGI shader / algorithm diff: PASS
  No hddagi_*.glsl or HDDA/direct/filter algorithm rewrites this task.
Plumbing / Task 2 runtime: PASS
Task 3 GI-mode tests: PASS
  _local_hddagi_validation/scripts/task3_gi_mode_test.gd
  C++ doctest exists in tests/scene/test_local_dynamic_gi_3d.cpp
  but this editor binary was compiled without tests=yes.
Headless scene load: PASS
  01 / 03 exit 0.
```

---

## Human Visual Validation

Current status: **PASS** (Task 3)

```text
00_global_baseline.tscn: PASS (Task 0)
01_local_static.tscn editor bounds: PASS (Task 1)
01_local_static.tscn Local bounce / bleeding: PASS (Task 2)
02_local_root_motion.tscn root translation / rotation: PASS (Task 2)
01 Local volume-centered, no CameraFade at distance: PASS
03_gi_mode_dynamic.tscn GI mode / dynamic contribution: PASS
```

Human notes:

- Visual comparison needs a closed Global-only room plus Tab, not an open wall.
- DYNAMIC contribution is easiest to judge on a saturated dark-blue mover (blue bounce should follow it).
- DISABLED contribution is easiest to judge on a magenta receiver (no magenta bounce onto nearby surfaces).
- Temporary rebuild flicker while the DYNAMIC box moves is acceptable for V0.

---

## Known Problems

None reproduced as open Local HDDAGI defects.

---

## Explicitly Not Implemented Yet

```text
World lights → Local
Global indirect → Local
Local → Global verification
Two active Local slots
Blend Distance shading
```

V0 will not implement:

```text
Local dirty-region optimization
Local radiance injection into Global
Dedicated dynamic voxel algorithms
Local cascade scrolling
Multiple Local cascades
More than 2 simultaneously active Local slots
Local nesting
Priority systems
Non-uniform-scale architecture
Speculative performance optimization
Generic GI-domain refactor
```

---

## Next Action

**Wait.** Do not start Task 4 in this session or after compaction unless the human explicitly asks to continue.

When the human later asks to continue, Task 4 is **Global → Local Lighting** only:

```text
World Directional / Omni / Spot / Area → Local Direct
Reuse existing Global occupancy/HDDA visibility where possible
Local probe miss/exit → sample existing Global HDDAGI, then Sky
Frame order: Global complete → Local reads Global result
```

New Task 4+ visual scenes must follow the Global-vs-Local comparison rule above. Do not implement dirty regions, two slots, or blend shading yet.

---

## Compaction Resume Instruction

After any Codex/Cursor compaction, execute this before continuing:

```text
Read LOCAL_HDDAGI_PLAN.md and LOCAL_HDDAGI_STATE.md first.
Then inspect git status, git diff, and recent relevant commits.
Treat the repository as the source of truth.
Continue only the current unfinished task.
Do not redesign or generalize HDDAGI.
Do not perform work belonging to later tasks.
If Next Action says wait, do not start the next task until the human asks.
If the current task requires visual validation, stop after automated validation and wait for the human result.
```
