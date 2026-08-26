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

两者最终只有两个连接点：

```text
1. GlobalIndirectCache
   HDDAGI → read-only external indirect input → LocalGI

2. Final Shading
   inside LocalGIVolume3D → LocalGI overrides Global GI
   outside volume         → HDDAGI
```

开发顺序冻结为：

```text
LocalGI correctness first
    ↓
LocalGI runtime transport on renderer RD
    ↓
Forward+ final Local/Global selection
    ↓
live HDDAGI GlobalIndirectCache last
```

在 live HDDAGI 接入之前：

```text
GlobalIndirectCache = Null / Debug Mock only
```

不得为了提前看到 GlobalGI 而引入 CPU readback fallback、跨 RenderingDevice 临时桥接或其他最终会删除的过渡路径。

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

Volume 内部由 LocalGI 完全覆盖，边缘使用平滑权重过渡：

```text
final_indirect =
    mix(global_indirect, local_indirect, local_volume_weight)
```

`local_volume_weight` 在 Volume 边界为 0，进入内部后平滑达到 1。禁止：

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
- sleeping

第一版必须做最小 Probe Classification：

```text
Static geometry:
    probe inside geometry → inactive

Dynamic geometry:
    dynamic object covers probe → temporarily inactive
```

暂不做 relocation。只有最终测试证明大量 inactive Probe 导致质量不足时，才在优化阶段评估 relocation。

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

外部间接光不是 LocalGI Core correctness 的前置条件。

LocalGI 必须先在完全没有 GlobalGI 输入的情况下独立正确工作。

在 LocalGI Core、renderer RD runtime transport、Forward+ integration 和完整 GPU Cornell 验证全部通过之前：

```text
Global indirect input = disabled
```

不得提前实现：

- GlobalIndirectCache API
- Debug / Mock GlobalIndirectCache
- live HDDAGI provider
- CPU readback bridge
- cross-device fallback
- LocalGI Core 对 HDDAGI internals 的依赖

最终阶段才实现：

```text
Local ray exits volume
    ↓
GlobalIndirectCache
    ↓
HDDAGI read-only provider
    ↓
external indirect input
```

GlobalIndirectCache 与 live HDDAGI provider 应在同一个后期阶段设计和实现，避免为了原型接口产生临时代码。

强解耦原则保持不变：

```text
LocalGI Core
    ↓
GlobalIndirectCache API
    ↓
HDDAGI Adapter
```

LocalGI Core 永远不得：

```text
#include hddagi_internal_shader
read HDDAGI cascade directly
understand HDDAGI texture layout
trace HDDAGI hierarchy
```

若最终无法在强解耦、零 CPU readback的前提下接入 HDDAGI：

> 不允许深改 HDDAGI Core；记录证据，LocalGI 仍应可以独立工作。

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

## 已完成 Phase 保留规则

本次 Plan 重排只保留 `STATE.md` 中已经确定要保留的 Phase 0–6。

```text
Phase 0–6:
    preserve completed results

Old Phase 7 GlobalIndirectCache API / Mock:
    DISCARD
    do not preserve code or completion state

Live HDDAGI / GlobalIndirectCache:
    deferred until LocalGI runtime path is complete
```

若工作树中仍存在旧 Phase 7 的未提交改动，应在继续开发前丢弃，并在 `STATE.md` 记录回退结果。

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

# 22. Phase 7 — Probe Classification

## 目标

处理 Probe 位于静态或动态几何内部的情况。

第一版只做 classification，不做 relocation。

## Static Classification

```text
Probe
    ↓
inside / embedded in static geometry?
    ↓
YES → inactive
NO  → active
```

优先复用现有 Triangle BVH / Probe ray 数据，采用最简单、确定、可测试的方法。

## Dynamic Classification

```text
active probe
    ↓
dynamic geometry covers it
    ↓
temporarily inactive
```

动态物体离开后恢复。

## Automated Verification

```text
[ ] free-space probe remains active
[ ] static-wall embedded probe becomes inactive
[ ] inactive probe is excluded from shading weights
[ ] remaining probe weights renormalize correctly
[ ] dynamic object can temporarily deactivate probe
[ ] probe reactivates after dynamic object leaves
[ ] no NaN/Inf when neighboring probes are inactive
```

## Human Visual

需要。

Cornell Thin Wall / Two-Chamber Cornell：

- 显示 active / inactive Probe。
- 确认墙内 Probe 被正确识别。
- 确认没有 invalid Probe 导致的明显异常亮点。

## Gate

若大量 Probe 被禁用导致质量明显不足：

> 记录为后续 relocation 候选，不在本 Phase 扩展算法。

---

# 23. Phase 8 — Temporal

## 目标

加入 Probe temporal convergence。

本阶段没有任何 GlobalGI 输入。

## Automated Verification

记录 probe 数值随 frame：

- constant input converges
- does not grow indefinitely
- light on/off responds correctly
- hysteresis parameter behavior clear
- inactive Probe history behavior deterministic

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

能量测试不得包含任何 GlobalGI 输入。

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
- inactive Probe 不得引入异常 feedback

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
5. 能正确影响 Probe Classification。

## Scene

Dynamic Object Cornell。

## Automated Verification

- moving object triggers rebuild
- stationary object does not
- static BVH unchanged
- hit result follows object transform
- dynamic coverage deactivates affected Probe
- Probe reactivates when coverage disappears

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
- no stale inactive-Probe artifacts

---

# 26. Phase 11 — Moving Volume Validation

## 目标

验证列车式运动下 LocalGI Core 自身稳定。

Cornell Box + LocalGIVolume 整体：

```text
translation
rotation
high-speed motion
combined motion
```

本阶段不包含任何 GlobalGI 输入。

## Automated Verification

```text
Static BVH rebuild count = 0
Probe local positions unchanged
Probe active/inactive state remains local-space correct
Probe history not reset solely because world transform changed
```

## Human Visual

必须。

观察：

- flicker
- swimming
- GI detachment
- brightness popping
- rotation/history errors

---

# 27. Phase 12 — Renderer RD Runtime Transport Migration

## 目标

把已经验证正确的 LocalGI transport 从 CPU/reference 路径迁移到正式 renderer RD runtime 路径。

CPU 实现必须保留为 correctness oracle，不得删除。

## CPU 保留职责

```text
Static BVH build
Dynamic BVH dirty rebuild
Reference transport / deterministic tests
configuration/resource management
```

## Renderer RD Runtime

迁移：

```text
Probe ray generation
Static/Dynamic BVH traversal
Nearest-hit selection
Direct lighting evaluation
Lambertian transport
Probe integration
Distance moments
Visibility
Temporal
Multi-bounce
Probe classification runtime state
```

## 关键约束

- 不接 live HDDAGI。
- 不实现 GlobalIndirectCache。
- 不改变 radiometric contract。
- 不删除 Phase 5–9 的 CPU reference implementation。

## Automated Verification

尽可能逐项做：

```text
CPU reference
vs
renderer-RD GPU runtime
```

至少覆盖：

- hit/miss
- one-bounce linearity
- probe irradiance
- visibility
- temporal convergence
- multi-bounce stability
- inactive Probe behavior

## Human Visual

需要。

Cornell Baseline / Thin Wall / Two-Chamber Cornell 重新验证。

---

# 28. Phase 13 — Forward+ LocalGI Integration

## 目标

让实际 Forward+ 材质 shading 使用 LocalGI。

Global GI 始终正常运行。

最终规则：

```text
outside LocalGIVolume3D:
    use Global GI

inside LocalGIVolume3D:
    use Local GI
```

Volume 内部 LocalGI 权重为 1；边缘按 Volume weight 与 Global GI 平滑混合。

禁止：

```text
Global GI + Local GI
```

## Automated Verification

- outside volume selects Global path
- inside volume selects Local path
- no additive double-count
- volume transform affects coverage correctly
- disabled LocalGI returns to Global path

## Human Visual

必须。

同屏验证：

- Volume 内 LocalGI
- Volume 外 Global GI
- Volume 边界选择行为

此时仍不把 GlobalGI 作为 LocalGI 的 transport 输入。

---

# 29. Phase 14 — Full GPU LocalGI Validation

## 目标

证明正式 GPU + Forward+ 路径与前面的 LocalGI correctness 结果一致。

重新跑 Local-only Cornell suite：

```text
Scene A — Cornell Baseline
Scene B — White Cornell Energy Box
Scene C — Cornell Thin Wall
Scene D — Two-Chamber Cornell
Scene F — Dynamic Object Cornell
Scene G — Moving Local Volume
```

Scene E 的 GlobalGI boundary test 延后。

## Automated Verification

重新执行：

```text
zero light
zero albedo
light ×2
albedo ×0.5
thin wall hit
visibility blocking
temporal convergence
multi-bounce stability
dynamic BVH
probe classification
moving-volume invariants
```

CPU reference 与 GPU runtime 差异必须记录。

## Human Visual

必须。

只有本 Phase 通过，才认为：

> LocalGI Core + Runtime Integration 完成。

---

# 30. Phase 15 — GlobalIndirectCache + Live HDDAGI Provider

## 目标

只有 LocalGI 已经完整工作后，才设计并实现 GlobalIndirectCache，以及真实 HDDAGI 外部间接光 provider。

不保留前期 Mock/API 实现作为依赖；该接口应按照最终 renderer-RD 使用方式一次性设计。

最终目标：

```text
Local ray exits volume
    ↓
GlobalIndirectCache
    ↓
HDDAGI read-only provider
    ↓
Local probe transport
```

要求：

```text
CPU readback = 0
```

不得加入临时 CPU bridge。

## 必须先记录实际语义

```text
HDDAGI cache quantity:
Sampling coordinates:
Required renderer resources:
View lifetime:
Unavailable behavior:
```

如果返回 irradiance：

> API、变量、测试必须保持 irradiance 语义，不得称为 raw incoming radiance。

如需数学转换：

> 转换公式、近似假设和量纲必须显式记录并自动测试。

## 强解耦 Gate

必须满足：

```text
LocalGI Core contains no HDDAGI-specific include
HDDAGI Core update/hierarchy code is not modified
HDDAGI knowledge exists only in provider/adapter
```

若不能满足：

> 停止本 Phase，不允许破坏架构；LocalGI 仍保持独立可用。

## Automated Verification

至少包括：

```text
[ ] adapter/provider resource validity
[ ] unavailable cache behavior deterministic
[ ] world-space query correctness
[ ] no CPU readback
[ ] LocalGI Core no HDDAGI dependency
[ ] local hit does not query GlobalIndirectCache
[ ] ray exit queries GlobalIndirectCache
```

可在本 Phase 创建最小 deterministic test provider，仅用于该最终接口的自动测试；不作为早期架构依赖。

## Human Visual

必须在 Play/runtime 下验证。

Prototype 不要求无 active render view 的 Inspector 路径支持 live HDDAGI。

---

# 31. Phase 16 — Global / Local Boundary Validation

## 目标

验证真实全局间接光作为 LocalGI 边界条件时的最终行为。

## Scene E — Open Cornell / External GI

外部：

```text
HDDAGI
colored bounce wall
ground
simple exterior geometry
```

内部：

```text
LocalGI
```

比较：

```text
Global input disabled
Deterministic test provider
Live HDDAGI Provider
```

## Human Visual

必须验证：

- Disabled：无外部间接输入
- Test Provider：确定性已知输入行为正确
- Live：外部 HDDAGI 间接光合理进入 LocalGI
- Volume 内最终仍由 LocalGI shading
- Volume 外仍由 Global GI shading
- 无 Global + Local additive double-counting

## Automated Verification

- provider selection deterministic
- ray exit → provider query
- local hit → no provider query
- invalid live cache behavior deterministic
- world/local transform conversion correct

---

# 32. Phase 17 — Performance Baseline

在此之前不得进行复杂性能优化。

测量最终正式路径。

## Measurement

```text
Static BVH build CPU
Dynamic BVH rebuild CPU
BVH upload
GPU ray tracing
Direct lighting
Probe integration
Visibility
Temporal
Multi-bounce
Probe classification
GlobalIndirectCache sampling
Forward+ LocalGI shading
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

Provider Matrix：

```text
Global input disabled
Live HDDAGI Provider
```

用于量化 GlobalIndirectCache 增量成本。

## Human Visual

不需要。

---

# 33. Phase 18 — Architecture / Optimization Decision

只根据真实瓶颈决定优化方向。

## 如果 ray traversal 贵

考虑：

- BVH node layout
- ray coherence
- better CPU BVH
- update scheduling
- probe sleeping

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

## 如果 Probe Classification 导致质量损失

考虑：

- static probe relocation
- smarter placement

## 如果 HDDAGI provider 贵

考虑：

- boundary-query reuse
- directional cache reuse
- lower-frequency external boundary refresh

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
- LocalGI remains fully correct without any GlobalIndirectCache implementation.
- After Phase 15, live HDDAGI GlobalIndirectCache works as an external boundary input.

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

No arbitrary performance target is assumed before Phase 17.

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
