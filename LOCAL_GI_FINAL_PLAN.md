# LOCAL_GI_FINAL_PLAN.md — Local Probe GI 最小正式原型计划

## 0. 目标

在 Godot 中实现一个可长期维护、强解耦、可独立验证的 `LocalGIVolume3D`。

目标不是立即完成最终产品级 GI，而是先验证一套足够简单、明确、可上线扩展的核心架构：

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
Temporal Update
    ↓
Multi-bounce Local GI
    ↓
Final Local Indirect Diffuse
```

Global GI 继续由 HDDAGI 正常计算整个世界。

LocalGI 与 HDDAGI 不共享 transport/update 逻辑，不依赖 HDDAGI traversal、cascade、voxel hierarchy 或内部更新流程。

两者只有两个连接点：

```text
1. GlobalIndirectCache
   HDDAGI → read-only external indirect radiance → LocalGI

2. Final Shading
   inside LocalGIVolume3D → LocalGI overrides Global GI
   outside volume         → HDDAGI
```

---

# 1. 冻结设计原则

以下内容在 Prototype 阶段视为冻结架构。

## 1.1 强解耦

LocalGI 不允许直接依赖：

- HDDAGI voxel layout
- HDDAGI hierarchy traversal
- HDDAGI cascade implementation
- HDDAGI update scheduling
- HDDAGI probe layout
- HDDAGI internal shader include
- HDDAGI internal resource ownership

LocalGI 只允许依赖一个薄接口：

```text
GlobalIndirectCache
```

概念接口：

```cpp
sample_global_indirect_radiance(
    world_position,
    world_direction
)
```

HDDAGI-specific 逻辑必须隔离在 adapter 内。

目标：

```text
LocalGI
   ↓
GlobalIndirectCache API
   ↓
HDDAGI Adapter
   ↓
HDDAGI
```

未来替换 HDDAGI 时，只替换 adapter。

---

## 1.2 Final Shading 规则

Global GI 始终正常运行。

LocalGI 不关闭、不暂停 HDDAGI。

最终 indirect diffuse：

```text
outside LocalGIVolume3D:
    use Global GI

inside LocalGIVolume3D:
    use Local GI
```

第一版允许硬覆盖。

后续如需要边缘平滑：

```text
final_indirect =
    mix(global_indirect, local_indirect, local_volume_weight)
```

禁止：

```text
global_indirect + local_indirect
```

避免重复计能。

---

## 1.3 Local Volume 是 shading 范围，也是 local transport 域

Volume 内的 transport 使用 LocalGI 自己的：

```text
Static BVH
Dynamic BVH
Probe Field
```

Local ray：

```text
inside volume
    ↓
trace Local Static BVH
trace Local Dynamic BVH
    ↓
nearest local hit
```

如果 ray 没有命中 Local Volume 内几何并离开 Volume：

```text
ray exits volume
    ↓
sample GlobalIndirectCache
    ↓
external incoming indirect radiance
```

因此 HDDAGI 被视为：

> LocalGI 的外部间接光边界条件。

不是 LocalGI transport 的一部分。

---

# 2. Godot 节点形式

新增：

```text
LocalGIVolume3D
```

心智模型尽量接近 `VoxelGI`。

建议最小属性：

```text
Transform

Volume:
    Size / Bounds

Bake:
    Bake Static Geometry

Probe:
    Probe Spacing
    Rays Per Probe

Runtime:
    Update Fraction
    Temporal Hysteresis
    Multi Bounce Enabled

Debug:
    Debug Mode
```

Prototype 阶段不追求最终 Inspector UI。

---

## 2.1 Bake

点击 Bake：

```text
LocalGIVolume3D Bounds
    ↓
collect eligible static geometry
    ↓
convert triangles to Volume local space
    ↓
CPU build Static BVH
    ↓
store baked LocalGI data
```

列车在世界中：

```text
translate
rotate
高速移动
```

都不需要重新构建 Static BVH。

原因：

```text
Static BVH coordinates = LocalGIVolume local space
```

---

# 3. Geometry 架构

## 3.1 Static Geometry

静态 Contributor：

```text
Bake
 ↓
Triangle Buffer
 ↓
CPU Static BVH
 ↓
GPU upload
```

原则：

- Bake 一次。
- 运行时不 rebuild。
- 不 voxelize。
- 不使用 SDF。
- 不依赖 hardware RT。
- 使用真实 triangle intersection。

---

## 3.2 Dynamic Geometry

第一版必须支持少量动态刚性 Mesh。

例如：

- 门
- 箱子
- 可移动家具
- 可拾取大物件
- 其他少量 rigid object

架构：

```text
Dynamic Contributors
    ↓
transform/mesh changed
    ↓
mark dirty
    ↓
before next LocalGI update
    ↓
CPU rebuild small Dynamic BVH
    ↓
upload GPU
```

Dynamic BVH 与 Static BVH 完全分离。

每条 ray：

```text
trace Static BVH
trace Dynamic BVH
    ↓
choose nearest hit
```

上层 GI 不关心 hit 来自哪种 BVH。

---

## 3.3 Prototype 暂不支持的动态几何

暂不支持：

- SkinnedMesh deformation contribution
- cloth deformation
- particle geometry GI contribution
- per-frame full-scene BVH rebuild
- GPU BVH build
- GPU BVH refit

这些只有 profiler 或正式需求证明必要后再加入。

---

# 4. GPU Ray Tracing

CPU 只负责：

```text
Static BVH build
Dynamic BVH rebuild
```

以下全部 GPU：

```text
Probe ray generation
BVH traversal
AABB intersection
Triangle intersection
Nearest-hit selection
Lighting evaluation
Probe integration
Visibility accumulation
Temporal update
Final probe sampling
```

目标：

> 运行时核心 transport 全部 GPU 化，同时保持 CPU BVH builder 简单直观。

---

# 5. Probe GI 核心算法

## 5.1 Probe Grid

规则 3D Probe Grid。

```text
○ ○ ○ ○
○ ○ ○ ○
○ ○ ○ ○
```

Probe 坐标永久位于：

```text
LocalGIVolume local space
```

第一版不做：

- adaptive placement
- cascades
- scrolling
- probe relocation
- probe classification
- sleeping

---

## 5.2 Probe Rays

每 Probe 使用固定数量 ray：

```text
32
64
128
256
```

第一阶段使用 deterministic spherical distribution。

之后才允许做 frame rotation / stochastic sampling。

---

# 6. Radiometric Contract

这是核心正确性约束。

Shader / C++ 中必须明确区分：

```text
radiance
irradiance
albedo
BRDF
visibility
distance
```

不得使用含义模糊的：

```text
energy
gi_value
light_value
```

如果变量实际上有明确物理含义，应直接表达。

---

## 6.1 Local Surface Hit

Local ray 命中内部 surface：

```text
incoming direct lighting
+
previous Local probe indirect
    ↓
Lambertian BRDF
    ↓
outgoing radiance
    ↓
return to Probe
```

Prototype 只做 diffuse transport。

---

## 6.2 Direct Lighting

LocalGI 直接读取 Godot 通用 renderer light 信息：

```text
Directional Light
Omni Light
Spot Light
Shadow information
Environment / sky when applicable
```

不得通过 HDDAGI 获取 direct light。

---

# 7. 外部间接光输入

## 7.1 Boundary Condition

如果 Local ray：

```text
does not hit local geometry
AND
exits LocalGIVolume
```

则：

```text
world_exit_position
world_ray_direction
        ↓
GlobalIndirectCache
        ↓
incoming external indirect radiance
```

这使外部：

- HDDAGI bounce
- 地面反射
- 建筑反射
- 环境间接光
- 世界整体 GI 变化

能够进入 LocalGI。

---

## 7.2 GlobalIndirectCache

LocalGI 只能调用通用接口。

不得：

```text
#include hddagi_internal_shader
read HDDAGI cascade directly
understand HDDAGI texture layout
trace HDDAGI hierarchy
```

Adapter 负责：

```text
GlobalIndirectCache API
    ↓
HDDAGI resource/sample implementation
```

Prototype Phase 0 必须确认：

1. HDDAGI 是否已有可复用的 directional radiance cache。
2. 是否可以在任意 world position/direction 做只读采样。
3. 最小需要绑定哪些 GPU resources。
4. 能否把 HDDAGI-specific 代码完全限制在 adapter。
5. 如果 cache 只能返回 irradiance 而不是 directional radiance，要记录实际语义，不得伪装成 radiance。

如果无法干净实现：

> 停止该部分，不允许深度耦合 HDDAGI。

LocalGI Core 仍继续完成，并将 GlobalIndirectCache 暂时返回 0。

---

# 8. Multi-bounce

第一阶段先实现 one-bounce。

Multi-bounce 只能在 one-bounce 正确后加入。

正确结构：

```text
ray hit local surface
    ↓
direct outgoing radiance
+
sample previous completed Local probe field
    ↓
current probe estimate
```

必须使用上一轮完成的数据。

禁止：

```text
same pass read/write feedback
```

建议 ping-pong：

```text
Probe Field A
Probe Field B
```

---

# 9. Temporal

Temporal 只允许作为估计值收敛：

```text
new_estimate =
    lerp(previous_estimate, current_sample, alpha)
```

禁止：

```text
history += current_sample
```

不得使用 arbitrary clamp 掩盖能量错误。

---

# 10. Visibility

Probe 至少保存：

```text
Directional Irradiance
Distance Mean
Distance Second Moment
```

最终 shading：

```text
nearest 8 probes
    ↓
trilinear weight
× normal weight
× visibility weight
    ↓
interpolated indirect irradiance
```

第一版优先标准、直观的 distance-moment visibility。

不得预先加入大量 leak-fix heuristic。

---

# 11. 最小测试项目

必须创建独立 Godot 测试项目：

```text
LocalGIPrototype
```

不得使用正式游戏项目验证核心算法。

所有核心视觉测试尽量基于 Cornell Box 及其变体。

目标：

- 场景小
- 几何语义明确
- 光照行为直观
- 易比较
- 易自动生成
- 易复现

---

# 12. 测试场景

## Scene A — Cornell Baseline

经典 Cornell Box 类型场景：

```text
Closed Box
White floor / ceiling / back wall
Red left wall
Green right wall
One bright area/point light
Simple white boxes
```

用途：

- basic ray correctness
- color bleeding
- direct/indirect separation
- one-bounce
- multi-bounce
- energy stability

---

## Scene B — White Cornell Energy Box

全部 diffuse white / neutral gray。

材质参数化：

```text
Albedo = 0.2
Albedo = 0.5
Albedo = 0.8
```

用途：

- radiometric linearity
- temporal convergence
- multi-bounce stability
- energy conservation

这是能量验证的主要场景。

---

## Scene C — Cornell Thin Wall

Cornell Box 中间加入一面薄隔墙：

```text
5 cm
10 cm
15 cm
20 cm
```

一侧有强光，一侧保持暗。

用途：

- triangle BVH thin geometry
- visibility leak
- interpolation leak

要求区分：

```text
BVH ray leak
vs
Probe interpolation leak
```

---

## Scene D — Two-Chamber Cornell

一个大 Cornell Box 被隔成两个房间：

```text
┌──────────┬──────────┐
│  LIGHT   │   DARK   │
│          │          │
│          │          │
└──────────┴──────────┘
```

可以有：

```text
closed wall
small doorway
window opening
```

用途：

- visibility
- room-to-room bleeding
- opening transport
- probe contamination

---

## Scene E — Open Cornell / External GI

Cornell Box 一侧存在窗户或大开口。

外部放置：

```text
colored wall
ground plane
simple exterior geometry
```

Global HDDAGI 在外部正常运行。

用途：

```text
Local ray exits volume
    ↓
GlobalIndirectCache
    ↓
external indirect enters Cornell interior
```

这是 HDDAGI Cache adapter 的主要视觉验证场景。

---

## Scene F — Dynamic Object Cornell

Cornell Box 内加入：

```text
movable white box
rotating door
movable colored panel
```

全部是刚性 Mesh。

用途：

- Dynamic BVH dirty detection
- Dynamic BVH rebuild
- static/dynamic nearest-hit
- dynamic occlusion
- dynamic indirect contribution

---

## Scene G — Moving Local Volume

整个 Cornell Box：

```text
LocalGIVolume3D
+
all local geometry
```

作为整体：

```text
translate
rotate
high-speed translation
translation + rotation
```

外部 HDDAGI 世界保持 world-space。

用途：

- Local-space stability
- Static BVH no rebuild
- Probe history stability
- GlobalIndirectCache world-space sampling correctness

---

## Scene H — Performance Cornell

参数化复杂度：

```text
Triangle Count:
    low
    medium
    high

Probe Count:
    100
    300
    500
    800

Rays Per Probe:
    32
    64
    128
    256

Update Fraction:
    100%
    50%
    25%
```

用于最终 profiler。

---

# 13. Debug Views

必须尽早提供 Debug View。

至少支持：

```text
Local Geometry
Static BVH hit
Dynamic BVH hit
Ray hit/miss
Hit normal
Hit distance
Probe positions
Selected probe rays
Raw probe radiance
Probe irradiance
Visibility
Probe weights
GlobalIndirectCache sample
Final LocalGI
GlobalGI
Final selected GI
```

Debug View 属于 correctness 工具，不是 editor polish。

---

# 14. Phase 执行规则

每个 Phase = 一个独立 AI context 周期。

固定流程：

```text
Read PLAN.md
Read STATE.md
    ↓
verify repository facts
    ↓
execute ONE Current Phase
    ↓
static verification
unit / automated verification
    ↓
if visual judgment required:
    request Human Visual Verification
    ↓
update STATE.md
    ↓
STOP
    ↓
compact
```

AI 不得自动进入下一 Phase。

---

# 15. Phase 0 — Repository Reconnaissance + Test Project

## 目标

只调查代码、确认 integration points、创建最小测试项目。

不实现 GI transport。

## 工作

确认：

- current Godot base revision
- HDDAGI branch/revision
- renderer architecture
- Forward+ GI integration
- VoxelGI node/bake integration reference
- RenderingDevice patterns
- mesh extraction
- shader include structure
- light/shadow GPU resources
- existing test infrastructure
- HDDAGI cache layout
- HDDAGI cache sampling path
- minimal GlobalIndirectCache adapter feasibility

创建：

```text
LocalGIPrototype
```

以及 Scene A–H 的最小场景骨架。

## Verification

Static:

- Godot build succeeds
- test project launches
- LocalGIVolume3D skeleton can exist in scene

Automated:

- smoke-test project/resources load

Human Visual:

- only confirm test project and Cornell scenes open correctly

## Gate

必须在 STATE 中记录：

```text
LocalGI-owned files
Godot integration files
HDDAGI adapter files
existing files expected to change
files explicitly not modified
```

---

# 16. Phase 1 — Static Bake + CPU BVH

## 目标

完成 `Bake Static Geometry`。

## Pipeline

```text
Bounds
→ collect static meshes
→ local-space triangle list
→ CPU BVH
→ baked data
```

## Unit Tests

必须测试：

- triangle extraction
- transform to volume local space
- direct ray hit
- miss
- nearest hit
- parallel ray
- edge hit
- thin wall 5/10/15/20 cm
- deterministic BVH build

## Human Visual

不需要。

## Gate

CPU geometry query 全部通过才进入 Phase 2。

---

# 17. Phase 2 — Dynamic BVH

## 目标

支持少量动态刚性 Contributor。

## Pipeline

```text
dynamic transform/mesh changed
→ dirty
→ rebuild small CPU Dynamic BVH
→ update baked/runtime GPU data
```

## Unit Tests

必须测试：

- dirty only on actual relevant change
- static BVH remains unchanged
- dynamic BVH rebuild changes hit result
- stationary dynamic object does not rebuild continuously
- removing dynamic object updates BVH
- nearest hit across static + dynamic CPU queries

## Human Visual

不需要。

---

# 18. Phase 3 — GPU BVH Traversal

## 目标

实现 GPU software ray tracer。

## GPU Buffers

至少：

```text
Static BVH Nodes
Static Triangles
Dynamic BVH Nodes
Dynamic Triangles
Ray Buffer
Hit Buffer
```

## GPU

```text
trace static
trace dynamic
choose nearest
```

## Automated Verification

固定 ray set：

```text
CPU result
vs
GPU result
```

比较：

- hit/miss
- nearest hit
- distance
- normal
- triangle identity where available

## Human Visual

需要。

Cornell Thin Wall 中显示 ray/hit debug。

人类只判断：

- 是否出现明显错误命中
- 薄墙是否被正确挡住
- normal/distance visualization 是否合理

AI 不得代替人类判 PASS。

---

# 19. Phase 4 — Probe Grid + Probe Rays

## 目标

生成规则 Probe Grid 并用 GPU tracing。

暂不计算 GI。

## Automated Verification

- probe count exact
- local positions exact
- directions normalized
- directions deterministic
- ray budget exact
- Volume transform 不改变 Probe local relationship

## Human Visual

需要。

查看：

```text
Probe grid
selected probe rays
moving/rotating Cornell box
```

---

# 20. Phase 5 — One-Bounce Local GI

## 目标

实现第一版物理意义明确的 indirect diffuse。

仅：

```text
direct light
→ ray hit surface
→ Lambertian reflected radiance
→ Probe irradiance
```

不做：

- temporal
- Local multi-bounce

## Automated Tests

White Cornell：

- zero light → zero GI
- zero albedo → zero reflected contribution
- light ×2 → GI approximately ×2
- albedo ×0.5 → reflected contribution approximately ×0.5

## Human Visual

需要。

Cornell Baseline：

- light direction
- color bleeding direction
- no obvious inversion
- no arbitrary overbright result

---

# 21. Phase 6 — Visibility + Final Local Shading

## 目标

完成：

```text
Probe distance moments
Visibility
8-probe interpolation
Final indirect diffuse
```

## Automated Verification

- weight sum validity
- zero visibility suppresses contribution
- deterministic interpolation
- no NaN/Inf
- boundary handling

## Human Visual

必须。

重点：

```text
Cornell Thin Wall
Two-Chamber Cornell
```

记录：

- 5 cm
- 10 cm
- 15 cm
- 20 cm
- closed divider
- doorway
- window

如果 BVH 正确但 final shading 漏：

> 问题归类为 visibility/interpolation。

禁止修改 BVH 来掩盖。

---

# 22. Phase 7 — GlobalIndirectCache Adapter

## 目标

让 LocalGI 在 ray 离开 Volume 后读取 HDDAGI 外部间接光。

## 工作

建立薄接口：

```text
GlobalIndirectCache
```

HDDAGI-specific implementation 仅存在于 adapter。

LocalGI Core 不得 include HDDAGI-specific implementation。

## Automated Verification

必须至少可以构造：

```text
cache disabled → returns 0
known debug cache value → LocalGI receives expected value
```

如果 HDDAGI cache 很难做 deterministic unit test，可以增加 test-only mock provider。

## Human Visual

必须。

Scene E：

```text
Open Cornell
+
external colored bounce surface
+
HDDAGI
```

验证：

- cache disabled 时外部 indirect 不进入
- cache enabled 时可以观察到合理外部间接输入
- LocalGI 内部不直接显示 Global GI，而是通过 Local probe transport 响应

## Gate

如果必须深改 HDDAGI 核心才能实现：

> 本 Phase 停止。
> 记录原因。
> 不允许破坏强解耦原则。

---

# 23. Phase 8 — Temporal

## 目标

加入 Probe temporal convergence。

## Automated Verification

记录 probe 数值随 frame：

- constant input converges
- does not grow indefinitely
- light on/off responds correctly
- hysteresis parameter behavior clear

## Human Visual

需要。

观察：

- flicker
- convergence
- ghosting
- sudden brightness instability

---

# 24. Phase 9 — Multi-Bounce

## 目标

在 LocalGI 内加入受控 multi-bounce。

## 规则

只能读取：

```text
previous completed Local probe field
```

禁止同 pass feedback。

## Automated Energy Test

White Cornell：

```text
Albedo 0.2
Albedo 0.5
Albedo 0.8
```

记录：

```text
iteration
average probe irradiance
peak probe irradiance
representative pixel luminance
```

必须满足：

- constant lighting 下最终稳定
- albedo 越高 persistence 越高
- albedo < 1 不得无限增长

## Human Visual

需要。

确认：

- bounce 合理
- 无持续增亮
- 无异常颜色漂移

---

# 25. Phase 10 — Dynamic Object Visual Validation

## 目标

验证动态物体：

1. 能受到 LocalGI。
2. 能遮挡 LocalGI rays。
3. 能贡献 diffuse indirect。
4. 仅 dirty 时 rebuild Dynamic BVH。

## Scene

Dynamic Object Cornell。

## Automated Verification

- moving object triggers rebuild
- stationary object does not
- static BVH unchanged
- hit result follows object transform

## Human Visual

必须。

测试：

- moving white box
- rotating door
- moving colored panel

观察：

- occlusion changes
- bounced color changes
- no stale BVH artifacts

---

# 26. Phase 11 — Moving Volume Validation

## 目标

验证列车式运动。

Cornell Box + LocalGIVolume 整体：

```text
translation
rotation
high-speed motion
combined motion
```

## Automated Verification

必须确认：

```text
Static BVH rebuild count = 0
Probe local positions unchanged
Probe history not reset solely because world transform changed
GlobalIndirectCache receives correct world-space query coordinates
```

## Human Visual

必须。

观察：

- flicker
- swimming
- GI detachment
- brightness popping
- external GI direction incorrect
- rotation-related history errors

---

# 27. Phase 12 — Performance Baseline

在此之前不得进行复杂性能优化。

## Measurement

分别记录：

```text
Static BVH build CPU
Dynamic BVH rebuild CPU
BVH upload
GPU ray tracing
Probe integration
Visibility
Temporal
GlobalIndirectCache sampling
Final shading
Total LocalGI GPU time
```

## Matrix

Probe Count:

```text
100
300
500
800
```

Rays:

```text
32
64
128
256
```

Update:

```text
100%
50%
25%
```

Geometry:

```text
low
medium
high
```

必须记录真实 triangle 数。

## Human Visual

不需要。

---

# 28. Phase 13 — Architecture Decision

只根据真实瓶颈决定优化方向。

## 如果 ray traversal 贵

考虑：

- BVH node layout
- ray coherence
- better CPU BVH
- update scheduling
- probe classification/sleeping

## 如果 Probe 数贵

考虑：

- adaptive density
- bricks
- placement optimization

## 如果 leak 仍严重

考虑：

- probe relocation
- stronger visibility
- geometry-aware weighting

## 如果 Dynamic BVH 贵

考虑：

- refit
- static/dynamic hierarchy split optimization
- GPU build only if profiling justifies

## 如果 HDDAGI cache adapter 贵

考虑：

- lower-frequency boundary sampling
- cache reuse
- coarser directional representation

但不得让 LocalGI 直接耦合 HDDAGI internals。

---

# 29. Prototype Success Criteria

## Correctness

- CPU/GPU triangle hit consistent.
- 5–20 cm thin walls are geometrically occlusive.
- Dynamic BVH produces correct nearest hits.
- one-bounce response approximately linear.
- temporal accumulation converges.
- multi-bounce does not produce unbounded energy.
- GlobalIndirectCache works as external boundary input.

## Quality

- Cornell Box produces plausible diffuse bounce.
- Two-Chamber Cornell has acceptable cross-wall leakage.
- openings transport light correctly.
- dynamic objects affect GI correctly.
- moving Local Volume remains visually stable.

## Architecture

- LocalGI Core has no direct HDDAGI dependency.
- HDDAGI-specific code limited to adapter.
- Global GI always runs.
- inside Volume LocalGI overrides Global GI.
- no Global + Local additive double-counting.

## Performance

No arbitrary performance target is assumed before Phase 12.

Decision must be based on measured data.

---

# 30. AI 工作约束

每个 Phase：

1. 完整读取 `PLAN.md`。
2. 完整读取 `STATE.md`。
3. 代码仓库是事实来源。
4. 只执行 `Current Phase`。
5. 不提前实现后续 Phase。
6. 不进行无关重构。
7. 不增加 fallback / compatibility layer。
8. 不通过 heuristic、clamp、magic multiplier 掩盖算法错误。
9. 不为了“画面看起来对”破坏 radiometric contract。
10. 所有视觉判断交给人类。
11. AI 不得声明 Human Visual PASS。
12. 阶段完成后更新 STATE。
13. 更新 STATE 后停止。
14. 等待 compaction 后再继续。
15. 如果 PLAN 与代码事实冲突，记录证据，不自行扩大设计范围。
16. 不创建额外长期设计文档；长期状态只进入 PLAN / STATE。

---

# 31. License / Reference Rule

目标是最小版权风险。

允许：

- 阅读公开论文和技术文档理解算法。
- 参考 MIT / BSD / Apache 等许可清晰的开源实现。
- 参考 Godot 自身 MIT 代码。
- 参考 Wicked Engine MIT 代码，并按许可证要求保留必要 notice。

禁止：

- 直接复制许可不明确代码。
- 直接复制 proprietary SDK shader/code。
- 把 RTXGI proprietary implementation 当代码来源。
- 在未确认具体文件许可前复制混合许可仓库中的代码。

STATE 必须维护：

```text
Reference / License Ledger
```

记录所有实际复制或改写的重要来源。

---

# 32. Compaction 规则

每个 Phase 结束前 STATE 必须包含：

```text
Current Phase
Last Completed Phase
Current commit/revision
Actual changed files
Implemented features
Automated test results
Human verification result when applicable
Known issues
Architecture decisions
Reference/license changes
Exact next-phase entry conditions
```

Compact 后：

```text
Read PLAN
Read STATE
Verify minimum repository facts
Continue Current Phase only
```

任何关键事实不得只存在于聊天上下文。
