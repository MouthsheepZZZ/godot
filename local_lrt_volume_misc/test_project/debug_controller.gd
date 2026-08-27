class_name LocalLRTDebugController
extends Node

## Provides minimal keyboard controls for the Cornell Box analytic lights.
##
## The controller intentionally owns no gameplay state. It only mutates the
## three test lights so later Local LRT stages can reuse the same scene.

const LIGHT_COLORS: Array[Color] = [
	Color(1.0, 0.92, 0.78),
	Color(1.0, 0.25, 0.18),
	Color(0.2, 0.45, 1.0),
	Color(0.35, 1.0, 0.45),
]
const MOVE_SPEED: float = 2.5
const ROTATION_SPEED: float = 1.5
const ENERGY_STEP: float = 0.5
const RANGE_STEP: float = 0.5

@onready var _lights: Array[Light3D] = [
	$"../DirectionalLight3D" as Light3D,
	$"../OmniLight3D" as Light3D,
	$"../SpotLight3D" as Light3D,
]
@onready var _status_label: Label = $"../DebugUI/StatusLabel"

var _selected_light: int = 0
var _color_index: int = 0
var _isolate_selected: bool = false


func _ready() -> void:
	_update_status()


func _process(delta: float) -> void:
	var movement := Vector3(
		float(Input.is_key_pressed(KEY_D)) - float(Input.is_key_pressed(KEY_A)),
		float(Input.is_key_pressed(KEY_E)) - float(Input.is_key_pressed(KEY_Q)),
		float(Input.is_key_pressed(KEY_S)) - float(Input.is_key_pressed(KEY_W))
	)
	if !movement.is_zero_approx():
		_lights[_selected_light].position += movement.normalized() * MOVE_SPEED * delta
		_update_status()

	var rotation := Vector2(
		float(Input.is_key_pressed(KEY_DOWN)) - float(Input.is_key_pressed(KEY_UP)),
		float(Input.is_key_pressed(KEY_RIGHT)) - float(Input.is_key_pressed(KEY_LEFT))
	)
	if !rotation.is_zero_approx():
		_lights[_selected_light].rotate_x(rotation.x * ROTATION_SPEED * delta)
		_lights[_selected_light].rotate_y(rotation.y * ROTATION_SPEED * delta)
		_update_status()


func _unhandled_key_input(event: InputEvent) -> void:
	if !event.is_pressed() or event.is_echo():
		return

	match event.keycode:
		KEY_TAB:
			_selected_light = (_selected_light + 1) % _lights.size()
			if _isolate_selected:
				_apply_isolation()
		KEY_I:
			_isolate_selected = !_isolate_selected
			_apply_isolation()
		KEY_SPACE:
			_lights[_selected_light].visible = !_lights[_selected_light].visible
		KEY_C:
			_color_index = (_color_index + 1) % LIGHT_COLORS.size()
			_lights[_selected_light].light_color = LIGHT_COLORS[_color_index]
		KEY_EQUAL, KEY_KP_ADD:
			_lights[_selected_light].light_energy += ENERGY_STEP
		KEY_MINUS, KEY_KP_SUBTRACT:
			_lights[_selected_light].light_energy = maxf(0.0, _lights[_selected_light].light_energy - ENERGY_STEP)
		KEY_BRACKETLEFT:
			_change_range(-RANGE_STEP)
		KEY_BRACKETRIGHT:
			_change_range(RANGE_STEP)
		_:
			return
	_update_status()


func _apply_isolation() -> void:
	for index in _lights.size():
		_lights[index].visible = !_isolate_selected or index == _selected_light


func _change_range(delta: float) -> void:
	var light := _lights[_selected_light]
	if light is OmniLight3D:
		var omni := light as OmniLight3D
		omni.omni_range = maxf(0.5, omni.omni_range + delta)
	elif light is SpotLight3D:
		var spot := light as SpotLight3D
		spot.spot_range = maxf(0.5, spot.spot_range + delta)


func _update_status() -> void:
	var light := _lights[_selected_light]
	var range_text := "N/A"
	if light is OmniLight3D:
		range_text = "%.1f" % (light as OmniLight3D).omni_range
	elif light is SpotLight3D:
		range_text = "%.1f" % (light as SpotLight3D).spot_range
	_status_label.text = "Selected: %s | Pos: %s | Rot: %s | Energy: %.1f | Range: %s | Isolated: %s" % [
		light.name,
		_format_vector(light.position),
		_format_vector(light.rotation_degrees),
		light.light_energy,
		range_text,
		str(_isolate_selected),
	]


func _format_vector(value: Vector3) -> String:
	return "(%.1f, %.1f, %.1f)" % [value.x, value.y, value.z]
