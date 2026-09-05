extends SceneTree

## Validates that a shadowed positional light remains in Local LRT when the viewport shadow atlas is unavailable.

const FRAME_DELAY: int = 4
const EPSILON: float = 0.0005


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	RenderingServer.viewport_set_positional_shadow_atlas_size(root.get_viewport_rid(), 0, false)

	var scene := Node3D.new()
	var camera := Camera3D.new()
	camera.current = true
	camera.position = Vector3(0.0, 0.0, 5.0)
	scene.add_child(camera)

	var volume := LocalLRTVolume3D.new()
	volume.size = Vector3(2.0, 2.0, 2.0)
	volume.probe_spacing = 1.0
	scene.add_child(volume)
	var receiver := MeshInstance3D.new()
	var receiver_mesh := BoxMesh.new()
	receiver_mesh.size = Vector3(0.75, 0.75, 0.75)
	receiver.mesh = receiver_mesh
	scene.add_child(receiver)

	root.add_child(scene)
	volume.rebuild()

	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 1.5, 0.0)
	omni.omni_range = 3.0
	var light_energy: float = await _validate_light(scene, volume, omni, "Omni")
	if light_energy < 0.0:
		return
	var total_energy := light_energy

	var spot := SpotLight3D.new()
	spot.position = Vector3(0.0, 0.0, 1.5)
	spot.spot_range = 3.0
	light_energy = await _validate_light(scene, volume, spot, "Spot")
	if light_energy < 0.0:
		return
	total_energy += light_energy

	var area := AreaLight3D.new()
	area.position = Vector3(0.0, 0.0, 1.5)
	area.area_range = 3.0
	area.area_size = Vector2(1.0, 1.0)
	light_energy = await _validate_light(scene, volume, area, "Area")
	if light_energy < 0.0:
		return
	total_energy += light_energy

	print("LOCAL_LRT_POSITIONAL_SHADOW_FALLBACK_PASS lights=3 energy=%.8f" % total_energy)
	quit()


func _wait_for_frames() -> void:
	for _frame: int in FRAME_DELAY:
		await process_frame


func _validate_light(scene: Node3D, volume: LocalLRTVolume3D, light: Light3D, label: String) -> float:
	light.shadow_enabled = false
	scene.add_child(light)
	await _wait_for_frames()
	var unshadowed: PackedVector4Array = RenderingServer.local_lrt_volume_get_injection(volume.get_rid())
	var energy := _total_energy(unshadowed)
	if energy <= EPSILON:
		_fail("%s produced no unshadowed Local LRT Direct energy." % label)
		return -1.0

	light.shadow_enabled = true
	await _wait_for_frames()
	var missing_atlas: PackedVector4Array = RenderingServer.local_lrt_volume_get_injection(volume.get_rid())
	if not _arrays_match(unshadowed, missing_atlas):
		_fail("%s changed or disappeared when its requested shadow atlas allocation was unavailable." % label)
		return -1.0

	light.queue_free()
	await process_frame
	return energy


func _total_energy(values: PackedVector4Array) -> float:
	var energy: float = 0.0
	for value: Vector4 in values:
		energy += value.length()
	return energy


func _arrays_match(first: PackedVector4Array, second: PackedVector4Array) -> bool:
	if first.size() != second.size():
		return false
	for index: int in first.size():
		if not first[index].is_equal_approx(second[index]) and first[index].distance_to(second[index]) > EPSILON:
			return false
	return true


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
