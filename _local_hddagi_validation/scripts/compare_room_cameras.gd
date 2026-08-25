extends Node3D

## Switches the current camera between two closed Cornell interiors.
##
## Rooms stay sealed so bounced GI remains visible. Tab compares Local vs Global.
## WASD flies the active camera; hold right mouse to look around.

## Fly speed for WASD movement, in meters per second.
@export var move_speed: float = 3.5
## Extra fly speed while Shift is held, in meters per second.
@export var sprint_speed: float = 7.0
## Radians per pixel for right-mouse look.
@export var look_sensitivity: float = 0.004

@onready var _local_camera: Camera3D = $LocalRoom/Camera3D
@onready var _global_camera: Camera3D = $GlobalRoom/Camera3D
@onready var _overlay: CanvasLayer = $Overlay

const _MIN_PITCH: float = deg_to_rad(-89.0)
const _MAX_PITCH: float = deg_to_rad(89.0)

var _show_global: bool = false


func _ready() -> void:
	_apply_view()


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and not key.echo and key.keycode == KEY_TAB:
			_show_global = not _show_global
			_apply_view()
			get_viewport().set_input_as_handled()
			return

	if event is InputEventMouseButton:
		var mouse := event as InputEventMouseButton
		if mouse.button_index == MOUSE_BUTTON_RIGHT:
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED if mouse.pressed else Input.MOUSE_MODE_VISIBLE
			get_viewport().set_input_as_handled()
			return

	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		var motion := event as InputEventMouseMotion
		_look_active_camera(motion.relative)
		get_viewport().set_input_as_handled()


func _process(delta: float) -> void:
	var camera := _active_camera()
	var wish := Vector3.ZERO
	if Input.is_physical_key_pressed(KEY_W):
		wish -= camera.global_transform.basis.z
	if Input.is_physical_key_pressed(KEY_S):
		wish += camera.global_transform.basis.z
	if Input.is_physical_key_pressed(KEY_A):
		wish -= camera.global_transform.basis.x
	if Input.is_physical_key_pressed(KEY_D):
		wish += camera.global_transform.basis.x
	if Input.is_physical_key_pressed(KEY_E) or Input.is_physical_key_pressed(KEY_SPACE):
		wish += Vector3.UP
	if Input.is_physical_key_pressed(KEY_Q) or Input.is_physical_key_pressed(KEY_CTRL):
		wish -= Vector3.UP
	if wish.is_zero_approx():
		return
	var speed := sprint_speed if Input.is_physical_key_pressed(KEY_SHIFT) else move_speed
	camera.global_position += wish.normalized() * speed * delta


func _active_camera() -> Camera3D:
	return _global_camera if _show_global else _local_camera


func _look_active_camera(relative: Vector2) -> void:
	var camera := _active_camera()
	var euler := camera.rotation
	euler.y -= relative.x * look_sensitivity
	euler.x = clampf(euler.x - relative.y * look_sensitivity, _MIN_PITCH, _MAX_PITCH)
	camera.rotation = euler


func _apply_view() -> void:
	_local_camera.current = not _show_global
	_global_camera.current = _show_global
	if _overlay.has_method("set_active_view"):
		var view_name := "GLOBAL ONLY interior" if _show_global else "LOCAL interior"
		_overlay.call("set_active_view", view_name)
