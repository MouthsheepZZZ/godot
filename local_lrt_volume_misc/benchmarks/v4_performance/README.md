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

## Optimization 3 — Dynamic update probe budget

Date: 2026-09-02

`LocalLRTVolume3D.dynamic_update_probe_budget` now limits how many Dirty Probe rows the CPU builder may process per internal frame. `0` keeps the unlimited one-frame behavior; a positive value enables deterministic x-major slices. Geometry sources and Dirty bounds are captured once, CPU slices are accumulated across frames, and static GPU data, light Injection, and debug probes update only after the complete region is ready.

The reference describes frame-sliced GPU transport and Trunk-local CPU construction, but does not specify a CPU Dirty-build budget or partial-update visibility contract. This implementation therefore keeps the previous final result and one-shot region upload while trading update latency for a bounded per-frame Probe count.

| Metric | Unlimited | Budgeted |
| --- | ---: | ---: |
| Dirty probes | `1690` | `1690` |
| Probe budget per frame | unlimited | `256` |
| Builder frames | `1` | `7` |
| Maximum Builder slice | `17.565 ms` | `3.212 ms` |
| Total Dirty CPU work | `18.518 ms` | `18.822 ms` |
| GPU static-data uploads | `1` | `1` |

Validation: incremental build PASS; Local LRT targeted `67 / 4611`, including serialized budget, exact frame count, SDF reuse, and budgeted Dirty result versus full rebuild; GPU Visibility, Radiance dirty-clear, and Analytic Injection cached-light validations PASS.

## Optimization 4 — Invisible Volume update pause

Date: 2026-09-02

Renderer updates now reuse the exact per-camera Volume selection used by Forward surface binding. A Volume outside the current camera frustum, or beyond `max_volumes_per_camera`, skips its complete renderer update loop: Environment upload, shadow rendering, Visibility propagation, Analytic Injection, and Radiance propagation. Its Global Visibility A/B choice, propagation depth, and Radiance history remain unchanged; CPU Geometry Dirty scheduling continues independently.

The deterministic two-Volume validation uses `max_volumes_per_camera = 1`. The selected Volume propagates, the culled Volume performs zero update-loop dispatches and retains every Global Visibility value, and rotating the camera reverses those roles without resetting state. This removes all per-frame Local LRT GPU work for an unused Volume while preserving the exact resumed result.

Validation: incremental build PASS; Local LRT targeted `67 / 4611`; GPU invisible-Volume selection / preservation / resume PASS on Forward+ and Forward Mobile; GPU Visibility, Radiance, Analytic Injection, Forward Surface, and DynamicGI composition regressions PASS.

## Optimization 5 — Complete-hop Probe slicing

Date: 2026-09-02

`visibility_probe_budget` and `radiance_probe_budget` cap the number of Probe rows dispatched per render update; `0` preserves unlimited behavior. A partial Jacobi hop writes only the destination A/B Buffer and keeps the previous complete source Buffer bound to Forward. The source/destination role changes only after every Probe row in the hop has been written. Static-data changes discard partial offsets before restarting propagation.

This follows the reference's Global Visibility A/B convolution and unlimited frame slicing (section 5.6), and applies the same scheduling invariant to the analogous Radiance propagation in section 5.7.

| Metric | Unlimited | Probe budget `16,384` | Change |
| --- | ---: | ---: | ---: |
| Radiance Probe rows dispatched per frame | `450,800` (`28,175 × 16`) | `16,384` | `-96.4%` |
| GPU Radiance mean | `0.758708 ms` | `0.067914 ms` | `-91.0%` |
| Complete-hop latency | `1` frame | `2` frames | configurable tradeoff |

The GPU timings are five one-second Forward+ profiler windows on the same RTX 5080 dev build. Visibility slices fell mostly below Godot's `0.01 ms` print threshold; the numerical validation is therefore the acceptance authority for Visibility.

Validation: incremental build PASS; Local LRT targeted `67 / 4613`; Forward+ and Forward Mobile GPU tests hide all partial Visibility/Radiance writes and match the unlimited complete-hop result. The Cornell comparison after `128` equal complete Radiance hops reports mean framebuffer error `0.00000033` and maximum error `0.00392157` (one 8-bit level). Captures: `probe_budget_reference.png`, `probe_budget_sliced.png`.

Commands:

```text
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method forward_plus --rendering-driver vulkan --gpu-profile --script res://v4_gpu_baseline_benchmark.gd -- visibility_probe_budget=16384 radiance_probe_budget=16384

bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method forward_plus --rendering-driver vulkan --script res://v4_probe_budget_visual_validation.gd
```

## Optimization 6 — Original 4-neighbor / 3-phase Radiance pattern

Date: 2026-09-02

The default Radiance gather now follows the reference section 5.7: it keeps only the twelve cube-edge neighbors (offsets with exactly one zero component), divides them into three four-sample phases, and advances one phase per complete Jacobi hop. `radiance_neighbor_pattern = Reference 26` retains the deterministic 26-neighbor golden path; `Dithered 4` is the optimized default.

Each Probe receives a fixed integer-hash phase offset. Directly replacing the operator with spatially dithered four-sample results produced visible stationary blotches, so the retained implementation combines every phase with `1/3` history accumulation. This is a deterministic engineering closure for the reference's unspecified dither integration: it preserves the original sample set and three-phase schedule while stabilizing the temporal result. Changing pattern clears both Radiance A/B Buffers and restarts phase zero.

| Metric | Reference 26 | Dithered 4 | Change |
| --- | ---: | ---: | ---: |
| Neighbor samples / Probe / hop | `26` | `4` | `-84.6%` |
| GPU Radiance mean, 16 hops | `0.824234 ms` | `0.429698 ms` | `-47.9%` |
| Cornell framebuffer mean error | — | `0.00321054` | `0.32%` full scale |
| Cornell framebuffer max error | — | `0.03529412` | localized |

GPU values are five one-second RTX 5080 Vulkan Forward+ dev-build windows. The automated shader test independently reproduces all three phases on CPU; Forward+ and Forward Mobile pass. The final Forward+ captures are `neighbor_reference_26.png` and `neighbor_dithered_4.png`; visual inspection shows the same light-bleed structure without the rejected stationary blotches.

Command:

```text
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method forward_plus --rendering-driver vulkan --gpu-profile --script res://v4_gpu_baseline_benchmark.gd -- radiance_neighbor_pattern=1
```

## Optimization 7 — Screen Space Gather

Date: 2026-09-02

Forward+ now reconstructs Local LRT surface lighting once at half width and half height (25% of full-resolution pixels). The RGBA16F gather stores RGB reflected GI and A sky occlusion as specified by reference section 5.8. A separate R16F texture retains the cascade edge weight required by the existing DynamicGI replacement composition. Base Pass performs two bilinear texture reads instead of the cubic 3D Probe reconstruction. The compute pass and forced depth/normal prepass are active only when a selected Local LRT Volume is visible.

| Metric | Direct surface sampling | Quarter-pixel gather | Change |
| --- | ---: | ---: | ---: |
| Surface reconstruction pixels | `746,496` | `186,624` | `-75.0%` |
| Viewport GPU mean | `0.624580 ms` | `0.458623 ms` | `-26.6%` |
| Viewport GPU mean of run medians | `0.566000 ms` | `0.376800 ms` | `-33.4%` |
| Additional single-view cache | — | `1,866,240 bytes` | `1.780 MiB` |
| Cornell mean framebuffer error | — | `0.00101157` | `0.10%` full scale |
| Cornell maximum error | — | `0.19607843` | localized geometry edges |

GPU values use five independent processes and 180 steady-state samples per process (`900` samples per path) on the same RTX 5080 Vulkan Forward+ dev build at `1152×648`. Direct sampling remains available at renderer startup through `rendering/global_illumination/local_lrt/screen_space_gather=false`.

Validation: incremental build PASS; Local LRT targeted `67 / 4614`; Forward+ Visibility, Radiance, Analytic Injection, Invisible Volume, Forward Surface, and DynamicGI composition PASS; Forward Mobile propagation regressions PASS. AI inspection of `screen_gather_reference.png` and `screen_gather_quarter_pixels.png` found no visible leakage, color shift, striping, or blotching. Forward Mobile does not yet consume Local LRT in its surface shader, so Mobile visual acceptance remains part of the explicit later adapter task.

Commands:

```text
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method forward_plus --rendering-driver vulkan --script res://v4_screen_space_gather_visual_validation.gd -- gather

bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method forward_plus --rendering-driver vulkan --script res://v4_screen_space_gather_benchmark.gd -- gather
```

## Optimization 8 — FP16 Luminance Transfer + RGB8 Tint

Date: 2026-09-02

The reference appendix explicitly proposes replacing three RGB 4×4 transfer matrices with one luminance matrix and three 8-bit color values. The implementation evaluates four startup-selectable formats through `rendering/global_illumination/local_lrt/transfer_format`. Luminance is the Rec.709-weighted matrix; a nonnegative least-squares RGB tint is normalized to `[0,1]`. The retained default packs the matrix as sixteen FP16 values and the tint as RGB8 UNORM.

| Format | LTM bytes / Probe | LTM change | Radiance GPU | Cornell mean / max error |
| --- | ---: | ---: | ---: | ---: |
| RGB FP32 reference | `192` | — | `0.477818 ms` | — |
| RGB FP16 | `96` | `-50.0%` | `0.352505 ms` (`-26.2%`) | `0.00005220 / 0.00392158` |
| Luminance FP32 + RGB8 Tint | `68` | `-64.6%` | `0.369491 ms` (`-22.7%`) | `0.00014444 / 0.01960785` |
| Luminance FP16 + RGB8 Tint | `36` | `-81.25%` | `0.331900 ms` (`-30.5%`) | `0.00017567 / 0.01960785` |

The GPU timings use the `28,175`-Probe benchmark with Dithered 4 and 16 Radiance hops on the RTX 5080. FP32 luminance reduces bytes further than RGB FP16 but adds tint reconstruction without enough bandwidth reduction to win; the combined FP16 format is both smallest and fastest.

| Volume metric | Previous | Retained format | Change |
| --- | ---: | ---: | ---: |
| Dedicated GPU memory | `14,798,584 B` | `10,403,284 B` | `-29.7%` |
| Full rebuild upload | `16,116,708 B` | `11,721,408 B` | `-27.3%` |
| Dirty upload (`1690` Probes) | `1,341,000 B` | `1,077,360 B` | `-19.7%` |

Dynamic Global Visibility, Radiance, and Injection remain FP32 because their values accumulate across hops and require a wider runtime range. Only the static Local Transfer Matrix uses FP16. Validation: incremental build PASS; targeted `67 / 4614`; Forward+ full GPU regression and Forward Mobile propagation regression PASS. AI inspection of `transfer_rgb_fp32.png` and `transfer_luminance_fp16_tint.png` found no visible color shift, striping, blotching, or energy discontinuity.
