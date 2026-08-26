@tool
extends Node3D

## Scene F driver for dynamic BVH, probe classification, and indirect-light validation.
##
## The Inspector pose is deterministic in editor mode. Play mode animates the same
## rigid contributors and recomputes LocalGI only after each Dynamic BVH rebuild.

## Debug visualization shown after each LocalGI update.
@export var start_debug_mode: LocalGIVolume3D.DebugMode = LocalGIVolume3D.DEBUG_PROBE_IRRADIANCE:
	set(value):
		start_debug_mode = value
		_apply_debug_mode()

## Enables deterministic contributor animation while the scene is running.
@export var animate_in_play: bool = true

## Number of complete contributor-motion cycles per second.
@export_range(0.02, 1.0, 0.01, "suffix:Hz") var animation_speed: float = 0.12

## Minimum time between Dynamic BVH and LocalGI updates in Play mode.
@export_range(0.05, 1.0, 0.05, "suffix:s") var update_interval: float = 0.2

## Deterministic editor pose used to inspect occlusion, color bounce, and classification.
@export_range(0.0, 1.0, 0.01) var dynamic_pose: float = 0.0:
	set(value):
		dynamic_pose = clampf(value, 0.0, 1.0)
		_queue_refresh()

var _volume: LocalGIVolume3D
var _white_box: MeshInstance3D
var _door: MeshInstance3D
var _colored_panel: MeshInstance3D
var _refresh_queued: bool = false
var _elapsed: float = 0.0


func _ready() -> void:
	_resolve_nodes()
	if _volume == null or _white_box == null or _door == null or _colored_panel == null:
		push_error("Scene F is missing LocalGI or a dynamic contributor.")
		return

	_volume.multi_bounce_enabled = true
	_volume.temporal_hysteresis = 0.8
	_apply_pose()
	_volume.bake()
	_volume.update_dynamic()
	_volume.build_probes()
	if not _compute_transport():
		return
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
	dynamic_pose = fmod(dynamic_pose + step_delta * animation_speed, 1.0)
	_apply_pose()
	_refresh_transport()


func _queue_refresh() -> void:
	if not Engine.is_editor_hint() or not is_inside_tree() or not is_node_ready() or _refresh_queued:
		return
	_refresh_queued = true
	_refresh_editor.call_deferred()


func _refresh_editor() -> void:
	_refresh_queued = false
	_resolve_nodes()
	if _volume == null or _white_box == null or _door == null or _colored_panel == null:
		return
	_apply_pose()
	_refresh_transport()


func _refresh_transport() -> void:
	if not _volume.update_dynamic():
		return
	_compute_transport()


func _compute_transport() -> bool:
	_volume.classify_probes()
	if not _volume.compute_one_bounce():
		push_error("Dynamic LocalGI compute produced no probes.")
		return false
	if not _volume.update_temporal():
		push_error("Dynamic LocalGI temporal update failed.")
		return false
	if not _volume.probe_irradiance_is_finite():
		push_error("Dynamic LocalGI irradiance contains NaN or Inf.")
		return false
	return true


func _apply_pose() -> void:
	var angle: float = dynamic_pose * TAU
	var wave: float = 0.5 + 0.5 * sin(angle)
	_white_box.position = Vector3(lerpf(-0.9, 0.9, wave), 0.3, 0.9)
	_door.position = Vector3(1.4, 1.1, 0.8)
	_door.rotation = Vector3(0.0, lerpf(-0.15, 1.25, wave), 0.0)
	_colored_panel.position = Vector3(-1.2, 0.8, lerpf(-0.9, 1.2, 0.5 + 0.5 * cos(angle)))
	_colored_panel.rotation = Vector3(0.0, deg_to_rad(30.0), 0.0)


func _apply_debug_mode() -> void:
	if _volume != null:
		_volume.debug_mode = start_debug_mode


func _resolve_nodes() -> void:
	_volume = find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D
	_white_box = find_child("MovableWhiteBox", true, false) as MeshInstance3D
	_door = find_child("RotatingDoor", true, false) as MeshInstance3D
	_colored_panel = find_child("MovableColoredPanel", true, false) as MeshInstance3D
