extends SceneTree

## Measures steady-state viewport GPU time for direct and quarter-pixel surface gather.

const SCENE: String = "res://cornell_box.tscn"
const WARMUP_FRAMES: int = 32
const SAMPLE_FRAMES: int = 180


func _initialize() -> void:
	call_deferred("_run_benchmark")


func _run_benchmark() -> void:
	var arguments: PackedStringArray = OS.get_cmdline_user_args()
	var mode: String = arguments[0] if not arguments.is_empty() else "gather"
	if change_scene_to_file(SCENE) != OK:
		push_error("Unable to load Cornell scene.")
		quit(1)
		return
	await scene_changed
	var viewport_rid: RID = root.get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(viewport_rid, true)
	for _frame: int in WARMUP_FRAMES:
		await process_frame

	var samples: Array[float] = []
	for _frame: int in SAMPLE_FRAMES:
		await process_frame
		var gpu_time: float = RenderingServer.viewport_get_measured_render_time_gpu(viewport_rid)
		if gpu_time > 0.0:
			samples.push_back(gpu_time)
	if samples.is_empty():
		push_error("No viewport GPU samples were captured.")
		quit(1)
		return
	samples.sort()
	var sum: float = 0.0
	for sample: float in samples:
		sum += sample
	var mean: float = sum / float(samples.size())
	var median: float = samples[samples.size() / 2]
	print("LOCAL_LRT_V4_SCREEN_GATHER_BENCHMARK mode=%s samples=%d mean_ms=%.6f median_ms=%.6f min_ms=%.6f" % [mode, samples.size(), mean, median, samples[0]])
	quit()
