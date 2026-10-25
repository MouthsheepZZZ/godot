# Project Milestones

方案依据：[Local LRT GI](lrt_module_development/local_lrt_design.md)。

- [x] 模块与编辑器基础：Volume、实例绑定、烘焙资源及全局设置可用。
- [ ] 几何烘焙：静态 Volume 缓存与实例 Color SDF 正确生成并保存。
- [x] 动态几何更新：Trunk 预算调度、Edge 遮挡及缓存批次发布支持实例移动、加入和移除。
- [x] GPU 光照：灯光、阴影、天空和自发光注入及 A/L 传播已接入，已完成源项数值对照。
- [ ] 首个 GI 视觉闭环：Cornell Box 点光首版已通过正式的直接 Probe 采样显示并截图；天空/自发光及动态遮挡的扩展视觉验收仍待完成。
- [ ] 多 Volume 协同：支持局部空间变换、外层到内层边界输入与优先级混合。
- [ ] 渲染融合：Screen Gather、引导升采样和直接 Probe 采样接入 Godot 间接漫反射。
- [ ] 调试与验收：调试视图和性能统计可用，测试项目完成视觉、动态更新与性能验证并留档。

## Current

当前结果：Cornell Box 已能看到第一版本 GI。已先补齐灯光、阴影、天空和固定自发光的完整源项，再接通 Forward+ 正式直接 Probe 采样与材质间接漫反射；不是调试着色器、环境补光或自发光替代。当前交付范围为单 Volume、不透明材质的首版表现，不代表完整 LRT 视觉或性能验收。

运行入口：打开 `lrt_module_development/lrt_development_playground/src/scenes/cornell_box/cornell_box.tscn` 后按 F6。场景已保存 `LRTVolume` 和 `cornell_lrt.res`；项目已开启 `rendering/lrt/enabled`，并显式设置 `rendering/lrt/screen_gather_enabled=false`。保持原有点光、材质、相机、曝光和零环境光；Probe spacing=0.25 m，25×21×25=13,125 个 Probe，Volume 网格采用半格偏移，避免接收面恰好落在 Inside Probe 平面上。

截图：[关闭](lrt_module_development/screenshot/cornell_lrt_v1_off.png)、[开启](lrt_module_development/screenshot/cornell_lrt_v1_on.png)、[同机位对照，左关右开](lrt_module_development/screenshot/cornell_lrt_v1_off_on.png)。原图各 1440×900；矮方块阴影面采样像素由 RGB8=(0,0,0) 变为 (32,28,25)，有偏红的间接受光。关闭点光并等待传播消退后，该像素恢复为黑色。首版间接光仍有明显的低分辨率插值分块，尚未进行画质调优。

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

### 3. 补齐完整方案与性能验收

实现外层到内层的多 Volume 边界与优先级混合，完成 Screen Gather、深度/法线引导升采样及其与直接 Probe 采样的质量对照，补齐透明表面等剩余融合路径，最后完成调试视图、性能统计和列车动态场景验收。现有未完成里程碑不删减、不提前勾选。

## 已有进展与验证记录

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

测试项目原有 `lrt_spacing_diagnostic.gd` 引用了不存在的 `LRTVolume3D` / `LRTVolume` 类型，仍有解析错误，未在本轮修改。

Cornell Box 第一版本 GI 的本轮目标已达成。下一步针对首版插值分块、天空/自发光及动态遮挡完成扩展视觉验收，再补齐 Screen Gather、多 Volume 和性能验收。高级材质未支持路径继续保留显式报错，不视为已完成。
