extends SceneTree

## Captures and compares direct and quarter-pixel Local LRT surface gather paths.

const SCENE: String = "res://cornell_box.tscn"
const SETTLE_FRAMES: int = 24
const REFERENCE_PATH: String = "res://../benchmarks/v4_performance/screen_gather_reference.png"
const GATHER_PATH: String = "res://../benchmarks/v4_performance/screen_gather_quarter_pixels.png"
const MAX_MEAN_ERROR: float = 0.03
const MAX_PIXEL_ERROR: float = 0.35


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var arguments: PackedStringArray = OS.get_cmdline_user_args()
	var mode: String = arguments[0] if not arguments.is_empty() else "gather"
	var packed_scene: PackedScene = load(SCENE)
	if packed_scene == null:
		_fail("Unable to load Cornell scene.")
		return
	var scene: Node = packed_scene.instantiate()
	root.add_child(scene)
	current_scene = scene
	for _frame: int in SETTLE_FRAMES:
		await process_frame
	var captured: Image = root.get_texture().get_image()
	var output_path: String = REFERENCE_PATH if mode == "reference" else GATHER_PATH
	if captured.save_png(ProjectSettings.globalize_path(output_path)) != OK:
		_fail("Unable to save %s capture." % mode)
		return
	if mode == "reference":
		print("LOCAL_LRT_V4_SCREEN_GATHER_REFERENCE_PASS frames=%d" % SETTLE_FRAMES)
		quit()
		return

	var reference: Image = Image.load_from_file(ProjectSettings.globalize_path(REFERENCE_PATH))
	if reference == null or reference.is_empty():
		_fail("Reference capture is missing.")
		return
	var metrics: Vector2 = _compare_images(reference, captured)
	if metrics.x > MAX_MEAN_ERROR or metrics.y > MAX_PIXEL_ERROR:
		_fail("Quarter-pixel gather mismatch: mean=%f max=%f" % [metrics.x, metrics.y])
		return
	print("LOCAL_LRT_V4_SCREEN_GATHER_VISUAL_PASS frames=%d mean_error=%.8f max_error=%.8f" % [SETTLE_FRAMES, metrics.x, metrics.y])
	quit()


func _compare_images(reference: Image, gathered: Image) -> Vector2:
	if reference.get_size() != gathered.get_size():
		return Vector2(INF, INF)
	var error_sum: float = 0.0
	var max_error: float = 0.0
	var pixel_count: int = reference.get_width() * reference.get_height()
	for y: int in reference.get_height():
		for x: int in reference.get_width():
			var a: Color = reference.get_pixel(x, y)
			var b: Color = gathered.get_pixel(x, y)
			var error: float = maxf(absf(a.r - b.r), maxf(absf(a.g - b.g), absf(a.b - b.b)))
			error_sum += error
			max_error = maxf(max_error, error)
	return Vector2(error_sum / float(pixel_count), max_error)


func _fail(message: String) -> void:
	push_error("LOCAL_LRT_V4_SCREEN_GATHER_VISUAL_FAIL: %s" % message)
	quit(1)
