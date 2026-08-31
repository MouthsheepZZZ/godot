extends SceneTree

## Validates RGB SH2 radiance ping-pong against an independent CPU recurrence.

const SH_Y00: float = 0.28209479177387814
const SH_Y1: float = 0.4886025119029199
const SH_FOUR_PI: float = 12.566370614359172
const RESOLUTION := Vector3i(3, 3, 3)
const SOURCE := Vector3i(0, 1, 1)
const SURFACE_NEIGHBOR := Vector3i(1, 1, 1)
const ITERATIONS: Array[int] = [1, 2, 4, 8]
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
	var local_visibility: PackedVector4Array = _create_local_visibility()
	var local_transfer: PackedVector4Array = _create_local_transfer()
	var injection: PackedVector4Array = _create_injection()
	RenderingServer.local_lrt_volume_set_visibility_iterations(volume, 1)
	for iteration: int in ITERATIONS:
		RenderingServer.local_lrt_volume_set_propagation_iterations(volume, iteration)
		RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer)
		RenderingServer.local_lrt_volume_set_injection(volume, injection)
		RenderingServer.local_lrt_volume_propagate_radiance(volume)
		var actual: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
		var expected: PackedVector4Array = _propagate_radiance(local_visibility, local_transfer, injection, iteration)
		if not _validate_iteration(iteration, actual, expected):
			RenderingServer.free_rid(volume)
			return

	RenderingServer.local_lrt_volume_set_propagation_iterations(volume, 1)
	RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer)
	RenderingServer.local_lrt_volume_set_injection(volume, injection)
	RenderingServer.local_lrt_volume_propagate_radiance(volume)
	RenderingServer.local_lrt_volume_set_injection(volume, injection)
	RenderingServer.local_lrt_volume_propagate_radiance(volume)
	var persistent_actual: PackedVector4Array = RenderingServer.local_lrt_volume_get_radiance(volume)
	var persistent_expected: PackedVector4Array = _propagate_radiance(local_visibility, local_transfer, injection, 2)
	if not _validate_iteration(2, persistent_actual, persistent_expected):
		RenderingServer.free_rid(volume)
		return

	RenderingServer.free_rid(volume)
	print("LOCAL_LRT_GPU_RADIANCE_PASS iterations=1,2,4,8 persistent=2 probes=27 values=81")
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


func _propagate_radiance(local_visibility: PackedVector4Array, local_transfer: PackedVector4Array, injection: PackedVector4Array, iterations: int) -> PackedVector4Array:
	var radiance := PackedVector4Array()
	radiance.resize(_probe_count() * 3)
	for _iteration: int in iterations:
		var next := PackedVector4Array()
		next.resize(radiance.size())
		for index: int in _probe_count():
			var position: Vector3i = _probe_position(index)
			var transmission: float = local_visibility[index].x * SH_Y00
			for channel: int in 3:
				var gathered := Vector4.ZERO
				for z: int in range(-1, 2):
					for y: int in range(-1, 2):
						for x: int in range(-1, 2):
							var offset := Vector3i(x, y, z)
							if offset == Vector3i.ZERO:
								continue
							var neighbor_position: Vector3i = position + offset
							if not _is_valid(neighbor_position):
								continue
							var neighbor: int = _probe_index(neighbor_position)
							var weight: float = _neighbor_weight(offset) * pow(DECAY, Vector3(offset).length())
							var neighbor_visibility: Vector4 = _antipodal(local_visibility[neighbor])
							var visible_radiance: Vector4 = _triple_product(radiance[neighbor * 3 + channel], neighbor_visibility)
							var direction: Vector3 = Vector3(offset).normalized()
							var basis: Vector4 = _sh_basis(direction)
							var directional_radiance: float = max(visible_radiance.dot(basis), 0.0)
							gathered += basis * (directional_radiance * SH_FOUR_PI * weight)
				var filtered_gathered: Vector4 = _triple_product(gathered, local_visibility[index])
				var value_index: int = index * 3 + channel
				var filtered_analytic: Vector4 = _triple_product(injection[value_index], local_visibility[index])
				next[value_index] = filtered_gathered * transmission + _transform_transfer(local_transfer, index, channel, filtered_analytic + filtered_gathered)
		radiance = next
	return radiance


func _transform_transfer(transfer: PackedVector4Array, index: int, channel: int, value: Vector4) -> Vector4:
	var offset: int = index * 12 + channel * 4
	return Vector4(transfer[offset].dot(value), transfer[offset + 1].dot(value), transfer[offset + 2].dot(value), transfer[offset + 3].dot(value))


func _triple_product(a: Vector4, b: Vector4) -> Vector4:
	return Vector4(a.dot(b), a.x * b.y + b.x * a.y, a.x * b.z + b.x * a.z, a.x * b.w + b.x * a.w) * SH_Y00


func _antipodal(value: Vector4) -> Vector4:
	return Vector4(value.x, -value.y, -value.z, -value.w)


func _sh_basis(direction: Vector3) -> Vector4:
	var normal: Vector3 = direction.normalized()
	return Vector4(SH_Y00, SH_Y1 * normal.x, SH_Y1 * normal.y, SH_Y1 * normal.z)


func _neighbor_weight(offset: Vector3i) -> float:
	const NORMALIZATION: float = 6.0 + 12.0 / sqrt(2.0) + 8.0 / sqrt(3.0)
	return (1.0 / Vector3(offset).length()) / NORMALIZATION


func _validate_iteration(iteration: int, actual: PackedVector4Array, expected: PackedVector4Array) -> bool:
	if actual.size() != expected.size():
		_fail("Iteration %d size mismatch: %d != %d" % [iteration, actual.size(), expected.size()])
		return false
	for index: int in actual.size():
		var value: Vector4 = actual[index]
		if not is_finite(value.x) or not is_finite(value.y) or not is_finite(value.z) or not is_finite(value.w):
			_fail("Non-finite Radiance at iteration %d index %d" % [iteration, index])
			return false
		if value.distance_to(expected[index]) > EPSILON:
			_fail("Iteration %d mismatch at %d: %s != %s" % [iteration, index, value, expected[index]])
			return false
	var reflected: int = _probe_index(SURFACE_NEIGHBOR) * 3
	if iteration > 1 and actual[reflected].length() <= actual[reflected + 1].length():
		_fail("Red transfer did not dominate green at iteration %d: %f <= %f" % [iteration, actual[reflected].length(), actual[reflected + 1].length()])
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
