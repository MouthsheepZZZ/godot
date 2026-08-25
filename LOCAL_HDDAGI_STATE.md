# Local Dynamic HDDAGI V0 — Current State

## Current Task

**Task 3 — GI Mode + Dynamic Geometry** (parked)

Task 2 is complete, including the later volume-centered sampling confirmation. **Do not start Task 3 until the human explicitly asks.**

---

## Last Completed Commit

```text
local-hddagi: add minimal static local runtime
```

Uncommitted follow-up on the working tree:

```text
gi.glsl: Local sampling skips camera-centered cascade-edge fade
LOCAL_HDDAGI_STATE.md (this file)
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
- One active Local slot reuses existing HDDAGI resources and passes.
- Local is forced to one cascade with no camera-centered scrolling.
- Local voxelization uses Local-domain view/transform and descendant contributors only.
- Local descendant lights are used first (world lights are still Task 4).
- Local root translation/rotation updates transform only and does not bump `data_version` or trigger Local voxel rebuild.
- Final shading samples Local inside bounds and leaves Global pixels unchanged outside bounds.
- Local final sampling is **volume-centered**. The Global camera-centered cascade-edge fade must not run in `local_mode`. Human confirmed: close-up and far views both keep Local GI; no CameraFade.

---

## Current Implementation State

```text
LocalDynamicGI3D
  → collect descendant GeometryInstance3D / Light3D
  → RS::local_dynamic_gi_* RID (bounds / transform / instances / data_version)
  → RendererSceneCull selects one active Local
  → RB_SCOPE_LOCAL_HDDAGI (independent Ref<HDDAGI>, sampling_ubo)
  → voxelize Local contributors in Local space (1 cascade, no scroll)
  → Direct / Probe / filter reuse existing HDDAGI
  → process_gi(local_mode): sample Local inside bounds, keep Global outside
  → local_mode skips camera-centered cascade-edge fade (volume-centered)
```

Global `hddagi_ubo` and the per-view Global solver are unchanged. `hddagi_preprocess` / HDDA / direct-light / probe filter / voxel format were not rewritten.

The far-away “GI disappears” look was not missing Local coverage from having one cascade. Local’s cascade is fitted to Local bounds and does not follow the camera. `hddagi_process` still used the Global camera-space edge fade (`cam_pos * to_probe`). With `max_cascades == 1` that fade has no coarser cascade to blend into, so GI went to zero when the camera backed up. `local_mode` now forces that blend to 0.

---

## Dedicated Validation Project

Status: **CREATED** (local-only)

```text
_local_hddagi_validation/
  project.godot
  scenes/00_global_baseline.tscn
  scenes/01_local_static.tscn
  scenes/02_local_root_motion.tscn
  scripts/test_overlay.gd
  scripts/compare_room_cameras.gd
  scripts/local_root_motion.gd
  scripts/task1_plumbing_test.gd
  scripts/task2_runtime_test.gd
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

`01_local_static.tscn`: two closed Cornell boxes. `LocalRoom` is Local HDDAGI. `GlobalRoom` at x=9 is Global only. Tab switches in-room cameras.

`02_local_root_motion.tscn`: Local room motion case. When this scene is touched again, add the same Global-only closed Cornell + view switcher so motion can be judged against a static Global control.

---

## Automated Validation

```text
Editor build: PASS
  Incremental Windows editor scons succeeded after closing the locked editor exe.
HDDAGI shader / algorithm diff: PASS
  No hddagi_*.glsl or HDDA/direct/filter algorithm rewrites.
  gi.glsl adds SceneData local_mode/bounds, bounds early-out, and local_mode
  skip of camera-centered cascade-edge fade.
Plumbing tests: PASS
  _local_hddagi_validation/scripts/task1_plumbing_test.gd
Runtime tests: PASS
  _local_hddagi_validation/scripts/task2_runtime_test.gd
Headless scene load: PASS
  00 / 01 / 02 exit 0.
```

---

## Human Visual Validation

Current status: **PASS** (Task 2, including volume-centered recheck)

```text
00_global_baseline.tscn: PASS (Task 0)
01_local_static.tscn editor bounds: PASS (Task 1)
01_local_static.tscn Local bounce / bleeding: PASS (Task 2)
02_local_root_motion.tscn root translation / rotation: PASS (Task 2)
01 Local volume-centered, no CameraFade at distance: PASS
```

Human notes:

- A closed Cornell with the camera inside is Local-only; that is expected for that room. Global comparison needs a second closed Global-only room plus a view switch, not an open wall and not a hidden +X slab.
- Opening the front wall makes bounce too weak. Keep rooms closed.
- Far-away Local GI loss was CameraFade on a volume-centered cascade, not missing cascade coverage. After the `local_mode` fade skip, far and near Local GI both remain.

---

## Known Problems

None reproduced as open Local HDDAGI defects.

---

## Explicitly Not Implemented Yet

```text
GI mode dirty rebuild
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

**Wait.** Do not start Task 3 in this session or after compaction unless the human explicitly asks to continue.

When the human later asks to continue, Task 3 is **GI Mode + Dynamic Geometry** only:

```text
DISABLED → receive only
STATIC   → Local contributor
DYNAMIC  → Local contributor + Local-transform change detection
```

V0 dynamic behavior:

```text
any GI_MODE_DYNAMIC Local transform change
→ Local DIRTY_ALL
→ full Local voxel rebuild
```

New Task 3+ visual scenes must follow the Global-vs-Local comparison rule above. Do not implement dirty regions, world-light transfer, two slots, or blend shading yet.

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
