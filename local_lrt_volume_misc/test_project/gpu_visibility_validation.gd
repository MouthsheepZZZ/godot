extends SceneTree

## Validates frame-budgeted Global Visibility propagation against a CPU reference.

const SH_Y00: float = 0.28209479177387814
const RESOLUTION := Vector3i(9, 9, 9)
const CENTER := Vector3i(4, 4, 4)
const BLOCKED := Vector3i(0, 4, 4)
const COMPLETION_STEPS: int = 4
const EPSILON: float = 0.0002


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var local_visibility: PackedVector4Array = _create_local_visibility()
	var cpu_states: Array[PackedVector4Array] = [local_visibility]
	for step: int in COMPLETION_STEPS:
		cpu_states.append(_propagate_cpu(cpu_states.back(), local_visibility))

	var volume: RID = _create_volume(Vector3(8.0, 8.0, 8.0), local_visibility)
	if not volume.is_valid():
		_fail("Local LRT requires a RenderingDevice renderer.")
		return

	RenderingServer.local_lrt_volume_set_visibility_iterations(volume, 1)
	for step: int in COMPLETION_STEPS:
		RenderingServer.local_lrt_volume_propagate_visibility(volume)
		var gpu_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_global_visibility(volume)
		if not _validate_equal("step %d" % (step + 1), gpu_result, cpu_states[step + 1]):
			RenderingServer.free_rid(volume)
			return

	var completed_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_global_visibility(volume)
	for step: int in 8:
		RenderingServer.local_lrt_volume_propagate_visibility(volume)
	var stable_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_global_visibility(volume)
	if not _validate_equal("completed propagation", stable_result, completed_result):
		RenderingServer.free_rid(volume)
		return

	RenderingServer.local_lrt_volume_set_visibility_iterations(volume, 2)
	_upload_static_data(volume, local_visibility)
	RenderingServer.local_lrt_volume_propagate_visibility(volume)
	var budget_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_global_visibility(volume)
	if not _validate_equal("two-step frame budget", budget_result, cpu_states[2]):
		RenderingServer.free_rid(volume)
		return

	var scaled_volume: RID = _create_volume(Vector3(16.0, 16.0, 16.0), local_visibility)
	RenderingServer.local_lrt_volume_set_visibility_iterations(scaled_volume, COMPLETION_STEPS)
	RenderingServer.local_lrt_volume_propagate_visibility(scaled_volume)
	var scaled_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_global_visibility(scaled_volume)
	if not _validate_equal("uniform spacing scale", scaled_result, cpu_states[COMPLETION_STEPS]):
		RenderingServer.free_rid(volume)
		RenderingServer.free_rid(scaled_volume)
		return

	RenderingServer.free_rid(volume)
	RenderingServer.free_rid(scaled_volume)
	print("LOCAL_LRT_GPU_VISIBILITY_PASS steps=1,2,3,4 budget=2 stable=true spacing_scale=true probes=%d" % _probe_count())
	quit()


func _create_volume(size: Vector3, local_visibility: PackedVector4Array) -> RID:
	var volume: RID = RenderingServer.local_lrt_volume_create()
	if not volume.is_valid():
		return RID()
	RenderingServer.local_lrt_volume_set_grid(volume, size, RESOLUTION)
	_upload_static_data(volume, local_visibility)
	return volume


func _upload_static_data(volume: RID, local_visibility: PackedVector4Array) -> void:
	var local_transfer := PackedVector4Array()
	local_transfer.resize(_probe_count() * 12)
	var mesh_light := PackedVector4Array()
	mesh_light.resize(_probe_count() * 3)
	RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light)


func _create_local_visibility() -> PackedVector4Array:
	var values := PackedVector4Array()
	values.resize(_probe_count())
	for index: int in values.size():
		values[index] = Vector4(1.0 / SH_Y00, 0.0, 0.0, 0.0)
	values[_probe_index(CENTER)] = Vector4(1.2, 0.2, -0.1, 0.05)
	values[_probe_index(BLOCKED)] = Vector4.ZERO
	return values


func _propagate_cpu(input: PackedVector4Array, local_visibility: PackedVector4Array) -> PackedVector4Array:
	var output := PackedVector4Array()
	output.resize(_probe_count())
	var fully_visible := Vector4(1.0 / SH_Y00, 0.0, 0.0, 0.0)
	var neighbor_offsets: Array[Vector3i] = _neighbor_offsets()
	for z: int in RESOLUTION.z:
		for y: int in RESOLUTION.y:
			for x: int in RESOLUTION.x:
				var position := Vector3i(x, y, z)
				var gathered := Vector4.ZERO
				for offset: Vector3i in neighbor_offsets:
					var neighbor_position: Vector3i = position + offset
					var neighbor_visibility: Vector4 = fully_visible
					if _is_valid_position(neighbor_position):
						neighbor_visibility = input[_probe_index(neighbor_position)]
					gathered += neighbor_visibility * _neighbor_weight(offset)
				output[_probe_index(position)] = _triple_product(gathered, local_visibility[_probe_index(position)])
	return output


func _neighbor_offsets() -> Array[Vector3i]:
	var offsets: Array[Vector3i] = []
	for z: int in range(-1, 2):
		for y: int in range(-1, 2):
			for x: int in range(-1, 2):
				if x != 0 or y != 0 or z != 0:
					offsets.append(Vector3i(x, y, z))
	return offsets


func _neighbor_weight(offset: Vector3i) -> float:
	const NORMALIZATION: float = 6.0 + 12.0 / sqrt(2.0) + 8.0 / sqrt(3.0)
	return (1.0 / Vector3(offset).length()) / NORMALIZATION


func _triple_product(a: Vector4, b: Vector4) -> Vector4:
	return Vector4(
			a.dot(b),
			a.x * b.y + b.x * a.y,
			a.x * b.z + b.x * a.z,
			a.x * b.w + b.x * a.w) * SH_Y00


func _validate_equal(label: String, actual: PackedVector4Array, expected: PackedVector4Array) -> bool:
	if actual.size() != expected.size():
		_fail("%s probe count mismatch: %d != %d" % [label, actual.size(), expected.size()])
		return false
	for index: int in actual.size():
		var value: Vector4 = actual[index]
		if not is_finite(value.x) or not is_finite(value.y) or not is_finite(value.z) or not is_finite(value.w):
			_fail("%s produced non-finite visibility at probe %d" % [label, index])
			return false
		if value.distance_to(expected[index]) > EPSILON:
			_fail("%s mismatch at probe %d: %s != %s" % [label, index, value, expected[index]])
			return false
	return true


func _is_valid_position(position: Vector3i) -> bool:
	return position.x >= 0 and position.y >= 0 and position.z >= 0 and position.x < RESOLUTION.x and position.y < RESOLUTION.y and position.z < RESOLUTION.z


func _probe_index(position: Vector3i) -> int:
	return position.x + RESOLUTION.x * (position.y + RESOLUTION.y * position.z)


func _probe_count() -> int:
	return RESOLUTION.x * RESOLUTION.y * RESOLUTION.z


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
