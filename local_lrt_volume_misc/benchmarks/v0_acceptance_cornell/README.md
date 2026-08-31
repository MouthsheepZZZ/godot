# Local LRT v0 总验收 Cornell Benchmark

本 benchmark 同时覆盖 Directional、Omni、Area、Spot、Geometry Emission 增量以及独立 Emission Mesh。Godot 与 Blender 共用 Cornell 几何、相机、Lambertian 材质、黑色 World、512×512、AgX 和 Exposure 0。

解析灯严格使用 `Shadow → Injection → 26-neighbor gather → current-probe Local Visibility → Local Transfer → Forward`。Emission Mesh 同时在 Base Pass 显示 authored emission，并按 PDF 的 `MeshLightSH → InComingLight → current-probe Local Visibility → Local Transfer` 产生 GI；`ColorToFill = albedo + emission` 仍用于材质 transfer。实现中不存在绕过 LTM 的 outgoing emission 注入通道，也没有隐藏解析灯替代 Emission Mesh。

## 场景与输出

- `cornell_v0_acceptance.tscn` / `cornell_v0_acceptance_cycles.blend`：四类解析灯组合，Geometry emission 关闭。
- `cornell_v0_emission.tscn` / `cornell_v0_emission_cycles.blend`：在相同四灯输入上启用暖色 Geometry emission。
- `cornell_v0_emission_mesh.tscn` / `cornell_v0_emission_mesh_cycles.blend`：关闭全部四类解析灯，仅由真实 Emission Mesh 照亮场景；Cycles 使用 Diffuse + Emission shader。
- `godot_combined_agx.png` / `cycles_combined_agx.png`：四灯组合结果。
- `godot_emission_agx.png` / `cycles_emission_gain_agx.png`：Emission 经 Local Transfer 的增量结果。
- `godot_emission_mesh_agx.png` / `cycles_emission_mesh_agx.png`：独立 Emission Mesh 的 Godot Local LRT / Cycles 对照。

Godot MCP 运行验证确认四灯同时启用，移动 Area 后静态 Geometry count 保持 `8 → 8`；独立 Emission Mesh 场的四灯均为 `visible=false`，`84525` 个 Radiance SH 值中 `60138` 个非零，最大长度 `4.2901459`。Emission 能量为 `0` 时 Radiance 全零，`64 → 128` 时最大值 `1.3071526 → 4.2901459`，结果有限且单调。最终 current run 无项目脚本或渲染错误；编辑器仅报告外部 Vulkan layer/OBS hook 警告。

![Godot Combined](godot_combined_agx.png)

![Cycles Combined](cycles_combined_agx.png)

![Godot Emission](godot_emission_agx.png)

![Cycles Emission](cycles_emission_gain_agx.png)

![Godot Emission Mesh](godot_emission_mesh_agx.png)

![Cycles Emission Mesh](cycles_emission_mesh_agx.png)
