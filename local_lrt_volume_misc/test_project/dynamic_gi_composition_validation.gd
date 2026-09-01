extends SceneTree

## Validates that Local LRT replaces DynamicGI diffuse only inside its Volume.
##
## The zero-energy Local LRT state makes an accidental DynamicGI overwrite
## visible while preserving DynamicGI execution and its scene contribution.

const GI_SETTLE_FRAMES: int = 60
const STATE_SETTLE_FRAMES: int = 4
const PIXEL_STEP: int = 8
const MIN_DYNAMIC_GI_LUMINANCE: float = 0.01
const MIN_OVERRIDE_DIFFERENCE: float = 0.0001
const MIN_OVERRIDE_TO_DRIFT_RATIO: float = 4.0
const LOCAL_LRT_EMISSION_CAPTURE_PATH: String = "res://../benchmarks/v2_global_gi/godot_emission_mesh_local_lrt_restored.png"
const DYNAMIC_GI_CAPTURE_PATH: String = "res://../benchmarks/v2_global_gi/godot_dynamic_gi_only_emission.png"
const LOCAL_LRT_CAPTURE_PATH: String = "res://../benchmarks/v2_global_gi/godot_dynamic_gi_lrt_zero_energy_override_emission.png"


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var error := change_scene_to_file("res://cornell_v0_emission_mesh.tscn")
	if error != OK:
		_fail("Could not load emission Cornell Box: %s" % error_string(error))
		return
	await scene_changed

	var world_environment := current_scene.get_node("ReferenceEnvironment") as WorldEnvironment
	var volume := current_scene.get_node("LocalLRTVolume3D") as LocalLRTVolume3D
	world_environment.environment.dynamic_gi_enabled = false
	volume.debug_draw = false
	volume.energy = 1.0
	volume.enabled = true
	await _wait_for_frames(GI_SETTLE_FRAMES)
	var gpu_radiance: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume.get_rid())
	var max_l0: float = -INF
	for coefficient in gpu_radiance:
		max_l0 = maxf(max_l0, coefficient.x)
	if max_l0 <= 0.0:
		_fail("Emission Mesh produced no positive radiance L0: %.8f" % max_l0)
		return
	var local_lrt_emission := await _capture_viewport()
	await _wait_for_frames(STATE_SETTLE_FRAMES)
	var local_lrt_emission_repeat := await _capture_viewport()
	var local_lrt_emission_drift := _mean_difference(local_lrt_emission, local_lrt_emission_repeat)
	volume.enabled = false
	await _wait_for_frames(STATE_SETTLE_FRAMES)
	var emission_base_pass := await _capture_viewport()
	var emission_difference := _mean_difference(local_lrt_emission_repeat, emission_base_pass)
	var required_emission_difference := maxf(MIN_OVERRIDE_DIFFERENCE, local_lrt_emission_drift * MIN_OVERRIDE_TO_DRIFT_RATIO)
	if emission_difference <= required_emission_difference:
		_fail("Emission Mesh radiance was not consumed by Local LRT: %.8f <= %.8f" % [emission_difference, required_emission_difference])
		return
	if local_lrt_emission_repeat.save_png(ProjectSettings.globalize_path(LOCAL_LRT_EMISSION_CAPTURE_PATH)) != OK:
		_fail("Could not save Local LRT Emission Mesh capture")
		return

	world_environment.environment.dynamic_gi_enabled = true
	world_environment.environment.dynamic_gi_energy = 16.0
	await _wait_for_frames(GI_SETTLE_FRAMES)
	var dynamic_gi_only := await _capture_viewport()
	if _mean_luminance(dynamic_gi_only) <= MIN_DYNAMIC_GI_LUMINANCE:
		_fail("DynamicGI-only reference did not produce visible lighting")
		return
	await _wait_for_frames(STATE_SETTLE_FRAMES)
	var dynamic_gi_repeat := await _capture_viewport()
	var dynamic_gi_drift := _mean_difference(dynamic_gi_only, dynamic_gi_repeat)
	if dynamic_gi_only.save_png(ProjectSettings.globalize_path(DYNAMIC_GI_CAPTURE_PATH)) != OK:
		_fail("Could not save DynamicGI-only capture")
		return

	volume.energy = 0.0
	volume.edge_blend_distance = 0.0
	volume.enabled = true
	await _wait_for_frames(STATE_SETTLE_FRAMES)
	var local_lrt_override := await _capture_viewport()
	var override_difference := _mean_difference(dynamic_gi_only, local_lrt_override)
	var required_difference := maxf(MIN_OVERRIDE_DIFFERENCE, dynamic_gi_drift * MIN_OVERRIDE_TO_DRIFT_RATIO)
	if override_difference <= required_difference:
		_fail("DynamicGI diffuse still overwrote Local LRT: %.8f <= %.8f" % [override_difference, required_difference])
		return
	if not world_environment.environment.dynamic_gi_enabled:
		_fail("Local LRT disabled DynamicGI instead of replacing its diffuse result")
		return
	if local_lrt_override.save_png(ProjectSettings.globalize_path(LOCAL_LRT_CAPTURE_PATH)) != OK:
		_fail("Could not save Local LRT override capture")
		return

	print("LOCAL_LRT_DYNAMIC_GI_COMPOSITION_PASS emission=%.8f difference=%.8f drift=%.8f" % [emission_difference, override_difference, dynamic_gi_drift])
	quit()


func _wait_for_frames(frame_count: int) -> void:
	for _frame in frame_count:
		await process_frame


func _capture_viewport() -> Image:
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()


func _mean_luminance(image: Image) -> float:
	var luminance: float = 0.0
	var sample_count: int = 0
	for y in range(0, image.get_height(), PIXEL_STEP):
		for x in range(0, image.get_width(), PIXEL_STEP):
			luminance += image.get_pixel(x, y).get_luminance()
			sample_count += 1
	return luminance / float(sample_count)


func _mean_difference(first: Image, second: Image) -> float:
	var difference: float = 0.0
	var sample_count: int = 0
	for y in range(0, first.get_height(), PIXEL_STEP):
		for x in range(0, first.get_width(), PIXEL_STEP):
			var delta := first.get_pixel(x, y) - second.get_pixel(x, y)
			difference += absf(delta.r) + absf(delta.g) + absf(delta.b)
			sample_count += 3
	return difference / float(sample_count)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
