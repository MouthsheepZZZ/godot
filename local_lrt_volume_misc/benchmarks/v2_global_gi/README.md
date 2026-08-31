# Local LRT v2 Global GI Benchmark

This benchmark isolates constant World diffuse input in an open Cornell box. All analytic lights, the ceiling, and the emission panel are disabled. Godot and Cycles use the same neutral `#808080` lighting radiance after sRGB-to-linear conversion.

## Files

- `cornell_global_v2_cycles.blend`: Cycles constant-World reference.
- `godot_v2_world_pose_a_agx.png`: Godot constant-World result.
- `godot_v2_sky_rotated_agx.png`: repeated constant-World invariance capture.
- `godot_v2_environment_injection_debug.png`: Local LRT Environment Injection probes.
- `blender_v2_world_pose_a_agx.png`: Cycles constant-World result.
- `blender_v2_sky_rotated_agx.png`: repeated constant-World invariance capture.

The Godot scene is `res://cornell_global_v2.tscn`. Press `R` for Volume Y90 plus rebuild, `E` for the constant-World repeat, and `V` for probe debug.

## Frozen setup

- Godot World background / ambient: `#808080`, energy `1.0`.
- Cycles lighting rays: linear gray `0.2158605`, strength `1.0`.
- The Cycles camera-ray gray is separately color-managed to `0.38`, producing the same AgX background pixel `164` as Godot; it does not change lighting rays.
- Local LRT size `10.5 x 7.5 x 10.5 m`, spacing `0.25 m`, Global Visibility iterations `24`, edge blend `1.0 m`, benchmark energy `2.0`.

## Numeric checks

- Constant environment injection is RGB-neutral and invariant under the repeat pose.
- Top / bottom environment R constant terms: `0.765042 / 0.400355`.
- Godot / Cycles background pixels both equal `(164, 164, 164)`.
- Representative Godot / Cycles pixels: back wall `(61,58,57) / (61,60,59)`, floor `(68,67,66) / (55,56,54)`, short box `(50,48,47) / (62,60,59)`, tall box `(53,53,50) / (54,53,52)`.
