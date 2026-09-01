extends SceneTree

## Compares the reference 26-neighbor and original three-phase dithered Radiance patterns.

const SCENE: String = "res://cornell_box.tscn"
const SETTLE_FRAMES: int = 16
const MAX_MEAN_ERROR: float = 0.05
const MAX_PIXEL_ERROR: float = 0.4
const REFERENCE_PATH: String = "res://../benchmarks/v4_performance/neighbor_reference_26.png"
const DITHERED_PATH: String = "res://../benchmarks/v4_performance/neighbor_dithered_4.png"


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var packed_scene: PackedScene = load(SCENE)
	if packed_scene == null:
		_fail("Unable to load Cornell scene.")
		return
	var reference: Image = await _capture_pattern(packed_scene, LocalLRTVolume3D.RADIANCE_NEIGHBOR_PATTERN_REFERENCE_26)
	if reference == null:
		return
	var dithered: Image = await _capture_pattern(packed_scene, LocalLRTVolume3D.RADIANCE_NEIGHBOR_PATTERN_DITHERED_4)
	if dithered == null:
		return
	var metrics: Vector2 = _compare_images(reference, dithered)
	if reference.save_png(ProjectSettings.globalize_path(REFERENCE_PATH)) != OK:
		_fail("Unable to save 26-neighbor capture.")
		return
	if dithered.save_png(ProjectSettings.globalize_path(DITHERED_PATH)) != OK:
		_fail("Unable to save dithered capture.")
		return
	if metrics.x > MAX_MEAN_ERROR or metrics.y > MAX_PIXEL_ERROR:
		_fail("Dithered Cornell mismatch: mean=%f max=%f" % [metrics.x, metrics.y])
		return
	print("LOCAL_LRT_V4_NEIGHBOR_PATTERN_VISUAL_PASS frames=%d mean_error=%.8f max_error=%.8f" % [SETTLE_FRAMES, metrics.x, metrics.y])
	quit()


func _capture_pattern(packed_scene: PackedScene, pattern: LocalLRTVolume3D.RadianceNeighborPattern) -> Image:
	var scene: Node = packed_scene.instantiate()
	root.add_child(scene)
	current_scene = scene
	var volume := scene.get_node_or_null("LocalLRTVolume3D") as LocalLRTVolume3D
	if volume == null:
		_fail("Cornell scene is missing LocalLRTVolume3D.")
		return null
	volume.set_radiance_neighbor_pattern(pattern)
	for _frame: int in SETTLE_FRAMES:
		await process_frame
	var captured: Image = root.get_texture().get_image()
	current_scene = null
	scene.queue_free()
	await process_frame
	return captured


func _compare_images(reference: Image, dithered: Image) -> Vector2:
	if reference.get_size() != dithered.get_size():
		_fail("Capture sizes differ: %s != %s" % [reference.get_size(), dithered.get_size()])
		return Vector2(INF, INF)
	var error_sum: float = 0.0
	var max_error: float = 0.0
	var pixel_count: int = reference.get_width() * reference.get_height()
	for y: int in reference.get_height():
		for x: int in reference.get_width():
			var a: Color = reference.get_pixel(x, y)
			var b: Color = dithered.get_pixel(x, y)
			var error: float = maxf(absf(a.r - b.r), maxf(absf(a.g - b.g), absf(a.b - b.b)))
			error_sum += error
			max_error = maxf(max_error, error)
	return Vector2(error_sum / float(pixel_count), max_error)


func _fail(message: String) -> void:
	push_error("LOCAL_LRT_V4_NEIGHBOR_PATTERN_VISUAL_FAIL: %s" % message)
	quit(1)
