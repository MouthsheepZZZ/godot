# Project Milestones

方案依据：[Local LRT GI](lrt_module_development/local_lrt_design.md)。

- [x] 模块与编辑器基础：Volume、实例绑定、烘焙资源及全局设置可用。
- [x] 几何烘焙：不透明 Standard/ORM 和 ShaderMaterial 通过原生 GPU 求值生成静态缓存与 Color SDF；透明仅接收 GI，完成材质、独立渲染线程和真实车厢验证。
- [x] 动态几何更新：Trunk 预算调度、Edge 遮挡及缓存批次发布支持实例移动、加入和移除。
- [x] GPU 光照：灯光、阴影、天空和自发光注入及 A/L 传播已接入，已完成源项数值对照。
- [x] GI 视觉闭环：Cornell 点光反弹、天空遮挡、有色自发光、动态实例移动和真实车厢均已完成材质画面对照及截图。
- [x] 多 Volume 协同：严格按 priority/稳定 ID 更新，外层原始 L/A 向内层供给并旋转 SH，Inside 零状态有效；完成边缘混合和优先级对照。
- [x] 渲染融合：Screen Gather、深度/法线引导升采样和直接 Probe 采样接入材质；透明及不匹配像素直接采样。
- [x] 调试与验收：20 项 LRT 视口模式、实例/Probe/切片检查及 CPU/GPU/内存统计已接入；车厢动态和性能验证已留档。

## Current

当前结果：多 Volume 边界、Screen Gather/引导升采样、透明接收、调试视图和统计已接入，Cornell 与真实车厢完成视觉、动态和性能验证。已按用户确认的范围完成：不透明 StandardMaterial3D/ORMMaterial3D 与 ShaderMaterial 使用原生 GPU 材质求值，透明仅接收 GI；全部里程碑已完成。几何、材质与视角/时间依赖保持烘焙快照，不扩展为运行时材质重烘焙或透明输运。

运行入口：打开 `lrt_module_development/lrt_development_playground/src/scenes/cornell_box/cornell_box.tscn` 后按 F6。项目已开启 LRT 和默认半分辨率 Screen Gather。Cornell 保存内层 0.25 m 网格（13,125 Probe）与外层 0.5 m 网格（4,335 Probe），共 17,460 Probe；矮方块已改为 Dynamic 并保存独立的 `cornell_short_sdf.res`，静态缓存已重烘焙以排除该实例。点光、材质、相机、曝光和零环境光保持原值。车厢入口为 `res://src/scenes/carriage/carriage_noGI.tscn`，当前静态缓存包含场景中已有的固定自发光。

主要截图：
- [ShaderMaterial 程序化自发光的原生材质 GI：左关右开](lrt_module_development/screenshot/cornell_shader_material_off_on.png)、[原生 GPU 材质重烘焙后的真实车厢：左关右开](lrt_module_development/screenshot/carriage_native_material_off_on.png)。
- [动态 Cornell：左关右开，上为原位、下为移动后](lrt_module_development/screenshot/cornell_dynamic_off_on.png)。
- [天空遮挡：左关右开](lrt_module_development/screenshot/cornell_sky_off_on.png)、[蓝色自发光反弹：左关右开](lrt_module_development/screenshot/cornell_emission_off_on.png)。
- [直接采样与半分辨率 Gather](lrt_module_development/screenshot/cornell_direct_gather.png)、[透明表面接收：左关右开](lrt_module_development/screenshot/cornell_transparent_off_on.png)。
- [嵌套混合与交换优先级](lrt_module_development/screenshot/cornell_nested_priority.png)、[车厢 GI：左关右开](lrt_module_development/screenshot/carriage_lrt_off_on.png)。
- [调试视图](lrt_module_development/screenshot/lrt_debug_views_initial.png)、[Color SDF](lrt_module_development/screenshot/lrt_debug_color_sdf.png)、[距离切片](lrt_module_development/screenshot/lrt_debug_color_sdf_slice.png)。

当前为 SH4、规则粗网格的低频方案，仍能看到明显的插值分块，不代表路径追踪品质。只验证了本机 Windows/D3D12/RTX 5080 的 Forward+；没有宣称其他后端、VR 或任意材质均已验收。

## 执行顺序：完整光照 → GI 视觉闭环 → 完整方案验收

### 1. 完成 GPU 光照处理（已接入并验证源项）

接入真实灯光、阴影及天空，形成 Ddirect、可见天空 B 和完整源项 S，完成设计要求的传播 A → 注入灯光和天空 → 传播 L 调度。覆盖方向光、点光、聚光的衰减与阴影处理，以及灯光开关、移动、天空变化和动态遮挡引起的输入更新；保留已有自发光 J。先验证源项与传播数值正确，再进入材质融合，不将缺失的输入用常量或临时实现替代。

### 2. 首个可见 GI：完整光照、单 Volume、直接 Probe 采样（Cornell 首版已出图）

- 复用方案已有的 `screen_gather_enabled=false` 正式路径，读取 GPU 最新 L/A 历史，打通 Forward+ 的资源绑定、Volume 空间变换和材质采样；不是临时调试着色器或单独的 GI 贴图展示。先验证单 Volume 的不透明接收表面，不将其他渲染器、透明表面或多 Volume 的完整支持作为首次出图的前置条件。
- 按设计插值八个有效 Probe，排除 Inside 并归一化有效权重；正确处理表面法线方向、边缘 coverage 和 Volume intensity。使用完整光照传播 L 与可见天空 B，按设计重建 irradiance/π，接入 Godot 的间接漫反射位置，由原材质处理颜色和 AO；不重复乘材质颜色、不叠加到自发光或直接光。
- 保持模块关闭及 Volume 覆盖外的原渲染结果；没有有效 Probe 时 coverage=0，遵循设计的融合规则。直接采样路径长期保留，后续作为 Screen Gather 的功能路径和质量对照，不做用后丢弃的临时实现。
- 通过 Godot MCP 设置受控场景，分别验证灯光反弹、天空照明及有色自发光对非自发光接收面的影响，排除其他 GI 对判断的干扰。验证间接照明、颜色传播与遮挡后，再到真实车厢验证。
- 首次视觉验收必须留存同机位、同曝光的 LRT 关闭/开启截图；源项移除后应消退，动态遮挡物移动后应改变传播与受光。截图保存至 `lrt_module_development/screenshot/`，回复引用路径。以真实材质上的差异为验收依据，数值回读或 Probe 调试颜色不能代替 GI 画面。
- 完整光照处理是该里程碑的前置条件；首次出图不等待 Screen Gather 或多 Volume 完成。完成即报告首个可见 GI，明确它是单 Volume 视觉验证，不宣称完整方案验收。

### 3. 补齐完整方案与性能验收（主体已完成，记录见下）

实现外层到内层的多 Volume 边界与优先级混合，完成 Screen Gather、深度/法线引导升采样及其与直接 Probe 采样的质量对照，补齐透明表面等剩余融合路径，最后完成调试视图、性能统计和列车动态场景验收。现有未完成里程碑不删减、不提前勾选。

## 已有进展与验证记录（以下为早期阶段历史记录）

几何烘焙状态：静态缓存与动态 Color SDF 均已验证生成及保存重载，真实车厢完成 81,925 个 Probe 的静态烘焙并留档；已补齐 Detail 颜色层，支持 Mix/Add/Subtract/Multiply、UV1/UV2、UV1 遮罩及金属度漫反射衰减。现已支持 UV1/UV2 的局部/世界 triplanar 投影、缩放偏移、顶点混合锐度和无 UV 网格，覆盖颜色、金属度/ORM、自发光及 Detail。世界投影颜色固定为烘焙时的快照，移动后不会重新采样世界贴图。ShaderMaterial、透明和额外材质 pass 等路径仍未支持，暂无 GI 渲染结果。

验证记录：
- Detail：UV1/UV2 × 四种混合的 Color SDF 保存重载，每组 216 个样本颜色字节与预期一致；四种混合的静态 Volume（每组 125 个 Probe）与等效纯色材质的 LTM、LocalVis、Edge 数据逐字节一致。
- Triplanar：开发版编辑器编译通过；Godot MCP 验证 24 组材质/世界投影/锐度组合，每组 216 个 Color SDF 样本保存重载后，颜色字节及自发光数据与预期完全一致。六组轴向/局部和世界投影，含非零缩放偏移及实例平移、旋转、均匀缩放，与等效平面 UV 的 Color SDF 颜色、静态 Volume LTM 和自发光结果完全一致。另以等效顶点颜色验证平滑法线的顶点权重插值，512 个样本最大颜色误差为 1 byte（参考顶点颜色的 RGBA8 量化）。
- 失败路径：缺失 triplanar 顶点法线时返回 ERR_INVALID_DATA，离树实例请求世界投影时返回 ERR_UNCONFIGURED；均保留原烘焙资源及文件（SHA-256 不变）。局部投影仍支持离树实例烘焙。
- 以上仅为数值验证，未做 GI 视觉验收。

动态几何进展：
- 已接入 LRTRuntime 生命周期及 Volume/Binding 进出场景树注册；CPU worker 按全局预算轮询提交 8³ Probe Trunk，稳定排序实例，从静态底板重建 T/V/Inside/J/Edge。静态烘焙和动态重建复用同一套四子线段 Edge 坐标。
- 完整批次完成后原子替换 CPU 缓存并发出 `geometry_updated`；通过 `get_geometry_status()` / `get_geometry_trunk()` 查询状态和已发布数据。在途任务期间发生的后续变化保留为下一批脏块。远处无效绑定不再阻塞无关 Volume。
- Godot MCP 数值验证：两个 Volume（各 17×5×5=425 个 Probe）共享每帧 1 个 Trunk 的预算；覆盖移动、连续移动、移回、移除、重新加入、GI Mode 切换、烘焙资源变更，以及空/非空静态底板。逐帧检查共享 Edge 错误数和同版本缓存改写数均为 0，每帧最大提交数为 1；移除后 LTM/LocalVis/Edge 与静态资源逐字节一致。Probe 中心位于 SDF 域外时仍能获得邻域贡献。
- 父节点整体平移至百万单位坐标并旋转、均匀缩放，局部缓存和发布版本均不变。缺失 SDF 的绑定进入范围会显式阻塞，移出后恢复；任务在途时 Volume 离树、重新入树可正常回收并重建。开发版编辑器编译通过。以上仍为 CPU 数值验证，没有 GI 视觉结果。

GPU 几何进展：
- `LRTRenderer` 将完整批次打包为一次 staging 上传，在渲染线程通过 compute scatter 更新 T/V/Edge/J，并将受影响 Probe 的两个历史缓冲重置为 L=0、A=V（Inside 为零）；未涉及的 Probe 不写入。停用、离树、CPU 几何阻塞时回收 GPU 资源；不支持 RenderingDevice 时显式报告 GPU blocked。
- `get_geometry_status().gpu` 提供非阻塞的已提交版本及内存统计；`get_geometry_gpu_trunk()` 提供仅供诊断的同步 GPU 回读。每 Probe 逻辑常驻数据 248 B（72 B 对齐几何＋48 B 固定源＋128 B 双历史），不含临时 staging、着色器和驱动分配开销。
- Godot MCP 在默认线程模型和独立渲染线程模式分别验证 24 组、72 个 Trunk 回读，覆盖移动、连续移动、移除、重新加入、资源变更、错误恢复及离树重入；几何、自发光、版本与两个历史缓冲的初值错误均为 0。独立线程验证在主线程 process_frame 中等待 CPU 更新；测试结束已恢复项目线程设置。
- 真实车厢 25×29×113=81,925 个 Probe 已完成 GPU 初始化及 4 个动态 Trunk 更新，逻辑常驻 GPU 数据 20,317,400 B；首/中/尾 Trunk 回读的几何、自发光、版本和历史初值均与 CPU 一致。开发版编辑器编译通过；仍为数值验证，没有 GI 视觉结果。

GPU 传播进展（仅自发光阶段的历史记录）：
- 新增 `lrt_propagate.glsl`，每帧在已就绪 Volume 上先迭代 A，再迭代 L。按设计实现 26 方向 SH4 Trpd、共享 Edge 权重、LTM 列主序解码、Inside 清零、FP32 Jacobi 双缓冲及松弛更新；依赖 pass 之间插入 barrier。该阶段 L 的源项仅为固定自发光 J，边界仅为开放边界 A=SH_ONE、L=0，未把尚未实现的灯光/天空或外层 Volume 输入计入已完成范围。
- GPU 状态及回读增加传播帧、累计 pass 数和最新 history_index。几何变更重置受影响 Trunk 的两个缓冲后，立即参与后续全 Volume 传播；其余 Probe 保留之前的传播历史。
- Godot MCP 在默认及独立渲染线程模式各完成 5 组 CPU 参考公式对照（27 Probe，开放空气/全遮挡/方向性 LocalVis/非对称 LTM/固定 J；每帧 1、2、3 次迭代，松弛 1、0.5、0.25）。两个缓冲最大绝对误差 4.77e-7，无非有限值，ping-pong 索引一致。
- 默认及独立渲染线程均通过 153 Probe、3 个 Trunk 的连续移动/移出验证：保留非零历史，3 次仅重置 1 个 Trunk；两个 GPU 历史与逐帧 CPU 参考的最大绝对误差 4.77e-7，未重置区域仍保有非零光照。迭代次数设为 0 会显式 GPU blocked，重新启用后完整重建恢复，误差 4.77e-7。测试后已恢复线程设置。
- 真实车厢 81,925 个 Probe 完成 60 个 A/L pass，抽检首/中/尾及全部 4 个动态更新 Trunk（共 7 个 Trunk），两个历史均无非有限值，Inside 状态清零错误数为 0。开发版编译、格式及 XML 检查通过。以上仍为数值验证，尚无 GI 画面融合结果。

完整光照与 Cornell 渲染进展：
- Forward+ 阴影完成后按世界每帧一次执行 A → 注入 S=T(Ddirect+B)+J → L。适配层收集世界内 Static/Dynamic 的 Directional、Omni、Spot 灯光，使用线性未曝光强度、indirect energy、距离/锥角衰减和现有影图；点光/聚光在缺少当前有效阴影数据时不注入，方向光影图覆盖外使用 A。Environment 颜色、能量、Sky contribution、实际天空辐亮度贴图和天空旋转参与 Sky SH 投影。
- 正式直接 Probe 路径读取最新历史，插值八个有效 Probe，排除 Inside 并归一化权重，以 L+B 重建 irradiance/π，在原材质颜色与 AO 之前按 coverage/intensity 融合。没有 GI 屏幕贴图或额外自发光材质；模块关闭恢复原渲染。Screen Gather 尚未实现，启用时显式 GPU blocked；Mobile/Compatibility 也显式阻塞，不静默替换路径。
- 实现保留在 `modules/lrt/`；引擎接入补丁仅桥接场景/灯光、阴影后的调度、固定着色器资源绑定与间接漫反射入口。每 Probe 传播数据现为 296 B（新增 48 B 源项 S），显示缓存另有每 Probe 136 B 的世界共享 GPU 几何/历史副本、每 Volume 128 B 元数据及小型头部/Sky 数据；此前记录的 248 B 为仅几何/J/历史阶段数据。
- Godot MCP 在默认和独立渲染线程分别验证 6 组源项参考：Omni、indirect energy、Spot、Directional、环境颜色、全输入关闭；另验证点光移动和固定 J 叠加。512 Probe 的完整 S 系数与 CPU 解码公式最大绝对误差 2.05e-7，无非有限值。三种灯光启用影图后均同时出现受光和遮挡 Probe，σ 比例落在 [0,1]，异常数为 0。
- 使用真实渐变 PanoramaSkyMaterial 验证 Sky SH：天空旋转 90° 的系数旋转最大绝对误差 3.77e-4（256 个方向投影的采样误差）；Sky contribution 减半及背景能量加倍的系数误差均为 0。以上为数值验证，不冒充天空视觉验收。
- Cornell 持久场景重启后可直接运行；最终同机位截图及点光关闭后的像素回读验证通过，固定自发光回读为零。抽检源项和双历史无非有限值，开发版编译通过；线程设置已恢复为默认模式 1。尚未完成动态物体遮挡视觉验收、透明材质专项验证、Screen Gather、多 Volume 边界和性能验收。

## 最新研发与验收记录

- 多 Volume：同世界先按 priority、再按稳定实例 ID 从外到内更新。出界邻居按世界位置选择最高的严格较低优先级供给，三线性插值保留 Inside 零状态并旋转 L/A 的一阶 SH；不使用显示 intensity/coverage。每个外层完成后立即将新历史复制到共享场，后续内层读取本帧结果。两种渲染线程均完成同位、旋转/均匀缩放和同优先级排除的 27 Probe CPU 对照；最大绝对误差 1.49e-8，非有限值为 0。专门以反向创建顺序验证 priority 不是实例创建顺序。Cornell 边界检查识别到 9 个供给邻居，材质画面验证嵌套混合和优先级交换。
- Gather：每个低分辨率样本的深度/法线来自同一个真实表面像素；记录预乘颜色/coverage 与 FP32 世界位置/打包法线，以深度平面距离和法线引导升采样。支持 1、0.5、0.25，透视/正交投影、MSAA 预通过解析、视口尺寸变化。透明 pass 明确绑定空 Gather Guide，直接走 Probe 路径；不匹配像素也直接采样。相机曝光仅在每视图缓存或直接结果上应用一次，避免未曝光 HDR 在 FP16 缓存溢出。
- Gather 验证：单 Volume Cornell 的 1/0.5/0.25 与直接采样 RGB8 最大差异分别为 1/8/16，抽样平均差异 0.029/0.088/0.110 byte。266 个透视 Guide 点重投影最大误差 0.000126 pixel，183 个正交点为 0.000137 pixel；独立线程验证通过。640×360 MSAA4 子视口可渲染并调整到 513×287；同世界两个视图的 20 帧只增加 80 个 A/L pass，没有重复传播。独立 World3D 的红色环境与主世界灯光/天空相互隔离，移除后主世界仍 ready。
- Sky：发现原 Godot 天空缓存可能烘入首次相机曝光，直接复用会改变未曝光源项。新增按需的未曝光 Sky 材质输出，复用原有 quarter/half/full 天空管线，保持原 Godot 缓存不变；使用 FP32 辐亮度避免 HDR 截断。两种线程中，相机曝光 0/1/2 的 Sky SH 完全一致，背景能量倍增严格倍增；100,000 辐亮度输入无非有限值。自定义 SkyShader 的 quarter/half 子 pass 在曝光 2 下输出正确的未曝光系数。
- 动态 Cornell：矮方块已使用独立 Color SDF；从静态缓存中移除原静态贡献并保存重烘焙。移动后发布版本 2→3，最多每帧提交 8 个 Trunk，传播场系数最大变化 2.249；同机位关闭/开启、移动前/后的材料截图显示间接受光随位置从偏红转向偏绿。受控天空与蓝色固定自发光均已完成 GI 开关对照；透明接收面的中心像素由黑色变为 RGB8=(50,51,31)。
- 车厢：按当前场景重新保存静态缓存（包含原有固定自发光）；81,925 Probe 的传播缓冲为 24,249,800 B，共享显示数据为 11,142,040 B。整体平移到百万坐标、旋转并均匀缩放后，缓存版本与累计 CPU 重建时间不变。相对移动实例使版本 2→3、每帧最多提交 6 个 Trunk；首/中/尾回读的 S、双历史 L/A 无非有限值。GI 开关及动态移动截图已留档。
- 调试：Display Advanced 中增加 20 项 LRT 模式，覆盖 Volume/Probe、26 几何采样、Color SDF、LTM、LocalVis/GlobalVis、Ddirect/B/S/L/最终间接光、Edge 四条子线段、Trunk 状态及旧/新范围、输入深度/法线、低分辨率/升采样/coverage 和直接采样对照。Inspect LRT 控制选中对象、Probe、局部入射方向和切片；几何查询为显式刷新快照。GPU 场在 GPU 上绘制，不将诊断回读用于正常渲染。
- 统计：非阻塞状态增加传播提交 CPU 时间、异步 GPU 时间及来源帧、每视图 Gather 时间/内存、共享显示与 Sky 缓存、可选直接光调试内存；视口原有 Information 面板叠加 LRT 信息。GPU 时间由 RenderingDevice timestamp 提供，不以 CPU 回读等待时间冒充 GPU 时间。
- 性能：Windows/D3D12、RTX 5080、开发构建、车厢 81,925 Probe、1440×900、每场每帧 2 次迭代、松弛 0.5、180 帧样本。半分辨率模式下传播/注入 GPU 中位数 243.456 µs、P95=251.136 µs；Gather 中位数 75.776 µs、P95=78.080 µs；这两部分中位数合计约 0.319 ms。CPU 提交中位数 452 µs。统计不包含材质融合、额外法线预通过和 Sky 更新，不是完整 GI 整帧增量；三种模式均受约 120 Hz 帧节奏限制，不能据此宣称帧率提升。
- HDR 与增量构建：每视图 Gather 在 FP16 存储前应用相机曝光，直接路径在材质侧应用，均仅一次；以背景能量 50,000,000、相机曝光 1e-7 验证缓存抽样无非有限值，scale=1 与直接采样 RGB8 最大差异 1、均差 0.024 byte。补齐模块 GLSL include 对自身内核及 Forward+ 生成头的 SCons 依赖，避免修改模块采样函数后仍链接旧着色器。
- 清理：通过 MCP 移除没有引用、使用旧 LRT 类型的临时 `lrt_spacing_diagnostic.gd`（移入系统回收站）；移除测试项目旧 `rendering/global_illumination/lrt/*` 设置。未添加单元测试文件，所有场景改动、数值与视觉检查均通过 Godot MCP。线程设置已恢复为 1，Gather 默认开启。

## 原生材质烘焙最终验收

- 删除 CPU 材质解释与功能白名单，统一复用 Forward+ 原生材质 vertex/fragment 管线；独立三角形图集不覆盖 UV2，保留自定义属性、原始 VERTEX_ID、材质/实例/全局参数、不透明 next_pass 与 overlay。颜色和自发光采用 FP32 输出，顶点输出进入 BVH；不经世界/相机浮点往返的普通对象空间路径保留细薄几何精度。小三角形最少覆盖 2×2 texel，防止真实车厢管线等细小面在光栅化时消失。
- 默认与独立渲染线程均通过原生 ShaderMaterial 的常量颜色/金属度/自发光、UV/UV2/CUSTOM0、实例/全局参数和 VERTEX_ID 对照：常量及属性颜色误差为 0 byte，原始 ID 与顶点颜色参考最大差异 1 byte（颜色量化）。vertex() 位移正确进入 SDF；默认材质、离树实例及 StandardMaterial 高度贴图/自发光验证通过。
- 不透明 next_pass 和 overlay 输出正确的覆盖颜色与自发光；透明附加 pass 不改变基础缓存。全透明材质即使输出 100,000 自发光，静态 LTM/V/Edge/J 仍与空场景逐字节一致；动态资源标记 receiver_only，非均匀缩放的透明实例不会阻塞 Volume，隔离场景 cached_trunks=0。
- 两种线程均完成 ShaderMaterial 与等效 StandardMaterial 的静态 Volume 对照：LTM/V/Edge 逐字节相同，自发光最大差异 9.54e-7；整体平移到百万坐标、旋转和均匀缩放后，缓存逐字节不变。
- Cornell 内外 Volume 与矮方块 SDF 已用新路径成功重烘焙；真实车厢 81,925 Probe 亦成功重烘焙并运行至 GPU ready。ShaderMaterial 条纹自发光与真实车厢的 GI 开关截图已留档。首次材质 shader 编译和车厢全量烘焙可能超过 MCP 的 10 秒 eval 窗口；已等待原生烘焙完成并确认返回 OK，不将工具超时误判为成功或连接故障。

## 已确认的材质范围

用户确认：不透明 StandardMaterial3D/ORMMaterial3D 和 ShaderMaterial 全支持，透明暂时仅接收 GI。烘焙改为执行原生 Forward+ 材质 shader，冻结当前几何和材质输出；不实现运行时材质编辑或透明输运。透明/裁切/折射 pass 不进入静态缓存与动态遮挡，保留其原有接收路径。
