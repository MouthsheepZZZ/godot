# LRT GI 与 Volume 设计

LRT 为载具和世界提供动态间接漫反射。本设计将**节点树中的几何归属、空间中的计算范围、Volume 之间的光照输入**分开：载具保留自己的精细静态烘焙，对外只提供可撤回的 Color SDF；粗外层计算大尺度光照，再单向传入精细区域。

复用现有 LRT 的材质烘焙、Probe 网格、Color SDF、Trunk 更新、SH4 传播和 Screen Gather，不改变其几何查询与传输公式。变化集中在几何收集、Volume 关系、组级烘焙和表面混合。

## 节点与归属

| 节点 | 职责 | 几何来源 |
|---|---|---|
| `LRTGI` | 拥有一组物体，烘焙时自动确定计算范围 | 自己子树中的几何，以及允许进入的运行时 Color SDF |
| `LRTVolume3D` | 显式划定空间范围，不建立几何归属 | 范围内未归属的 Static，以及允许进入的运行时 Color SDF |
| `LRTDetailVolume3D` | 细化所属者的局部区域，以下称 Detail | 继承所属 `LRTGI`／`LRTVolume3D` 的几何来源，按自己的网格计算 |

几何属于最近的 `LRTGI` 祖先。查找跨越普通容器和场景实例边界，不依赖 Godot 的 `Node.owner`。`LRTVolume3D` 和 Detail 不建立几何归属；物体跨越包围盒不会改变归属，节点重新挂载才会改变归属。

`LRTGI` 之间不能在节点树上嵌套，但允许空间重叠。`LRTVolume3D` 不限制子节点，也不因几何位于其子树中而赋予特殊含义。

Detail 归属于最近的 `LRTGI`／`LRTVolume3D` 祖先，不能嵌套 Detail。它必须相对所属者固定，范围完整位于所属者内，`probe_spacing` 更小。父级与所有所属 Detail 构成一个**计算组**。Detail 有独立网格、静态缓存和传播状态，但不独立烘焙、供给其他组或设置全局优先级。

```text
World
├─ GlobalGI               LRTVolume3D，priority 0
├─ Terrain                Static，无 LRTGI 归属
├─ StreamedChunks
│  └─ Chunk
│     └─ ChunkGI          LRTGI，priority 10
│        ├─ Buildings     Static
│        └─ RoomDetail    LRTDetailVolume3D
└─ Vehicles
   └─ Train
      └─ TrainGI          LRTGI，priority 20
         ├─ Body          Static
         ├─ Door          Dynamic，对外 Enabled
         ├─ Box           Dynamic，对外 Inherit
         └─ CabinDetail   LRTDetailVolume3D
```

场景树负责组织和生命周期，空间相交负责参与范围。载具进入地块不需要重新挂载到地块下；LRT 不自动修改节点树。

## 范围与布局

`LRTGI` 不提供手动 `size`。烘焙时，在节点局部空间合并所属参与几何的包围盒：包含 Static 和 Dynamic 的烘焙时范围，不包含 Disabled、仅接收型几何、外部贡献或 Detail 范围。按 `extend_margin` 向各方向外扩，再按 `probe_spacing` 向外对齐，生成有限的 Probe 网格。Extend Margin 以世界空间米计量，默认 1 m；烘焙时根据节点的正均匀缩放换算为局部距离，不随 Probe spacing 改变。它只在显式烘焙时应用，运行时缩放只变换已有网格，不重新应用边距。

范围随成功的显式 Bake 更新，运行时不跟随 Dynamic 自动扩容或重排。整个 `LRTGI` 移动时网格一起移动；Dynamic 是否影响本组仍按网格及采样邻域判断，完全离开影响范围也不触发扩容。它仍按空间覆盖接收光照，并按对外策略影响其他组。Gizmo 只显示最近成功烘焙的范围，不提供尺寸手柄；分散过远的子树会产生大网格，应由用户合理分组。

`LRTVolume3D` 和 Detail 保留显式 `size` 及六面尺寸手柄，位置与旋转使用节点变换。Detail 不反向扩大父级自动范围；烘焙时以本次生成的父级范围检查内含关系。没有所属参与几何可用于推导范围的 `LRTGI` 无法烘焙；需要在空区域布置计算网格时，使用显式范围的 `LRTVolume3D`。

## 几何参与与对外贡献

沿用 Geometry Instance 原生 `gi_mode`，不增加另一套 LRT Static／Dynamic 模式或启用开关。完整支持 `MeshInstance3D` 和 `MultiMeshInstance3D`；MultiMesh 使用节点与子实例组合变换，遵循实际可见数量、材质及实例数据。

`LRTGI` 提供两个默认值：

| External Contribution | 默认值 |
|---|---|
| Static | On |
| Dynamic | Off |

物体的 `External Contribution` 为 `Inherit / Disabled / Enabled`，默认 `Inherit`。Inherit 按物体当前 GI Mode 读取所属 `LRTGI` 对应的开关；显式 Disabled／Enabled 覆盖默认值。该设置仅控制对**其他计算组**的几何贡献，不影响本组贡献或表面接收光照。

| 几何状态 | 所属 `LRTGI` 及其 Detail | 其他 `LRTGI` 及其 Detail | `LRTVolume3D` 及其 Detail |
|---|---|---|---|
| 已归属 Static | 三角形精细静态烘焙 | 对外开启时使用 Color SDF | 对外开启时使用 Color SDF，不写入静态底板 |
| 已归属 Dynamic | Color SDF 运行时合成 | 对外开启时使用 Color SDF | 对外开启时使用 Color SDF |
| 未归属 Static | 不适用 | 不贡献几何 | 按范围三角形静态烘焙 |
| 未归属 Dynamic | 不适用 | 已配置有效 Color SDF 时参与 | 已配置有效 Color SDF 时参与 |
| Disabled | 不贡献几何 | 不贡献几何 | 不贡献几何 |

未归属物体不使用 External Contribution。未归属 Static 是合法状态，不警告；未归属 Dynamic 没有配置 Color SDF 时，不参与 LRT，也不警告。已配置但无效的资源属于配置问题。

烘焙收集和运行时贡献限定在同一 World3D 的当前可见节点中；已生成的静态底板不随节点显隐改变。所有参与都经过空间筛选，包含 Probe 邻域和 Edge 查询所需范围，不是每次更新全部 Volume。同一几何在同一个目标网格中只计入一次；Detail 继承来源，不叠加父级已经合成的几何缓存。`LRTGI` 只直接输出自己所属且允许对外贡献的实例，不转发接收到的外部贡献。

因此，示例中的 Body 在车内保留精细静态信息，在 GlobalGI 中使用可移动 SDF；Box 默认只更新车内，Door 则可同时影响外部。

## 数据与更新

每个网格保存自己的数据：

- **静态底板**：`LocalLRTVolumeData` 保存当前布局下的 LTM（T）、LocalVis（V）、Inside、Edge 和可选固定自发光 J。
- **对象空间几何**：`LRTInstanceGeometryData` 保存 Color SDF，由现有 `LRTGeometryBinding` 绑定到实例。相同实际几何、材质及实例求值可共享数据，不按 Mesh 标识盲目共享。
- **运行时合成几何**：从该网格静态底板开始，合成当前有效的 Color SDF，生成传播所需约束。
- **传播状态**：各网格独立保存反弹光 L 和环境可见性 A，各自双缓冲。

静态烘焙使用三角形查询和真实几何法线；Color SDF 保留现有对象空间采样、Inside 和 Edge 查询方式。不同 spacing 的网格可以共享对象空间 SDF，但不能直接重采样其他 Volume 的 T／Edge 作为自己的烘焙结果。

### 静态底板只由显式烘焙改变

**烘焙到哪个 Volume，静态数据就留在哪个 Volume。**原 Static 节点随后移动、隐藏、删除或重新挂载，都不自动修改底板，也不触发烘焙。调用方自行决定何时重烘焙；运行时不持续扫描 Static 以自动修复旧数据。

同一个物体向其他 Volume 提供的 Color SDF 是运行时贡献，不是这些 Volume 的静态底板。节点移除或对外贡献关闭时，撤回运行时贡献；已经烘焙在本地的旧静态数据可以继续保留。两者无需同步，不增加静态对外快照集合。

### 运行时合成使用当前相对关系

更新依据是 `inverse(volume_transform) × instance_transform`。贡献实例移动、加入、移除、显隐、归属或对外策略改变，以及目标 Volume 自身移动，都应更新受影响的旧、新范围。整组共同移动且相对关系不变时，不重复重建内部几何；外部几何仍按新的相对关系更新。

复用现有 8³ Probe Trunk 调度：分别标记旧、新影响范围，从静态底板重建局部块，异步发布，不重建两者之间无关区域。保留持续为空气的 Probe 历史，新 Inside 清零，由 Inside 变为空气时初始化；不因局部几何更新重置整组光场。

沿用节点树显隐控制活动状态，不增加 Enabled 开关。父级隐藏或未就绪时，Detail 不独立供光或显示；隐藏保留缓存，恢复前同步运行时变化。卸载释放相应运行时状态、撤回对外贡献和边界来源，不保留幽灵光场。其他 Volume 已烘焙的静态数据仍只在显式重烘焙时改变。

## 光照传播

几何贡献与光照输入是两条独立路径：

```text
Body ──Color SDF──→ Global 的运行时几何

Global 的 A/L ──→ 地块或载具的 A/L ──→ 各自 Detail 的 A/L
```

Color SDF 不携带源 Volume 已计算的光照。Global 根据收到的几何独立计算大尺度影响，不需要载具反向传回精细光场。

三种节点使用同一套 26 邻域传播。A 表示环境／天空方向可见性，L 表示反弹光方向辐亮度；范围内读取自己的邻居，出界邻居按下面的规则取输入：

| 接收者 | 边界来源 |
|---|---|
| `LRTGI`／`LRTVolume3D` | 覆盖查询世界位置、已就绪且 priority 严格更低的 `LRTGI`／`LRTVolume3D`；选择最高 priority，同级候选按场景树顺序取最后一个 |
| Detail | 只读取所属 `LRTGI`／`LRTVolume3D`，不搜索其他组 |

同优先级组不交换 A/L，高优先级不向低优先级反馈，Detail 不成为其他组的边界来源。不存在可用来源时，开放边界取 `L=0`、`A=SH_ONE`。边界读取原始传播场，Inside 的零状态也是有效输入，不套用表面采样的 Inside 剔除规则；SH 方向转到接收者局部空间后，继续经过接收者的几何约束。

每个世界沿用 A／L 交替更新：A 帧传播 A；L 帧根据当前灯光、天空、阴影和 J 注入并传播 L。按组 priority 从低到高执行，每个父级完成后再执行所属 Detail。接收者读取上游本阶段已完成状态，灯光与天空仍由各网格独立注入。多视图不重复推进世界传播，intensity 只作用于最终显示。

大尺度跨地块影响由更低优先级、覆盖更广的 Volume 表达，而非同级交换。没有外层时允许缺少外部反弹，不通过补光、传播增益或保留已卸载状态掩盖。

## 表面接收与组级混合

表面接收与几何贡献分开，但 **Static 的接收受 LRTGI 归属限制，Dynamic 和 Disabled 不受此限制**。所有来源仍必须实际覆盖表面位置：

| 接收物体 | 有资格接收的计算组 |
|---|---|
| Static，有所属 `LRTGI` | 自己的 `LRTGI` 组，以及覆盖位置的 `LRTVolume3D` 组；不能直接接收其他 `LRTGI` 组 |
| Static，无所属 `LRTGI` | 仅覆盖位置的 `LRTVolume3D` 组 |
| Dynamic | 所有覆盖位置的 `LRTGI` 和 `LRTVolume3D` 组，与节点归属无关 |
| Disabled | 与 Dynamic 相同，但不贡献几何 |

Detail 继承所属组的接收资格，不能绕过父级限制。先筛选有资格的组，再应用 GI 覆盖顺序、priority、样本有效性和边缘混合规则。接收使用当前 GI Mode 和节点树归属，不需要重新烘焙，也不增加接收开关或绑定参数。

External Contribution 仍只控制对外几何贡献，不控制接收。Dynamic 即使没有 Color SDF，也能在进入车厢时接收其 `LRTGI`；无关 Static 即使位于车厢范围内，也不能直接接收该组。此限制不改变跨组 SDF 贡献或既有 A/L 边界输入规则。

**间接漫反射的覆盖顺序为：有效 Lightmap／Lightmap Capture、VoxelGI 优先于 LRT，LRT 优先于 SDFGI。**具体表面已有有效 Lightmap／Lightmap Capture 或 VoxelGI 时跳过 LRT；SDFGI 不抑制 LRT，也不作为 LRT 的 A/L 边界输入。保留 SDFGI 等原有路径算出的间接光作为 `ambient_original`，由有接收资格且样本有效的 LRT 组按真实覆盖率混合；满覆盖时替换，包括有效黑色，边缘和无有效覆盖的位置保留相应原有间接光。此顺序只作用于间接漫反射，不替换原有镜面反射。不能仅因为场景存在原生 GI 就关闭整个 LRT，也不排除物体作为 LRT 几何参与。

LRT 内部使用现有表面采样：在真实位置沿世界法线偏移后，插值八个 Probe；剔除 Inside 并归一化，L/A 使用同一权重。没有有效 Probe 时跳过当前网格，有效黑色仍参与。覆盖判断与边缘权重使用未偏移的真实表面位置。

三种节点统一使用世界空间的 `blend_width`，默认 1 m，Inspector 显示为 Blend Width；`LRTGI` 也有自己的边缘过渡。令 d 为表面到当前网格最近边界的世界距离，边缘权重为 `smoothstep(0, blend_width, d)`，宽度为 0 时禁用过渡。宽度不随 Probe spacing 或节点缩放变化，修改后立即影响表面混合，无需重烘焙，也不改变 A/L 传播或 Edge Visibility。

`LRTGI` 的 Extend Margin 决定烘焙范围，Blend Width 决定范围内的显示过渡，二者独立、不自动联动。需要所属几何位于满权重区时，应留出足够的烘焙边距；Detail 的局部过渡与父级整体过渡都使用各自的米制宽度。

先组合每个父级及其 Detail，再混合各组：

1. 组内从有效父级样本开始，按场景树顺序，用各有效 Detail 的边缘权重混合；后者覆盖前者。父级 Probe 无效不阻止有效 Detail 参与。
2. 组结果整体受父级边界权重限制，Detail 不扩大父级覆盖范围；intensity 统一继承父级并只应用一次。
3. 各组按 priority 从低到高混合，同 priority 按场景树顺序；后者覆盖前者。

使用预乘颜色 C 与覆盖率 c。组内有效父样本初始化为 `(I_parent, 1)`，无效则为 `(0, 0)`；每个有效 Detail 样本 I 按边缘权重 w 覆盖：

```text
C ← (1 − w) C + w I
c ← (1 − w) c + w
```

组内结果同乘父级边界权重，颜色再乘父级 intensity，得到 `(C_group, c_group)`。各组从 `(0, 0)` 开始合成：

```text
C ← (1 − c_group) C + C_group
c ← (1 − c_group) c + c_group
```

最终沿用 `ambient = (1 − c) × ambient_original + C`。

Screen Gather、材质直接采样、透明路径和相关表面调试视图遵循相同的接收资格、GI 覆盖顺序、组级混合、bias 和有效性规则。

### 共享表面采样配置

逐物体采样使用运行时共享配置 `SurfaceProfile = { normal_bias, receiver_scope }`。接收域只有三种语义：Dynamic／Disabled 不限制组，未归属 Static 仅允许公开的 `LRTVolume3D` 组，已归属 Static 允许公开组及指定 `LRTGI` 组。Detail 使用父组身份，不创建接收域。

按实际 bias 和接收域去重，物体 GPU 实例记录传递 32 位配置 ID，而非每物体分配不同配置。复用现有实例属性同步与几何归属结果，模式、归属或 bias 改变时更新；配置属于运行时数据，不写入几何烘焙资源，不增加 Inspector 参数。实例记录原来传 bias 的位置改传配置 ID，采样时从共享表读取 bias 和接收域；配置表与本次采样使用的组元数据保持一致。

全分辨率预通过的 R16F bias 通道和 Gather 的 R16F 代表样本 bias 通道，均替换为 `R32_UINT` 配置 ID 通道，不另加独立的接收域附件。保留已有位置／法线 guide 和光照缓存。预通过仍须表达 Lightmap／Lightmap Capture、VoxelGI 优先而不参与 LRT 的表面状态，SDFGI 表面保留正常配置 ID，不能因通道替换丢失这项判断。MSAA resolve 的整数配置 ID 与深度、法线取自同一个样本，不能插值或平均。

统一采样入口解析配置，在每个组读取 Probe 之前检查接收资格，不符合则跳过；再执行覆盖判断、Probe 插值和组级混合。不为每个物体建立 Volume 列表，也不改变 A/L 传播或几何贡献。

Gather 读取代表像素的配置 ID，并保存到代表样本通道。材质仅在配置 ID 相同且已有位置／法线条件匹配时复用 Gather，ID 比较同时覆盖 bias 与接收域；不匹配走既有正式的直接 Probe 采样路径，并使用目标物体自己的配置。透明路径与表面调试共用配置及资格判断。Indirect Lighting 调试复用 Gather 时同样检查目标表面的配置 ID 与位置／法线 guide，不匹配则按目标表面直接采样，避免隔离视图中跨接收域显示其他物体的缓存。不能因位置、法线和 bias 相近而跨接收域复用，也不让所有 Static 绕过 Gather。

使用 32 位精确索引，不引入 8／16 位配置数量上限或哈希碰撞式匹配。新增成本主要来自原 16 位 bias 通道变为 32 位 ID 通道，以及共享配置表，不增加一份独立的全分辨率接收域纹理。

## 显式组级烘焙

`LRTGI`／`LRTVolume3D` 是烘焙入口，Detail 没有独立 Bake。编辑器与运行时复用现有异步 `LRTBakeTask`，仅由用户操作或显式调用启动，不因数据缺失或场景变化自动触发。

| 入口 | 一次任务准备的数据 |
|---|---|
| `LRTGI` | 先对所属几何求值并推导新范围，再生成自身与所属 Detail 的静态底板；准备所属 Dynamic，以及对外贡献开启的 Static 所需 Color SDF |
| `LRTVolume3D` | 自身与所属 Detail 范围内未归属 Static 的静态底板；不替外部来源烘焙 SDF |
| Geometry Instance | 单独烘焙对象空间 Color SDF，用于局部修改或游离 Dynamic 加入 LRT |

父级和 Detail 使用同一次捕获的场景输入，各自按网格烘焙，不因节点树相邻而扩大几何范围。父级及所属 Detail 共用任务成败；失败或取消不替换整组旧资源，成功后统一提交本任务结果。沿用已有进度、取消、问题报告、增量求值和资源保存机制。

资源提交成功与 GPU 几何就绪分开处理，运行时仍异步合成、上传，不要求所有网格同帧完成。编辑器保存组资源时，将 Detail 和 Color SDF 写为父资源旁的压缩资源并保留引用，避免把大块缓存展开进场景文本。实例化场景内部的几何绑定通过 Godot 原生可编辑实例覆盖保存，不改变节点树拓扑或几何归属。不为原本已烘焙的数据增加自动失效、自动重烘焙或卸载撤回静态底板的机制。

## Inspector、警告与集成

采用 Godot 原生属性行、枚举、复选框和折叠分组，不添加定制卡片、额外缩进、常驻状态说明或归属选择器。参数含义放 tooltip，只有配置错误时显示警告。

### LRTGI

用米制 `Extend Margin` 替代手动 `Size`，并保留米制 `Blend Width` 控制本组边缘过渡：

```text
LRTGI
  Extend Margin             1 m
  Probe Spacing             0.5 m
  Priority                  0
  Blend Width               1 m
  Intensity                 1
  Bake Data                 <empty>

External Contribution
  Static                    On
  Dynamic                   Off
```

`External Contribution` 只决定所属物体选择 Inherit 时的默认策略，不是不可覆盖的总开关。tooltip 说明其不影响本组贡献和光照接收。

### LRTVolume3D

显式设置覆盖空间，不提供 External Contribution，也不建立新的贡献策略继承来源：

```text
LRTVolume3D
  Size                      (10, 10, 10) m
  Probe Spacing             0.5 m
  Priority                  0
  Blend Width               1 m
  Intensity                 1
  Bake Data                 <empty>
```

### LRTDetailVolume3D

只展示局部网格参数和组内生成的数据，不提供独立 priority、intensity、External Contribution 或 Bake：

```text
LRTDetailVolume3D
  Size                      …
  Probe Spacing             …
  Blend Width               1 m
  Bake Data                 …
```

Detail 的范围与 spacing 必须符合所属者约束。所属关系由节点树确定，不增加可编辑的 Owner 字段。父级调整边距或 spacing 不自动修改 Detail。

### Geometry Instance

保留原生 `Global Illumination → Mode` 和 `Lightmap Texel Scale`，在现有 LRT 子组中追加标准枚举，不在核心节点重复定义 GI 模式：

```text
Global Illumination
  Mode                      Dynamic
  Lightmap Texel Scale      1

  LRT
    External Contribution   Inherit
    Surface Normal Bias     …
    SDF Resolution          32
    Bake Data               <empty>
    [Bake Color SDF]
```

`External Contribution` 固定为 `Inherit / Disabled / Enabled`。tooltip 说明：只控制对所属组之外的贡献；Inherit 按当前 GI Mode 读取所属 `LRTGI` 默认值。不把枚举替换成计算结果，也不额外显示 Owner、Effective Contribution 等状态字段。

按适用条件显示属性：

- GI Mode 为 Disabled：隐藏 External Contribution 和 SDF 参数、烘焙入口，保留 Surface Normal Bias。
- 没有 `LRTGI` 归属：隐藏 External Contribution；Dynamic 仍可准备或绑定 SDF，Static 不显示 SDF 操作。
- 已归属 Dynamic：显示 External Contribution 及 SDF 参数、烘焙入口。
- 已归属 Static：显示 External Contribution；仅最终对外开启时显示 SDF 参数、烘焙入口。

Surface Normal Bias 在属性值区域使用覆盖复选框，后接数值输入框：未勾选时显示并继承 Project Settings，勾选后允许按世界单位编辑，不直接暴露 -1。SDF Resolution 默认最长轴 32 格。

### 编辑器操作与存储

`LRTGI`／`LRTVolume3D` 的组级 Bake 沿用 3D 编辑器工具栏；选择 Detail 时不提供独立 Bake，需选择其所属者操作。Geometry 沿用已有 Bake Color SDF 入口。

Geometry 配置继续存入模块持有的 `LRTGeometryBinding`，由 Inspector 插件展示，随场景保存并支持复制、实例化、撤销和重做，不向引擎核心节点添加 LRT 专用属性。使用原生 EditorProperty、子组层级、重置与 tooltip，LRT 子组不绘制额外竖线或缩进。

载具通常使用 0.25 m spacing；全局 `LRTVolume3D` 按覆盖范围和预算选更粗间距。所有节点与实例继续支持平移、旋转和正均匀缩放，不支持非均匀／负缩放或剪切。

### 配置警告

Warning 只报告明确参与后无法工作的配置：

- `LRTGI` 嵌套或烘焙时没有参与几何可用于推导范围；Detail 无所属者、嵌套、越界、spacing 不够细或局部布局与烘焙不匹配。
- 必需的 Volume 数据缺失或网格布局不匹配，不支持的非均匀／负缩放或剪切。
- 本组 Dynamic 或对外 Static 缺少必要 SDF；已配置 SDF 无效、与 Mesh／MultiMesh 布局不匹配。仅接收型材质沿用现有规则，不要求贡献几何。

未归属 Static、未配置 SDF 的游离 Dynamic、关闭对外贡献、暂不相交、隐藏或空静态底板都不是错误。不要以原 Static 节点变化或消失为由自动失效已有底板。警告说明受影响的贡献及修复动作，不在每个子物体重复报告父级问题。

Volume 使用原生节点配置警告，Geometry 复用已有 LRT Inspector 验证面板；烘焙细节保留在任务问题报告中。

## Display Advanced 调试视图

菜单入口固定，选中的计算节点统一决定所有 LRT 视图的观察范围，不为三类节点建立不同菜单：

| 选择 | 观察范围 |
|---|---|
| `LRTGI`／`LRTVolume3D` | 父级及其所有所属 Detail，作为完整计算组 |
| Detail | 仅该 Detail，不包含父级或其他 Detail |
| 没有选择计算节点 | 当前世界所有已就绪的活动计算组 |

Geometry Instance 不作为调试视图的筛选入口。选择只改变诊断输出，不改变实际几何参与、传播、优先级或正常渲染。Indirect Lighting、Probe Sampling 和 Volume Blending 缩小观察范围后，仍遵守真实表面的接收资格，不能让 Static 接收其他 `LRTGI`；LTM、LocalVis 等网格数据诊断则完整显示目标网格，不按接收物体裁切。视口标明当前计算组／Detail 的名称；选中者隐藏、未就绪或缺少数据时说明原因，不退回显示其他节点。

保留现有九个入口，增加一个 Volume Blending，统一遵循上述观察范围：

| 视图（省略 LRT 前缀） | 显示内容 |
|---|---|
| Indirect Lighting | 观察范围内的间接光结果，读取 Screen Gather 分辨率的缓存；选父级显示组内混合，选 Detail 显示自身，未选择时显示全局组级混合 |
| Probe Sampling | 与 Indirect Lighting 相同的观察范围和混合规则，但在全分辨率直接采样 Probe |
| Probes | 观察范围内各网格的 Probe 布局、Inside 与反弹光 L |
| Local Visibility／Global Visibility | 各网格合成后的 LocalVis／传播后的环境可见性 A |
| Edge Visibility | 各网格相邻 Probe 之间的四位 Edge 通路 |
| Static LTM | 各网格已烘焙静态底板的反弹能力，不混入运行时 SDF 贡献 |
| Color SDF | 观察范围内的目标网格实际认定为 Color SDF 贡献的实例，而不是所选实例拥有的资源 |
| Trunk Updates | 各网格运行时几何更新与撤回状态，包含外部 Static SDF 和目标网格移动导致的更新 |
| Volume Blending | 用诊断颜色表达观察范围内的实际混合权重，不受材质颜色、光照强度或曝光影响 |

空间诊断在选择父级时同时显示父级与 Detail 的数据和范围，不只聚焦父级网格，也不把 Detail 数据合并到粗网格。Gizmo 和诊断绘制使用各自实际烘焙范围，支持 `LRTGI` 自动范围相对节点原点的偏移。

### Color SDF 的目标视角

显示集合使用与运行时几何合成相同的参与规则及空间筛选。选择父级时取父级和所属 Detail 的参与集合并去重；单选 Detail 时只显示影响该 Detail 的 SDF。MultiMesh 按实际子实例及其变换处理，不能仅按共享资源去重。

例如选中 TrainGI 时，本组已烘焙的 Body 不作为 SDF 显示，即使它持有用于对外贡献的 SDF；Box、Door 以及有效的外部 SDF 则按参与情况显示。选中 GlobalGI 时，Body 可以因对外贡献而作为 SDF 显示。关闭对外贡献的 Box 不会因此出现在 GlobalGI 的 SDF 视图中。

没有 SDF 贡献是合法空结果，不能用原始 Mesh、其他计算组或任意实例的数据填充。此规则也用于其他视图：始终展示所选计算范围中的对应数据。

### 混合与既有表达

Volume Blending 为不同组分配不同颜色，Detail 使用父组同色系的不同明度，并按真实边缘权重混合。选父级时检查本组及 Detail 的组合，单选 Detail 时检查自身覆盖，未选择时检查全局混合；Lightmap／Lightmap Capture、VoxelGI 优先区域显示固定灰色，SDFGI 不屏蔽 LRT 来源颜色，无有效 LRT 覆盖显示黑色，有效零光照仍正常着色。隔离显示的颜色表示当前观察范围的结果，不冒充最终全局结果。

沿用现有 Probe 球、Inside 品红、可见性灰度、Edge 通路颜色、Trunk 更新颜色与短暂旧／新范围历史。光照视图保留既有曝光规则；数据缺失不以有效黑色或 Inside 代替。复用现有性能统计和 3D 视口，不增加独立调试窗口。

三种节点共享现有模块内的烘焙、运行时和渲染能力，不复制传播算法或引入通用空间管理器。`LRTVolume3D` 仍使用密集规则网格，范围与 spacing 决定其内存和计算成本；Streaming、节点树组织与显式烘焙时机由游戏控制。
