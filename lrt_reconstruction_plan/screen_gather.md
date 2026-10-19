# LRT 重建方案：直接低分辨率 Screen Gather

本阶段消费 [辐射传播](propagation.md) 的最新 Radiance A/B 和独立天光结果。依据用户最终选择，只做低分辨率 Gather 与基础 Probe 插值，不做额外平滑链。设置和八种生产诊断见 [设置与调试](settings_and_debug.md)，原生交接见 [引擎适配](engine_integration.md)。

## 1. 最短显示链路

```text
不透明深度＋法线
    → 低分辨率重建表面位置
    → 沿世界表面法线偏移固定距离（默认 0.1 m）
    → 8 点基础三线性采样最新 Probe 场
    → 计算漫反射 GI 与标量天光可见性
    → 按 Volume 顺序合成两个输出
    → Basepass 直接采样
```

全局 `screen_gather/resolution` 默认宽高各一半，即约 25% 总像素，可选原尺寸 100% 像素，不增加 Volume 级覆盖。Basepass 使用常规纹理采样读取 Gather 结果，不额外生成全分辨率上采样光照图。

删除当前光照金字塔、固定世界半径平滑、几何引导滤波与显式上采样 pass。删除独立 display pass、显示光场副本和 0.15 秒时间响应；不添加另一套屏幕时间历史。

传播停止时直接保留最终光场，不继续平滑。相机或场景仍在绘制时，Screen Gather 仍有每帧成本；“静态停更”不代表整套 LRT 零渲染成本。

## 2. 从 Probe 场取得表面光照

用 Gather 样本对应的不透明深度和法线恢复世界位置，再转换到 Volume 局部空间。位置是否受 Volume 覆盖及边界混合权重，始终依据真实表面位置。

光场采样位置沿世界空间单位表面法线向外偏移：`sample_world_position = world_position + world_normal × normal_bias`，再转换到 Volume 局部空间。全局 `screen_gather/normal_bias` 默认 0.1 m，与 Probe 间距和 Volume 缩放解耦，修改后下一帧 Gather 生效，不重建光场。覆盖判断和边界混合仍使用未偏移的真实表面位置，漫反射卷积仍使用原表面法线。偏移同时作用于 RGB Radiance 和天光可见度 H。

使用偏移位置周围 8 个 Probe 做基础三线性插值，读取：

- RGB SH4 Radiance，直接绑定当前 A 或 B，不先打包成显示纹理。
- 可选的无色 SH4 天光渲染系数 H，来自唯一全局方向掩码的派生结果。

沿网格边缘采用基础 clamp 寻址；实体 Probe 保持其零光场和原始插值权重，不剔除实体样本、不按有效权重归一化，也不用邻居值填充。有效性归一化会在支持权重归零时形成突变黑边，已撤回；当前接受实体零值混合造成的渐变暗带。

实际 D3D12 GPU 夹具验证了 Probe 间距 0.5／0.2／0.1 m、Volume X 缩放 1／2 共六种组合：线性世界坐标光场均采到 0.1 m 外推位置，Coverage 保持 1。报告：`%TEMP%/lrt-normal-bias-regression.json`。这验证了偏移尺度及 UBO 布局，转角和窄缝画质仍需在新编辑器版本中检查。

法线偏移不做邻近几何碰撞约束：窄缝中仍可能进入或穿过邻近物体，导致残余暗区或漏光。不增加前侧样本重加权或空间扩展修补，不将此版本描述为薄墙或无有效探针区域的完整修复。

Y、P、K 的定义见 [局部数据](pre_propagation.md)。对插值光场 R、插值 H 和单位表面法线 n：

```text
GI_c = max(dot(R_c, K(n)), 0) / π
SkyVisibility = dot(H, K(n)) / π
```

receive_sky 关闭时直接令 SkyVisibility 为零，不读取 H。反弹 GI 不再乘这个天空可见度。材质 albedo、原生 AO 与曝光仍在原生着色的相应位置处理，不能在 Source、Gather 和 Basepass 中重复套用。

基础插值是 Gather 内部取值，不是额外滤波 pass。它并不保证薄墙两侧完全隔离；出现不可接受的混光时，应先检查局部阻断和格点精度，不宣称屏幕低分辨率或 SSGI 可以自动修复。

## 3. 两个明确输出

| 输出 | 内容 |
| --- | --- |
| RGBA16F Lighting | RGB 为预乘覆盖率的反弹 GI，A 为累计 Coverage |
| R16F Sky Visibility | 预乘覆盖率的标量天空可见度 |

五个独立量不塞进一张 RGBA，不使用特殊打包技巧，也不让 Basepass 重新遍历全部 Volume 计算覆盖率。

这里的天空可见度不是 SSAO，不用于再次压暗反弹 GI。直接天空使用 Godot 原生环境漫反射颜色乘这个标量，不保存每 Probe 的完整 RGB 直接天空方向场。

接受不同开口看到蓝天、暖色地平线时的直接天空颜色方向差异被简化。天空反弹仍按方向掩码与天空输入生成 Source，不等于完全丢弃天空颜色。

## 4. 多 Volume 合成

保留当前优先级及相同优先级的场景顺序，较高层覆盖较低层。每个 Volume 独立求解，不交换辐射。

初始值：

```text
GIpre = 0
C = 0
Vpre = 0
```

对当前 Volume 的表面混合权重 w、GI 和天空可见度 v：

```text
GIpre = w × GI + (1 - w) × GIpre
Vpre  = w × v  + (1 - w) × Vpre
C     = w      + (1 - w) × C
```

GI 与 V 使用相同的覆盖、排序和边缘权重。关闭 receive_sky 的 Volume 取 v 为零，它会按优先级遮去下层或原生环境的漫反射天光，不是变成完全无遮挡。

## 5. Basepass 的边界语义

无 Volume 覆盖时保留原生照明。概念合成：

```text
FinalIndirect = GIpre + (1 - C) × NativeOtherIndirect
FinalSkyDiffuse = NativeSkyDiffuse × (Vpre + 1 - C)
```

NativeOtherIndirect 不包含这里单独处理的直接天空。具体引擎接入必须拆清原生环境漫反射与其他 GI，不能对同一项重复替换或叠加。

- Volume 内完整覆盖时，使用 LRT 反弹和其天空可见度。
- Volume 外，反弹回到原生输入，天空可见度为 1。
- 边缘按 Coverage 过渡。
- receive_sky 只控制本 Volume 的环境漫反射与天空反弹，不关闭太阳直接光、emission 或原生镜面反射。

本地直接天空不写进 GIpre。原生 HDDAGI 等其他间接光在完整覆盖区域不再全量叠加，边缘按 Coverage 过渡，避免把已由 LRT 边界接收的原生 GI 再覆盖相加。

但 [HDDAGI 边界](external_gi.md) 接受完整混合场，可能已含直接天空；这部分经传播进入 GIpre，仍可能与本地天光重复并偏亮，是已接受的近似。`receive_sky=false` 只控制本地天空链，不从 GIpre 中拆除外部混合天空。

隐藏或因实际资源分配失败而失效的 Volume 不贡献 GI、Sky Visibility 或 Coverage，不能以无效黑色字段遮去原生照明。

## 6. 首版接收范围与 SSGI

首版仅支持不透明 LRT 接收，包含按原生不透明路径参与预通道的材质。普通透明表面保留 Godot 原生照明，不使用不透明深度对应的 Gather 结果，也不额外直接采样 Probe 场。

这是明确接受的能力缩减，用于删除专门的透明 receiver 打包和字段绑定路径。材质是否贡献粗几何、反射或 emission，仍由局部构建的捕获规则决定，不能与是否接收混为一谈。

现有 SSGI 继续负责屏幕高频细节，不反馈 Radiance、Source 或局部矩阵。其具体残差或叠加方法属于该方案自身接口，不在这里另建 SSGI 系统。

## 7. 接受的风险与核对项

没有额外时间和空间平滑，Source 更新及相机移动的变化会直接显示。RGB 已改为固定六面算子，不再产生四点轮换的三相输出。基本三线性改善格点跳变，但不保证近远一致的世界尺度平滑，也不保证低分辨率跨轮廓采样无误差。

重点核对：深度对应位置、网格边缘、实体零样本、薄墙两侧、多 Volume 交叠、隐藏／恢复、关闭天光的室内 Volume，以及 Basepass 不再全量叠加同一份原生 GI。HDDAGI 混合天空的重复按已接受的边界近似单独观察，不宣称完全去重。

若这些误差超出可接受范围，应回到数据精度或再次明确接收端取舍，不未经确认恢复金字塔、滤波或时间历史。
