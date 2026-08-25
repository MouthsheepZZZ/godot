# Local Dynamic HDDAGI V0 — Current State

## Current Task

**Task 2 — Single Static Local HDDAGI Loop**

Task 1 is complete (human PASS). After compaction, implement only Task 2.

---

## Last Completed Commit

```text
local-hddagi: add LocalDynamicGI3D scene plumbing
```

Baseline before Local V0:

```text
ab154bfd170dce1047ec4b2842c0fc1be31a90ff
HDDAGI: increase LIGHTPROBE_OCT_SIZE to 6, remove redundant barrier, pack occlusion shared memory as half2x16
```

---

## Completed

- Task 0 Global baseline remains accepted.
- `LocalDynamicGI3D` scene node added: create/destroy, `enabled`, `extend`, `blend_distance`.
- Descendant Geometry/Light collection and `gi_mode` classification (STATIC/DYNAMIC contribute, DISABLED receive-only).
- Automatic Local bounds: descendant contributor AABBs merged in Local space, then expanded by `extend`.
- Nested `LocalDynamicGI3D` subtrees are skipped (no nesting).
- Multiple Local nodes register as independent RenderingServer RIDs.
- Editor gizmo draws computed bounds (cyan) and Blend Distance (fainter).
- `_local_hddagi_validation/scenes/01_local_static.tscn` added for bounds inspection.

---

## Current Implementation State

Local HDDAGI still does **not** voxelize, trace, or shade. Task 1 is scene plumbing only.

```text
LocalDynamicGI3D
  → collect descendant GeometryInstance3D / Light3D
  → classify by gi_mode
  → merge contributor AABBs in Local space + Extend
  → RS::local_dynamic_gi_* RID (enabled / extend / blend / bounds / transform / scenario)
  → editor gizmo
```

No new `InstanceType`. No HDDAGI shader/algorithm changes. Shared `hddagi_ubo` and the per-View Global solver are unchanged.

---

## Dedicated Validation Project

Status: **CREATED** (local-only)

```text
_local_hddagi_validation/
  project.godot
  scenes/00_global_baseline.tscn
  scenes/01_local_static.tscn
  scripts/test_overlay.gd
  scripts/task1_plumbing_test.gd
```

Excluded by `.git/info/exclude`, not `.gitignore`.

Editor to use:

```text
F:\godot\bin\godot.windows.editor.x86_64.exe
4.7.stable.custom_build.2d9b15977
```

`01_local_static.tscn`: same Cornell box as 00, but room geometry and the ceiling Omni are children of `LocalRoom` (`LocalDynamicGI3D`). Global HDDAGI is still on; Local runtime is not.

---

## Automated Validation

```text
Editor build: PASS
  Incremental Windows editor scons succeeded.
HDDAGI shader / algorithm diff: PASS
  No hddagi_*.glsl or gi.cpp algorithm changes.
Plumbing tests: PASS
  _local_hddagi_validation/scripts/task1_plumbing_test.gd
  create/delete, AABB+Extend, gi_mode classification, nested exclusion, multi-RID registration.
Headless scene load: PASS
  00_global_baseline.tscn and 01_local_static.tscn both exit 0.
Git diff cleanliness: PASS
  Validation project remains excluded. Engine diff is Local plumbing only.
```

C++ tests exist at `tests/scene/test_local_dynamic_gi_3d.cpp` but were not compiled (`tests=yes` not built this session).

---

## Human Visual Validation

Current status: **PASS**

```text
00_global_baseline.tscn: PASS (Task 0)
01_local_static.tscn editor bounds: PASS
```

Human confirmed the editor/debug bounds follow `LocalRoom` correctly.

---

## Known Problems

None reproduced as Local HDDAGI defects.

---

## Explicitly Not Implemented Yet

```text
Local voxelization
Local Direct pass
Local Probe integration
Local final shading
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

After compaction, start **Task 2 — Single Static Local HDDAGI Loop** only:

- One active Local slot
- Reuse existing HDDAGI resources/passes
- Force one cascade, no camera-centered scrolling
- Voxelize only Local contributors
- Local root motion must not rebuild voxels

Do not implement GI-mode dirty rebuild, world-light transfer, two slots, or blend shading yet.

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
