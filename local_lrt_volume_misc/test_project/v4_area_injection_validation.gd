extends SceneTree

## Benchmarks the Area Light scene and freezes the analytic-injection visual result.

const SCENE: String = "res://cornell_area_v09b.tscn"
const REFERENCE_PATH: String = "res://../benchmarks/area_cornell_v09b/godot_combined_agx.png"
const OUTPUT_PATH: String = "res://../benchmarks/v4_performance/area_analytic_after.png"
const SETTLE_FRAMES: int = 48
const SAMPLE_FRAMES: int = 240


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var arguments: PackedStringArray = OS.get_cmdline_user_args()
	var hide_area: bool = arguments.has("hide_area=true")
	root.size = Vector2i(512, 512)
	var packed_scene: PackedScene = load(SCENE)
	if packed_scene == null:
		_fail("Unable to load the Area Light Cornell scene.")
		return
	var scene: Node = packed_scene.instantiate()
	root.add_child(scene)
	current_scene = scene
	var area_light := scene.get_node_or_null("AreaLight3D") as AreaLight3D
	if area_light == null:
		_fail("The Area Light Cornell scene has no AreaLight3D.")
		return
	area_light.visible = not hide_area
	for _frame: int in SETTLE_FRAMES:
		await process_frame

	var samples := PackedFloat64Array()
	var previous_usec: int = Time.get_ticks_usec()
	for _frame: int in SAMPLE_FRAMES:
		await process_frame
		var now_usec: int = Time.get_ticks_usec()
		samples.append(float(now_usec - previous_usec) / 1000.0)
		previous_usec = now_usec
	samples.sort()
	var total_ms: float = 0.0
	for sample: float in samples:
		total_ms += sample
	var mode: String = "hidden" if hide_area else "visible"
	print("LOCAL_LRT_V4_AREA_PERFORMANCE mode=%s frames=%d mean_ms=%.6f median_ms=%.6f min_ms=%.6f max_ms=%.6f" % [
		mode,
		SAMPLE_FRAMES,
		total_ms / float(samples.size()),
		samples[samples.size() >> 1],
		samples[0],
		samples[-1],
	])
	if hide_area:
		quit()
		return

	var captured: Image = root.get_texture().get_image()
	if captured.save_png(ProjectSettings.globalize_path(OUTPUT_PATH)) != OK:
		_fail("Unable to save the analytic Area Light capture.")
		return
	var reference: Image = Image.load_from_file(ProjectSettings.globalize_path(REFERENCE_PATH))
	if reference == null or reference.is_empty() or reference.get_size() != captured.get_size():
		_fail("The Area Light reference is missing or has a different size.")
		return
	var metrics: Vector2 = _compare_images(reference, captured)
	print("LOCAL_LRT_V4_AREA_VISUAL_PASS mean_error=%.8f max_error=%.8f" % [metrics.x, metrics.y])
	quit()


func _compare_images(reference: Image, captured: Image) -> Vector2:
	var error_sum: float = 0.0
	var max_error: float = 0.0
	var pixel_count: int = reference.get_width() * reference.get_height()
	for y: int in reference.get_height():
		for x: int in reference.get_width():
			var expected: Color = reference.get_pixel(x, y)
			var actual: Color = captured.get_pixel(x, y)
			var error: float = maxf(absf(expected.r - actual.r), maxf(absf(expected.g - actual.g), absf(expected.b - actual.b)))
			error_sum += error
			max_error = maxf(max_error, error)
	return Vector2(error_sum / float(pixel_count), max_error)


func _fail(message: String) -> void:
	push_error("LOCAL_LRT_V4_AREA_VALIDATION_FAIL: %s" % message)
	quit(1)
