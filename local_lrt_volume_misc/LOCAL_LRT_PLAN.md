# Local LRT 最小原型开发计划

> 目标：在 Godot 4.7 中以最小侵入方式验证 `LocalLRTVolume3D` 是否可行。V0 优先证明 Local Transfer 驱动的基础间接光照在物理意义上正确，再做 Global GI、屏幕空间重建、工程化和性能优化。
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
- v0 只解决：**静态 Geometry + 动态解析灯光及其 Shadow Visibility + Local Transfer 驱动的基础 Diffuse GI**。
- V0 的核心验收是 Local Visibility / Local Transfer / Shadow-aware Analytic Light Injection / 反射 Radiance 的基础物理关系正确；天光遮蔽和 Global GI 输入不属于 V0 通过条件。
- 遵循原文的 CPU / GPU 分工：CPU 根据局部 Geometry 构建 Local Visibility / Local Transfer，GPU 完成解析灯光注入、Shadow Visibility、Radiance gather 与传播；不得为了采样 GPU Shadow Map 而把静态 LRT Builder 整体迁移到 GPU。
- Probe 密度是空间离散化参数，不是独立的质量开关；改变 spacing 时，Local Geometry 离散化、采样权重、LTM 能量、传播收敛和表面重建必须保持一致。
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
- 局部 Geometry 采样、方向采样和立体角 / 能量归一化。
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

Local Geometry 约定：

原文把局部 Geometry / Color SDF / Height Field / Precomputed Local Transfer 数据映射到 Probe Grid，并在 Trunk 内由每个 Probe 查询周围 26 个邻接信息构建 Local Transfer。V0 先沿用这个 26-neighbor 局部构建方式；spacing 变小时，几何体素 / Primitive 采样应变细，但不额外引入独立的 Local Support 半径，也不改变 Radiance 的 26-neighbor 传播拓扑。

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
- 区分 Local Geometry 构建阶段和 Radiance propagation 阶段：前者构造 Local Visibility / Local Transfer，后者在 Probe 邻接图上传播 Radiance。
- 26-neighbor 局部采样在 spacing 变化后的方向、立体角权重和能量归一化。
- `LocalVisibilitySH` 的语义：明确是 visibility，不得与 occlusion 混用。
- Radiance recurrence。
- 明确空空间 Radiance 如何继续传输。
- 明确 spacing 对照实验：固定同一 Volume size、几何、材质和灯光，分别使用 `1.0 / 0.5 / 0.25m` requested spacing；每次重建并等待 Radiance 收敛，再分别比较 Local Transfer、一次反射和最终表面 GI。该实验不是比较同一 Probe-hop 数下的瞬时画面。

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
- 同一平面 / 墙角的 Local Visibility / Local Transfer 在不同 spacing 下保持物理响应稳定，并随 spacing 细化收敛。
- spacing 变小只能增加几何采样密度，不得改变同一连续几何的总反射能量。

完成条件：数学约定冻结，后续 CPU / GPU 都严格遵循。

---

## P0.2 — CPU 最小 Reference Solver

只做用于验证算法的最小 CPU Grid，不扩展成正式 Runtime 子系统。

实现：

- 规则 Probe Grid。
- 人工 occupancy / albedo 输入。
- Local Visibility。
- Local Transfer Matrix。
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
- 平面、内角、红白墙在 `1.0 / 0.5 / 0.25m` spacing 下进行收敛对比；细网格应提供更细的空间变化，同时物理积分响应不得反向恶化。
- 单独记录 Local Visibility、Local Transfer、一次局部反射和传播后 Radiance，避免把 Forward 过滤误差混入 Local Transfer 验证。

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

## V0 核心正确性要求

V0 只有同时满足以下要求才可完成：

1. Radiance 必须按 Probe 邻接关系正确传播：每次迭代只读取约定邻域，方向、A/B ping-pong、Visibility mask、空空间传输和 Local Transfer recurrence 均与 CPU Reference 一致。
2. Local Visibility、Local Transfer Matrix、Injection 和 reflected Radiance 的物理语义必须明确且一致；解析灯光不得绕过 LTM 直接成为间接光，无光源时不得产生能量，反照率 `< 1` 时不得发散。
3. 输出至少符合基础扩散逻辑：光只从已有 Injection / Emission 和上一轮邻居 Radiance 逐步传播；遮挡、材质颜色和反射方向必须在对应阶段产生可解释的结果，不得出现跨 Probe 瞬移或无来源亮度。
4. 提高 Probe 密度必须降低空间离散误差：同一场景充分收敛后，更小 spacing 的 Local Geometry、Local Transfer 和表面 GI 应比粗网格更细，并向高分辨率 reference 收敛；不得出现系统性能量漂移、断层或整体质量反向下降。

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

生成 V0 Surface Voxel Field：

- Surface coverage，不再只使用二值 `occupied`。
- Albedo。
- Emission。
- Surface normal。
- `LocalVisibilitySH`。
- RGB `LocalTransferMatrix`。

构建方式保持最简单：将静态 Geometry 转入 Volume Local Space，使用保守三角形体素化生成 coverage / material / normal，再由周围完整 26 邻接 voxel 信息构建当前 Probe 的 Local Visibility 和 Local Transfer。多个三角形覆盖同一 voxel 时必须按 coverage 稳定合并材质与法线。

spacing 变化时，同一连续几何应得到更细的 Surface Voxel Field；26 邻域的方向、立体角 / 能量权重和材质合并规则保持一致，不得因为 Probe 数量改变而改变同一几何的总 Transfer 能量。V0 不实现 SDF。

自动验证：

- Cube / Wall 与 CPU Reference 一致。
- 斜平面 / Sphere 的 coverage、法线和边界随 spacing 细化而趋近高分辨率 reference。
- 同一平面在不同 spacing 下的有效表面位置、厚度和总 coverage 保持稳定。
- 红色表面的 R transfer > G/B。
- 空 Probe transfer = 0。
- Volume Local 坐标正确。

人工视觉验证 Debug：

- Probe Sphere。
- Local Visibility。
- Local Transfer 有效区域。

确认 Cornell Box 几何对应关系正确。

---

## V0.3 — GPU Resources + Local Data Upload

建立最小 RD 数据：

- Local Visibility。
- Local Transfer Matrix。
- Radiance R/G/B A/B。
- Direct / Emissive Injection。

实现：

- 为 Local Visibility、Local Transfer Matrix 和 Injection 建立 GPU storage buffer。
- 保持 CPU Reference 与 GPU 数据布局一致。
- 为后续 Radiance propagation 提供最直接的资源生命周期和上传路径。

自动验证：

- CPU → GPU 上传 / readback 与 reference 数值一致。
- Probe 数量、buffer 长度和 spacing 改变后的资源重建正确。
- 无 NaN / Inf。

人工视觉验证：

- Debug 显示 Local Visibility 和 Local Transfer 的有效区域。
- Probe 与 Cornell Box 几何对应关系正确。

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

灯光变化只更新 Injection，不重建静态 Geometry / Local Transfer。本阶段先冻结原文中 Directional / Local Light 的位置、方向、范围、锥体和 SH 注入语义；正式 Runtime GPU Injection 与 Shadow Visibility 分别在 V0.8 / V0.9 完成。

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

## V0.5 — Local Transfer Radiance Propagation Compute

V0.5 是 V0 的核心阶段：证明基础间接光照由 Local Transfer 正确产生，而不是由直接光照叠加产生。

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
- 直接 Injection 只有经过当前 Probe 的 Local Visibility 和 Local Transfer 才能成为 reflected Radiance；邻域传播只传播上一轮 reflected Radiance。
- 对 `1.0 / 0.5 / 0.25m` spacing 做充分收敛后的对比：细网格应减少局部空间离散误差，不得因 Probe 数量增加而改变反射能量、颜色比例或产生系统性更差的结果。
- 使用独立高分辨率 CPU reference 计算误差；spacing 变小时，Local Transfer 与收敛 Radiance 的误差不得反向增大。
- 分别记录 Surface Voxel Field、Local Transfer、一次反射、邻域传播和最终 Radiance，定位问题时不得只看最终 framebuffer。

人工视觉验证：

- Radiance Debug 能看到传播过程。
- 红墙产生红色反弹。
- 绿墙产生绿色反弹。
- 动态灯变化后 history 收敛到新结果。

---

## V0.6 — Direct Forward Surface Sampling + Edge Blend

V0.6 只负责把已经验证过的 Local Transfer Radiance 直接显示到表面。

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
- 表面重建必须明确其物理采样范围；固定 Probe 数量的插值核不能被误认为固定物理范围。
- V0 使用直接 Probe sampling / cubic reconstruction 作为验证路径。

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

## V0.8 — GPU Analytic Light Injection + Directional Shadow Visibility

原文在全局光照传输伪码中把 Directional Light 单独处理：方向光没有有限作用范围，只有当 `probe not in Shadow` 时才把 `DirectionalLightSH` 加入 `InComingLight`；随后再 gather 邻居 Radiance、应用当前 Probe 的 Local Visibility，并经过 Local Transfer Matrix 形成 reflected Radiance。本阶段严格保持这个次序。

目标：

- 保留 CPU Local LRT Builder；Local Visibility / Local Transfer / Emission 仍只在 Geometry 数据变化时由 CPU 构建并上传。
- 将正式 Runtime 的 Directional / Omni / Spot 解析灯 SH Injection 统一迁移到 GPU；CPU Injection 仅保留为数学与 GPU golden reference，避免读取 GPU Shadow Map 回 CPU。
- 首先为 Directional Light 实现逐 Probe Shadow Visibility，使被遮挡 Probe 不再成为错误的间接光源。
- Editor Scene Viewport 与 F5 Runtime 共用同一 RendererRD shadow / injection / propagation 路径，不建立 Editor 专用近似。

实现：

```text
CPU Local Geometry Builder (dirty only)
        ↓ upload
Local Visibility / Local Transfer / Emission

Shadow-casting Geometry
        ↓ rasterize
Volume Directional Shadow Map
        ↓ sample at Probe world position
DirectionalLightSH × Shadow Visibility
        ↓
Injection Buffer
        ↓
Neighbor Radiance Gather → Local Visibility → Local Transfer → Radiance A/B
```

- 在 Local LRT GPU 子系统中建立共享 Analytic Light Injection compute；每个 Probe 使用最新 Volume transform 恢复 World position，并按冻结的 SH 方向约定写入 RGB SH2 Injection。
- 为当前有效 `LocalLRTVolume3D` 与 Directional Light 建立低分辨率正交 Shadow Map。接收范围由 Volume world bounds 决定；Caster 收集必须包含 Volume 外能够沿光线方向向 Volume 投影的静态 Shadow Caster，不能只收集 Volume 内 Geometry。
- Directional Shadow projection 不依赖相机 CSM；使用稳定的 light-space bounds、depth bias、PCF 与 texel snapping，避免 Editor 相机移动改变 Probe GI 或 Volume 平移时产生阴影抖动。
- 执行顺序固定为 `Shadow rendering → Injection compute → Radiance propagation → Forward sampling`，并显式处理 RD resource barrier / layout。
- 灯光、Shadow Caster 或 Volume transform 变化只标脏对应 Shadow / Injection；不得重建 Local Visibility / Local Transfer，也不得清空已有 Radiance history。新的 Injection 通过现有 recurrence 逐步收敛。
- 原文中的 `MeshLight` 在 V0 继续由现有 Geometry emission 输入表达；本阶段不新增独立 Godot Area Light 或通用 Mesh Light 系统。
- 增加独立 Debug：Directional Shadow Visibility、shadowed Directional Injection、最终 reflected Radiance，避免把 Shadow、Local Visibility 和 LTM 问题混在最终 framebuffer 中判断。

自动验证：

- Directional light-space projection、depth compare、bias 与 PCF 边界。
- 简单隔墙中墙前 Probe 的 Directional Injection 正常，墙后 Injection 接近零；关闭阴影后与 V0.4 CPU unshadowed reference 一致。
- GPU unshadowed Directional / Omni / Spot Injection 与现有 CPU reference 一致；Directional shadowed Injection 与独立 shadow visibility reference 一致。
- 移动 / 旋转 Directional Light、Volume 或 Shadow Caster 只更新 Shadow / Injection，静态 Geometry build count 不变，Radiance history 不被清空。
- Editor Scene Viewport 与 Runtime 对相同 scene state 产生一致的 Shadow Visibility / Injection / Radiance readback。
- Editor 相机移动但 Scene、Light、Volume 不变时，Directional Probe Shadow Visibility 不变。
- 全部 Probe Shadow Visibility / Injection / Radiance 无 NaN / Inf。

人工视觉验证：

- 不运行项目时，Editor Scene Viewport 已能看到方向光阴影对间接光注入的影响。
- Cornell Box 隔墙后不再出现由未遮挡 Directional Injection 产生的间接漏光；受光区域仍能产生正确 Color Bleeding。
- 移动 Editor 相机不改变 GI；移动方向光、Volume 或 Caster 后，Editor 与 F5 Runtime 均从旧 Radiance 平滑收敛到同一结果。
- Shadow Visibility、Injection 与最终 Radiance 三种 Debug 的空间关系可解释且一致。

---

## V0.9 — Local Analytic Light Shadow Visibility（Omni / Spot）

原文把有有限范围的点光源、Spot Light 与面光源归入 Local Light 路径：只有 `probe in LocalLight` 时才把对应 `LocalLightSH` 加入 `InComingLight`，然后统一执行邻域 gather、Local Visibility mask 与 Local Transfer。原文伪码只对 Directional Light 显式写出 `probe not in Shadow`；本阶段保留其 Local Light 分类与范围判断，并把同一直接光可见性原则扩展到 Godot Omni / Spot Shadow Map，防止局部解析灯跨墙错误注入。

目标：

- 复用 V0.8 的 GPU Analytic Light Injection、Shadow resource、dirty update 与 Editor / Runtime 共享调度，不建立第二套传播路径。
- `OmniLight3D` 先按 range attenuation 判断 `probe in LocalLight`；`SpotLight3D` 再应用 range 与 cone attenuation；仅对有效 Probe 采样对应 Shadow Map。
- Shadow factor 只作用于该灯对该 Probe 的直接 SH Injection；Local Visibility 仍在 gather 后应用一次，Global Visibility 不得作为 Omni / Spot Shadow Map。

实现：

- Spot 使用灯光锥体的二维透视 Shadow Map；按 Probe world position 投影并执行 depth compare、bias 与 PCF。
- Omni 使用与 Godot RendererRD 一致的点光阴影表达（Cubemap 六面或实际 Shadow Atlas 布局）；按 `probe_world_position - light_world_position` 选择正确方向 / 面并比较径向深度。
- 每个 Local Light 只处理其 range / cone 与 Volume 相交的 Probe；V0 保持直接、确定的 reference 路径，不提前加入 clustered budget、temporal shadow cache 或多灯性能近似。
- Caster 收集以灯光与 Volume 的有效光路为准，必须包含 Volume 外能够向 Volume 内 Probe 投影的 Shadow Caster。
- 灯光 transform、color、energy、range、spot angle、shadow 参数、Caster 或 Volume transform 变化只更新受影响 Shadow / Injection；不重建 CPU Local LRT 数据，不清空 Radiance history。
- Editor Scene Viewport 只要正常渲染当前 Scene，就执行与 Runtime 相同的 `Shadow → Injection → Propagation → Forward` 路径；不得要求 F5、运行脚本或 Editor 专用 bake 才能看到结果。
- V0.9 不引入通用 Area Light。原文 `MeshLight` 继续映射到现有材质 emission / Geometry contribution；真实解析 Area Light 的采样与可见性另行规划。
- Debug 可按灯光类型隔离显示 Omni / Spot Shadow Visibility、attenuation 后 Injection 与传播后 Radiance。

自动验证：

- Omni 六个主方向及面边界的 shadow projection / depth compare 正确；Spot cone 内外、range 边界与 Shadow Map UV 边界正确。
- 隔墙 Omni / Spot：墙前 Probe Injection 正常，墙后接近零；无 Caster 时与 V0.4 CPU attenuation / cone reference 一致。
- 多灯叠加必须逐灯应用各自 Shadow Visibility，不得对汇总后的 Injection 乘单一 shadow factor。
- 移动、旋转、改 range / spot angle 或开关阴影只更新对应 Shadow / Injection；静态 LRT build count 不变，Radiance history 保留。
- Volume 平移 / 旋转后，世界灯与 Volume 一起运动时 local shadowed Injection 保持；世界灯不动时 shadowed Injection 按最新 World / Local 关系变化。
- Editor Scene Viewport 与 Runtime 的 Omni / Spot Shadow Visibility、Injection 与收敛 Radiance 一致。
- 所有 Shadow / Injection / Radiance 数据无 NaN / Inf，灯关闭后对应 Injection 清零。

人工视觉验证：

- Editor Scene Viewport 中无需运行项目即可移动 Omni / Spot，并看到墙前受光、墙后间接光抑制及新的 Radiance 收敛过程。
- Omni 穿越隔墙、Spot 转动或改变锥角时，不出现旧位置残留、跨墙注入或明显 Cubemap 面接缝。
- Directional / Omni / Spot 单独与组合启用时，Editor 与 F5 Runtime 的阴影注入、Color Bleeding 和暗部表现一致。
- 相机移动不改变 Probe Shadow Visibility；Volume 外仍无 Local GI，Edge Blend 行为不回归。

### v0 总验收

只有以下全部满足才进入 v1：

自动：

- P0 / v0 数学与机制测试全部 PASS。
- GPU 与 CPU reference 符合误差要求。
- Surface Voxel Field / Local Visibility / Local Transfer / 一次局部反射 / Radiance propagation 在多个 spacing 下通过离散一致性和收敛测试。
- 细网格对高分辨率 reference 的误差不增，且不会出现“Probe 越多、整体物理反射越弱、断层越明显或结果越错误”的反向结果。
- Directional / Omni / Spot 的范围、方向、attenuation、Shadow Visibility 与逐灯 RGB SH2 Injection 均通过独立 reference；被遮挡 Probe 不得成为未经过 Shadow Visibility 的解析灯间接光源。
- Shadow rendering、Injection compute、Radiance propagation 与 Forward sampling 在 Editor / Runtime 使用同一路径；灯光、Caster 或 Volume 变化不得触发静态 LRT rebuild 或清空 Radiance history。

人工：

- Cornell Box Local GI 明确可见。
- 动态解析灯实时影响 GI，墙后不存在由未遮挡解析灯 Injection 产生的明显间接漏光。
- 红 / 绿墙 Color Bleeding 正确。
- Volume 整体平移 / 旋转稳定。
- Edge Blend 正常。
- Editor Scene Viewport 无需运行项目即可显示正确并持续收敛的 Local LRT；与 F5 Runtime 一致。
- 移动 Editor 相机不改变 Local LRT Shadow Visibility 或收敛结果。

---

# 6. v1 — 动态物体与 Local Geometry 表示

## V1.1 — 动态物体

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

## V1.2 — Per-object Local Color SDF

目标：在 V0 Surface Voxel Field reference 已正确后，增加原文提出的逐物体 Local Color SDF Geometry source，提高同分辨率下曲面、斜面、表面距离和法线的精度；不得改变已冻结的 LTM 与 Radiance propagation 语义。

数据：

- Object-local signed distance field。
- 与 SDF 对齐的低分辨率 albedo / emission 数据。
- Object local → Volume local transform。

实现：

- 从闭合静态 Mesh / Primitive 构建或导入 object-local SDF。
- 每个 Probe 的 26 邻接查询点变换到物体 Local Space，采样距离、颜色和 emission，再构建 Surface / Local Visibility / Local Transfer 数据。
- 动态物体移动 / 旋转时复用 object-local SDF，只更新其到 Volume Local Space 的变换并执行当前阶段允许的重建。
- V0 Surface Voxel Field 保留为 correctness reference；SDF 路径必须复用同一套 LTM、Injection、Radiance propagation 和 Forward sampling。

自动验证：

- Sphere / 斜平面在相同 Probe spacing 下，SDF 的表面距离、法线和 LTM 误差低于 Surface Voxel Field。
- SDF 物体平移 / 旋转后的 Local Geometry 与重新生成的 reference 一致。
- 红 / 绿材质和 emission 的颜色通道正确。
- 切换 Geometry source 不改变空空间、能量稳定性和 Radiance recurrence。

人工视觉验证：

- 曲面与斜面附近的间接光过渡更平滑。
- 不引入新的漏光、能量漂移或旧位置残留。

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

- Screen Space Gather：以低分辨率缓存 RGB reflected GI，供 Base Pass 采样；它只能作为低频表现和性能优化，不能替代 V0 的 Local Transfer 正确性。
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
