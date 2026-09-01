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
Current Phase: V4 — 性能优化
Current Status: V4_BASELINE_RECORDED — 28,175 Probe 的未优化 CPU / GPU / 显存 / 上传基线已冻结；下一步优化 Dynamic Dirty Region 上传与全量状态重置。
Last Completed Phase: V3 — 多 Volume + Priority / Blend
Human Visual Validation: V2 Cornell 已通过；V3 双 Volume 与 per-camera N 均已通过用户验收。
Directional Benchmark: `benchmarks/directional_cornell_v08/`；详细修复记录：`LOCAL_LRT_V08_DIRECTIONAL_FIX_REPORT.md`
Omni Benchmark: `benchmarks/omni_cornell_v09a/`
Area Benchmark: `benchmarks/area_cornell_v09b/`
Spot Benchmark: `benchmarks/spot_cornell_v09c/`
V0 Acceptance Benchmark: `benchmarks/v0_acceptance_cornell/`
V1.1 Benchmark: `benchmarks/v1_dynamic_cornell/`
V1.2 Benchmark: `benchmarks/v12_dynamic_source_reuse/`
V2 Benchmark: `benchmarks/v2_global_gi/`
V4 Benchmark: `benchmarks/v4_performance/`
```

```text
Repository: https://github.com/MouthsheepZZZ/godot.git
Branch: feature/hddagi-4.7/local-lrt-volume-3d
Base / Upstream: origin
Last Known Commit: Allow configurable per-camera Local LRT volume sampling.
```

---

# 2. Frozen Project Decisions

- 最小侵入 Godot 主线，方便长期 rebase。
- 优先新增独立 Local LRT 子系统；现有 GI 只做薄接入。
- GI 工作在 `LocalLRTVolume3D` Local Space。
- Editor / Runtime 共用实现。
- 后端 Local LRT Volume 仅在对应 `LocalLRTVolume3D` 位于活动 SceneTree 且节点 enabled 时启用；退出场景树必须停用。
- v0 = 静态 Geometry + 动态解析灯光 + Shadow-aware GPU Injection；执行顺序冻结为前半期 Directional-only，后半期 Point / Omni → Area → Spot，禁止并行调试多类灯光。
- v1 = 动态物体。
- v2 = Global GI 注入。
- v3 = 多 Volume + Priority / Blend。
- v5 = 可选 Local LRT specular；不作为 v2 / v3 / v4 的完成条件，进入前继续使用 DynamicGI / Reflection Probe / SSR specular。
- V1.2 只提前实现其动态 Geometry 可用性所必需的 SDF 复用、Dirty Region 与局部 GPU 更新；其余性能优化仍留到 v4。
- v0 使用完整 26 邻居 reference 路径。
- 首版只做 Diffuse GI。
- SH2 = `Vector4`，顺序为 `[Y00, Y1x, Y1y, Y1z]`。
- SH basis 使用正交归一化常数 `Y00 = 0.28209479177387814`、`Y1 = 0.4886025119029199`。
- SH 系数按 `f(direction) = dot(coefficients, basis(direction))` 求值。
- Local Transfer Matrix 为 row-major，`output = B * input`。
- 令 `D` 将 World SH 转到 Local SH，则 `B_world = D^T * B_local * D`。
- Local Visibility 表示可见比例：`1 = fully visible`，`0 = blocked`，不得作为 occlusion 使用。
- 26 邻居按 z-major 的 `[-1, 1]^3` 顺序枚举并跳过中心，使用归一化 inverse-distance 权重；每个邻居经 antipodal Local Visibility 后沿其 offset 方向求非负 Radiance，并用 `4π × weight` 重投影为 SH2，不得直接平均整组 SH coefficient。
- 空空间通过独立 transmission 项继续传递 Radiance；表面通过 transfer matrix 反射；decay `< 1` 保证无注入时衰减。
- 视觉验证必须由人类完成。
- 数学 / 算法机制使用自动单元测试。
- 如无必要勿增实体；先看到效果，再优化。
- Cornell Box 是长期主测试关卡。
- V0.8 / V0.9 遵循原文 CPU / GPU 分工：CPU 保留 Local Visibility / Local Transfer / Emission 构建，正式 Runtime 解析灯 Injection 迁移到 GPU并逐 Probe 采样 Shadow Map；CPU Injection 仅保留为 reference。
- V0.8 只验收 Directional Light，使用不依赖相机 CSM 的 Volume Directional Shadow Map，并完成 Cornell / Cycles 的 direct-only、GI-only、合成结果和能量缩放对照。V0.9 在其通过后依次完成 Omni、Area、Spot；Editor Scene Viewport 与 Runtime 必须共用 `Shadow → Injection → Propagation → Forward` 路径。
- Global Visibility 只保留给后续天空遮蔽 / Global GI，不得充当 Directional / Omni / Spot Shadow Map。
- V0.6 只维护并消费单一 reflected Radiance 场；解析灯 Injection 仅作为当前 Probe 的 Local Transfer 入射光，不参与 Base Pass 直接叠加。
- `visibility_iterations` 与 `propagation_iterations` 独立；后者表示每帧继续执行的 Radiance Probe-hop 数，修改它不得清空 Radiance 或重算解析灯 Injection。
- Radiance decay 按世界米计算，不再按 Probe hop 固定衰减。
- V0.2 Gap Closure 按原文分离 per-object Local Geometry Source 与 Radiance Probe Grid：Geometry Resource 拥有独立 voxel size / resolution，`probe_spacing` 只决定 Radiance Probe 查询位置。
- 26 个 LTM 查询点为 `probe_center + neighbor_offset * actual_probe_spacing`，变换到 object local 后直接采样 Color SDF；`SampleBasis = sh_basis(SampleDir)`，`GetSH2PIDivDFT(d) = (Y00, Y1 * d * 2/3)`。
- 26 邻域 inverse-distance 权重之和为 1，再乘 `4π`；不得使用均匀 `4π / 26`。
- V3 Forward 同一摄像机视锥内最多采样 N 个启用 Volume；N 由项目设置 `rendering/global_illumination/local_lrt/max_volumes_per_camera` 配置，范围 `1–8`，默认 `2`。排序为 priority 降序、相等时 RID 升序。重叠权重为 cascade：`w_i = edge_i * remaining`。
- V0 LTM 为一次局部反弹；Neumann 无限反弹不作为 V0 通过条件。
- fractional coverage 只参与 Local Visibility / LTM 积分；只有 Probe center 合并 SDF `< 0` 时才为 `inside_solid`。不得再由 `coverage > 0` 派生二值 Radiance Probe 失效，也不得跳过表面 Probe 的 LTM 构建。
- 重叠 Geometry Source 取最小 signed distance；颜色 / emission / normal 来自胜出 Source。
- `ColorToFill = albedo + transfer_emission`；PDF 5.11 的 LTM 自发光增益与 MeshLight source emission 分开保存，删除 `emissive_injection` outgoing 旁路。
- Emission Mesh 的静态 `MeshLightSH` 作为 `InComingLight` 在当前 Probe Local Visibility / Local Transfer 之前进入 recurrence；Base Pass 负责显示 authored emission。完整 segment hit 下使用 26-neighbor 非负乘积投影，避免普通 L1 Triple Product 产生负 L0；`Emission Energy Multiplier` 只缩放 MeshLight source，BaseMaterial3D 使用重新对齐 Cycles 的 `2.0` 能量适配系数。禁止隐藏解析灯替代 Emission Mesh，禁止 outgoing emission 旁路。
- Local LRT 逐帧比较已收集 `BaseMaterial3D` 的 albedo、emission enable/color/energy 快照；字段变化时自动重建静态 LTM / MeshLight，解析灯仍只更新 Injection。
- Radiance gather 使用邻居 Local Visibility：先计算 `Trpd(otherRadiance, -otherLocalViSH)`，再沿邻居方向采样并以 `4π × normalized inverse-distance weight` 重投影。Global Visibility 不是 V0 通过条件。
- Directional Light 的 Godot energy 表示 Lambertian diffuse radiance；换算为共享 `2π` SH encoder 输入时乘 `1/2`。Forward 漫反射重建得到 irradiance，进入后续 albedo 乘法前必须乘 `1/π`。
- Forward V0 使用 cubic B-spline；查询中心沿接收面 local normal 外移当前 4-tap kernel 的半支撑宽度 `1.5 × min(actual_spacing)`，再用完整 cubic 权重读取非 `inside_solid` Probe。该做法保持 Radiance 场原始精度，同时避免 receiver half-space 逐 Probe 裁剪在旋转表面产生 cell-phase 条纹。表面漫反射使用 maximum-entropy L1 closure，以 `|D| / (3A)` 作为一阶方向矩并保持球面平均能量；不得将 `|D| / A = 4/3` 直接重映射为饱和单瓣，否则会在 Shadow 边缘产生反向零值黑边。
- 原文规定方向性 Global Visibility 单独传播、Screen Space Gather 的 A 保存天光遮蔽，但未指定 L1 到标量 A 的闭合公式。当前实现保留方向项，将一阶方向矩限制到非负线性 L1 域 `moment ≤ 1/3` 后使用正值 maximum-entropy closure 输出标量 A；不得直接 clamp 线性 SH 负瓣，也不得用 SH0 球面平均代替接收面求值。
- object transform 含缩放时，Color SDF signed distance 按 inverse-transpose normal length 换算到 Volume local，normal 使用 inverse-transpose。
- 静态 Local Geometry / LTM 构建必须 deterministic；不得用 temporal/random dither 掩盖 first-bounce 误差。固定 seed stratified / blue-noise subcell sampling 仅可用于可重复数值积分。
- 原文的 4-neighbor / 3-frame pattern 与 temporal/spatial dither 保留到 v4，且必须以完整 deterministic 26-neighbor 作为 golden reference。

---

# 3. Current Phase

## V4 — 性能优化

Status: `BASELINE_RECORDED`.

### Baseline

- [x] 为 Visibility、Radiance、Environment Injection、Analytic Injection 加入 Godot GPU timestamp。
- [x] 冻结 `35×23×35 = 28,175` Probe、`0.25m`、3 解析灯的同硬件 dev-build 基线。
- [x] GPU：Visibility `0.003562 ms / hop`；Radiance `0.708926 ms / 16 hops`；Analytic Injection `0.015098 ms / 3 lights`。
- [x] CPU：完整 Geometry / Transfer rebuild 中位数 `2106.545 ms`；Dirty update `111.323 ms`（`1690 / 28175` Probe）。
- [x] Dedicated GPU memory `14,798,584 bytes`；full rebuild upload `16,116,708 bytes`；Dirty update upload `2,856,072 bytes`；稳定帧 upload `128 bytes`。
- [x] 基线脚本与口径记录在 `benchmarks/v4_performance/README.md`。

### Validation

- Incremental build PASS。
- Local LRT targeted `67 passed / 4606 assertions / 0 failed`。
- GPU Visibility / Radiance / Analytic Injection regression PASS。
- GPU benchmark 在 RTX 5080、Vulkan Forward+、dev build 上完成四个 1 秒窗口；Visibility 以 10-hop batch 越过内置 `0.01 ms` 输出阈值后按 hop 归一化。
- 退出时仍有既有 `PipelineDeferredRD::~PipelineDeferredRD free()` 清理错误。

### Next Optimization

- 先处理 V1.2 Dirty Region 当前触发的全 Volume Global Visibility reset、全 Volume Injection upload 与逐 row 多次 GPU update；以当前 `2.724 MiB / 111.323 ms` 为 golden baseline，保持局部更新与 full rebuild 全 Probe 结果一致。

---

## V3 — 多 Volume + Priority / Blend

Status: `VISUAL_ACCEPTED`.

### Required Work

- [x] 多个 Volume 独立维护 Probe / Visibility / Radiance / Injection / Shadow state。
- [x] `LocalLRTVolume3D.priority` 经 RenderingServer 传到 RendererRD。
- [x] 重叠选择：与当前摄像机视锥相交的启用 Volume，按 priority 降序、RID 升序，取前 N。
- [x] N 由项目设置 `rendering/global_illumination/local_lrt/max_volumes_per_camera` 配置，范围 `1–8`，默认 `2`；测试工程已设为 `4`。
- [x] 重叠 Blend：高优先级先消耗自身 `edge_weight`，剩余权重交给后续 Volume，再与 World ambient 混合。
- [x] 各自 Local Transform 独立采样。
- [x] 删除一个 Volume 不影响其余 Volume 的 CPU / RID 数据。
- [x] 所有启用 Volume 每帧独立执行 Environment / Shadow / Injection / Propagation。
- [x] 测试场景 `cornell_multi_v3.tscn`：Volume A 覆盖全房间，Volume B 重叠右侧并可用 `R` 旋转。

### Automated / Runtime Validation

- Incremental build PASS。
- Local LRT targeted `72 passed / 4663 assertions / 0 failed`。
- 新增视锥过滤 + priority 截断 + N 夹取 + 4-volume cascade 回归。
- GPU Visibility / Injection / Radiance / Analytic Injection / Directional Shadow Injection PASS（V3 基线，本轮未重跑 GPU）。
- Forward Surface `full=0.06230847 blended=0.00000000` PASS（V3 基线）。
- `cornell_multi_v3.tscn` current run 无项目脚本 / uniform 错误；退出时仍有既有 `PipelineDeferredRD::~PipelineDeferredRD free()` 清理错误。

### Human Visual Validation

PASS — 用户确认 `cornell_multi_v3.tscn`：两 Volume 独立工作、重叠无硬切、Priority 交换生效、Volume B 旋转后仍按自身 Local Transform 采样。
PASS — 用户确认 Project Settings 可配置 N，以及同一摄像机内 `N >= 数量` 时多于 2 个互不重叠 Volume 均可被采样。

---

## V2 — Global GI 注入

Status: `VISUAL_ACCEPTED`; Local LRT specular is explicitly deferred to v5.

### Required Work

- [x] RendererRD 从当前 Environment ambient / Sky irradiance octmap 投影 RGB World SH2，并按 Sky orientation 与 Volume transform 转到 Volume Local SH。
- [x] Sky / Global diffuse 经传播后的 Global Visibility 遮蔽一次后进入 LTM；不重复 Local Visibility，不绕过 LTM 写出 Radiance。
- [x] Sun 保持解析 Directional Shadow 路径，Global Visibility 不参与 Sun shadow。
- [x] 增加 Environment Injection GPU readback / Debug 模式，验证方向旋转、常量输入、能量线性与开放 / 遮挡 Probe 差异。
- [x] Forward 在 Volume 内以受限正值 closure 将方向性 Global Visibility 求为标量 sky-occlusion A，再遮蔽 World ambient、叠加 Local LRT bounce，并按 edge weight 与 Volume 外 ambient 连续混合。
- [x] DynamicGI / Local LRT 共存：DynamicGI 全程更新；Volume 外保留 DynamicGI diffuse，Volume 内按 edge weight 替换为 Local LRT diffuse，且 DynamicGI specular 保持不变。
- [x] 决策：Local LRT specular 不属于 V2 完成条件；在 v5 之前 Volume 内外继续使用 DynamicGI / Reflection Probe / SSR specular。
- [x] 新增开放 Cornell Godot 场景、控制脚本、同一纯色 World 的 Cycles 对照 `.blend` 与 512×512 AgX 截图。
- [x] 用户视觉确认开放 Cornell 的纯色环境能量、内部遮蔽与 Volume 边界无明显异常。

### Automated / Runtime Validation

- Incremental build PASS；最终 targeted / full suite 结果见“Latest Verification”。
- 纯色 Environment Injection 三通道一致；Top / Bottom Probe 常数项为 `0.765042 / 0.400355`，Global Visibility 对外部常量输入产生空间遮蔽。
- Directional Sky rotation 仍由自动 SH 数值测试覆盖；视觉能量对齐不再比较不同程序天空。
- Constant ambient energy `0.5 → 1.0` 时三个通道所有 SH 系数精确 `2×`；Radiance readback 非零。
- Godot current run 无项目错误；编辑器仅有外部 Vulkan registry / OBS layer 警告。
- Benchmark：`benchmarks/v2_global_gi/`；Godot / Cycles AgX 背景像素均为 `(164,164,164)`；修复后 back wall 为 `(61,58,57) / (61,60,59)`，Tall Box 为 `(53,53,50) / (54,53,52)`。

### Human Visual Validation

PASS — 用户已确认 `cornell_global_v2.tscn` 的 16:9 视图、纯色环境能量、内部遮蔽、Volume 边界连续性，以及 `R` 旋转交互。

---

## V1.1 — 动态物体

Status: `COMPLETED`.

### Required Work

- [x] `GI_MODE_DYNAMIC` MeshInstance3D 使用与静态 Geometry 相同的逐物件 Local Color SDF、26 邻域 Local Visibility / Local Transfer / MeshLight 构建路径。
- [x] 动态物体平移、旋转、显隐、Mesh 替换、加入、删除或 GI mode 切换时自动检测变化并执行确定性 full rebuild；未提前实现 V1.2 的 SDF 复用 / Dirty Region。
- [x] `volume_set_static_data()` 在每次动态重建时重新创建并清零 Radiance / Injection buffer，旧位置不残留。
- [x] 单元测试覆盖移动 + 旋转、自动结果与显式 full rebuild reference 全 Probe 一致、删除后旧 Visibility / Transfer / Emission 清理。
- [x] 在复用 Cornell Box 中增加红色动态 Cube、运行时控制及 `benchmarks/v1_dynamic_cornell/` Godot / Blender Cycles 双位置 benchmark。
- [x] 用户视觉确认动态 Cube 接受 GI、红色 bleeding 跟随、遮挡变化生效且旧位置无残留，并允许进入 V1.2。

### Automated / Runtime Validation

- Incremental build PASS；Local LRT targeted `56 passed / 4520 assertions / 0 failed`；full suite `1415 passed / 424533 assertions / 0 failed / 3 skipped`。
- Godot MCP runtime 未调用 `rebuild()`：Cube A → B 后 Geometry count `9 → 9`，旧中心 Probe `inside_solid=false`，新中心 Probe `inside_solid=true`。
- GPU Radiance 总量 `24872.6215 → 23894.4382`，X 空间矩 `429888.6752 → 399344.7801`；动态遮挡 / 反射更新改变最终光场。
- Godot current run 无项目错误；编辑器仅有外部 Vulkan registry / OBS layer 警告。
- Blender Cycles reference 使用 Directional Cornell、512 samples、AgX，frame 1 / 2 对应 Godot A / B 位姿。

### Human Visual Validation

用户确认 V1.1 可进入下一阶段；其 full-rebuild 性能问题由 V1.2 的 Source reuse / Dirty Region 路径解决。

---

## V0.9A — Point / Omni GI Reference Matching

Status: `COMPLETED`.

### Required Work

- [x] Omni range 使用 Godot RendererRD 的 `((1 - (distance / range)^4)^2) × distance^-attenuation`。
- [x] Omni Injection 使用与 Directional 相同的 `1/2` SH energy 换算，并保持 `probe in LocalLight → Injection → Local Visibility → Local Transfer` 次序。
- [x] 复用 Godot positional shadow atlas 的 dual-paraboloid 布局、hemisphere offset、reverse-Z radial depth、bias 与 4-tap PCF。
- [x] Shadow factor 逐灯作用于该灯的直接 RGB SH2 Injection；关闭阴影恢复未遮挡 reference。
- [x] 六主轴、双抛物面边界、reverse-Z depth compare 与 Godot attenuation 单元测试。
- [x] Runtime 隔墙验证：无遮挡 / 遮挡代表探针 Visibility 为 `1 / 0`；有效墙后探针 shadowed Injection `0.0`、关闭阴影后 `0.3318557`。
- [x] Runtime 动态验证：移动 Omni 后 `39 / 384` 个采样探针的 Shadow Visibility 改变；Volume、Geometry 与灯共同平移后 `0 / 384` 改变；保持世界灯不动时 `8 / 384` 改变。
- [x] Godot GPU shadowed Injection 的半能量比为 `0.5`；Cycles Direct / Indirect / Combined 半能量比分别为 `0.4999825 / 0.5026028 / 0.5008565`。
- [x] Debug：独立 Omni Shadow Visibility 与 shadowed Injection；首帧 readback 未就绪时不再访问空缓冲。
- [x] Godot / Blender 单灯 Cornell benchmark、Direct-only、GI-only、Combined、线性 EXR 和截图已冻结在 `benchmarks/omni_cornell_v09a/`。
- [x] 用户复验 Omni 穿墙遮挡、dual-paraboloid 接缝、Direct / GI-only / Combined 与 Cycles 观感。

### Human Visual Validation

在 `cornell_omni_v09a.tscn` 中移动 Omni，确认墙前受光、墙后解析灯 Injection 被抑制、无明显双抛物面接缝，且 Editor / Runtime 收敛一致。

用户于 2026-08-31 确认通过。

---

## V0.9B — Area Light GI Reference Matching

Status: `COMPLETED`.

### Required Work

- [x] 按原文把 Area Light 作为有限范围 `LocalLightSH`，保持 `probe in LocalLight → 邻域 gather → 当前 Probe Local Visibility → Local Transfer` 次序。
- [x] 对齐 Godot `AreaLight3D` 的面积、矩形形状、单面方向、range attenuation 与 `area_normalize_energy`。
- [x] 对矩形发光面进行确定性 `8×8` 面采样并编码 RGB SH2，未使用 emission 或 Point Light 近似。
- [x] 复用 Godot Area positional shadow atlas 与 soft-shadow 参数，以确定性 16-sample blocker search / filter 逐 Probe 计算 Shadow Visibility。
- [x] 添加面积、方向、归一化能量、面采样与软阴影自动验证。
- [x] 建立 Godot / Blender 单灯 Cornell Area benchmark，冻结 Direct-only、GI-only、Combined、线性输出与截图。
- [x] 用户视觉复验软阴影、墙后抑制及 Godot / Cycles 对照。

### Automated / Runtime Validation

- Godot GPU analytic injection：`7 cases / 27 probes`，Area 与独立 CPU `8×8` 面积分 reference 一致。
- Runtime Shadow Visibility：隔两格抽样范围 `0–1`，`451` 个半影探针、`284` 个强遮挡探针。
- Runtime 动态更新：移动 Area 后代表探针 Visibility `0.75 → 0.375`，Geometry count 保持 `8`，未 rebuild。
- 能量缩放：Godot Injection 半能量比 `0.5`；Cycles Direct / Indirect / Combined 为 `0.5000005 / 0.4999729 / 0.4999926`。
- Benchmark：`benchmarks/area_cornell_v09b/` 含 Godot Direct-only / GI-only / Combined / Area Shadow Debug、Cycles linear EXR / AgX 与 `.blend`。

### Human Visual Validation

用户于 2026-08-31 确认通过矩形灯单面方向、箱体软阴影与半影过渡、墙后 Injection 抑制及 Cycles 对照。

---

## V0.9C — Spot Light GI Reference Matching

Status: `COMPLETED`.

### Required Work

- [x] 按原文把 Spot 作为有限范围 `LocalLightSH`，保持 `probe in LocalLight → 邻域 gather → 当前 Probe Local Visibility → Local Transfer` 次序。
- [x] 对齐 Godot `SpotLight3D` 的 exact range attenuation、cone angle 与 `spot_angle_attenuation`。
- [x] 复用 Godot Spot 透视 Shadow Atlas，逐 Probe 执行 reverse-Z depth compare、bias 与 PCF。
- [x] 添加 cone 内外、range 边界、Shadow UV 边界、能量缩放与动态更新自动验证。
- [x] 建立 Godot / Blender 单灯 Cornell Spot benchmark，冻结 Direct-only、GI-only、Combined、线性输出与截图。
- [x] 用户视觉复验锥体边缘、墙后抑制及 Godot / Cycles 对照。

### Automated / Runtime Validation

- Godot 全量单元测试：`1411 cases / 421216 assertions` 全部通过。
- Spot CPU / GPU 统一使用 Godot range window、distance attenuation、cone exponent 与 `1/2` SH energy 换算；运行场景无 shader / render error。
- Runtime Shadow Visibility：`28175` 个 Probe 范围 `0–1`，`6366` 个被遮挡 Probe，包含 `19` 个 4-tap PCF 分数样本。
- 动态更新：旋转 Spot 后注入 Probe 数 `3397 → 3460`，静态 Geometry count 保持 `8`。
- 能量缩放：Godot Injection 半能量比 `0.5`；Cycles Direct / Indirect / Combined 为 `0.4999993 / 0.4999298 / 0.4999791`。
- Benchmark：`benchmarks/spot_cornell_v09c/` 含 Godot Direct-only / GI-only / Combined / Spot Shadow Debug、Cycles linear EXR / AgX 与 `.blend`。

### Human Visual Validation

用户于 2026-08-31 确认通过。

---

## v0 总验收

Status: `COMPLETED`.

### Required Work

- [x] P0 / v0 数学与机制全量测试通过；GPU Visibility / Injection / Radiance / Analytic Injection / Directional Shadow 与 Forward Surface 回归通过。
- [x] Directional / Omni / Area / Spot 在同一 Cornell 场景组合启用；运行时移动 Area 后 Geometry count 保持 `8 → 8`，未触发静态 LRT rebuild。
- [x] 删除 CPU Probe、RenderingServer、RendererRD buffer / binding / shader 中遗留的 `emissive_injection` outgoing 旁路。
- [x] Geometry emission 按 PDF 拆分为 MeshLight source 与 `ColorToFill = albedo + transfer_emission`；Godot 运行时确认 `380` 个非零 Emission Probe。
- [x] 建立 Godot / Blender 四灯组合与 Emission 增量 benchmark，冻结 512×512 AgX 截图与 `.blend`。
- [x] Godot MCP 独立重启组合场景与 Emission 场景，current run 无项目脚本 / 渲染错误。
- [x] 用户复验四灯组合的阴影注入、Color Bleeding、暗部、墙后抑制与 Editor / F5 一致性。
- [x] 按原文 `if (probe in MeshLight)` 补充静态 `MeshLightSH` incoming source，并确保它在当前 Probe Local Transfer 之前进入 recurrence；未恢复 outgoing emission 旁路。
- [x] 增加所有解析灯关闭的 `cornell_v0_emission_mesh.tscn`，Emission Mesh Base Pass 使用真实 `StandardMaterial3D` emission。
- [x] 完成 MeshLight CPU / GPU 自动回归、增量构建、Emission-only Godot / Cycles reference 与截图。
- [x] 修复 Emission Energy Multiplier 改动只更新 Base Pass、未使已烘焙 MeshLight 失效的问题；现在无需手动 `rebuild()`。
- [x] 修复同一 multiplier 同时放大 MeshLight source 与 `ColorToFill` 导致的超线性；当前 `8 → 16` 的 GPU 最大 Radiance 为 `4.1105776 → 8.2211552`，比例 `2.0000`。
- [x] 冻结 Godot Multiplier `8` / Cycles Strength `8` 对照，更新 `.tscn`、`.tres`、`.blend` 元数据与两张 512×512 截图。
- [x] 用户复验 Emission Mesh 自身亮度、暖色 Local GI、无隐藏解析灯及无跨墙漏光。

### Automated / Runtime Validation

- Incremental build PASS；全量测试 `1414 passed / 421602 assertions / 0 failed / 3 skipped`；新增 source/transfer 分离与全探针 2× 线性回归。
- GPU Visibility / Injection / Radiance / Analytic Injection / Directional Shadow 全部 PASS；Radiance marker 含 `mesh_light=1`；Forward Surface `full=0.08516519`、`blended=0.00037243`。
- 四灯组合场景 `has_built_data=true`，四类灯全部 visible；Area 运行时位移前后 Geometry count 均为 `8`。
- Emission 场景 `has_built_data=true`，resolution `35×23×35`，`380` 个 Probe 具有非零 emission。
- 独立 Emission Mesh 场四灯均关闭；Godot Multiplier `8` 时 Radiance `61368 / 84525` 个 SH 值非零，最大分量 `4.1105776`，无 NaN / Inf；Multiplier `16` 时最大分量 `8.2211552`，严格 2×。
- Cycles 独立 reference 使用真实 Diffuse + Emission shader，四个 Light object 全部 `hide_render=true`，512 samples。
- 静态材质自动更新：单元测试确认 Energy `2 → 4` 后 MeshLight source emission 线性 2×，相邻 Probe 的 LTM 不变；Godot MCP runtime 未调用 `rebuild()` 时，Multiplier `8 → 16` 使最大 Radiance `4.1105776 → 8.2211552`。
- Benchmark：`benchmarks/v0_acceptance_cornell/`。

### Human Visual Validation

用户于 2026-08-31 确认四灯组合与独立 Emission Mesh 全部通过，允许进入 V1.1。

---

## V0.8 — Directional Light GI Reference Matching

Status: `COMPLETED`.

### Objective

V0 前半期只启用 Directional Light。完成 Shadow-aware Injection 后，以相同 Cornell Geometry、Lambertian 材质、黑色 World、相机、512×512 输出、AgX / exposure 和实际方向光辐照度，与 Blender Cycles 建立一对一 reference。Omni / Area / Spot 在本阶段全部关闭。

### Required Work

- [x] GPU Analytic Light Injection compute：每个 Probe 用 Volume transform 恢复 World position，按冻结 SH 约定写入 RGB SH2；跳过 `inside_solid`。
- [x] GPU unshadowed Directional / Omni / Spot 与 CPU reference 一致。
- [x] 灯光或 Volume transform 变化只更新 Injection，不重建 Local Visibility / Local Transfer，不清空 Radiance history。
- [x] Volume Directional Shadow Map（独立于相机 CSM）；Caster 含 Volume 外能向 Volume 投影的静态物体。
- [x] `DirectionalLightSH × Shadow Visibility`；墙前 Injection 正常，墙后接近零；关阴影后回到 unshadowed CPU reference。
- [x] 顺序固定 `Shadow → Injection → Propagation → Forward`。
- [x] Debug：Directional Shadow Visibility、shadowed Directional Injection、reflected Radiance。
- [x] Cornell / Cycles 基准输入配置：仅方向光、黑色 World、无 emission、Lambertian 材质、相同相机与 512×512 输出、AgX、Exposure 0。
- [x] 方向光输入换算：Blender Sun Strength `5.0` 对应 Godot 非物理 Directional Energy `5 / π = 1.5915494`；两端灯色与材质颜色按各自 sRGB / linear 存储语义对齐。
- [x] Cycles reference 固定为 Blender 5.1.2、512 samples、32 diffuse bounces、无 denoise、无 direct / indirect clamp；Godot 嵌入运行窗口固定保持 512×512，不再 Stretch to Fit。
- [x] 两端 combined reference 已成功出图且相机画幅 / Geometry 轮廓一致；Godot 运行日志无错误。当前可见剩余差异集中在 Directional GI 能量与空间分布。
- [x] 分别采集 direct-only、GI-only 与合成线性输出；校正 Blender Short / Tall Box 旋转后，无遮挡地板 reference pixel 的 Direct RGB 为 Godot `(0.56630, 0.50032, 0.40311)`、Cycles `(0.56417, 0.50212, 0.40088)`，Short Box 正面为 Godot `(0.29992, 0.26574, 0.21405)`、Cycles `(0.29931, 0.26639, 0.21268)`；各通道误差 `< 0.7%`，无 clipping。Geometry、方向光强、颜色、Lambert 材质与曝光口径已对齐。
- [x] 首个 GI 数值偏差已定位并修复：旧实现直接平均邻居 SH coefficient，导致方向能量逐 hop 稀释；同时 Directional Injection 缺少 `1/2` 能量换算、Forward irradiance 缺少 `1/π` 转换，合计造成约 `2π` 的口径偏差。
- [x] 排除“只有一次反弹”和“距离衰减过强”：Runtime 每帧执行 `propagation_iterations = 16` 的持续 A/B recurrence，当前 GPU `decay_per_meter = 1.0`。增加 Cycles bounce 数不是当前首要差异来源。
- [x] 独立 CPU A/B 已依次完成：equal-weight LTM、equal / unnormalized gather、Neumann local reflection 均被排除；确认 26-direction streaming gather 是空间分布根因。
- [x] CPU / GPU 同步实现方向 gather；Directional-only Injection 输入乘 `1/2`；Forward 输出乘 `1/π`；表面 L1 使用 maximum-entropy 正值 closure。
- [x] Cycles GI-only 数值对照：Tall Box 阴影面 LRT `(0.05464, 0.05464, 0.03877)` 对 Cycles `(0.05160, 0.05058, 0.03616)`，误差约 `6–8%`；地面 LRT 均值 `0.01345` 对 Cycles `0.01540`，空间分布不再出现近场过亮 / 阴影区过暗的相反偏差。
- [x] 固化 CPU/GPU/Forward 自动回归并完成人工 Combined 视觉验收；Directional benchmark 已冻结。
- [x] 回退无物理依据的多轮 Radiance 切向预滤波；Forward 恢复读取原始 A/B reflected Radiance。
- [x] 回退 finite-volume、geometry-guidance 与 Trace LTM 试验；A/B 证明它们只改变条纹频率/低频形态或增加烘焙成本，不是该问题根因。
- [x] 修正缩放几何的 Volume-local distance / normal。
- [x] Forward 改为 cubic 半支撑宽度的外侧连续取样，不再以 receiver half-space 权重逐 Probe 裁剪。
- [x] CPU 全量单元测试、GPU Visibility / Injection / Radiance / Directional Shadow、Forward Surface 回归通过。
- [x] MCP 默认与近景 capture 均不再出现 Tall/Short Box 周期条纹，接触阴影从几何边缘连续展开。
- [x] 定位并修复 Directional Shadow 边缘黑色勾边：旧单瓣重建把饱和 diffuse lobe 的反方向强制为零；maximum-entropy L1 closure 保持平均能量、由一阶方向矩构造，且不制造零值断层。
- [x] 用户要求进入下一子阶段，视为完成 Cornell Box 表面条纹、接触处与 Directional Shadow 边缘复验。

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

Status: ENERGY / DATA SEMANTICS COMPLETED; SURFACE PRECISION REOPENED IN V0.8
Date: 2026-08-29
Visual PASS: 2026-08-29

Implemented:
- per-object Color SDF 与 Radiance Probe 分离；`geometry_voxel_size` 独立于 `probe_spacing`。
- SampleDir LTM、`ColorToFill = albedo + transfer_emission`、`inside_solid` GPU/Forward 丢弃规则。
- 旧的旋转薄板 spacing 结论已撤销：它只覆盖旧三线性 reference，未复现当前 Forward reconstruction，也没有证明条纹振幅随密度下降。新的 `0.25 / 0.125m` 测试分别记录方差与最大相邻跳变。

Human Visual Validation:
- Color SDF / inside_solid 语义 PASS；表面条纹精度 PASS 已撤销并转入当前 V0.8 子项。

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
- scene/3d/local_lrt_volume_3d.{h,cpp}
- servers/rendering/rendering_server.{h,cpp}
- servers/rendering/rendering_server_default.h
- servers/rendering/environment/renderer_gi.h
- servers/rendering/dummy/environment/gi.h
- drivers/gles3/environment/gi.{h,cpp}
- servers/rendering/renderer_rd/environment/gi.{h,cpp}
- servers/rendering/renderer_rd/environment/local_lrt.{h,cpp}
- servers/rendering/renderer_rd/renderer_scene_render_rd.cpp
- servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.{h,cpp}
- servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered_inc.glsl
- servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl
- tests/scene/test_local_lrt_math.cpp
- tests/scene/test_local_lrt_volume_3d.cpp
- local_lrt_volume_misc/test_project/cornell_multi_v3.tscn
- local_lrt_volume_misc/test_project/multi_volume_v3_controller.gd
- local_lrt_volume_misc/LOCAL_LRT_STATE.md

Relevant Symbols / Functions:
- LocalLRTMath::volume_priority_before
- LocalLRTMath::volume_cascade_blend_weights
- LocalLRTVolume3D::set_priority
- LocalLRT::get_sorted_enabled_volumes
- LocalLRT::get_surface_data
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
Compile: PASS — V0.9A Omni positional-shadow incremental editor build
Unit Tests: PASS — 50 cases / 1186 assertions (`[LocalLRTMath]`, `[LocalLRTBuilder]`, `[LocalLRTVolume3D]`)
GPU Visibility Validation: PASS — Vulkan Forward Mobile; 1/2/4/8 iterations matched pinned CPU values
GPU Injection Validation: PASS — 81 RGB SH2 values uploaded/read back exactly and clear returned zero
GPU Radiance Validation: PASS — Vulkan Forward Mobile; directional-streaming CPU reference matched 1/2/4/8 iterations and persistent 1+1-step propagation for all 81 RGB SH2 values
GPU Analytic Injection Validation: PASS — Directional、Godot exact Omni attenuation / `1/2` energy 与 Spot matched independent CPU reference
GPU Directional Shadow Injection Validation: PASS — Vulkan Forward Mobile; Directional `1/2` normalization and synthetic reverse-Z plane occluder matched 2 cases / 125 probes
Runtime Smoke Test: PASS — Forward+ and Dummy/headless Cornell Box loaded without errors
Runtime Dynamic Radiance: PASS — moving Omni changed center Probe radiance; has_gpu_data=true
Runtime Radiance Capture: PASS — Directional-only and Omni-only captures completed; analytic lights remain unshadowed until an explicit renderer shadow input implements the reference `probe not in Shadow` condition
Directional Isolation Validation: PASS — residual Radiance was traced to the EmissionPanel (max R SH length 1.34666); with all sources disabled it is exactly zero. Analytic-light isolation now disables the panel emission and rebuilds Local LRT data.
Forward Surface Validation: PASS — Forward+ Vulkan framebuffer consumes the `1/π` receiver normalization and diffuse-lobe `4/3` reconstruction (`full=0.06437079`, `blended=0.00024797`).
Forward+ Runtime Binding: PASS — shader/UBO/storage binding initialized without Local LRT uniform errors.
Fine Grid Rebuild: PASS — runtime `probe_spacing=0.25` rebuilt resolution `35×23×35` with valid CPU/GPU data；修复 Inspector grid property 修改只清空、不重建的问题。
Three-light Direction-Corrected Runtime: PASS — after the reference-direction LTM fix, 16-frame isolated captures show positive, spatially distributed Local LRT contributions: Directional `0.05752297→0.05754675` with `7495→7495` changed samples, Omni `0.03971097→0.03977896` with `8084→8084`, and Spot `0.01000463→0.01002052` with `4903→4915`; no prior negative-energy edge truncation observed in runtime screenshots.
V0.2 Surface Voxel Field: PASS — 半开法线区间 + sample_mask 并集；相位 / 细分 / spacing coverage 测试通过
V0.5 Spacing Stages: PASS — 平面与红白内角相对独立 0.125m reference；残差收敛；贴表面 Transfer / 一次反射 / 收敛 Radiance 误差不反向
V0.7 Transform Parity: PASS — unit test; transform does not rebuild Local GI; world lights re-inject; co-moving lights keep local injection
Directional Cornell / Cycles Direct: PASS — floor and Short Box RGB error below 0.7%
Directional Cornell / Cycles GI-only: NUMERICAL PASS — Tall Box shadow RGB error about 6–8%; floor mean 0.01345 vs 0.01540
Human Visual Validation: PASS — 用户确认 V0.6（bleeding / 暗部 / Volume 外 / Edge Blend）与 V0.7（Shift 平移旋转不重建）；V0.8 Directional Cornell / Cycles Combined 除离线噪点外视觉差异已不大。
Surface Precision Incremental Build: PASS — `python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6`。
Surface Precision Unit Tests: PASS — targeted LocalLRTBuilder `22 cases / 374 assertions`；full suite `1407 cases / 421156 assertions`。
Surface Precision GPU Regression: PASS — Visibility、Injection、Radiance、Analytic Injection、Directional Shadow Injection；Forward Surface marker `full=0.08990950`, `blended=0.00043722`。
Surface Precision Visual Capture: AI PASS / WAITING USER — MCP 512×512 默认与近景 capture 中 Tall/Short Box 的 Probe 周期竖纹已消失，地面接触阴影保持连续。
Omni Unit Coverage: PASS — 六主轴、dual-paraboloid seam、reverse-Z radial depth compare、exact attenuation / energy。
Omni Runtime Shadow: PASS — 有效墙后 Probe shadowed Injection `0.0`，关闭阴影为 `0.3318557`；移动灯改变 `39/384`，共同平移 `0/384`，世界灯固定时 `8/384`。
Omni Energy Scaling: PASS — Godot shadowed Injection 半能量比 `0.5`；Cycles Combined / Direct / Indirect 为 `0.5008565 / 0.4999825 / 0.5026028`。
Omni Cornell Capture: AI PASS / WAITING USER — Godot Direct-only / GI-only / Combined、Omni Shadow Debug 与 Blender Cycles 512-sample reference 已冻结。
Area Incremental Build: PASS — `python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6`。
Area Unit Coverage: PASS — Local LRT targeted suite `51 cases / 1194 assertions`；矩形面积、单面方向、normalize energy 与半能量缩放通过。
Area GPU Analytic Injection: PASS — Directional / Omni / Spot / Area 共 `7 cases / 27 probes`，Area `8×8` 面采样与独立 CPU reference 一致。
Area Runtime Shadow: PASS — Visibility `0–1`，`451` 个半影探针、`284` 个强遮挡探针；移动灯后代表探针 `0.75 → 0.375`，Geometry count `8 → 8`。
Area Energy Scaling: PASS — Godot Injection 半能量比 `0.5`；Cycles Combined / Direct / Indirect 为 `0.4999926 / 0.5000005 / 0.4999729`。
Area Regression: PASS — GPU Visibility / Injection / Radiance / Directional Shadow 与 Forward Surface marker 全部通过；Area Cornell 独立重启后的当前 run 日志无项目错误。
Area Cornell Capture: PASS — Godot Direct-only / GI-only / Combined / Area Shadow Debug 与 Blender Cycles 512-sample reference 已冻结，用户已验收。
Spot Human Visual Validation: PASS — 用户于 2026-08-31 确认通过。
V0 Total Acceptance Incremental Build: PASS — 当前代码增量构建完成。
V0 Total Acceptance Full Unit Tests: PASS — `1411 passed / 421213 assertions / 0 failed / 3 skipped`。
V0 Total Acceptance GPU Regression: PASS — Visibility / Injection / Radiance / Analytic Injection / Directional Shadow 与 Forward Surface marker `full=0.08516519`, `blended=0.00037243`。
V0 Emission Semantics: PASS — 删除 `emissive_injection` outgoing 旁路；MeshLight source 与 `ColorToFill = albedo + transfer_emission` 分离；运行时 `380` 个非零 Emission Probe。
V0 Combined Runtime: PASS — Directional / Omni / Area / Spot 同时启用，Area 移动前后 Geometry count `8 → 8`，最终 current run 无项目错误。
V0 Acceptance Capture: AI PASS / WAITING USER — Godot / Cycles 四灯组合与 Emission 增量截图冻结在 `benchmarks/v0_acceptance_cornell/`。
V0 Emission Mesh Incremental Build: PASS — MeshLight buffer / binding / shader 与场景增量构建完成。
V0 Emission Mesh Unit Regression: PASS — source/transfer 职责与 2× 线性 targeted tests 通过；full suite `1414 passed / 421602 assertions / 0 failed / 3 skipped`。
V0 Emission Mesh GPU Regression: PASS — `LOCAL_LRT_GPU_RADIANCE_PASS ... mesh_light=1`；其余 Visibility / Injection / Analytic / Directional Shadow / Forward Surface 全部通过。
V0 Emission Mesh Runtime: PASS — 四灯全部关闭；Multiplier `8` 时 Radiance `61368 / 84525` 非零、最大分量 `4.1105776`；`8 → 16` 得到 `4.1105776 → 8.2211552`，比例 `2.0000`。
V0 Emission Mesh Capture: AI PASS / WAITING USER — Godot Base Pass + Local GI 与 Cycles 真实 Emission-only reference 已冻结。
V0 Static Material Invalidation: PASS — Emission Energy Multiplier 无需手动 rebuild；source emission 与 transfer emission 已分离，MCP runtime `8 → 16` 精确 2×，无项目错误。
V0 Total Human Acceptance: PASS — 用户于 2026-08-31 确认独立 Emission Mesh 通过，v0 总验收完成。
V1.1 Incremental Build: PASS — 当前代码增量构建完成。
V1.1 Unit Regression: PASS — targeted `56 cases / 4520 assertions`；full suite `1415 passed / 424533 assertions / 0 failed / 3 skipped`。
V1.1 Dynamic Geometry Runtime: PASS — 未显式 rebuild；A → B 后 Geometry count `9 → 9`、旧中心 `inside_solid=false`、新中心 `inside_solid=true`，GPU Radiance 总量与 X 空间矩均改变。
V1.1 Cornell Capture: AI PASS / WAITING USER — Godot / Cycles A、B 两位置 512×512 AgX 截图与 `.blend` 已冻结在 `benchmarks/v1_dynamic_cornell/`。
V1.1 Human Visual Validation: PASS — 用户允许进入下一阶段。
V1.2 Incremental Build: PASS — `python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6`。
V1.2 Unit Regression: PASS — targeted `57 passed / 4534 assertions / 0 failed`；full suite `1416 passed / 424580 assertions / 0 failed / 3 skipped`。
V1.2 Source Reuse: PASS — 纯 transform 更新 SDF build count `11 → 11`；局部结果与显式 full rebuild 的 Visibility / Transfer / Emission / `inside_solid` 全 Probe 一致。
V1.2 Dirty Region Runtime: PASS — 15 次同步更新平均 `7.999 ms`、最大 `9.944 ms`；平均 Dirty `1123.33 / 28175`（`3.99%`），最大 `1320 / 28175`（`4.68%`）。
V1.2 Cornell Capture: AI PASS / WAITING USER — 动态 Box / Sphere / Slope 的 Godot / Cycles Pose A、B 截图、`.blend` 与 `benchmark.json` 已冻结在 `benchmarks/v12_dynamic_source_reuse/`。
V1.2 Human Correctness Acceptance: PASS — 用户要求将性能优化后置并继续下一阶段；同步 Dirty Region 的帧率优化留到 v4。
V2 Incremental Build: PASS — `python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6`。
V2 Unit Regression: PASS — targeted `58 passed / 4536 assertions / 0 failed`；full suite `1417 passed / 424599 assertions / 0 failed / 3 skipped`。
V2 World/Sky Runtime: PASS — World irradiance 投影为 RGB SH2，按 Sky orientation / Volume transform 旋转，经 Global Visibility 单次遮蔽后进入 LTM；代表 Radiance readback 非零。
V2 Rotation / Scaling: PASS — Sky X 轴 90° 将 R 方向项 `local Z 0.0161373 → local X 0.0161372` 且常数项不变；ambient energy `0.5 → 1.0` 精确 `2×`。
V2 Sky Occlusion: PASS — 纯色 World 下开口上方 / 底部 Probe R 常数项 `0.765042 / 0.400355`；Global Visibility 空间遮蔽生效。
V2 Forward Environment Composition: PASS — Volume 内以 Global Visibility 遮蔽 World ambient，再叠加 Local LRT bounce；edge weight 与 Volume 外原始 ambient 连续混合，不再重复叠加未遮蔽 ambient。
V2 Constant World Alignment: PASS — 两端使用同一 `#808080` lighting radiance；AgX 背景均为 `(164,164,164)`，修复后代表 back wall 为 Godot `(61,58,57)` / Cycles `(61,60,59)`，Tall Box `(53,53,50)` / `(54,53,52)`。
V2 Cornell Capture: AI PASS / WAITING USER — 纯色 World 的 Godot / Cycles、常量不变量与 Environment Injection Debug 已冻结在 `benchmarks/v2_global_gi/`。
Specular Comparison Harness: PASS — 新增完全独立的 `cornell_specular_compare_independent.tscn`，场景内直接 authored 两个相同 Cornell 组的几何、材质、灯光、WorldEnvironment、Camera 与 LocalLRTVolume3D，不引用任何其他地图或 PackedScene；左侧为 DynamicGI 对照，右侧启用 Local LRT，并包含相同的金属低粗糙度球作为 specular 观察目标。运行时已确认 `1/2/3` 模式切换与 `V` Probe 调试无项目错误。
V2 Sky Occlusion Closure Fix: PASS / WAITING USER — 线性 SH 负瓣会在 Probe cell 边界形成周期零值区；现将 visibility moment 限制为 `≤ 1/3` 后执行正值 closure。直接 `|D| ≤ A` 线性压缩因明显抬高整体天光已回退。增量构建 PASS；Local LRT targeted `64 / 4596`；full suite `1418 / 424602`；Godot MCP v2 与四灯夹角近景无周期黑斑且 current run 无项目错误；Blender MCP 512-sample 夹角 reference 完成。
V2 Scene Switch Isolation: PASS / WAITING USER — `LocalLRTVolume3D` 构造期不再启用后端 Volume，进入 / 退出 SceneTree 时同步 enabled；同一 Godot runtime 进程经 `cornell_box → cornell_global_v2 → cornell_box` 后，前后 512×512 PNG 的 SHA-256 完全一致，game log 无错误。
Test Harness Camera / Rotation UX: PASS — 测试工程窗口与 viewport 统一为 `1152×648`（16:9）；`cornell_global_v2.tscn` 运行时显示姿态状态，`R` 使用 physical keycode 后确认将 Volume Y 旋转到 `90°`，当前运行无项目错误。
V0.2 Spacing / Grid-phase Repair: PASS / WAITING USER — 上一轮截图实际都运行了磁盘中的 `0.25m`，已作废。运行时分别确认 `0.25m = 35×23×35`、`0.5m = 18×12×18`。撤销把 Probe-cell footprint 体积分数乘入 LTM 的错误路径；现对固定 26 个方向端点读取同一 Color SDF 的中心 / 端点，通过端点负 SDF 或两端外向法线相反且 surface-distance 和不超过段长判定薄表面穿越，并按原文取得完整 `ColorToFill` 样本。最终 A/B 中 `0.5m` 不再系统性丢失间接光；固定 back-wall framebuffer ROI 的 sRGB luminance 均值为 `48.60 / 43.83`（`0.25 / 0.5m`），残余约 `9.8%` 差异与转角带宽继续作为空间重建误差处理。
V0.5 Radiance Visibility Application: PASS — 当前 Probe 的 Local Visibility 已在 `Trpd(gathered, local)` 中方向性应用一次；移除随后再次乘 SH0 平均 open fraction 的重复衰减。新增空空间 continuation 回归；canonical red-wall golden 更新为 R `0.842778`、G `0.0963297`。
V0.2 Spacing Regression: PASS — 新增 thin slab `0.5 / 0.25m` 与两种 grid phase 回归；Local LRT targeted `61 cases / 4555 assertions / 0 failed`。
V2 Frame-budgeted Visibility: PASS — `visibility_iterations` 改为每帧 Probe-hop 预算，静态更新仅重置 A/B；新增 RenderingServer propagation API，按最近 Volume 边界半径自动停止。GPU validation `steps=1,2,3,4 budget=2 stable=true spacing_scale=true probes=729`，Injection / Radiance / Analytic Injection / Directional Shadow Injection GPU 回归全部 PASS。
V2 DynamicGI / Local LRT Composition: PASS — 根因是 Local LRT 合成位于 `USE_LIGHTMAP` 的排他分支内，DynamicGI 使用该 shader 变体时会跳过 Local LRT。Forward+ 现先完成 DynamicGI diffuse / specular，再在实际未使用 Lightmap / VoxelGI 的实例上以 Local LRT `edge_weight` 仅替换 diffuse；Local LRT 使用替换前的 Environment ambient 构建自身结果，DynamicGI specular 保持原路径并登记 Local LRT specular TODO。增量构建 PASS；Local LRT targeted `61 cases / 4555 assertions / 0 failed`；Forward surface `full=0.09720786 blended=0.00042828` PASS；DynamicGI composition `difference=0.08887708 drift=0.00121560` PASS，并输出 512×512 对照截图。
V2 Emission Mesh / DynamicGI Composition Regression: PASS — 完整 opaque segment hit 令普通 `Trpd(MeshLightSH, LocalVisibilitySH)` 的 L0 全部为负，Forward 因非负重建而得到黑色。MeshLight 专用路径现用同一完整 26-neighbor 集做非负乘积投影，仍在 LTM 前消费 source；解析灯与邻居 Radiance 保持原 Triple Product。完整命中消除了旧 sparse sampling 对能量的隐式衰减，因此 MeshLight scale 从历史 `64.0` 重新以 Cycles Strength `8` 校准为 `2.0`；最终 MCP 截图均值 `14.0577`，冻结 Cycles reference 为 `13.2236`。增量构建 PASS；Local LRT targeted `61 cases / 4555 assertions / 0 failed`；Emission targeted `5 cases / 403 assertions / 0 failed`；GPU Radiance、Forward Surface 与 DynamicGI composition 均 PASS，最终 composition 为 `emission=0.05043339 difference=0.08989162 drift=0.00039733`。CLI 退出时仍有既有 `PipelineDeferredRD::~PipelineDeferredRD free()` 清理错误。
Forward Corner A/B: USER ACCEPTED — 用户已确认当前 V2 Cornell 视觉结果；现有外侧 cubic reconstruction 与 Cycles 的宽转角差异保留为后续质量改进项。
V3 Incremental Build: PASS — `python -m SCons platform=windows target=editor dev_build=yes tests=yes accesskit=no d3d12=no -j6`。
V3 Unit Regression: PASS — targeted `65 passed / 4591 assertions / 0 failed`；覆盖 priority 稳定排序、cascade blend 与删除独立性。
V3 GPU Regression: PASS — Visibility / Injection / Radiance / Analytic Injection / Directional Shadow Injection。
V3 Forward Surface: PASS — `full=0.06230847 blended=0.00000000`。
V3 Multi-Volume Runtime: PASS — `cornell_multi_v3.tscn` 启动无项目脚本 / uniform 错误。
V3 Human Visual Validation: PASS — 用户确认双 Volume、重叠 Blend、Priority 与旋转采样。
V3 Per-Camera N: PASS — 用户确认可配置 N 与同一摄像机内多于 2 个 Volume。
V4 Baseline Incremental Build: PASS — GPU timestamp 与 benchmark harness 已编译。
V4 Baseline CPU / Memory / Upload: PASS — `28175` Probe；full rebuild median `2106.545 ms`；Dirty `1690` Probe / `111.323 ms`；dedicated GPU `14.113 MiB`；full upload `15.370 MiB`；dirty upload `2.724 MiB`。
V4 Baseline GPU: PASS — RTX 5080 Vulkan Forward+ dev build；Visibility `0.003562 ms/hop`；Radiance `0.708926 ms/16 hops`；Analytic Injection `0.015098 ms/3 lights`。
V4 Baseline Regression: PASS — targeted `67 / 4606`；GPU Visibility / Radiance / Analytic Injection PASS。
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
- Forward surface sampling 沿 local normal 外移 cubic kernel 半支撑宽度 `1.5 × min(actual_spacing)`，再读取完整非实体 Probe 支撑域；不再逐 Probe 应用 receiver half-space 裁剪。
- Occupancy / Geometry Coverage Debug 按 fractional coverage 着色；Inside Solid 模式才把 `inside_solid` 画成洋红。Radiance 不再覆盖洋红 occupied。
- V0.7 Cornell Box 用 Shift 平移/旋转房间，相机保持世界位姿。
- Debug Probe 半径为 `min(debug_probe_scale, min(actual_spacing)*0.35)`。
- Color SDF 的 `0.25 / 0.125m` 对照表明：提高 Probe 密度只缩短条纹周期。64-sample Trace LTM A/B 也不消除条纹，因此该试验已完整回退。
- Color SDF 的每个方向样本以中心—端点 SDF crossing 判断薄表面命中，命中后使用完整 `ColorToFill`；不再把 Probe-cell footprint 体积分数乘入能量，也不会把未穿越表面的第二层 Probe 误记为 LTM。
- 当前 Forward 使用外侧连续 cubic reconstruction；已直接读取 Global Visibility 并输出线性 SH sky-occlusion A，但尚未实现 v4 的低分辨率 Screen Space Gather 缓存，也未使用 geometry-guidance 或 Radiance volume blur。
- Occupancy-grid `rasterize_triangle` / `set_occupancy` 仍是离散回归路径：`inside_solid = coverage > 0`。Runtime Volume 只走 Color SDF。
- Canonical red-wall occupancy golden 在当前 Probe Local Visibility 只应用一次后为 visibility X `1.06501`、radiance R X `0.842778`、G X `0.0963297`。
- GPU 已上传 `inside_solid`；GPU Injection / Radiance 跳过 `inside_solid`。Forward 在外移后的连续查询中心用完整 cubic 权重从非实体 Probe 重建表面 Radiance。
- V1.2 的 Dirty Region 只局部更新语义所需的 Visibility / Transfer / MeshLight / `inside_solid`；Dirty Region 外的 `signed_distance` 调试元数据保持上次 full rebuild 值，不参与运行时传播或 Forward 结果。
- 当前局部更新仍在主线程同步执行；跨 Source region 合并、工作预算、异步构建与更紧凑的 GPU copy 留给 v4。
- V1.2 动态物体编辑器 / Runtime 帧率仍不满足实时目标；本轮仅确认正确性，异步更新、工作预算、跨帧调度与 GPU copy 合并统一留到 v4。
- Local LRT specular 尚未实现；在接入前，Volume 内外继续使用 DynamicGI specular。后续按 Local LRT `edge_weight` 替换，不与 DynamicGI specular 相加。
- V3 Forward 同一摄像机视锥内最多绑定 N 个 Volume；视锥外及超出 N 的 Volume 仍独立更新，但不进入当前像素采样。

---

# 10. Blockers / Decisions Needed

- 无实现阻塞。V4 未优化基线已记录。

---

# 11. Next Action

```text
优化 V1.2 Dynamic Dirty Region 上传：消除不必要的全 Volume Global Visibility reset、全 Volume Injection upload 与逐 row 多次 GPU update，并与 full rebuild reference 对照。
```

---

# 12. Session Handoff

```text
Last Session Summary:
V4 已启动并冻结未优化基线。RendererRD 的四个 Local LRT compute pass 已加入 GPU timestamp；新增 CPU / memory / upload 与 sustained GPU benchmark harness。

Current Phase:
V4 — 性能优化

Current Status:
V4_BASELINE_RECORDED — 当前 28,175 Probe golden baseline 已记录。

What Was Completed:
- GPU timestamp: Visibility / Radiance / Environment Injection / Analytic Injection
- `v4_baseline_benchmark.gd`: CPU rebuild、Dirty update、dedicated memory、upload accounting
- `v4_gpu_baseline_benchmark.gd`: sustained GPU profile
- `benchmarks/v4_performance/README.md`: frozen hardware、scene、commands、results、accounting scope

Test Results:
- Incremental build PASS
- Local LRT targeted `67 cases / 4606 assertions / 0 failed`
- GPU Visibility / Radiance / Analytic Injection PASS
- GPU: Visibility `0.003562 ms/hop`; Radiance `0.708926 ms/16 hops`; Injection `0.015098 ms/3 lights`
- CPU: full median `2106.545 ms`; dirty `111.323 ms`

Human Visual Validation:
- V4 baseline instrumentation only; no visual change and no new visual validation required.

Exact Next Step:
- Optimize Dynamic Dirty Region uploads and state resets against the frozen baseline without changing the deterministic 26-neighbor reference.
```
