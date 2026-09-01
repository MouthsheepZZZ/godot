extends SceneTree

## Captures and compares FP32 RGB and packed FP16 luminance-plus-tint transfer data.

const SCENE: String = "res://cornell_box.tscn"
const SETTLE_FRAMES: int = 48
const REFERENCE_PATH: String = "res://../benchmarks/v4_performance/transfer_rgb_fp32.png"
const COMPRESSED_PATH: String = "res://../benchmarks/v4_performance/transfer_luminance_fp16_tint.png"
const MAX_MEAN_ERROR: float = 0.03
const MAX_PIXEL_ERROR: float = 0.35


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var arguments: PackedStringArray = OS.get_cmdline_user_args()
	var mode: String = arguments[0] if not arguments.is_empty() else "compressed"
	if change_scene_to_file(SCENE) != OK:
		_fail("Unable to load Cornell scene.")
		return
	await scene_changed
	for _frame: int in SETTLE_FRAMES:
		await process_frame
	var captured: Image = root.get_texture().get_image()
	var output_path: String = REFERENCE_PATH if mode == "reference" else COMPRESSED_PATH
	if captured.save_png(ProjectSettings.globalize_path(output_path)) != OK:
		_fail("Unable to save %s capture." % mode)
		return
	if mode == "reference":
		print("LOCAL_LRT_V4_TRANSFER_COMPRESSION_REFERENCE_PASS frames=%d" % SETTLE_FRAMES)
		quit()
		return

	var reference: Image = Image.load_from_file(ProjectSettings.globalize_path(REFERENCE_PATH))
	if reference == null or reference.is_empty() or reference.get_size() != captured.get_size():
		_fail("Reference capture is missing or has a different size.")
		return
	var error_sum: float = 0.0
	var max_error: float = 0.0
	var pixel_count: int = reference.get_width() * reference.get_height()
	for y: int in reference.get_height():
		for x: int in reference.get_width():
			var a: Color = reference.get_pixel(x, y)
			var b: Color = captured.get_pixel(x, y)
			var error: float = maxf(absf(a.r - b.r), maxf(absf(a.g - b.g), absf(a.b - b.b)))
			error_sum += error
			max_error = maxf(max_error, error)
	var mean_error: float = error_sum / float(pixel_count)
	if mean_error > MAX_MEAN_ERROR or max_error > MAX_PIXEL_ERROR:
		_fail("Compressed transfer mismatch: mean=%f max=%f" % [mean_error, max_error])
		return
	print("LOCAL_LRT_V4_TRANSFER_COMPRESSION_VISUAL_PASS frames=%d mean_error=%.8f max_error=%.8f" % [SETTLE_FRAMES, mean_error, max_error])
	quit()


func _fail(message: String) -> void:
	push_error("LOCAL_LRT_V4_TRANSFER_COMPRESSION_VISUAL_FAIL: %s" % message)
	quit(1)
