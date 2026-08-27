# Local LRT Development State

> 本文件是执行 AI 的唯一跨会话进度记录。
>
> 每次开始工作：先读 `LOCAL_LRT_PLAN.md`，再完整读取本文件。
>
> 每次结束工作：必须更新本文件。不要把临时聊天摘要当作事实来源。

---

# 1. Project Status

```text
Project: Local LRT Volume for Godot 4.7
Plan: LOCAL_LRT_PLAN.md
Current Phase: V0.1 — `LocalLRTVolume3D` + RID + Probe Gizmo
Current Status: WAITING_HUMAN_VISUAL_VALIDATION
Last Completed Phase: P0.3 — 测试项目与 Cornell Box
Human Visual Validation: REQUIRED — WAITING FOR USER
```

```text
Repository: https://github.com/MouthsheepZZZ/godot.git
Branch: feature/hddagi-4.7/local-lrt-volume-3d
Base / Upstream: origin
Last Known Commit: 848e20f6c8
```

---

# 2. Frozen Project Decisions

- 最小侵入 Godot 主线，方便长期 rebase。
- 优先新增独立 Local LRT 子系统；现有 GI 只做薄接入。
- GI 工作在 `LocalLRTVolume3D` Local Space。
- Editor / Runtime 共用实现。
- v0 = 静态 Geometry + 动态解析灯光。
- v1 = 动态物体。
- v2 = Global GI 注入。
- v3 = 多 Volume + Priority / Blend。
- v4 才允许性能优化。
- v0 使用完整 26 邻居 reference 路径。
- 首版只做 Diffuse GI。
- SH2 = `Vector4`，顺序为 `[Y00, Y1x, Y1y, Y1z]`。
- SH basis 使用正交归一化常数 `Y00 = 0.28209479177387814`、`Y1 = 0.4886025119029199`。
- SH 系数按 `f(direction) = dot(coefficients, basis(direction))` 求值。
- Local Transfer Matrix 为 row-major，`output = B * input`。
- 令 `D` 将 World SH 转到 Local SH，则 `B_world = D^T * B_local * D`。
- Local Visibility 表示可见比例：`1 = fully visible`，`0 = blocked`，不得作为 occlusion 使用。
- 26 邻居按 z-major 的 `[-1, 1]^3` 顺序枚举并跳过中心，使用归一化 inverse-distance 权重。
- 空空间通过独立 transmission 项继续传递 Radiance；表面通过 transfer matrix 反射；decay `< 1` 保证无注入时衰减。
- 视觉验证必须由人类完成。
- 数学 / 算法机制使用自动单元测试。
- 如无必要勿增实体；先看到效果，再优化。
- Cornell Box 是长期主测试关卡。

---

# 3. Current Phase

## V0.1 — `LocalLRTVolume3D` + RID + Probe Gizmo

Status: `WAITING_HUMAN_VISUAL_VALIDATION`

### Objective

实现最小 `LocalLRTVolume3D` 节点、RenderingServer RID 生命周期、派生 Probe Grid 与 Editor Gizmo。

### Required Work

- [x] 实现并注册 `LocalLRTVolume3D` 最小节点 API。
- [x] 实现 Local AABB、派生 resolution / actual spacing / probe positions。
- [x] 实现 RenderingServer RID 创建、释放与 Transform / property 同步。
- [x] 实现 Bounds + Probe Sphere Editor Gizmo。
- [x] 将 Cornell Box 占位节点替换为真实 `LocalLRTVolume3D`。
- [x] 添加创建 / 删除 / 保存 / 加载、网格和 RID 生命周期自动验证。

### Human Visual Validation

Required. 自动验证完成后必须等待用户确认 Bounds、Probe Grid 对齐及旋转后的 Gizmo。

---

# 4. Known Godot Entry Points

```text
CPU / Math:
  scene/3d/local_lrt_math.h
  scene/3d/local_lrt_builder.h
  scene/3d/local_lrt_builder.cpp

Tests:
  tests/scene/test_local_lrt_math.cpp
  tests/scene/test_local_lrt_builder.cpp

Scene Node:
  scene/3d/local_lrt_volume_3d.h
  scene/3d/local_lrt_volume_3d.cpp
  scene/register_scene_types.cpp

RendererRD:
  servers/rendering/renderer_rd/environment/local_lrt.h
  servers/rendering/renderer_rd/environment/local_lrt.cpp
  servers/rendering/renderer_rd/shaders/environment/local_lrt_*.glsl

Rendering / Cull:
  servers/rendering/rendering_server.h
  servers/rendering/rendering_server.cpp
  servers/rendering/rendering_server_default.h
  servers/rendering/rendering_server_enums.h
  servers/rendering/renderer_scene_cull.h
  servers/rendering/renderer_scene_cull.cpp
  servers/rendering/renderer_scene_render.h
  servers/rendering/renderer_rd/renderer_scene_render_rd.*

Forward:
  servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.*
  servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl

Editor Gizmo:
  editor/scene/3d/gizmos/local_lrt_volume_3d_gizmo_plugin.h
  editor/scene/3d/gizmos/local_lrt_volume_3d_gizmo_plugin.cpp
  editor/scene/3d/node_3d_editor_plugin.cpp

Test Project:
  local_lrt_volume_misc/test_project/
```

---

# 5. Completed Phases

## P0.3 — 测试项目与 Cornell Box

Status: COMPLETED
Commit: `848e20f6c8`
Date: 2026-08-27

Implemented:
- 创建长期复用 Cornell Box 测试项目、基础材质、emission、三种解析灯光与运行时控制。
- 添加非几何 Sprite3D 灯光标记、方向光方向提示及选中灯光隔离。
- 通过 MCP 验证场景层级、运行时加载、灯光移动与 DirectionalLight 旋转。

Human Visual Validation:
- PASS — 用户要求继续下一阶段开发。

## P0.1 — 冻结数学与传播约定

Status: COMPLETED
Commit: `5bd61eff6e`
Date: 2026-08-27

Implemented:
- 冻结 SH2 basis、encode/evaluate、truncated triple product 与旋转规则。
- 冻结 row-major Local Transfer Matrix 及 `D^T * B * D` 旋转规则。
- 冻结 Local / World / Grid / UVW 与 Probe index 转换。
- 冻结 26 邻居枚举和归一化 inverse-distance 权重。
- 冻结 visibility 与 radiance recurrence，包括空空间 continuation 和 decay。

Files Changed:
- `scene/3d/local_lrt_math.h`
- `tests/scene/test_local_lrt_math.cpp`

Tests:
- command: `python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6`
  result: PASS
- command: `bin/godot.windows.editor.dev.x86_64.console.exe --test --test-case="*[LocalLRTMath]*" --no-colors`
  result: PASS — 9 test cases, 181 assertions

Human Visual Validation:
- NOT REQUIRED
- N/A

Frozen Interfaces / Formats:
- `LocalLRTMath::SH2Matrix`
- SH2 `[Y00, Y1x, Y1y, Y1z]`
- `output = B * input`
- `B_world = D^T * B_local * D`
- Local Visibility uses visible fraction semantics.

## P0.2 — CPU 最小 Reference Solver

Status: COMPLETED
Commit: `41c08a9da0`
Date: 2026-08-27

Implemented:
- 规则 CPU Probe Grid 与人工 occupancy / albedo / emission 输入。
- 基于完整 26 邻域的 Local Visibility 和 RGB Local Transfer Matrix 构建。
- Global Visibility 与 RGB Radiance ping-pong 传播。
- Directional、Omni、Spot 解析灯光注入及 Local-space 转换。
- 空网格、白墙、红墙、红白夹角、封闭盒、开口盒、隔墙点光源及整体旋转解析场景。
- 固定 canonical red-wall GPU golden reference 数值。

Files Changed:
- `scene/3d/local_lrt_builder.h`
- `scene/3d/local_lrt_builder.cpp`
- `tests/scene/test_local_lrt_builder.cpp`

Tests:
- command: `python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6`
  result: PASS
- command: `bin/godot.windows.editor.dev.x86_64.console.exe --test --test-case="*[LocalLRTMath]*,*[LocalLRTBuilder]*" --no-colors`
  result: PASS — 17 test cases, 211 assertions

Human Visual Validation:
- NOT REQUIRED
- N/A

Frozen Interfaces / Formats:
- `LocalLRTBuilder::Probe`, `SH2RGB`, and `TransferRGB` are the CPU reference data layout.
- Empty/out-of-grid visibility is fully visible; out-of-grid radiance is zero.
- Local transfer uses per-channel reflectance with continuation plus transfer bounded by neighbor weights.
- Omni attenuation is `(1 - distance / range)^2`; Spot additionally applies squared normalized cone attenuation.
- Golden red-wall center after 4 iterations: visibility X `1.06501`, radiance R X `3.40430`, radiance G X `2.96087`.

---

# 6. Current Working Set

```text
Files Read:
- local_lrt_volume_misc/LOCAL_LRT_PLAN.md
- local_lrt_volume_misc/LOCAL_LRT_STATE.md
- local_lrt_volume_misc/test_project/cornell_box.tscn

Files Modified:
- scene/3d/local_lrt_volume_3d.h
- scene/3d/local_lrt_volume_3d.cpp
- servers/rendering/renderer_rd/environment/local_lrt.h
- servers/rendering/renderer_rd/environment/local_lrt.cpp
- editor/scene/3d/gizmos/local_lrt_volume_3d_gizmo_plugin.h
- editor/scene/3d/gizmos/local_lrt_volume_3d_gizmo_plugin.cpp
- tests/scene/test_local_lrt_volume_3d.cpp
- local_lrt_volume_misc/test_project/cornell_box.tscn

Relevant Symbols / Functions:
- LocalLRTVolume3D
- RendererRD::LocalLRT
- LocalLRTVolume3DGizmoPlugin

Reference Implementations Consulted:
- VoxelGI node and gizmo
- RendererGI / RenderingServer split RID API
```

---

# 7. Build / Test Commands

```text
Build:
python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6

Unit Tests:
bin/godot.windows.editor.dev.x86_64.console.exe --test --test-case="*[LocalLRTMath]*,*[LocalLRTBuilder]*,*[LocalLRTVolume3D]*" --no-colors

Test Project Run:
bin/godot.windows.editor.dev.x86_64.console.exe --headless --path local_lrt_volume_misc/test_project --quit-after 2

Renderer / Debug Capture:
Godot MCP editor_screenshot(source="viewport") with LocalLRTVolume3D selected
```

---

# 8. Latest Verification

```text
Compile: PASS
Unit Tests: PASS — 20 test cases, 233 assertions
Runtime Smoke Test: PASS — Cornell Box loaded with the real LocalLRTVolume3D; Forward+ runtime RID valid; resolution 10×7×10
Editor Gizmo Capture: PASS — bounds and probe grid captured unrotated and at Y=30°
Human Visual Validation: WAITING FOR USER
```

Notes:
- Initial build without `accesskit=no d3d12=no` stopped because optional local SDK dependencies were absent; the recorded build command disables those unrelated drivers and passes.

---

# 9. Known Issues / Deferred

- `--headless --editor --quit` reaches editor initialization, then this custom engine build crashes in `EditorNode::is_cmdline_mode` with a null singleton. Runtime headless loading succeeds without errors.

---

# 10. Blockers / Decisions Needed

- 等待用户人工确认 Cornell Box 中 Bounds / Probe Grid 对齐，且旋转 Volume 后 Gizmo 方向正确。
- 用户确认 PASS 前不得完成 V0.1 或进入 V0.2。

---

# 11. Next Action

```text
Ask the user to select LocalLRTVolume3D in cornell_box.tscn and validate bounds/probe alignment and rotated gizmo orientation.
```

---

# 12. Session Handoff

```text
Last Session Summary:
Implemented V0.1 LocalLRTVolume3D, RenderingServer/RD RID storage, probe-grid math, tests, and editor gizmo.

Current Phase:
V0.1 — LocalLRTVolume3D + RID + Probe Gizmo

Current Status:
WAITING_HUMAN_VISUAL_VALIDATION

What Was Completed:
- Registered LocalLRTVolume3D with the required minimal API.
- Added independent RendererRD::LocalLRT RID storage and RenderingServer forwarding.
- Added bounds/probe gizmo and replaced the Cornell Box placeholder node.
- Added grid, scene serialization, and RID lifecycle tests.

Tests Run:
- python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6
- bin/godot.windows.editor.dev.x86_64.console.exe --test --test-case="*[LocalLRTMath]*,*[LocalLRTBuilder]*,*[LocalLRTVolume3D]*" --no-colors
- bin/godot.windows.editor.dev.x86_64.console.exe --headless --path local_lrt_volume_misc/test_project --quit-after 2
- Godot MCP runtime RID/grid query and editor gizmo captures.

Test Results:
- Compile PASS; 20 cases / 233 assertions PASS; runtime smoke PASS.
- Forward+ runtime RID valid; resolution 10×7×10; rotated gizmo capture succeeded.

Human Visual Validation:
- Waiting for explicit user PASS on bounds/probe alignment and rotated gizmo orientation.

Known Issues / Deferred:
- GL Compatibility keeps a stub Local LRT RID; GPU Local LRT stages require Forward+.

Exact Next Step:
- User selects LocalLRTVolume3D in cornell_box.tscn and validates the gizmo before V0.2.

Last Commit:
a00dea4874 Complete Cornell Box validation
```
