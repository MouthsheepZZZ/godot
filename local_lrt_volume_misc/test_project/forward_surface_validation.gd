extends SceneTree

## Validates that Forward+ surface shading consumes Local LRT radiance and edge weight.

const FRAME_DELAY: int = 4
const PIXEL_STEP: int = 8


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var error := change_scene_to_file("res://cornell_box.tscn")
	if error != OK:
		_fail("Could not load Cornell Box: %s" % error_string(error))
		return
	await scene_changed

	var volume := current_scene.get_node("LocalLRTVolume3D") as LocalLRTVolume3D
	volume.debug_draw = false
	volume.enabled = false
	volume.energy = 1.0
	await _wait_for_frames()
	var baseline := _capture_viewport()

	volume.enabled = true
	volume.edge_blend_distance = 0.0
	await _wait_for_frames()
	var full_gi := _capture_viewport()
	var full_difference := _mean_difference(baseline, full_gi)
	if full_difference <= 0.0001:
		_fail("Local LRT surface contribution was not visible: %.8f" % full_difference)
		return

	volume.edge_blend_distance = 100.0
	await _wait_for_frames()
	var edge_blended := _capture_viewport()
	var blended_difference := _mean_difference(baseline, edge_blended)
	if blended_difference >= full_difference:
		_fail("Edge blend did not reduce Local LRT contribution: %.8f >= %.8f" % [blended_difference, full_difference])
		return

	print("LOCAL_LRT_FORWARD_SURFACE_PASS full=%.8f blended=%.8f" % [full_difference, blended_difference])
	quit()


func _wait_for_frames() -> void:
	for _frame in FRAME_DELAY:
		await process_frame


func _capture_viewport() -> Image:
	return root.get_texture().get_image()


func _mean_difference(first: Image, second: Image) -> float:
	var difference: float = 0.0
	var sample_count: int = 0
	for y in range(0, first.get_height(), PIXEL_STEP):
		for x in range(0, first.get_width(), PIXEL_STEP):
			var delta := first.get_pixel(x, y) - second.get_pixel(x, y)
			difference += absf(delta.r) + absf(delta.g) + absf(delta.b)
			sample_count += 3
	return difference / float(sample_count)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
