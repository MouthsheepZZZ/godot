extends SceneTree

## Validates GPU analytic light injection against the CPU LocalLRTBuilder reference.

const SH_Y00: float = 0.28209479177387814
const SH_Y1: float = 0.4886025119029199
const RESOLUTION := Vector3i(3, 3, 3)
const SIZE := Vector3(2.0, 2.0, 2.0)
const EPSILON: float = 0.0005
const LIGHT_DIRECTIONAL: float = 1.0
const LIGHT_OMNI: float = 2.0
const LIGHT_SPOT: float = 3.0
const LIGHT_AREA: float = 4.0


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var volume: RID = RenderingServer.local_lrt_volume_create()
	if not volume.is_valid():
		_fail("Local LRT requires a RenderingDevice renderer.")
		return

	var identity := Transform3D.IDENTITY
	var rotated := Transform3D(Basis(Vector3.UP, PI * 0.5), Vector3(0.25, 0.0, -0.5))
	var cases: Array[Dictionary] = [
		{"label": "directional", "transform": identity, "lights": _directional_lights(), "solid": PackedInt32Array()},
		{"label": "omni", "transform": identity, "lights": _omni_lights(), "solid": PackedInt32Array()},
		{"label": "spot", "transform": identity, "lights": _spot_lights(), "solid": PackedInt32Array()},
		{"label": "area", "transform": identity, "lights": _area_lights(), "solid": PackedInt32Array()},
		{"label": "combined-rotated", "transform": rotated, "lights": _combined_lights(), "solid": PackedInt32Array()},
		{"label": "inside-solid", "transform": identity, "lights": _directional_lights(), "solid": _center_inside_solid()},
		{"label": "empty", "transform": identity, "lights": PackedVector4Array(), "solid": PackedInt32Array()},
	]
	for case: Dictionary in cases:
		if not _validate_case(volume, String(case["label"]), case["transform"], case["lights"], case["solid"]):
			RenderingServer.free_rid(volume)
			return
	if not _validate_budgeted_publish(volume):
		RenderingServer.free_rid(volume)
		return

	RenderingServer.free_rid(volume)
	print("LOCAL_LRT_GPU_DIRECT_INJECTION_PASS cases=7 probes=27 cached_lights=true full_grid_publish=true")
	quit()


func _validate_case(volume: RID, label: String, volume_transform: Transform3D, lights: PackedVector4Array, inside_solid: PackedInt32Array) -> bool:
	RenderingServer.local_lrt_volume_set_grid(volume, SIZE, RESOLUTION)
	RenderingServer.local_lrt_volume_set_transform(volume, volume_transform)
	_set_identity_static_data(volume)
	var solid: PackedInt32Array = inside_solid
	if solid.is_empty():
		solid.resize(_probe_count())
	RenderingServer.local_lrt_volume_set_inside_solid(volume, solid)
	RenderingServer.local_lrt_volume_inject_analytic_lights(volume, lights)
	var actual: PackedVector4Array = RenderingServer.local_lrt_volume_get_injection(volume)
	var expected: PackedVector4Array = _cpu_injection(volume_transform, lights, solid)
	if not _validate_values(label, actual, expected):
		return false
	RenderingServer.local_lrt_volume_inject_analytic_lights(volume, lights)
	var cached_actual: PackedVector4Array = RenderingServer.local_lrt_volume_get_injection(volume)
	return _validate_values(label + "-cached", cached_actual, expected)


func _validate_budgeted_publish(volume: RID) -> bool:
	RenderingServer.local_lrt_volume_set_grid(volume, SIZE, RESOLUTION)
	RenderingServer.local_lrt_volume_set_transform(volume, Transform3D.IDENTITY)
	_set_identity_static_data(volume)
	var solid := PackedInt32Array()
	solid.resize(_probe_count())
	RenderingServer.local_lrt_volume_set_inside_solid(volume, solid)
	var lights: PackedVector4Array = _area_lights()
	var expected: PackedVector4Array = _cpu_injection(Transform3D.IDENTITY, lights, solid)
	RenderingServer.local_lrt_volume_inject_analytic_lights(volume, lights)
	if not _validate_values("full-grid-published", RenderingServer.local_lrt_volume_get_injection(volume), expected):
		return false

	var changing_lights: PackedVector4Array = _area_lights()
	changing_lights[0] = Vector4(LIGHT_AREA, 1.7, 2.0, 1.0)
	RenderingServer.local_lrt_volume_inject_analytic_lights(volume, changing_lights)
	return _validate_values("moving-latest", RenderingServer.local_lrt_volume_get_injection(volume), _cpu_injection(Transform3D.IDENTITY, changing_lights, solid))


func _set_identity_static_data(volume: RID) -> void:
	var local_visibility := PackedVector4Array()
	local_visibility.resize(_probe_count())
	local_visibility.fill(Vector4(1.0 / SH_Y00, 0.0, 0.0, 0.0))
	var local_transfer := PackedVector4Array()
	local_transfer.resize(_probe_count() * 12)
	var identity_rows: Array[Vector4] = [
		Vector4(1.0, 0.0, 0.0, 0.0),
		Vector4(0.0, 1.0, 0.0, 0.0),
		Vector4(0.0, 0.0, 1.0, 0.0),
		Vector4(0.0, 0.0, 0.0, 1.0),
	]
	for probe: int in _probe_count():
		for channel: int in 3:
			for row: int in 4:
				local_transfer[probe * 12 + channel * 4 + row] = identity_rows[row]
	var mesh_light := PackedVector4Array()
	mesh_light.resize(_probe_count() * 3)
	RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light)


func _directional_lights() -> PackedVector4Array:
	return _pack_light(LIGHT_DIRECTIONAL, Color(0.8, 0.1, 0.05), 1.25, Vector3(0.0, 1.0, 0.0).normalized())


func _omni_lights() -> PackedVector4Array:
	return _pack_light(LIGHT_OMNI, Color(0.1, 0.7, 0.2), 2.0, Vector3(0.0, 0.6, 0.0), 1.5, 2.0)


func _spot_lights() -> PackedVector4Array:
	return _pack_light(LIGHT_SPOT, Color(0.2, 0.15, 0.9), 1.8, Vector3(0.0, 0.8, 0.0), 2.0, 1.5, Vector3(0.0, -1.0, 0.0), cos(deg_to_rad(35.0)), 1.0 / 0.75)


func _area_lights() -> PackedVector4Array:
	var lights := PackedVector4Array()
	lights.append(Vector4(LIGHT_AREA, 1.4, 2.0, 1.0))
	lights.append(Vector4(0.9, 0.45, 0.15, 0.0))
	lights.append(Vector4(0.0, 0.9, 0.0, 2.0))
	lights.append(Vector4(0.0, -1.0, 0.0, 0.0))
	lights.append(Vector4(1.0, 0.0, 0.0, 0.0))
	lights.append(Vector4(0.0, 0.0, 0.75, 0.0))
	lights.append(Vector4.ZERO)
	lights.append(Vector4.ZERO)
	lights.append(Vector4.ZERO)
	return lights


func _combined_lights() -> PackedVector4Array:
	var lights := _directional_lights()
	lights.append_array(_omni_lights())
	lights.append_array(_spot_lights())
	lights.append_array(_area_lights())
	return lights


func _pack_light(type: float, color: Color, energy: float, vector: Vector3, range: float = 0.0, attenuation: float = 1.0, spot_direction: Vector3 = Vector3.ZERO, cone_limit: float = 0.0, cone_exponent: float = 0.0) -> PackedVector4Array:
	var lights := PackedVector4Array()
	lights.append(Vector4(type, energy, range, cone_limit))
	lights.append(Vector4(color.r, color.g, color.b, 0.0))
	lights.append(Vector4(vector.x, vector.y, vector.z, attenuation))
	lights.append(Vector4(spot_direction.x, spot_direction.y, spot_direction.z, cone_exponent))
	lights.append(Vector4(1.0, 0.0, 0.0, 0.0))
	lights.append(Vector4(0.0, 1.0, 0.0, 0.0))
	lights.append(Vector4(0.0, 0.0, 1.0, 0.0))
	lights.append(Vector4.ZERO)
	lights.append(Vector4.ZERO)
	return lights


func _center_inside_solid() -> PackedInt32Array:
	var solid := PackedInt32Array()
	solid.resize(_probe_count())
	solid[_probe_index(Vector3i(1, 1, 1))] = 1
	return solid


func _cpu_injection(volume_transform: Transform3D, lights: PackedVector4Array, inside_solid: PackedInt32Array) -> PackedVector4Array:
	var values := PackedVector4Array()
	values.resize(_probe_count() * 3)
	var spacing: Vector3 = SIZE / Vector3(RESOLUTION - Vector3i.ONE)
	for index: int in _probe_count():
		if inside_solid[index] != 0:
			continue
		var local_position: Vector3 = Vector3(_probe_position(index)) * spacing - SIZE * 0.5
		var world_position: Vector3 = volume_transform * local_position
		var acc_r := Vector4.ZERO
		var acc_g := Vector4.ZERO
		var acc_b := Vector4.ZERO
		var light_count: int = lights.size() / 9
		for light: int in light_count:
			var base: int = light * 9
			var packed: Vector4 = lights[base]
			var color := Vector3(lights[base + 1].x, lights[base + 1].y, lights[base + 1].z)
			var vector := Vector3(lights[base + 2].x, lights[base + 2].y, lights[base + 2].z)
			var attenuation_decay: float = lights[base + 2].w
			var spot_direction := Vector3(lights[base + 3].x, lights[base + 3].y, lights[base + 3].z)
			var area_width := Vector3(lights[base + 4].x, lights[base + 4].y, lights[base + 4].z)
			var area_height := Vector3(lights[base + 5].x, lights[base + 5].y, lights[base + 5].z)
			var type: int = int(packed.x)
			var energy: float = packed.y
			var range: float = packed.z
			var cone_limit: float = packed.w
			var local_direction := Vector3.ZERO
			var attenuated: float = energy
			if type == int(LIGHT_DIRECTIONAL):
				attenuated *= 0.5
				local_direction = volume_transform.basis.transposed() * vector
			elif type == int(LIGHT_OMNI):
				var to_light: Vector3 = vector - world_position
				var distance: float = to_light.length()
				if distance >= range or is_zero_approx(distance):
					continue
				var normalized_distance: float = distance / range
				normalized_distance *= normalized_distance
				normalized_distance *= normalized_distance
				var range_window: float = max(1.0 - normalized_distance, 0.0)
				range_window *= range_window
				attenuated *= range_window * pow(max(distance, 0.0001), -attenuation_decay) * 0.5
				local_direction = volume_transform.basis.transposed() * to_light
			elif type == int(LIGHT_SPOT):
				var light_to_probe: Vector3 = world_position - vector
				var distance: float = light_to_probe.length()
				if distance >= range or is_zero_approx(distance):
					continue
				var normalized_distance: float = distance / range
				normalized_distance *= normalized_distance
				normalized_distance *= normalized_distance
				var range_window: float = max(1.0 - normalized_distance, 0.0)
				range_window *= range_window
				var cone_cosine: float = max(spot_direction.normalized().dot(light_to_probe / distance), cone_limit)
				var spot_rim: float = max(0.0001, (1.0 - cone_cosine) / (1.0 - cone_limit))
				var cone_attenuation: float = 1.0 - pow(spot_rim, lights[base + 3].w)
				attenuated *= range_window * pow(max(distance, 0.0001), -attenuation_decay) * cone_attenuation * 0.5
				local_direction = volume_transform.basis.transposed() * -light_to_probe
			elif type == int(LIGHT_AREA):
				var width_length: float = area_width.length()
				var height_length: float = area_height.length()
				var area: float = width_length * height_length
				if area <= 0.000000000001 or range <= 0.0 or spot_direction.length_squared() < 0.000000000001:
					continue
				var area_direction: Vector3 = spot_direction.normalized()
				var light_to_probe: Vector3 = world_position - vector
				if area_direction.dot(light_to_probe) <= 0.0:
					continue
				var width_direction: Vector3 = area_width / width_length
				var height_direction: Vector3 = area_height / height_length
				var closest_x: float = clampf(light_to_probe.dot(width_direction), -width_length * 0.5, width_length * 0.5)
				var closest_y: float = clampf(light_to_probe.dot(height_direction), -height_length * 0.5, height_length * 0.5)
				var closest_point: Vector3 = vector + width_direction * closest_x + height_direction * closest_y
				var closest_distance: float = world_position.distance_to(closest_point)
				if closest_distance >= range:
					continue
				var normalized_distance: float = closest_distance / range
				normalized_distance *= normalized_distance
				normalized_distance *= normalized_distance
				var range_window: float = max(1.0 - normalized_distance, 0.0)
				range_window *= range_window
				var area_attenuation: float = range_window * pow(max(closest_distance, 0.0001), 2.0 - attenuation_decay)
				var energy_scale: float = 1.0 / area if cone_limit > 0.5 else 1.0
				var directions: Array[Vector3] = [
					vector - area_width * 0.5 - area_height * 0.5 - world_position,
					vector + area_width * 0.5 - area_height * 0.5 - world_position,
					vector + area_width * 0.5 + area_height * 0.5 - world_position,
					vector - area_width * 0.5 + area_height * 0.5 - world_position,
				]
				var encoded: Vector4 = _encode_spherical_quad(directions, energy * area_attenuation * energy_scale * 0.5 * TAU)
				var local_first_moment: Vector3 = volume_transform.basis.transposed() * Vector3(encoded.y, encoded.z, encoded.w)
				encoded.y = local_first_moment.x
				encoded.z = local_first_moment.y
				encoded.w = local_first_moment.z
				acc_r += encoded * color.x
				acc_g += encoded * color.y
				acc_b += encoded * color.z
				continue
			if attenuated <= 0.0 or local_direction.length_squared() < 0.000000000001:
				continue
			var encoded: Vector4 = _encode_direction(local_direction, attenuated)
			acc_r += encoded * color.x
			acc_g += encoded * color.y
			acc_b += encoded * color.z
		values[index * 3] = acc_r
		values[index * 3 + 1] = acc_g
		values[index * 3 + 2] = acc_b
	return values


func _encode_direction(direction: Vector3, energy: float) -> Vector4:
	var n: Vector3 = direction.normalized()
	return Vector4(SH_Y00, SH_Y1 * n.x, SH_Y1 * n.y, SH_Y1 * n.z) * (energy * TAU)


func _encode_spherical_quad(directions: Array[Vector3], value: float) -> Vector4:
	var normalized: Array[Vector3] = []
	for direction: Vector3 in directions:
		normalized.append(direction.normalized())
	var solid_angle: float = _triangle_solid_angle(normalized[0], normalized[1], normalized[2]) + _triangle_solid_angle(normalized[0], normalized[2], normalized[3])
	var first_moment := Vector3.ZERO
	for index: int in 4:
		var edge_cross: Vector3 = normalized[index].cross(normalized[(index + 1) % 4])
		var cross_length: float = edge_cross.length()
		if cross_length > 0.000000000001:
			var edge_angle: float = atan2(cross_length, normalized[index].dot(normalized[(index + 1) % 4]))
			first_moment += edge_cross * (0.5 * edge_angle / cross_length)
	if solid_angle < 0.0:
		solid_angle = -solid_angle
		first_moment = -first_moment
	return Vector4(SH_Y00 * solid_angle, SH_Y1 * first_moment.x, SH_Y1 * first_moment.y, SH_Y1 * first_moment.z) * value


func _triangle_solid_angle(a: Vector3, b: Vector3, c: Vector3) -> float:
	return 2.0 * atan2(a.dot(b.cross(c)), 1.0 + a.dot(b) + b.dot(c) + c.dot(a))


func _validate_values(label: String, actual: PackedVector4Array, expected: PackedVector4Array) -> bool:
	if actual.size() != expected.size():
		_fail("%s size mismatch: %d != %d" % [label, actual.size(), expected.size()])
		return false
	for index: int in actual.size():
		if actual[index].distance_to(expected[index]) > EPSILON:
			_fail("%s mismatch at %d: %s != %s" % [label, index, actual[index], expected[index]])
			return false
	return true


func _probe_count() -> int:
	return RESOLUTION.x * RESOLUTION.y * RESOLUTION.z


func _probe_index(position: Vector3i) -> int:
	return position.x + RESOLUTION.x * (position.y + RESOLUTION.y * position.z)


func _probe_position(index: int) -> Vector3i:
	var plane: int = RESOLUTION.x * RESOLUTION.y
	var z: int = index / plane
	var plane_index: int = index - z * plane
	return Vector3i(plane_index % RESOLUTION.x, plane_index / RESOLUTION.x, z)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
