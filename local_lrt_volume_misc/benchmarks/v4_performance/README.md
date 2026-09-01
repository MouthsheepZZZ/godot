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

## Optimization 1 — Dynamic dirty upload

Date: 2026-09-01
Commit baseline: `91bf38c541`

The first retained optimization replaces CPU zero uploads for dirty Radiance rows with GPU buffer clears, skips the unchanged full-volume CPU Injection upload while still rerunning analytic Injection, and caches unchanged analytic-light records.

| Metric | Baseline | Optimized | Change |
| --- | ---: | ---: | ---: |
| Dirty update upload | `2,856,072 bytes` | `1,341,000 bytes` | `-53.0%` |
| Dirty Geometry / Transfer update | `111.323 ms` | `109.010 ms` | `-2.1%` |
| Dirty probes | `1690 / 28175` | `1690 / 28175` | unchanged |

The full Global Visibility A/B reset remains because the current finite-hop recurrence needs a clean Local Visibility seed to reproduce the deterministic reference after geometry changes. Removing that reset without storing propagation depth or an equivalent exact invalidation scheme would change results; it is therefore not part of this optimization.

Validation: incremental build PASS; Local LRT targeted `67 / 4606`; GPU Radiance validation confirms all dirty RGB rows clear while every value outside the dirty region is preserved; GPU Analytic Injection confirms repeated identical light records reuse the cached buffer and produce the same reference result.

## Optimization 2 — Geometry-source segment broadphase

Date: 2026-09-01

Dirty-update subphase timing showed that geometry building consumed `106.401 / 106.850 ms` (`99.6%`), while source synchronization, packing, and the RenderingServer call together consumed less than `0.5 ms`. Each of the 26 neighbor segments previously sampled every Color SDF source even when the segment could not reach that source. The builder now rejects those impossible queries using each source's conservative volume-local surface AABB before sampling the SDF endpoints.

| Metric | Previous | Optimized | Change |
| --- | ---: | ---: | ---: |
| CPU full Geometry / Transfer rebuild, median | `2106.545 ms` | `361.452 ms` | `-82.8%` |
| CPU dirty Geometry / Transfer update | `109.010 ms` | `18.518 ms` | `-83.0%` |
| Dirty builder subphase | `106.401 ms` | `17.565 ms` | `-83.5%` |
| Dirty source synchronization | — | `0.066 ms` | — |
| Dirty data packing | — | `0.387 ms` | — |
| Dirty RenderingServer upload call | — | `0.500 ms` | — |

The broadphase does not approximate Color SDF values: a source is skipped only when its conservative surface bounds do not intersect the probe-to-neighbor segment. Source order, nearest-center signed distance, overlap precedence, full segment-hit evaluation, and uploaded values remain unchanged.

Validation: incremental build PASS; Local LRT targeted `67 / 4606`; GPU Visibility, Radiance dirty-clear, and Analytic Injection cached-light validations PASS. No visual algorithm or shader output changed, so this CPU query optimization does not add a new visual acceptance gate.
