extends SceneTree

## Compares Cornell output after equal complete Radiance hop counts with and without Probe slicing.

const SCENE: String = "res://cornell_box.tscn"
const REFERENCE_FRAMES: int = 8
const RADIANCE_HOPS_PER_FRAME: int = 16
const MAX_MEAN_ERROR: float = 0.0005
const MAX_PIXEL_ERROR: float = 0.02
const REFERENCE_PATH: String = "res://../benchmarks/v4_performance/probe_budget_reference.png"
const SLICED_PATH: String = "res://../benchmarks/v4_performance/probe_budget_sliced.png"


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var packed_scene: PackedScene = load(SCENE)
	if packed_scene == null:
		_fail("Unable to load Cornell scene.")
		return

	var reference: Image = await _capture_mode(packed_scene, 0, REFERENCE_FRAMES)
	if reference == null:
		return
	var probe_count: int = 35 * 23 * 35
	var sliced_frames: int = REFERENCE_FRAMES * RADIANCE_HOPS_PER_FRAME
	var sliced: Image = await _capture_mode(packed_scene, probe_count, sliced_frames)
	if sliced == null:
		return

	var metrics: Vector2 = _compare_images(reference, sliced)
	if metrics.x > MAX_MEAN_ERROR or metrics.y > MAX_PIXEL_ERROR:
		_fail("Cornell mismatch: mean=%f max=%f" % [metrics.x, metrics.y])
		return
	if reference.save_png(ProjectSettings.globalize_path(REFERENCE_PATH)) != OK:
		_fail("Unable to save reference capture.")
		return
	if sliced.save_png(ProjectSettings.globalize_path(SLICED_PATH)) != OK:
		_fail("Unable to save sliced capture.")
		return

	print("LOCAL_LRT_V4_PROBE_BUDGET_VISUAL_PASS reference_frames=%d sliced_frames=%d complete_hops=%d mean_error=%.8f max_error=%.8f" % [REFERENCE_FRAMES, sliced_frames, REFERENCE_FRAMES * RADIANCE_HOPS_PER_FRAME, metrics.x, metrics.y])
	quit()


func _capture_mode(packed_scene: PackedScene, probe_budget: int, frame_count: int) -> Image:
	var scene: Node = packed_scene.instantiate()
	root.add_child(scene)
	current_scene = scene
	var volume := scene.get_node_or_null("LocalLRTVolume3D") as LocalLRTVolume3D
	if volume == null:
		_fail("Cornell scene is missing LocalLRTVolume3D.")
		return null
	volume.set_visibility_probe_budget(probe_budget)
	volume.set_radiance_probe_budget(probe_budget)
	for _frame: int in frame_count:
		await process_frame
	var captured: Image = root.get_texture().get_image()
	current_scene = null
	scene.queue_free()
	await process_frame
	return captured


func _compare_images(reference: Image, sliced: Image) -> Vector2:
	if reference.get_size() != sliced.get_size():
		_fail("Capture sizes differ: %s != %s" % [reference.get_size(), sliced.get_size()])
		return Vector2(INF, INF)
	var error_sum: float = 0.0
	var max_error: float = 0.0
	var pixel_count: int = reference.get_width() * reference.get_height()
	for y: int in reference.get_height():
		for x: int in reference.get_width():
			var a: Color = reference.get_pixel(x, y)
			var b: Color = sliced.get_pixel(x, y)
			var error: float = maxf(absf(a.r - b.r), maxf(absf(a.g - b.g), absf(a.b - b.b)))
			error_sum += error
			max_error = maxf(max_error, error)
	return Vector2(error_sum / float(pixel_count), max_error)


func _fail(message: String) -> void:
	push_error("LOCAL_LRT_V4_PROBE_BUDGET_VISUAL_FAIL: %s" % message)
	quit(1)
