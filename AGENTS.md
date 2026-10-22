分支用来实现LRT高性能全局动态GI方案（以下简称LRT），方案原文位于：
- C:\Users\Zykert\.herdr\worktrees\godot\master-lrt\lrt_reference\lrt_ref.pdf
相关算法与核心思路完全参考原文，原文中没有的可以根据设计改动

测试项目位于：
- G:\lrt_godot_test

改动相关计划和信息位于：
- PLAN.md

LRT模块由我自己独立维护，不打算提PR，因此这个模块需要与原始引擎分支最小耦合，方便rebase，resolve压力为零。
优先采用胶水适配器模块进行耦合编辑。仅只有涉及到需要调用private接口等无破坏改动的情况下才允许接触原始引擎分支源码改动

LRT是给风格化画面使用的，因此不需要追求精度与物理准确，光照平滑优先，漏光但是对比度不强也允许接受。不希望出现尖锐明显突出对比过强的artifact
LRT的运行时性能效率优先，运行时不进行任何trace类操作，trace类仅在离线only
LRT Probe数据的LTM和LocalVisibility的来源只有两个，Precomputed和Voxel DDA运行时构建。前者储存在表面，后者运行时动态快速构建。

LRT模块目前设计为全部GeometryInstance3D考虑（Mesh、MultiMesh等）的全动态变化生效，ProceduralMesh等实时动态变化的不考虑，同时也不考虑粒子、BlendShape、ShaderWPO、TimeBased渐变控制等材质动态内容变化，仅支持Opaque材质，包含StandardMaterial和ShaderMaterial，透明材质只接受GI，不参与GI。

不要编写单元测试，最小化改动原则，遵循fast-fail，快速迭代优先

对测试项目的改动、debug均使用godot-mcp调用，如果出现了连接问题，请重启编辑器。

bin文件夹，即编译后的编辑器产物仅允许一版，如果编译的时候出现占用，请保存并强制退出后再进行编译

涉及到视觉验证相关的内容，请截图留档，并贴在回复里面。
