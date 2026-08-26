@tool
extends Node3D

## Scene A helper: bake, compute one-bounce GI, and show probe irradiance.

@export var start_debug_mode: int = LocalGIVolume3D.DEBUG_PROBE_IRRADIANCE


func _ready() -> void:
	var volume := find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D
	if volume == null:
		push_error("Scene is missing LocalGIVolume3D.")
		return

	volume.bake()
	volume.update_dynamic()
	volume.build_probes()
	if not volume.compute_one_bounce():
		push_error("One-bounce compute produced no probes.")
		return
	if not volume.probe_irradiance_is_finite():
		push_error("One-bounce irradiance contains NaN or Inf.")
		return

	volume.debug_mode = start_debug_mode as LocalGIVolume3D.DebugMode
