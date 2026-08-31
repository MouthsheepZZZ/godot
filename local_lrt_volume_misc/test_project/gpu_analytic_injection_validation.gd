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
const AREA_SAMPLE_AXIS_COUNT: int = 8


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

	RenderingServer.free_rid(volume)
	print("LOCAL_LRT_GPU_ANALYTIC_INJECTION_PASS cases=7 probes=27")
	quit()


func _validate_case(volume: RID, label: String, volume_transform: Transform3D, lights: PackedVector4Array, inside_solid: PackedInt32Array) -> bool:
	RenderingServer.local_lrt_volume_set_grid(volume, SIZE, RESOLUTION)
	RenderingServer.local_lrt_volume_set_transform(volume, volume_transform)
	var local_visibility := PackedVector4Array()
	local_visibility.resize(_probe_count())
	var local_transfer := PackedVector4Array()
	local_transfer.resize(_probe_count() * 12)
	RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer)
	var solid: PackedInt32Array = inside_solid
	if solid.is_empty():
		solid.resize(_probe_count())
	RenderingServer.local_lrt_volume_set_inside_solid(volume, solid)
	RenderingServer.local_lrt_volume_inject_analytic_lights(volume, lights)
	var actual: PackedVector4Array = RenderingServer.local_lrt_volume_get_injection(volume)
	var expected: PackedVector4Array = _cpu_injection(volume_transform, lights, solid)
	return _validate_values(label, actual, expected)


func _directional_lights() -> PackedVector4Array:
	return _pack_light(LIGHT_DIRECTIONAL, Color(0.8, 0.1, 0.05), 1.25, Vector3(0.0, 1.0, 0.0).normalized())


func _omni_lights() -> PackedVector4Array:
	return _pack_light(LIGHT_OMNI, Color(0.1, 0.7, 0.2), 2.0, Vector3(0.0, 0.6, 0.0), 1.5, 2.0)


func _spot_lights() -> PackedVector4Array:
	return _pack_light(LIGHT_SPOT, Color(0.2, 0.15, 0.9), 1.8, Vector3(0.0, 0.8, 0.0), 2.0, 1.0, Vector3(0.0, -1.0, 0.0), cos(deg_to_rad(35.0)))


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


func _pack_light(type: float, color: Color, energy: float, vector: Vector3, range: float = 0.0, attenuation: float = 1.0, spot_direction: Vector3 = Vector3.ZERO, cone_limit: float = 0.0) -> PackedVector4Array:
	var lights := PackedVector4Array()
	lights.append(Vector4(type, energy, range, cone_limit))
	lights.append(Vector4(color.r, color.g, color.b, 0.0))
	lights.append(Vector4(vector.x, vector.y, vector.z, attenuation))
	lights.append(Vector4(spot_direction.x, spot_direction.y, spot_direction.z, 0.0))
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
				var cone_cosine: float = spot_direction.normalized().dot(light_to_probe / distance)
				if cone_cosine <= cone_limit:
					continue
				attenuated *= pow(1.0 - distance / range, 2.0) * pow((cone_cosine - cone_limit) / (1.0 - cone_limit), 2.0)
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
				var sample_area: float = area / float(AREA_SAMPLE_AXIS_COUNT * AREA_SAMPLE_AXIS_COUNT)
				var energy_scale: float = 1.0 / area if cone_limit > 0.5 else 1.0
				for y: int in AREA_SAMPLE_AXIS_COUNT:
					var v: float = (float(y) + 0.5) / float(AREA_SAMPLE_AXIS_COUNT) - 0.5
					for x: int in AREA_SAMPLE_AXIS_COUNT:
						var u: float = (float(x) + 0.5) / float(AREA_SAMPLE_AXIS_COUNT) - 0.5
						var sample_position: Vector3 = vector + area_width * u + area_height * v
						var sample_to_probe: Vector3 = world_position - sample_position
						var distance_squared: float = sample_to_probe.length_squared()
						if distance_squared <= 0.000000000001:
							continue
						var sample_to_probe_direction: Vector3 = sample_to_probe / sqrt(distance_squared)
						var emission_cosine: float = max(area_direction.dot(sample_to_probe_direction), 0.0)
						var solid_angle_weight: float = emission_cosine * sample_area / distance_squared
						local_direction = volume_transform.basis.transposed() * -sample_to_probe_direction
						var encoded: Vector4 = _encode_direction(local_direction, energy * area_attenuation * energy_scale * solid_angle_weight * 0.5)
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
