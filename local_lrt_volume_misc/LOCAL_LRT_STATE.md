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
Current Phase: V0.6 — Forward Surface Sampling + Edge Blend
Current Status: WAITING_HUMAN_VISUAL_VALIDATION
Last Completed Phase: V0.5 — Radiance Propagation Compute
Human Visual Validation: REQUIRED — WAITING FOR USER
```

```text
Repository: https://github.com/MouthsheepZZZ/godot.git
Branch: feature/hddagi-4.7/local-lrt-volume-3d
Base / Upstream: origin
Last Known Commit: 0adf8b0bfd
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
- V0.6 表面只消费 indirect Radiance；解析灯直接 Injection 仅作为传输种子，不得在 Base Pass 中重复叠加。
- `visibility_iterations` 与 `propagation_iterations` 独立；后者只控制 Radiance，修改它不得重算解析灯 Injection。
- Radiance decay 按世界米计算，不再按 Probe hop 固定衰减。

---

# 3. Current Phase

## V0.6 — Forward Surface Sampling + Edge Blend

Status: `WAITING_HUMAN_VISUAL_VALIDATION`

### Objective

在 Forward GI 路径中以最薄接入采样 Local LRT Radiance：World position / normal 转 Volume Local，三线性采样 RGB SH2，按表面法线求漫反射，并在 Volume 边缘平滑衰减。

### Required Work

- [x] 实现 Volume Bounds 与 World → Local → Grid UVW。
- [x] 将当前 Radiance buffer 绑定到 Forward shader 并三线性采样 RGB SH2。
- [x] 按 World surface normal 以 clamped-cosine SH convolution 求 Local LRT diffuse indirect。
- [x] 实现 `edge_blend_distance` 边缘权重。
- [x] 无有效 LocalLRTVolume 时 shader contribution 为零。
- [x] 添加数学 / Forward+ framebuffer 自动验证并更新 Cornell Box 控制。
- [x] 将解析灯 direct transport 与 surface indirect Radiance 分离，避免 Base Pass 重复直接光。
- [x] 将 Visibility 与 Radiance 迭代解耦；解析灯使用 Occupancy Grid transmittance，不再由传播中的 Global Visibility 调制 Injection。
- [x] 将 Radiance 衰减改为按实际邻居世界距离计算。
- [x] 将细网格表面采样改为 occupied-aware cubic B-spline，消除三线性分段梯度条纹。

### Human Visual Validation

Required — WAITING FOR USER。细网格条纹修正已完成自动验证和 AI 辅助截图检查，但视觉 PASS 只能由用户确认。请使用 `probe_spacing=0.25`、`visibility_iterations=4`、`propagation_iterations=16`、`energy=1`、`edge_blend_distance=0`，按 `V` 隐藏 Probe Debug、按 `G` 对比 Local GI 开关；确认物体表面不再出现 Probe 网格条纹、提高 Radiance 迭代不再整体变暗、红/绿 bleeding 与暗部间接照明合理、Volume 外无 Local GI，再检查 Edge Blend。

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

## V0.5 — Radiance Propagation Compute

Status: COMPLETED
Date: 2026-08-27

Implemented:
- RGB SH2 Radiance A/B ping-pong propagation，集成 Injection、Global / Local Visibility、Local Transfer、empty-space transmission 与 decay。
- GPU 1 / 2 / 4 / 8 iterations 与独立 CPU recurrence 数值一致。
- Radiance RGB Probe Debug 与解析灯光隔离控制。

Human Visual Validation:
- PASS — 用户确认当前 Radiance 数据与调试显示语义符合预期，同意进入 V0.6；Directional 精确阴影仅在最终表面采样出现可见泄漏时再评估。

## V0.4 — 动态解析灯光 Injection

Status: COMPLETED
Commits: `177b65ffd1`, `2fdbae05d8`, `b31819b052`, `9ea40cc321`
Date: 2026-08-27

Implemented:
- 每帧收集可见 Directional / Omni / Spot 解析灯光并转换到 Volume Local Space。
- 灯光 transform、color、energy、range、spot angle 与 visibility 变化只更新 RGB SH2 Injection，不重建静态 Geometry。
- 添加 Injection GPU upload/readback、运行时 Probe MultiMesh 与 depth-tested alpha scissor。

Tests:
- Compile PASS。
- Unit tests PASS — 23 test cases, 629 assertions。
- GPU Injection Validation PASS — 81 个 RGB SH2 值上传/readback 与 clear 正确。
- Runtime Dynamic Injection PASS — 移动/关闭 Omni 改变 Probe Injection，静态 Geometry count 不变。

Human Visual Validation:
- PASS — 用户确认 Omni / Spot Injection 渐变正确；Directional 在当前无阴影 Injection 阶段保持空间均匀符合预期；Probe 深度遮挡正确。

## V0.3 — GPU Resources + Global Visibility Compute

Status: COMPLETED
Commits: `8d862744e6`, `36fb1f3524`
Date: 2026-08-27

Implemented:
- 为每个 Local LRT Volume 建立 Local Visibility、Local Transfer、Global Visibility A/B、Radiance A/B 与 Injection storage buffers。
- 通过 RenderingServer 薄 API 上传 CPU-built Local Visibility / Local Transfer 数据。
- 实现完整 26-neighbor SH2 Global Visibility gather、out-of-grid fully-visible 语义、A/B ping-pong 与 iteration dispatch。
- 添加 GPU 1 / 2 / 4 / 8 iterations 对 CPU golden reference 验证、NaN / Inf 验证及传播方向符号验证。
- 添加 Global Visibility 灰度 Gizmo 模式并将 Cornell Box desktop renderer 切换为 Forward+。

Tests:
- Compile PASS。
- Unit tests PASS — 22 test cases, 621 assertions。
- GPU Visibility Validation PASS — Vulkan Forward Mobile，1 / 2 / 4 / 8 iterations 与 CPU 数值一致，27 probes finite。
- Runtime GPU Query PASS — Cornell Box `has_gpu_data=true`，resolution 10×7×10，Global Visibility SH2 readback finite。

Human Visual Validation:
- PASS — 用户确认 Local Visibility 与 Global Visibility 的传播显示无误，occupied Probe 保持洋红色。

## V0.2 — 静态 Geometry → Local Grid / Visibility / Transfer

Status: COMPLETED
Commits: `ec9a97c794`, `9b4bdd6f7f`
Date: 2026-08-27

Implemented:
- 收集 Volume 范围内可见的静态 `MeshInstance3D`，将三角形转入 Volume Local Space 并栅格化到 Probe Grid。
- 提取 `StandardMaterial3D` albedo / emission，复用 CPU reference builder 生成 Local Visibility 与 RGB Local Transfer。
- 添加 Occupancy、灰度 Local Visibility 与实际 RGB Local Transfer 三种独立 Gizmo 调试模式。
- 添加 Wall / CPU reference、颜色通道、空 Probe、Cube 与旋转 Local-space 自动测试。

Tests:
- Compile PASS。
- Unit tests PASS — 22 test cases, 621 assertions。
- Cornell Box runtime smoke PASS；收集 8 个静态 MeshInstance3D，resolution 为 10×7×10。

Human Visual Validation:
- PASS — 用户确认 Occupancy、Local Visibility 与 Local Transfer 调试显示符合 Cornell Box Geometry；白墙 Transfer 保持中性灰白。

## V0.1 — `LocalLRTVolume3D` + RID + Probe Gizmo

Status: COMPLETED
Commit: `6e08c69ffb`
Date: 2026-08-27

Implemented:
- 注册最小 `LocalLRTVolume3D` API、Local AABB 与派生 Probe Grid。
- 添加独立 RendererRD Local LRT RID 存储及 RenderingServer 薄接入。
- 添加 Bounds + Probe Sphere Editor Gizmo，并替换 Cornell Box 占位节点。
- 添加网格、场景序列化及 RID 生命周期测试。

Tests:
- Compile PASS。
- Unit tests PASS — 20 test cases, 233 assertions。
- Cornell Box runtime smoke PASS；Forward+ RID 有效，resolution 为 10×7×10。

Human Visual Validation:
- PASS — 用户确认 Bounds / Probe Grid 对齐及旋转 Gizmo 正确，并要求上传后继续 V0.2。

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
- Golden red-wall center after 4 iterations: visibility X `1.06501`, indirect radiance R X `0.445574`, indirect radiance G X `0.0535885`.

---

# 6. Current Working Set

```text
Files Modified:
- scene/3d/local_lrt_math.h
- scene/3d/local_lrt_builder.h
- scene/3d/local_lrt_builder.cpp
- scene/3d/local_lrt_volume_3d.h
- scene/3d/local_lrt_volume_3d.cpp
- tests/scene/test_local_lrt_math.cpp
- tests/scene/test_local_lrt_builder.cpp
- tests/scene/test_local_lrt_volume_3d.cpp
- servers/rendering/environment/renderer_gi.h
- servers/rendering/renderer_rd/environment/local_lrt.h
- servers/rendering/renderer_rd/environment/local_lrt.cpp
- servers/rendering/renderer_rd/environment/gi.h
- servers/rendering/renderer_rd/environment/gi.cpp
- servers/rendering/renderer_rd/shaders/environment/local_lrt_radiance.glsl
- servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl
- servers/rendering/rendering_server.h
- servers/rendering/rendering_server.cpp
- servers/rendering/rendering_server_default.h
- local_lrt_volume_misc/test_project/gpu_visibility_validation.gd
- local_lrt_volume_misc/test_project/gpu_injection_validation.gd
- local_lrt_volume_misc/test_project/gpu_radiance_validation.gd

Relevant Symbols / Functions:
- LocalLRTMath::radiance_distance_decay
- LocalLRTMath::cubic_bspline_weights
- LocalLRTBuilder::_is_unoccluded
- RendererRD::LocalLRT::_reset_and_propagate_radiance
- LocalLRTVolume3D::set_visibility_iterations
- local_lrt_sample_sh
- local_lrt_compute
```

---

# 7. Build / Test Commands

```text
Build:
python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6

Unit Tests:
bin/godot.windows.editor.dev.x86_64.console.exe --test --test-case="*[LocalLRTMath]*,*[LocalLRTBuilder]*,*[LocalLRTVolume3D]*" --no-colors

GPU Visibility Validation:
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method mobile --rendering-driver vulkan --script res://gpu_visibility_validation.gd

GPU Injection Validation:
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method mobile --rendering-driver vulkan --script res://gpu_injection_validation.gd

GPU Radiance Validation:
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method mobile --rendering-driver vulkan --script res://gpu_radiance_validation.gd

Forward Surface Validation:
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method forward_plus --rendering-driver vulkan --script res://forward_surface_validation.gd

Test Project Run:
bin/godot.windows.editor.dev.x86_64.console.exe --headless --path local_lrt_volume_misc/test_project --quit-after 2

Renderer / Debug Capture:
Godot MCP editor_screenshot(source="viewport") with LocalLRTVolume3D selected
```

---

# 8. Latest Verification

```text
Compile: PASS
Unit Tests: PASS — 29 test cases, 660 assertions
GPU Visibility Validation: PASS — Vulkan Forward Mobile; 1/2/4/8 iterations matched pinned CPU values
GPU Injection Validation: PASS — 81 RGB SH2 values uploaded/read back exactly and clear returned zero
GPU Radiance Validation: PASS — Vulkan Forward Mobile; 1/2/4/8 iterations and all 81 RGB SH2 values matched independent CPU recurrence; finite and red-transfer checks passed
Runtime Smoke Test: PASS — Forward+ and Dummy/headless Cornell Box loaded without errors
Runtime Dynamic Radiance: PASS — moving Omni changed center Probe radiance; has_gpu_data=true
Runtime Radiance Capture: PASS — Directional-only and Omni-only captures completed; spherical Probe surface lobes visibly rotate with Directional SH, and Global Visibility SH modulates Injection without runtime Trace
Directional Isolation Validation: PASS — residual Radiance was traced to the EmissionPanel (max R SH length 1.34666); with all sources disabled it is exactly zero. Analytic-light isolation now disables the panel emission and rebuilds Local LRT data.
Forward Surface Validation: PASS — Forward+ Vulkan framebuffer changes with indirect-only Local LRT (`full=0.04888408`), and large edge blend reduces contribution (`blended=0.00045171`); surface sampling uses one-cell normal bias, occupied rejection and cubic B-spline weights.
Forward+ Runtime Binding: PASS — shader/UBO/storage binding initialized without Local LRT uniform errors.
Fine Grid Rebuild: PASS — runtime `probe_spacing=0.25` rebuilt resolution `35×23×35` with valid CPU/GPU data；修复 Inspector grid property 修改只清空、不重建的问题。
Human Visual Validation: REQUIRED — WAITING FOR USER
```

Notes:
- Initial build without `accesskit=no d3d12=no` stopped because optional local SDK dependencies were absent; the recorded build command disables those unrelated drivers and passes.
- Test mode always initializes `RasterizerDummy`, so actual GPU validation runs as a separate deterministic Forward Mobile Vulkan script against pinned CPU-reference values.
- Cornell Box desktop renderer was changed from GL Compatibility to default Forward+ because Local LRT GPU resources require RenderingDevice.

---

# 9. Known Issues / Deferred

- `--headless --editor --quit` reaches editor initialization, then this custom engine build crashes in `EditorNode::is_cmdline_mode` with a null singleton. Runtime headless loading succeeds without errors.
- GL Compatibility retains no-op Local LRT storage; GPU compute and Global Visibility debug require Forward+ or Forward Mobile.
- Directional Injection 使用低频 Global Visibility 近似；仅当 V0.6 最终表面出现可见静态墙体漏光时，再评估独立 Directional transmittance sweep。
- `propagation_iterations` 仍是 Radiance Probe hop 数；减小 `probe_spacing` 时需同比提高它以维持传播距离，但每跳 decay 已按世界距离补偿，调整迭代不再连带改变 Visibility 或 Injection。
- Surface normal bias 当前固定为一个 Probe grid cell；后续若不同几何尺度出现漏光或接触变暗，再评估暴露为可调参数，当前不提前增加 API。
- occupied-aware cubic B-spline surface gather 使用 64 Probe taps，当前优先保证 V0.6 正确性；性能优化或 Screen Space Gather 统一留到 v4。

---

# 10. Blockers / Decisions Needed

- None.

---

# 11. Next Action

```text
Wait for the user to visually validate V0.6 in the Cornell Box: press V to hide probes, G to toggle Local GI, then inspect bleeding, dark areas, outside-volume behavior, and edge blend.
```

---

# 12. Session Handoff

```text
Last Session Summary:
Corrected V0.6 transport semantics and fine-grid artifacts: surface sampling now consumes indirect-only Radiance, analytic lights use occupancy transmittance, Visibility/Radiance iteration controls are separate, attenuation is world-distance based, and cubic sampling removes grid-aligned bands. Human visual revalidation is pending.

Current Phase:
V0.6 — Forward Surface Sampling + Edge Blend

Current Status:
WAITING_HUMAN_VISUAL_VALIDATION

What Was Completed:
- Exposed the first enabled v0 Local LRT volume's inverse transform, size, resolution, energy, edge blend distance, and current Radiance buffer to Forward+.
- Added Forward+ UBO/storage bindings with a zero-contribution inactive path.
- Added Local-space bounds checks, occupied-aware cubic RGB SH2 sampling, one-cell normal sampling bias, normal rotation, clamped-cosine diffuse convolution, energy scaling, and edge fade.
- Split analytic direct transport from surface indirect Radiance and preserved emissive injection in the indirect path.
- Added deterministic occupancy-grid light transmittance, separate Visibility/Radiance iterations, and world-distance decay.
- Added CPU transport/cubic/decay tests and deterministic GPU/Forward+ validation.
- Added G/V Cornell Box controls for Local GI and Probe Debug comparison.

Test Results:
- Compile PASS; 29 cases / 660 assertions PASS.
- GPU Visibility, Injection, and Radiance regression validations PASS.
- Forward+ Vulkan framebuffer validation PASS: full contribution 0.04888408; large edge blend contribution 0.00045171.
- Forward+ runtime starts without Local LRT uniform or shader binding errors.
- Grid properties now rebuild existing data; `0.25m` spacing produces valid `35×23×35` CPU/GPU data.

Human Visual Validation:
- REQUIRED — WAITING FOR USER.

Known Issues / Deferred:
- V0 supports one active Local LRT volume; multi-volume selection/blending remains v3.
- Forward Mobile surface sampling is not part of this Forward clustered phase.
- Directional transmittance sweep remains deferred unless final surfaces show unacceptable wall leakage.

Exact Next Step:
- Restart the rebuilt Forward+ editor; set Spacing 0.25, Visibility Iterations 4, Propagation Iterations 16, Energy 1 and Edge Blend 0. Press V/G and confirm the grid-aligned bands and iteration-driven global darkening are gone, then validate bleeding, dark areas, outside-volume behavior, and edge blend. Do not advance to V0.7 before explicit human PASS.
```
