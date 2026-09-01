# Local LRT v2 Global GI Benchmark

This benchmark isolates constant World diffuse input in an open Cornell box. All analytic lights, the ceiling, and the emission panel are disabled. Godot and Cycles use the same neutral `#808080` lighting radiance after sRGB-to-linear conversion.

## Files

- `cornell_global_v2_cycles.blend`: Cycles constant-World reference.
- `godot_v2_world_pose_a_agx.png`: Godot constant-World result.
- `godot_v2_sky_rotated_agx.png`: repeated constant-World invariance capture.
- `godot_v2_environment_injection_debug.png`: Local LRT Environment Injection probes.
- `godot_v2_sky_occlusion_corner_bounded.png`: close-up regression for the bounded positive sky-occlusion closure.
- `godot_emission_mesh_local_lrt_restored.png`: Emission-only Cornell capture proving that MeshLight radiance remains visible through Local LRT after the DynamicGI composition fix; the full segment-hit path uses the `2.0` MeshLight scale calibrated against the frozen Cycles Strength `8` reference.
- `godot_dynamic_gi_only_emission.png`: DynamicGI-only emission Cornell capture with Local LRT disabled.
- `godot_dynamic_gi_lrt_zero_energy_override_emission.png`: controlled composition capture with DynamicGI still enabled and a full-volume Local LRT override at zero energy; the removed DynamicGI diffuse confirms that Local LRT owns diffuse shading inside the Volume.
- `blender_v2_world_pose_a_agx.png`: Cycles constant-World result.
- `blender_v2_sky_rotated_agx.png`: repeated constant-World invariance capture.

The Godot scene is `res://cornell_global_v2.tscn`. Press `R` for Volume Y90 plus rebuild, `E` for the constant-World repeat, and `V` for probe debug.

## Frozen setup

- Godot World background / ambient: `#808080`, energy `1.0`.
- Cycles lighting rays: linear gray `0.2158605`, strength `1.0`.
- The Cycles camera-ray gray is separately color-managed to `0.38`, producing the same AgX background pixel `164` as Godot; it does not change lighting rays.
- Local LRT size `10.5 x 7.5 x 10.5 m`, spacing `0.25 m`. Global Visibility uses a `1` Probe-hop per-frame budget and automatically completes after the grid's nearest-sky-boundary radius (`15` steps here); Radiance propagation uses `16` steps per rendered frame. Edge blend is `1.0 m`, benchmark energy is `2.0`.
- The paper specifies separately propagated directional Global Visibility and sky occlusion in Screen Space Gather A, but not the L1-to-scalar closure. This implementation bounds the first moment to `1/3` and evaluates scalar A with a positive maximum-entropy closure, preserving the average while preventing linear SH negative lobes from becoming periodic black intervals.

## Numeric checks

- Constant environment injection is RGB-neutral and invariant under the repeat pose.
- Top / bottom environment R constant terms: `0.765042 / 0.400355`.
- Godot / Cycles background pixels both equal `(164, 164, 164)`.
- Representative Godot / Cycles pixels: back wall `(61,58,57) / (61,60,59)`, floor `(68,67,66) / (55,56,54)`, short box `(50,48,47) / (62,60,59)`, tall box `(53,53,50) / (54,53,52)`.
