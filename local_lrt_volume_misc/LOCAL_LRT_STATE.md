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
Current Phase: V0.8 — GPU Analytic Light Injection + Directional Shadow Visibility
Current Status: COMPLETE — Volume Directional Shadow Map
Last Completed Phase: V0.8 — GPU Analytic Light Injection + Directional Shadow Visibility
Human Visual Validation: V0.6 PASS；V0.7 PASS；Color SDF Volume + SampleDir LTM PASS；GPU / Forward inside_solid PASS；Color SDF spacing variance PASS。未遮挡 GPU Injection 无独立人工项。Directional Shadow PASS — 用户确认阴影已影响 GI。
```

```text
Repository: https://github.com/MouthsheepZZZ/godot.git
Branch: feature/hddagi-4.7/local-lrt-volume-3d
Base / Upstream: origin
Last Known Commit: Add volume directional shadow maps for Local LRT GI.
```

---

# 2. Frozen Project Decisions

- 最小侵入 Godot 主线，方便长期 rebase。
- 优先新增独立 Local LRT 子系统；现有 GI 只做薄接入。
- GI 工作在 `LocalLRTVolume3D` Local Space。
- Editor / Runtime 共用实现。
- v0 = 静态 Geometry + 动态解析灯光 + Directional / Omni / Spot Shadow-aware GPU Injection。
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
- V0.8 / V0.9 遵循原文 CPU / GPU 分工：CPU 保留 Local Visibility / Local Transfer / Emission 构建，正式 Runtime 解析灯 Injection 迁移到 GPU 并逐 Probe 采样 Shadow Map；CPU Injection 仅保留为 reference。
- V0.8 使用不依赖相机 CSM 的 Volume Directional Shadow Map；V0.9 复用同一 GPU Injection 路径完成 Omni / Spot Shadow Visibility。Editor Scene Viewport 与 Runtime 必须共用 `Shadow → Injection → Propagation → Forward` 路径。
- Global Visibility 只保留给后续天空遮蔽 / Global GI，不得充当 Directional / Omni / Spot Shadow Map。
- V0.6 只维护并消费单一 reflected Radiance 场；解析灯 Injection 仅作为当前 Probe 的 Local Transfer 入射光，不参与邻域传播或 Base Pass 直接叠加。
- `visibility_iterations` 与 `propagation_iterations` 独立；后者表示每帧继续执行的 Radiance Probe-hop 数，修改它不得清空 Radiance 或重算解析灯 Injection。
- Radiance decay 按世界米计算，不再按 Probe hop 固定衰减。
- V0.2 Gap Closure 按原文分离 per-object Local Geometry Source 与 Radiance Probe Grid：Geometry Resource 拥有独立 voxel size / resolution，`probe_spacing` 只决定 Radiance Probe 查询位置。
- 26 个 LTM 查询点为 `probe_center + neighbor_offset * actual_probe_spacing`，变换到 object local 后直接采样 Color SDF；`SampleBasis = sh_basis(SampleDir)`，`GetSH2PIDivDFT(d) = (Y00, Y1 * d * 2/3)`。
- 26 邻域 inverse-distance 权重之和为 1，再乘 `4π`；不得使用均匀 `4π / 26`。
- V0 LTM 为一次局部反弹；Neumann 无限反弹不作为 V0 通过条件。
- fractional coverage 只参与 Local Visibility / LTM 积分；只有 Probe center 合并 SDF `< 0` 时才为 `inside_solid`。不得再由 `coverage > 0` 派生二值 Radiance Probe 失效，也不得跳过表面 Probe 的 LTM 构建。
- 重叠 Geometry Source 取最小 signed distance；颜色 / emission / normal 来自胜出 Source。
- `ColorToFill = albedo + emission`；Geometry emission 经 LTM，删除 `emissive_injection` outgoing 旁路。
- Radiance gather 使用邻居 Local Visibility：`Trpd(otherRadiance, -otherLocalViSH)`。Global Visibility 不是 V0 通过条件。
- Forward V0 使用 cubic B-spline，只排除 `inside_solid`；不得用 Local Visibility 长度推断 occupied。cubic 核在 Probe index 空间，物理半径随 `actual_spacing` 缩放。
- 静态 Local Geometry / LTM 构建必须 deterministic；不得用 temporal/random dither 掩盖 first-bounce 误差。固定 seed stratified / blue-noise subcell sampling 仅可用于可重复数值积分。
- 原文的 4-neighbor / 3-frame pattern 与 temporal/spatial dither 保留到 v4，且必须以完整 deterministic 26-neighbor 作为 golden reference。

---

# 3. Current Phase

## V0.8 — GPU Analytic Light Injection + Directional Shadow Visibility

Status: `COMPLETE`.

### Objective

正式 Runtime 的 Directional / Omni / Spot SH Injection 迁到 GPU；CPU Injection 只作 golden reference。本子版本先做未遮挡 GPU Injection，与 V0.4 CPU reference 一致；随后再加 Volume Directional Shadow Map。Editor Scene Viewport 与 F5 共用同一路径。

### Required Work

- [x] GPU Analytic Light Injection compute：每个 Probe 用 Volume transform 恢复 World position，按冻结 SH 约定写入 RGB SH2；跳过 `inside_solid`。
- [x] GPU unshadowed Directional / Omni / Spot 与 CPU reference 一致。
- [x] 灯光或 Volume transform 变化只更新 Injection，不重建 Local Visibility / Local Transfer，不清空 Radiance history。
- [x] Volume Directional Shadow Map（独立于相机 CSM）；Caster 含 Volume 外能向 Volume 投影的静态物体。
- [x] `DirectionalLightSH × Shadow Visibility`；墙前 Injection 正常，墙后接近零；关阴影后回到 unshadowed CPU reference。
- [x] 顺序固定 `Shadow → Injection → Propagation → Forward`。
- [x] Debug：Directional Shadow Visibility、shadowed Directional Injection、reflected Radiance。

### Human Visual Validation

未遮挡 GPU Injection 应与当前画面一致，无独立人工项。Directional Shadow 完成后：Editor Viewport 无需 F5 即可见墙后间接漏光消失；移动相机不改变 GI。

---

# 4. Known Godot Entry Points

```text
CPU / Math:
  scene/3d/local_lrt_math.h
  scene/3d/local_lrt_builder.h
  scene/3d/local_lrt_builder.cpp
  scene/3d/local_lrt_color_sdf.h
  scene/3d/local_lrt_color_sdf.cpp

Tests:
  tests/scene/test_local_lrt_math.cpp
  tests/scene/test_local_lrt_builder.cpp
  tests/scene/test_local_lrt_color_sdf.cpp
  tests/scene/test_local_lrt_volume_3d.cpp

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

## V0 Gap Closure — Independent Local Geometry Field / Pre-V0.8 Remaining Gaps

Status: COMPLETED
Date: 2026-08-29
Visual PASS: 2026-08-29

Implemented:
- per-object Color SDF 与 Radiance Probe 分离；`geometry_voxel_size` 独立于 `probe_spacing`。
- SampleDir LTM、`ColorToFill = albedo + emission`、`inside_solid` GPU/Forward 丢弃规则。
- 固定 Geometry voxel size 下旋转薄板切向 LTM 方差与连续参考误差随 spacing 不增。

Human Visual Validation:
- PASS — Color SDF Volume + SampleDir LTM；GPU / Forward inside_solid；Cornell Box `geometry_voxel_size=0.125` 下 `1.0` vs `0.25` probe spacing。

## V0.7 — Local Space 平移 / 旋转 + Editor / Runtime Parity

Status: COMPLETED
Date: 2026-08-29

Implemented:
- Transform 变化只同步 Volume / builder / RS inverse，不 rebuild Local GI。
- 动态灯按当前 Volume Local Space 重新 injection。
- Forward sampling 每帧使用 `world_to_local`。
- Cornell Box：Shift+WASD/QE 平移房间、Shift+方向键旋转房间，相机保持世界位姿。

Tests:
- Unit test PASS — 平移/旋转不改变 occupancy / Local Visibility / Transfer；世界灯 injection 随 Volume 旋转变化；灯随 Volume 一起运动时 local injection 保持。

Human Visual Validation:
- PASS — 用户确认整体平移 / 旋转时 GI 贴在几何上、不重建、不重置 history；Editor Viewport 与 Runtime 一致。

## V0.6 — Forward Surface Sampling + Edge Blend

Status: COMPLETED
Date: 2026-08-27
Visual PASS: 2026-08-29

Implemented:
- Volume Bounds 与 World → Local → Grid UVW；occupied-aware cubic B-spline 采样 RGB SH2；clamped-cosine 漫反射；`edge_blend_distance`。
- 单一 reflected Radiance 场；解析灯 Injection 只进入当前 Local Transfer。
- Visibility 与 Radiance 迭代解耦；衰减按世界米；LTM 按原文入射/出射方向约定。
- 一格 normal bias。

Human Visual Validation:
- PASS — 用户确认 Forward+ Cornell Box 红/绿 bleeding、暗部间接照明、Volume 外行为与 Edge Blend；`V` / `G` 对比通过。

## V0.5 — Radiance Propagation Compute

Status: COMPLETED
Date: 2026-08-27
Gap fill: 2026-08-29

Implemented:
- RGB SH2 Radiance A/B ping-pong propagation，集成 Injection、Global / Local Visibility、Local Transfer、empty-space transmission 与 decay。
- GPU 1 / 2 / 4 / 8 iterations 与独立 CPU recurrence 数值一致。
- Radiance RGB Probe Debug 与解析灯光隔离控制。
- Gap fill: 平面与红白内角在 `1.0 / 0.5 / 0.25m` spacing 下分别记录 coverage / Local Transfer / 一次反射 / 邻域传播 / 收敛 Radiance；以独立 `0.125m` CPU reference 比较贴表面 Probe 的 Transfer / 一次反射 / 收敛 Radiance 误差，残差收敛后再判定，细网格不得反向增大。

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
Gap fill: 2026-08-29

Implemented:
- 收集 Volume 范围内可见的静态 `MeshInstance3D`，将三角形转入 Volume Local Space 并栅格化到 Probe Grid。
- 提取 `StandardMaterial3D` albedo / emission，复用 CPU reference builder 生成 Local Visibility 与 RGB Local Transfer。
- 添加 Occupancy、灰度 Local Visibility 与实际 RGB Local Transfer 三种独立 Gizmo 调试模式。
- 添加 Wall / CPU reference、颜色通道、空 Probe、Cube 与旋转 Local-space 自动测试。
- Gap fill: Surface Voxel Field 改为 coverage / albedo / emission / surface normal；多三角形按 4×4 `sample_mask` 并集合并 coverage，材质按本次命中样本加权；LTM / emission 使用 occupancy 与存储法线（无法线时回退 `-offset`）；Occupancy Debug 按 coverage 调制 alpha。
- Gap fill tests: overlapping merge、平面相位 `y=0/0.25/0.5/0.75` coverage 不归零、同一平面细分 1 段 vs 8 段 coverage 不变、plane `1.0 / 0.5 / 0.25m` coverage 稳定、斜平面 / Sphere 随 spacing 细化、Volume QuadMesh 与 builder rasterize 一致。

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
- Local transfer uses the reference `-SampleDir` incident direction and opposite diffuse output direction, with per-channel reflectance and explicit SH outer-product construction.
- Omni attenuation is `(1 - distance / range)^2`; Spot additionally applies squared normalized cone attenuation.
- Golden red-wall center after 4 iterations: visibility X `1.06501`, reflected radiance R X `1.65471`, reflected radiance G X `0.206852` after reference-direction LTM reconstruction with unshadowed Directional Injection.

---

# 6. Current Working Set

```text
Files Modified:
- scene/3d/local_lrt_math.h
- tests/scene/test_local_lrt_math.cpp
- servers/rendering/renderer_rd/shaders/environment/local_lrt_injection.glsl
- servers/rendering/renderer_rd/environment/local_lrt.h
- servers/rendering/renderer_rd/environment/local_lrt.cpp
- servers/rendering/rendering_server.h
- servers/rendering/rendering_server.cpp
- servers/rendering/rendering_server_default.h
- servers/rendering/environment/renderer_gi.h
- servers/rendering/renderer_rd/environment/gi.h
- servers/rendering/renderer_rd/environment/gi.cpp
- servers/rendering/dummy/environment/gi.h
- drivers/gles3/environment/gi.h
- drivers/gles3/environment/gi.cpp
- servers/rendering/renderer_scene_render.h
- servers/rendering/renderer_rd/renderer_scene_render_rd.h
- servers/rendering/renderer_rd/renderer_scene_render_rd.cpp
- servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp
- servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp
- servers/rendering/renderer_scene_cull.cpp
- scene/3d/local_lrt_volume_3d.h
- scene/3d/local_lrt_volume_3d.cpp
- local_lrt_volume_misc/test_project/gpu_directional_shadow_injection_validation.gd
- local_lrt_volume_misc/LOCAL_LRT_STATE.md

Relevant Symbols / Functions:
- LocalLRTMath::compute_directional_shadow_projection
- RenderingServer::local_lrt_volume_set_directional_shadow
- LocalLRT::_inject_analytic_lights
- RendererSceneRenderRD::_update_local_lrt_volume
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

GPU Analytic Injection Validation:
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method mobile --rendering-driver vulkan --script res://gpu_analytic_injection_validation.gd

GPU Directional Shadow Injection Validation:
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method mobile --rendering-driver vulkan --script res://gpu_directional_shadow_injection_validation.gd

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
Unit Tests: PASS — 51 cases / 1209 assertions (`[LocalLRTMath]`, `[LocalLRTBuilder]`, `[LocalLRTVolume3D]`, `[LocalLRTColorSDF]`)
GPU Visibility Validation: PASS — Vulkan Forward Mobile; 1/2/4/8 iterations matched pinned CPU values
GPU Injection Validation: PASS — 81 RGB SH2 values uploaded/read back exactly and clear returned zero
GPU Radiance Validation: PASS — Vulkan Forward Mobile; 1/2/4/8 iterations and persistent 1+1-step propagation matched the independent CPU recurrence for all 81 RGB SH2 values; Injection upload no longer resets A/B Radiance
GPU Analytic Injection Validation: PASS — directional / omni / spot / combined-rotated / center inside_solid / empty lights vs CPU reference, 27 probes; re-run after Directional Shadow still PASS
GPU Directional Shadow Injection Validation: PASS — Vulkan Forward Mobile; synthetic reverse-Z plane occluder, 125 probes; front vis>0.9, back vis<0.1; disabled shadow matches unshadowed CPU reference
Runtime Smoke Test: PASS — Forward+ and Dummy/headless Cornell Box loaded without errors
Runtime Dynamic Radiance: PASS — moving Omni changed center Probe radiance; has_gpu_data=true
Runtime Radiance Capture: PASS — Directional-only and Omni-only captures completed; analytic lights remain unshadowed until an explicit renderer shadow input implements the reference `probe not in Shadow` condition
Directional Isolation Validation: PASS — residual Radiance was traced to the EmissionPanel (max R SH length 1.34666); with all sources disabled it is exactly zero. Analytic-light isolation now disables the panel emission and rebuilds Local LRT data.
Forward Surface Validation: PASS — Forward+ Vulkan framebuffer changes with persistent reflected-only Local LRT (`full=0.01703586`), and large edge blend reduces contribution (`blended=0.00013417`); analytic Injection only feeds the current Local Transfer, neighbor gather consumes the preserved previous Radiance field, and surface sampling uses one-cell normal bias with occupied-aware cubic B-spline weights.
Forward+ Runtime Binding: PASS — shader/UBO/storage binding initialized without Local LRT uniform errors.
Fine Grid Rebuild: PASS — runtime `probe_spacing=0.25` rebuilt resolution `35×23×35` with valid CPU/GPU data；修复 Inspector grid property 修改只清空、不重建的问题。
Three-light Direction-Corrected Runtime: PASS — after the reference-direction LTM fix, 16-frame isolated captures show positive, spatially distributed Local LRT contributions: Directional `0.05752297→0.05754675` with `7495→7495` changed samples, Omni `0.03971097→0.03977896` with `8084→8084`, and Spot `0.01000463→0.01002052` with `4903→4915`; no prior negative-energy edge truncation observed in runtime screenshots.
V0.2 Surface Voxel Field: PASS — 半开法线区间 + sample_mask 并集；相位 / 细分 / spacing coverage 测试通过
V0.5 Spacing Stages: PASS — 平面与红白内角相对独立 0.125m reference；残差收敛；贴表面 Transfer / 一次反射 / 收敛 Radiance 误差不反向
V0.7 Transform Parity: PASS — unit test; transform does not rebuild Local GI; world lights re-inject; co-moving lights keep local injection
Human Visual Validation: PASS — 用户确认 V0.6（bleeding / 暗部 / Volume 外 / Edge Blend）与 V0.7（Shift 平移旋转不重建）；V0.8 Directional Shadow 已影响 GI
```

Notes:
- Initial build without `accesskit=no d3d12=no` stopped because optional local SDK dependencies were absent; the recorded build command disables those unrelated drivers and passes.
- Test mode always initializes `RasterizerDummy`, so actual GPU validation runs as a separate deterministic Forward Mobile Vulkan script against pinned CPU-reference values.
- Cornell Box desktop renderer was changed from GL Compatibility to default Forward+ because Local LRT GPU resources require RenderingDevice.

---

# 9. Known Issues / Deferred

- `--headless --editor --quit` reaches editor initialization, then this custom engine build crashes in `EditorNode::is_cmdline_mode` with a null singleton. Runtime headless loading succeeds without errors.
- GL Compatibility retains no-op Local LRT storage; GPU compute and Global Visibility debug require Forward+ or Forward Mobile.
- Volume Directional Shadow 使用 Godot RD reverse-Z（`set_depth_correction(true, true)`，近=1 远=0），比较 `(probe + bias) >= occluder`，与 heightfield 光栅 `GREATER_OR_EQUAL` + clear 0 一致；不得复用相机 CSM 或 Global Visibility。
- `inside_solid` uint buffer 必须 `resize_initialized`；`Vector<uint32_t>::resize` 不清零，会导致 Radiance 随机跳过 Probe。
- `propagation_iterations` 是每帧 Radiance Probe-hop 数；Radiance A/B 在 Injection 不变时继续跨帧传播，在 Injection 更新时也保留旧场并通过 recurrence 逐步收敛。
- Surface normal bias 当前固定为一个 Probe grid cell；后续若不同几何尺度出现漏光或接触变暗，再评估暴露为可调参数。
- Occupancy / Geometry Coverage Debug 按 fractional coverage 着色；Inside Solid 模式才把 `inside_solid` 画成洋红。Radiance 不再覆盖洋红 occupied。
- V0.7 Cornell Box 用 Shift 平移/旋转房间，相机保持世界位姿。
- Debug Probe 半径为 `min(debug_probe_scale, min(actual_spacing)*0.35)`。
- Color SDF spacing variance 人工视觉已 PASS。
- Occupancy-grid `rasterize_triangle` / `set_occupancy` 仍是离散回归路径：`inside_solid = coverage > 0`。Runtime Volume 只走 Color SDF。
- Canonical red-wall occupancy golden 因 SampleDir 外积更新为 visibility X `1.06501`、radiance R X `1.32879`、G X `0.166258`。
- GPU 已上传 `inside_solid`；Forward cubic 与 GPU Radiance 只跳过该标志。人工视觉 PASS。

---

# 10. Blockers / Decisions Needed

- 无。V0.8 Directional Shadow 人工视觉 PASS。下一阶段是 V0.9 Omni / Spot Shadow Visibility。

---

# 11. Next Action

```text
Start V0.9 Omni / Spot Shadow Visibility on the same GPU Injection path. Do not expand into Area Light.
```

---

# 12. Session Handoff

```text
Last Session Summary:
Volume Directional Shadow Map landed with reverse-Z PCF. Unit/GPU tests PASS. User confirmed shadows affect GI; V0.8 complete.

Current Phase:
V0.8 — GPU Analytic Light Injection + Directional Shadow Visibility

Current Status:
COMPLETE — Volume Directional Shadow Map

What Was Completed:
- Volume Directional Shadow Map independent of camera CSM
- Reverse-Z PCF sample: `(probe + bias) >= occluder`
- Renderer order Shadow → Injection → Propagation → Forward
- GPU synthetic front/back + disabled-shadow cases PASS

Test Results:
- Compile PASS
- Unit tests 51/51 PASS (1209 assertions)
- GPU analytic injection PASS (27 probes)
- GPU directional shadow injection PASS (125 probes, 2 cases)

Human Visual Validation:
- Directional Shadow PASS — user confirmed shadows affect GI

Exact Next Step:
- Start V0.9 Omni / Spot Shadow Visibility. Do not commit cornell_box.tscn.
```
