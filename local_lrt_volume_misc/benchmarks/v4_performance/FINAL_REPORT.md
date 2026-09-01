# Local LRT V4 Performance Final Report

Date: 2026-09-02  
Branch: `feature/hddagi-4.7/local-lrt-volume-3d`  
Hardware: NVIDIA GeForce RTX 5080, Vulkan Forward+, Godot 4.7 dev build

## Scope and source

The implementation follows `local_lrt_volume_misc/lrt_ref.pdf` where the reference defines the relevant data flow:

- Section 5.6: Global Visibility A/B convolution and frame slicing.
- Section 5.7: Radiance transfer split into three four-neighbor phases with dithering.
- Section 5.8: quarter-pixel Screen Space Gather carrying RGB bounce and sky occlusion in alpha.
- Sections 5.9–5.10: CPU Grid Trunks, 26 neighboring Trunks, GI Primitive lists, caches, and Trunk-local Probe construction.
- Appendix: replace three RGB transfer matrices with one luminance matrix and three 8-bit color values.

Invisible-Volume pausing and explicit CPU/GPU Probe budgets are deterministic engineering extensions required to make the reference data flow schedulable in the current renderer. They preserve completed A/B state and never expose a partially written propagation hop.

## Baseline

The frozen baseline scene is `cornell_dynamic_v12.tscn`, with a `35 × 23 × 35` grid (`28,175` Probes) at `0.25 m` spacing.

| Metric | Unoptimized baseline |
| --- | ---: |
| Visibility | `0.003562 ms / hop` |
| Radiance | `0.708926 ms / 16 hops` |
| Analytic Injection | `0.015098 ms / 3 lights` |
| CPU full Geometry / Transfer rebuild | `2106.545 ms` |
| CPU Dirty Geometry / Transfer update | `111.323 ms` (`1690` Probes) |
| Dedicated Local LRT GPU memory | `14,798,584 B` (`14.113 MiB`) |
| Full rebuild upload | `16,116,708 B` |
| Dirty update upload | `2,856,072 B` |

## Retained optimizations

| Commit | Change | Method and reason | Retained result |
| --- | --- | --- | ---: |
| `3f063f1c42` | Dirty upload | GPU-clears dirty Radiance rows, skips unchanged Injection upload, caches analytic-light records. | Dirty upload `-53.0%` |
| `31f6e9c56f` | Geometry source broadphase | Rejects Color SDF sources whose conservative surface AABB cannot intersect a Probe-neighbor segment. | Full CPU `-82.8%`; Dirty CPU `-83.0%` |
| `9bd3bfae5d` | Dynamic Geometry budget | Builds a captured Dirty snapshot in deterministic x-major Probe slices and uploads only after completion. | Max Builder slice `17.565 → 3.212 ms` |
| `d99675d494` | Invisible Volume pause | Reuses per-camera Volume selection and skips all renderer updates for unselected Volumes while retaining state. | Unused Volume GPU update work removed |
| `d89d8e4ffc` | Complete-hop Probe budgets | Slices destination buffers by Probe range; swaps A/B only after a complete hop. | Radiance/frame `-91.0%` at budget `16,384` |
| `4346022ab3` | Dithered 4 / 3 phase | Uses the reference's twelve edge neighbors in three four-sample phases with fixed hash offsets and `1/3` history accumulation. | Samples `26 → 4`; Radiance `-47.9%` |
| `2e35df0265` | Screen Space Gather | Reconstructs RGB bounce, sky occlusion, and edge weight at half width/height; Base Pass bilinearly samples the cache. | Surface invocations `-75%`; viewport GPU `-26.6%` |
| `17e00ae823` | Transfer compression | Packs one FP16 luminance 4×4 matrix plus RGB8 tint; dynamic propagation remains FP32. | LTM `192 → 36 B/Probe`; Radiance `-30.5%` |
| `e1765942f5` | Trunk Scene Management | Partitions CPU grid into `8³`-Probe Trunks with 26 neighbors, local Primitive caches, and dirty/revision tracking. | Full CPU `-10.4%`; Dirty CPU `-7.9%` vs prior stage |

The Trunk scheduler clips each covered Trunk to its intersection with the exact Dirty Probe AABB. This keeps the update at `1690` Probes and seven budgeted frames instead of rebuilding all `6144` Probes in the coarse covered Trunks. The completed rectangular Dirty range is still uploaded once.

## Final measured state

| Metric | Baseline | Final |
| --- | ---: | ---: |
| CPU full Geometry / Transfer rebuild | `2106.545 ms` | `323.956 ms` (`-84.6%`) |
| CPU Dirty Geometry / Transfer update | `111.323 ms` | `17.326 ms` (`-84.4%`) |
| Maximum budgeted Dirty Builder slice | `17.565 ms` | `2.726 ms` (`-84.5%`) |
| Radiance GPU, RGB FP32 / Dithered 4 reference | `0.477818 ms` | reference retained |
| Radiance GPU, final compressed Dithered 4 | — | `0.331900 ms` |
| Dedicated Local LRT GPU memory | `14,798,584 B` | `10,403,284 B` (`-29.7%`) |
| Full rebuild upload | `16,116,708 B` | `11,721,408 B` (`-27.3%`) |
| Dirty update upload | `2,856,072 B` | `1,077,360 B` (`-62.3%`) |

Trunk CPU results are means of five independent processes, each using the median of three full rebuilds. Screen Gather uses five independent processes and `900` steady-state viewport samples per path. GPU Radiance format results use sustained profiler windows on the same hardware and scene.

## Correctness and visual acceptance

- Incremental dev build: PASS.
- Targeted Local LRT suite: `69 cases / 4634 assertions / 0 failed`.
- Forward+ GPU: Visibility, Radiance, Analytic Injection, Invisible Volume, Forward Surface, and DynamicGI composition PASS.
- Forward Mobile GPU: Visibility, Radiance, and Invisible Volume PASS.
- Complete-hop budget comparison: mean `0.00000033`, max `0.00392157`.
- Dithered 4 vs 26-neighbor reference: mean `0.00321054`, max `0.03529412`.
- Screen Gather vs direct sampling: mean `0.00101157`, max `0.19607843`, localized at geometry edges.
- Final compressed transfer vs RGB FP32: mean `0.00017567`, max `0.01960785`.
- Automated thresholds and AI visual inspection found no visible color shift, striping, stationary blotching, leakage, or energy discontinuity in the retained paths.

Frozen acceptance images:

- `probe_budget_reference.png` / `probe_budget_sliced.png`
- `neighbor_reference_26.png` / `neighbor_dithered_4.png`
- `screen_gather_reference.png` / `screen_gather_quarter_pixels.png`
- `transfer_rgb_fp32.png` / `transfer_luminance_fp16_tint.png`

Detailed commands, per-run values, implementation rationale, and stage-specific acceptance notes are retained in `benchmarks/v4_performance/README.md`; the full project state and known issues are retained in `local_lrt_volume_misc/LOCAL_LRT_STATE.md`.

## Known limitations

- Forward Mobile validates propagation but does not yet consume Local LRT in its surface shader; no Mobile visual result is claimed.
- Dynamic Geometry construction is budgeted on the main thread; asynchronous Worker construction remains unimplemented.
- Multi-source changes merge to one Dirty AABB before Trunk splitting; non-rectangular GPU copy batching remains unimplemented.
- CLI shutdown still reports the pre-existing `PipelineDeferredRD::~PipelineDeferredRD free()` cleanup errors after otherwise successful Vulkan runs.

All planned V4 performance stages through Trunk Scene Management are complete.
