extends Node

## Provides deterministic controls for the V1.1 dynamic geometry benchmark.
##
## Moving or rotating the cube must invalidate Local LRT automatically; this
## controller never calls [method LocalLRTVolume3D.rebuild].


const MOVE_SPEED: float = 1.5
const ROTATION_SPEED: float = 1.2

@onready var _cube: MeshInstance3D = $"../DynamicCube" as MeshInstance3D
@onready var _volume: LocalLRTVolume3D = $"../LocalLRTVolume3D" as LocalLRTVolume3D
@onready var _status_label: Label = $"../DebugUI/StatusLabel" as Label

var _initial_transform: Transform3D


func _ready() -> void:
	_initial_transform = _cube.transform
	_update_status()


func _process(delta: float) -> void:
	var movement := Vector3(
		float(Input.is_key_pressed(KEY_D)) - float(Input.is_key_pressed(KEY_A)),
		0.0,
		float(Input.is_key_pressed(KEY_S)) - float(Input.is_key_pressed(KEY_W))
	)
	if !movement.is_zero_approx():
		_cube.position += movement.normalized() * MOVE_SPEED * delta

	var rotation_direction := float(Input.is_key_pressed(KEY_E)) - float(Input.is_key_pressed(KEY_Q))
	if !is_zero_approx(rotation_direction):
		_cube.rotate_y(rotation_direction * ROTATION_SPEED * delta)

	if !movement.is_zero_approx() or !is_zero_approx(rotation_direction):
		_update_status()


func _unhandled_key_input(event: InputEvent) -> void:
	if !event.is_pressed() or event.is_echo():
		return

	match event.keycode:
		KEY_R:
			_cube.transform = _initial_transform
		KEY_V:
			_volume.debug_draw = !_volume.debug_draw
		_:
			return
	_update_status()


## Sets the benchmark pose without forcing a Local LRT rebuild.
func set_cube_pose(position: Vector3, yaw: float) -> void:
	_cube.position = position
	_cube.rotation = Vector3(0.0, yaw, 0.0)
	_update_status()


func _update_status() -> void:
	_status_label.text = "Dynamic Cube | Pos: (%.2f, %.2f, %.2f) | Yaw: %.1f° | Geometry: %d | GI: %s" % [
		_cube.position.x,
		_cube.position.y,
		_cube.position.z,
		_cube.rotation_degrees.y,
		_volume.get_built_geometry_count(),
		str(_volume.has_built_data()),
	]
