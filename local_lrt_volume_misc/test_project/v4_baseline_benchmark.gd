extends SceneTree

## Records the deterministic V4 CPU, GPU-memory, and upload baseline.
##
## GPU pass timings are emitted by the engine's [code]--gpu-profile[/code]
## timestamps and are intentionally measured in a separate sustained run.

const BENCHMARK_SCENE: String = "res://cornell_dynamic_v12.tscn"
const REBUILD_SAMPLE_COUNT: int = 3
const DYNAMIC_UPDATE_PROBE_BUDGET: int = 256
const VECTOR4_BYTES: int = 16
const UINT_BYTES: int = 4
const FLOAT_BYTES: int = 4
const DIRECTIONAL_SHADOW_BYTES: int = 512 * 512 * 4
const ANALYTIC_LIGHT_VECTOR4_COUNT: int = 9
const ANALYTIC_LIGHT_COUNT: int = 3


func _initialize() -> void:
	call_deferred("_run_benchmark")


func _run_benchmark() -> void:
	var error: Error = change_scene_to_file(BENCHMARK_SCENE)
	if error != OK:
		_fail("Unable to load the V4 benchmark scene: %s" % error_string(error))
		return
	await process_frame
	await process_frame

	var scene := current_scene
	if scene == null:
		_fail("The V4 benchmark scene has no current root.")
		return
	var volume := scene.get_node_or_null("LocalLRTVolume3D") as LocalLRTVolume3D
	var dynamic_cube := scene.get_node_or_null("DynamicCube") as MeshInstance3D
	if volume == null or dynamic_cube == null:
		_fail("The V4 benchmark scene is missing its volume or dynamic cube.")
		return

	var rebuild_samples_usec: Array[int] = []
	for _sample_index: int in REBUILD_SAMPLE_COUNT:
		volume.rebuild()
		rebuild_samples_usec.push_back(int(volume.get_last_geometry_update_usec()))
	rebuild_samples_usec.sort()
	var median_rebuild_usec: int = rebuild_samples_usec[REBUILD_SAMPLE_COUNT >> 1]

	var initial_transform: Transform3D = dynamic_cube.transform
	volume.set_dynamic_update_probe_budget(DYNAMIC_UPDATE_PROBE_BUDGET)
	dynamic_cube.position += Vector3(0.75, 0.0, 0.5)
	dynamic_cube.rotate_y(deg_to_rad(25.0))
	await process_frame
	while volume.is_geometry_update_pending():
		await process_frame
	var dirty_probe_count: int = volume.get_last_geometry_update_probe_count()
	var dirty_update_usec: int = int(volume.get_last_geometry_update_usec())
	var dirty_frame_count: int = volume.get_last_geometry_update_frame_count()
	var dirty_max_build_slice_usec: int = int(volume.get_last_geometry_max_build_slice_usec())
	var dirty_build_usec: int = int(volume.get_last_geometry_build_usec())
	var dirty_pack_usec: int = int(volume.get_last_geometry_pack_usec())
	var dirty_upload_usec: int = int(volume.get_last_geometry_upload_usec())
	var dirty_source_usec: int = dirty_update_usec - dirty_build_usec - dirty_pack_usec - dirty_upload_usec
	dynamic_cube.transform = initial_transform

	var resolution: Vector3i = volume.get_resolution()
	var probe_count: int = resolution.x * resolution.y * resolution.z
	var gpu_memory_bytes: int = _estimate_gpu_memory_bytes(probe_count)
	var full_rebuild_upload_bytes: int = _estimate_full_rebuild_upload_bytes(probe_count)
	var dirty_update_upload_bytes: int = _estimate_dirty_update_upload_bytes(probe_count, dirty_probe_count)
	print(
		"LOCAL_LRT_V4_BASELINE_PASS resolution=%s probes=%d rebuild_median_ms=%.3f rebuild_samples_ms=%s dirty_probes=%d dirty_budget=%d dirty_frames=%d dirty_ms=%.3f dirty_max_build_slice_ms=%.3f dirty_source_ms=%.3f dirty_build_ms=%.3f dirty_pack_ms=%.3f dirty_upload_ms=%.3f gpu_memory_bytes=%d full_rebuild_upload_bytes=%d dirty_update_upload_bytes=%d stable_frame_upload_bytes=128" % [
			resolution,
			probe_count,
			float(median_rebuild_usec) / 1000.0,
			_rebuild_samples_msec(rebuild_samples_usec),
			dirty_probe_count,
			DYNAMIC_UPDATE_PROBE_BUDGET,
			dirty_frame_count,
			float(dirty_update_usec) / 1000.0,
			float(dirty_max_build_slice_usec) / 1000.0,
			float(dirty_source_usec) / 1000.0,
			float(dirty_build_usec) / 1000.0,
			float(dirty_pack_usec) / 1000.0,
			float(dirty_upload_usec) / 1000.0,
			gpu_memory_bytes,
			full_rebuild_upload_bytes,
			dirty_update_upload_bytes,
		]
	)
	quit()


func _estimate_gpu_memory_bytes(probe_count: int) -> int:
	var vector4_values_per_probe: int = 1 + 12 + 3 + 2 + 6 + 3 + 3
	var uint_values_per_probe: int = 1
	var float_values_per_probe: int = 1
	var fixed_vector4_values: int = 3 + 4 + ANALYTIC_LIGHT_COUNT * ANALYTIC_LIGHT_VECTOR4_COUNT
	var fixed_float_values: int = 16
	return (
		probe_count * vector4_values_per_probe * VECTOR4_BYTES
		+ probe_count * uint_values_per_probe * UINT_BYTES
		+ probe_count * float_values_per_probe * FLOAT_BYTES
		+ fixed_vector4_values * VECTOR4_BYTES
		+ fixed_float_values * FLOAT_BYTES
		+ DIRECTIONAL_SHADOW_BYTES
	)


func _estimate_full_rebuild_upload_bytes(probe_count: int) -> int:
	var allocated_bytes_without_shadow_texture: int = _estimate_gpu_memory_bytes(probe_count) - DIRECTIONAL_SHADOW_BYTES
	var visibility_reset_bytes: int = probe_count * 2 * VECTOR4_BYTES
	var inside_solid_update_bytes: int = probe_count * UINT_BYTES
	var injection_update_bytes: int = probe_count * 3 * VECTOR4_BYTES
	return allocated_bytes_without_shadow_texture + visibility_reset_bytes + inside_solid_update_bytes + injection_update_bytes


func _estimate_dirty_update_upload_bytes(probe_count: int, dirty_probe_count: int) -> int:
	var dirty_values_per_probe: int = 1 + 12 + 3
	var dirty_bytes: int = dirty_probe_count * (dirty_values_per_probe * VECTOR4_BYTES + UINT_BYTES)
	var full_visibility_reset_bytes: int = probe_count * 2 * VECTOR4_BYTES
	return dirty_bytes + full_visibility_reset_bytes


func _rebuild_samples_msec(samples_usec: Array[int]) -> String:
	var samples_msec: PackedStringArray = []
	for sample_usec: int in samples_usec:
		samples_msec.push_back("%.3f" % (float(sample_usec) / 1000.0))
	return ",".join(samples_msec)


func _fail(message: String) -> void:
	push_error("LOCAL_LRT_V4_BASELINE_FAIL: %s" % message)
	quit(1)
