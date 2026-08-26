@tool
extends Node3D

## Scene A / G helper: build the probe grid and show probe debug.

@export var start_debug_mode: int = LocalGIVolume3D.DEBUG_PROBE_POSITIONS


func _ready() -> void:
	var volume := find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D
	if volume == null:
		push_error("Scene is missing LocalGIVolume3D.")
		return

	volume.bake()
	volume.update_dynamic()
	volume.build_probes()
	volume.debug_mode = start_debug_mode as LocalGIVolume3D.DebugMode
	if volume.get_probe_count() <= 0:
		push_error("Probe build produced no probes.")
		return
	if volume.get_probe_ray_budget() != volume.get_probe_count() * volume.rays_per_probe:
		push_error("Probe ray budget is not exact.")
		return

	if not volume.is_gpu_available():
		push_warning("LocalGI GPU tracer is unavailable; selected probe rays will use CPU.")
		return
	if not volume.upload_gpu():
		push_warning("LocalGI GPU upload failed; selected probe rays will use CPU.")
