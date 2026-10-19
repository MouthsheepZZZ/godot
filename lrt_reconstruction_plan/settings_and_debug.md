# LRT 重建方案：设置与生产调试

[阅读索引](README.md) · [引擎适配](engine_integration.md)

设置必须单一归属：Project Settings 管全局成本与构建规格，Volume 管空间和场景意图，Debug View 管临时观察。不增加 Volume 级预算覆盖，不把诊断状态保存进场景。

## 1. Project Settings：7 项

以下路径均以 `rendering/global_illumination/lrt/` 为前缀。

| 路径 | 默认 | 含义 |
| --- | --- | --- |
| `screen_gather/resolution` | 0 | 0 为宽高各一半、25% 像素；1 为原尺寸、100% 像素 |
| `screen_gather/normal_bias` | 0.1 m | 沿世界表面法线外推光场采样位置，与 Probe 间距解耦；不改变 Coverage，不重建光场 |
| `geometry/target_cell_size` | 0.125 m | 按 Mesh 局部尺寸换算捕获分辨率的目标尺度，正数，不是 Probe 间距 |
| `directional_shadow/resolution` | 512 | 每个 Volume、每个投影方向光的深度图边长，64–2048 |
| `directional_shadow/bias` | 0.02 m | Volume 局部距离，除以光空间深度跨度后参与深度比较，不影响原生直接光阴影 |
| `propagation/iterations_per_frame` | 1 | 活跃 Volume 每帧最多执行的 RGB 轮数，正整数 |
| `propagation/iterations_after_change` | 64 | 有效输入变化后续充的 RGB 总轮数，正整数 |

粗几何精度变化使相关共享派生几何和局部构建缓存失效；Gather 分辨率变化只影响屏幕资源。传播预算的修改语义见 [传播方案](propagation.md)。这些默认值是成本测量起点，不是质量或响应时限保证。

删除旧的 `sdf/target_cell_size`，由 `geometry/target_cell_size` 替代；不保留旧键迁移或双读。还需删除：

- `sdf/minimum_feature_size_percent`。
- `propagation/sampling`、`propagation/display_response_time`。
- `dynamic_objects/update_interval`。
- `cache/memory_budget_mb`、`limits/max_volume_gpu_memory_mb`。

脏对象每帧收集并合并到最新状态，不等于捕获或构建必须当帧完成。独立天空任务仍按传播文档每帧最多一轮，不新增设置。

## 2. Volume：仅 7 项 LRT 专属 Inspector 设置

| 设置 | 默认 | 职责 |
| --- | --- | --- |
| `size` | Vector3(6, 4, 6) | Volume 局部尺寸 |
| `probe_spacing` | 0.5 m | Probe 格点间距，正数 |
| `priority` | 0 | 多 Volume 合成顺序 |
| `blend_distance` | 0.5 m | 覆盖边缘混合距离 |
| `editor_preview` | true | 是否在编辑器预览，不改变运行时启停语义 |
| `receive_sky` | true | 本地天空可见性、SkyBounce 和接收面的直接环境漫反射 |
| `external_gi_enabled` | true | 是否接收兼容的 HDDAGI 外部边界 |

尺寸及间距改变需要重新准备对应网格数据。优先级和混合距离只改变显示合成，不重建局部几何。天空和边界开关分别按各自输入失效规则处理。

不新增独立 `enabled`，启停使用节点自身的可见性。删除 `spacing`／`volume_size` 等脚本属性别名，以及 geometry backend、visibility mode、multi-bounce、sampling、blur、display blend 等旧算法开关。内部构建缓存及来源版本可以序列化，但不作为用户设置。

隐藏时停止该 Volume 的准备、注入、边界采样、RGB 和天空求解，并退出 Gather 合成；已提交工作按正常同步安全完成，不能再向失效版本发布。保留已分配资源和光场，并记录或在恢复时核对失效输入。重新显示先同步最新输入，不把旧缓存无条件当作有效结果。

## 3. 内存不设人为上限

- 不设置 CPU 共享缓存预算或 Volume 显存上限，也不以固定常量偷偷恢复限制。
- 不自动降低 Probe 密度、粗几何精度或删除功能来应付内存需求。
- 共享粗几何按实际使用寿命持有；最后一个使用者释放后移除内存缓存。隐藏 Volume 可以继续持有引用，删除 Volume 或卸载场景则释放其引用。
- 资源修订后，无使用者的旧版本随之释放，不无限积累历史版本。
- 实际分配失败时报告所需资源及失败阶段，让失效 Volume 退出合成；不能将无效数据或黑色 Coverage 继续覆盖原生照明。无覆盖位置按原有渲染语义处理。

常规资源计数、字节统计和构建错误可用于诊断，不因此引入另一个预算或调度器。

## 4. Debug View：严格 8 项

沿用现有 **Display Advanced → LRT** 全局入口，以及原生 Viewport Debug Draw 的调用方式。下面每行就是一个实际模式，不再添加独立子模式、选择方向控件或另一个 LRT 调试面板。

| 模式 | 显示内容与用途 |
| --- | --- |
| Lighting | 实际合成的 LRT GI，附 Coverage 边缘提示，检查覆盖与多 Volume 过渡 |
| Radiance Probes | 当前 RGB SH4 光场方向瓣，检查扩散分布 |
| Source Probes | 已发布的合并 Source 方向瓣，检查首跳及发光输入 |
| Coarse Geometry | 粗表面／占据，以 albedo 着色并叠加发光标记；同时检查几何与材质 |
| Local Visibility | OpenMask 的开闭方向标记，检查局部连接阻断 |
| Sky Visibility | 已发布的天空可见方向及其可见性表达，检查屋顶、门窗和边界 |
| Boundary | 实际越界方向的外部 RGB 与覆盖状态，区分有效黑、无覆盖和未就绪 |
| Update Regions | 按颜色区分局部构建、Source 更新及求解活动，检查不必要的持续工作 |

默认观察当前可见 Volume，不引入选中单个 Volume 的额外界面。粗几何以 albedo 为底色，发光 voxel 在角落显示黄色小标记；黄色仅表示存在 emission，不表示发光颜色或强度。

移除旧 Transfer、SDF Surface、独立 Albedo／Emission 视图，不保留原文对照或 Full26 比较算法。诊断数据直接来自实际生产字段；Source 不常驻分灯或分来源副本。额外绘制和必要诊断工作仅在所选视图启用时进行，不常驻 GPU 读回。

移除暂停和单步的工具、脚本控制及状态。不增加独立 reset／Probe 检查面板；正常重建和构建状态反馈不属于 Debug View，继续服务于数据构建。

## 5. 实施验收

核对全局 7 项、节点 7 项、菜单 8 项及旧入口的完整删除。验证隐藏／恢复、共享缓存最后引用释放、分配失败退出合成，以及关闭调试时没有额外诊断任务。当前实现及验证范围见[阅读索引](README.md)。
