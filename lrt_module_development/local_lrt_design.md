# Local LRT GI 方案

**为列车和大世界提供低频、可随灯光与物体移动变化的间接漫反射。**静态场景按 Volume 烘焙；动态实例挂载自己的 Color SDF；Probe 利用这些几何缓存传播光照。

车厢 Probe spacing 约 **0.25 m**，世界约 **0.5–1 m**。每个 Volume 使用规则网格，缓存和传播统一在 Volume 局部空间。静态指“相对 Volume 固定”：列车整体移动时保留车厢缓存，家具相对车厢移动时更新局部缓存。

几何与材质保持固定，运行时处理实例整体变换、灯光和天空变化。Volume 与实例支持平移、旋转和正均匀缩放；粗网格着重表现较大的几何关系。

# 模块与数据

所有实现集中在 **modules/lrt/**，分为几何烘焙、缓存与调度、GPU 传播、场景与编辑器、Godot 适配五部分。现有引擎功能优先复用；渲染接入和私有接口依赖集中在适配层，必要时开放最小接口。模块与接入补丁分开维护，便于随 master rebase，并保持 Godot 原有功能。

| 对象 | 保存或负责什么 |
|---|---|
| LocalLRTVolume3D | 范围、Probe spacing、优先级、强度、边缘混合和烘焙资源 |
| LocalLRTVolumeData | 当前 Volume 的静态 LTM、LocalVis、Inside、Edge 和可选固定自发光 J |
| LRTInstanceGeometryData | 单个实例的对象空间 Color SDF，包含其实际材质颜色和可选固定自发光 |
| LRT 实例绑定配置 | 由 LRT 模块持有并随场景保存，将 Geometry Instance 与其烘焙资源绑定；通过 Inspector 注册展示，不使用独立 Binding 子节点 |
| Trunk | 一块 8³ Probe、影响它的实例列表及局部几何缓存 |
| 传播状态 | 反弹光 L 和全局可见性 A，各用双缓冲 |
| Screen Gather 缓存 | 每个视图的预乘间接光 RGB 和覆盖权重 coverage |

**Color SDF 按实际几何与材质配置绑定，而不是只按 Mesh 绑定。**同一 Mesh 的不同材质实例分别烘焙；几何与材质求值结果一致的 MultiMesh 子实例可共享同一份对象空间 Color SDF。实例颜色、自定义数据或实例索引等导致烘焙结果不同时，需分别保存对应数据，不能忽略这些差异。同一实例进入不同 spacing 的 Volume 时，使用自己的同一份数据。编辑器中修改几何、材质或 Volume 布局后，重烘焙相应缓存。

# 几何缓存怎么生成

每个 Probe 向周围取 26 个采样位置：qᵢ = p + sδᵢ，δᵢ 为相邻网格的整数偏移，s 为 spacing。每个方向得到占用 oᵢ 和颜色 aᵢ，再构建：

- **LTM（T）**：局部物体怎样反弹入射光。

- **LocalVis（V）**：局部各方向的通透程度。

- **Inside**：Probe 中心是否位于实体内。

空气取 T=0、V=SH_ONE。实体内部的 Probe 取 T=V=0，传播状态清零；最终表面采样使用其余可用 Probe。

## 静态物体

按当前 Volume 的网格和 spacing，用三角形 BVH 查询 p→qᵢ 的有限线段。命中时记录 oᵢ=1 及命中表面的实例材质颜色，通路记录 oᵢ=0。同时烘焙 Inside 和静态 Edge，结果保存到 Volume。

## 动态实例

在实例局部空间烘焙 Color SDF：距离外正内负，颜色由最近表面的有效材质取得并延拓到体积内。数据覆盖模型包围盒及约两格边界，封闭几何提供内外符号。

运行时将 qᵢ 转到实例空间。落在 SDF 内且距离≤0 时取 oᵢ=1，并读取颜色；其余位置取 oᵢ=0。因此，**Probe 中心可以在 SDF 范围外，只要邻域采样点进入物体，就能取得贡献。**动态反弹细节随 Probe 间距变化。

静态与动态使用同一套构建公式。合成时从静态底板开始，累加各实例的 T，并按稳定顺序将 V 做 SH 乘积；合成完成后压缩保存。

# 动态更新与跨墙传播

实例移动、加入或移除时，取旧、新包围盒的并集，每轴外扩 s 加 Edge 采样半径，找到受影响的 Trunk。

CPU worker 根据当前实例列表，从静态底板重建这些块的几何缓存。相邻块共享的 Edge 同步更新，结果作为完整批次交给 GPU，相关传播历史随新缓存重置。更新任务按预算分帧执行。

**Edge Visibility 表示两个相邻 Probe 之间的通透率。**每条边使用四条固定的平行子线段，采样半径取 0.05s。静态部分通过 BVH 烘焙，动态部分使用 Color SDF 做线段查询；线段先裁剪到 SDF 包围盒，再在域内保守步进。

每条子线段以 1 表示通路、0 表示受挡。静态与各动态实例的四位 mask 逐位 AND，通路数量除以 4 得到边权 e。一条边由两端共用，传播时乘入该边权。这样墙体遮挡由连线检测决定，墙边的变化由多条子线段平均得到。 Volume 边界处通向出界邻居的连线同样计算 Edge，用于控制外层光照的传入。

# 光照如何进入画面

当前帧的深度、法线和阴影就绪后，先接收几何更新，再按 **外层 → 内层**依次完成各 Volume 的 **传播 A → 注入灯光和天空 → 传播 L**，最后执行 **Screen Gather → 材质融合**。更新顺序沿用 priority，从低到高；Probe 状态按世界每帧更新，Screen Gather 按视图生成。

## 光照获取与注入

适配层按活动 Volume 范围收集 Directional、Omni、Spot 光源，对 Bake Mode 为 Static/Dynamic 的灯光统一进行运行时注入。读取位置、方向、颜色、强度、距离与锥角衰减、阴影，并将 light_indirect_energy 乘入注入强度。颜色和单位统一为线性、未曝光数据。

在 Probe 处计算每盏灯到达这里的光强 Eⱼ、阴影可见性 σⱼ 和传播方向 tⱼ，累加为：

```math
Ddirect_p=\sum_j E_j(p)\,\sigma_j(p)\,Y(t_j)
```

tⱼ 使用 Volume 局部方向；局部表面的朝向响应由 T 承担。随后以 **S=T(Ddirect+B)+J** 产生反弹光源，继续按既有公式传播。

阴影优先复用有效影图。方向光在覆盖外取 σ=clamp(dot(A,Y(u)),0,1)，u 为朝太阳的方向，用当前 LRT 几何范围近似远方通透率。点光、聚光的带阴影项在影图有效时注入；缓存随光源、采样位置或遮挡改变而失效。灯光关闭阴影时，σ=1。

Environment 的 ambient source、颜色、能量、sky contribution 和天空旋转共同生成一份入射环境辐亮度 Sky SH，在环境变化时更新。A 决定各 Probe 可见的天空 B；实例烘焙的固定自发光通过 J 注入。

## Screen Gather

Screen Gather 将 Probe 中的光照汇集成屏幕缓存，减少材质逐像素查询 Volume 的开销。默认使用**半宽、半高，即四分之一像素数**：

1. 适配层请求 Forward+ 深度/法线预通过。每个低分辨率代表样本的位置、法线取自同一真实表面像素，并转到相应 Volume 空间。

2. 查询覆盖该位置的各 Volume，分别插值八个有效 Probe 的 L/A，求可见天空 B 和间接漫反射，再按优先级与边缘权重合成为 I_LRT 及 coverage。

3. 输出 HDR RGB=coverage×I_LRT、Alpha=coverage；I_LRT 包含反弹光与可见天空，coverage 表示 LRT 对该表面的覆盖权重。Godot 适配层在写入每视图 FP16 缓存前应用一次相机曝光编码，材质读取时不再次曝光；传播状态、源项、Sky SH 和边界输入仍保持未曝光。天空投影读取独立的未曝光 Sky 材质输出，避免原天空缓存的曝光及 FP16 范围影响 GI。

4. 用全分辨率深度和法线引导升采样，同时过滤预乘颜色与 coverage，使同一表面和 Volume 边缘平滑衔接。

材质在间接漫反射位置融合：ambient=(1−coverage)×ambient_original+gather_rgb，再由 Godot 处理表面颜色和 AO。透明表面及缺少匹配屏幕样本的像素，直接按自身位置与法线采样 Probe。

## Volume 嵌套与混合

**外层提供边界光照，内层计算局部细节。**各 Volume 保存自己的几何缓存和 L/A；世界网格表达整体关系，车厢网格表达内部细节，光照按外层 → 内层单向传入。

内层传播时，范围内的邻居读取自身 Probe；出界邻居按世界位置采样外层已完成的 L/A，并将 SH 方向转换到内层空间。来源从覆盖该位置、已就绪且 priority 更低的 Volume 中选择最高优先级者；同级候选按稳定顺序择一。

边界读取原始传播场，Inside 的零状态也属于有效输入。L 带来外界反弹光，A 带来天空遮挡；输入继续经过内层的 Edge Visibility、LocalVis 和 LTM。灯光与天空仍按各 Volume 自身的数据注入，intensity 与 coverage 在最终显示时应用。

表面采样对有效 Probe 权重归一化；权重总和为零时，该 Volume 的 coverage 取 0。Screen Gather 按优先级与边缘权重混合结果：内层满权重区域使用内层结果，边缘逐渐过渡到外层。以世界 Volume 完全覆盖车厢为例：

```math
I=(1-w)I_{\mathrm{世界}}+wI_{\mathrm{车厢}}
```

w 从车厢 Volume 边界的 0 增加到内部的 1。将车厢输入边界布置在车壳外侧，让车内需要精细表现的表面位于满权重区。整体 coverage 控制与 Godot 原有间接光的衔接；大世界通过外部流送系统加载和卸载 Volume。

Godot 适配层集中处理输入、调度、Screen Gather 和材质融合；模块关闭时使用原有渲染路径。

# 参数

**Volume 管空间与混合，实例沿用 GI Mode 选择参与方式，Project Settings 管全局质量与预算。**灯光与环境的艺术参数沿用场景中的 Light 和 Environment。

## Volume

| 参数 | 作用与默认值 |
|---|---|
| enabled | 参与计算与画面混合；默认开启 |
| size | 以节点原点为中心的 Volume 局部尺寸；与 VoxelGI 一样通过 Inspector 和六面手柄调整，位置与旋转使用节点变换 |
| probe_spacing | 默认 0.5 m；车厢约 0.25 m，世界约 0.5–1 m |
| priority | 画面混合优先级及边界输入顺序；默认 0，车厢可设为 10 |
| blend_width_cells | 边缘混合宽度，默认 2 格 Probe |
| intensity | 最终 LRT 间接光强度，默认 1；在 Volume 混合前作用于输出 |
| bake_data | 当前 Volume 的静态缓存，由烘焙生成 |

## Geometry Instance

Geometry Instance 的几何贡献支持 **MeshInstance3D 和 MultiMeshInstance3D**，不能仅以 MeshInstance3D 实现代替完整支持。直接读取实例自带的 **Global Illumination → Mode（gi_mode）**，沿用实例当前设置；Volume 按空间范围收集实例。

MultiMesh 静态烘焙逐个展开当前可见数量范围内的子实例，使用节点变换与子实例局部变换的组合；动态运行时按子实例计算影响范围、采样 Color SDF，并响应节点或子实例变换、实例数量、可见数量及资源变化。每个有效组合变换均遵循正均匀缩放、无剪切的限制。

| GI Mode | 在 LRT 中的作用 |
|---|---|
| Static | 将几何与有效材质烘焙到 Volume 的静态缓存 |
| Dynamic | 使用实例自己的 Color SDF 合成局部缓存，随整体变换更新 |
| Disabled（No） | 仅接收 LRT 光照 |

烘焙使用实例 Mesh 与实际生效的材质，复用现有材质覆盖设置。不透明 StandardMaterial3D/ORMMaterial3D 和 ShaderMaterial 通过原生 Forward+ 材质 GPU 管线求值，不维护 CPU 材质功能白名单；保留 UV/UV2、自定义属性、原始 VERTEX_ID、实例/全局参数及不透明额外 pass/overlay。读取线性漫反射颜色、未曝光自发光和顶点输出，构建固定快照。材质图集每个几何采样格使用四个 texel，使用独立的逐三角形坐标而不覆盖 UV2；超过图集上限显式报错。透明、裁切、折射和屏幕读取 pass 暂时仅接收 GI，不贡献缓存或遮挡；全透明实例的烘焙资源标记为仅接收，运行时不调度其几何。时间、视角依赖和顶点 shader 的输出冻结在烘焙时，仍沿用“几何与材质固定”的前提。

### Inspector 集成

在 Geometry Instance 的 **Global Illumination → LRT** 折叠子菜单中展示实例绑定配置，将现有 Binding 的参数、烘焙入口和配置警告集中到这里。MeshInstance3D 与 MultiMeshInstance3D 使用相同入口；不再要求添加 LRTGeometryBinding3D 子节点。

由 **LRT 编辑器模块注册 EditorInspectorPlugin**，为支持的 Geometry Instance 注入 LRT 子菜单与属性编辑器，不直接向 MeshInstance3D、MultiMeshInstance3D 或 GeometryInstance3D 的核心属性列表添加 LRT 专用属性。绑定配置使用模块内的 LRTGeometryBinding Resource，保存在节点的 `_lrt_binding` metadata 中，并启用 resource_local_to_scene；Inspector 编辑通过撤销/重做替换该节点的配置资源，避免复制节点后修改配置影响原节点。存储与运行时读取由 LRT 模块负责，不能仅保存在 Inspector 控件中；配置必须随场景保存，并支持重新加载、节点复制、场景实例化和编辑器撤销/重做。烘焙、资源变更监听、运行时注册与调度均留在 LRT 模块，脱离编辑器运行时也能读取绑定，不依赖 Inspector 插件。

LRT 子菜单包含以下参数：

| 参数 | 作用与默认值 |
|---|---|
| sdf_resolution | 动态 Color SDF 最长轴格数，默认 32，其余轴按比例确定 |
| bake_data | 该 Geometry Instance 的 Color SDF 烘焙资源；MultiMesh 保存共享或按子实例区分的数据绑定，由烘焙生成 |

子菜单同时提供 **Bake** 操作；动态模式缺少烘焙数据、变换不受支持等问题在此明确提示。GI Mode 继续使用原有 Mode，不在 LRT 子菜单重复设置。调试选择直接使用 Geometry Instance，不再依赖 Binding 子节点。

## Project Settings

统一位于 rendering/lrt/：

| 参数 | 作用与默认值 |
|---|---|
| enabled | 全局开关，默认关闭 |
| propagation_iterations | A、L 各自每帧迭代次数，默认 2 |
| propagation_relaxation | 新结果融入传播状态的比例，默认 0.5 |
| geometry_trunks_per_frame | 全部活动 Volume 每帧提交重建的 Trunk 数上限，默认 8 |
| screen_gather_enabled | 使用屏幕缓存完成表面采样，默认开启；关闭时直接采样 Probe |
| screen_gather_scale | Gather 宽高比例，可选 1、0.5、0.25，默认 0.5 |

# Debug View

调试视图接入 3D 视口现有的 **Display Advanced...** 菜单，以 **LRT** 为前缀，与 VoxelGI、SDFGI Cascades 等视图并列。菜单入口由编辑器适配层接入，调试绘制留在 LRT 模块。

各视图可按选中的 Volume、实例或 Probe 查看，并支持空间切片。默认使用正常渲染，调试模式随当前视口切换。

| 视图 | 显示内容 |
|---|---|
| Volume / Probe | 范围、网格、Inside、有效 Probe、重叠 coverage，以及边界输入的来源 Volume 和方向 |
| Color SDF / 几何采样 | SDF 距离与颜色、零等值面，以及选中 Probe 的 26 个采样点和命中结果 |
| LTM | 单位白光从选定方向入射时，局部反弹的方向与颜色 |
| LocalVis | 用方向小球显示局部通透率，白色通透、黑色遮挡 |
| GlobalVis | 传播后的环境可见性 A，与 LocalVis 对照 |
| 光照 | 切换 Ddirect、可见天空 B、一跳源 S、传播光 L 和最终间接光；显示阴影数据有效范围 |
| Edge | 相邻 Probe 的通透率；选中边可查看四条子线段 |
| Trunk | 待更新、计算中、已发布状态，以及移动实例的旧、新影响范围 |
| Screen Gather | 输入深度/法线、低分辨率 GI、升采样 GI、coverage，及直接 Probe 采样对照 |

调试视口叠加显示 Probe 数、待更新 Trunk 数、内存与 CPU/GPU 耗时。

# 关键计算

## SH 约定

使用四系数 SH，矩阵按列向量计算。所有光照使用线性、未曝光颜色。L 和直接光 Ddirect 按光线传播方向编码；A、Sky、LocalVis 按看向来源的方向编码。Flip 转换这两种方向。

```text
c0 = 1 / sqrt(4π)
c1 = sqrt(3 / (4π))
Y(w) = [c0, c1*w.x, c1*w.y, c1*w.z]
D(n) = [c0, (2/3)*c1*n.x, (2/3)*c1*n.y, (2/3)*c1*n.z]
SH_ONE = [1/c0, 0, 0, 0]
Flip(a) = [a0, -a1, -a2, -a3]

Trpd(a,b)₀ = c0 * dot(a,b)
Trpd(a,b)ₖ = c0 * (a0*bₖ + b0*aₖ), k=1..3
```

D 是归一化 Lambert 核；Trpd 表示球面函数相乘后投影回 SH4。

## 构建与合成

令 ωᵢ=normalize(δᵢ)，oᵢ 为占用，aᵢ 为线性反射率，c 表示 RGB 通道：

```math
T_c=\frac{4\pi}{26}\sum_i o_i a_{i,c}D(-\omega_i)Y(\omega_i)^{\mathsf T},
\qquad
V=SH_{\mathrm{ONE}}-\frac{4\pi}{26}\sum_i o_iY(\omega_i)
```

归一化使用全部 26 个方向。可选固定自发光作为独立源项：

```math
J_c=\frac{4\pi}{26}\sum_i o_iL_{e,i,c}Y(-\omega_i)
```

从静态缓存开始，对当前动态实例依次合成：

```text
T ← T + T_instance
V ← Trpd(V, V_instance)
J ← J + J_instance
```

## 传播与最终采样

对邻居 qᵢ=p+sδᵢ，令 dᵢ=normalize(δᵢ)。方向权重及邻域聚合为：

```math
K_i=\left(\frac{1}{26c_0},-\frac{d_{i,x}}{26c_1},
-\frac{d_{i,y}}{26c_1},-\frac{d_{i,z}}{26c_1}\right)^{\mathsf T}
```

```math
H_p=\sum_i e_{pq_i}\operatorname{Trpd}(K_i,L^{old}_{q_i}),
\qquad
U_p=\sum_i e_{pq_i}\operatorname{Trpd}(\operatorname{Flip}(K_i),A^{old}_{q_i})
```

K 使用固定的 26 方向归一化，Edge 通过减少贡献表达遮挡。全局可见性、可见天空和反弹光按以下顺序计算：

```math
\begin{aligned}
A_p^{raw}&=\operatorname{Trpd}(V_p,U_p)\\
B_p&=\operatorname{Flip}\bigl(\operatorname{Trpd}(Sky,A_p)\bigr)\\
S_p&=T_p(Ddirect_p+B_p)+J_p\\
L_p^{raw}&=S_p+\operatorname{Trpd}(V_p,H_p)+T_pH_p
\end{aligned}
```

Ddirect 是考虑阴影与衰减后的直接入射光；B 是可见天空；S 是它们产生的一跳反弹与自发光。L 更新式中的后两项分别表示空气直通和后续反弹。

A 初始取 V，L 初始取 0。邻域式中的出界 L/A 使用外层已完成的传播状态，并转换到当前 Volume 空间；该位置没有就绪的外层供给时，开放边界取 L=0、A=SH_ONE。A/L 使用双缓冲和松弛更新 Xnew=mix(Xold,Xraw,0.5)，各取每帧两次迭代作为默认值。

表面插值得到 L、A 后，用 A 求 B，再计算：

```math
I_{\mathrm{LRT}}(x,n)=
\max\left(0,\left\langle L_{\mathrm{interp}}+B_{\mathrm{interp}},D(-n)\right\rangle\right)
```

结果为 irradiance/π，交给 Godot 乘接收表面的材质。Volume coverage 控制它与现有间接漫反射的混合。

## 存储与压缩

LTM 合成后压成 **16 个 FP16 矩阵系数＋RGB8 整体颜色＋1 byte flags，共 36 B/Probe**。这种有损表示保留方向亮度和整体颜色。LocalVis 用 4×FP16；L/A 使用 FP32 双缓冲。

编解码使用以下关系，其中 w=[0.2126,0.7152,0.0722]：

```text
a = [max(TR[0,0],0), max(TG[0,0],0), max(TB[0,0],0)]
m = max(a)
m=0 时保存零 T；其余情况：
C = a / m
M = (w.r*TR + w.g*TG + w.b*TB) / dot(w,C)
保存 FP16(M) 与 UNORM8(C)；解码 Tc ≈ C[c] * M
```

Color SDF 保存 SNORM16 距离及其缩放值、RGBA8 颜色，可选固定自发光另存。烘焙资源关联各自的网格、几何和材质配置。
