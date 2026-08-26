# STATE.md — Local Probe GI Prototype Execution State

> 本文件是跨 compaction 的唯一运行状态来源。
>
> 每个 Phase 完成后必须更新本文件，然后停止执行。
>
> 不记录长篇过程，只记录下一上下文继续工作所需的事实。

---

# 1. Current Status

```text
Current Phase: Phase 6 — Visibility + Final Local Shading
Status: NOT STARTED

Last Completed Phase: Phase 5 — One-Bounce Local GI
Next Phase: Phase 6
Blocked: No
Block Reason:
```

---

# 2. Current Objective

Phase 5 已完成（自动化 + 人工看图）。等待用户开始 Phase 6。不要自动进入 Phase 6。

---

# 3. Frozen Architecture

当前 Prototype 固定使用：

```text
LocalGIVolume3D
    ↓
Static Geometry Bake
    ↓
CPU Static BVH
    +
CPU Dynamic BVH (dirty rebuild)
    ↓
GPU Software Ray Tracing
    ↓
Regular Probe Grid
    ↓
Directional Irradiance + Visibility
    ↓
Temporal
    ↓
Multi-bounce Local GI
    ↓
Final Local Indirect Diffuse
```

Global GI：

```text
HDDAGI always runs globally
```

LocalGI 与 HDDAGI 只通过：

```text
1. GlobalIndirectCache
2. Final GI selection / override
```

连接。

---

# 4. Architecture Invariants

除非 `PLAN.md` 正式修改，否则不得改变。

## 4.1 LocalGI Core

不得直接依赖：

```text
HDDAGI voxel layout
HDDAGI hierarchy traversal
HDDAGI cascade layout
HDDAGI update scheduling
HDDAGI probe layout
HDDAGI internal shader files
HDDAGI internal resource ownership
```

允许依赖：

```text
GlobalIndirectCache API
```

---

## 4.2 Final GI Selection

```text
outside LocalGIVolume3D:
    Global GI

inside LocalGIVolume3D:
    Local GI
```

禁止：

```text
Global GI + Local GI
```

未来允许：

```text
mix(Global GI, Local GI, volume_weight)
```

---

## 4.3 Coordinate Space

以下全部固定在：

```text
LocalGIVolume3D local space
```

- Static geometry
- Static BVH
- Dynamic geometry representation
- Dynamic BVH
- Probe positions
- Local ray origins/directions before world conversion

Volume world transform 改变不得导致 Static BVH rebuild。

---

## 4.4 Geometry

Static:

```text
Bake once
→ CPU Static BVH
```

Dynamic:

```text
transform/mesh changed
→ dirty
→ CPU rebuild small Dynamic BVH
```

GPU：

```text
trace Static BVH
trace Dynamic BVH
choose nearest hit
```

---

## 4.5 Transport

只实现：

```text
Diffuse Lambertian GI
```

明确区分：

```text
Radiance
Irradiance
Albedo
BRDF
Visibility
Distance
```

不得使用不明确的 magic multiplier / clamp 修正物理错误。

---

## 4.6 External Indirect

Local ray：

```text
no local hit
+
exits LocalGIVolume
    ↓
GlobalIndirectCache
    ↓
external incoming indirect radiance
```

LocalGI Core 不得知道 provider 是 HDDAGI。

---

# 5. Repository Facts

由 Phase 0 填写。

```text
Repository root:
    F:/godot

Godot branch:
    feature/hddagi-4.7/local-dynamic-gi
    tracks origin/feature/hddagi-4.7/local-dynamic-gi

Godot base revision:
    4.7-stable
    merge-base with origin/master: 5b4e0cb0fd279832bbdd69fed5354d4e5ad26f88
    "Bump version to 4.7-stable"

HDDAGI branch/revision:
    hddagi-4.7 tip == HEAD
    ab154bfd170dce1047ec4b2842c0fc1be31a90ff
    "HDDAGI: increase LIGHTPROBE_OCT_SIZE to 6, remove redundant barrier, pack occlusion shared memory as half2x16"

Renderer backend:
    Forward+ / clustered RenderingDevice
    LocalGI prototype targets Forward+ only

Build configuration:
    platform=windows arch=x86_64 target=editor tests=yes
    MSVC 14.5 / Windows SDK 10.0.26100.0
    Binary: bin/godot.windows.editor.x86_64.exe

Forward+ renderer files:
    servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp
    servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h
    servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp
    servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl
    servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered_inc.glsl

GI integration files:
    servers/rendering/renderer_rd/environment/gi.cpp
    servers/rendering/renderer_rd/environment/gi.h
    servers/rendering/environment/renderer_gi.h
    servers/rendering/renderer_rd/shaders/environment/gi.glsl
    servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl
    servers/rendering/renderer_rd/renderer_scene_render_rd.cpp
    scene/resources/environment.cpp  (dynamic_gi_* == HDDAGI)

VoxelGI reference files:
    scene/3d/voxel_gi.h
    scene/3d/voxel_gi.cpp
    scene/3d/voxelizer.cpp
    editor/scene/3d/voxel_gi_editor_plugin.cpp
    editor/scene/3d/gizmos/voxel_gi_gizmo_plugin.cpp
    servers/rendering/renderer_rd/shaders/environment/voxel_gi.glsl
    Registration: scene/register_scene_types.cpp (GDREGISTER_CLASS next to VoxelGI)

RenderingDevice helper files:
    servers/rendering/rendering_device.h
    compute_list_begin / storage_buffer_create / compute_list_dispatch
    Pattern reference: servers/rendering/renderer_rd/environment/gi.cpp

Mesh extraction path:
    VoxelGI::_find_meshes in scene/3d/voxel_gi.cpp
    Eligible: visible MeshInstance3D / MultiMesh / get_meshes() with GI_MODE_STATIC
    Local xform: volume_global.affine_inverse() * mesh_global
    Triangle source: Mesh::surface_get_arrays / Mesh::get_faces
    Phase 1 should follow this collection pattern, then emit triangles (not voxels)

Light buffer path:
    servers/rendering/storage/light_storage.h
    servers/rendering/renderer_rd/storage_rd/light_storage.h
    servers/rendering/renderer_rd/storage_rd/light_storage.cpp
    Forward+ clustered light/cluster buffers in render_forward_clustered.*
    LocalGI must read generic renderer lights, never HDDAGI lights

Shadow resource path:
    ShadowAtlas + directional shadows in light_storage.h/.cpp
    light_instance_get_shadow_atlas_rect / shadow textures owned by LightStorage

Shader include path:
    servers/rendering/renderer_rd/shaders/
    servers/rendering/renderer_rd/shaders/environment/
    servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl

Existing test infrastructure:
    tests/ doctest via scons tests=yes
    Command: bin/godot.windows.editor.x86_64.exe --headless --test
    Scene tests auto-picked from tests/scene/*.cpp
    Separate editor project: _local_hddagi_validation/ (unrelated HDDAGI validation, leave untouched)

Prototype project path:
    F:/godot/_local_gi_prototype
```

---

# 6. HDDAGI / GlobalIndirectCache Facts

Phase 0 必须填写。

```text
HDDAGI radiance cache exists:
Yes — per-view-buffer octahedral lightprobe textures on GI::HDDAGI

Cache stores:
- lightprobe_diffuse / lightprobe_diffuse_filter : octahedral directional irradiance
- lightprobe_specular : octahedral specular / reflection-like radiance
- lightprobe_ambient : low-frequency ambient
- occlusion_tex[2] : probe occlusion
- cascade voxel/probe hierarchy (NOT for LocalGI Core)

Semantic warning:
This is probe-interpolated directional irradiance + specular oct lookup,
NOT a general incoming radiance field. Adapter must not advertise it as raw radiance.

Directional sampling possible:
Yes, GPU-only, via octahedron lookup by direction

Arbitrary world-position sampling possible:
Partial. Works when the world position falls inside an HDDAGI cascade.
Positions outside all cascades return empty/zero.
No CPU sampling path exists.

Required GPU resources:
- hddagi_lightprobe_diffuse (texture2DArray)
- hddagi_lightprobe_specular (texture2DArray)
- hddagi_occlusion[0/1] (texture3D)
- HDDAGI UBO (cascades, probe_axis_size, grid_size, y_mult, ...)
Resources live on RenderBufferCustomDataRD (per view)

Existing sampling function/path:
- hddagi_process(vertex, normal, reflection, roughness) in scene_forward_gi_inc.glsl
- sdfvoxel_gi_process(...) same file
- Forward+ bindings: scene_forward_clustered_inc.glsl set 1 binding 31/32
- Tightly coupled to cascade layout / probe_axis_size / occlusion packing

Adapter can remain isolated:
Yes, if adapter owns a new sample include + uniform set.
LocalGI Core may only call sample_global_indirect_radiance(world_position, world_direction).
Adapter may contain HDDAGI texture/UBO knowledge. LocalGI Core may not.

Deep HDDAGI modification required:
No, for a read-only adapter that binds existing probe textures.
Yes, only if CPU sampling or a dedicated non-cascade cache is demanded.
```

Evidence / source locations:

```text
servers/rendering/renderer_rd/environment/gi.h
    GI::HDDAGI, LIGHTPROBE_OCT_SIZE=6, get_lightprobe_diffuse_texture()
servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl
    hddagi_process / sdfvoxel_gi_process
servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered_inc.glsl
    hddagi_lightprobe_specular/diffuse bindings
servers/rendering/renderer_rd/shaders/environment/gi.glsl
    screen-space GI resolve using the same probe textures
```

Decision:

```text
FEASIBLE with isolated HDDAGI adapter.
Phase 7 implements the adapter.
If adapter isolation cannot be kept, fall back to Null/Zero provider and continue LocalGI Core.
Do not modify HDDAGI internals in Phase 0–6.
```

---

# 7. LocalGI File Ownership

Phase 0 后填写实际文件。

## LocalGI-owned

```text
scene/3d/local_gi/local_gi_volume_3d.h
scene/3d/local_gi/local_gi_volume_3d.cpp
scene/3d/local_gi/local_gi_bvh.h
scene/3d/local_gi/local_gi_bvh.cpp
scene/3d/local_gi/local_gi_static_geometry.h
scene/3d/local_gi/local_gi_static_geometry.cpp
scene/3d/local_gi/local_gi_gpu_tracer.h
scene/3d/local_gi/local_gi_gpu_tracer.cpp
scene/3d/local_gi/local_gi_bvh_trace.glsl
scene/3d/local_gi/local_gi_probe_grid.h
scene/3d/local_gi/local_gi_probe_grid.cpp
scene/3d/local_gi/local_gi_direct_light.h
scene/3d/local_gi/local_gi_direct_light.cpp
scene/3d/local_gi/SCsub
```

后续 Phase 目标目录（尚未创建）：

```text
servers/rendering/renderer_rd/environment/local_gi/*
servers/rendering/renderer_rd/shaders/environment/local_gi/*
```

---

## GlobalIndirectCache Adapter-owned

Proposed, not created yet (Phase 7):

```text
servers/rendering/renderer_rd/environment/local_gi/global_indirect_cache.h
servers/rendering/renderer_rd/environment/local_gi/hddagi_global_indirect_cache_adapter.cpp
servers/rendering/renderer_rd/environment/local_gi/hddagi_global_indirect_cache_adapter.h
servers/rendering/renderer_rd/shaders/environment/local_gi/hddagi_global_indirect_cache.glsl
```

HDDAGI-specific 代码必须尽量限制在这里。

---

## Godot Integration Modifications

```text
scene/3d/SCsub
    chain-load local_gi/SCsub
scene/register_scene_types.cpp
    include + GDREGISTER_CLASS(LocalGIVolume3D)
doc/classes/LocalGIVolume3D.xml
    class reference
editor/scene/3d/gizmos/local_gi_volume_3d_gizmo_plugin.h
    Phase 3 ray/hit debug gizmo
editor/scene/3d/gizmos/local_gi_volume_3d_gizmo_plugin.cpp
    Phase 3 ray/hit debug gizmo
editor/scene/3d/node_3d_editor_plugin.cpp
    register LocalGIVolume3D gizmo
```

只允许必要 integration hooks。尚未改 RenderingServer / gi.cpp / 着色。

---

## Test-owned

```text
tests/scene/test_local_gi_volume_3d.cpp
tests/scene/test_local_gi_static_bvh.cpp
tests/scene/test_local_gi_dynamic_bvh.cpp
tests/scene/test_local_gi_gpu_bvh.cpp
tests/scene/test_local_gi_probe_grid.cpp
tests/scene/test_local_gi_one_bounce.cpp
_local_gi_prototype/project.godot
_local_gi_prototype/.gitignore
_local_gi_prototype/scripts/smoke_test.gd
_local_gi_prototype/scripts/ray_hit_debug.gd
_local_gi_prototype/scripts/probe_grid_debug.gd
_local_gi_prototype/scripts/one_bounce_debug.gd
_local_gi_prototype/scripts/energy_albedo.gd
_local_gi_prototype/scenes/a_cornell_baseline.tscn
_local_gi_prototype/scenes/b_white_cornell_energy.tscn
_local_gi_prototype/scenes/c_cornell_thin_wall.tscn
_local_gi_prototype/scenes/d_two_chamber_cornell.tscn
_local_gi_prototype/scenes/e_open_cornell_external_gi.tscn
_local_gi_prototype/scenes/f_dynamic_object_cornell.tscn
_local_gi_prototype/scenes/g_moving_local_volume.tscn
_local_gi_prototype/scenes/h_performance_cornell.tscn
_local_gi_prototype/scenes/parts/test_rig.tscn
_local_gi_prototype/scenes/parts/cornell_interior.tscn
_local_gi_prototype/scenes/parts/cornell_interior_white.tscn
_local_gi_prototype/materials/*
_local_gi_prototype/resources/prototype_environment.tres
```

---

## Existing Files Explicitly Intended To Remain Untouched

```text
servers/rendering/renderer_rd/environment/gi.cpp
servers/rendering/renderer_rd/environment/gi.h
servers/rendering/renderer_rd/shaders/environment/hddagi_*.glsl
servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl
servers/rendering/renderer_rd/shaders/environment/gi.glsl
scene/3d/voxel_gi.cpp
scene/3d/voxel_gi.h
editor/scene/3d/voxel_gi_editor_plugin.cpp
_local_hddagi_validation/*
scene/3d/local_dynamic_gi_3d.cpp
scene/3d/local_dynamic_gi_3d.h
servers/rendering/local_dynamic_gi.cpp
```

---

# 8. Implemented Features

当前：

```text
[x] LocalGIVolume3D skeleton
[x] Bake entry point
[x] Static triangle extraction
[x] CPU Static BVH
[x] Dynamic contributor registration
[x] CPU Dynamic BVH
[x] GPU BVH traversal
[x] Static/Dynamic nearest hit
[x] Probe grid
[x] Probe ray generation
[x] One-bounce diffuse GI
[ ] Probe distance moments
[ ] Visibility interpolation
[ ] Final LocalGI shading
[ ] GlobalIndirectCache API
[ ] HDDAGI cache adapter
[ ] Temporal
[ ] Multi-bounce
[ ] Dynamic contributor visual behavior
[ ] Moving volume support
[ ] Performance instrumentation
```

---

# 9. Test Project

Project:

```text
Name: LocalGIPrototype
Path: F:/godot/_local_gi_prototype
Main scene: res://scenes/a_cornell_baseline.tscn
Renderer: forward_plus
HDDAGI: Environment.dynamic_gi_enabled = true
```

Scenes:

```text
Scene A — Cornell Baseline:
Status: SKELETON CREATED
Path: _local_gi_prototype/scenes/a_cornell_baseline.tscn

Scene B — White Cornell Energy Box:
Status: SKELETON CREATED
Path: _local_gi_prototype/scenes/b_white_cornell_energy.tscn
Albedo helper: scripts/energy_albedo.gd default 0.5

Scene C — Cornell Thin Wall:
Status: SKELETON CREATED
Path: _local_gi_prototype/scenes/c_cornell_thin_wall.tscn
Default wall thickness: 10 cm

Scene D — Two-Chamber Cornell:
Status: SKELETON CREATED
Path: _local_gi_prototype/scenes/d_two_chamber_cornell.tscn
Doorway gap remains at +Z end of divider

Scene E — Open Cornell / External GI:
Status: SKELETON CREATED
Path: _local_gi_prototype/scenes/e_open_cornell_external_gi.tscn
Front wall hidden; exterior ground + blue wall added

Scene F — Dynamic Object Cornell:
Status: SKELETON CREATED
Path: _local_gi_prototype/scenes/f_dynamic_object_cornell.tscn
Dynamic contributors use gi_mode = DYNAMIC

Scene G — Moving Local Volume:
Status: SKELETON CREATED
Path: _local_gi_prototype/scenes/g_moving_local_volume.tscn
LocalGI + interior parented under MovingRoot

Scene H — Performance Cornell:
Status: SKELETON CREATED
Path: _local_gi_prototype/scenes/h_performance_cornell.tscn
Parametric complexity not yet generated
```

---

# 10. Test Parameters

Keep these stable unless a Phase explicitly changes them.

## Thin Wall

```text
5 cm
10 cm
15 cm
20 cm
```

## White Cornell Albedo

```text
0.2
0.5
0.8
```

## Probe Benchmark Counts

```text
100
300
500
800
```

## Rays Per Probe

```text
32
64
128
256
```

## Update Fraction

```text
100%
50%
25%
```

---

# 11. Verification Ledger

---

## Phase 0 — Repository Reconnaissance + Test Project

Status:

```text
COMPLETE
```

Static:

```text
[x] Godot builds
[x] Prototype project loads
[x] LocalGIVolume3D skeleton can exist
```

Automated:

```text
[x] Smoke-test project/resources load
[x] doctest [SceneTree][LocalGIVolume3D] 2 passed / 17 assertions
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Open LocalGIPrototype with:
    bin/godot.windows.editor.x86_64.exe --editor --path F:/godot/_local_gi_prototype
Confirm Cornell test scenes/project open correctly.
No GI correctness judgment required.
```

Human result:

```text
PASS
```

Notes:

```text
Import printed EditorNode::is_cmdline_mode singleton-null once; smoke still passed.
No LocalGI gizmo/icon yet; volume is visible in the scene tree and inspector.
bake() is a documented no-op.
Human confirmed Cornell scenes and LocalGIVolume3D are visible in the editor.
```

---

## Phase 1 — Static Bake + CPU BVH

Status:

```text
COMPLETE
```

Automated / Unit:

```text
[x] triangle extraction
[x] local-space transform
[x] direct hit
[x] miss
[x] nearest hit
[x] parallel ray
[x] edge hit
[x] 5 cm wall
[x] 10 cm wall
[x] 15 cm wall
[x] 20 cm wall
[x] deterministic BVH build
```

Human Visual:

```text
NOT REQUIRED
```

Result:

```text
PASS
```

Notes:

```text
doctest [SceneTree][LocalGIVolume3D] 7 passed / 95 assertions
prototype smoke now bakes A–H and requires triangle_count > 0
Bake collects GI_MODE_STATIC visible meshes whose triangles intersect the volume AABB
Coordinates stay in LocalGIVolume local space; volume move does not rebuild
```

---

## Phase 2 — Dynamic BVH

Status:

```text
COMPLETE
```

Automated / Unit:

```text
[x] relevant transform change marks dirty
[x] relevant mesh change marks dirty
[x] stationary object does not rebuild continuously
[x] static BVH unchanged
[x] dynamic BVH rebuild changes hit
[x] remove object updates BVH
[x] static + dynamic nearest hit correct
```

Human Visual:

```text
NOT REQUIRED
```

Result:

```text
PASS
```

Notes:

```text
doctest [SceneTree][LocalGIVolume3D] 13 passed / 144 assertions
prototype smoke A–H still bake; Scene F also update_dynamic with triangle_count > 0 and no stationary rebuild
Dynamic contributors are discovered as visible GI_MODE_DYNAMIC meshes that intersect the volume AABB
Rebuild only when contributor snapshot or volume bounds change; static BVH is not touched
```

---

## Phase 3 — GPU BVH Traversal

Status:

```text
COMPLETE
```

Automated:

```text
[x] CPU/GPU hit/miss match
[x] CPU/GPU nearest hit match
[x] distance within tolerance
[x] normal within tolerance
[x] static/dynamic nearest hit match
```

Tolerance:

```text
distance <= 1e-3
normal error (1-dot) <= 2e-3 when triangle identity matches
shared-edge identity/normal disagreement is reported but does not fail if hit/distance/position match
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Open _local_gi_prototype / scenes/c_cornell_thin_wall.tscn
debug_mode is DEBUG_RAY_HIT_MISS via scripts/ray_hit_debug.gd
- inspect ray/hit debug (green hit, dim miss, blue normals)
- confirm the 10cm thin wall visibly blocks +X rays
- confirm normals/distances look coherent
Also try DEBUG_HIT_NORMAL and DEBUG_HIT_DISTANCE
```

Human result:

```text
PASS
```

Notes:

```text
doctest [SceneTree][LocalGIVolume3D] 16 passed / 188 assertions
prototype smoke A–H still bake/update; all scenes compare a small CPU/GPU ray set
GPU tracer lives in scene/3d/local_gi (lightmapper-style local RD), not HDDAGI/gi.cpp
Shared process-wide local RenderingDevice; recreating local Vulkan devices crashed nvoglv64
Human confirmed green hit rays stop on first surfaces and the 10cm thin wall blocks +X rays.
```

---

## Phase 4 — Probe Grid + Probe Rays

Status:

```text
COMPLETE
```

Automated:

```text
[x] exact probe count
[x] exact local positions
[x] normalized directions
[x] deterministic directions
[x] exact ray budget
[x] world transform does not alter local layout
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Open _local_gi_prototype with the editor binary.
Scene A (a_cornell_baseline.tscn): DEBUG_PROBE_POSITIONS — cyan probe crosses, yellow selected probe.
Scene G (g_moving_local_volume.tscn): DEBUG_SELECTED_PROBE_RAYS — selected probe spherical rays, green hits / dim misses.
Move/rotate MovingRoot or LocalGIVolume3D and confirm probes stay attached in local space.
Switch debug_mode between Probe Positions and Selected Probe Rays in the inspector.
```

Human result:

```text
PASS
```

Notes:

```text
doctest [SceneTree][LocalGIVolume3D] 21 passed / 1022 assertions
prototype smoke A–H still bake/update/GPU-compare and now build_probes with exact budget + transform-invariant local positions
Grid is cell-centered: resolution[i] = max(2, floor(size[i] / spacing)); default 4.4m / 0.5m → 8^3 = 512
Directions are a deterministic Fibonacci lattice shared by every probe
No GI / shading / irradiance yet
Human confirmed probe grid and selected rays.
```

---

## Phase 5 — One-Bounce Local GI

Status:

```text
COMPLETE
```

Automated:

```text
[x] zero light → zero GI
[x] zero albedo → zero reflected contribution
[x] light ×2 → GI approximately ×2
[x] albedo ×0.5 → reflected contribution approximately ×0.5
[x] no NaN/Inf
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Cornell Baseline:
- inspect light direction
- inspect red/green color bleeding direction
- inspect obvious overbright/inversion artifacts
```

Human result:

```text
PASS
```

---

## Phase 6 — Visibility + Final Local Shading

Status:

```text
NOT STARTED
```

Automated:

```text
[ ] probe weight validity
[ ] visibility suppresses blocked contribution
[ ] deterministic interpolation
[ ] no NaN/Inf
[ ] boundary sampling valid
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Cornell Thin Wall:
- 5 cm
- 10 cm
- 15 cm
- 20 cm

Two-Chamber Cornell:
- closed divider
- doorway
- window

Record whether leakage is:
- geometry/BVH
- probe interpolation/visibility
- unclear
```

Human result:

```text
PENDING
```

---

## Phase 7 — GlobalIndirectCache Adapter

Status:

```text
NOT STARTED
```

Automated:

```text
[ ] Null provider returns 0
[ ] Mock provider returns known value
[ ] LocalGI receives mock value correctly
[ ] adapter-specific code isolated
```

HDDAGI Integration:

```text
[ ] cache resources identified
[ ] sampling semantics identified
[ ] no LocalGI Core dependency on HDDAGI internals
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Open Cornell / External GI:
- cache disabled
- cache enabled
- external colored indirect source

Confirm external indirect enters LocalGI only through boundary sampling.
```

Human result:

```text
PENDING
```

---

## Phase 8 — Temporal

Status:

```text
NOT STARTED
```

Automated:

```text
[ ] constant input converges
[ ] no indefinite growth
[ ] light-on response valid
[ ] light-off decay valid
[ ] hysteresis behavior measurable
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Inspect:
- flicker
- convergence speed
- ghosting
- brightness instability
```

Human result:

```text
PENDING
```

---

## Phase 9 — Multi-Bounce

Status:

```text
NOT STARTED
```

Automated Energy Tests:

```text
[ ] Albedo 0.2 stable
[ ] Albedo 0.5 stable
[ ] Albedo 0.8 stable
[ ] no same-pass read/write feedback
[ ] ping-pong/history semantics verified
```

Measurements:

```text
Albedo 0.2:
Average irradiance:
Peak irradiance:
Representative luminance:

Albedo 0.5:
Average irradiance:
Peak irradiance:
Representative luminance:

Albedo 0.8:
Average irradiance:
Peak irradiance:
Representative luminance:
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
White Cornell:
- confirm no persistent brightening
- confirm bounce persistence increases with albedo
- confirm no unexplained color drift
```

Human result:

```text
PENDING
```

---

## Phase 10 — Dynamic Object Visual Validation

Status:

```text
NOT STARTED
```

Automated:

```text
[ ] moving object rebuilds Dynamic BVH
[ ] stationary object does not rebuild
[ ] Static BVH remains unchanged
[ ] hit follows dynamic transform
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Dynamic Object Cornell:
- move white box
- rotate door
- move colored panel

Inspect:
- occlusion updates
- bounce updates
- stale-BVH artifacts
```

Human result:

```text
PENDING
```

---

## Phase 11 — Moving Volume Validation

Status:

```text
NOT STARTED
```

Automated:

```text
[ ] Static BVH rebuild count stays 0
[ ] Probe local positions unchanged
[ ] history not reset only because world transform changes
[ ] GlobalIndirectCache queries use correct world-space coordinates
```

Human Visual:

```text
REQUIRED
```

Human task:

```text
Test:
- translation
- rotation
- high-speed translation
- combined motion

Inspect:
- flicker
- swimming
- detachment
- brightness popping
- external GI direction
- rotation/history errors
```

Human result:

```text
PENDING
```

---

## Phase 12 — Performance Baseline

Status:

```text
NOT STARTED
```

Human Visual:

```text
NOT REQUIRED
```

Environment:

```text
GPU:
Driver:
Resolution:
Renderer:
Build type:
OS:
```

CPU Measurements:

```text
Static BVH build:
Dynamic BVH rebuild:
BVH upload:
```

GPU Measurements:

```text
Ray tracing:
Probe integration:
Visibility:
Temporal:
GlobalIndirectCache:
Final shading:
Total LocalGI:
```

Benchmark Table:

| Probes | Rays/Probe | Update % | Triangles | Dynamic Tris | Trace ms | Probe ms | Cache ms | Shading ms | Total ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| | | | | | | | | | |

---

## Phase 13 — Architecture Decision

Status:

```text
NOT STARTED
```

Measured primary bottleneck:

```text
UNKNOWN
```

Chosen next optimization:

```text
NONE
```

Rejected optimizations:

```text
-
```

Evidence:

```text
-
```

---

# 12. Debug Views

Implementation status:

```text
[ ] Local Geometry
[ ] Static BVH Hit
[ ] Dynamic BVH Hit
[ ] Ray Hit/Miss
[ ] Hit Normal
[ ] Hit Distance
[ ] Probe Positions
[ ] Selected Probe Rays
[ ] Raw Probe Radiance
[ ] Probe Irradiance
[ ] Visibility
[ ] Probe Weights
[ ] GlobalIndirectCache Sample
[ ] Final LocalGI
[ ] GlobalGI
[ ] Final Selected GI
```

---

# 13. Performance Counters

Must exist before Phase 12 if practical.

```text
Static BVH rebuild count:
Dynamic BVH rebuild count:
Static triangle count:
Dynamic triangle count:
Probe count:
Rays per probe:
Updated probes/frame:
Total rays/frame:
```

Current values:

```text
N/A
```

---

# 14. Known Issues

Current:

```text
None recorded.
```

Use this format:

```text
ID:
Phase introduced:
Symptom:
Reproduction:
Subsystem:
Evidence:
Suspected cause:
Blocking: Yes/No
Workaround:
```

不得把猜测写成事实。

---

# 15. Decisions Made

```text
D001
LocalGI uses CPU-built Static BVH.

D002
Dynamic rigid contributors use a separate CPU Dynamic BVH rebuilt only when dirty.

D003
Static/Dynamic BVH traversal and all runtime transport run on GPU.

D004
LocalGI geometry and probes live in LocalGIVolume local space.

D005
LocalGI Core is independent of HDDAGI internals.

D006
HDDAGI remains active globally at all times.

D007
Inside LocalGIVolume, LocalGI replaces Global GI for indirect diffuse.

D008
Global and Local indirect are not additively combined.

D009
External indirect enters LocalGI through GlobalIndirectCache when a Local ray exits the Volume.

D010
GlobalIndirectCache is an abstract provider interface; HDDAGI is only one provider implementation.

D011
Prototype starts with diffuse-only Lambertian transport.

D012
Temporal is estimation/EMA, never additive accumulation.

D013
Multi-bounce reads only a previous completed Local probe field.

D014
No performance optimization before measured baseline.

D015
Cornell Box variants are the canonical visual validation scenes.

D016
HDDAGI already has a GPU octahedral probe cache that an isolated adapter can sample.
Cache semantic is probe-interpolated directional irradiance/specular, not raw radiance.
Phase 7 owns the adapter. Phases 0–6 must not modify HDDAGI internals.
If isolation fails, GlobalIndirectCache provider = Null/Zero.

D017
Default bake root is the highest non-Viewport ancestor, not just get_parent().
This is required because LocalGIVolume3D lives under TestRig while Cornell meshes are siblings of TestRig.

D018
CPU Static BVH is an explicit binary tree: one triangle per leaf, longest-axis centroid median split, original-index tie-break.
Node arrays stay upload-friendly for Phase 3. No Embree / TriangleMesh / Wicked Engine code.

D019
Bake may run before the node is inside the SceneTree. Collection uses get_global_transform() when in-tree, otherwise composed Node3D local transforms.
This Godot branch's Node3D::is_visible_in_tree() does not check is_inside_tree().

D020
Dynamic contributors are discovered by a GI_MODE_DYNAMIC scene walk, not an explicit register/unregister API.
add/remove/hide/mode-change is detected because the contributor snapshot membership changes.

D021
Dynamic dirty detection compares a snapshot of instance id, mesh RID, surface count, mesh AABB, extra index, volume-local transform, and volume AABB.
update_dynamic() rebuilds only when that snapshot differs. GPU upload is deferred to Phase 3.

D022
Combined CPU intersect_ray traces Static BVH and Dynamic BVH independently and keeps the nearer hit.
Equal distances prefer the static hit so the result is deterministic.

D023
Phase 3 GPU tracer is a scene-owned compute pass using a packed std430 copy of the CPU BVH arrays.
It does not touch RenderingServer GI, gi.cpp, or HDDAGI shaders.
A process-wide shared local RenderingDevice is reused; per-tracer Vulkan device create/destroy crashed the NVIDIA driver in --test.

D024
CPU/GPU comparison requires hit/miss, nearest distance, and position to match within 1e-3.
Triangle identity and opposite normals on shared edges are reported but do not fail the comparison.
GPU combined query prefers static on equal distance, same as CPU.

D025
Probe grid is cell-centered in volume local space: each axis uses max(2, floor(size / spacing)) probes.
Index order is X slowest, then Y, then Z. Volume world transform does not change local positions.

D026
Probe directions are a deterministic Fibonacci / golden-spiral unit lattice, shared by every probe.
Same rays_per_probe always yields the same directions. Phase 4 traces them with the existing GPU tracer and does not compute radiance.

D027
Phase 5 evaluates one-bounce on the CPU from CPU ray hits so energy tests are exact.
GPU lighting remains later transport work. Triangle albedo is BaseMaterial3D albedo RGB used as linear reflectance.
Lights use color * energy * indirect_energy. Missed rays contribute 0 until Phase 7.

D028
Probe spherical irradiance is (4π / N) * Σ incoming radiance.
Incoming radiance is Lambertian outgoing radiance from the hit: albedo / π * direct irradiance.
Direct irradiance uses Godot omni/spot attenuation, N·L, and a BVH shadow ray.

D029
Probe irradiance debug draws opaque spheres. Each vertex uses the actual incoming radiance of the nearest Fibonacci ray. Vertex alpha is forced to 1. Display is not remapped, exposed, or percentile-normalized.
```

---

# 16. Deferred Work

Do not implement unless Phase 13 or a revised PLAN explicitly requires it.

```text
GPU BVH build
GPU BVH refit
SkinnedMesh GI contribution
Cloth GI contribution
Particle GI contribution
Adaptive probe density
Probe relocation
Probe classification
Probe sleeping
Cascades
Scrolling grids
Specular GI
Denoiser
Multiple LocalGI volume overlap
Volume priority UI
Production editor polish
Streaming integration
LocalGI → HDDAGI feedback
Direct HDDAGI hierarchy tracing from LocalGI
```

---

# 17. Changed Files

Current:

```text
scene/3d/local_gi/local_gi_volume_3d.h
- LocalGIVolume3D skeleton node
- ownership: LocalGI
- phase introduced: 0

scene/3d/local_gi/local_gi_volume_3d.cpp
- properties, AABB, no-op bake
- ownership: LocalGI
- phase introduced: 0

scene/3d/local_gi/SCsub
- compile local_gi sources into scene library
- ownership: LocalGI
- phase introduced: 0

scene/3d/SCsub
- chain-load local_gi/SCsub
- ownership: Godot Integration
- phase introduced: 0

scene/register_scene_types.cpp
- register LocalGIVolume3D
- ownership: Godot Integration
- phase introduced: 0

doc/classes/LocalGIVolume3D.xml
- class reference
- ownership: Godot Integration
- phase introduced: 0

tests/scene/test_local_gi_volume_3d.cpp
- skeleton defaults, AABB, PackedScene round-trip
- ownership: Test
- phase introduced: 0

_local_gi_prototype/**
- Cornell Scene A–H skeletons + smoke script
- ownership: Test
- phase introduced: 0
```

Update format:

```text
path/to/file
- change summary
- ownership: LocalGI / Adapter / Godot Integration / Test
- phase introduced
```

Phase 1 additions:

```text
scene/3d/local_gi/local_gi_bvh.h
- CPU triangle / node / ray-hit types and LocalGIBVH
- ownership: LocalGI
- phase introduced: 1

scene/3d/local_gi/local_gi_bvh.cpp
- deterministic median-split build + nearest-hit CPU traversal
- ownership: LocalGI
- phase introduced: 1

scene/3d/local_gi/local_gi_static_geometry.h
- static mesh collection / triangle extract API
- ownership: LocalGI
- phase introduced: 1

scene/3d/local_gi/local_gi_static_geometry.cpp
- VoxelGI-like STATIC collection, local-space triangles, off-tree transform compose
- ownership: LocalGI
- phase introduced: 1

scene/3d/local_gi/local_gi_volume_3d.h
- bake data accessors and CPU ray query
- ownership: LocalGI
- phase introduced: 0, updated 1

scene/3d/local_gi/local_gi_volume_3d.cpp
- bake() collects + builds static BVH
- ownership: LocalGI
- phase introduced: 0, updated 1

doc/classes/LocalGIVolume3D.xml
- bake / triangle count / intersect_static_ray
- ownership: Godot Integration
- phase introduced: 0, updated 1

tests/scene/test_local_gi_static_bvh.cpp
- Phase 1 geometry / BVH unit tests
- ownership: Test
- phase introduced: 1

_local_gi_prototype/scripts/smoke_test.gd
- deferred bake + triangle_count > 0
- ownership: Test
- phase introduced: 0, updated 1
```

Phase 2 additions:

```text
scene/3d/local_gi/local_gi_static_geometry.h
- GI mode collect + LocalGIContributorKey snapshot
- ownership: LocalGI
- phase introduced: 1, updated 2

scene/3d/local_gi/local_gi_static_geometry.cpp
- parameterized STATIC/DYNAMIC walk and key compare
- ownership: LocalGI
- phase introduced: 1, updated 2

scene/3d/local_gi/local_gi_volume_3d.h
- dynamic dirty / update / nearest-hit query
- ownership: LocalGI
- phase introduced: 0, updated 2

scene/3d/local_gi/local_gi_volume_3d.cpp
- update_dynamic rebuilds only when snapshot changes
- ownership: LocalGI
- phase introduced: 0, updated 2

doc/classes/LocalGIVolume3D.xml
- update_dynamic / dirty / dynamic and combined ray queries
- ownership: Godot Integration
- phase introduced: 0, updated 2

tests/scene/test_local_gi_dynamic_bvh.cpp
- Phase 2 dirty / rebuild / nearest-hit unit tests
- ownership: Test
- phase introduced: 2

_local_gi_prototype/scripts/smoke_test.gd
- Scene F update_dynamic + no stationary rebuild
- ownership: Test
- phase introduced: 0, updated 2
```

Phase 3 additions:

```text
scene/3d/local_gi/local_gi_gpu_tracer.h
- packed GPU BVH structs + CPU/GPU compare result
- ownership: LocalGI
- phase introduced: 3

scene/3d/local_gi/local_gi_gpu_tracer.cpp
- shared local RD, upload, compute trace, readback
- ownership: LocalGI
- phase introduced: 3

scene/3d/local_gi/local_gi_bvh_trace.glsl
- software BVH traversal compute shader
- ownership: LocalGI
- phase introduced: 3

scene/3d/local_gi/local_gi_volume_3d.h/.cpp
- upload_gpu / intersect_gpu_* / compare_cpu_gpu_rays / debug mesh
- ownership: LocalGI
- phase introduced: 0, updated 3

editor/scene/3d/gizmos/local_gi_volume_3d_gizmo_plugin.*
- volume box + ray/hit debug
- ownership: Godot Integration
- phase introduced: 3

tests/scene/test_local_gi_gpu_bvh.cpp
- CPU/GPU hit, thin wall, static+dynamic nearest
- ownership: Test
- phase introduced: 3

_local_gi_prototype/scripts/ray_hit_debug.gd
- Scene C bake/upload + DEBUG_RAY_HIT_MISS
- ownership: Test
- phase introduced: 3
```

Phase 5 additions:

```text
scene/3d/local_gi/local_gi_direct_light.h/.cpp
- collect Directional/Omni/Spot lights in volume local space; Godot-matching attenuation
- ownership: LocalGI
- phase introduced: 5

scene/3d/local_gi/local_gi_bvh.h/.cpp
- triangle/hit albedo
- ownership: LocalGI
- phase introduced: 1, updated 5

scene/3d/local_gi/local_gi_static_geometry.h/.cpp
- per-surface BaseMaterial3D albedo extraction
- ownership: LocalGI
- phase introduced: 1, updated 5

scene/3d/local_gi/local_gi_volume_3d.h/.cpp
- compute_one_bounce, probe irradiance/radiance accessors, irradiance debug draw
- ownership: LocalGI
- phase introduced: 0, updated 5

doc/classes/LocalGIVolume3D.xml
- one-bounce methods and DEBUG_PROBE_IRRADIANCE / DEBUG_RAW_PROBE_RADIANCE
- ownership: Godot Integration
- phase introduced: 0, updated 5

tests/scene/test_local_gi_one_bounce.cpp
- energy linearity, zero light/albedo, finite, red/green bleed
- ownership: Test
- phase introduced: 5

_local_gi_prototype/scripts/one_bounce_debug.gd
- Scene A bake + compute_one_bounce + DEBUG_PROBE_IRRADIANCE
- ownership: Test
- phase introduced: 5
```

---

# 18. Current Build / Test Commands

Phase 0 must replace placeholders with exact commands.

```text
Build Godot:
    python -m SCons platform=windows arch=x86_64 target=editor tests=yes -j8

Run LocalGIPrototype:
    bin/godot.windows.editor.x86_64.exe --editor --path F:/godot/_local_gi_prototype

Import / headless project load:
    bin/godot.windows.editor.x86_64.exe --headless --path F:/godot/_local_gi_prototype --import --quit

Run smoke test:
    bin/godot.windows.editor.x86_64.exe --headless --path F:/godot/_local_gi_prototype -s res://scripts/smoke_test.gd

Run unit tests:
    bin/godot.windows.editor.x86_64.exe --headless --test --test-case="*LocalGIVolume3D*"
    Phase 5: 27 passed / 1061 assertions

Run GPU comparison tests:
    bin/godot.windows.editor.x86_64.exe --headless --test --test-case="*LocalGIVolume3D*"
    included in the LocalGIVolume3D suite above

Run benchmark:
    N/A (Phase 12)
```

Do not rely on chat history for commands.

---

# 19. Current Commit / Revision

```text
Working branch:
    feature/hddagi-4.7/local-dynamic-gi

HEAD commit:
    318e6c6343
    LocalGI: add one-bounce Lambertian probe irradiance.

Base commit:
    5b4e0cb0fd279832bbdd69fed5354d4e5ad26f88
    4.7-stable (merge-base with origin/master)

HDDAGI revision:
    ab154bfd170dce1047ec4b2842c0fc1be31a90ff
    (hddagi-4.7 tip; Phase 0–2 LocalGI commits are on top)

Dirty working tree:
    No — Phase 5 committed as 318e6c6343
```

Update every Phase.

---

# 20. Reference / License Ledger

Only record sources actually used for implementation.

| Source | Component Referenced | License | Code Copied/Adapted? | Notice Required? | Notes |
|---|---|---|---|---|---|
| Godot | VoxelGI node/bake/register pattern | MIT | Adapted structure, no copied bake/voxelizer | Yes | Phase 0 skeleton only |
| Godot | VoxelGI::_find_meshes eligibility + Mesh::get_faces | MIT | Adapted collection, emits triangles not voxels | Yes | Phase 1 |
| Godot | Geometry3D::ray_intersects_triangle | MIT | Called on CPU; GLSL ports the same Möller–Trumbore test | Yes | Phase 1 CPU, Phase 3 GPU |
| Godot | AABB::find_intersects_ray | MIT | GLSL ports the slab test | Yes | Phase 3 GPU |
| Godot | LightmapperRD local RD + RDShaderFile | MIT | Adapted device/shader compile pattern, no lightmap code | Yes | Phase 3 |
| Fibonacci sphere / golden spiral | Probe ray directions | Public math | Independent implementation, not copied | No | Phase 4 |
| Godot | scene_forward_lights_inc.glsl get_omni_attenuation + spot rim | MIT | Reimplemented in C++, not copied shader text | Yes | Phase 5 |
| Wicked Engine | unused | MIT | No | No | Not used in Phase 1–5 |

Rules:

```text
- Do not copy proprietary SDK code.
- Do not copy code with unclear license.
- Do not treat RTXGI implementation as copy source.
- Record exact file/source if code is adapted.
```

---

# 21. Human Verification Queue

Only put tasks here when AI has completed all non-visual validation.

Current:

```text
None. Phase 5 human visual PASS. Do not start Phase 6 until asked.
```

Last completed visual task:

```text
Phase: 5
Scene: _local_gi_prototype / A Cornell Baseline probe irradiance spheres
Human result: PASS
Human notes: 可以了。Directionality visible (red/green walls). Alpha holes were a debug transparency bug, now opaque.
```

Format:

```text
Phase:
Scene:
Steps:
Expected observation:
Human result: PENDING / PASS / FAIL
Human notes:
```

AI must not set Human result to PASS itself.

---

# 22. Last Completed Phase Summary

Current:

```text
What was implemented:
    One-bounce Lambertian probe irradiance from direct lights + BVH shadows
Files changed:
    scene/3d/local_gi/local_gi_direct_light.*,
    scene/3d/local_gi/local_gi_bvh.*,
    scene/3d/local_gi/local_gi_static_geometry.*,
    scene/3d/local_gi/local_gi_volume_3d.*,
    editor/scene/3d/gizmos/local_gi_volume_3d_gizmo_plugin.cpp,
    doc/classes/LocalGIVolume3D.xml,
    tests/scene/test_local_gi_one_bounce.cpp,
    _local_gi_prototype/scripts/one_bounce_debug.gd,
    _local_gi_prototype/scenes/a_cornell_baseline.tscn,
    _local_gi_prototype/scripts/smoke_test.gd,
    LOCAL_GI_STATE.md
Automated tests:
    doctest LocalGIVolume3D 27 passed / 1061 assertions
    prototype smoke A–H passed; Scene B mean irradiance > 0
Human result:
    PASS — 可以了
Known limitations:
    No temporal / multi-bounce / visibility interpolation / final shading
    Missed rays return 0 (no GlobalIndirectCache yet)
    Lighting evaluation is CPU-only
    Debug spheres show raw incoming radiance; they are darker than directly lit walls
Important measurements:
    White-room energy tests are exact within 3% relative (linear in light energy and albedo)
Architecture impact:
    Radiometric contract: outgoing radiance = albedo / π * direct irradiance
    Probe spherical irradiance = (4π / N) * Σ incoming radiance
```

After each Phase keep only:

```text
What was implemented:
Files changed:
Automated tests:
Human result:
Known limitations:
Important measurements:
Architecture impact:
```

Do not retain long implementation narrative.

---

# 23. Current Phase Entry Conditions

For Phase 0 (already satisfied):

```text
[x] PLAN.md read completely
[x] STATE.md read completely
[x] repository root identified
[x] current branch/revisions identified
```

For Phase 1 (satisfied):

```text
[x] Phase 0 human visual PASS
[x] STATE Last Completed Phase == Phase 0
[x] LocalGIVolume3D class exists
[x] do not implement GPU traversal or probes
```

For Phase 2 (satisfied):

```text
[x] Phase 1 CPU geometry query PASS
[x] STATE Last Completed Phase == Phase 1
[x] do not implement GPU traversal or probes
```

For Phase 3 (satisfied):

```text
[x] Phase 2 CPU dynamic query PASS
[x] STATE Last Completed Phase == Phase 2
[x] do not implement probes or shading
```

For Phase 4 (satisfied):

```text
[x] Phase 3 human visual PASS
[x] STATE Last Completed Phase == Phase 3
[x] do not implement GI / shading / irradiance
```

For Phase 5 (satisfied except human visual):

```text
[x] Phase 4 human visual PASS
[x] STATE Last Completed Phase == Phase 4
[x] do not implement temporal or multi-bounce until those phases
```

---

# 24. Current Phase Exit Conditions

Phase 0 is complete only when:

```text
[x] relevant renderer paths recorded
[x] VoxelGI reference integration recorded
[x] RenderingDevice patterns recorded
[x] mesh extraction path recorded
[x] light/shadow resource paths recorded
[x] HDDAGI cache structure/sampling feasibility recorded
[x] LocalGI ownership boundary recorded
[x] GlobalIndirectCache adapter boundary proposed
[x] LocalGIPrototype project created
[x] Cornell Scene A–H skeletons created
[x] project loads successfully
[x] LocalGIVolume3D skeleton can exist in a scene
[x] automated smoke tests pass
[x] human confirms project/scenes open
[x] STATE.md updated
```

Phase 1 is complete only when:

```text
[x] static meshes collected
[x] triangles converted to volume local space
[x] CPU Static BVH built
[x] triangle extraction tests pass
[x] local-space transform tests pass
[x] direct / miss / nearest / parallel / edge hit tests pass
[x] thin wall 5/10/15/20 cm tests pass
[x] deterministic BVH build tests pass
[x] no GPU traversal or probes implemented
[x] STATE.md updated
```

Phase 2 is complete only when:

```text
[x] dynamic contributors discovered
[x] relevant transform/mesh change marks dirty
[x] stationary object does not rebuild continuously
[x] static BVH remains unchanged across dynamic rebuild
[x] dynamic BVH rebuild changes hit result
[x] removing a dynamic object updates BVH
[x] static + dynamic nearest hit is correct
[x] no GPU traversal or probes implemented
[x] STATE.md updated
```

Phase 3 is complete only when:

```text
[x] static/dynamic CPU BVHs uploaded to GPU buffers
[x] GPU software traces static and dynamic trees
[x] GPU chooses nearest hit (static on ties)
[x] CPU/GPU hit/miss match
[x] CPU/GPU nearest hit / distance / normal match within tolerance
[x] no probes or shading implemented
[x] human confirms Cornell Thin Wall ray/hit debug
[x] STATE.md updated
```

Phase 4 is complete only when:

```text
[x] regular local probe grid built
[x] probe count exact
[x] local positions exact
[x] directions normalized and deterministic
[x] ray budget exact
[x] volume transform does not change local layout
[x] probe rays can be GPU-traced
[x] no GI / shading implemented
[x] human confirms probe grid and selected rays, including move/rotate
[x] STATE.md updated
```

Phase 5 is complete only when:

```text
[x] triangle albedo extracted
[x] scene lights collected in volume local space
[x] one-bounce Lambertian radiance computed at probe-ray hits
[x] probe spherical irradiance integrated
[x] zero light → zero GI
[x] zero albedo → zero reflected contribution
[x] light ×2 → GI approximately ×2
[x] albedo ×0.5 → reflected contribution approximately ×0.5
[x] no NaN/Inf
[x] human confirms Cornell Baseline light direction and color bleeding
[x] no temporal or multi-bounce implemented
[x] STATE.md updated
```

After Phase 5 human PASS set:

```text
Last Completed Phase: Phase 5 — One-Bounce Local GI
Current Phase: Phase 6 — Visibility + Final Local Shading
Status: NOT STARTED
```

Then STOP. Do not enter Phase 6 in the same context.

---

# 25. Compaction Handoff Checklist

Before every compact:

```text
[x] Current Phase accurate (Phase 6 NOT STARTED)
[x] Last Completed Phase accurate
[x] HEAD/base revisions recorded
[x] changed files recorded
[x] implemented features updated
[x] automated test results recorded
[x] human result recorded if required
[x] known issues recorded
[x] decisions recorded
[x] license ledger updated
[x] exact next entry conditions recorded
[x] no important fact exists only in chat
```

If any item is missing:

> Update STATE.md before compaction.

---

# 26. Instruction for the Next AI Context

After compaction:

1. Read `PLAN.md` completely.
2. Read `STATE.md` completely.
3. Treat repository code as the primary factual source.
4. Verify only the minimum critical repository facts needed for the current Phase.
5. Execute only `Current Phase`.
6. Do not redesign frozen architecture.
7. Do not implement future-phase optimizations.
8. Run static/unit/automated verification.
9. If human visual verification is required, stop and request it.
10. After human feedback, update STATE.
11. When the Phase is complete, update STATE and stop.
12. Do not enter the next Phase in the same context.
