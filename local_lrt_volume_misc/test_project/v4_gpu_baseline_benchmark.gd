extends SceneTree

## Sustains the unoptimized V4 GPU passes for [code]--gpu-profile[/code].
##
## The grid and radiance budget match [code]cornell_dynamic_v12.tscn[/code].
## Visibility runs ten hops per sample so its sub-0.01 ms single-hop cost clears
## the built-in profiler threshold; reports normalize that result per hop.

const BENCHMARK_SCENE: String = "res://cornell_dynamic_v12.tscn"
const RESOLUTION := Vector3i(35, 23, 35)
const SIZE := Vector3(8.5, 5.5, 8.5)
const VISIBILITY_ITERATIONS: int = 10
const RADIANCE_ITERATIONS: int = 16
const VISIBILITY_RESET_INTERVAL: int = 1
const BENCHMARK_FRAMES: int = 360

var _volume: RID
var _local_visibility: PackedVector4Array
var _local_transfer: PackedVector4Array
var _mesh_light: PackedVector4Array
var _inside_solid: PackedInt32Array
var _injection: PackedVector4Array
var _lights: PackedVector4Array
var _frame_index: int = 0
var _started: bool = false


func _initialize() -> void:
	call_deferred("_start_benchmark")


func _process(_delta: float) -> bool:
	if not _started:
		return false
	if _frame_index % VISIBILITY_RESET_INTERVAL == 0:
		RenderingServer.local_lrt_volume_set_static_data(_volume, _local_visibility, _local_transfer, _mesh_light)
		RenderingServer.local_lrt_volume_set_inside_solid(_volume, _inside_solid)
	RenderingServer.local_lrt_volume_set_injection(_volume, _injection)
	RenderingServer.local_lrt_volume_inject_analytic_lights(_volume, _lights)
	RenderingServer.local_lrt_volume_propagate_visibility(_volume)
	RenderingServer.local_lrt_volume_propagate_radiance(_volume)
	_frame_index += 1
	if _frame_index < BENCHMARK_FRAMES:
		return false

	RenderingServer.free_rid(_volume)
	print("LOCAL_LRT_V4_GPU_BASELINE_PASS frames=%d probes=%d visibility_iterations=%d radiance_iterations=%d lights=3" % [
		BENCHMARK_FRAMES,
		_probe_count(),
		VISIBILITY_ITERATIONS,
		RADIANCE_ITERATIONS,
	])
	quit()
	return true


func _start_benchmark() -> void:
	var error: Error = change_scene_to_file(BENCHMARK_SCENE)
	if error != OK:
		push_error("LOCAL_LRT_V4_GPU_BASELINE_FAIL: %s" % error_string(error))
		quit(1)
		return
	await process_frame
	await process_frame
	var scene_volume := current_scene.get_node_or_null("LocalLRTVolume3D") as LocalLRTVolume3D
	if scene_volume == null:
		push_error("LOCAL_LRT_V4_GPU_BASELINE_FAIL: The benchmark scene is missing its volume.")
		quit(1)
		return
	scene_volume.enabled = false

	_volume = RenderingServer.local_lrt_volume_create()
	if not _volume.is_valid():
		push_error("LOCAL_LRT_V4_GPU_BASELINE_FAIL: Local LRT requires a RenderingDevice renderer.")
		quit(1)
		return

	_create_probe_data()
	_lights = _create_analytic_lights()
	RenderingServer.local_lrt_volume_set_grid(_volume, SIZE, RESOLUTION)
	RenderingServer.local_lrt_volume_set_visibility_iterations(_volume, VISIBILITY_ITERATIONS)
	RenderingServer.local_lrt_volume_set_propagation_iterations(_volume, RADIANCE_ITERATIONS)
	_started = true


func _create_probe_data() -> void:
	var probe_count: int = _probe_count()
	_local_visibility.resize(probe_count)
	_local_transfer.resize(probe_count * 12)
	_mesh_light.resize(probe_count * 3)
	_inside_solid.resize(probe_count)
	_injection.resize(probe_count * 3)
	for probe_index: int in probe_count:
		_local_visibility[probe_index] = Vector4(3.5449078, 0.0, 0.0, 0.0)


func _create_analytic_lights() -> PackedVector4Array:
	var lights := PackedVector4Array()
	_append_light(lights, 1, Vector3(0.0, -1.0, 0.0), 0.0, Vector3.ZERO, 0.0)
	_append_light(lights, 2, Vector3(0.5, 1.5, 0.5), 6.0, Vector3.ZERO, 0.0)
	_append_light(lights, 3, Vector3(-0.5, 1.5, 0.5), 6.0, Vector3(0.0, -1.0, 0.0), cos(deg_to_rad(35.0)))
	return lights


func _append_light(
	lights: PackedVector4Array,
	light_type: int,
	vector: Vector3,
	range_value: float,
	spot_direction: Vector3,
	cone_limit: float
) -> void:
	lights.push_back(Vector4(float(light_type), 1.0, range_value, cone_limit))
	lights.push_back(Vector4(1.0, 1.0, 1.0, 0.0))
	lights.push_back(Vector4(vector.x, vector.y, vector.z, 1.0))
	lights.push_back(Vector4(spot_direction.x, spot_direction.y, spot_direction.z, 1.0))
	lights.push_back(Vector4(1.0, 0.0, 0.0, 0.0))
	lights.push_back(Vector4(0.0, 1.0, 0.0, 0.0))
	lights.push_back(Vector4(0.0, 0.0, 1.0, 0.0))
	lights.push_back(Vector4.ZERO)
	lights.push_back(Vector4.ZERO)


func _probe_count() -> int:
	return RESOLUTION.x * RESOLUTION.y * RESOLUTION.z
