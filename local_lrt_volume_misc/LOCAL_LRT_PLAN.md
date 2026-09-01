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
- v0 只解决：**静态 Geometry + 动态解析灯光及其 Shadow Visibility + Local Transfer 驱动的基础 Diffuse GI**，并严格分成两个半期顺序执行。
- V0 前半期只允许 `DirectionalLight3D` 对 GI 作出贡献；必须先完成与 Cycles 的方向光输入、直接光、间接光、阴影、收敛和能量缩放对照。该阶段不得同时调试 Omni / Area / Spot。
- V0 后半期仅在方向光验收完成后开始，并依次执行 Point（`OmniLight3D`）→ Area（`AreaLight3D`）→ Spot（`SpotLight3D`）三个独立子阶段；前一类灯未通过时不得并行推进后一类灯或组合灯光。
- V0 的核心验收是 Local Visibility / Local Transfer / Shadow-aware Analytic Light Injection / 反射 Radiance 的基础物理关系正确；天光遮蔽和 Global GI 输入不属于 V0 通过条件。
- Geometry Emission 必须同时覆盖两条原文职责：Base Pass 中 Emission Mesh 自身按材质可见；GI source 按原文把局部 `MeshLightSH` 加入 `InComingLight`，再经过当前 Probe 的 Local Visibility / Local Transfer；PDF 5.11 的 LTM 自发光增益独立写入 `ColorToFill`。`Emission Energy Multiplier` 只缩放 MeshLight source，不得同时缩放 LTM，否则能量会呈超线性。禁止用隐藏 Point / Omni / Area Light 替代 Emission Mesh，也禁止把 emission 作为绕过 Local Transfer 的 outgoing Radiance 直接写入。
- `MeshLightSH × Local Visibility` 使用完整 26-neighbor 方向集做非负乘积投影，避免 L1 截断在完整 opaque segment hit 上产生负 L0、抹掉真实 emissive source；解析灯和传播 Radiance 继续使用原文 Triple Product。
- Local LRT 消费的静态 `BaseMaterial3D` albedo、emission enable/color/energy 发生变化时必须自动重建对应 Local Geometry / LTM / MeshLight 数据；不得要求用户手动调用 `rebuild()`。解析灯参数变化仍只更新动态 Injection，不触发静态 rebuild。
- 遵循原文的 CPU / GPU 分工：CPU 根据局部 Geometry 构建 Local Visibility / Local Transfer，GPU 完成解析灯光注入、Shadow Visibility、Radiance gather 与传播；不得为了采样 GPU Shadow Map 而把静态 LRT Builder 整体迁移到 GPU。
- Probe 密度是空间离散化参数，不是独立的质量开关；改变 spacing 时，Local Geometry 离散化、采样权重、LTM 能量、传播收敛和表面重建必须保持一致。
- 首版优先直接、明确、可验证的实现；不要在功能阶段顺手做性能优化。
- 数学 / 算法机制验证使用自动单元测试。
- 最终视觉效果验证必须由人眼确认；AI 不得自行判定视觉 PASS。
- 每个阶段独立编译、测试、提交；一次只处理一个主要问题。
- 除 V1.2 为保证动态 Geometry 可用而明确要求的逐物件 SDF 复用、Dirty Region 与局部上传外，其余性能优化统一放到最后的 v4。

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
Per-object Color SDF     signed distance + albedo / emission
LocalVisibilitySH        vec4
Radiance R/G/B A/B       vec4 × RGB ping-pong
LocalTransferMatrix      RGB mat4
Injection                RGB SH2
inside_solid             bool per Radiance Probe
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

原文并不是先把 Geometry 压成与 Radiance Probe 共用的二值 occupied Grid，再由 Probe 读取该 Grid；它把逐物体 Color SDF、Height Field 或 Precomputed Local Transfer 作为独立 Local Geometry Source。每个 Probe 的局部查询位置变换到物体 Local Space，直接读取 distance / color / emission / transfer，再构建 Local Visibility 与 Local Transfer。

V0 保持完整 26-neighbor 的方向与 Radiance 传播拓扑，但 Local Geometry Field 与 Radiance Probe Grid 必须分离：`probe_spacing` 只决定 Radiance Probe 的空间采样位置；逐物体 Local Geometry Resource 拥有自己的 voxel size / resolution。spacing 变小时，增加的是 Probe 对连续 Local Geometry / LTM 的空间采样精度，不得通过改变传播拓扑、增加 iteration、扩大固定 Probe 数滤波或添加随机 dither 来掩盖 first-bounce 误差。

Surface hit 与 Radiance Probe validity 必须分离。局部方向样本只用 coverage 判断是否取得原文的一个完整 `ColorToFill` 样本，不得把实体体积分数继续乘入 Visibility / LTM 能量；只有 Probe center 经 Local Geometry Source 明确判断位于实体内部时才标记 `inside_solid`。

---

# 4. P0 — 数学参考与测试环境

## P0.1 — 冻结数学与传播约定

目标：GPU 接入前先把所有容易产生轴向、矩阵和 SH 歧义的规则冻结。

实现：

- SH2 basis 顺序与归一化。
- SH encode / evaluate。
- L1 Surface Irradiance 禁止直接把线性重建负值硬截断为零。Forward 与 CPU reference 统一使用 Geomerics / Enlighten non-linear L1 reconstruction：先把 SH 转为 ambient `A` 与 directional vector `D`，令 `r = clamp(length(D) / (2A), 0, 1)`、`q = 0.5 + 0.5 dot(N, normalize(D))`、`p = 1 + 2r`、`a = (1-r)/(1+r)`，最终 `E = A * lerp((1+p) * q^p, 1, a)`。该重建必须非负并保持 `A` 对应的球面平均能量。
- Non-linear L1 参考：Enlighten SDK `Light probe evaluation`（https://documentation.enlighten.siliconstudio.co.jp/wiki/spaces/SDK402/pages/2485157996/Light%2Bprobe%2Bevaluation）与 Peter-Pike Sloan `Deringing Spherical Harmonics`（https://research.activision.com/publications/2020-03/deringing-spherical-harmonics）。
- SH Triple Product。
- SH2 rotation。
- `LocalTransferMatrix` 行列约定。
- `B'` 的旋转约定并用数值测试确认。
- Local / World / Grid / UVW 转换。
- Probe linear index。
- 26 邻居方向与权重。
- 区分 Local Geometry 构建阶段和 Radiance propagation 阶段：前者构造 Local Visibility / Local Transfer，后者在 Probe 邻接图上传播 Radiance。
- 26-neighbor 局部采样在 spacing 变化后的方向、立体角权重和能量归一化：inverse-distance 权重已归一化为 1，再乘 `4π`；不得改用均匀 `4π / 26`。
- 原文 LTM 外积冻结为：`SampleDir = normalize(sample_position - probe_center)`，`SampleBasis = sh_basis(SampleDir)`，`GetSH2PIDivDFT(d) = (Y00, Y1 * d * 2/3)`，Diffuse 输出使用 `GetSH2PIDivDFT(-SampleDir)`；不得用存储的 surface normal 代替 `SampleBasis`。
- V0 LTM 只构造一次局部反弹。原文 Neumann `InfBoundT = (I - T)^-1 - I` 只影响收敛速度，不作为 V0 通过条件。
- `LocalVisibilitySH` 的语义：明确是 visibility，不得与 occlusion 混用。
- Radiance gather 使用邻居 **Local** Visibility：`Trpd(otherRadiance, -otherLocalViSH)`。Global Visibility 只留给后续天空遮蔽，不得作为该 mask，也不得充当解析灯 Shadow Map。
- 当前 Probe 的 Local Visibility 对 gathered Radiance 只应用一次。空空间 continuation 直接使用该方向性过滤结果；不得再乘 Local Visibility 的 SH0 / 平均 open fraction，否则平面按 coverage 平方衰减、内角被重复遮蔽。
- Radiance recurrence。
- 明确空空间 Radiance 如何继续传输。
- PDF 5.11 的 transfer emission 并入 `ColorToFill`（albedo + transfer emission），使 LTM 能量可以 `> 1`；MeshLight source emission 与该增益分开保存。不得再把 emission 当成绕过 LTM 的 outgoing Radiance。解析灯 Injection 仍必须经过当前 Probe 的 Local Visibility 与 LTM。
- 明确 spacing 对照实验：固定同一 Volume size、几何、材质和灯光，分别使用 `1.0 / 0.5 / 0.25 / 0.125m` requested spacing；每次重建并等待 Radiance 收敛，再分别比较 Local Transfer、一次反射和最终表面 GI。该实验不是比较同一 Probe-hop 数下的瞬时画面。当 Geometry Field 与 Probe Grid 分离后，occupancy-grid fixture 只验证 recurrence 数学；连续几何的 spacing 实验必须固定 Geometry voxel size。若条纹只缩短周期而振幅不降，必须分别 A/B Local Transfer 与 Forward reconstruction，不能把周期缩短记为密度收敛 PASS。

必须通过的单元测试：

- 常量 SH。
- 单方向 lobe。
- Non-linear L1：常量输入保持 `πL`，高方向性输入在任意法线方向均非负，CPU 与 Forward Shader 数值约定一致。
- Triple Product 常量乘法。
- 90° / 180° rotation。
- Local ↔ World。
- Grid index / UVW。
- 简单 Transfer Matrix。
- `SampleDir` / `GetSH2PIDivDFT(-SampleDir)` 外积与转置、符号对照。
- 26 邻域 inverse-distance 权重之和为 1，乘 `4π` 后覆盖全立体角。
- 空空间 Radiance 可以跨 Probe 传播。
- 无光源时能量不发散并最终衰减。
- gather 使用邻居 Local Visibility，不使用 Global Visibility。
- 同一平面 / 墙角的 Local Visibility / Local Transfer 在不同 spacing 下保持物理响应稳定，并随 spacing 细化收敛。
- spacing 变小只能增加几何采样密度，不得改变同一连续几何的总反射能量。

完成条件：数学约定冻结，后续 CPU / GPU 都严格遵循。

---

## P0.2 — CPU 最小 Reference Solver

只做用于验证算法的最小 CPU Grid，不扩展成正式 Runtime 子系统。

实现：

- 规则 Probe Grid。
- 人工 occupancy / albedo / emission 输入：这是 recurrence / Injection / 传播的数学 golden，occupancy 在此就是 Geometry，不代替 V0.2 的 Color SDF。
- 另备连续 / 解析 Color SDF reference，只用于 Local Visibility / LTM / first-bounce 对照。
- Local Visibility。
- Local Transfer Matrix：`SampleDir` 外积；仅 transfer emission 写入 `ColorToFill`，MeshLight source emission 单独编码。
- Radiance ping-pong。
- Directional / Omni / Spot 简单 injection。
- 完整 26 邻居传播；gather mask 为邻居 Local Visibility。

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
visibility_iterations
propagation_iterations
energy
edge_blend_distance
debug_draw
debug_mode
debug_probe_scale
rebuild()
```

完成：

- 节点注册。
- Local AABB。
- Probe Grid。
- RenderingServer RID 创建 / 释放。
- Transform / property 同步。
- Editor gizmo：Bounds + Probe Sphere。`debug_probe_scale` 为世界单位；细网格目视时必须缩小或随 `actual_spacing` 缩放，不得把 Debug 拥挤当成精度回退。

自动验证：

- 创建 / 删除 / 保存 / 加载。
- resolution / spacing / probe positions。
- RID 生命周期。

人工视觉验证：

- Cornell Box 中 Bounds 与 Probe Grid 对齐。
- Volume 旋转后 gizmo 正确。

---

## V0.2 — 静态 Local Geometry Source → Visibility / Transfer

只收集能够影响 Volume 的静态 Geometry。首版材质只要求 `StandardMaterial3D` albedo 与 emission。

本阶段按原文建立彼此分离的数据：

```text
Per-object Local Geometry Source
- object-local distance / surface coverage
- albedo / emission
- surface normal
- object local → Volume local transform

Radiance Probe
- inside_solid / valid
- LocalVisibilitySH
- RGB LocalTransferMatrix
```

实现：

- 为闭合静态 Mesh / Primitive 构建最小 object-local Color SDF：voxel 中心存储 signed distance，并保存与其对齐的低分辨率 albedo / emission；其 voxel size / resolution 属于 Geometry Resource，不与 `probe_spacing` 绑定。Box / Sphere 可用解析 SDF 作为精确对照；任意闭合三角网格做最近表面距离 + 内外判定。V0 Color SDF 要求闭合体，Cornell Box 使用 BoxMesh，不把 QuadMesh 当作正式 SDF 输入。
- 收集范围是「能影响 Volume 内 Probe 邻域的静态 Geometry」：物体 AABB 与 Volume 沿一格 `actual_spacing` 外扩后的 bounds 相交即收集，不能只收严格落在 Volume 内的物体。
- 每个 Radiance Probe center 直接查询相关 Local Geometry Source；合并后 `sdf < 0` 才设置 `inside_solid`。表面 fractional coverage / 表面带上的 Probe 不得使整个 Probe 失效，也不得跳过该 Probe 的 Local Visibility / LTM 构建。
- 每个有效 Probe 的完整 26 个查询位置为 `probe_center + neighbor_offset * actual_probe_spacing`，变换到对象 Local Space 后直接采样 distance / coverage / color / emission / normal；不得先压成 Probe-aligned occupied voxel 后再二次查询。按原文，一个被取用的 `ColorToFill` 点是完整方向样本；中心与方向端点对同一 Color SDF 的符号 / 外向法线表明该段穿过表面时才取用，禁止把仅接近端点但未穿越表面的下一层 Probe 计入 LTM。
- 对每个有效方向样本按原文构建 LTM：`SampleDir = normalize(sample_position - probe_center)`，`SampleBasis = sh_basis(SampleDir)`，Diffuse 输出使用 `GetSH2PIDivDFT(-SampleDir)`；`ColorToFill = albedo + transfer_emission`。MeshLight source emission 不参与 `ColorToFill` 的能量缩放。在现有 row-major `output = B * input` 约定下实现等价 SH 外积，并用独立数值测试排除转置或符号错误。
- Local Visibility 与 RGB Local Transfer 使用同一组方向样本和冻结的 `4π * inverse-distance` 权重。多个 Geometry Source 重叠时取 Volume-local signed distance 最小者，颜色、emission、法线来自该胜出 Source。object transform 含缩放时，distance 必须按逆转置法线长度换算，normal 必须按 inverse-transpose 变换；不得直接用 object-space distance 与 `Basis.xform(normal)`。
- 现有保守三角形 Surface Voxel Field 只保留为离散回归对照，不再作为正式 LTM Runtime 输入；不得保留 silent fallback。
- Geometry 或材质变化只重建受影响的 Local Geometry / Visibility / Transfer；灯光变化不得触发该路径。物体 Local Color SDF 在物体自身网格/材质未变时保持；仅 object → Volume transform 变化时复用 SDF、重查 Probe。

spacing 语义：

- `probe_spacing` 只改变 Radiance Probe 数量和查询位置，不改变 Geometry Resource 的 voxel size，也不改变 26-neighbor 拓扑。
- 方向查询不再使用 Probe-cell footprint 体积分数。对每个既定端点只直接读取同一 Geometry Source 的中心与端点 SDF：端点位于实体内，或两端位于实体外但外向法线相反且两端 surface distance 之和不大于段长时，判定该段穿越薄表面并取得一个完整 `ColorToFill` 样本。该判定不增加中间采样、ray march 或运行时 Trace，不改变 Color SDF voxel size、26 个查询位置、LTM 权重或 `inside_solid`。
- 独立提高 Geometry Resource 分辨率应降低 distance / normal / coverage 误差；独立提高 Probe 密度应降低 Local Visibility / LTM 的空间采样误差。
- 两者必须分别测试，不能把 Geometry 精度与 Radiance Probe spacing 混成同一个质量参数。

自动验证：

- Cube / Wall 与连续解析 CPU reference 一致。
- 大尺寸旋转平面、斜平面与 Sphere 的 SDF distance、normal、coverage 随 Geometry Resource 分辨率提高而收敛。
- 固定高精度 Geometry Resource，分别使用 `1.0 / 0.5 / 0.25 / 0.125m` Probe spacing；沿旋转平面切向分别记录 Local Visibility / LTM 系数、Forward reconstruction 方差、最大相邻跳变和条纹周期。若提高密度只缩短条纹周期，则必须检查 Forward 有效 Probe 集合是否随 cell phase 离散切换；不能把周期缩短记为精度根治。
- 固定 Probe spacing，单独改变 Geometry voxel size；Local Geometry 与 LTM 误差必须随 Geometry voxel size 变小而不增。
- `coverage > 0` 的部分表面命中按原文取得一个完整 `ColorToFill` 方向样本，但不得使 Radiance Probe 失效；仅 Probe center 的 signed distance `< 0` 才设置 `inside_solid`，并禁止其参与 Radiance / Injection / Forward reconstruction。
- 红色表面的 R transfer > G/B；空空间 Probe transfer = 0、Local Visibility 为 fully visible；Volume / object Local 坐标正确。
- 带 transfer emission 的表面可使 LTM 能量 `> 1`；MeshLight source 仍经同一 LTM 成为 reflected Radiance，不得再走绕过 LTM 的 `emissive_injection` 旁路。
- 原文 `SampleDir` / `-SampleDir` 与 row-major 外积通过平面、内角和旋转场景 reference。
- 所有构建结果确定且可重复；静态 LTM 构建不得使用每帧随机采样。

人工视觉验证 Debug：

- 分别显示 Local Geometry distance / fractional coverage、inside-solid Probe、Local Visibility 与 Local Transfer，四种语义不得混合着色。
- Cornell Box 的旋转 Box 在 `1.0 / 0.5 / 0.25m` 下不出现随 Probe 密度增加而增强的周期条纹；细网格必须显示更细且更稳定的 first-bounce 响应。

---

## V0.3 — GPU Resources + Local Data Upload

建立最小 RD 数据：

- Local Visibility。
- Local Transfer Matrix。
- Radiance R/G/B A/B。
- Direct Injection。
- `inside_solid` / Probe valid 标志。不得再用 `dot(LocalVisibility, LocalVisibility) ≈ 0` 推断 occupied。

实现：

- 为 Local Visibility、Local Transfer Matrix、Injection 和 `inside_solid` 建立 GPU storage buffer。
- 保持 CPU Reference 与 GPU 数据布局一致。
- 为后续 Radiance propagation 与 Forward sampling 提供最直接的资源生命周期和上传路径。
- 已实现的 Global Visibility compute 可保留，但不是 V0 通过条件；V0 Radiance gather 不得依赖它。

自动验证：

- CPU → GPU 上传 / readback 与 reference 数值一致。
- Probe 数量、buffer 长度和 spacing 改变后的资源重建正确。
- `inside_solid` 与 CPU 标志一致；部分 coverage Probe 不得被上传成失效 Probe。
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

灯光变化只更新 Injection，不重建静态 Geometry / Local Transfer。本阶段先冻结原文中 Directional / Local Light 的位置、方向、范围、锥体和 SH 注入语义；正式 Runtime GPU Injection 与 Shadow Visibility 分别在 V0.8 / V0.9 完成。仅 `inside_solid` Probe 跳过 Injection；部分表面覆盖不得跳过。Geometry emission 走 V0.2 LTM，不在本阶段再写一条 outgoing emission 旁路。本阶段的未遮挡 CPU Injection 是 V0.8 的 golden reference。

自动验证：

- World → Volume Local position/direction。
- Omni attenuation。
- Spot cone。
- Directional direction。
- 灯关闭后 injection 清零。
- `inside_solid` Probe Injection 为 0；表面旁有效 Probe Injection 非 0。

人工视觉验证：

- Cornell Box 移动灯时 GI 响应。
- 改灯颜色后反弹颜色响应。
- 开关灯后 GI 重新传播和收敛。

---

## V0.5 — Local Transfer Radiance Propagation Compute

V0.5 是 V0 的核心阶段：证明基础间接光照由 Local Transfer 正确产生，而不是由直接光照叠加产生。

实现：

```text
Analytic Injection
        ↓
Gather 26 Neighbor Radiance
        ↓
Neighbor Local Visibility mask  (Trpd, -ViSH)
        ↓
Current Probe Local Visibility
        ↓
Local Transfer Matrix
        ↓
Empty-space transmission
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
- 直接 Injection 只有经过当前 Probe 的 Local Visibility 和 Local Transfer 才能成为 reflected Radiance；邻域传播只传播上一轮 reflected Radiance。gather 只使用邻居 Local Visibility，不使用 Global Visibility。
- Empty-space continuation 使用当前 Probe Local Visibility 过滤后的 `gathered`，不得再乘 `empty_space_transmission`；该标量只保留为局部几何统计 / fully-visible 快速判定，不进入 recurrence。
- Geometry MeshLight source 只作为 `InComingLight` 并通过当前 Probe 的 LTM 进入 Radiance；PDF 5.11 transfer emission 仅改变 LTM，删除绕过 LTM 的 `emissive_injection` outgoing 旁路。
- 仅 `inside_solid` Probe 跳过传播。
- 对 `1.0 / 0.5 / 0.25m` spacing 做充分收敛后的对比：固定高精度 Geometry Resource，细网格应减少局部空间离散误差，不得因 Probe 数量增加而改变反射能量、颜色比例或产生系统性更差的结果。
- 使用独立高分辨率 CPU reference 计算误差；spacing 变小时，Local Transfer 与收敛 Radiance 的误差不得反向增大。
- 分别记录 Local Geometry distance / coverage / inside-solid、Local Visibility、Local Transfer、一次反射、邻域传播和最终 Radiance，定位问题时不得只看最终 framebuffer。

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
Grid Position at the actual surface
        ↓
Cubic B-spline RGB SH Sample on the receiver side
        ↓
Skip inside_solid Probe
        ↓
Non-linear L1 Diffuse Evaluate
        ↓
Diffuse Indirect
```

实现：

- Volume Bounds 判断。
- `edge_blend_distance`。
- Volume 边缘向外平滑衰减。
- 表面位置不得沿法线固定偏移一个 Probe cell；该偏移会使接触处先出现暗间隔，再在外侧形成脱离表面的光环。
- Cubic gather 只使用接收面外侧且非 `inside_solid` 的 Probe，并在该有效域归一化，保留从几何接触边缘连续开始的少量间接光。
- RGB SH 表面求值使用 P0.1 冻结的 non-linear L1 reconstruction；不得用 `max(linear_l1, 0)` 制造平面内的零值断层。
- 表面重建必须明确其物理采样范围；cubic 核定义在 Probe index 空间，物理半径随 `actual_spacing` 缩放，不得被当成固定世界半径。
- V0 使用直接 Probe sampling / cubic B-spline reconstruction 作为验证路径；不用 trilinear，也不做 Screen Space Gather。
- 只排除真正 `inside_solid` Probe；不得用 Local Visibility 长度、coverage 或零 SH 当作 occupied 丢弃。部分覆盖表面 Probe 必须参与重建。

自动验证：

- World → Local → UVW。
- Receiver-side cubic gather 在接触边缘连续，不产生一格暗间隔或 detached halo。
- Non-linear L1 normal evaluate 保持平均能量且不会产生负辐照度；高方向性 Probe 不得在同一平面形成负值硬截断边界。
- edge weight。
- `inside_solid` 丢弃与 CPU 标志一致；`coverage > 0` 但非 `inside_solid` 的 Probe 仍贡献采样。
- 旋转平面切向 Forward sample 方差随 Probe spacing 变小而不增。
- 没有 LocalLRTVolume 时原渲染结果不改变。

人工视觉验证：

- Cornell Box 红 / 绿 Color Bleeding 可见。
- 暗部存在合理间接照明。
- 旋转 Box 的同一平面内不存在由 L1 负值硬截断形成的明显亮暗断层；与 Cycles 对照时允许低频精度差异，但 GI 梯度必须连续。
- Volume 外无 Local GI。
- Volume 边缘没有硬切。
- 细网格目视时 Debug Probe 不得以固定世界半径铺满画面；Radiance Debug 不得把非 `inside_solid` 的表面 Probe 画成洋红色。

---

## V0.7 — Local Space 平移 / 旋转 + Editor / Runtime Parity

整体移动：

```text
Cornell Box + LocalLRTVolume3D
```

要求：

- Probe / Transfer / Visibility / Radiance 数据仍在 Volume Local Space。
- Cornell Box 与 Volume 一起平移 / 旋转时不重新构建 Local GI，也不重新生成 object-local Color SDF。
- 仅 Volume 相对 Geometry 运动时，复用已有 Color SDF，只更新 object → Volume transform 并重查 Local Visibility / LTM；不得把这种情况当成 V0.7 的「不重建」用例。
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

## V0.8 — V0 前半期：Directional Light GI 完整对齐

原文在全局光照传输伪码中把 Directional Light 单独处理：方向光没有有限作用范围，只有当 `probe not in Shadow` 时才把 `DirectionalLightSH` 加入 `InComingLight`；随后再 gather 邻居 Radiance、应用当前 Probe 的 Local Visibility，并经过 Local Transfer Matrix 形成 reflected Radiance。本阶段严格保持这个次序。

冻结修复记录、Cycles 场景、机器可读配置、线性 pass 与 LookDev 对照位于 `LOCAL_LRT_V08_DIRECTIONAL_FIX_REPORT.md` 和 `benchmarks/directional_cornell_v08/`；后续 Directional 回归必须使用该输入口径，不得用同名 UI 数值相等替代物理能量相等。

目标：

- 保留 CPU Local LRT Builder；Local Visibility / Local Transfer / Emission 仍只在 Geometry 数据变化时由 CPU 构建并上传。
- 正式 Runtime 的通用 GPU Analytic Light Injection 基础设施可以保留，但本阶段的场景、reference、自动验收和人工验收只启用 Directional Light；Omni / Area / Spot 必须关闭且不计入任何 GI 对照。
- 首先为 Directional Light 实现逐 Probe Shadow Visibility，使被遮挡 Probe 不再成为错误的间接光源。
- 在相同 Cornell Geometry、Lambertian 材质、黑色 World、相机、分辨率、AgX / exposure 和方向光辐照度下，与 Blender Cycles 建立一对一 reference；分别核对直接光、GI-only、合成结果和能量缩放。
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
- 本阶段禁用 Geometry emission，并且不新增独立 Area Light；Area Light 留到 V0 后半期的独立子阶段。
- 增加独立 Debug：Directional Shadow Visibility、shadowed Directional Injection、最终 reflected Radiance，避免把 Shadow、Local Visibility 和 LTM 问题混在最终 framebuffer 中判断。
- 原文附录的 Local Transfer Matrix 一次反弹离散式使用 `Factor = 4π / WeightSum`，其中 `WeightSum` 是参与的 `ColorToFill` 样本数。V0.8 的 CPU A/B 已排除 equal-weight LTM 与 Neumann 局部无限反弹项；两者均不能修复近场过亮、阴影区过暗的相反偏差。
- 全局 transport 必须把 26 个邻居当作离散方向通道：先用邻居的 antipodal Local Visibility 遮罩 Radiance，再沿 `normalize(neighbor_offset)` 求非负方向 Radiance，最后以 `4π × normalized inverse-distance weight` 重投影为 SH2。不得直接平均整组邻居 SH coefficient，否则每个 Probe hop 都会稀释方向能量并造成阴影区 transport 不足。
- Directional Injection 的能量口径固定为：Godot Directional Energy 是 Lambertian diffuse radiance，对应 incident irradiance 为 `π × energy`；共享 SH encoder 使用 `2π`，因此 Directional-only encoder 输入必须乘 `1/2`。Omni / Area / Spot 不在 V0.8 修改。
- Forward `local_lrt_evaluate_diffuse` 输出 irradiance，而 Base Pass 后续还会乘 surface albedo；采样结果必须先乘 `1/π` 转回 Lambertian diffuse radiance。Non-linear L1 reconstruction 的方向性归一化使用 diffuse transfer lobe 上限 `|D| / A = 4/3`，不得沿用 delta-light 的 `2`。
- 原文的 `InfBoundT = (I - T)^-1 - I` 是局部无限反弹加速项；当前 single-bounce `T` 加跨帧全局 recurrence 已不是“仅一次反弹”，且 `decay_per_meter = 1`。Neumann A/B 对 Cornell 目标 ROI 影响近零，不作为 V0.8 能量匹配条件，也不得用增加 iteration 或全局 energy multiplier 掩盖 operator 误差。
- V0.8 Directional 能量匹配完成后，表面条纹作为同阶段的独立精度子项处理。禁止在 Radiance A/B 上增加多轮 Laplacian / Gaussian 预滤波；Forward 必须直接读取物理传播场。
- Forward cubic reconstruction 的完整支撑域当前沿接收面 local normal 外移 `1.5 × min(actual_probe_spacing)` 后，对所有非 `inside_solid` Probe 使用原始 cubic 权重归一化。不得使用 Global Visibility 引导局部重建；Local Visibility 的归一化方向、角平分方向和一阶矩切向偏移 A/B 在 `0.5m` 下分别产生粗网格圆弧轮廓或无可见改善，均须回退，不能把实验路径写成规范。
- `1.5 cell` 来自当前 4-tap cubic B-spline 的半支撑宽度，不是 Radiance blur；Radiance A/B、Local Visibility 与 Local Transfer 数据均保持原样。Precomputed / Traced LTM 仍是未来更高局部传输精度路径，但 A/B 已证明它不是当前表面周期条纹的根因。
- 原文没有规定 Base Pass 的 Probe-volume 重建闭合；当前 normal bias 是 Godot Forward 的工程重建路径，不属于原文 Radiance / Visibility propagation。转角宽暗带在 Cycles 对照下仍不合格，后续修复必须独立量化并同时通过 `0.25 / 0.5m`，不得回写传播场掩盖问题。
- Global Visibility 仍只用于后续天光遮蔽；Screen Space Gather 仍只作为后续屏幕空间细节/带宽路径。两者均不进入当前 Directional surface precision 修复的正确性条件。

自动验证：

- Directional light-space projection、depth compare、bias 与 PCF 边界。
- 简单隔墙中墙前 Probe 的 Directional Injection 正常，墙后 Injection 接近零；关闭阴影后与 V0.4 CPU unshadowed reference 一致。
- GPU unshadowed Directional Injection 与现有 CPU reference 一致；Directional shadowed Injection 与独立 shadow visibility reference 一致。
- Cornell / Cycles 的未遮挡 Lambertian reference patch 在线性 HDR 中一致；方向光能量按两端实际辐照度约定换算，不以同名 UI 数值相等代替物理输入相等。
- 对同一组 Injection / Local Visibility / LTM，CPU 分别执行原文 equal-weight LTM、当前 inverse-distance LTM、原文 unnormalized neighbor sum、当前 normalized gather；记录近场受光面、阴影面和远墙的 reflected Radiance，并与 Cycles GI-only 的相同 ROI 比值对照。
- Directional 对齐顺序冻结为 `Direct → GI-only → Combined`。Direct 通过后不得再改灯强、材质、曝光或 tone mapping；GI-only 先比较线性 HDR 的表面 irradiance、总能量和空间分布，Combined 只作为最终视觉验收。
- GI-only 不要求复制 Cycles 的高频 Monte Carlo 细节，但必须满足：灯强缩放近似线性；直接受光区不能靠过强局部反射补偿阴影区缺能；阴影面 / 远墙保持连续正能量；各代表 ROI 不得再出现约 `5×` 过亮与约 `10×` 过暗的相反误差。
- 移动 / 旋转 Directional Light、Volume 或 Shadow Caster 只更新 Shadow / Injection，静态 Geometry build count 不变，Radiance history 不被清空。
- Editor Scene Viewport 与 Runtime 对相同 scene state 产生一致的 Shadow Visibility / Injection / Radiance readback。
- Editor 相机移动但 Scene、Light、Volume 不变时，Directional Probe Shadow Visibility 不变。
- 全部 Probe Shadow Visibility / Injection / Radiance 无 NaN / Inf。
- 非均匀缩放几何的 Volume-local signed distance / normal 通过 CPU 测试。
- `0.25 / 0.125m` 对照必须区分数据精度与 Forward reconstruction：密度只缩短周期、Trace LTM 不改变周期条纹时，修复必须落在接收面连续重建，不能继续扩大源端采样成本。

人工视觉验证：

- 不运行项目时，Editor Scene Viewport 已能看到方向光阴影对间接光注入的影响。
- Cornell Box 隔墙后不再出现由未遮挡 Directional Injection 产生的间接漏光；受光区域仍能产生正确 Color Bleeding。
- 移动 Editor 相机不改变 GI；移动方向光、Volume 或 Caster 后，Editor 与 F5 Runtime 均从旧 Radiance 平滑收敛到同一结果。
- Shadow Visibility、Injection 与最终 Radiance 三种 Debug 的空间关系可解释且一致。

---

## V0.9 — V0 后半期：局部解析灯逐类对齐

只有 V0.8 的 Directional Light GI 一对一验收完成后才进入本阶段。固定执行顺序如下，每次只处理一种灯：

1. **V0.9A — Point / Omni**：对齐 range、距离衰减、Shadow Visibility、Injection、传播与 Cycles 点光 reference。
2. **V0.9B — Area**：对齐面积、形状、方向、归一化能量、面采样、软阴影、Injection 与 Cycles Area Light reference。
3. **V0.9C — Spot**：对齐 range、cone、角度衰减、Shadow Visibility、Injection、传播与 Cycles Spot reference。

每个子阶段必须分别通过 direct-only、GI-only、合成结果和能量缩放验证；三个子阶段全部完成后才做组合灯光测试。

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
- V0.9B 引入当前 Godot `AreaLight3D` 的最小解析 GI 路径；不得以材质 emission 或 Point Light 近似代替 Area Light 的面积、方向和软阴影 reference。
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
- Omni / Area / Spot 必须按 V0.9A → V0.9B → V0.9C 分别通过；全部单灯通过后，Directional / Omni / Area / Spot 组合启用时，Editor 与 F5 Runtime 的阴影注入、Color Bleeding 和暗部表现一致。
- 相机移动不改变 Probe Shadow Visibility；Volume 外仍无 Local GI，Edge Blend 行为不回归。

### v0 总验收

只有以下全部满足才进入 v1：

自动：

- P0 / v0 数学与机制测试全部 PASS。
- GPU 与 CPU reference 符合误差要求。
- Independent Local Geometry Field / Local Visibility / Local Transfer / 一次局部反射 / Radiance propagation 在多个 Geometry voxel size 与 Probe spacing 下分别通过离散一致性和收敛测试。
- 细网格对连续 / 高分辨率 reference 的误差不增；旋转平面切向的 coverage、LTM、first-bounce 与 Forward sample 方差必须下降，且不得出现“Probe 越多、条纹越明显、整体物理反射越弱、断层越明显或结果越错误”的反向结果。
- fractional coverage、`inside_solid` 与 Radiance Probe validity 语义完全分离；不得由 `coverage > 0` 派生二值 Probe 失效。
- 静态 Local Geometry / LTM 构建完全确定且可重复；V0 不使用 temporal/random dither，完整 26-neighbor 始终作为后续优化的 golden reference。
- PDF 5.11 transfer emission 并入 LTM `ColorToFill`；MeshLight source emission 独立编码，不得存在绕过 Local Transfer 的 outgoing emission 旁路。
- Emission Mesh 的静态 `MeshLightSH` 必须由 Geometry emission 构建并作为当前 Probe 的 incoming source；Emission-only 场景关闭 Directional / Omni / Area / Spot 后仍应产生非零初始 Radiance，并按 26-neighbor 路径向外传播。
- Radiance gather 使用邻居 Local Visibility；Forward receiver-side cubic 重建只排除 `inside_solid`，不得用 Local Visibility 长度推断 occupied；表面 SH 使用 non-linear L1 reconstruction，禁止线性负值硬截断。
- Directional / Omni / Area / Spot 的范围、方向、attenuation、Shadow Visibility 与逐灯 RGB SH2 Injection 均按阶段通过独立 reference；被遮挡 Probe 不得成为未经过 Shadow Visibility 的解析灯间接光源。
- Shadow rendering、Injection compute、Radiance propagation 与 Forward sampling 在 Editor / Runtime 使用同一路径；灯光、Caster 或 Volume 变化不得触发静态 LRT rebuild 或清空 Radiance history。
- Emission Mesh 自动验收必须独立关闭所有解析灯：验证 mesh-light buffer 非零、首轮 Local Transfer 后 Radiance 非零、后续 Probe-hop 扩散、关闭 emission 后清零，并确认能量缩放单调且无 NaN / Inf。
- 自动验收必须在不显式调用 `LocalLRTVolume3D.rebuild()` 的情况下修改 Emission Energy Multiplier，并确认下一次内部处理自动更新 Probe emission 与 GPU Radiance。
- Emission Energy Multiplier 的量化标准冻结为：完整 Geometry segment hit 与非负 Local Visibility 投影下，BaseMaterial3D 到 MeshLight source 使用 `2.0` 的 Cornell/Cycles 能量适配系数；Godot Multiplier `8` 对照 Cycles Strength `8`。固定材质颜色与 LTM 时，`8 → 16` 的 GPU Radiance 必须满足 `2× ± 1%`，不得通过再次放大 `ColorToFill` 获得亮度。

人工：

- Cornell Box Local GI 明确可见。
- 动态解析灯实时影响 GI，墙后不存在由未遮挡解析灯 Injection 产生的明显间接漏光。
- 红 / 绿墙 Color Bleeding 正确。
- Volume 整体平移 / 旋转稳定。
- Edge Blend 正常。
- Editor Scene Viewport 无需运行项目即可显示正确并持续收敛的 Local LRT；与 F5 Runtime 一致。
- 移动 Editor 相机不改变 Local LRT Shadow Visibility 或收敛结果。
- Emission Mesh 自身必须呈现 authored emission，周围表面出现对应颜色的 Local GI；独立场景不得依赖同位置 Area Light 或其他隐藏解析灯伪造照明。

---

# 6. v1 — 动态物体与 Local Geometry 表示

## V1.1 — 动态物体

继续使用同一个 Cornell Box，仅增加必要的可移动测试 Cube。

目标：验证相对于 Volume 真正移动的 Geometry 可以 Receive / Contribute GI。

实现：

- 动态物体 Receive Local GI。
- 动态物体参与 Local Geometry 构建。
- 动态物体移动 / 旋转后重新构建受影响 Local GI 数据。
- V1.1 以动态 MeshInstance3D 的 instance / transform / visibility / mesh / GI mode / lifecycle 状态快照驱动完整重建；加入、移除、显隐或切换 GI mode 必须与移动 / 旋转一样自动失效，不要求用户手动 `rebuild()`。
- 更新 Local Visibility。
- 更新 Local Transfer Matrix。
- 更新 Emission。
- 重置 / 重新传播必要的 Radiance state。
- V1.1 full rebuild 上传新静态数据时必须清零旧 Radiance / Injection buffer，确保旧位置不残留；逐物件 SDF 复用与受影响 Probe 更新仍属于 V1.2。

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

## V1.2 — Dynamic Local Geometry Source Reuse

目标：复用 V0.2 已验证的 per-object Local Color SDF，把动态物体变化限制为 object local → Volume local transform 与受影响 Probe 数据更新；不得改变已冻结的 Local Geometry 查询、LTM 与 Radiance propagation 语义。

实现：

- 为每个 Geometry Source 缓存 object-local Color SDF；动态物体仅平移 / 旋转时复用该资源，不重新生成物体内部数据。Mesh、材质或会改变 object-local 几何内容的状态变化仍必须重建对应 SDF。
- 以变化 Source 在 Volume local 中旧 / 新影响 AABB 的并集生成 Dirty Region，只重建该范围内的 Local Visibility / Local Transfer / MeshLight / `inside_solid`，同时清理旧位置贡献。
- Dirty Region 查询必须保持 V0.2 的 26-neighbor、最小 signed distance 胜出、颜色 / emission / normal 来源及重叠 Source 次序语义；允许用 Source surface AABB broadphase 跳过不可能命中的 SDF 查询，但不得近似 center SDF 或改变结果。
- RenderingServer / RendererRD 提供局部静态数据更新入口，只上传受影响的 Probe row；仅清零受影响 Radiance row，并在必要时重新传播 Global Visibility，不重新创建整套静态 GPU buffer。
- 暴露最近一次实际 Geometry 更新的 Dirty Probe 数、CPU 时间和累计 SDF build count，供 Editor / Runtime benchmark 量化复用与局部更新。
- 后续 Height Field 与 Precomputed Local Transfer Matrix 必须实现同一 `Local Geometry Source` 查询契约，不建立独立传播路径。

自动验证：

- SDF 物体平移 / 旋转后的 Local Geometry 与 full rebuild reference 一致。
- 多个动态 Geometry Source 重叠、离开与删除后，旧位置数据被完全清除。
- 红 / 绿材质和 emission 的颜色通道正确；切换 Geometry Source 不改变空空间、能量稳定性和 Radiance recurrence。
- 纯 transform 更新前后 SDF build count 不变，Dirty Probe 数大于零且小于完整 Volume Probe 数；记录 dev build 的更新耗时与 Dirty 比例。

人工视觉验证：

- 动态曲面与斜面附近的间接光平滑跟随物体。
- 不引入新的条纹、漏光、能量漂移或旧位置残留。

---

# 7. v2 — Global GI 注入

目标：实现 World / Global Lighting → Local LRT 的单向输入。

输入：

- Sun。
- Sky。
- Global diffuse GI。

实现：

- RendererRD 从当前 `Environment` 的 ambient source、ambient color / energy、Sky irradiance octmap、Sky contribution、background energy 与 Sky orientation 构建低频 RGB SH2；World SH 必须先按 Sky orientation 采样，再旋转到 Volume Local SH。
- Sky / Global diffuse 只作为间接 `InComingLight` 进入 Local Transfer Matrix，不绕过 LTM 直接写出 Radiance；Godot Base Pass 继续负责已有 World ambient / background。
- 传播后的 Global Visibility 只对 Sky / Global diffuse 应用一次，用于 sky occlusion；不得再乘当前 Probe Local Visibility，也不得用于 Sun shadow。
- Sun 保持原文独立的 `probe not in Shadow → DirectionalLightSH` 解析灯路径，再经过当前 Probe Local Visibility / Local Transfer；不得把 Sun 合并到长期 SH sky transport 中。
- Volume 边缘沿用现有 Forward `edge_blend_distance` 与 Godot World ambient 连续混合，Volume 外不采样 Local LRT。
- Forward 合成必须在 Volume 内用 Global Visibility 遮蔽 World ambient，再叠加 Local LRT bounce，并按 `edge_blend_distance` 与 Volume 外原始 World ambient 混合；不得把未遮蔽 Base Pass ambient 与 Local LRT 环境项直接相加。
- `Environment.dynamic_gi_enabled` 与 Local LRT 共存时，DynamicGI 必须继续完整更新和接收全场 Geometry，使 Volume 内的 Geometry 仍能向 Volume 外贡献 Global diffuse / specular GI。Base Pass 先得到 DynamicGI diffuse，再以 Local LRT `edge_weight` 在 Volume 内替换为 `World ambient × Global Visibility + Local LRT bounce`；Volume 外保留 DynamicGI diffuse，边界只在两套最终 diffuse 结果之间连续混合，不得相加或让 DynamicGI 在 LRT 之后再次覆盖。
- Local LRT specular 不属于 v2 正确性验收条件；在后续实现前，Volume 内外均继续使用现有 DynamicGI / Reflection Probe / SSR specular 路径。若未来接入 Local LRT specular，必须按同一 Volume `edge_weight` 替换对应的 DynamicGI specular，不得与其相加。
- `LocalLRTVolume3D` 的后端 Volume 只能在节点位于活动 SceneTree 且 `enabled=true` 时启用；离开场景树必须立即停用，避免编辑器场景切换后残留 Volume 污染当前场景。
- 原文只规定方向性 Global Visibility 单独传播，并由 Screen Space Gather 的 A 保存天光遮蔽，未指定 L1 到标量 A 的闭合公式。当前实现将一阶方向矩限制在非负线性 L1 域 `moment ≤ 1/3`，再用正值 maximum-entropy closure 求值，避免线性 SH 负瓣在 Probe cell 边界形成周期黑斑；不得退化为只取 SH0 球面平均。v2 可在 Forward 直接计算该 A，低分辨率 Screen Space Gather 缓存仍留到 v4。
- `visibility_iterations` 表示每个渲染帧的 Global Visibility Probe-hop 预算；静态数据更新只把 A/B 重置为 Local Visibility，随后逐帧继续传播，达到 `min((resolution - 1) / 2)` 的最近 Volume 边界半径后停止。该调度不改变原文 A/B recurrence，也不得因 uniform spacing 缩放改变完成步数。
- Godot / Cycles 首个能量对齐 benchmark 必须使用同一纯色 World lighting radiance；方向性 Sky rotation 由 SH 数值测试验证，不得用两种不同程序天空做视觉对照。

自动验证：

- World → Local SH rotation。
- Constant external lighting injection。
- Volume 旋转后世界光方向保持正确。
- Ambient energy / Sky contribution 线性缩放。
- Global Visibility 对开放 / 遮挡 Probe 的 Sky occlusion，且不重复 Local Visibility。
- Blend weight 与 Volume 外 World ambient 连续。
- DynamicGI 开启时，Volume 外 diffuse 与无 Local LRT 时一致；Volume 内 diffuse 由 Local LRT 替换且不被 DynamicGI 覆盖；边界按 `edge_weight` 连续过渡；DynamicGI specular 不受 Local LRT diffuse 替换影响。
- 同一进程执行 `Cornell → Sky → Cornell` 后，Cornell 输出必须与首次进入逐像素一致。

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
- 同一摄像机视锥内最多采样 N 个启用 Volume。N 由项目设置 `rendering/global_illumination/local_lrt/max_volumes_per_camera` 配置，范围 `1–8`，默认 `2`。Shader / 绑定上限固定为 8。
- 选择：与当前摄像机视锥相交的启用 Volume，按 priority 降序、RID 升序，取前 N。视锥外 Volume 不占用采样槽，仍每帧独立更新。
- 重叠区域 cascade Blend：`w_i = edge_i * remaining`，`remaining *= (1 - edge_i)`，再与 World ambient 混合。
- 各自 Local Transform 独立采样。

自动验证：

- Volume 选择规则确定且稳定：视锥过滤 + priority 排序 + N 截断。
- Priority 排序稳定。
- Blend 权重对任意 N 正确。
- 删除一个 Volume 不影响其他 Volume。
- 项目设置 N 被夹到 `[1, 8]`。

人工视觉验证：

- 两个独立 Volume 正常工作。
- 重叠区域无硬切。
- Priority 行为符合预期。
- 不同旋转 Volume 可同时工作。
- 同一摄像机内多于 2 个互不重叠 Volume 在 `N >= 数量` 时均可被采样。

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
- 按原文把完整 26 Neighbor reference 优化为 4 Neighbor / 3-frame propagation pattern；必须逐场景与 deterministic 26-neighbor 收敛结果对照。
- Temporal / spatial dither 只用于打散 4-neighbor 分帧 pattern 的方向采样误差，不得进入静态 Local Geometry / LTM 构建，也不得用于掩盖 first-bounce 条纹。
- 若 Local Geometry 数值积分需要抗锯齿，只允许固定 seed、可重复的 stratified / blue-noise subcell samples；改变采样模式不得引起 Editor 闪烁或持续重置 / 扰动 Radiance history。
- FP32 → FP16。
- Local Transfer Matrix 压缩。
- Luminance Matrix + RGB Tint。
- Trunk Scene Management。
- 在 V1.2 局部更新基线上进一步合并多 Source Dirty Region、压缩上传区间并批处理 GPU copy。
- Dynamic update budget。
- 不可见 Volume 暂停更新：先于 GPU 分帧预算实现；以活动 viewport / camera 的实际 Volume 选择结果标记本帧使用状态。未被任何活动视图选择的 Volume 暂停 Visibility / Radiance propagation，但保留 A/B Buffer、传播深度与 Radiance history；重新被选择时从原状态无损继续。CPU Geometry Dirty 仍按 Dynamic update budget 更新，确保重新可见时静态数据为最新状态。
- Visibility / Radiance update budget：在不可见暂停验证完成后实现。一个 Jacobi hop 可按 Probe 区间跨帧写入目标 Buffer，只有完整 hop 完成后才交换 A/B；不得向采样端暴露半更新结果。
- GPU buffer / texture layout 优化。
- Forward Mobile 适配与移动端验证。

剩余性能阶段按原文的数据流顺序执行，并在 Trunk 完成后停止本轮：

1. [x] Visibility / Radiance 完整-hop Probe 分帧预算：以 Probe 区间限制单帧 dispatch；目标 Buffer 全部写完前保持当前源 Buffer 对 Forward 可见，完整后才交换 A/B。
2. [x] 4 Neighbor / 3-frame Radiance pattern：实现原文 5.7 的 12 个 edge-neighbor、三组 4-sample pattern；deterministic 26-neighbor 保留为 golden reference。每个 Probe 的 pattern phase 使用固定整数 hash 偏移，并以 `1/3` history accumulation 融合三个相位；该累积是为避免无历史 dither 斑驳而加入的确定性工程闭合，不改变 Local Geometry / LTM。
3. [ ] Screen Space Gather：实现原文 5.8 的低分辨率 RGB reflected GI + A sky occlusion 缓存；Base Pass 只采样该缓存，保留直接 Volume sampling 作为 reference / debug 对照。
4. [ ] GPU 数据布局：验证 FP16、Local Transfer Matrix 压缩和 Luminance Matrix + RGB Tint；只保留具有显存或带宽收益且数值 / Cornell 视觉通过的组合。
5. [ ] Trunk Scene Management：按原文 5.9–5.10 建立粗粒度 Grid；每个 Trunk 保存重叠 GI Primitive 列表、26 邻接索引与 Cache dirty/revision，由 Primitive 增删 / transform / material 变化只置脏覆盖 Trunk，并由 Trunk 查询驱动 Probe 构建。

上述每一步独立提交并更新 `LOCAL_LRT_STATE.md`。Forward+ 与 Forward Mobile 自动回归、固定 Cornell 截图和同硬件性能数据由执行 AI 完成；只有无法自动判定的主观画面偏好才标记为待用户复核，不阻塞数值正确性阶段。

每项优化必须：

- 保留可对照的 reference 行为或可重复测试结果。
- 自动数学测试不回归。
- Cornell Box 人工视觉验证不回归。
- 有明确 CPU / GPU 时间或显存收益。

---

# 10. v5 — 可选 Local LRT Specular

Local LRT specular 不作为 v2 / v3 / v4 的完成条件，只有在现有 Dynamic GI、Reflection Probe 或 SSR 无法满足具体场景需求时才启动。当前优先保留已有 specular 路径，避免为低阶 LRT 引入额外的方向性数据、传播状态和 Forward 绑定。

进入条件：

- v0～v4 的 diffuse 正确性、视觉结果、多 Volume 行为和性能基线均已确认。
- 独立 specular 对照场景明确证明现有 specular 路径存在需要由 Local LRT 解决的问题。
- 先冻结 Local LRT specular 的数据表示、BRDF / roughness 采样和能量守恒验收指标，再开始实现。

实现与验证：

- 使用独立 specular 对照场景，比较 Dynamic GI、Reflection Probe / SSR 与 Local LRT specular。
- Local LRT specular 只替换 Volume 内对应的 DynamicGI specular，并沿 `edge_weight` 连续混合，不与 DynamicGI specular 相加。
- 必须补充 roughness、反射方向、旋转、边界、能量和不重复计入的自动回归，以及 Godot / Cycles 视觉对照。

---

# 11. 每阶段固定执行规则

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
