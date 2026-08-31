extends Node

## Controls and reports the V1.2 Local Geometry Source reuse benchmark.
##
## The selected dynamic object is moved without calling
## [method LocalLRTVolume3D.rebuild]. Keys 1–3 select the cube, sphere, and
## slope; WASD and Q/E move and rotate it, while R restores pose A.


const MOVE_SPEED: float = 1.5
const ROTATION_SPEED: float = 1.2

@onready var _volume: LocalLRTVolume3D = $"../LocalLRTVolume3D" as LocalLRTVolume3D
@onready var _status_label: Label = $"../DebugUI/StatusLabel" as Label
@onready var _dynamic_objects: Array[MeshInstance3D] = [
	$"../DynamicCube" as MeshInstance3D,
	$"../DynamicSphere" as MeshInstance3D,
	$"../DynamicSlope" as MeshInstance3D,
]

var _initial_transforms: Array[Transform3D] = []
var _active_index: int = 0


func _ready() -> void:
	for dynamic_object: MeshInstance3D in _dynamic_objects:
		_initial_transforms.push_back(dynamic_object.transform)
	_update_status()


func _process(delta: float) -> void:
	var movement := Vector3(
		float(Input.is_key_pressed(KEY_D)) - float(Input.is_key_pressed(KEY_A)),
		0.0,
		float(Input.is_key_pressed(KEY_S)) - float(Input.is_key_pressed(KEY_W))
	)
	var rotation_direction := float(Input.is_key_pressed(KEY_E)) - float(Input.is_key_pressed(KEY_Q))
	if movement.is_zero_approx() and is_zero_approx(rotation_direction):
		return

	var active_object := _dynamic_objects[_active_index]
	if !movement.is_zero_approx():
		active_object.position += movement.normalized() * MOVE_SPEED * delta
	if !is_zero_approx(rotation_direction):
		active_object.rotate_y(rotation_direction * ROTATION_SPEED * delta)
	_update_status()


func _unhandled_key_input(event: InputEvent) -> void:
	if !event.is_pressed() or event.is_echo():
		return

	match event.keycode:
		KEY_1, KEY_2, KEY_3:
			_active_index = int(event.keycode - KEY_1)
		KEY_R:
			set_validation_pose(0)
		KEY_V:
			_volume.debug_draw = !_volume.debug_draw
		_:
			return
	_update_status()


## Applies deterministic benchmark pose A ([param pose] = 0) or B (1).
func set_validation_pose(pose: int) -> void:
	if pose == 0:
		for index: int in _dynamic_objects.size():
			_dynamic_objects[index].transform = _initial_transforms[index]
	elif pose == 1:
		_dynamic_objects[0].position = Vector3(1.4, -1.8, 1.7)
		_dynamic_objects[0].rotation = Vector3(0.0, deg_to_rad(35.0), 0.0)
		_dynamic_objects[1].position = Vector3(-0.15, -1.72, 1.2)
		_dynamic_objects[1].rotation = Vector3(0.0, deg_to_rad(-20.0), 0.0)
		_dynamic_objects[2].position = Vector3(-1.55, -2.08, 1.65)
		_dynamic_objects[2].rotation = Vector3(deg_to_rad(24.0), deg_to_rad(-30.0), 0.0)
	_update_status()


func _update_status() -> void:
	var active_object := _dynamic_objects[_active_index]
	var total_probe_count := _volume.get_resolution().x * _volume.get_resolution().y * _volume.get_resolution().z
	_status_label.text = "%s | Pos: (%.2f, %.2f, %.2f) | Dirty: %d/%d | SDF builds: %d | CPU: %.2f ms" % [
		active_object.name,
		active_object.position.x,
		active_object.position.y,
		active_object.position.z,
		_volume.get_last_geometry_update_probe_count(),
		total_probe_count,
		_volume.get_sdf_build_count(),
		float(_volume.get_last_geometry_update_usec()) / 1000.0,
	]
