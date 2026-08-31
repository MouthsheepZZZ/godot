# Local LRT V1.2 Dynamic Source Reuse Benchmark

本基准使用同一 Cornell Box 中的动态 Box、Sphere 与斜面，对照逐物体 Local Color SDF 复用、旧/新影响区局部 Probe 更新及旧位置清理。Godot 与 Blender Cycles 的 Pose A / B 使用相同几何、材质、相机和 AgX 输出。

## 运行

- Godot：打开 `res://cornell_dynamic_v12.tscn`；`1/2/3` 选择 Box/Sphere/Slope，`WASD` 平移，`Q/E` 旋转，`R` 恢复 Pose A，`V` 切换 Probe。
- Blender：打开 `cornell_dynamic_source_reuse_v12_cycles.blend`；frame 1/2 分别为 Pose A/B，Cycles 512 samples，512×512，AgX Medium High Contrast。
- Godot 状态栏显示最近一次局部更新的 Dirty Probe、SDF Build Count 与 CPU 时间。

## 自动验证

- 15 次同步动态更新：平均 `7.999 ms`，最大 `9.944 ms`（Windows dev build）。
- 平均 Dirty Probe `1123.33 / 28175`（`3.99%`），最大 `1320 / 28175`（`4.68%`）。
- SDF Build Count 保持 `11 → 11`，证明纯平移/旋转复用 object-local SDF。
- targeted tests：`57 passed / 4534 assertions / 0 failed`；full suite：`1416 passed / 424580 assertions / 0 failed / 3 skipped`。

详细机器可读结果见 `benchmark.json`。

## 截图

| Pose | Godot | Blender Cycles |
| --- | --- | --- |
| A | `godot_v12_pose_a_agx.png` | `cycles_v12_pose_a_agx.png` |
| B | `godot_v12_pose_b_agx.png` | `cycles_v12_pose_b_agx.png` |
