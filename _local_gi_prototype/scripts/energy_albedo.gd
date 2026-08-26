@tool
extends Node3D

## Scene B helper: apply a uniform Lambertian albedo and run controlled multi-bounce.

@export_range(0.05, 0.95, 0.01) var albedo: float = 0.5:
	set(value):
		albedo = clampf(value, 0.05, 0.95)
		_queue_refresh()
@export var multi_bounce_enabled: bool = true:
	set(value):
		multi_bounce_enabled = value
		_queue_refresh()
@export var start_from_zero: bool = true

var _volume: LocalGIVolume3D
var _refresh_queued: bool = false


func _ready() -> void:
	_refresh()


func _queue_refresh() -> void:
	if not is_inside_tree() or not is_node_ready() or _refresh_queued:
		return
	_refresh_queued = true
	_refresh.call_deferred()


func _refresh() -> void:
	_refresh_queued = false
	_apply_albedo(self)
	_volume = find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D
	if _volume == null:
		push_error("Scene is missing LocalGIVolume3D.")
		return

	_volume.multi_bounce_enabled = multi_bounce_enabled
	_volume.temporal_hysteresis = 0.0
	_volume.bake()
	_volume.update_dynamic()
	_volume.build_probes()
	if not _volume.compute_one_bounce():
		push_error("Multi-bounce compute produced no probes.")
		return
	if not _volume.probe_irradiance_is_finite():
		push_error("Multi-bounce irradiance contains NaN or Inf.")
		return
	if start_from_zero:
		_volume.reset_temporal_history()
	_volume.debug_mode = LocalGIVolume3D.DEBUG_PROBE_IRRADIANCE
	set_process(not Engine.is_editor_hint())


func _process(_delta: float) -> void:
	if Engine.is_editor_hint() or _volume == null:
		return
	if not _volume.compute_one_bounce() or not _volume.update_temporal():
		push_error("Multi-bounce update failed.")


func _apply_albedo(node: Node) -> void:
	var mesh_instance := node as MeshInstance3D
	if mesh_instance != null:
		var material := StandardMaterial3D.new()
		material.albedo_color = Color(albedo, albedo, albedo, 1.0)
		material.metallic = 0.0
		material.roughness = 1.0
		mesh_instance.material_override = material

	for child: Node in node.get_children():
		_apply_albedo(child)
