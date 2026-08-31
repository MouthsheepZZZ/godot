# Local LRT v2 Global GI Benchmark

This benchmark isolates World/Sky diffuse input in an open Cornell box. All analytic lights, the ceiling, and the emission panel are disabled.

## Files

- `cornell_global_v2_cycles.blend`: Cycles reference with a directional gradient World.
- `godot_v2_world_pose_a_agx.png`: Godot authored Sky orientation.
- `godot_v2_sky_rotated_agx.png`: Godot Sky rotated 90 degrees around X.
- `godot_v2_environment_injection_debug.png`: Local LRT Environment Injection probes.
- `blender_v2_world_pose_a_agx.png`: Cycles authored World orientation.
- `blender_v2_sky_rotated_agx.png`: Cycles World rotated 90 degrees around X.

The Godot scene is `res://cornell_global_v2.tscn`. Press `E` for Sky X90, `R` for Volume Y90 plus rebuild, and `V` for probe debug.

## Numeric checks

- Top probe environment R constant: `0.912066`; bottom probe: `0.2077`.
- Sky X90 rotates the R first-order term from local Z `0.0161373` to local X `0.0161372`; the constant term remains `0.912066`.
- Constant ambient energy `0.5 -> 1.0` produces an exact `2x` SH response.
