extends SceneTree

## Captures the Local LRT directional-shadow Probe debug view for the fixed far-caster benchmark.

const FRAME_DELAY: int = 8
const OUTPUT_DIRECTORY_NAME: String = "p2_directional_shadow"


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var arguments := OS.get_cmdline_user_args()
	if arguments.size() != 1 or (arguments[0] != "before" and arguments[0] != "after"):
		_fail("Expected one capture label: before or after.")
		return

	var scene := Node3D.new()
	var camera := Camera3D.new()
	camera.current = true
	camera.look_at_from_position(Vector3(7.0, 5.0, 7.0), Vector3.ZERO)
	scene.add_child(camera)

	var volume := LocalLRTVolume3D.new()
	volume.size = Vector3(4.0, 4.0, 4.0)
	volume.probe_spacing = 1.0
	volume.debug_draw = true
	volume.debug_mode = LocalLRTVolume3D.DEBUG_MODE_DIRECTIONAL_SHADOW
	volume.debug_probe_scale = 0.22
	scene.add_child(volume)

	var caster := MeshInstance3D.new()
	var caster_mesh := BoxMesh.new()
	caster_mesh.size = Vector3(8.0, 8.0, 0.5)
	caster.mesh = caster_mesh
	caster.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
	caster.position = Vector3(0.0, 0.0, 12.0)
	scene.add_child(caster)

	var light := DirectionalLight3D.new()
	light.shadow_enabled = true
	light.directional_shadow_max_distance = 32.0
	scene.add_child(light)

	root.add_child(scene)
	volume.rebuild()
	for _frame: int in FRAME_DELAY:
		await process_frame

	var image := root.get_texture().get_image()
	var project_directory := ProjectSettings.globalize_path("res://")
	var output_directory := project_directory.path_join("..").path_join("benchmarks").path_join(OUTPUT_DIRECTORY_NAME).simplify_path()
	var output_path := output_directory.path_join("directional_shadow_%s.png" % arguments[0])
	var error := image.save_png(output_path)
	if error != OK:
		_fail("Could not save capture: %s" % error_string(error))
		return

	var mean_visibility := _mean_shadow_visibility(volume)
	print("LOCAL_LRT_P2_DIRECTIONAL_SHADOW_CAPTURE label=%s mean_visibility=%.8f" % [arguments[0], mean_visibility])
	if arguments[0] == "after" and not await _validate_shadow_updates(camera, volume, caster, light):
		return
	quit()


func _validate_shadow_updates(camera: Camera3D, volume: LocalLRTVolume3D, caster: MeshInstance3D, light: DirectionalLight3D) -> bool:
	light.shadow_caster_mask = 0
	await _wait_frames()
	if _mean_shadow_visibility(volume) < 0.99:
		_fail("Shadow caster mask did not exclude the caster.")
		return false

	caster.layers = 2
	light.shadow_caster_mask = 2
	camera.cull_mask = 1
	await _wait_frames()
	if _mean_shadow_visibility(volume) < 0.99:
		_fail("Camera cull mask did not exclude the caster.")
		return false

	camera.cull_mask = 0xFFFFFFFF
	await _wait_frames()
	if _mean_shadow_visibility(volume) > 0.95:
		_fail("Restoring the camera cull mask did not restore the shadow.")
		return false

	caster.position = Vector3(0.0, 0.0, 64.0)
	await _wait_frames()
	if _mean_shadow_visibility(volume) < 0.99:
		_fail("Moving the caster outside the light shadow distance did not invalidate the cache.")
		return false

	caster.position = Vector3(0.0, 0.0, 12.0)
	await _wait_frames()
	if _mean_shadow_visibility(volume) > 0.95:
		_fail("Moving the caster back did not invalidate the cache.")
		return false

	print("LOCAL_LRT_P2_DIRECTIONAL_SHADOW_FILTERS_OK")
	return true


func _wait_frames() -> void:
	for _frame: int in FRAME_DELAY:
		await process_frame


func _mean_shadow_visibility(volume: LocalLRTVolume3D) -> float:
	var visibility: PackedFloat32Array = RenderingServer.local_lrt_volume_get_shadow_visibility(volume.get_rid())
	var mean_visibility: float = 0.0
	for value: float in visibility:
		mean_visibility += value
	return mean_visibility / float(visibility.size()) if not visibility.is_empty() else 1.0


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
