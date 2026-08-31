extends SceneTree

## Runs deterministic Global Visibility GPU validation against pinned CPU-reference values.

const SH_Y00: float = 0.28209479177387814
const RESOLUTION := Vector3i(3, 3, 3)
const CENTER := Vector3i(1, 1, 1)
const RIGHT := Vector3i(2, 1, 1)
const BLOCKED := Vector3i(0, 1, 1)
const ITERATIONS: Array[int] = [1, 2, 4, 8]
const EPSILON: float = 0.0002


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var volume: RID = RenderingServer.local_lrt_volume_create()
	if not volume.is_valid():
		_fail("Local LRT requires a RenderingDevice renderer.")
		return

	RenderingServer.local_lrt_volume_set_grid(volume, Vector3(2.0, 2.0, 2.0), RESOLUTION)
	var local_visibility := _create_local_visibility()
	var local_transfer := PackedVector4Array()
	local_transfer.resize(_probe_count() * 12)
	var mesh_light := PackedVector4Array()
	mesh_light.resize(_probe_count() * 3)

	for iteration: int in ITERATIONS:
		RenderingServer.local_lrt_volume_set_visibility_iterations(volume, iteration)
		RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light)
		var result: PackedVector4Array = RenderingServer.local_lrt_volume_get_global_visibility(volume)
		if not _validate_iteration(iteration, result):
			RenderingServer.free_rid(volume)
			return

	RenderingServer.free_rid(volume)
	print("LOCAL_LRT_GPU_VISIBILITY_PASS iterations=1,2,4,8 probes=27")
	quit()


func _create_local_visibility() -> PackedVector4Array:
	var values := PackedVector4Array()
	values.resize(_probe_count())
	for index: int in values.size():
		values[index] = Vector4(1.0 / SH_Y00, 0.0, 0.0, 0.0)
	values[_probe_index(CENTER)] = Vector4(1.2, 0.2, -0.1, 0.05)
	values[_probe_index(BLOCKED)] = Vector4.ZERO
	return values


func _validate_iteration(iteration: int, result: PackedVector4Array) -> bool:
	if result.size() != _probe_count():
		_fail("Unexpected probe count for iteration %d: %d" % [iteration, result.size()])
		return false
	for value: Vector4 in result:
		if not is_finite(value.x) or not is_finite(value.y) or not is_finite(value.z) or not is_finite(value.w):
			_fail("Non-finite Global Visibility at iteration %d" % iteration)
			return false

	var expected_center := _expected_center(iteration)
	var expected_right := _expected_right(iteration)
	if result[_probe_index(CENTER)].distance_to(expected_center) > EPSILON:
		_fail("Center mismatch at iteration %d: %s != %s" % [iteration, result[_probe_index(CENTER)], expected_center])
		return false
	if result[_probe_index(RIGHT)].distance_to(expected_right) > EPSILON:
		_fail("Direction mismatch at iteration %d: %s != %s" % [iteration, result[_probe_index(RIGHT)], expected_right])
		return false
	if not result[_probe_index(BLOCKED)].is_equal_approx(Vector4.ZERO):
		_fail("Blocked probe propagated visibility at iteration %d" % iteration)
		return false
	if result[_probe_index(RIGHT)].y <= 0.0 or result[_probe_index(RIGHT)].z >= 0.0 or result[_probe_index(RIGHT)].w <= 0.0:
		_fail("Directional SH signs are incorrect at iteration %d" % iteration)
		return false
	return true


func _expected_center(iteration: int) -> Vector4:
	match iteration:
		1:
			return Vector4(1.137186204, 0.189531034, -0.094765517, 0.047382759)
		2:
			return Vector4(1.078678344, 0.182222519, -0.091111259, 0.045555630)
		4:
			return Vector4(1.043029659, 0.177457363, -0.088728682, 0.044364341)
		8:
			return Vector4(1.034423106, 0.176112762, -0.088056381, 0.044028191)
	return Vector4.ZERO


func _expected_right(iteration: int) -> Vector4:
	match iteration:
		1:
			return Vector4(3.422163907, 0.010468966, -0.005234483, 0.002617241)
		2:
			return Vector4(3.329170051, 0.014810653, -0.007405326, 0.003702663)
		4:
			return Vector4(3.258049572, 0.016732752, -0.008366376, 0.004183188)
		8:
			return Vector4(3.239646362, 0.016789329, -0.008394664, 0.004197332)
	return Vector4.ZERO


func _probe_index(position: Vector3i) -> int:
	return position.x + RESOLUTION.x * (position.y + RESOLUTION.y * position.z)


func _probe_count() -> int:
	return RESOLUTION.x * RESOLUTION.y * RESOLUTION.z


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
