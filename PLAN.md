# Project Milestones

方案依据：[Local LRT GI](lrt_module_development/local_lrt_design.md)。

- [x] 模块与编辑器基础：Volume、实例绑定、烘焙资源及全局设置可用。
- [ ] 几何烘焙：静态 Volume 缓存与实例 Color SDF 正确生成并保存。
- [ ] 动态几何更新：Trunk 预算调度、Edge 遮挡及缓存批次发布支持实例移动、加入和移除。
- [ ] GPU 光照：完成灯光、阴影、天空和自发光注入及 A/L 传播。
- [ ] 多 Volume 协同：支持局部空间变换、外层到内层边界输入与优先级混合。
- [ ] 渲染融合：Screen Gather、引导升采样和直接 Probe 采样接入 Godot 间接漫反射。
- [ ] 调试与验收：调试视图和性能统计可用，测试项目完成视觉、动态更新与性能验证并留档。

## Current

当前里程碑：几何烘焙。静态缓存与动态 Color SDF 均已验证生成及保存重载，真实车厢完成 81,925 个 Probe 的静态烘焙并留档；高级材质适配尚未完成，暂无 GI 渲染结果。
