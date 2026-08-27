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
Current Phase: P0.2 — CPU 最小 Reference Solver
Current Status: NOT STARTED
Last Completed Phase: P0.1 — 冻结数学与传播约定
Human Visual Validation: NOT REQUIRED YET
```

```text
Repository: https://github.com/MouthsheepZZZ/godot.git
Branch: feature/hddagi-4.7/local-lrt-volume-3d
Base / Upstream: origin
Last Known Commit: 5bd61eff6e
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

## P0.2 — CPU 最小 Reference Solver

Status: `NOT STARTED`

### Objective

使用 P0.1 冻结的约定，实现只用于算法验证的最小 CPU Probe Grid 与 golden reference。

### Required Work

- [ ] 规则 Probe Grid。
- [ ] 人工 occupancy / albedo 输入。
- [ ] Local Visibility。
- [ ] Local Transfer Matrix。
- [ ] Global Visibility ping-pong。
- [ ] Radiance ping-pong。
- [ ] Directional / Omni / Spot injection。
- [ ] 完整 26 邻居传播。
- [ ] 保存 GPU 对照所需关键数值。

### Required Tests

- [ ] 空网格。
- [ ] 单白墙。
- [ ] 单红墙。
- [ ] 红墙 + 白墙夹角。
- [ ] 封闭盒。
- [ ] 带开口盒。
- [ ] 点光源被墙分隔。
- [ ] Grid 整体旋转。
- [ ] 空空间传播。
- [ ] 墙附近 visibility 方向正确。
- [ ] 红墙 R transfer / bleeding 强于 G/B。
- [ ] 反照率 `< 1` 时能量稳定。
- [ ] Volume 旋转后 Local-space reference 一致。

### Human Visual Validation

Not required for this phase.

---

# 4. Known Godot Entry Points

```text
CPU / Math:
  scene/3d/local_lrt_math.h
  scene/3d/local_lrt_builder.h (planned for P0.2)
  scene/3d/local_lrt_builder.cpp (planned for P0.2)

Tests:
  tests/scene/test_local_lrt_math.cpp

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

---

# 6. Current Working Set

```text
Files Read:
- local_lrt_volume_misc/LOCAL_LRT_PLAN.md
- local_lrt_volume_misc/LOCAL_LRT_STATE.md
- local_lrt_volume_misc/lrt_ref_ai_summary.md
- tests/core/math/test_basis.cpp
- tests/SCsub

Files Modified:
- scene/3d/local_lrt_math.h
- tests/scene/test_local_lrt_math.cpp
- local_lrt_volume_misc/LOCAL_LRT_STATE.md

Relevant Symbols / Functions:
- LocalLRTMath::SH2Matrix
- LocalLRTMath::triple_product
- LocalLRTMath::rotate_to_world
- LocalLRTMath::rotate_transfer_to_world
- LocalLRTMath::propagate_visibility
- LocalLRTMath::propagate_radiance

Reference Implementations Consulted:
- tests/core/math/test_basis.cpp
- local_lrt_volume_misc/lrt_ref_ai_summary.md
```

---

# 7. Build / Test Commands

```text
Build:
python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6

Unit Tests:
bin/godot.windows.editor.dev.x86_64.console.exe --test --test-case="*[LocalLRTMath]*" --no-colors

Test Project Run:
NOT ESTABLISHED

Renderer / Debug Capture:
NOT ESTABLISHED
```

---

# 8. Latest Verification

```text
Compile: PASS
Unit Tests: PASS — 9 test cases, 181 assertions
Runtime Smoke Test: NOT REQUIRED FOR P0.1
Human Visual Validation: N/A
```

Notes:
- Initial build without `accesskit=no d3d12=no` stopped because optional local SDK dependencies were absent; the recorded build command disables those unrelated drivers and passes.

---

# 9. Known Issues / Deferred

- None.

---

# 10. Blockers / Decisions Needed

- None.

---

# 11. Next Action

```text
Read only the P0.2 requirements and the P0.1 math API/tests listed above.
Design the smallest CPU reference grid around LocalLRTMath.
Implement analytic fixtures and golden values without starting P0.3 or renderer integration.
```

---

# 12. Session Handoff

```text
Last Session Summary:
Completed P0.1 and froze the Local LRT SH2, transform, transfer, neighborhood, visibility, and radiance conventions.

Current Phase:
P0.2 — CPU 最小 Reference Solver

Current Status:
NOT STARTED

What Was Completed:
- Added header-only Local LRT math implementation.
- Added and passed all P0.1 required automatic tests.
- Committed implementation as 5bd61eff6e.

Files Changed:
- scene/3d/local_lrt_math.h
- tests/scene/test_local_lrt_math.cpp
- local_lrt_volume_misc/LOCAL_LRT_STATE.md

Tests Run:
- Full tests-enabled editor build.
- Filtered LocalLRTMath unit tests.

Test Results:
- Compile PASS.
- 9 test cases / 181 assertions PASS.

Human Visual Validation:
- Not required for P0.1.

Important Findings / Frozen Decisions:
- See section 2 and P0.1 completed-phase record.

Known Issues / Deferred:
- None.

Exact Next Step:
- Start P0.2 with a minimal CPU Probe Grid using scene/3d/local_lrt_math.h.

Last Commit:
5bd61eff6e Implement Local LRT math conventions
```
