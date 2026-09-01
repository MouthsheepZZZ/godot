# Local LRT V4 Unoptimized Baseline

Date: 2026-09-01  
Engine: Godot 4.7 dev build, Vulkan Forward+  
GPU: NVIDIA GeForce RTX 5080  
Scene: `cornell_dynamic_v12.tscn`  
Grid: `35 × 23 × 35` (`28,175` probes), `0.25 m` spacing  
Budgets: `visibility_iterations = 1`, `propagation_iterations = 16`

## Baseline

| Metric | Unoptimized result |
| --- | ---: |
| GPU Visibility, one probe hop | `0.003562 ms` |
| GPU Radiance, 16 probe hops | `0.708926 ms` |
| GPU Radiance, normalized one hop | `0.044308 ms` |
| GPU Analytic Injection, 3 lights | `0.015098 ms` |
| CPU full Geometry / Transfer rebuild, median | `2106.545 ms` |
| CPU dirty Geometry / Transfer update | `111.323 ms` (`1690 / 28175` probes) |
| Dedicated Local LRT GPU memory | `14,798,584 bytes` (`14.113 MiB`) |
| Full rebuild upload | `16,116,708 bytes` (`15.370 MiB`) |
| Dirty update upload | `2,856,072 bytes` (`2.724 MiB`) |
| Stable-frame CPU → GPU upload | `128 bytes` |

Full rebuild CPU samples were `2098.716 / 2106.545 / 2111.136 ms`.

GPU values are the mean of four one-second `--gpu-profile` windows. Visibility was measured as a deterministic 10-hop batch (`0.035620 ms`) and divided by ten because Godot's printed GPU profiler omits entries below `0.01 ms`. Radiance used the actual 16-hop frame budget. These are dev-build measurements and must be compared against later optimizations on the same build, hardware, renderer, resolution, and scene.

## Memory and upload accounting

Dedicated memory includes the volume's storage buffers, analytic-light buffer, shadow visibility/matrix buffers, and its `512 × 512 D32` directional shadow texture. It excludes shared shader pipelines, the default fallback textures, and the renderer's shared positional shadow atlas.

The full rebuild upload includes new buffer initialization, both Global Visibility reset uploads, the explicit `inside_solid` update, Injection upload, and three analytic-light records. The dirty update includes region rows for Visibility / Transfer / MeshLight / `inside_solid` / both Radiance buffers, the current full-volume Global Visibility reset, full-volume Injection upload, and analytic lights. Stable-frame upload is the Environment data plus directional shadow matrix.

## Commands

```text
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method forward_plus --rendering-driver vulkan --script res://v4_baseline_benchmark.gd

bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method forward_plus --rendering-driver vulkan --gpu-profile --script res://v4_gpu_baseline_benchmark.gd
```

The recurring `PipelineDeferredRD::~PipelineDeferredRD free()` shutdown messages are the pre-existing CLI cleanup issue already recorded in `LOCAL_LRT_STATE.md`; neither benchmark reported a runtime validation failure.
