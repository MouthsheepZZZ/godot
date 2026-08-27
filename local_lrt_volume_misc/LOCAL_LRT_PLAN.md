# Local LRT 最小原型开发计划

> 目标：在 Godot 4.7 中以最小侵入方式验证 `LocalLRTVolume3D` 是否可行。优先看到正确效果，再做工程化和性能优化。
>
> 跨会话规则：执行 AI 每次会话开始时读取本文件与 `LOCAL_LRT_STATE.md`；本文件默认只读，进度与执行事实只写入 STATE。除非用户明确要求，不得自行改 PLAN。
>
> 相关文档和参考均放置在local_lrt_volume_misc/目录内

---

# 1. 总体原则

- 尽量新增独立文件，避免修改 Godot 现有 GI 核心。
- 现有 Renderer / RenderingServer 只增加必要的薄接入点。
- 如无必要勿增实体：不提前建立抽象层、缓存系统、兼容层、额外数据类型或通用框架。
- GI 全部工作在 `LocalLRTVolume3D` 的 Local Space。
- Editor 与 Runtime 使用同一套构建、计算和渲染路径。
- v0 只解决：**静态 Geometry + 动态解析灯光**。
- 首版优先直接、明确、可验证的实现；不要在功能阶段顺手做性能优化。
- 数学 / 算法机制验证使用自动单元测试。
- 最终视觉效果验证必须由人眼确认；AI 不得自行判定视觉 PASS。
- 每个阶段独立编译、测试、提交；一次只处理一个主要问题。
- 所有性能优化统一放到最后的 v4。

---

# 2. 已知 Godot 接入位置

以下路径来自前期对 Godot 4.7 分支结构的调查，执行 AI 应优先从这些位置进入，先确认当前分支中路径和职责是否仍一致，再做最小修改；不要重新全仓库盲搜。

## 2.1 Scene Node

新增：

```text
scene/3d/local_lrt_volume_3d.h
scene/3d/local_lrt_volume_3d.cpp
```

节点注册入口：

```text
scene/register_scene_types.cpp
```

## 2.2 Local LRT RendererRD 子系统

优先新增独立实现：

```text
servers/rendering/renderer_rd/environment/local_lrt.h
servers/rendering/renderer_rd/environment/local_lrt.cpp
servers/rendering/renderer_rd/shaders/environment/local_lrt_*.glsl
```

职责仅限 Local LRT：

- Volume RID / instance 数据。
- GPU resource 生命周期。
- Visibility / Radiance compute pipeline。
- Light injection。
- Surface sampling 所需绑定。
- Debug 数据。

不要把主体逻辑塞进现有 VoxelGI / SDFGI / HDDAGI。

## 2.3 RenderingServer / Scene Cull 薄接入点

优先检查：

```text
servers/rendering/rendering_server.h
servers/rendering/rendering_server.cpp
servers/rendering/rendering_server_default.h
servers/rendering/rendering_server_enums.h
servers/rendering/renderer_scene_cull.h
servers/rendering/renderer_scene_cull.cpp
servers/rendering/renderer_scene_render.h
servers/rendering/renderer_rd/renderer_scene_render_rd.*
```

如确有必要，新增独立：

```text
INSTANCE_LOCAL_LRT_VOLUME
```

不要伪装成 `INSTANCE_VOXEL_GI`。

## 2.4 Forward 渲染接入点

优先检查：

```text
servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.*
servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl
```

这里只做：

- 将当前有效 Local LRT Volume 数据传到 Forward GI shading。
- World position / normal → Volume local。
- Sample Local LRT diffuse irradiance。
- Edge blend。

## 2.5 CPU 构建 / 数学模块

优先新增：

```text
scene/3d/local_lrt_builder.h
scene/3d/local_lrt_builder.cpp
scene/3d/local_lrt_math.h
```

职责：

- SH2 数学。
- Local / World / Grid / UVW 坐标转换。
- 静态 Geometry → Local Grid。
- Local Visibility。
- Local Transfer Matrix。
- CPU Reference Solver。

Editor 与 Runtime 必须调用同一套实现，不维护两套 builder。

## 2.6 Editor Gizmo

新增：

```text
editor/scene/3d/gizmos/local_lrt_volume_3d_gizmo_plugin.h
editor/scene/3d/gizmos/local_lrt_volume_3d_gizmo_plugin.cpp
```

注册入口优先检查：

```text
editor/scene/3d/node_3d_editor_plugin.cpp
```

---

# 3. 最小数据约定

首版使用 SH2，即每个方向函数为 `vec4`。

建议保持最直白的数据：

```text
LocalVisibilitySH        vec4
GlobalVisibility A/B     vec4 ping-pong
Radiance R/G/B A/B       vec4 × RGB ping-pong
LocalTransferMatrix      RGB mat4
Injection                RGB SH2
```

首版不要为了显存或 binding 数做压缩。

Probe 布局统一：

```text
local_bounds = AABB(-size * 0.5, size)
resolution   = ceil(size / requested_probe_spacing) + 1
actual_spacing = size / (resolution - 1)
```

Volume 首版只允许平移、旋转；尺寸由 `size` 控制。

---

# 4. P0 — 数学参考与测试环境

## P0.1 — 冻结数学与传播约定

目标：GPU 接入前先把所有容易产生轴向、矩阵和 SH 歧义的规则冻结。

实现：

- SH2 basis 顺序与归一化。
- SH encode / evaluate。
- SH Triple Product。
- SH2 rotation。
- `LocalTransferMatrix` 行列约定。
- `B'` 的旋转约定并用数值测试确认。
- Local / World / Grid / UVW 转换。
- Probe linear index。
- 26 邻居方向与权重。
- `LocalVisibilitySH` 的语义：明确是 visibility，不得与 occlusion 混用。
- Visibility recurrence。
- Radiance recurrence。
- 明确空空间 Radiance 如何继续传输。

必须通过的单元测试：

- 常量 SH。
- 单方向 lobe。
- Triple Product 常量乘法。
- 90° / 180° rotation。
- Local ↔ World。
- Grid index / UVW。
- 简单 Transfer Matrix。
- 空空间 Radiance 可以跨 Probe 传播。
- 无光源时能量不发散并最终衰减。

完成条件：数学约定冻结，后续 CPU / GPU 都严格遵循。

---

## P0.2 — CPU 最小 Reference Solver

只做用于验证算法的最小 CPU Grid，不扩展成正式 Runtime 子系统。

实现：

- 规则 Probe Grid。
- 人工 occupancy / albedo 输入。
- Local Visibility。
- Local Transfer Matrix。
- Global Visibility ping-pong。
- Radiance ping-pong。
- Directional / Omni / Spot 简单 injection。
- 完整 26 邻居传播。

解析测试：

1. 空网格。
2. 单白墙。
3. 单红墙。
4. 红墙 + 白墙夹角。
5. 封闭盒。
6. 带开口盒。
7. 点光源被墙分隔。
8. Grid 整体旋转。

必须通过的单元测试：

- 空空间传播。
- 墙附近 visibility 方向正确。
- 红墙产生 R 更强的 transfer / bleeding。
- 反照率 `< 1` 时能量稳定。
- Volume 整体旋转后 Local-space reference 保持一致。

保存关键数值，作为 GPU golden reference。

---

## P0.3 — 测试项目与 Cornell Box

创建一个长期复用的最小测试项目，不为每个阶段重复建新工程。

建议位置：

```text
local_lrt_volume_misc/test_project/
```

主测试关卡：

```text
CornellBox
├── Floor / Ceiling
├── White Wall
├── Red Wall
├── Green Wall
├── Back Wall
├── Simple Box Geometry
├── LocalLRTVolume3D
├── DirectionalLight3D
├── OmniLight3D
├── SpotLight3D
├── Camera3D
└── Minimal Debug Controller
```

要求：

- 只使用简单 Primitive / Mesh。
- 使用基础 `StandardMaterial3D` albedo / emission。
- 灯光允许运行时移动、旋转、改颜色、改能量、改范围、开关。
- 后续 v1/v2/v3 继续扩展这个项目。
- 不为了测试框架额外建立复杂 gameplay 实体。

视觉验收：人工确认 Cornell Box 尺寸、材质与灯光控制正常。

---

# 5. v0 — 静态场景 + 动态灯光

## V0.1 — `LocalLRTVolume3D` + RID + Probe Gizmo

实现最小节点 API：

```text
enabled
size
probe_spacing
resolution          # derived/read-only
propagation_iterations
energy
edge_blend_distance
debug_draw
debug_probe_scale
rebuild()
```

完成：

- 节点注册。
- Local AABB。
- Probe Grid。
- RenderingServer RID 创建 / 释放。
- Transform / property 同步。
- Editor gizmo：Bounds + Probe Sphere。

自动验证：

- 创建 / 删除 / 保存 / 加载。
- resolution / spacing / probe positions。
- RID 生命周期。

人工视觉验证：

- Cornell Box 中 Bounds 与 Probe Grid 对齐。
- Volume 旋转后 gizmo 正确。

---

## V0.2 — 静态 Geometry → Local Grid / Visibility / Transfer

只收集 Volume 范围内静态 Geometry。

首版材质只要求：

- `StandardMaterial3D` albedo。
- emission。

生成：

- Occupancy / surface information。
- Albedo。
- Emission。
- `LocalVisibilitySH`。
- RGB `LocalTransferMatrix`。

构建方式保持最简单：将静态 Geometry 转入 Volume Local Space，再使用完整 26 邻域信息构建 Local Visibility 和 Local Transfer。

自动验证：

- Cube / Wall 与 CPU Reference 一致。
- 红色表面的 R transfer > G/B。
- 空 Probe transfer = 0。
- Volume Local 坐标正确。

人工视觉验证 Debug：

- Probe Sphere。
- Local Visibility。
- Local Transfer 有效区域。

确认 Cornell Box 几何对应关系正确。

---

## V0.3 — GPU Resources + Global Visibility Compute

建立最小 RD 数据：

- Local Visibility。
- Global Visibility A/B。
- Local Transfer Matrix。
- Radiance A/B。
- Injection。

实现最直接的：

```text
26-neighbor gather
→ visibility propagation
→ ping-pong
```

自动验证：

- GPU 1 / 2 / 4 / 8 iterations 与 CPU golden reference 对比。
- 无 NaN / Inf。
- 传播方向与数值正确。

人工视觉验证：

- Debug 显示 Global Visibility。
- Cornell Box 中遮蔽可以从局部向更远处传播。

---

## V0.4 — 动态解析灯光 Injection

支持：

- `DirectionalLight3D`。
- `OmniLight3D`。
- `SpotLight3D`。

运行时支持：

- 平移。
- 旋转。
- 颜色。
- energy。
- range。
- spot angle。
- 开关。

灯光变化只更新 Injection，不重建静态 Geometry / Local Transfer。

自动验证：

- World → Volume Local position/direction。
- Omni attenuation。
- Spot cone。
- Directional direction。
- 灯关闭后 injection 清零。

人工视觉验证：

- Cornell Box 移动灯时 GI 响应。
- 改灯颜色后反弹颜色响应。
- 开关灯后 GI 重新传播和收敛。

---

## V0.5 — Radiance Propagation Compute

实现：

```text
Direct / Emissive Injection
        ↓
Gather 26 Neighbor Radiance
        ↓
Visibility
        ↓
Local Visibility
        ↓
Local Transfer Matrix
        ↓
Next Radiance
```

使用 A/B ping-pong。

自动验证：

- 空空间。
- 单墙。
- 红墙。
- Cornell Box。
- Omni / Spot / Directional。
- 1 / 2 / 4 / 8 iterations。
- GPU 与 CPU reference 在允许误差内一致。

人工视觉验证：

- Radiance Debug 能看到传播过程。
- 红墙产生红色反弹。
- 绿墙产生绿色反弹。
- 动态灯变化后 history 收敛到新结果。

---

## V0.6 — Forward Surface Sampling + Edge Blend

在 Forward GI 路径加入最薄的 Local LRT sampling：

```text
World Position / Normal
        ↓
Volume Local
        ↓
Grid UVW
        ↓
Trilinear RGB SH Sample
        ↓
Normal Evaluate
        ↓
Diffuse Indirect
```

实现：

- Volume Bounds 判断。
- `edge_blend_distance`。
- Volume 边缘向外平滑衰减。

自动验证：

- World → Local → UVW。
- SH normal evaluate。
- edge weight。
- 没有 LocalLRTVolume 时原渲染结果不改变。

人工视觉验证：

- Cornell Box 红 / 绿 Color Bleeding 可见。
- 暗部存在合理间接照明。
- Volume 外无 Local GI。
- Volume 边缘没有硬切。

---

## V0.7 — Local Space 平移 / 旋转 + Editor / Runtime Parity

整体移动：

```text
Cornell Box + LocalLRTVolume3D
```

要求：

- Probe / Transfer / Visibility / Radiance 数据仍在 Volume Local Space。
- 整体平移 / 旋转时不重新构建 Local GI。
- 动态灯仍正确转换到当前 Volume Local Space。
- Surface sampling 使用最新 inverse transform。

自动验证：

- Transform 数学。
- SH rotation / direction。
- Editor 与 Runtime 使用同一路径。

人工视觉验证：

- Cornell Box 整体高速平移时 GI 稳定。
- 整体旋转时 GI 方向正确。
- 不因世界空间运动触发重建或重新初始化 history。
- Editor Viewport 与 Runtime 表现一致。

### v0 总验收

只有以下全部满足才进入 v1：

自动：

- P0 / v0 数学与机制测试全部 PASS。
- GPU 与 CPU reference 符合误差要求。

人工：

- Cornell Box Local GI 明确可见。
- 动态解析灯实时影响 GI。
- 红 / 绿墙 Color Bleeding 正确。
- Volume 整体平移 / 旋转稳定。
- Edge Blend 正常。
- Editor / Runtime 一致。

---

# 6. v1 — 动态物体

继续使用同一个 Cornell Box，仅增加必要的可移动测试 Cube。

目标：验证相对于 Volume 真正移动的 Geometry 可以 Receive / Contribute GI。

实现：

- 动态物体 Receive Local GI。
- 动态物体参与 Local Geometry 构建。
- 动态物体移动 / 旋转后重新构建受影响 Local GI 数据。
- 更新 Local Visibility。
- 更新 Local Transfer Matrix。
- 更新 Emission。
- 重置 / 重新传播必要的 Radiance state。

原型阶段优先使用最简单可靠的重建方式；不要提前做 Trunk / Dirty Region 优化。

自动验证：

- 动态 Cube 不同位置与 full rebuild reference 一致。
- 移除后旧位置数据被清除。
- 动态物体 Local Transform 正确。

人工视觉验证：

- 动态物体能接受 GI。
- 红色 Cube 移动时 bleeding 跟随。
- 动态遮挡变化影响最终 GI。
- 不存在明显旧位置残留。

---

# 7. v2 — Global GI 注入

目标：实现 World / Global Lighting → Local LRT 的单向输入。

输入：

- Sun。
- Sky。
- Global diffuse GI。

实现：

- World SH → Volume Local SH。
- 外部光照进入 Local LRT。
- Global Visibility 参与 Sky occlusion。
- Local LRT 与 Global GI 在 Volume 边缘连续衔接。

自动验证：

- World → Local SH rotation。
- Constant external lighting injection。
- Volume 旋转后世界光方向保持正确。
- Blend weight。

人工视觉验证：

- Cornell Box 开口能接受外部环境光。
- Volume 旋转后 Sun / Sky 世界方向正确。
- Volume 边界没有明显亮度跳变。

---

# 8. v3 — 多 Volume + Priority

继续扩展同一测试项目，加入第二个 `LocalLRTVolume3D`。

实现：

- 多个 Volume 独立维护 Probe / Visibility / Radiance state。
- `priority`。
- Volume overlap 选择。
- 重叠区域 Blend。
- 各自 Local Transform 独立采样。

自动验证：

- Volume 选择规则确定且稳定。
- Priority 排序稳定。
- Blend 权重正确。
- 删除一个 Volume 不影响其他 Volume。

人工视觉验证：

- 两个独立 Volume 正常工作。
- 重叠区域无硬切。
- Priority 行为符合预期。
- 不同旋转 Volume 可同时工作。

---

# 9. v4 — 性能优化

只有 v0～v3 的正确性与视觉结果确认后才进入。

先记录未优化 reference 的：

- GPU Visibility 时间。
- GPU Radiance 时间。
- Injection 时间。
- CPU Geometry / Transfer rebuild 时间。
- GPU memory。
- Upload bytes。

然后逐项尝试优化；每一项都必须单独验证并有明确收益才保留。

候选：

- 26 Neighbor → 4 Neighbor / 3-frame pattern。
- Temporal / spatial dither。
- FP32 → FP16。
- Local Transfer Matrix 压缩。
- Luminance Matrix + RGB Tint。
- Trunk Scene Management。
- Dirty Region。
- Partial rebuild。
- Partial GPU upload。
- Dynamic update budget。
- Visibility / Radiance update budget。
- 不可见 Volume 暂停更新。
- Screen Space Gather。
- GPU buffer / texture layout 优化。
- Forward Mobile 适配与移动端验证。

每项优化必须：

- 保留可对照的 reference 行为或可重复测试结果。
- 自动数学测试不回归。
- Cornell Box 人工视觉验证不回归。
- 有明确 CPU / GPU 时间或显存收益。

---

# 10. 每阶段固定执行规则

## 会话开始

只读取：

1. `LOCAL_LRT_PLAN.md`。
2. `LOCAL_LRT_STATE.md`。
3. STATE 中列出的当前阶段相关文件。
4. 必要时最多 1～2 个 Godot 参考子系统。

不要重新扫描全部历史，不重新通读整篇参考文章，除非当前阶段确实需要核对某个算法定义。

## 阶段内

- 只解决 STATE 中的 `Current Phase`。
- 不顺手做后续阶段。
- 不顺手重构已有 GI。
- 不提前做性能优化。
- 新发现但不阻塞当前阶段的问题写入 STATE 的 `Deferred / Known Issues`。

## 阶段结束

必须：

- 编译。
- 跑当前阶段自动测试。
- 更新 Cornell Box 测试项目（如适用）。
- 更新 `LOCAL_LRT_STATE.md`。
- 单独 Commit。

如果阶段需要视觉验证：

1. AI 完成自动验证后停止。
2. STATE 设置为 `WAITING_HUMAN_VISUAL_VALIDATION`。
3. 明确告诉用户打开哪个测试场景、看什么现象。
4. **只有用户明确确认 PASS 后**，才能把该阶段标记为完成并进入下一阶段。

## PLAN / STATE 权限

- `LOCAL_LRT_PLAN.md`：默认只读。
- `LOCAL_LRT_STATE.md`：执行 AI 的唯一跨会话进度写入点。
- 如果实现事实迫使计划变化：先在 STATE 记录原因并停止，等待用户决定是否修改 PLAN。
