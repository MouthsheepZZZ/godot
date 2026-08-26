@tool
extends Node3D

## Scene A helper: bake one-bounce, then EMA-converge probe irradiance from zero.

@export var start_debug_mode: LocalGIVolume3D.DebugMode = LocalGIVolume3D.DEBUG_PROBE_IRRADIANCE:
	set(value):
		start_debug_mode = value
		_apply_debug_mode()

@export_range(0.0, 1.0, 0.01) var temporal_hysteresis: float = 0.9:
	set(value):
		temporal_hysteresis = clampf(value, 0.0, 1.0)
		_apply_hysteresis()

@export_range(0.0, 16.0, 0.1) var light_energy: float = 8.0:
	set(value):
		var next := maxf(value, 0.0)
		if is_equal_approx(light_energy, next):
			return
		light_energy = next
		_apply_light_energy(true)

@export var start_from_zero: bool = true

var _volume: LocalGIVolume3D


func _ready() -> void:
	_volume = find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D
	if _volume == null:
		push_error("Scene is missing LocalGIVolume3D.")
		return

	_volume.temporal_hysteresis = temporal_hysteresis
	_apply_light_energy(false)
	_volume.bake()
	_volume.update_dynamic()
	_volume.build_probes()
	if not _volume.compute_one_bounce():
		push_error("One-bounce compute produced no probes.")
		return
	if not _volume.probe_irradiance_is_finite():
		push_error("One-bounce irradiance contains NaN or Inf.")
		return

	if start_from_zero:
		_volume.reset_temporal_history()
	_volume.debug_mode = start_debug_mode
	set_process(not Engine.is_editor_hint())


func _process(_delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if _volume == null or not _volume.has_temporal_history():
		return
	_volume.update_temporal()


func _apply_debug_mode() -> void:
	if _volume == null:
		return
	_volume.debug_mode = start_debug_mode


func _apply_hysteresis() -> void:
	if _volume == null:
		return
	_volume.temporal_hysteresis = temporal_hysteresis


func _apply_light_energy(recompute: bool) -> void:
	var light := find_child("CeilingLight", true, false) as OmniLight3D
	if light != null:
		light.light_energy = light_energy
	if not recompute or _volume == null:
		return
	if not _volume.compute_one_bounce():
		push_error("One-bounce recompute produced no probes.")
		return
	if not _volume.probe_irradiance_is_finite():
		push_error("One-bounce irradiance contains NaN or Inf.")
