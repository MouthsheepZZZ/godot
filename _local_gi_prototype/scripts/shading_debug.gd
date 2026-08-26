@tool
extends Node3D

## Scene C / D helper for editor inspection: rebuild occluder + LocalGI when Inspector values change.

enum DividerMode {
	CLOSED,
	DOORWAY,
	WINDOW,
}

const _WHITE_MATERIAL := preload("res://materials/white.tres")

@export var start_debug_mode: LocalGIVolume3D.DebugMode = LocalGIVolume3D.DEBUG_FINAL_LOCAL_GI:
	set(value):
		start_debug_mode = value
		_queue_refresh(false)

@export_range(1.0, 40.0, 1.0, "suffix:cm") var wall_thickness_cm: float = 10.0:
	set(value):
		var next := clampf(value, 1.0, 40.0)
		if is_equal_approx(wall_thickness_cm, next):
			return
		wall_thickness_cm = next
		_queue_refresh(true)

@export var divider_mode: DividerMode = DividerMode.DOORWAY:
	set(value):
		if divider_mode == value:
			return
		divider_mode = value
		_queue_refresh(true)

@export_range(-2.0, 2.0, 0.05, "suffix:m") var light_x: float = -1.2:
	set(value):
		if is_equal_approx(light_x, value):
			return
		light_x = value
		_queue_refresh(true)

var _pending_refresh: bool = false
var _pending_rebuild: bool = false
var _extra_occluders: Array[MeshInstance3D] = []


func _ready() -> void:
	_refresh(true)


func _queue_refresh(rebuild: bool) -> void:
	if not is_inside_tree() or not is_node_ready():
		return
	_pending_rebuild = _pending_rebuild or rebuild
	if _pending_refresh:
		return
	_pending_refresh = true
	_flush_refresh.call_deferred()


func _flush_refresh() -> void:
	var rebuild := _pending_rebuild
	_pending_refresh = false
	_pending_rebuild = false
	_refresh(rebuild)


func _refresh(rebuild: bool) -> void:
	if rebuild:
		_apply_occluder()
		_offset_light()

	var volume := find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D
	if volume == null:
		push_error("Scene is missing LocalGIVolume3D.")
		return

	if rebuild:
		volume.bake()
		volume.update_dynamic()
		volume.build_probes()
		if not volume.compute_one_bounce():
			push_error("One-bounce compute produced no probes.")
			return
		if not volume.probe_irradiance_is_finite():
			push_error("One-bounce irradiance contains NaN or Inf.")
			return

	volume.debug_mode = start_debug_mode


func _offset_light() -> void:
	var light := find_child("CeilingLight", true, false) as OmniLight3D
	if light == null:
		return
	light.position = Vector3(light_x, light.position.y, light.position.z)


func _clear_extra_occluders() -> void:
	for piece: MeshInstance3D in _extra_occluders:
		if is_instance_valid(piece):
			piece.free()
	_extra_occluders.clear()


func _spawn_occluder_box(size: Vector3, position: Vector3) -> void:
	var mesh := BoxMesh.new()
	mesh.size = size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = position
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.material_override = _WHITE_MATERIAL
	add_child(instance)
	_extra_occluders.append(instance)


func _hide_scene_occluder() -> void:
	var occluder := find_child("DividerWall", true, false) as MeshInstance3D
	if occluder == null:
		occluder = find_child("ThinWall", true, false) as MeshInstance3D
	if occluder == null:
		return
	occluder.visible = false
	occluder.gi_mode = GeometryInstance3D.GI_MODE_DISABLED


func _apply_occluder() -> void:
	_clear_extra_occluders()
	_hide_scene_occluder()

	var thickness := wall_thickness_cm * 0.01
	match divider_mode:
		DividerMode.CLOSED:
			_spawn_occluder_box(Vector3(thickness, 4.0, 4.2), Vector3(0.0, 2.0, 0.0))
		DividerMode.DOORWAY:
			_spawn_occluder_box(Vector3(thickness, 4.0, 3.2), Vector3(0.0, 2.0, -0.5))
		DividerMode.WINDOW:
			_spawn_occluder_box(Vector3(thickness, 1.5, 4.2), Vector3(0.0, 0.75, 0.0))
			_spawn_occluder_box(Vector3(thickness, 1.3, 4.2), Vector3(0.0, 3.35, 0.0))
			_spawn_occluder_box(Vector3(thickness, 1.2, 1.5), Vector3(0.0, 2.1, -1.35))
			_spawn_occluder_box(Vector3(thickness, 1.2, 1.5), Vector3(0.0, 2.1, 1.35))
