extends Camera3D

## Runtime fly camera. Hold right mouse to look; WASD moves, QE down/up.

@export var move_speed: float = 4.0
@export var fast_multiplier: float = 3.0
@export var mouse_sensitivity: float = 0.0025

var _yaw: float = 0.0
var _pitch: float = 0.0
var _look_active: bool = false


func _unhandled_input(event: InputEvent) -> void:
	if Engine.is_editor_hint():
		return

	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_RIGHT:
		_set_look_active(event.pressed)
		get_viewport().set_input_as_handled()
		return

	if _look_active and event is InputEventMouseMotion:
		_yaw -= event.relative.x * mouse_sensitivity
		_pitch -= event.relative.y * mouse_sensitivity
		_pitch = clampf(_pitch, deg_to_rad(-89.0), deg_to_rad(89.0))
		rotation = Vector3(_pitch, _yaw, 0.0)
		get_viewport().set_input_as_handled()


func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		return

	var wish := Vector3.ZERO
	if Input.is_physical_key_pressed(KEY_W):
		wish -= basis.z
	if Input.is_physical_key_pressed(KEY_S):
		wish += basis.z
	if Input.is_physical_key_pressed(KEY_A):
		wish -= basis.x
	if Input.is_physical_key_pressed(KEY_D):
		wish += basis.x
	if Input.is_physical_key_pressed(KEY_Q):
		wish -= Vector3.UP
	if Input.is_physical_key_pressed(KEY_E):
		wish += Vector3.UP

	if wish.length_squared() <= 0.0:
		return

	var speed := move_speed
	if Input.is_physical_key_pressed(KEY_SHIFT):
		speed *= fast_multiplier
	position += wish.normalized() * speed * delta


func _set_look_active(active: bool) -> void:
	_look_active = active
	if active:
		var euler := rotation
		_yaw = euler.y
		_pitch = euler.x
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	else:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE


func _exit_tree() -> void:
	if _look_active:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
