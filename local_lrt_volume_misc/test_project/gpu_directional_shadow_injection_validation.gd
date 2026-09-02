extends SceneTree

## Validates GPU directional shadow visibility against a synthetic depth map.

const SH_Y00: float = 0.28209479177387814
const SH_Y1: float = 0.4886025119029199
const RESOLUTION := Vector3i(5, 5, 5)
const SIZE := Vector3(4.0, 4.0, 4.0)
const EPSILON: float = 0.002
const SHADOW_SIZE: int = 64
const SHADOW_BIAS: float = 0.001
const LIGHT_DIRECTIONAL: float = 1.0


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var volume: RID = RenderingServer.local_lrt_volume_create()
	if not volume.is_valid():
		_fail("Local LRT requires a RenderingDevice renderer.")
		return

	var camera := Transform3D(Basis.looking_at(Vector3(0.0, -1.0, 0.0), Vector3(1.0, 0.0, 0.0)), Vector3(0.0, 6.0, 0.0))
	var projection := Projection.create_orthogonal(-4.0, 4.0, -4.0, 4.0, 0.05, 20.0)
	var view_proj: Projection = _view_projection(camera, projection)
	var projected: Vector4 = _project_point(view_proj, Vector3.ZERO)
	if projected.x < 0.5:
		RenderingServer.free_rid(volume)
		_fail("Plane occluder is outside the synthetic shadow projection.")
		return
	var plane_depth: float = projected.w

	var depths := PackedFloat32Array()
	depths.resize(SHADOW_SIZE * SHADOW_SIZE)
	depths.fill(plane_depth)
	var lights: PackedVector4Array = _directional_light(true)

	if not _validate_case(volume, "front-back", camera, projection, depths, lights, true):
		RenderingServer.free_rid(volume)
		return
	if not _validate_case(volume, "disabled", camera, projection, PackedFloat32Array(), lights, false):
		RenderingServer.free_rid(volume)
		return

	RenderingServer.free_rid(volume)
	print("LOCAL_LRT_GPU_DIRECTIONAL_SHADOW_INJECTION_PASS cases=2 probes=125")
	quit()


func _validate_case(
		volume: RID,
		label: String,
		camera: Transform3D,
		projection: Projection,
		depths: PackedFloat32Array,
		lights: PackedVector4Array,
		shadow_enabled: bool
) -> bool:
	RenderingServer.local_lrt_volume_set_grid(volume, SIZE, RESOLUTION)
	RenderingServer.local_lrt_volume_set_transform(volume, Transform3D.IDENTITY)
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
	var solid := PackedInt32Array()
	solid.resize(_probe_count())
	RenderingServer.local_lrt_volume_set_inside_solid(volume, solid)
	RenderingServer.local_lrt_volume_set_directional_shadow(volume, depths, SHADOW_SIZE if shadow_enabled else 0, camera, projection, SHADOW_BIAS)
	RenderingServer.local_lrt_volume_inject_analytic_lights(volume, lights)
	var actual_injection: PackedVector4Array = RenderingServer.local_lrt_volume_get_injection(volume)
	var actual_visibility: PackedFloat32Array = RenderingServer.local_lrt_volume_get_shadow_visibility(volume)
	var expected_visibility: PackedFloat32Array = _cpu_visibility(camera, projection, depths, shadow_enabled)
	var expected_injection: PackedVector4Array = _cpu_injection(lights, expected_visibility)
	if not _validate_floats(label + " visibility", actual_visibility, expected_visibility, 0.05):
		return false
	if shadow_enabled:
		var front: int = _probe_index(Vector3i(2, 4, 2))
		var back: int = _probe_index(Vector3i(2, 0, 2))
		if actual_visibility[front] < 0.9:
			_fail("%s front visibility %s" % [label, actual_visibility[front]])
			return false
		if actual_visibility[back] > 0.1:
			_fail("%s back visibility %s" % [label, actual_visibility[back]])
			return false
	return _validate_values(label + " injection", actual_injection, expected_injection)


func _directional_light(shadow: bool) -> PackedVector4Array:
	var lights := PackedVector4Array()
	lights.append(Vector4(LIGHT_DIRECTIONAL, 1.25, 0.0, 0.0))
	lights.append(Vector4(0.8, 0.1, 0.05, 1.0 if shadow else 0.0))
	lights.append(Vector4(0.0, 1.0, 0.0, 0.0))
	for index: int in 6:
		lights.append(Vector4.ZERO)
	return lights


func _cpu_visibility(camera: Transform3D, projection: Projection, depths: PackedFloat32Array, shadow_enabled: bool) -> PackedFloat32Array:
	var values := PackedFloat32Array()
	values.resize(_probe_count())
	values.fill(1.0)
	if not shadow_enabled or depths.is_empty():
		return values
	var view_proj: Projection = _view_projection(camera, projection)
	var spacing: Vector3 = SIZE / Vector3(RESOLUTION - Vector3i.ONE)
	for index: int in _probe_count():
		var world_position: Vector3 = Vector3(_probe_position(index)) * spacing - SIZE * 0.5
		values[index] = _sample_shadow(depths, view_proj, world_position)
	return values


func _cpu_injection(lights: PackedVector4Array, visibility: PackedFloat32Array) -> PackedVector4Array:
	var values := PackedVector4Array()
	values.resize(_probe_count() * 3)
	for index: int in _probe_count():
		var acc_r := Vector4.ZERO
		var acc_g := Vector4.ZERO
		var acc_b := Vector4.ZERO
		var light_count: int = lights.size() / 9
		for light: int in light_count:
			var base: int = light * 9
			var packed: Vector4 = lights[base]
			var color := Vector3(lights[base + 1].x, lights[base + 1].y, lights[base + 1].z)
			var vector := Vector3(lights[base + 2].x, lights[base + 2].y, lights[base + 2].z)
			var energy: float = packed.y
			if int(packed.x) != int(LIGHT_DIRECTIONAL):
				continue
			energy *= 0.5
			if lights[base + 1].w > 0.5:
				energy *= visibility[index]
			var local_direction: Vector3 = vector
			if energy <= 0.0 or local_direction.length_squared() < 0.000000000001:
				continue
			var encoded: Vector4 = _encode_direction(local_direction, energy)
			acc_r += encoded * color.x
			acc_g += encoded * color.y
			acc_b += encoded * color.z
		values[index * 3] = acc_r
		values[index * 3 + 1] = acc_g
		values[index * 3 + 2] = acc_b
	return values


func _sample_shadow(depths: PackedFloat32Array, view_proj: Projection, world_position: Vector3) -> float:
	var projected: Vector4 = _project_point(view_proj, world_position)
	if projected.x < 0.5:
		return 1.0
	var uv := Vector2(projected.y, projected.z)
	var probe_depth: float = projected.w
	var texel: float = 1.0 / float(SHADOW_SIZE)
	var vis: float = 0.0
	var offsets: Array[Vector2] = [Vector2(-0.5, -0.5), Vector2(0.5, -0.5), Vector2(-0.5, 0.5), Vector2(0.5, 0.5)]
	for offset: Vector2 in offsets:
		var sample_uv: Vector2 = uv + offset * texel
		var x: int = clampi(int(floor(sample_uv.x * SHADOW_SIZE)), 0, SHADOW_SIZE - 1)
		var y: int = clampi(int(floor(sample_uv.y * SHADOW_SIZE)), 0, SHADOW_SIZE - 1)
		var occluder: float = depths[y * SHADOW_SIZE + x]
		vis += 1.0 if (probe_depth + SHADOW_BIAS) >= occluder else 0.0;
	return vis * 0.25


func _view_projection(camera: Transform3D, projection: Projection) -> Projection:
	return Projection.create_depth_correction(true) * projection * Projection(camera.affine_inverse())


func _project_point(view_proj: Projection, world_position: Vector3) -> Vector4:
	var clip: Vector4 = view_proj * Vector4(world_position.x, world_position.y, world_position.z, 1.0)
	if is_zero_approx(clip.w):
		return Vector4.ZERO
	var inv_w: float = 1.0 / clip.w
	var uv := Vector2(clip.x * inv_w * 0.5 + 0.5, clip.y * inv_w * 0.5 + 0.5)
	var depth: float = clip.z * inv_w
	if uv.x < 0.0 or uv.x > 1.0 or uv.y < 0.0 or uv.y > 1.0 or depth < 0.0 or depth > 1.0:
		return Vector4.ZERO
	return Vector4(1.0, uv.x, uv.y, depth)


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


func _validate_floats(label: String, actual: PackedFloat32Array, expected: PackedFloat32Array, epsilon: float) -> bool:
	if actual.size() != expected.size():
		_fail("%s size mismatch: %d != %d" % [label, actual.size(), expected.size()])
		return false
	for index: int in actual.size():
		if absf(actual[index] - expected[index]) > epsilon:
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
