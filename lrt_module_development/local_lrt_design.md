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
| LRT 实例绑定配置 | 由 LRT 模块持有并随场景保存，绑定几何烘焙资源和接收光照时的逐物体 normal bias；通过 Inspector 注册展示，不使用独立 Binding 子节点 |
| Trunk | 一块 8³ Probe、影响它的实例列表及局部几何缓存 |
| 传播状态 | 反弹光 L 和全局可见性 A，各用双缓冲 |
| Screen Gather 缓存 | 每个视图的预乘间接光 RGB、覆盖权重 coverage、位置/法线 guide，以及预通过写入的 R16F 表面 bias |

**Color SDF 按实际几何与材质配置绑定，而不是只按 Mesh 绑定。**同一 Mesh 的不同材质实例分别烘焙；几何与材质求值结果一致的 MultiMesh 子实例可共享同一份对象空间 Color SDF。实例颜色、自定义数据或实例索引等导致烘焙结果不同时，需分别保存对应数据，不能忽略这些差异。同一实例进入不同 spacing 的 Volume 时，使用自己的同一份数据。编辑器中修改几何、材质或 Volume 布局后，重烘焙相应缓存。

# 几何缓存怎么生成

每个 Probe 向周围取 26 个采样位置：qᵢ = p + sδᵢ，δᵢ 为相邻网格的整数偏移，s 为 spacing。每个方向得到占用 oᵢ 和颜色 aᵢ，再构建：

- **LTM（T）**：局部物体怎样反弹入射光。

- **LocalVis（V）**：局部各方向的通透程度。

- **Inside**：Probe 中心是否位于实体内。

空气取 T=0、V=SH_ONE。实体内部的 Probe 取 T=V=0，传播状态清零；最终表面采样使用其余可用 Probe。

## 静态物体

按当前 Volume 的网格和 spacing，用三角形 BVH 查询 p→qᵢ 的有限线段。命中时取 oᵢ=1，并读取有效材质颜色、自发光和 Volume 空间的三角形几何法线 nᵢ；通路取 oᵢ=0。

几何法线由材质烘焙得到的实际三角形顶点计算，采用 Godot 顺时针正面约定，在 double 精度下归一化后转为 Vector3。入射余弦响应由 nᵢ 决定，命中几何在 Probe 周围的角分布决定出射辐亮度；不把采样连线当作表面法线，也不包含平滑顶点法线或法线贴图。

只有朝向 Probe 的正面命中贡献反射矩阵；背向命中仍按现有规则贡献遮挡和固定自发光。法线仅在 CPU 烘焙期间临时保存，不增加 Probe 或资源存储字段。同时烘焙 Inside 和静态 Edge，将 LTM、LocalVis、Edge 和可选自发光保存到 Volume。

## 动态实例

在实例局部空间烘焙 Color SDF：距离外正内负，颜色由最近表面的有效材质取得并延拓到体积内。数据覆盖模型包围盒及约两格边界。静态 Inside 与动态距离符号统一按广义绕数绝对值大于 0.5 判定；不要求整个 Mesh 封闭或流形，局部开口不再使整个实例失去内部判定资格。封闭流形使用等价的 BVH 射线绕数加速。开放模型的内部按这一绕数定义，单层薄片不形成实体内部；整体反向绕序不影响判定，局部绕序不一致仍会影响结果。

运行时将 qᵢ 转到实例空间。落在 SDF 内且距离≤0 时取 oᵢ=1，并读取颜色；其余位置取 oᵢ=0。因此，**Probe 中心可以在 SDF 范围外，只要邻域采样点进入物体，就能取得贡献。**动态反弹细节随 Probe 间距变化。

动态 Color SDF 不保存表面法线，反射矩阵沿用邻域方向近似；静态烘焙使用实际命中法线，两者的 T 构建式分别见下文。它们共用 LocalVis、Inside、Edge、存储格式及运行时传输规则。合成时从静态底板开始，累加各实例的 T，并按稳定顺序将 V 做 SH 乘积；每次可见性乘积的输入和输出都应用下文的 Visibility 约束，合成完成后压缩保存。

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

tⱼ 使用 Volume 局部方向；局部表面的朝向响应由 T 承担。灯光累加后先应用 Light 约束，再与可见天空 B 一起求 **Reflect(T,Light(Ddirect)+B)**，其中 **Reflect(T,I)=Diffuse(T×Light(I))**；加入 Diffuse(J) 得到一跳源 S。不再计算一个最终不用的直通项来与反射共享 DC 预算。S 在传播时与直通及后续反射合并后应用 Diffuse，不额外做方向 1/3 滤波；该系数只用于后续反射输出的矩匹配。

方向光不读取观察相机的 CSM。每个世界维护一张所有 Volume 共享的 D32F 太阳阴影 Atlas，跟随该世界每帧首个非反射探针视图的位置，接收范围是半径 `sun_shadow_extent` 的球体，与视角朝向、FOV 和 Volume 尺寸无关。光空间中心按 texel 对齐；每个 LRT 更新帧绘制一次，同世界后续视图复用。多盏参与 GI 的方向光在同一张 Atlas 内分块，纹理总分辨率不变，单灯精度相应降低。

额外阴影任务并入原生场景裁剪和 Forward+ Shadow Pass，复用投影开关、灯光 caster mask、显隐依赖、骨骼 Mesh 更新及阴影材质/Alpha 裁切，不使用观察视锥或其层掩码剔除遮挡物。距离显隐仍以跟随视图位置为参照；阴影绘制禁用网格 LOD，主相机 Shader 参照使用稳定的跟随位置。遮挡物可以在接收球外、朝太阳的任意距离，裁剪后按实际遮挡物包围盒拟合深度范围，不用固定距离截断上游遮挡物。未加载几何仍需由大世界流送负责。

带阴影方向光只在接收球内注入太阳反弹源，球外取 σ=0，不回退到 CSM 或低阶 A。天空、自发光、点光/聚光及正常传播不受这个覆盖范围限制；球外局部太阳反弹缺失，以及跟随范围移动后的传播收敛属于该有限覆盖方案的限制。点光、聚光继续复用原生有效影图。灯光关闭阴影时仍取 σ=1。阴影资源缺失显式阻断 LRT，不用其他遮挡来源静默替代。

阴影 Bias 必须按原生渲染路径所属的空间施加，不能统一加到透视除法后的深度：

- **Directional**：沿用原生向光源偏移的距离 Bias 语义，取 `shadow_bias × 0.01 × 2 × sun_shadow_extent`，不随远处遮挡物改变拟合深度而改变世界空间 Bias，先以 world_biased=world−t_world×bias 向光源偏移，再乘 LRT 专用阴影矩阵并执行透视除法。
- **Spot**：先投影到齐次阴影坐标，将 Bias 加到 projected.z，再除以 projected.w。
- **Omni**：Bias 是径向距离，在距离归一化前扣除，比较深度为 1−(distance−bias)/range。

以上沿用灯光的 Bias 参数及现有适配层换算；Probe 没有接收表面法线，不额外模拟表面 Normal Bias。

Environment 的 ambient source、颜色、能量、sky contribution 和天空旋转共同生成一份入射环境辐亮度 Sky SH，在环境变化时更新，投影结果应用 Light 约束。可见天空统一取 B=Flip(Light(Trpd(Light(Sky),Visibility(A))))；实例烘焙的固定自发光 J 在注入时应用 Diffuse，独立于传输预算。

## 表面采样、Bias 与有效性

**表面阶段只做法线偏移和八 Probe 插值，不查询 Edge，也不做半球剔除或余弦重加权。**Edge Visibility 保留在 Probe 间传播和 Volume 边界输入阶段，四射线平均规则不变；不使用最近参考 Probe 或 28 对 Edge 混合给表面额外遮光。

接收物体通过 `LRTGeometryBinding.surface_normal_bias` 配置偏移。默认 `-1` 继承 `rendering/global_illumination/lrt/surface_normal_bias`（默认 **0.1 m**）；`0` 禁用，正值覆盖全局设置。偏移使用世界单位米，不随 Probe spacing、Volume 或物体缩放变化。MeshInstance3D 与 MultiMeshInstance3D 使用同一入口；MultiMesh 的全部子实例共享节点配置，同一物体的所有表面共享该值，Static、Dynamic 和 Disabled GI Mode 均可配置，无需重烘焙。

对真实表面位置 x 和单位世界法线 n，使用 `x_sample = x + bias × n` 选择网格单元、计算三线性权重并插值 L/A。**Volume 是否覆盖该表面，以及边缘混合权重，仍由未偏移的 x 决定**；最终 Lambert 求值仍使用真实表面法线。

令八个候选的三线性权重为 t_i，非 Inside 时 valid_i=1，Inside 时为 0：

```text
w_i = t_i × valid_i
W = Σ w_i
W > 0：L = Σ(w_i × L_i) / W，A = Σ(w_i × A_i) / W
W = 0：跳过当前 Volume
W > 0：当前 Volume 的混合权重 = 边界混合权重
```

L/A 始终使用同一组有效权重；Inside 的零光照不混入归一化结果。`W=0` 时跳过当前 Volume，保留已合成的低优先级 Volume，并通过整体 coverage 与 Godot 原有环境间接光融合。`W>0` 时正常归一化，不再按 W 衰减 coverage，有效的黑色 Probe 仍保持黑色。采样支撑不足不代表当前空间应露出外层或天空，因此不使用有效样本软退出。

运行时将节点的 bias 覆盖值按 ObjectID 同步到渲染线程，通过渲染几何实例关联到接收物体；最终值写入 Forward+ 已有 InstanceData 的压缩 AABB position `.w` 预留槽，不增加实例 GPU 记录大小。Gather、材质直接采样和 Probe Sampling 调试视图共用 `lrt_sample` 和相同的物体配置，关闭 Gather 不改变 bias 语义。

**限制：**bias 是采样位置修正，不是表面到 Probe 的可见性检测，不能保证完全遮住薄壁另一侧的亮 Probe。过大的偏移可能跨越狭窄结构或丢失接触暗部；应按接收物体配置，不把全局值无限增大。没有有效 Probe 的窄缝不能生成局部 GI；W 从正值归零时可能出现当前 Volume 与外层或环境光之间的硬切换。必要时调整网格位置或使用更密的现有局部 Volume。

## Screen Gather

Screen Gather 将 Probe 中的光照汇集成屏幕缓存，减少材质逐像素查询 Volume 的开销。默认使用**半宽、半高，即四分之一像素数**：

1. 适配层请求 Forward+ 深度/法线预通过，并在附加的 **R16F** 通道写入每个像素所属接收物体的实际 bias。位置、法线和 bias 使用同一真实表面像素；MSAA resolve 的 bias 与深度、法线取自同一个被选中的样本，不平均混合不同物体的 bias。仅需要 Gather 或其调试视图时才分配和写入该附件，关闭时沿用原深度路径。

2. 每个低分辨率代表样本读取位置、法线和 bias，按真实位置查询覆盖它的 Volume，再按上节偏移、Inside 剔除和有效点归一化规则插值八个 Probe 的 L/A，求可见天空 B 和间接漫反射，按优先级合成为 I_LRT 及 coverage。

3. 输出 HDR RGB=coverage×I_LRT、Alpha=coverage；I_LRT 包含反弹光与可见天空，coverage 表示 LRT 对该表面的覆盖权重。Godot 适配层在写入每视图 FP16 缓存前应用一次相机曝光编码，材质读取时不再次曝光；传播状态、源项、Sky SH 和边界输入仍保持未曝光。天空投影读取独立的未曝光 Sky 材质输出，避免原天空缓存的曝光及 FP16 范围影响 GI。

4. 用全分辨率深度和法线引导升采样，同时过滤预乘颜色与 coverage；排除 bias 与目标像素不同的代表样本，避免相邻物体的覆盖配置互相混合。没有匹配代表样本时，用目标像素自身的位置、法线和 bias 直接采样。

材质在间接漫反射位置融合：ambient=(1−coverage)×ambient_original+gather_rgb，再由 Godot 处理表面颜色和 AO。透明表面及缺少匹配屏幕样本的像素，按自身位置、法线和实例 bias 直接采样 Probe。

## Volume 嵌套与混合

**外层提供边界光照，内层计算局部细节。**各 Volume 保存自己的几何缓存和 L/A；世界网格表达整体关系，车厢网格表达内部细节，光照按外层 → 内层单向传入。

内层传播时，范围内的邻居读取自身 Probe；出界邻居按世界位置采样外层已完成的 L/A，并将 SH 方向转换到内层空间。来源从覆盖该位置、已就绪且 priority 更低的 Volume 中选择最高优先级者；同级候选按稳定顺序择一。

边界读取原始传播场，Inside 的零状态也属于有效输入。L 带来外界反弹光，A 带来天空遮挡；输入继续经过内层的 Edge Visibility、LocalVis 和 LTM。灯光与天空仍按各 Volume 自身的数据注入，intensity 与 coverage 在最终显示时应用。

表面混合按优先级从低到高进行，有有效样本的 Volume 按边界权重混合到已有结果；无有效样本时保留外层结果。以世界 Volume 在该位置满权重且有有效样本为例：

```math
I=(1-w)I_{\mathrm{世界}}+wI_{\mathrm{车厢}}
```

w 是车厢 Volume 的边界混合权重，从边界的 0 增加到内部的 1；无有效样本时该 Volume 不参与混合。将车厢输入边界布置在车壳外侧，让车内需要精细表现的表面位于满权重区。整体 coverage 控制与 Godot 原有间接光的衔接；大世界通过外部流送系统加载和卸载 Volume。

### 粗外层与精细内层

世界外层优先使用 **0.5 m** Probe spacing，车厢内层约 **0.25 m**。外层范围应覆盖地面等重要供光几何，优先级低于内层；内层输入边界布置在车壳外侧。各层按自己的网格和 spacing 烘焙静态缓存，不能直接降采样其他布局的 T/Edge。

外层通过真实几何注入和传播提供边界光照，不使用参考渲染结果补光。缺少外层时，范围外的反射输入为零；不能用提高内层 intensity 或引入传播增益代替缺失的供光几何。

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

### 材质烘焙与 Forward+ 接入

`modules/lrt/lrt_material_baker.cpp` 负责材质分类、独立三角形图集、GPU 求值及颜色/自发光/顶点输出回读，继续复用原生 `PASS_MODE_DEPTH_MATERIAL`，不另写近似材质求值器。

Forward+ 的接入改动集中在 `servers/rendering/renderer_rd/forward_clustered/`：

- `scene_shader_forward_clustered.h/.cpp` 将材质深度通道的普通与 Ubershader 变体划入独立 `SHADER_GROUP_MATERIAL`，通过 `enable_material_shader_group()` 按需启用；材质烘焙不再启用整个 Advanced Shader Group。
- `render_forward_clustered.cpp` 在材质通道只更新本次输入实例所需的 Surface Cache，不触发全场景常规颜色、阴影等 Pipeline 的预生成；实际绘制仍沿用原生材质 Pipeline。原生材质与 UV2 烘焙入口同步使用独立材质组，其他渲染通道维持原有更新流程。

此调整仅缩小 Shader 编译与 Pipeline 准备范围，不改变材质求值、采样精度、烘焙数据或资源格式，也不引入批量回读或材质结果缓存。

### Inspector 集成

在 Geometry Instance 的 **Global Illumination → LRT** 折叠子菜单中展示实例绑定配置，将现有 Binding 的参数、烘焙入口和配置警告集中到这里。MeshInstance3D 与 MultiMeshInstance3D 使用相同入口；不再要求添加 LRTGeometryBinding3D 子节点。

由 **LRT 编辑器模块注册 EditorInspectorPlugin**，为支持的 Geometry Instance 注入 LRT 子菜单与属性编辑器，不直接向 MeshInstance3D、MultiMeshInstance3D 或 GeometryInstance3D 的核心属性列表添加 LRT 专用属性。绑定配置使用模块内的 LRTGeometryBinding Resource，保存在节点的 `_lrt_binding` metadata 中，并启用 resource_local_to_scene；Inspector 编辑通过撤销/重做替换该节点的配置资源，避免复制节点后修改配置影响原节点。存储与运行时读取由 LRT 模块负责，不能仅保存在 Inspector 控件中；配置必须随场景保存，并支持重新加载、节点复制、场景实例化和编辑器撤销/重做。烘焙、资源变更监听、运行时注册与调度均留在 LRT 模块，脱离编辑器运行时也能读取绑定，不依赖 Inspector 插件。

LRT 子菜单位于原生 Mode、Lightmap Texel Scale 之后，按 Godot 原生子组（如 Process → Thread Group）的层级左对齐，不绘制左侧蓝色竖条，也不增加额外缩进或 MarginContainer。参数使用原生 EditorProperty，支持属性重置和撤销/重做。Surface Normal Bias 使用可选覆盖复选框，未勾选时显示并继承全局值，不直接展示 -1；所有 GI Mode 均可配置。SDF Resolution、Bake Data 和 Bake Color SDF 操作仅在 Dynamic 模式显示，切换模式不会清除已有配置；警告仅在动态几何存在配置问题时显示。

LRT 子菜单包含以下参数：

| 参数 | 作用与默认值 |
|---|---|
| surface_normal_bias | 接收光照时的世界单位法线偏移，默认 -1 继承 Project Settings；0 禁用，正值以米覆盖全局值，所有 GI Mode 均生效，无需重烘焙；MultiMesh 全部子实例共享 |
| sdf_resolution | 动态 Color SDF 最长轴格数，默认 32，其余轴按比例确定 |
| bake_data | 该 Geometry Instance 的 Color SDF 烘焙资源；MultiMesh 保存共享或按子实例区分的数据绑定，由烘焙生成 |

Dynamic 模式下，子菜单同时提供 **Bake Color SDF** 操作；缺少烘焙数据、变换不受支持等问题在此明确提示。GI Mode 继续使用原有 Mode，不在 LRT 子菜单重复设置。调试选择直接使用 Geometry Instance，不再依赖 Binding 子节点。

Inspector 布局示意：[原生子组左对齐、无蓝色竖条](screenshot/lrt_inspector_left_aligned.png)。

## Project Settings

统一位于 `rendering/global_illumination/lrt/`，在 Rendering → Global Illumination 中与 GI、Voxel GI、SDFGI 并列，不再注册 `rendering/lrt/`：

| 参数 | 作用与默认值 |
|---|---|
| enabled | 全局开关，默认关闭 |
| propagation_iterations | A、L 各自每帧迭代次数，默认 2 |
| propagation_relaxation | 新结果融入传播状态的比例，默认 0.5 |
| geometry_trunks_per_frame | 全部活动 Volume 每帧提交重建的 Trunk 数上限，默认 8 |
| surface_normal_bias | 表面采样位置沿世界空间单位法线外移的距离，世界单位米，默认 0.1；不随 Probe spacing 或 Volume 缩放变化，0 禁用偏移；Volume coverage 仍按真实表面位置计算 |
| sun_shadow_extent | 太阳阴影接收球半径，默认 32 m；与 Volume 大小和相机朝向无关 |
| sun_shadow_resolution | 世界共享太阳阴影 Atlas 的边长，默认 2048，至少 256 且不超过设备上限；D32F 2048² 占 16 MiB，不含驱动开销，多方向光共享总预算 |
| screen_gather_enabled | 使用屏幕缓存完成表面采样，默认开启；关闭时直接采样 Probe |
| screen_gather_scale | Gather 宽高比例，枚举 Full (100%)、Half (50%)、Quarter (25%)，存储为 0、1、2，默认 1（Half）；Half 的像素数为全分辨率的四分之一 |

项目设置界面：[Global Illumination → LRT](screenshot/lrt_project_settings.png)。

## 亮度参数的职责

- **Light energy / physical intensity** 决定灯光本身的输入，同时影响直接光与 GI；`light_indirect_energy` 只缩放该灯的 LRT 注入源，不改变传播公式。非物理灯光单位沿用 Godot 的 π 换算，不额外补偿。
- **材质 albedo** 作为反射率烘焙进 T，接收阶段再乘接收面的材质色；改变几何贡献材质需重烘焙。Environment 的颜色与能量决定天空输入，不是传播增益。
- **Volume intensity** 只乘最终间接光输出，不参与传播反馈；blend 与 priority 决定覆盖和多 Volume 混合，不负责补偿能量。
- **Iterations / relaxation** 控制更新与收敛速度，不应作为稳态亮度旋钮。Spacing / size 控制几何精度与输入边界；缺少外层 Volume 时不会自动生成范围外的反射光。
- **Normal bias** 只修正表面采样位置，Gather 只改变采样与重建方式；二者均不是亮度倍率。相机曝光与色调映射只作用于显示，不改变未曝光的传播场。

LRT 只提供间接漫反射。直接光、镜面反射、接收材质的 BRDF 和最终显示变换由宿主渲染器处理，不通过调整 GI 强度补偿这些环节的差异。

# Debug View

调试统一使用 3D 视口现有的 **Display Advanced...** 菜单，只保留以下 **9 个以 LRT 为前缀的入口**，与 VoxelGI、SDFGI 等视图并列。菜单和视口文字由编辑器适配层接入，数据读取与绘制留在 LRT 模块，要求 Forward+。不再使用独立调试窗口或 Inspect LRT 弹窗，也不要求先输入 Probe 坐标、入射方向或切片才能查看结果。

正常渲染是默认模式，调试模式按当前视口切换。视口直接显示视图名称、图例及缺数据原因；统计信息沿用现有 **Information** 显示，不再另开面板。

## 九个视图

| 菜单名称（省略 LRT 前缀） | 数据与用途 | 视觉表达 |
|---|---|---|
| Indirect Lighting | 最终表面间接光：经过低分辨率 Screen Gather 和深度/法线引导升采样，包含传播反弹光与可见天空，并应用 Volume 强度与混合权重 | 显示间接光本身，不乘表面材质色，不混入直接光或原有环境间接光 |
| Probe Sampling | 在全分辨率真实表面位置直接插值 Probe，跳过 Gather；其余光照计算、Volume 优先级与边缘混合规则不变 | 与 Indirect Lighting 共用曝光与显示映射，通过切换视图判断 Gather 带来的平滑或细节损失 |
| Probes | 同时检查 Probe 布局、有效性和传播后的反弹光 L；不显示直射注入、源项 S 或可见天空 B | 方向小球半径为 0.08 × spacing，球面显示光照颜色、强度和方向；Inside Probe 用不受曝光影响的纯品红色标识；有效零光照保持黑色，以暗灰轮廓辨识，并显示 Volume 边框 |
| Local Visibility | 当前静态与动态几何合成后的 LocalVis，检查局部各方向的通透程度 | 方向球半径为 0.12 × spacing；固定黑色 0 至白色 1，不使用光照曝光或色调映射；Inside Probe 不绘制 |
| Global Visibility | 传播后的环境可见性 A，检查遮挡如何影响整个光场 | 与 Local Visibility 使用相同球大小、位置和灰度刻度，可以直接切换对照；Inside Probe 不绘制 |
| Edge Visibility | 检查 26 邻域中相邻 Probe 之间的共享四位 Edge mask | 直接画连接线，不再用 Probe 球着色代替边权；四条子线段的通路数 0/1/2/3 分别显示为红/橙/黄/青，4 条全部通路时隐藏；两端均 Inside 的连接和出界邻居不绘制 |
| Static LTM | 检查静态烘焙底板本身的局部反弹能力，不读取动态实例合成后的 LTM | 半径为 0.12 × spacing 的方向球显示单位均匀白色入射场下的反弹颜色与方向；只绘制非 Inside 且有效反弹非零的 Probe |
| Color SDF | 检查选中 Geometry Instance 的对象空间烘焙缓存，而不是原始 Mesh | 绘制缓存的零等值面与材质色，以 SDF 梯度产生辨形明暗；明暗不代表场景实际光照；MultiMesh 按当前可见子实例的绑定数据和组合变换分别绘制 |
| Trunk Updates | 检查 8³ Probe 块的动态几何更新状态，以及实例变化前后的 SDF 影响范围 | 用块边框而非整片 Probe 球表达状态；叠加在正常场景上，保留原生编辑器 Gizmo，支持直接拖动实例进行实时测试 |

## 光照与有效性

Indirect Lighting 和 Probe Sampling 使用同一份表面深度/法线约定和运行时采样函数。二者都显示预乘 coverage 的 LRT 间接光，不用去除 coverage 的结果冒充最终采样。Coverage 仍是内部融合权重，但不再提供独立调试显示，也不叠加到其他视图。

光照显示统一使用非负 HDR 值，经相机曝光编码后按 **c / (1 + c)** 压缩，再应用 **1 / 2.2** 显示 Gamma；不逐球、逐通道或逐视图自动归一化，保留强弱差异。Probes 使用相同的光照映射，品红无效标记不参与曝光。

Static LTM 使用单位均匀白光 **SH_ONE** 作为入射场，显示 **Filter(T×SH_ONE)**，不读取动态合成后的 T，不再乘 LocalVis 或应用旧的共享 DC 预算。有效性按反弹能量是否非零判断，不按当前灯光是否照到该 Probe 筛选；显示使用固定白光基准，不随相机曝光改变。LocalVis、GlobalVis 同样使用运行时 Visibility 约束，不能把未约束 SH 重建当作实际通透率。

## 空间参照与选择

- **Volume 选择**：选中 LocalLRTVolume3D 时，Probe、Visibility、Edge、Static LTM 和 Trunk 视图聚焦该 Volume；没有选中 Volume 时显示当前世界中已就绪的活动 Volume。前两个表面光照视图始终使用正常的多 Volume 混合，不因编辑器选择而改变采样结果。
- **实例选择**：Color SDF 使用选中的 MeshInstance3D 或 MultiMeshInstance3D。没有选择合适实例、缺少烘焙数据、子实例布局与烘焙不符或几何仅接收 GI 时，直接提示原因，不显示其他实例的数据来代替。
- **遮挡与密度**：除 Trunk Updates 外，空间诊断使用低亮度中性场景作为参照。Probe 球遵循场景深度遮挡；远处小球自然缩小，不强制维持最小屏幕尺寸。Edge 与 Trunk 被场景遮挡的部分使用弱化虚线，前景使用实线。
- **数据缺失**：黑色只表达零光照或零通透率；LRT 关闭、Volume 无可用缓存等情况通过文字说明，不与有效零值混淆。尚未就绪的数据不能标成品红 Inside Probe。

## Trunk 更新与撤回

块边框颜色固定为：**灰色闲置、黄色待更新、蓝色 CPU 重建或等待 GPU 发布、绿色已发布**。绿色表示几何缓存已发布，不代表传播光照已经收敛；发布后短暂保留，再恢复灰色。

紫色**虚线为旧范围、实线为新范围**，两者都是实例变换后的 SDF 缓存包围盒，包含烘焙边界，不能当作原始 Mesh 边框或全部受影响 Trunk 的并集。移动、撤回、加入和移除实例都遵循同一套变化记录与局部重建规则。

最近发布状态及旧/新范围使用 **60 个 LRT 更新帧**的短暂历史；范围框只在无待处理批次、无脏块且 GPU 已发布对应几何后清除，Volume 重置时同步清空。撤回后短暂同时看到变化前后的框属于诊断信息，更新结束后不得永久残留，也不能把历史框误当作仍未恢复的几何。

视觉参考：[Gather 输出](screenshot/lrt_debug_indirect.png)、[直接 Probe 采样](screenshot/lrt_debug_probe_sampling.png)、[品红无效 Probe](screenshot/lrt_debug_probes_invalid.png)、[Color SDF](screenshot/lrt_debug_color_sdf.png)、[撤回完成后的 Trunk 与 Gizmo](screenshot/lrt_trunk_undo_settled.png)。

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

D 是归一化 Lambert 核的 SH4 投影；Trpd 表示球面函数相乘后投影回 SH4。低阶投影不保持非负性，因此运行时统一应用以下约束，不改变任何资源或 GPU 数据布局。

### 非负性与传输约定

L 保存 Probe 位置方向辐亮度的 SH4 传输矩，不是网格单元的入射功率，也不是非负的离散角度样本。令系数 a=(a0,a_dir)，其中 a_dir=(a1,a2,a3)。辐亮度重建极值为 c0*a0 ± c1*length(a_dir)，Lambert 接收响应的极值为 c0*a0 ± (2/3)*c1*length(a_dir)。允许前者有负瓣，只约束后者非负；邻域传播使用有符号矩乘积，不能将单方向重建截为非负后重新投影：

- **Light(a)**：a0≤0 时返回全零；正 DC 时保留原始全部方向系数。直接灯光入射与天空投影使用此规则，不按漫反射场限幅。
- **Diffuse(a)**：先应用 Light，再将方向系数长度限为 (3/2)*(c0/c1)*a0，约为 0.8660254*a0；只等比例收缩超界方向项，不改变 DC 或方向轴。保证任意法线的 Lambert 响应非负，但允许 SH 辐亮度重建存在负瓣。
- **Visibility(a)**：将平均值 m=c0*a0 限于 [0,1]；方向系数长度不超过 min(m,1−m)/c1。
- **Reflect(T,I)**：Diffuse(T×Light(I))，用于灯光/天空一跳注入。
- **Filter(a)**：Light(a) 的方向系数乘 **1/3**，DC 不变。用于将反射输出匹配到邻域核 K 的矩尺度，以及其单位场/Static LTM 调试响应；不作用于自由空间直通 H 或源 S。数学上等价于单位积分核 (1+dot(n,w))/(4π) 的低阶卷积，但这里不是每步对整个 L 进行空气散射，也不是艺术增益或任意无效 SH 的修复器。

公共 GPU 运算集中在 `modules/lrt/shaders/lrt_sh_inc.glsl`：`lrt_constrain_light`、`lrt_constrain_diffuse`、`lrt_constrain_visibility`、`lrt_reflect`、`lrt_filter_radiance`。`lrt_visible_sky` 保持原有约定，供注入和表面可见天空使用；CPU 几何合成沿用 Visibility 公式。

L 的直通使用 Σ Edge×Trpd(K,L)，只做一次路径遮挡，不再乘 LocalVis，不逐邻居裁剪负瓣或做 Diffuse。反射以本地旧 L 近似近邻表面的入射场，先计算 T×L_old，再仅对反射输出应用 Filter；不把 H 当作全部表面入射。H、匹配后的反射和源 S 合并后统一应用 Diffuse，再松弛写回。这样保留合法均匀空气场的方向，不用虚假的反向光补偿缺失的外部供光。

T 的中心线命中和 Edge 的四条偏移线可能不完全互补。因此用 O=Σ Edge×K 与 Filter(T×SH_ONE) 的和计算每通道缩放 g≤1：只在单位场响应的 Lambert 最大值超过1时减小 H+R，不提高光照、不缩放源 S，也不按参考渲染结果拟合倍率。此约束针对几何传输算子的单位场响应，不是完整的物理通量守恒证明。

静态 Volume 沿用实际几何法线的 LTM 烘焙；传播与反射矩匹配不增加烘焙字段，已有真实法线缓存无需重烘焙。新增外层或改变 spacing 时仍需按该布局烘焙，不能直接降采样旧 T/Edge。LocalVis 用于 A/天空可见性及调试，其上传、CPU 合成和 Visibility 约束保持一致，A 仍按下文的可见性模型传播。初始化双缓冲 A=Visibility(V)、L=0；更换计算规则后清空运行时历史并重新传播。LTM 36 B/Probe、LocalVis half4、L/A SH4 双缓冲、Color SDF、Trunk、Edge 及其压缩和资源格式保持一致。

## 构建与合成

令 ωᵢ=normalize(δᵢ)，oᵢ 为占用，aᵢ 为线性反射率，c 表示 RGB 通道。

静态命中三角形的 Volume 空间几何法线为 nᵢ。令 fᵢ 在命中且 nᵢ·ωᵢ<0 时为 1，否则为 0，即只取朝向 Probe 的反射表面：

```math
T_c^{static}=\frac{4\pi}{26}\sum_i f_i a_{i,c}Y(-\omega_i)D(-n_i)^{\mathsf T}
```

同一平面的入射响应由真实法线确定，太阳方向只影响入射强度，不通过采样连线改变其归一化出射形状。

动态实例按 Color SDF 的占用与颜色使用邻域方向近似：

```math
T_c^{dynamic}=\frac{4\pi}{26}\sum_i o_i a_{i,c}D(-\omega_i)Y(\omega_i)^{\mathsf T}
```

静态与动态使用相同的可见性构建式，背向命中同样计入遮挡：

```math
V=SH_{\mathrm{ONE}}-\frac{4\pi}{26}\sum_i o_iY(\omega_i)
```

归一化使用全部 26 个方向。可选固定自发光作为独立源项：

```math
J_c=\frac{4\pi}{26}\sum_i o_iL_{e,i,c}Y(-\omega_i)
```

从静态缓存开始，对当前动态实例依次合成：

```text
T ← T + T_instance
V ← Visibility(Trpd(Visibility(V), Visibility(V_instance)))
J ← J + J_instance
```

## 传播与最终采样

对邻居 qᵢ=p+sδᵢ，令 dᵢ=normalize(δᵢ)。L 和 A 共用传输矩核：

```math
K_i=\left(\frac{1}{26c_0},-\frac{d_{i,x}}{26c_1},
-\frac{d_{i,y}}{26c_1},-\frac{d_{i,z}}{26c_1}\right)^{\mathsf T}
```

```math
H_p=\sum_i e_{pq_i}\operatorname{Trpd}(K_i,L^{old}_{q_i}),
\qquad
U_p=\sum_i e_{pq_i}\operatorname{Visibility}(\operatorname{Trpd}(\operatorname{Flip}(K_i),\operatorname{Visibility}(A^{old}_{q_i})))
```

H 不逐邻居应用 Diffuse，也不计算 max(dot(L,Y),0)，以保留有符号 SH 矩之间的正常相消；Edge 只筛选一次路径。反射输出经过 Filter，与 K 的方向矩尺度匹配，源 S 不额外滤波。按 RGB 分别计算单位场响应及只减不增的几何缩放：

```math
\begin{aligned}
O_p&=\sum_i e_{pq_i}K_i\\
C_p&=O_p+\operatorname{Filter}(T_pSH_{\mathrm{ONE}})\\
g_p&=\frac{1}{\max(1,c_0C_{p,0}+\frac{2}{3}c_1\lVert C_{p,\mathrm{dir}}\rVert)}
\end{aligned}
```

g 的峰值取 Lambert 响应，不是点辐亮度重建值；它不依赖当前灯光、曝光或显示强度。全局可见性、可见天空、一跳源及反弹光依次为：

```math
\begin{aligned}
A_p^{raw}&=\operatorname{Visibility}(\operatorname{Trpd}(\operatorname{Visibility}(V_p),\operatorname{Visibility}(U_p)))\\
B_p&=\operatorname{Flip}\bigl(\operatorname{Light}(\operatorname{Trpd}(\operatorname{Light}(Sky),\operatorname{Visibility}(A_p)))\bigr)\\
S_p&=\operatorname{Reflect}(T_p,\operatorname{Light}(Ddirect_p)+B_p)+\operatorname{Diffuse}(J_p)\\
R_p&=\operatorname{Filter}(T_pL_p^{old})\\
L_p^{raw}&=\operatorname{Diffuse}\left(S_p+g_p(H_p+R_p)\right)
\end{aligned}
```

Ddirect 是考虑阴影与衰减后的直接入射光，B 是可见天空，S 是直接光/天空的一跳反射及自发光。H 和 R 使用同一本地迭代的旧状态，外层边界读取已完成的外层状态；合并后只应用 Diffuse，不再整体 Filter。在无几何、全通路、均匀且满足 Diffuse 约束的场中，ΣK=SH_ONE、H=L、g=1，因此保留 DC 和方向，不再每步把空气当作散射介质。Diffuse 限幅、SH4、局部入射近似、固定26方向及有限边界仍是非物理闭合，不能据此宣称任意光束精确传播或完整通量守恒。

A 初始取 Visibility(V)，L 初始取0。邻域式中的出界 L/A 使用外层已完成的传播状态，并转换到当前 Volume 空间；没有就绪外层供给时，开放边界取 L=0、A=SH_ONE。A/L 使用双缓冲和松弛更新 Xnew=mix(Xold,Xraw,0.5)，A 写回恢复 Visibility，L 混合已满足 Lambert 约束的新旧状态；默认各每帧两次迭代。改变传播公式后需重置运行时历史，不改变静态缓存。

表面在 x_sample=x+bias×n 处对有效 Probe 插值得到 L、A 后，分别应用 Diffuse/Visibility；用 Light(Sky)、A 求可见天空 B，再对 B 单独应用 Diffuse，最后以真实表面法线计算：

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

Color SDF 保存 SNORM16 距离及其缩放值、RGBA8 颜色，可选固定自发光另存。烘焙资源关联各自的网格、几何和材质配置。表面 bias 和 Inside 剔除后的归一化采样不改变上述 Probe、Color SDF、Edge 或烘焙资源格式。

每视图光照/guide 缓存在全分辨率与 Gather 分辨率分别使用 **FP16 RGBA 8 B＋FP32 RGBA 16 B，共 24 B/像素**。逐物体 bias 额外使用全分辨率 **R16F 2 B/像素**；开启 MSAA 时另有对应多重采样附件，其容量随样本数增加。深度和法线沿用渲染器现有纹理。
