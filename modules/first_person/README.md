# First-person rendering (Forward+)

This module implements the behavior described in Epic's [First Person Rendering documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/first-person-rendering). It is an independent implementation, not a port of Unreal Engine source.

## Setup

1. Use the Forward+ renderer and a perspective `Camera3D`.
2. Enable `first_person_enabled` on the camera and on each view-model `GeometryInstance3D` (including held-item meshes).
3. Set `Camera3D.first_person_fov` independently of `Camera3D.fov`. It follows the camera's existing `keep_aspect` convention.
4. Author meshes and animation in their normal dimensions. Set `first_person_scale` so the rendered geometry fits inside the player collision shape. Do not also shrink the view-model root unless this is intentional.
5. Keep the camera near plane closer than the scaled geometry. For example, a model 0.6 m from the camera with scale 0.05 is only 0.03 m away after rendering transforms.
6. Enable shadows on the relevant lights. Configure the world's Environment/GI normally.

The prototype uses root scale 1 and camera `first_person_scale = 0.05`. Its view-model root script automatically marks newly equipped and refreshed geometry.

### Properties

| Owner | Property | Default | Meaning |
| --- | --- | --- | --- |
| GeometryInstance3D | `first_person_enabled` | false | Marks this geometry as a view model. Does not recursively mark children. |
| Camera3D | `first_person_enabled` | false | Enables the first-person transform for this view. |
| Camera3D | `first_person_fov` | 75 | First-person field of view in degrees. |
| Camera3D | `first_person_scale` | 0.1 | Uniform camera-centered scale, applied after FOV correction. |
### Project Settings: Rendering > Lights and Shadows > First Person Shadow

All settings below use the prefix `rendering/lights_and_shadows/first_person_shadow/` and take effect at runtime without restarting. Distance and thickness now live here, not on Camera3D.

| Setting | Default | Meaning |
| --- | --- | --- |
| `enabled` | true | Self-shadow computation; independent of incoming world shadows. |
| `half_resolution` | true | Trace at ceil(width/2) × ceil(height/2), then reconstruct with depth/normal guidance. Disable for full-resolution tracing. Models and caster depth remain full resolution. |
| `block_skipping` | true | Conservatively skip 8×8 depth blocks. Disabling provides an exact DDA reference for comparisons. |
| `distance` | 1.0 | Maximum ray length in unscaled model-space meters; zero disables tracing. |
| `thickness` | 0.05 | Assumed occluder thickness in unscaled model-space meters. |

Both distances are multiplied by each camera's first-person scale.

World shadow casting is suppressed for marked geometry regardless of the camera setting. The authored `cast_shadow` value is retained and restored when the geometry flag is disabled. Non-disabled GI modes are treated as dynamic while the flag is enabled, keeping view models out of static SDFGI geometry. The authored GI mode is retained.

## Rendering and limitations

- Rendering uses the same world, materials, direct lights, shadow maps and GI as ordinary scene geometry. There is no SubViewport or duplicated lighting scene.
- Camera-space vertices are scaled and their XY coordinates corrected for the independent FOV. Normals/tangents and previous-frame transforms follow that correction.
- A full-resolution depth-only pass of opaque/alpha-tested first-person surfaces supplies a separate caster layer. After the current frame's light buffers are updated, compute passes prepare receivers and per-light shadow masks. Directional, omni, spot and area lights have separate masks; area lights use a center ray. Local lights outside the transformed view-model bounds are excluded.
- Self-shadow rays use pixel-sized, perspective-correct DDA steps, clipped to the near plane and viewport. An 8×8 min/max depth grid includes a one-texel halo covering the full bilinear footprint. Entire ray segments are skipped only when their depth interval cannot overlap the block. Remaining steps compare four depths before filtering coverage; fully occluded rays exit early.
- Half resolution reduces receiver ray count to about one quarter without coarsening caster depth. The opaque pass reconstructs each light's visibility using plane-distance and normal weights. Very thin receivers or shadow details can be lost at half resolution; disabling it improves detail. Mask memory grows with the number of shadow-enabled lights affecting the view model. Multiview uses separate receiver and mask layers per eye; hardware XR validation has not been performed.
- The main world shadow maps supply incoming world shadows. Native screen-space contact-shadow casters matching the first-person layer are excluded from world shadowing.
- Self-shadows have the usual single-layer screen-space limitations: offscreen and hidden back layers cannot cast; thickness and ray distance affect artifacts. Alpha-blended geometry without a depth prepass is not a caster.
- Scaling prevents intersection only while the transformed geometry remains within unobstructed player bounds. It is not an unconditional depth-test override.
- GI is evaluated at the transformed position near the camera, as are local-light attenuation and world-shadow sampling.
- The implementation targets Forward+ perspective cameras. Mobile, Compatibility and orthographic cameras do not implement this mode. It does not create a third-person/world-space representation automatically.
- Screen-space effects retain their normal depth-buffer limitations. Custom shaders that override `POSITION` are responsible for their own projection.

## Engine integration / rebasing

Implementation lives in this directory. `forward_clustered_adapter.inc` contains the render-list coupling instead of copying the renderer.

The small upstream hooks carry:

- Node method declarations, binding calls and lifetime cleanup.
- Source instance RID identity from scene culling to the renderer.
- First-person camera parameters through `CameraData` and render-scene data.
- Two vec4s in Forward+'s implementation UBO (current and previous parameters).
- Instance flag bit 1 and render-pass bindings 39–43 (depth, light-to-mask mapping, masks, receiver positions, normals).
- A dedicated depth render list and depth-pass invocation, followed by module-owned compute passes after light-buffer updates.
- Read-only accessors for packed light data; no changes to upstream light-buffer layouts.
- Shader transform and per-light shadow hooks; contact-shadow caster exclusion.

When rebasing, check for upstream use of instance flag bit 1 or render-pass bindings 39–43, UBO layout changes, render-list buffer initialization, and changes to the depth-only material path. Keep C++ and GLSL layouts synchronized.

`module_first_person_enabled=no` removes the node-facing feature and module execution. The small renderer transport fields and shader hooks remain inert with zero camera parameters.

## Build and GPU regression test

```powershell
scons platform=windows target=editor dev_build=yes debug_symbols=no module_mono_enabled=no -j12
.\bin\godot.windows.editor.dev.x86_64.console.exe --path G:\MyGameProjects\project_lamplit_line\project_lamplit_line --rendering-method forward_plus --rendering-driver vulkan --script "$PWD/modules/first_person/tests/render_validation.gd"
```

Verified on NVIDIA RTX 5080 with both Vulkan and D3D12: all 26 assertions passed. These include exact block-skipping comparisons at full and half resolution, actual GPU mask dimensions, odd viewport sizes, runtime setting changes, mixed-light layer isolation, directional/omni/spot/area shadows, SDFGI and MSAA 4x. TAA and camera enable/disable received rendering smoke checks. The saved prototype configuration also ran through Godot MCP with a backpack equipped.

Append `-- --benchmark` to run an optional 1920×1080 benchmark (90 warm-up frames and 120 samples per configuration, no MSAA/TAA/VSync). One Vulkan run measured whole-viewport GPU medians of 0.917 ms (full/exact), 0.345 ms (full/blocks), 0.510 ms (half/exact), and 0.247 ms (half/blocks). These are a small synthetic scene on RTX 5080, not a guarantee for gameplay or a comparison against the previous inline-fragment implementation.

The test creates its scene programmatically without modifying the game prototype. It checks independent FOV, size-preserving scaling, near-wall visibility, directional/omni/spot self-shadows, subpixel shadow-edge coverage, incoming world shadows, outgoing world-shadow exclusion, SDFGI, MSAA and camera enable/disable transitions. Captures are saved under `OS.get_cache_dir()/godot-first-person-validation`. Rendering tests require a GPU/window and must not use `--headless`.
