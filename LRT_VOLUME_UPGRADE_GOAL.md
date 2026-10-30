# LRT Volume 升级目标

## 目标与依据

在当前 Godot 分支完整实现 `lrt_module_development/lrt_advance_volume_design.md` 定义的 LRT GI／Volume 系统，覆盖引擎节点、几何收集与缓存、烘焙、运行时传播、表面混合、Inspector、Gizmo 和 Display Advanced 调试视图，并通过编译与实际场景验证。

核心问题是：载具结构相对车厢静止，需要保留精细静态烘焙；相对世界则在移动，不能被写入世界的静态底板。解决方式是把节点树几何归属、空间计算范围、跨组几何贡献和光照边界输入分开。不要只做节点重命名或 UI 改动，也不要把全部载具几何改成 Dynamic 来规避问题。

本文件包含执行所需的核心约定；完整 UI 布局和设计细节见上述设计文件。`lrt_module_development/local_lrt_design.md` 记录现有 LRT 的底层几何、材质、传播和渲染机制；与本次升级冲突的旧节点、收集、混合和调试约定，以新设计及本目标为准。其余现有机制保持不变。

## 节点、归属与范围

| 节点 | 行为 |
|---|---|
| `LRTGI` | 拥有子树几何；没有手动 Size，显式烘焙时自动确定有限网格范围 |
| `LRTVolume3D` | 使用显式 Size 按空间收集，不建立几何归属；用于 Global 等范围型计算 |
| `LRTDetailVolume3D` | 从属的局部精细网格，可依附 `LRTGI` 或 `LRTVolume3D` |

几何属于最近的 `LRTGI` 祖先，查找跨越普通容器和场景实例边界，不依赖 Godot 的 `Node.owner`。`LRTVolume3D` 和 Detail 不建立几何归属。空间移动不改变归属，重新挂载节点才改变归属；LRT 不自动修改节点树。

- `LRTGI` 不能在节点树上互相嵌套，但允许空间重叠。`LRTVolume3D` 不限制子节点，也不为子树赋予特殊收集含义。
- Detail 归属于最近的 `LRTGI`／`LRTVolume3D` 祖先，不能嵌套 Detail；必须相对所属者固定，范围完整内含，spacing 更小。
- 父级和所有所属 Detail 构成计算组。各网格独立保存几何与 A/L 状态；Detail 没有独立 Bake、全局 priority、intensity 或对外贡献策略。
- `LRTGI` 烘焙范围是所属参与 Static／Dynamic 几何在烘焙时的局部包围盒并集，排除 Disabled、仅接收型几何、外部贡献和 Detail 范围。按世界单位 `extend_margin` 外扩，再按 spacing 向外对齐。无可用于推导范围的几何时不能烘焙；空区域计算使用 `LRTVolume3D`。
- `extend_margin` 默认 1 m，烘焙时按正均匀缩放换算为局部距离。只在显式成功烘焙时更新范围；运行时不跟随 Dynamic 自动扩容，也不因缩放重新应用边距。Detail 不反向扩大父级范围。
- `LRTGI` Gizmo 显示最近成功烘焙的实际范围，不提供尺寸手柄；`LRTVolume3D` 和 Detail 保留显式范围及六面尺寸手柄。自动范围可能偏离节点原点，所有采样、裁剪和诊断必须正确处理。

典型组织是独立的 Global `LRTVolume3D`、各地块 `LRTGI`、常驻载具 `LRTGI`。载具不归属于地块，也不因进入地块而重新挂载。支持平移、旋转和正均匀缩放，不支持非均匀／负缩放或剪切。

## 几何参与与对外贡献

沿用原生 `gi_mode`，不新增 LRT 专用 Static／Dynamic 模式或启用开关。完整支持 `MeshInstance3D` 与 `MultiMeshInstance3D`，包括可见子实例数量、组合变换、实际材质和实例数据差异。

`LRTGI` 的 External Contribution 默认 Static=On、Dynamic=Off。物体配置同名枚举 `Inherit / Disabled / Enabled`，默认 Inherit，按当前 GI Mode 读取所属 `LRTGI` 的对应默认值；显式配置覆盖默认值。这只决定对其他组的几何贡献，不限制本组贡献或接收光照。

| 几何 | 所属 LRTGI 及 Detail | 其他 LRTGI 及 Detail | LRTVolume3D 及 Detail |
|---|---|---|---|
| 已归属 Static | 三角形精细静态烘焙 | 对外开启时 Color SDF | 对外开启时 Color SDF，不进入静态底板 |
| 已归属 Dynamic | Color SDF | 对外开启时 Color SDF | 对外开启时 Color SDF |
| 未归属 Static | 不适用 | 不贡献 | 按范围三角形静态烘焙 |
| 未归属 Dynamic | 不适用 | 有效 Color SDF 时参与 | 有效 Color SDF 时参与 |
| Disabled | 不贡献 | 不贡献 | 不贡献 |

未归属物体不使用 External Contribution。未归属 Static 不参与普通组是合法状态；未归属 Dynamic 未配置 SDF 也合法，不自动参与或警告。已配置但无效的资源则需要明确提示。不要给未归属 Static 增加向普通组贡献 SDF 的新路径。

收集限定于同一 World3D 的当前可见节点，并进行包含 Probe／Edge 采样邻域的空间筛选。同一几何在同一网格只计入一次。普通组只直接输出自己所属实例，不递归转发外部贡献。Detail 继承几何来源并按自己的网格计算，不复制、重采样或再次叠加父级 T／Edge。

Color SDF 按实际几何、材质和实例求值绑定，可在网格之间共享，不能仅按 Mesh 标识共享。它不携带源 Volume 的光照。

## 静态数据与运行时生命周期

**烘焙到哪个网格，静态数据就保留在哪个网格；只有显式烘焙才改变。**Static 节点移动、隐藏、删除、重新挂载或地块卸载，都不自动修改其他仍存在网格的静态底板，不自动失效、重烘焙或重建其静态贡献。由游戏决定烘焙时机，不限制原生节点树工作流。

同一物体向其他网格提供的 Color SDF 是运行时贡献。节点移除、隐藏、归属变化或对外贡献关闭时，撤回对应贡献；本地旧静态底板可继续保留，两者无需同步。不增加静态对外快照集合。

更新取决于 `inverse(volume_transform) × instance_transform`：源或目标移动都需要考虑，不能只监听贡献节点移动。相对关系不变则不重复重建。复用现有 8³ Probe Trunk 的旧／新影响范围标脏、有界异步合成与发布，保留持续为空气的 Probe 历史，不整网格清零，不重建移动路径之间无关区域。

沿用节点显隐控制活动状态，不加 Enabled 开关。父级隐藏或未就绪时，Detail 不独立工作；隐藏保留缓存，恢复前同步运行时变化。卸载释放自身运行时状态，撤回外部 SDF 贡献及边界来源，不保留幽灵光场；这不撤回其他网格已有的静态烘焙。

## 光照传播与表面混合

复用现有 26 邻域、SH4、A／L 双缓冲和交替阶段：A 为环境／天空可见性，L 为反弹光。三类节点使用同一传播算法，保留现有材质、灯光、阴影、天空和自发光注入方式。

- 范围内读取自身邻居。普通组出界时，从覆盖该位置、已就绪且 priority 严格更低的 `LRTGI`／`LRTVolume3D` 中选择最高 priority；同级候选按场景树顺序取最后一个。
- Detail 出界只读取所属父级，不搜索其他组；Detail 不作为其他组的边界来源。
- 同优先级组不交换 A/L，高优先级不向低优先级反馈。大尺度信息由更低优先级的 Global 等网格独立计算并向上提供。
- 无来源时 `L=0`、`A=SH_ONE`。边界读取原始传播场，Inside 零状态有效；进行 SH 空间转换后继续通过接收网格的约束，不套用表面 Inside 剔除。
- 每阶段按组 priority 从低到高执行，父级完成后执行所属 Detail，读取上游本阶段已完成状态；多视图不重复推进世界传播。

表面接收独立于 External Contribution，但受当前 GI Mode 和 LRTGI 归属限定：已归属 Static 仅接收自己的 LRTGI 组及公开的 LRTVolume3D 组，未归属 Static 仅接收公开组；Dynamic／Disabled 接收所有覆盖组，Detail 继承父组资格。逐物体共享去重的 SurfaceProfile={normal_bias,receiver_scope}，实例、预通过和 Gather 均传递 32 位精确 ID；仅同 ID 且位置／法线匹配才复用 Gather，MSAA ID 与深度／法线来自同一样本。模式／归属／bias 实时更新，无需 Bake，不改变几何贡献及 A/L 边界传播。间接漫反射覆盖顺序为有效 Lightmap／Lightmap Capture、VoxelGI > LRT > SDFGI，而不是按场景是否存在原生 GI 一刀切。SDFGI 不抑制 LRT；LRT 按有效覆盖率与包含 SDFGI 的原有间接光混合，满覆盖时替换，包括有效黑色，边缘或无有效覆盖时保留相应原有结果。此顺序不改变镜面反射或 A/L 边界传播，几何仍可参与 LRT。

所有节点使用世界米制 `blend_width`，默认 1 m，0 禁用过渡。取未施加 normal bias 的真实表面到边界世界距离 d，权重为 `smoothstep(0, blend_width, d)`，不随 spacing 或节点缩放改变。实时修改不需烘焙，不影响传播。它与 Extend Margin 独立，不自动联动。

表面使用现有世界单位 normal bias、八 Probe 插值、Inside 剔除和有效权重归一化；L/A 使用同一权重，有效黑色仍参与。先组合父级与 Detail，再组合各组：

- 组内有效父样本初始化 `(C,c)=(I_parent,1)`，无效则 `(0,0)`。有效 Detail 按场景树顺序覆盖，边缘权重 w：`C=(1-w)C+wI`，`c=(1-w)c+w`。父级 Probe 无效不能阻止有效 Detail。
- 组内结果同乘父级边界权重，颜色再乘一次父级 intensity；Detail 不扩大组覆盖范围。
- 各组按 priority 升序、同级按场景树顺序合成预乘结果：`C=(1-c_group)C+C_group`，`c=(1-c_group)c+c_group`。最终 `ambient=(1-c)ambient_original+C`。

Screen Gather、直接采样、透明路径及对应调试显示遵循同一语义。不要增加补光、传播增益、同级交换或反向反馈。

## 烘焙、编辑器与诊断

### 显式组级烘焙

`LRTGI`／`LRTVolume3D` 是组级入口，编辑器和运行时复用现有异步任务、增量求值、进度、取消、问题报告和资源保存机制。Detail 不能独立 Bake。

- LRTGI 的 Bake 准备自动范围、自身和所有 Detail 的静态底板，以及所属 Dynamic 和对外 Static 必需的 SDF。
- LRTVolume3D 的 Bake 准备自身和 Detail 的未归属 Static 底板，不替外部来源烘焙 SDF。
- Geometry Instance 保留单独 Bake Color SDF，用于局部更新或游离 Dynamic 加入 LRT。
- 父级和 Detail 使用同一次场景捕获，共用成败；失败或取消不替换整组旧资源，成功后统一提交任务结果。资源完成与异步 GPU 几何就绪分开，不要求各网格同帧完成。

### Inspector 与 Gizmo

复用 Godot 原生属性行、分组、枚举、复选框、tooltip、撤销／重做与属性重置，不做定制卡片或常驻状态面板。

- LRTGI：Extend Margin 1 m、Probe Spacing 0.5 m、Priority 0、Blend Width 1 m、Intensity 1、Bake Data，以及 External Contribution 的 Static／Dynamic 默认复选框；无 Size 手柄。
- LRTVolume3D：Size 默认 `(10,10,10)` m，其余 spacing、priority、blend width、intensity 同上及 Bake Data；无 External Contribution 默认策略。
- Detail：Size、比父级更细的 spacing、Blend Width 默认 1 m、组内 Bake Data；不提供 Owner 选择器、独立 priority／intensity／对外策略或 Bake。
- Geometry：沿用原生 Global Illumination Mode 和 Lightmap Texel Scale，在 LRT 子组添加 External Contribution。Disabled 或未归属时隐藏该枚举；Dynamic 显示 SDF 参数，已归属 Static 仅最终对外开启时显示 SDF 参数。GI Mode Disabled 隐藏 SDF 操作，但保留 surface normal bias。
- SDF Resolution 默认最长轴 32 格。normal bias 保留可选覆盖复选框和世界单位，不直接展示继承哨兵值 -1。
- Geometry 配置继续由模块内 `LRTGeometryBinding` 持有并随场景保存，支持复制、实例化、撤销／重做，不把 LRT 属性加入引擎核心 GeometryInstance。组级 Bake 沿用 3D 工具栏。

Warning 只报告明确参与后无法工作的配置：非法嵌套、Detail 关系／范围／spacing、无法推导范围、必需数据缺失、布局或 SDF 不匹配、不支持的变换。未归属 Static、无 SDF 的游离 Dynamic、对外关闭、暂不相交、隐藏和空静态底板不是错误。不因 Static 节点变动自动警告底板失效，不在各子物体重复报告父级问题。沿用 Volume 配置警告、Geometry 验证面板和烘焙任务问题报告。

### Display Advanced

固定十个入口：Indirect Lighting、Probe Sampling、Probes、Local Visibility、Global Visibility、Edge Visibility、Static LTM、Color SDF、Trunk Updates，以及新增 Volume Blending。全部使用同一目标选择规则：

- 选 LRTGI／LRTVolume3D：父级及所有所属 Detail。
- 单选 Detail：仅该 Detail，不包含父级或其他 Detail。
- 没选计算节点：当前世界所有已就绪活动组。Geometry Instance 不作为筛选入口。
- 选中对象隐藏、未就绪或缺数据时说明原因，不退回其他节点；选择不改变真实计算或正常渲染。

光照视图显示观察范围的组内／组间混合；空间视图同时显示范围内各自网格的数据，不把 Detail 合并到粗网格。LTM、LocalVis、A、Edge、Probes、Trunk 都遵守此规则。Static LTM 仍只读静态底板，LocalVis 读当前合成几何。

Color SDF 使用目标网格实际认定为 SDF 贡献的参与集合，而非选中实例的资源。父级观察范围取父级与 Detail 集合并去重，Detail 仅显示影响自己的 SDF。按实例／MultiMesh 子实例和变换区分，不能只按共享资源去重。本组静态 Body 即使有对外 SDF，在本组也不作为 SDF 显示；在 Global 中则可能作为对外 SDF 显示。合法空结果不能用 Mesh 或其他组数据填充。

Volume Blending 用组颜色、Detail 同色系明度和真实混合权重表达来源；Lightmap／Lightmap Capture、VoxelGI 优先区域灰色，SDFGI 不屏蔽 LRT 来源颜色，无 LRT 覆盖黑色，有效零光照仍着色，不受材质、光照或曝光影响。保持同样的选择范围，不把隔离结果冒充全局结果。复用既有球、Inside 品红、Visibility 灰度、Edge／Trunk 颜色和旧／新范围历史，不新增调试窗口。

## 代码背景与工程约束

仓库根目录：`C:\Users\Zykert\.herdr\worktrees\godot\master-lrt-sdf`。

现有节点入口主要是 `modules/lrt/local_lrt_volume_3d.*`；相关能力位于 `lrt_geometry_baker.*`、`lrt_material_baker.*`、`lrt_geometry_binding.*`、`lrt_data.*`、`lrt_bake_task.h`、`lrt_runtime.*`、`lrt_renderer.*`、`modules/lrt/shaders/`、`modules/lrt/editor/` 及模块注册。Forward+ 接入集中在 `servers/rendering/renderer_rd/forward_clustered/`。这些是现有能力位置，不要求采用特定新类层次或文件拆分。

- 复用现有材质 GPU 求值、三角形 BVH、Color SDF、SH4、Edge、异步 Trunk 和渲染接入，不修改无关算法或原生 GI 工作流，不引入通用空间管理器、额外反馈系统或静默降级。
- 遵循最小改动、fast-fail，保留用户现有修改；不增加旧节点名称、旧格数参数的兼容分支，更新必要调用和场景引用。
- 同步最终实现与新设计，并更新旧主设计中发生冲突的相关描述；设计文档不写任务进度或实现日志。
- 不编写单元测试。验证项目使用 `lrt_module_development/lrt_development_playground`，项目修改和 debug 均通过 Godot MCP；连接问题按项目约定重启编辑器，不用原始文本编辑替代编辑器管理的内容操作。
- 编译编辑器产物在 `bin` 只保留一版；遇到占用，保存后强制退出再编译。项目自有 GDScript 使用静态类型并遵循仓库与 Godot skill 约定。
- 视觉验证截图保存在 `lrt_module_development/screenshot/`，最终报告引用截图路径。实测、编译结果和限制必须如实报告。

## 完成与验证标准

交付完整可编译的引擎实现、必要的现有验证场景适配，以及与实现一致的设计。验证以现有工程的针对性场景、状态接口、日志和截图为证据，不用占位实现或只验证 UI 代替功能完成。

必须覆盖以下实际行为：

- 三类节点可创建、保存和重新加载；Geometry 的 Inherit、覆盖、资源绑定、撤销／重做及复制／实例化可用。MeshInstance 和 MultiMesh 都能使用本方案。
- LRTGI 自动范围正确包含烘焙时所属参与几何与米制边距，支持非原点中心；外部物体和 Detail 不扩大范围，运行时移动不自动扩容。显式范围和 Detail 内含／spacing 约束可验证。
- 移动整车保留本地精细静态底板，Global 只更新旧／新 SDF 影响区域，几何更新完成后无不可撤回的载具静态残留。默认 Box 只影响本组，Enabled Door 可影响外部。
- 源物体不动而目标组移动时，外部几何仍正确更新；显示、隐藏、重新挂载、对外策略切换和卸载能撤回运行时贡献。静态节点移除后底板保持，只有显式重烘焙改变。
- 未归属 Static 只被范围型网格静态收集；游离 Dynamic 无 SDF 时合法不参与，有效 SDF 时参与相交目标。参与不重复，跨 World3D 不串数据。
- 父级 Bake 同时准备 Detail 和必要 SDF；Detail 无独立入口，任一 Detail 失败／取消不替换整组旧资源。
- 实测确认严格低优先级向高优先级供光、Detail 只读父级、同级不交换，无来源开放边界正确。组内与组间覆盖顺序、父级 Probe 无效但 Detail 有效、有效黑色和无有效样本均遵循设计。
- 改变 spacing 或正均匀缩放后，Blend Width 仍是相同世界宽度；修改 Blend Width 无需 Bake，Extend Margin 只在 Bake 时应用。Gather、直接采样、透明路径和Lightmap／Lightmap Capture、VoxelGI > LRT > SDFGI 的间接漫反射覆盖顺序保持一致。
- 十个调试入口完整，所有视图的父级含 Detail／Detail 单选规则一致。Color SDF 确实反映目标网格的参与集合，Static LTM 与合成 LocalVis 含义正确；空结果不冒用数据，选择不改变正常计算。

编译必须通过，实际场景不出现新增脚本、渲染或资源错误。使用现有性能统计确认局部更新与有界调度仍成立，不引入无关的全场景扫描、全网格重烘焙或高频同步等待；无需增加新的性能系统或另设性能目标。完成报告简述实现范围、实际验证结果、截图证据及尚存限制。
