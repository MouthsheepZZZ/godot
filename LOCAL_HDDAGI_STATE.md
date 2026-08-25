# Local Dynamic HDDAGI V0 — Current State

## Current Task

**Task 3 — GI Mode + Dynamic Geometry**

Task 2 is complete (human PASS). After compaction, implement only Task 3.

---

## Last Completed Commit

```text
local-hddagi: add minimal static local runtime
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
```

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
  scripts/test_overlay.gd
  scripts/local_root_motion.gd
  scripts/task1_plumbing_test.gd
  scripts/task2_runtime_test.gd
```

Excluded by `.git/info/exclude`, not `.gitignore`.

Editor to use:

```text
F:\godot\bin\godot.windows.editor.x86_64.exe
4.7.stable.custom_build.e705be2f1
```

`01_local_static.tscn`: Cornell room under `LocalRoom`, plus an exterior slab/light at +X that is not a Local child.

`02_local_root_motion.tscn`: same Local room, with `local_root_motion.gd` translating and yawing the Local root.

---

## Automated Validation

```text
Editor build: PASS
  Incremental Windows editor scons succeeded.
HDDAGI shader / algorithm diff: PASS
  No hddagi_*.glsl or HDDA/direct/filter algorithm rewrites.
  gi.glsl only adds SceneData local_mode/bounds and an early-out.
Plumbing tests: PASS
  _local_hddagi_validation/scripts/task1_plumbing_test.gd
Runtime tests: PASS
  _local_hddagi_validation/scripts/task2_runtime_test.gd
  create/delete/enable/disable, root motion does not bump data_version,
  bounds/extend change does bump data_version.
Headless scene load: PASS
  00_global_baseline.tscn, 01_local_static.tscn, 02_local_root_motion.tscn exit 0.
Git diff cleanliness: PASS
  Validation project remains excluded. Engine diff is Local runtime + SceneData hook.
```

Headless scripts do not execute the GPU voxelization path, so `voxelization_count` stays 0 there. The useful runtime proof is that root motion does not bump `data_version`.

---

## Human Visual Validation

Current status: **PASS**

```text
00_global_baseline.tscn: PASS (Task 0)
01_local_static.tscn editor bounds: PASS (Task 1)
01_local_static.tscn Local bounce / bleeding: PASS (Task 2)
02_local_root_motion.tscn root translation / rotation: PASS (Task 2)
```

Human note for `01_local_static.tscn`: the Cornell room is fully enclosed by the Local volume, so the interior is Local-only as expected. Global comparison is the +X exterior slab (not a Local child) or `00_global_baseline.tscn`.

---

## Known Problems

None reproduced as Local HDDAGI defects.

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

After compaction, start **Task 3 — GI Mode + Dynamic Geometry** only:

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

Do not implement dirty regions, world-light transfer, two slots, or blend shading yet.

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
If the current task requires visual validation, stop after automated validation and wait for the human result.
```
