extends SceneTree

## Validates that renderer-selected Local LRT volumes propagate while culled volumes retain state.

const SH_Y00: float = 0.28209479177387814
const RESOLUTION := Vector3i(9, 9, 9)
const CENTER := Vector3i(4, 4, 4)
const BLOCKED := Vector3i(0, 4, 4)
const VOLUME_SIZE := Vector3(2.0, 2.0, 2.0)
const FRONT_POSITION := Vector3(0.0, 0.0, -4.0)
const BACK_POSITION := Vector3(0.0, 0.0, 4.0)
const EPSILON: float = 0.0002


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var scene_root := Node3D.new()
	root.add_child(scene_root)
	var camera := Camera3D.new()
	camera.current = true
	scene_root.add_child(camera)

	var local_visibility: PackedVector4Array = _create_local_visibility()
	var front_volume: RID = _create_volume(FRONT_POSITION, local_visibility)
	var back_volume: RID = _create_volume(BACK_POSITION, local_visibility)
	if not front_volume.is_valid() or not back_volume.is_valid():
		_fail("Local LRT requires a RenderingDevice renderer.")
		return

	await process_frame
	await process_frame
	var front_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_global_visibility(front_volume)
	var back_result: PackedVector4Array = RenderingServer.local_lrt_volume_get_global_visibility(back_volume)
	if not _validate_changed("front selected volume", front_result, local_visibility):
		_free_volumes(front_volume, back_volume)
		return
	if not _validate_equal("back culled volume", back_result, local_visibility):
		_free_volumes(front_volume, back_volume)
		return

	_upload_static_data(front_volume, local_visibility)
	_upload_static_data(back_volume, local_visibility)
	camera.rotation.y = PI
	await process_frame
	await process_frame
	front_result = RenderingServer.local_lrt_volume_get_global_visibility(front_volume)
	back_result = RenderingServer.local_lrt_volume_get_global_visibility(back_volume)
	if not _validate_equal("front culled volume after camera turn", front_result, local_visibility):
		_free_volumes(front_volume, back_volume)
		return
	if not _validate_changed("back resumed volume", back_result, local_visibility):
		_free_volumes(front_volume, back_volume)
		return

	_free_volumes(front_volume, back_volume)
	print("LOCAL_LRT_GPU_INVISIBLE_VOLUME_PASS selected_updates=true culled_preserves_state=true resumed_updates=true")
	quit()


func _create_volume(position: Vector3, local_visibility: PackedVector4Array) -> RID:
	var volume: RID = RenderingServer.local_lrt_volume_create()
	if not volume.is_valid():
		return RID()
	RenderingServer.local_lrt_volume_set_grid(volume, VOLUME_SIZE, RESOLUTION)
	RenderingServer.local_lrt_volume_set_transform(volume, Transform3D(Basis.IDENTITY, position))
	RenderingServer.local_lrt_volume_set_visibility_iterations(volume, 1)
	RenderingServer.local_lrt_volume_set_enabled(volume, true)
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


func _validate_equal(label: String, actual: PackedVector4Array, expected: PackedVector4Array) -> bool:
	if actual.size() != expected.size():
		_fail("%s probe count mismatch: %d != %d" % [label, actual.size(), expected.size()])
		return false
	for index: int in actual.size():
		if actual[index].distance_to(expected[index]) > EPSILON:
			_fail("%s changed at probe %d: %s != %s" % [label, index, actual[index], expected[index]])
			return false
	return true


func _validate_changed(label: String, actual: PackedVector4Array, reference: PackedVector4Array) -> bool:
	if actual.size() != reference.size():
		_fail("%s probe count mismatch: %d != %d" % [label, actual.size(), reference.size()])
		return false
	for index: int in actual.size():
		if actual[index].distance_to(reference[index]) > EPSILON:
			return true
	_fail("%s did not propagate" % label)
	return false


func _probe_index(position: Vector3i) -> int:
	return position.x + RESOLUTION.x * (position.y + RESOLUTION.y * position.z)


func _probe_count() -> int:
	return RESOLUTION.x * RESOLUTION.y * RESOLUTION.z


func _free_volumes(front_volume: RID, back_volume: RID) -> void:
	RenderingServer.free_rid(front_volume)
	RenderingServer.free_rid(back_volume)


func _fail(message: String) -> void:
	push_error("LOCAL_LRT_GPU_INVISIBLE_VOLUME_FAIL: %s" % message)
	quit(1)
