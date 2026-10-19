# LRT 重建方案：光照注入

输入格式与局部核见 [扩散前准备](pre_propagation.md)，消费者见 [辐射传播](propagation.md)。本阶段只生成已完成首跳反射或首次发射的总 Source，不维护逐灯光照场。原生数据通过 [LRT 适配器](engine_integration.md) 交接，设置及诊断见 [设置与调试](settings_and_debug.md)。

## 1. 唯一注入链路

```text
整个 Volume 的候选灯
    → Volume 方向光阴影／原生局部灯阴影更新完成
    → 对有效源项 Probe 评估入射光 SH
    → 紧凑 LTM 完成一次反射
    → 加入 EmissionSH 和 SkyBounce
    → 覆盖写入总 Source
```

```text
IncomingDirectSH = Σ 光源入射贡献
SourceDirect_c = rho_c × G × IncomingDirectSH_c
Source_c = SourceDirect_c + EmissionSH_c + SkyBounce_c
```

- 使用反射 G 和 rho，不使用包含通行的完整传输算子。
- 直接入射光不提前乘 albedo 或表面 Lambert `N·L`，避免与 G 重复着色。
- 传播用的固定系数 k 不缩放 Source，也不参与首跳。
- Source 在每轮传播中直接加入，不再次经过 G。
- 刷新时重新合成总结果，不向旧 Source 持续追加。

## 2. 整个 Volume 注入，不按相机视锥筛选

有效计算范围包含局部支持和边界所需区域。灯在 Volume 外，只要照明范围进入该区域，仍然是候选。

- 方向光统一处理，不登记到每个 Trunk。
- 点光、聚光和面光按保守影响范围登记到 Trunk。
- Probe 读取 Trunk 的短列表，再做实际距离、光锥等判断。
- 排除禁用或没有间接光贡献的灯。
- 不保存逐 Probe 灯列表，也不每次扫描全部场景光源。Trunk 候选列表只在网格、有效灯种类／槽位、局部灯位置或影响半径改变时重建并上传；颜色、能量及阴影参数变化复用列表。传播参数上传不执行灯光准备。

有反射或发光贡献的位置参与源项更新；普通空域 Source 为零，由扩散取得光照。局部记录索引同时支持纯发光位置，不能只按 rho 非零筛选。

候选灯为空时仍必须发布 EmissionSH 和有效的天光反弹，不得提前退出整个源项更新。

## 3. 原生照明输入与宽 SH 投影

位置、光方向、阴影投影分别转换到正确空间，SH 使用 Volume 局部坐标。方向基函数和固定投影核沿用准备方案的 Y、P、K。

对方向可近似为单一方向 d 的灯，纯入射量按以下职责处理：

```text
光源颜色与单位换算
    × 能量、间接能量、温度、符号
    × 距离与光锥衰减
    × 原生投影纹理等调制
    × 对应光源的阴影可见度
    → 入射幅度
    → 使用 P(d) 投影到 IncomingDirectSH
```

投影使用固定宽核，不在后面再加另一套“方向修正”。负光源沿用原生符号作为源项差值，不通过提高矩阵增益或逐系数截断实现。

面光提供与其形状、能量归一化相符的低频入射近似；不能直接把已经含接收表面余弦的原生 LTC diffuse 结果再送进 G。具体引擎接口和单位换算需要在实施时对照原生路径验证，不另设隐藏 GI 亮度补偿。

接受 LRT 内的接收层混合：同一 Probe 的平均材质响应不再保持逐物件 light cull mask 的精确隔离。直接光仍完全遵循 Godot 原生规则。

## 4. Volume 方向光阴影与原生局部灯阴影

方向光使用 LRT 自己维护的低分辨率深度图，不再采样主相机的原生级联。每个 Volume 的每个开启阴影的方向光占一个 2D array layer，默认 512×512、D32F，约 1 MiB；256² 约 256 KiB。没有此类灯时仅保留 1×1 占位层。

LRT 用 Volume 接收范围沿光源方向挤出的凸体收集原生场景遮挡物，包含 Volume 外影响区域及 GI_MODE_DISABLED 几何，过滤不可见、Shadow Off 和 caster mask 不匹配的实例。正交投影覆盖完整 Probe 网格，深度范围包含实际遮挡物包围盒。已变形网格在绘制前更新原生 mesh instance；阴影不等待 LRT 粗几何重新构建。

绘制调用新增的 Forward+ C++ 接口 `RenderForwardClustered::render_shadow_depth()`。该接口完整封装已有 shadow begin／append／process／end，复用原生 Shadow shader、Alpha Scissor／Hash、discard、骨骼、顶点变形、镜像和双面投影规则。LRT 不维护体素阴影绘制、遮挡三角网格、材质变体、额外 Viewport 或颜色附件。SHADOWS_ONLY 直接进入遮挡列表；光源的 reverse cull face 同样生效。原生 CSM 和 Atlas 的覆盖、分配及已有绘制路径不变。

相机运动、颜色、强度和间接能量变化不重绘方向光图；视角相关材质仍按其真实输入失效。太阳方向、遮挡物／骨骼／顶点或材质参数、caster mask、Volume 网格及分辨率变化时刷新；材质资源编辑采用保守失效，即使只改 albedo 也可能重绘。Overlay 及其 Next Pass、贴图、Shader 参数和 TIME 与表面材质同样参与失效检查，包括 GI_MODE_DISABLED 遮挡物。ToD 每次方向变化直接更新，不加角度阈值、历史补偿或分帧滞后。

采样和投影都在 Volume 局部空间，沿用原生 reverse-Z：near=1、far=0、清零深度，nearest 比较 `receiver_depth + bias >= blocker_depth`。独立 bias 默认 0.02 个 Volume 局部米，在比较前除以实际深度跨度，不复用原生方向光的 bias、normal bias、级联距离或淡出参数。阴影细节由原始场景几何与深度图分辨率决定，不再受 LRT voxel 精度限制；场景实例的包围盒仍须包含 shader／骨骼形变。

接口只允许在渲染线程、没有未完成 shadow batch／draw list 的位置调用。LRT 在原生阴影批次结束后的既有输入交接点调用，随后注入 Source；不重入整个 render_scene，不重置主视图 UBO 分配器和主渲染列表。RenderingDevice 管理深度写入到计算采样的依赖。D32 管线需求在首次调用后保留，避免项目原生阴影均为 16 位时反复切换预编译格式。

点光、聚光及面光继续复用原生阴影和对应 atlas／投影输入。关闭阴影的灯按无遮挡注入；分配或绘制失败时报告失败，不回退到主相机方向光 atlas。

## 5. Probe 中心采样与更新触发

第一版在 Probe 中心评估入射及阴影，不维护逐表面 receiver。薄墙和墙角的受光位置误差是这一近似的主要风险。

只缓存总 Source，不缓存每灯空间结果。输入未变时复用；需要刷新时合成全部当前贡献。

| 输入变化 | 处理 |
| --- | --- |
| 局部灯移动或范围改变 | 重算旧、新影响范围的总 Source |
| 局部灯颜色、能量或投影参数改变 | 重算其影响范围 |
| 灯关闭或删除 | 重算原影响范围，不能只追加新灯贡献 |
| 方向光方向或 Volume 阴影改变 | 刷新整个 Volume 的有效源项位置；主相机级联变化不触发 |
| 局部几何或反射材质改变 | 使用新局部记录重算相应 Source；原生遮挡物或材质输入改变时同时重绘方向光阴影并全域刷新 Source |
| emission 改变 | 更新 EmissionSH，再重算对应 Source |
| 天光可见性或环境天空改变 | 更新相应 SkyBounce，再合入 Source |

阴影变化可能影响远处接收位置，刷新范围不能局限于遮挡物所在的几何脏 Trunk。不要为寻找精确阴影轮廓增加另一套追踪。

局部 G、rho、EmissionSH 和源项索引按一致构建版本发布。独立天光使用最近一次完整发布的可见性结果，允许其更新滞后，不读取半完成求解状态。

Source 发布后通知辐射场有新输入，由传播调度续充预算；不会直接触发多次重复注入。

不保留动态物体固定帧间隔设置，每帧收集并合并最新脏输入，不要求捕获或构建当帧完成。节点隐藏时停止该 Volume 的输入准备和注入，恢复时核对最新输入版本。原生交接点只提供所需灯光、阴影和就绪信息，不承担 LRT 源项业务。

## 6. EmissionSH：构建一次，合入总 Source

```text
实例最终线性 HDR emission
    → 局部粗表面与发光朝向
    → 局部可见性、方向覆盖权重
    → Probe RGB SH4 EmissionSH
```

具体公式和归属见准备方案。它与 G 和掩码共享粗几何和构建入口，但表示表面自身发出的辐射。

- 不乘外部光照、albedo、G 或光源 shadow map。
- 不使用 `Diffuse > 1` 的矩阵反馈增益。
- 不把 emission 编码为 RGB8，不钳制为 `[0,1]`。
- 只分配到局部范围，远处由统一扩散传递。
- 不创建 MeshLight 对象、逐发光体灯列表、专用阴影或另一轮传播。
- 已完成局部遮挡判定的 EmissionSH 不在注入时再次按灯光阴影压暗。

没有外部光源和天光时，Source 仍等于 EmissionSH。无需隐藏底光或随机初始光场。

仅发光参数改变时，不重建 G 或通行位；移动、变形、裁剪或遮挡变化时，重建旧、新局部影响范围。关闭和删除后重新合成剩余贡献，清除旧源项，不从传播历史反向扣除能量。

EmissionSH 是 Source 的构建输入，不是逐灯动态结果。新增成本是局部变化时的投影、必要存储及源项刷新时的读取和相加，不增加每轮扩散的 emission 评估。

## 7. 天光反弹与接收开关

每个 Volume 的 `receive_sky` 默认开启，由 Volume 控制，不自动推断室内外。

- 开启：使用独立的全局方向可见性结果生成 SkyBounce。
- 关闭：直接环境漫反射天光和本 Volume 的 SkyBounce 都为零，跳过相关可见性工作并清除旧天空贡献。
- 不影响太阳 DirectionalLight3D、其他灯光、emission 或 Godot 原生镜面反射。

通畅方向到达 Volume 边界就视为天空，因而 Volume 必须包含决定其天空遮挡的主要墙和屋顶。边界外遮挡不额外查询。

设完整发布的可见方向集合为 V：

```text
SkyIncoming_c = Σ(j∈V) (4π/26) × P(dj) × max(SkyRadiance_c(dj), 0)
SkyBounce_c = rho_c × G × SkyIncoming_c
```

天空使用原生环境的低阶辐射输入，按统一单位与 Volume 坐标转换。本地直接天空不写入 Radiance，不把未经反射的天空作为普通 Source。

[HDDAGI 边界](external_gi.md) 不进入本阶段 Source。其字段可能已混合直接天空和反弹；本方案接受外部输入与本地天光的重复近似及偏亮，不再因包含天空而禁用边界。`receive_sky=false` 不会剔除这部分外部混合天空。

Source 刷新时可以直接从已发布掩码与环境输入计算 SkyBounce，不要求额外保存每 Probe 的 RGB 天空方向场。

环境天空或已发布可见性改变时更新 SkyBounce；几何不变时不重算天空遮挡。刷新任意灯光的总 Source 时都必须保留当前 EmissionSH 和 SkyBounce，不能被直接光覆盖掉。

直接天光的标量遮蔽及多 Volume 合成见 [Screen Gather](screen_gather.md)。

## 8. 验收边界

首要目标为 PC Forward+，不套用原文移动端 0.5 ms 作为承诺。测量完整源项成本，不只统计矩阵乘法。Source Debug View 读取实际合并 Source，不为诊断常驻分灯、分发光或分天空副本。

必要核对：原生照明单位、不同光源入射量、Volume 方向光阴影覆盖与 ToD 更新、局部灯原生阴影、负光源语义，以及 Probe 中心采样在墙角的误差。

必要场景：灯删除后无残留 Source、全黑环境中的发光、仅改环境光不改 EmissionSH、相机转头不改变方向光 Source。有限传播停更后的光场残差属于传播预算问题，不能与残留源项混为一谈。
