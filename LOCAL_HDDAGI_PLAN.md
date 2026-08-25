# Local Dynamic HDDAGI V0 — Development Plan

## 1. Goal

Implement the smallest possible Local Dynamic HDDAGI patch on top of the existing HDDAGI branch.

The prototype must prioritize:

1. Minimal diff against upstream HDDAGI.
2. Maximum reuse of the existing HDDAGI algorithm and render pipeline.
3. Easy rebase when the HDDAGI author or Godot upstream changes.
4. Immediate visible results before any performance work.
5. Correct Local ↔ Global behavior first; performance is explicitly out of scope.

The intended architecture is:

```text
Global HDDAGI                         LocalDynamicGI3D
World-space solver                    Local-space adapter
      │                                      │
      ├──────── existing HDDAGI ─────────────┤
      │   voxelize / occupancy / direct      │
      │   HDDA / probes / cache / filter     │
      └───────────────────────────────────────┘
```

Local HDDAGI is a thin adapter around the existing HDDAGI implementation, not a new GI architecture.

---

## 2. Hard Constraints

These constraints apply to every task and every AI session.

### Must do

- Reuse the existing HDDAGI voxelization, occupancy, direct lighting, HDDA tracing, probe integration, temporal history, filtering, and data formats.
- Support multiple `LocalDynamicGI3D` nodes in the scene.
- Support at most **2 active visible Local HDDAGI slots per View**.
- The 2 active Local slots update in the same frame and at the same frequency.
- Local root translation/rotation must not invalidate Local voxel/probe history.
- Local bounds are automatically derived from descendant geometry AABB + `Extend`/margin.
- Local ownership is hierarchy-based, not bounds-based.
- Reuse Godot `GeometryInstance3D.gi_mode` semantics.
- Support Local static geometry, Local dynamic geometry, and receive-only geometry.
- World lights must affect Local GI.
- Global indirect GI must enter Local GI.
- Local geometry/lights must continue to affect Global GI.
- Final shading must support Global + Local0 + Local1 and blend near Local bounds.

### Must not do in V0

- Do not redesign HDDAGI into a generic GI-domain framework.
- Do not refactor core HDDAGI algorithms merely for cleanliness.
- Do not change HDDA mathematics.
- Do not change voxel formats.
- Do not change probe temporal/filter algorithms.
- Do not implement Local dirty-region optimization.
- Do not implement Local radiance injection into Global.
- Do not implement dedicated dynamic-geometry voxel algorithms.
- Do not implement Local cascade scrolling.
- Do not implement multiple cascades for Local GI.
- Do not implement more than 2 simultaneously active Local slots.
- Do not implement Local nesting or priority systems.
- Do not add fallback/compatibility architecture not required by the current task.
- Do not perform speculative performance optimization.
- Do not touch unrelated renderer code.

Prefer a small explicit patch over a generalized abstraction.

---

## 3. V0 User-Facing Model

### Node

```text
LocalDynamicGI3D
├── GeometryInstance3D descendants
├── Light3D descendants
└── other scene nodes
```

### Suggested V0 properties

```text
Enabled
Extend / Bounds Margin: Vector3
Blend Distance: float

HDDAGI quality/update properties matching the existing Global HDDAGI where practical
```

No manual `Size` property.

### Bounds

```text
descendant contributor AABBs
        ↓ merge in Local space
        ↓ expand by Extend
fixed_local_bounds
```

The bounds define the Local voxel/probe calculation domain and blending region.
They do **not** define ownership.

### Ownership

A geometry/light belongs to the Local GI input set because it is a descendant of `LocalDynamicGI3D`.

---

## 4. GI Mode Semantics

Reuse Godot `GeometryInstance3D.gi_mode`.

```text
GI_MODE_DISABLED
- Receives Local GI when shaded inside an active Local domain.
- Does not contribute geometry to Local voxelization.

GI_MODE_STATIC
- Receives Local GI.
- Contributes geometry to Local voxelization.
- Local root world motion does not make it dirty.

GI_MODE_DYNAMIC
- Receives Local GI.
- Contributes geometry to Local voxelization.
- If its transform relative to LocalDynamicGI3D changes, mark the entire Local GI DIRTY_ALL in V0.
```

Dynamic lights do not trigger geometry voxelization. Their light data is simply refreshed for the Direct pass.

---

## 5. Minimal Data/Execution Model

Do not generalize the renderer first.

Conceptually keep:

```text
Global HDDAGI
Local Slot 0
Local Slot 1
```

The scene may register any number of Local nodes, but the View selects at most two active ones.

The compute sequence may remain explicitly ordered:

```text
Global
  geometry/update
  direct
  probes

Local Slot 0
  geometry/update if dirty
  direct
  probes

Local Slot 1
  geometry/update if dirty
  direct
  probes

Final forward shading
  Global + Local0 + Local1
```

Prefer reuse of existing temporary compute state where sequential execution makes it safe. Avoid broad HDDAGI resource ownership refactors unless compilation/correctness proves they are unavoidable.

---

## 6. Global ↔ Local Rules

### Global → Local: direct lights

World lights affecting a Local domain are transformed to Local space and passed through the existing HDDAGI Direct Light pipeline.

Supported types must match current HDDAGI support:

```text
Directional
Omni / Point
Spot
Area
```

Do not create a new lighting model.

### Global → Local: indirect GI

Local Probe tracing keeps the existing logic:

```text
Local Probe ray
    ↓
Local HDDA
    ├─ hit Local voxel → read Local VoxelLight
    └─ miss/exit Local domain
           ↓
       sample Global HDDAGI directional radiance
           ↓
       Global unavailable → existing Sky fallback
```

Reuse Global HDDAGI probe-atlas sampling rather than tracing a second Global HDDA ray.

### Local → Global

V0 adds no dedicated transfer algorithm.

Local descendant geometry and lights remain normal World3D renderer instances and therefore continue to participate in the existing Global HDDAGI path.

```text
Local Geometry
├─ Local voxelization
└─ existing Global voxelization

Local Light
├─ Local Direct pass
└─ existing Global Direct pass
```

Duplicate computation is acceptable in V0.

---

## 7. Active Local Selection

Any number of Local nodes may be registered.

For each View:

1. Ignore disabled Local nodes.
2. Determine visibility using their world-space computed bounds.
3. Prefer a Local domain containing the camera.
4. Otherwise sort by camera-to-bounds distance.
5. Select the first two.

```text
MAX_ACTIVE_LOCAL_HDDAGI = 2
```

Inactive Local runtime data may remain allocated, but no Direct/Probe/Voxel update is required while inactive.

If dynamic descendants changed while inactive, set `needs_full_rebuild` and rebuild when activated again.

---

## 8. Final Shading

Final forward shading has fixed access to:

```text
Global HDDAGI
Local Slot 0
Local Slot 1
```

At a shaded world position:

```text
outside Local bounds
→ Global GI

inside Local bounds
→ Local GI

near Local boundary
→ Local / Global blend using Blend Distance
```

If two Local domains overlap, V0 may normalize the two Local weights. No priority system is required.

---

# 9. Dedicated Validation Project

A dedicated Godot project must be created before functional Local GI implementation.

## Location

Prefer a local-only project inside the checkout:

```text
_local_hddagi_validation/
```

Add it only to `.git/info/exclude`, not the repository `.gitignore`, so the validation project does not enlarge the engine patch or create rebase conflicts.

If the environment cannot safely use `.git/info/exclude`, create it as a sibling project outside the Godot source checkout.

The validation project is a development instrument, not part of the engine patch.

## Project requirements

- Use the custom-built Godot editor/runtime from the current branch.
- Keep geometry simple and deterministic.
- Use strongly colored diffuse materials for bounce visibility.
- Use fixed camera paths where useful.
- Add simple scripts to move/rotate roots, dynamic objects, and lights.
- Add on-screen labels showing the current test case and active Local slots if practical.
- Do not use complex game content.

## Required scenes

```text
00_global_baseline.tscn
01_local_static.tscn
02_local_root_motion.tscn
03_gi_mode_dynamic.tscn
04_world_lights_to_local.tscn
05_global_indirect_to_local.tscn
06_local_to_global.tscn
07_two_local.tscn
08_three_local_switching.tscn
09_blend_boundary.tscn
```

### 00_global_baseline
Existing Global HDDAGI only. Used to catch regressions.

### 01_local_static
Closed/simple room, white surfaces, one saturated wall, one Local light.
Checks basic Local voxel/direct/probe/shading loop.

### 02_local_root_motion
Same Local room moved and rotated continuously as a whole.
Checks that Local history remains stable under root world transform changes.

### 03_gi_mode_dynamic
Contains:
- `GI_MODE_STATIC` large object.
- `GI_MODE_DYNAMIC` movable box/door.
- `GI_MODE_DISABLED` small receiver-only object.

### 04_world_lights_to_local
World Directional, Omni, Spot, and Area lights affect the Local room.
Include rotation of Local root.

### 05_global_indirect_to_local
A strong green exterior bounce target illuminated by Global lighting outside a Local room/window.
Checks Global indirect transfer into Local.

### 06_local_to_global
Strong red Local light / red wall with a large opening toward a white Global floor.
Checks that Local descendants still participate in Global HDDAGI.

### 07_two_local
Two simultaneously visible Local rooms with very different colors/lights.
Checks independent resources/history.

### 08_three_local_switching
Three Local rooms; camera moves so the active two must change.
Checks slot selection and inactive/active transitions.

### 09_blend_boundary
Camera/object crosses Local bounds repeatedly.
Checks Blend Distance and absence of severe hard seams.

---

# 10. Development Tasks and Validation Gates

A single AI session may continue across tasks using compaction, but **each task is a milestone**:

```text
implement
→ automated validation
→ human visual validation when required
→ fix if needed
→ commit
→ update LOCAL_HDDAGI_STATE.md
→ compact
→ next task
```

The repository and Git history are always the source of truth, not the previous model summary.

---

## Task 0 — Baseline Audit + Validation Project

### Implementation

Do not implement Local GI.

- Read the current HDDAGI branch thoroughly.
- Record current relevant C++/shader call paths.
- Identify HDDAGI state that is per-instance versus shared/global.
- Create `_local_hddagi_validation/`.
- Create `00_global_baseline.tscn` and basic project scaffolding.
- Add the validation project to `.git/info/exclude` only.

### Automated validation

- Full relevant engine build succeeds.
- Existing HDDAGI shaders compile.
- Validation project opens with the custom editor.
- `git diff` contains no validation-project files.

### Human visual validation — REQUIRED

Human confirms `00_global_baseline.tscn` behaves correctly before Local changes.

### Commit

No engine commit is required if no engine code changed.

---

## Task 1 — LocalDynamicGI3D Scene Plumbing

### Implementation

Add the thinnest scene/renderer registration necessary for `LocalDynamicGI3D`.

Implement:

- Node creation/destruction.
- `Enabled`.
- `Extend`.
- `Blend Distance` storage.
- Descendant Geometry/Light collection.
- Local contributor classification using `gi_mode`.
- Automatic Local bounds computation.
- Multiple Local node registration.

Do not run Local HDDAGI yet.

### Automated validation

- Build succeeds.
- Node can be created/deleted repeatedly.
- AABB merge/Extend logic is tested where practical.
- GI mode classification is tested where practical.
- No unrelated core HDDAGI shader changes.

### Human visual validation — REQUIRED

Human confirms editor/debug bounds follow the Local node correctly.

### Commit

```text
local-hddagi: add LocalDynamicGI3D scene plumbing
```

---

## Task 2 — Single Static Local HDDAGI Loop

### Implementation

Implement only one active Local slot first.

- Reuse existing HDDAGI resources/passes.
- Force Local to one cascade.
- Disable camera-centered cascade scrolling for Local.
- Reuse existing voxelization with Local-domain view/transform.
- Voxelize only Local contributor descendants.
- Use Local descendant lights first.
- Reuse existing Direct/Probe/Cache/filter logic unchanged.
- Add final Local sampling inside Local bounds.
- Keep Global behavior unchanged outside Local.

Local root translation/rotation must update transform only and must not cause Local voxel rebuild.

### Automated validation

- Engine and shaders compile.
- Repeated enable/disable/create/delete does not crash.
- Global-only scene still matches baseline behavior structurally.
- Add counters/asserts if useful to prove Local root motion does not increment full Local voxelization count.
- Check `git diff` for accidental HDDAGI algorithm rewrites.

### Human visual validation — REQUIRED

Use:

```text
01_local_static.tscn
02_local_root_motion.tscn
```

Human must confirm:

- Local bounce is visible.
- Color bleeding is visible.
- Local root translation does not reset/drag GI in world space.
- Local root rotation keeps GI attached to the Local room.
- Global exterior remains correct.

Do not mark Task 2 complete until the human reports PASS.

### Commit

```text
local-hddagi: add minimal static local runtime
```

---

## Task 3 — GI Mode + Dynamic Geometry

### Implementation

Implement:

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

Do not implement dirty regions.

Dynamic lights update light data only and do not mark Local geometry dirty.

### Automated validation

- Transform dirty-detection tests where practical.
- Root world transform changes do not dirty Local dynamic descendants if their Local transforms are unchanged.
- Dynamic descendant Local transform change causes exactly the expected full rebuild request.
- `GI_MODE_DISABLED` is absent from Local voxel contributors.
- Dynamic light movement does not trigger geometry rebuild.

### Human visual validation — REQUIRED

Use:

```text
03_gi_mode_dynamic.tscn
```

Human must confirm:

- Dynamic object receives Local GI while moving.
- Its occlusion/contribution eventually updates after movement.
- Static object remains stable.
- Disabled object receives GI but does not visibly contribute.
- Temporary convergence/flicker/performance cost is acceptable for V0.

### Commit

```text
local-hddagi: support GI mode and dynamic contributors
```

---

## Task 4 — Global → Local Lighting

### Implementation A: World direct lights

- Feed relevant World Directional/Omni/Spot/Area lights into Local.
- Transform light data into Local coordinates.
- Reuse existing Direct Light calculations.

### Implementation B: external visibility

World lights must not incorrectly penetrate Global occluders such as a tunnel roof.
Reuse existing Global occupancy/HDDA visibility where possible. Do not create a new shadow system.

### Implementation C: Global indirect

Modify only the Local probe miss/exit path:

```text
Local hit
→ Local VoxelLight

Local miss/exit
→ transform sample to World
→ sample existing Global HDDAGI directional radiance / probe atlas
→ existing Sky fallback if unavailable
```

Do not perform a second Global HDDA probe trace.

Frame order must remain:

```text
Global complete
→ Local reads Global result
```

### Automated validation

- Shader compilation.
- Correct position/direction transforms for each light type.
- No invalid Global resource access when Global HDDAGI is disabled/unavailable.
- No circular Global↔Local same-frame dependency.
- No modifications to direct-light BRDF equations unless strictly required.

### Human visual validation — REQUIRED

Use:

```text
04_world_lights_to_local.tscn
05_global_indirect_to_local.tscn
```

Human must confirm:

- World sun direction is correct while Local root rotates.
- World Omni/Spot/Area influence Local correctly.
- Global occlusion prevents obvious external-light penetration.
- Strong exterior green indirect light enters Local near an opening/window.
- The green bounce changes when Local moves away.

### Commit

```text
local-hddagi: receive global direct and indirect lighting
```

---

## Task 5 — Local → Global + Two Active Local Slots + Blend

### Local → Global

Do not add a transfer algorithm.

Ensure Local descendant geometry/lights remain normal World renderer instances and continue to participate in the existing Global HDDAGI pipeline.

### Two active slots

- Support arbitrary registered Local nodes.
- Select at most 2 active visible Local nodes per View.
- Update both active slots every frame using the same cadence.
- Keep Local0/Local1 resources/history independent.
- Inactive Local nodes do not run Direct/Probe/Voxel update.

### Blend

Implement `Blend Distance` for final shading.

If two Local bounds overlap, V0 may normalize Local weights. Do not add priority.

### Automated validation

- Register 3+ Local nodes without crash.
- Active slot count never exceeds 2.
- Slot selection changes correctly with camera/bounds position.
- Local0/Local1 resources are independent.
- Inactive→active transitions do not use invalid resources.
- Local descendants are still present in Global contributor path.

### Human visual validation — REQUIRED

Use:

```text
06_local_to_global.tscn
07_two_local.tscn
08_three_local_switching.tscn
09_blend_boundary.tscn
```

Human must confirm:

- Strong Local lighting/interior content affects Global exterior through the existing Global calculation.
- Two Local domains render independently without obvious cross-contamination.
- Three Local domains switch active slots as expected.
- Local/Global boundary blend has no severe hard seam.

### Commit

```text
local-hddagi: support two active local GI slots
```

---

## Task 6 — Stabilization and Rebase Audit

Do not add features or optimize performance.

### Audit

Check specifically for unnecessary modifications to:

```text
hddagi_preprocess.glsl
HDDA core traversal
Direct-light equations
Probe temporal accumulation
Probe filter
Voxel formats
```

Revert unnecessary changes.

### Resource/lifecycle validation

Exercise:

```text
create/remove node
enable/disable
scene reload
editor reload
viewport changes
Local slot switching
Global HDDAGI on/off
```

### Automated validation

- Debug build.
- Release/editor build as appropriate.
- Shader compilation.
- Existing relevant tests.
- New lightweight tests.
- Git diff/rebase-friendliness review.

### Human final visual regression — REQUIRED

Human runs all validation scenes 00–09 and reports PASS/FAIL individually.

### Commit

```text
local-hddagi: stabilize minimal local GI prototype
```

---

# 11. AI / Compaction Workflow

Each milestone may run in a long Codex/Cursor session, but after every completed task:

1. Run all automated validation required by that task.
2. If visual validation is required, stop and ask the human to execute the exact validation scene/procedure.
3. Do not mark the task complete until the human reports the result.
4. Fix any reported failures.
5. Commit only the current task.
6. Update `LOCAL_HDDAGI_STATE.md` by replacing current-state sections; do not append a long journal.
7. Compact the conversation.
8. Before continuing after compaction, reread:
   - `LOCAL_HDDAGI_PLAN.md`
   - `LOCAL_HDDAGI_STATE.md`
   - current `git status`
   - current `git diff`
   - recent relevant commits
9. Treat current repository contents as source of truth.
10. Continue only the next unfinished task.

Recommended instruction after every compaction:

```text
Before continuing, read LOCAL_HDDAGI_PLAN.md and LOCAL_HDDAGI_STATE.md, inspect git status/diff and recent relevant commits, then continue only the current unfinished task. The repository is the source of truth. Do not redesign or generalize the existing HDDAGI implementation.
```

---

# 12. Validation Responsibility Boundary

```text
AI may validate:
- compilation
- shader compilation
- static analysis
- unit tests
- transform/math tests
- resource lifetime assertions
- git diff scope
- automated counters

Human must validate:
- GI visual correctness
- color bleeding
- light leakage
- temporal flicker
- Local root stability in motion
- Local/Global transfer appearance
- Local/Global blend quality
- Local slot switching visual behavior
```

The AI must never report visual validation as passed unless the human explicitly reported PASS.

---

# 13. Required Handoff Format After Every Task

Update `LOCAL_HDDAGI_STATE.md` with this structure:

```text
Current Task:
Last Completed Commit:

Completed:
- ...

Current Implementation State:
- ...

Automated Validation:
- ...

Human Visual Validation:
- PASS / FAIL / NOT RUN

Known Problems:
- ...

Explicitly Not Implemented Yet:
- ...

Next Action:
- ...
```

Keep it short. The state file is a checkpoint, not a development diary.
