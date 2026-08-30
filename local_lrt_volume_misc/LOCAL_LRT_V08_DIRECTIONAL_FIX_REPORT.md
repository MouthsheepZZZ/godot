# Local LRT V0.8 Directional GI 修复记录

日期：2026-08-30  
阶段：V0.8 — Directional Light GI Reference Matching  
参考实现：`local_lrt_volume_misc/lrt_ref.pdf`  
标准对照：`local_lrt_volume_misc/benchmarks/directional_cornell_v08/`

## 1. 结论

本轮不是通过提高全局 GI 倍率、改变曝光或增加传播次数来“调得像 Cycles”，而是修复了四个互相叠加的实现问题：

1. 邻居 Radiance 直接按 SH coefficient 混合，丢失了 26 邻居所代表的离散入射方向。
2. Godot Directional Energy 与共享 SH 注入编码器之间缺少 `1/2` 的能量口径换算。
3. Forward 采样把 irradiance 当作 diffuse radiance 交给后续 albedo 乘法，缺少 `1/π`。
4. Non-linear L1 重建沿用了 delta-light 的方向性上限 `2`，而 LRT 输出是 diffuse transfer lobe，其上限应为 `4/3`。

修复后，Godot 与 Cycles 的 Directional-only Cornell Box 除 Cycles Monte Carlo 噪声、SH2 低频限制和有限 Probe 密度造成的平滑差异外，直接光、阴影区间接光、整体能量与 Color Bleeding 已接近。

这不意味着此前全部工作都错误。CPU Local Geometry / LTM 构建、GPU A/B recurrence、SH Triple Product、Directional Shadow Injection、receiver-side surface gather 与静态 Geometry dirty 管理均是有效基础。本轮问题集中在 transport 离散方式和 Godot 渲染管线的单位衔接。

## 2. 原文要求与实现判断

### 2.1 原文明确要求的部分

原文第 17–20 页描述全局 Radiance Transfer：

- 每个 Probe 访问周围 26 个 Probe。
- Directional Light 只在 `probe not in Shadow` 时注入。
- 邻居 Radiance 先与邻居 antipodal Local Visibility 做 SH Triple Product。
- Gather 后应用当前 Probe 的 Local Visibility。
- 最后乘 Local Transfer Matrix 得到 reflected Radiance。
- 传播通过 A/B recurrence 随时间收敛到长程结果。

附录第 28–29 页给出 Local Transfer Matrix 的一次局部反弹构造和 `4π / WeightSum` importance factor，并明确说明 Neumann `InfBoundT = (I - T)^-1 - I` “求也可以，不求也可以”，主要影响收敛速度。

### 2.2 不是“原文错误实现”的部分

以下内容是 Godot 集成必须补充的约定，原文没有给出可直接照抄的数值：

- Godot `DirectionalLight3D.light_energy` 与 Blender Sun Strength 的单位换算。
- Base Pass 在何处乘 surface albedo，以及 LRT 应提供 irradiance 还是 diffuse radiance。
- SH2 在表面法线方向上的 non-linear non-negative reconstruction。
- 非等距 26 邻居使用 equal weight 还是 inverse-distance cubature。

因此，本轮不能笼统描述为“之前很多事情都不符合论文”。更准确的判断是：原文的总体执行顺序已经实现，但若干原文未展开的离散和引擎单位细节选择错误，最终造成了明显的能量与空间分布偏差。

## 3. 问题一：邻域传播丢失方向

### 修复前

CPU 与 GPU 都执行了：

```text
visible = Trpd(neighbor_radiance, antipodal(neighbor_visibility))
incoming += visible * normalized_neighbor_weight
```

这在形式上接近原文伪码，但它把完整邻居 SH coefficient 直接平均。邻居位于 `+X`、`-X` 或对角方向这一事实没有进入重投影过程。每传播一个 Probe hop，方向能量都会被进一步摊平；结果是：

- 直接受光面附近积累过强。
- 阴影区和远距离区域明显缺能。
- 增加 iteration 只能收敛到错误 operator 的稳态。

### 修复后

每个邻居作为一个离散方向通道处理：

```text
visible = Trpd(neighbor_radiance, antipodal(neighbor_visibility))
direction = normalize(neighbor_offset)
scalar = max(EvaluateSH(visible, direction), 0)
incoming += ProjectSH(direction, scalar) * 4π * normalized_inverse_distance_weight
```

其中非负约束只作用于物理方向样本，不再对最终线性 SH coefficient 做任意截断。常量 Radiance 场经过 26-direction gather 后保持常量，已加入 CPU golden test。

### 修改位置

- `scene/3d/local_lrt_math.h`
- `servers/rendering/renderer_rd/shaders/environment/local_lrt_radiance.glsl`
- `local_lrt_volume_misc/test_project/gpu_radiance_validation.gd`
- `tests/scene/test_local_lrt_math.cpp`

## 4. 问题二：Directional Injection 多出 2 倍

共享方向编码器使用 `2π` 将方向样本编码为 SH。Godot Directional Energy 在 Lambertian 表面语义下对应 diffuse radiance；其等效 incident irradiance 是 `π × energy`。因此 Directional-only 注入共享编码器前必须使用：

```text
encoded_energy = energy * π / (2π) = energy * 1/2
```

修复只作用于 Directional Light。Omni / Area / Spot 属于 V0.9，未借本轮修改其能量模型。

### 修改位置

- `scene/3d/local_lrt_builder.cpp`
- `servers/rendering/renderer_rd/shaders/environment/local_lrt_injection.glsl`
- `local_lrt_volume_misc/test_project/gpu_analytic_injection_validation.gd`
- `local_lrt_volume_misc/test_project/gpu_directional_shadow_injection_validation.gd`

## 5. 问题三：Forward 缺少 `1/π`

`local_lrt_evaluate_diffuse()` 对 SH Radiance 执行 diffuse convolution，输出量是 irradiance。Godot Forward 的 `ambient_light` 稍后还会乘 surface albedo，因此这里需要先根据 Lambertian BRDF 转为 diffuse radiance：

```text
L_o = albedo * E / π
```

修复前直接返回 `E`，等价于最终间接漫反射多出 `π`。与 Directional Injection 的 2 倍误差叠加后，总能量口径恰好约多出 `2π`。

### 修改位置

- `servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl`

## 6. 问题四：Diffuse lobe 方向性上限错误

此前 non-linear L1 reconstruction 使用：

```text
directionality = |D| / (2A)
```

`2A` 是 delta directional light 的 L1 上限。LRT Radiance 已经过 diffuse transfer，其最大 `|D| / A` 为 `4/3`。继续使用 2 会低估方向性，让反向半球保留不应存在的能量，表现为几何接触区域附近的发白光环或过宽过渡。

修复为：

```text
directionality = |D| / ((4/3)A)
```

并加入 diffuse lobe 在反方向重建为零的单元测试。

### 修改位置

- `scene/3d/local_lrt_math.h`
- `servers/rendering/renderer_rd/shaders/scene_forward_gi_inc.glsl`
- `tests/scene/test_local_lrt_math.cpp`

## 7. 已排除的假设

### Equal-weight LTM

原文附录使用 `4π / WeightSum`。本项目 Probe 的轴向、边向和角向邻居物理距离不同，当前采用 normalized inverse-distance cubature。独立 A/B 中切换为 equal weight 没有修复阴影区缺能，整体误差反而略增，因此保留 inverse-distance 作为明确记录的 Godot 离散扩展。

### Unnormalized gather

取消归一化会让能量快速发散，不是原文长程传播的正确补偿方式。

### Neumann 无限局部反弹

加入 `InfBoundT` 对目标 ROI 几乎无影响。当前跨 Probe recurrence 本身已持续传播，问题不是“只有一次反弹”。这与原文附录所说 Neumann 可选、主要影响收敛速度一致。

### 距离衰减

Directional benchmark 使用 `decay_per_meter = 1`，不存在过强距离衰减。提高全局 GI multiplier 只会同时放大近场错误，不能修复旧实现中“近场过亮、阴影区过暗”的相反偏差。

## 8. Benchmark 配置

| 项目 | Godot | Blender Cycles |
|---|---:|---:|
| Directional energy | `1.5915494` | Sun Strength `5.0` |
| 能量换算 | `E_godot` | `π × E_godot` |
| Light color | `(1, 0.95, 0.86)` sRGB authored | `(1, 0.8900054, 0.7105665)` linear |
| Angular diameter | `0.5°` | `0.5°` |
| Camera FOV | `52°` | `52°` |
| Resolution | `512×512` | `512×512` |
| World | black, energy `0` | black Background, strength `0` |
| Color management | AgX, Exposure `0` | AgX, Exposure `0` |
| Materials | Lambertian, specular `0` | Diffuse BSDF |
| Other lights/emission | disabled | disabled |
| GI iterations/bounces | 16 Probe hops per frame, persistent | 32 diffuse bounces |
| Samples | deterministic realtime | 512, no denoise, no clamp |

Godot 与 Blender 的 UI 数值不应强行相等。`5.0 / π = 1.5915494` 才是当前两套渲染器直接光匹配的输入。

## 9. 数值结果

### Direct 基线

| ROI | Godot RGB | Cycles RGB | 结果 |
|---|---|---|---|
| Floor | `(0.56630, 0.50032, 0.40311)` | `(0.56417, 0.50212, 0.40088)` | 各通道误差 `< 0.7%` |
| Short Box front | `(0.29992, 0.26574, 0.21405)` | `(0.29931, 0.26639, 0.21268)` | 各通道误差 `< 0.7%` |

### GI-only 修复后

| ROI | Godot LRT RGB | Cycles RGB | 结果 |
|---|---|---|---|
| Floor | `(0.017244, 0.016049, 0.007045)` | `(0.01460, 0.02242, 0.00918)` | 均值 `0.01345` vs `0.01540` |
| Tall Box shadow | `(0.054642, 0.054642, 0.038765)` | `(0.05160, 0.05058, 0.03616)` | 约 `6–8%` |

修复前同一类对照呈现受光地面约 5 倍偏亮、Tall Box 阴影面约 10 倍偏暗的相反误差；修复后该空间分布错误消失。

## 10. 自动验证

- Incremental editor build：PASS。
- `[LocalLRTMath]`、`[LocalLRTBuilder]`、`[LocalLRTVolume3D]`：47 cases / 1158 assertions PASS。
- GPU Radiance：1/2/4/8 iterations 与 persistent 2-step，81 RGB SH2 values PASS。
- GPU Analytic Injection：6 cases / 27 probes PASS。
- GPU Directional Shadow Injection：2 cases / 125 probes PASS。
- Forward Surface Validation：Forward+ Vulkan PASS。

## 11. 当前仍属预期的差异

- Cycles 512 samples 仍有 Monte Carlo 噪点，LRT 是 deterministic Probe transport。
- SH2 只能表达低频方向信息；高盒表面仍可能出现轻微低阶条带。
- `probe_spacing = 0.25 m` 限制几何边缘和小尺度间接阴影的空间频率。
- Cycles 的高阶路径与连续几何积分不会与有限 Probe/SH2 完全逐像素一致。

这些差异不应再表现为一格空隙、接触光环、受光区能量爆炸或阴影区系统性缺能。

## 12. 资产索引

完整场景、线性 EXR pass、AgX LookDev 图和六宫格对照见：

`local_lrt_volume_misc/benchmarks/directional_cornell_v08/README.md`
