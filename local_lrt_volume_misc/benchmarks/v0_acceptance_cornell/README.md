# Local LRT v0 总验收 Cornell Benchmark

本 benchmark 同时覆盖 Directional、Omni、Area、Spot 以及 Geometry Emission。Godot 与 Blender 共用 Cornell 几何、相机、Lambertian 材质、黑色 World、512×512、AgX 和 Exposure 0。

解析灯严格使用 `Shadow → Injection → 26-neighbor gather → current-probe Local Visibility → Local Transfer → Forward`。Emission 按 PDF 的 `ColorToFill = albedo + emission` 进入 Local Transfer；实现中不存在绕过 LTM 的 outgoing emission 注入通道。Blender Emission 对照也使用 Diffuse BSDF 的线性颜色增益，不使用 Emission BSDF。

## 场景与输出

- `cornell_v0_acceptance.tscn` / `cornell_v0_acceptance_cycles.blend`：四类解析灯组合，Geometry emission 关闭。
- `cornell_v0_emission.tscn` / `cornell_v0_emission_cycles.blend`：在相同四灯输入上启用暖色 Geometry emission。
- `godot_combined_agx.png` / `cycles_combined_agx.png`：四灯组合结果。
- `godot_emission_agx.png` / `cycles_emission_gain_agx.png`：Emission 经 Local Transfer 的增量结果。

Godot MCP 运行验证确认四灯同时启用，移动 Area 后静态 Geometry count 保持 `8 → 8`；Emission 场有 `380` 个非零 Probe，最大 RGB 和为 `1.4299999922514`。最终独立重启的 Godot current run 无项目脚本或渲染错误；编辑器仅报告外部 Vulkan layer/OBS hook 警告。

![Godot Combined](godot_combined_agx.png)

![Cycles Combined](cycles_combined_agx.png)

![Godot Emission](godot_emission_agx.png)

![Cycles Emission](cycles_emission_gain_agx.png)
