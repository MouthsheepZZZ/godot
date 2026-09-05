# P2 Directional Shadow Validation

This benchmark renders the Local LRT directional-shadow Probe debug view with a caster placed beyond the former fixed 8 m extrusion but within the DirectionalLight shadow distance.

- `directional_shadow_before.png`: fixed-extrusion implementation.
- `directional_shadow_after.png`: per-camera, per-volume caster collection using the light shadow distance.

Both captures use the same programmatic scene, camera, Volume, light, and caster.

The caster is centered 12 m from the Volume. The DirectionalLight shadow distance is 32 m.
The former fixed 8 m extrusion misses it completely; the corrected implementation uses the
light distance and produces the expected shadowed probes.

- Before mean visibility: `1.00000000`.
- After mean visibility: `0.20000000`.
- After validation also checks `shadow_caster_mask`, camera cull layers, and caster-transform cache invalidation.

Run the corrected capture with:

```text
bin/godot.windows.editor.dev.x86_64.console.exe --path local_lrt_volume_misc/test_project --rendering-method mobile --rendering-driver vulkan --script res://p2_directional_shadow_capture.gd -- after
```
