@tool
extends Node3D

## Scene G driver for LocalGI stability while its complete local domain moves.
##
## The [code]MovingRoot[/code] transform changes, while geometry, lights, probes,
## classification, and transport remain expressed in [LocalGIVolume3D] local space.

## Debug visualization shown while exercising moving-volume invariants.
@export var start_debug_mode: LocalGIVolume3D.DebugMode = LocalGIVolume3D.DEBUG_PROBE_IRRADIANCE:
	set(value):
		start_debug_mode = value
		_apply_debug_mode()

## Enables the translation, rotation, high-speed, and combined cycle in Play mode.
@export var animate_in_play: bool = true

## Number of complete four-stage motion cycles per second.
@export_range(0.01, 0.25, 0.01, "suffix:Hz") var animation_speed: float = 0.04

## Minimum time between LocalGI invariant checks and transport updates.
@export_range(0.05, 1.0, 0.05, "suffix:s") var update_interval: float = 0.15

## Deterministic cycle position for editor inspection.
@export_range(0.0, 1.0, 0.01) var motion_phase: float = 0.0:
	set(value):
		motion_phase = clampf(value, 0.0, 1.0)
		_queue_refresh()

@onready var _moving_root: Node3D = $MovingRoot
@onready var _volume: LocalGIVolume3D = find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D

var _initial_static_rebuild_count: int = 0
var _initial_dynamic_rebuild_count: int = 0
var _initial_probe_positions: PackedVector3Array
var _initial_probe_states: PackedByteArray
var _refresh_queued: bool = false
var _elapsed: float = 0.0


func _ready() -> void:
	if _moving_root == null or _volume == null:
		push_error("Scene G is missing MovingRoot or LocalGIVolume3D.")
		return

	_apply_motion()
	_volume.multi_bounce_enabled = true
	_volume.temporal_hysteresis = 0.8
	_volume.bake()
	_volume.update_dynamic()
	_volume.build_probes()
	_volume.classify_probes()
	if not _compute_transport():
		return

	_initial_static_rebuild_count = _volume.get_static_rebuild_count()
	_initial_dynamic_rebuild_count = _volume.get_dynamic_rebuild_count()
	_initial_probe_positions = _volume.get_probe_positions()
	_initial_probe_states = _volume.get_probe_active_states()
	_volume.debug_mode = start_debug_mode
	set_process(not Engine.is_editor_hint() and animate_in_play)


func _process(delta: float) -> void:
	if Engine.is_editor_hint() or not animate_in_play or _volume == null:
		return
	_elapsed += delta
	if _elapsed < update_interval:
		return
	var step_delta: float = _elapsed
	_elapsed = 0.0
	motion_phase = fmod(motion_phase + step_delta * animation_speed, 1.0)
	_apply_motion()
	_validate_and_update()


func _queue_refresh() -> void:
	if not Engine.is_editor_hint() or not is_inside_tree() or not is_node_ready() or _refresh_queued:
		return
	_refresh_queued = true
	_refresh_editor.call_deferred()


func _refresh_editor() -> void:
	_refresh_queued = false
	if _moving_root == null or _volume == null:
		return
	_apply_motion()
	_validate_and_update()


func _validate_and_update() -> void:
	if _volume.is_static_dirty():
		push_error("Moving the complete LocalGI domain marked the Static BVH dirty.")
		set_process(false)
		return
	if _volume.update_dynamic():
		push_error("Moving the complete LocalGI domain rebuilt the Dynamic BVH.")
		set_process(false)
		return
	if _volume.get_static_rebuild_count() != _initial_static_rebuild_count:
		push_error("Moving the complete LocalGI domain rebuilt the Static BVH.")
		set_process(false)
		return
	if _volume.get_dynamic_rebuild_count() != _initial_dynamic_rebuild_count:
		push_error("Moving the complete LocalGI domain changed the Dynamic BVH rebuild count.")
		set_process(false)
		return
	if not _volume.has_temporal_history():
		push_error("Moving the complete LocalGI domain reset temporal history.")
		set_process(false)
		return
	if not _local_probe_state_is_stable():
		set_process(false)
		return
	if not _compute_transport():
		set_process(false)


func _local_probe_state_is_stable() -> bool:
	var positions: PackedVector3Array = _volume.get_probe_positions()
	var states: PackedByteArray = _volume.get_probe_active_states()
	if positions.size() != _initial_probe_positions.size() or states.size() != _initial_probe_states.size():
		push_error("Moving the complete LocalGI domain changed the Probe layout.")
		return false
	for index: int in positions.size():
		if not positions[index].is_equal_approx(_initial_probe_positions[index]):
			push_error("Moving the complete LocalGI domain changed a Probe local position.")
			return false
		if states[index] != _initial_probe_states[index]:
			push_error("Moving the complete LocalGI domain changed Probe Classification.")
			return false
	return true


func _compute_transport() -> bool:
	if not _volume.compute_one_bounce():
		push_error("Moving-volume LocalGI compute produced no probes.")
		return false
	if not _volume.update_temporal():
		push_error("Moving-volume LocalGI temporal update failed.")
		return false
	if not _volume.probe_irradiance_is_finite():
		push_error("Moving-volume LocalGI irradiance contains NaN or Inf.")
		return false
	return true


func _apply_motion() -> void:
	var segment_position: float = fmod(motion_phase * 4.0, 1.0)
	var segment: int = mini(floori(motion_phase * 4.0), 3)
	var position: Vector3 = Vector3.ZERO
	var rotation: Vector3 = Vector3.ZERO
	match segment:
		0:
			position = Vector3(lerpf(-6.0, 6.0, segment_position), 0.0, 0.0)
		1:
			rotation.y = segment_position * TAU
		2:
			position = Vector3(lerpf(-40.0, 40.0, segment_position), 0.0, 0.0)
		3:
			position = Vector3(
				lerpf(8.0, -8.0, segment_position),
				2.0 * sin(segment_position * TAU),
				lerpf(-5.0, 5.0, segment_position)
			)
			rotation = Vector3(0.25 * sin(segment_position * TAU), segment_position * TAU, 0.15)
	_moving_root.transform = Transform3D(Basis.from_euler(rotation), position)


func _apply_debug_mode() -> void:
	if _volume != null:
		_volume.debug_mode = start_debug_mode
