extends SceneTree

## Validates RGB SH2 radiance ping-pong against an independent CPU recurrence.

const SH_Y00: float = 0.28209479177387814
const SH_Y1: float = 0.4886025119029199
const SH_FOUR_PI: float = 12.566370614359172
const RESOLUTION := Vector3i(3, 3, 3)
const SOURCE := Vector3i(0, 1, 1)
const SURFACE_NEIGHBOR := Vector3i(1, 1, 1)
const ITERATIONS: Array[int] = [1, 2, 4, 8]
const PROBE_BUDGET: int = 10
const EPSILON: float = 0.0005
const DECAY: float = 1.0


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var volume: RID = RenderingServer.local_lrt_volume_create()
	if not volume.is_valid():
		_fail("Local LRT requires a RenderingDevice renderer.")
		return

	RenderingServer.local_lrt_volume_set_grid(volume, Vector3(2.0, 2.0, 2.0), RESOLUTION)
	RenderingServer.local_lrt_volume_set_radiance_neighbor_pattern(volume, 0)
	var local_visibility: PackedVector4Array = _create_local_visibility()
	var local_transfer: PackedVector4Array = _create_local_transfer()
	var mesh_light: PackedVector4Array = _create_mesh_light()
	var injection: PackedVector4Array = _create_injection()
	RenderingServer.local_lrt_volume_set_visibility_iterations(volume, 1)
	for iteration: int in ITERATIONS:
		RenderingServer.local_lrt_volume_set_propagation_iterations(volume, iteration)
		RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light)
		RenderingServer.local_lrt_volume_set_injection(volume, injection)
		RenderingServer.local_lrt_volume_propagate_radiance(volume)
		var actual: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
		var expected: PackedVector4Array = _propagate_radiance(local_visibility, local_transfer, mesh_light, injection, iteration)
		if not _validate_iteration(iteration, actual, expected):
			RenderingServer.free_rid(volume)
			return

	RenderingServer.local_lrt_volume_set_propagation_iterations(volume, 1)
	RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light)
	RenderingServer.local_lrt_volume_set_injection(volume, injection)
	RenderingServer.local_lrt_volume_propagate_radiance(volume)
	RenderingServer.local_lrt_volume_set_injection(volume, injection)
	RenderingServer.local_lrt_volume_propagate_radiance(volume)
	var persistent_actual: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
	var persistent_expected: PackedVector4Array = _propagate_radiance(local_visibility, local_transfer, mesh_light, injection, 2)
	if not _validate_iteration(2, persistent_actual, persistent_expected):
		RenderingServer.free_rid(volume)
		return
	if not _validate_dirty_radiance_history(volume, persistent_actual, local_visibility, local_transfer, mesh_light):
		RenderingServer.free_rid(volume)
		return

	RenderingServer.local_lrt_volume_set_propagation_iterations(volume, 1)
	RenderingServer.local_lrt_volume_set_radiance_probe_budget(volume, PROBE_BUDGET)
	RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light)
	RenderingServer.local_lrt_volume_set_injection(volume, injection)
	var zero_radiance := PackedVector4Array()
	zero_radiance.resize(_probe_count() * 3)
	var slice_count: int = ceili(float(_probe_count()) / float(PROBE_BUDGET))
	for slice: int in slice_count - 1:
		RenderingServer.local_lrt_volume_propagate_radiance(volume)
		var partial_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
		if not _validate_values("hidden partial hop %d" % (slice + 1), partial_result, zero_radiance):
			RenderingServer.free_rid(volume)
			return
	RenderingServer.local_lrt_volume_propagate_radiance(volume)
	var sliced_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
	var sliced_expected: PackedVector4Array = _propagate_radiance(local_visibility, local_transfer, mesh_light, injection, 1)
	if not _validate_values("completed sliced hop", sliced_result, sliced_expected):
		RenderingServer.free_rid(volume)
		return

	RenderingServer.local_lrt_volume_set_radiance_neighbor_pattern(volume, 1)
	RenderingServer.local_lrt_volume_set_radiance_probe_budget(volume, PROBE_BUDGET)
	RenderingServer.local_lrt_volume_set_propagation_iterations(volume, 1)
	RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light)
	RenderingServer.local_lrt_volume_set_injection(volume, injection)
	var dithered_slice_count: int = ceili(float(_probe_count() * 3) / float(PROBE_BUDGET))
	for slice: int in dithered_slice_count - 1:
		_update_static_probe(volume, SURFACE_NEIGHBOR, local_visibility, local_transfer, mesh_light)
		RenderingServer.local_lrt_volume_propagate_radiance(volume)
		var partial_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
		if not _validate_values("hidden dithered phase %d" % (slice + 1), partial_result, zero_radiance):
			RenderingServer.free_rid(volume)
			return
	_update_static_probe(volume, SURFACE_NEIGHBOR, local_visibility, local_transfer, mesh_light)
	RenderingServer.local_lrt_volume_propagate_radiance(volume)
	var dithered_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
	var dithered_expected: PackedVector4Array = _propagate_radiance(local_visibility, local_transfer, mesh_light, injection, 1, true)
	if not _validate_values("completed dithered four-neighbor cycle", dithered_result, dithered_expected):
		RenderingServer.free_rid(volume)
		return

	RenderingServer.free_rid(volume)
	print("LOCAL_LRT_GPU_RADIANCE_PASS iterations=1,2,4,8 persistent=2 probe_budget=%d slices=%d partial_hidden=true dithered_cycle_slices=%d dithered_partial_hidden=true continuous_dirty=true probes=27 values=81 mesh_light=1 dirty_history=true" % [PROBE_BUDGET, slice_count, dithered_slice_count])
	quit()


func _create_local_visibility() -> PackedVector4Array:
	var values := PackedVector4Array()
	values.resize(_probe_count())
	for index: int in values.size():
		values[index] = Vector4(1.0 / SH_Y00, 0.0, 0.0, 0.0)
	values[_probe_index(SURFACE_NEIGHBOR)] = Vector4(0.75 / SH_Y00, 0.12, -0.05, 0.08)
	return values


func _create_local_transfer() -> PackedVector4Array:
	var values := PackedVector4Array()
	values.resize(_probe_count() * 12)
	var index: int = _probe_index(SURFACE_NEIGHBOR)
	for channel: int in 3:
		var strength: float = 0.18 if channel == 0 else 0.035
		for row: int in 4:
			var diagonal := Vector4.ZERO
			diagonal[row] = strength
			values[index * 12 + channel * 4 + row] = diagonal
	return values


func _create_mesh_light() -> PackedVector4Array:
	var values := PackedVector4Array()
	values.resize(_probe_count() * 3)
	var surface: int = _probe_index(SURFACE_NEIGHBOR) * 3
	values[surface] = Vector4(0.22, 0.04, -0.01, 0.03)
	values[surface + 1] = Vector4(0.08, 0.015, -0.004, 0.01)
	return values


func _create_injection() -> PackedVector4Array:
	var values := PackedVector4Array()
	values.resize(_probe_count() * 3)
	var source: int = _probe_index(SOURCE) * 3
	values[source] = Vector4(1.4, 0.25, -0.1, 0.06)
	values[source + 1] = Vector4(0.65, 0.1, -0.04, 0.02)
	values[source + 2] = Vector4(0.3, 0.05, -0.02, 0.01)
	var surface: int = _probe_index(SURFACE_NEIGHBOR) * 3
	values[surface] = values[source]
	values[surface + 1] = values[source + 1]
	values[surface + 2] = values[source + 2]
	return values


func _validate_dirty_radiance_history(
	volume: RID,
	before: PackedVector4Array,
	local_visibility: PackedVector4Array,
	local_transfer: PackedVector4Array,
	mesh_light: PackedVector4Array
) -> bool:
	var probe_index: int = _probe_index(SURFACE_NEIGHBOR)
	var value_index: int = probe_index * 3
	if before[value_index].is_zero_approx():
		_fail("Dirty history source Radiance is unexpectedly zero.")
		return false

	_update_static_probe(volume, SURFACE_NEIGHBOR, local_visibility, local_transfer, mesh_light)
	var after: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
	for index: int in after.size():
		if not after[index].is_equal_approx(before[index]):
			_fail("Dirty update changed published Radiance at %d: %s != %s" % [index, after[index], before[index]])
			return false
	return true


func _update_static_probe(
	volume: RID,
	position: Vector3i,
	local_visibility: PackedVector4Array,
	local_transfer: PackedVector4Array,
	mesh_light: PackedVector4Array
) -> void:
	var probe_index: int = _probe_index(position)
	var value_index: int = probe_index * 3
	var region_visibility := PackedVector4Array([local_visibility[probe_index]])
	var region_transfer := PackedVector4Array()
	var region_mesh_light := PackedVector4Array()
	for index: int in 12:
		region_transfer.push_back(local_transfer[probe_index * 12 + index])
	for index: int in 3:
		region_mesh_light.push_back(mesh_light[value_index + index])
	RenderingServer.local_lrt_volume_update_static_data(
		volume,
		position,
		Vector3i.ONE,
		region_visibility,
		region_transfer,
		region_mesh_light,
		PackedInt32Array([0])
	)


func _propagate_visibility(local: PackedVector4Array, iterations: int) -> PackedVector4Array:
	var current: PackedVector4Array = local.duplicate()
	for _iteration: int in iterations:
		var next := PackedVector4Array()
		next.resize(_probe_count())
		for index: int in _probe_count():
			var position: Vector3i = _probe_position(index)
			var gathered := Vector4.ZERO
			for z: int in range(-1, 2):
				for y: int in range(-1, 2):
					for x: int in range(-1, 2):
						var offset := Vector3i(x, y, z)
						if offset == Vector3i.ZERO:
							continue
						var neighbor_position: Vector3i = position + offset
						var neighbor: Vector4 = current[_probe_index(neighbor_position)] if _is_valid(neighbor_position) else Vector4(1.0 / SH_Y00, 0.0, 0.0, 0.0)
						gathered += neighbor * _neighbor_weight(offset)
			next[index] = _triple_product(gathered, local[index])
		current = next
	return current


func _propagate_radiance(local_visibility: PackedVector4Array, local_transfer: PackedVector4Array, mesh_light: PackedVector4Array, injection: PackedVector4Array, iterations: int, dithered_four_neighbor: bool = false) -> PackedVector4Array:
	var radiance := PackedVector4Array()
	radiance.resize(_probe_count() * 3)
	for _iteration: int in iterations:
		var next := PackedVector4Array()
		next.resize(radiance.size())
		var phase_count: int = 3 if dithered_four_neighbor else 1
		for phase: int in phase_count:
			for index: int in _probe_count():
				var position: Vector3i = _probe_position(index)
				for channel: int in 3:
					var gathered := Vector4.ZERO
					var offsets: Array[Vector3i] = _dithered_offsets(position, phase) if dithered_four_neighbor else _neighbor_offsets()
					for offset: Vector3i in offsets:
						var neighbor_position: Vector3i = position + offset
						if not _is_valid(neighbor_position):
							continue
						var neighbor: int = _probe_index(neighbor_position)
						var weight: float = (0.25 if dithered_four_neighbor else _neighbor_weight(offset)) * pow(DECAY, Vector3(offset).length())
						var neighbor_visibility: Vector4 = _antipodal(local_visibility[neighbor])
						var visible_radiance: Vector4 = _triple_product(radiance[neighbor * 3 + channel], neighbor_visibility)
						var direction: Vector3 = Vector3(offset).normalized()
						var basis: Vector4 = _sh_basis(direction)
						var directional_radiance: float = max(visible_radiance.dot(basis), 0.0)
						gathered += basis * (directional_radiance * SH_FOUR_PI * weight)
					var filtered_gathered: Vector4 = _triple_product(gathered, local_visibility[index])
					var value_index: int = index * 3 + channel
					var filtered_incoming: Vector4 = _positive_product(mesh_light[value_index], local_visibility[index]) + _triple_product(injection[value_index] + gathered, local_visibility[index])
					var propagated: Vector4 = filtered_gathered + _transform_transfer(local_transfer, index, channel, filtered_incoming)
					next[value_index] += propagated / float(phase_count)
		radiance = next
	return radiance


func _neighbor_offsets() -> Array[Vector3i]:
	var offsets: Array[Vector3i] = []
	for z: int in range(-1, 2):
		for y: int in range(-1, 2):
			for x: int in range(-1, 2):
				if x != 0 or y != 0 or z != 0:
					offsets.append(Vector3i(x, y, z))
	return offsets


func _dithered_offsets(position: Vector3i, phase: int) -> Array[Vector3i]:
	var hash_value: int = position.x * 73856093 ^ position.y * 19349663 ^ position.z * 83492791
	var pattern: int = (phase + hash_value % 3) % 3
	var offsets: Array[Vector3i] = []
	for first_sign: int in range(-1, 2, 2):
		for second_sign: int in range(-1, 2, 2):
			if pattern == 0:
				offsets.append(Vector3i(first_sign, second_sign, 0))
			elif pattern == 1:
				offsets.append(Vector3i(first_sign, 0, second_sign))
			else:
				offsets.append(Vector3i(0, first_sign, second_sign))
	return offsets


func _transform_transfer(transfer: PackedVector4Array, index: int, channel: int, value: Vector4) -> Vector4:
	var offset: int = index * 12 + channel * 4
	return Vector4(transfer[offset].dot(value), transfer[offset + 1].dot(value), transfer[offset + 2].dot(value), transfer[offset + 3].dot(value))


func _triple_product(a: Vector4, b: Vector4) -> Vector4:
	return Vector4(a.dot(b), a.x * b.y + b.x * a.y, a.x * b.z + b.x * a.z, a.x * b.w + b.x * a.w) * SH_Y00


func _positive_product(a: Vector4, b: Vector4) -> Vector4:
	var result := Vector4.ZERO
	for z in range(-1, 2):
		for y in range(-1, 2):
			for x in range(-1, 2):
				var offset := Vector3i(x, y, z)
				if offset == Vector3i.ZERO:
					continue
				var direction: Vector3 = Vector3(offset).normalized()
				var basis: Vector4 = _sh_basis(direction)
				var value: float = maxf(a.dot(basis), 0.0) * maxf(b.dot(basis), 0.0)
				result += basis * (value * SH_FOUR_PI * _neighbor_weight(offset))
	return result


func _antipodal(value: Vector4) -> Vector4:
	return Vector4(value.x, -value.y, -value.z, -value.w)


func _sh_basis(direction: Vector3) -> Vector4:
	var normal: Vector3 = direction.normalized()
	return Vector4(SH_Y00, SH_Y1 * normal.x, SH_Y1 * normal.y, SH_Y1 * normal.z)


func _neighbor_weight(offset: Vector3i) -> float:
	const NORMALIZATION: float = 6.0 + 12.0 / sqrt(2.0) + 8.0 / sqrt(3.0)
	return (1.0 / Vector3(offset).length()) / NORMALIZATION


func _validate_iteration(iteration: int, actual: PackedVector4Array, expected: PackedVector4Array) -> bool:
	if not _validate_values("iteration %d" % iteration, actual, expected):
		return false
	var reflected: int = _probe_index(SURFACE_NEIGHBOR) * 3
	if iteration > 1 and actual[reflected].length() <= actual[reflected + 1].length():
		_fail("Red transfer did not dominate green at iteration %d: %f <= %f" % [iteration, actual[reflected].length(), actual[reflected + 1].length()])
		return false
	return true


func _validate_values(label: String, actual: PackedVector4Array, expected: PackedVector4Array) -> bool:
	if actual.size() != expected.size():
		_fail("%s size mismatch: %d != %d" % [label, actual.size(), expected.size()])
		return false
	for index: int in actual.size():
		var value: Vector4 = actual[index]
		if not is_finite(value.x) or not is_finite(value.y) or not is_finite(value.z) or not is_finite(value.w):
			_fail("Non-finite Radiance at %s index %d" % [label, index])
			return false
		if value.distance_to(expected[index]) > EPSILON:
			_fail("%s mismatch at %d: %s != %s" % [label, index, value, expected[index]])
			return false
	return true


func _probe_index(position: Vector3i) -> int:
	return position.x + RESOLUTION.x * (position.y + RESOLUTION.y * position.z)


func _probe_position(index: int) -> Vector3i:
	var plane_size: int = RESOLUTION.x * RESOLUTION.y
	var z: int = index / plane_size
	var plane_index: int = index - z * plane_size
	var y: int = plane_index / RESOLUTION.x
	return Vector3i(plane_index - y * RESOLUTION.x, y, z)


func _is_valid(position: Vector3i) -> bool:
	return position.x >= 0 and position.y >= 0 and position.z >= 0 and position.x < RESOLUTION.x and position.y < RESOLUTION.y and position.z < RESOLUTION.z


func _probe_count() -> int:
	return RESOLUTION.x * RESOLUTION.y * RESOLUTION.z


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
