extends Node

## Drives the v3 multi-volume priority and overlap blend scene.
##
## Volume A covers the full Cornell room. Volume B overlaps the right side with
## an independent local transform. Priority cascade blend is evaluated in Forward.

@onready var volume_a: LocalLRTVolume3D = $"../VolumeA"
@onready var volume_b: LocalLRTVolume3D = $"../VolumeB"
@onready var debug_ui: CanvasLayer = $"../DebugUI"
@onready var status_label: Label = $"../DebugUI/StatusLabel"

var _rotate_b: bool = false


func _ready() -> void:
	debug_ui.visible = true
	_update_status()


func _unhandled_key_input(event: InputEvent) -> void:
	if not event.is_pressed() or event.is_echo():
		return
	var keycode: int = event.physical_keycode if event.physical_keycode != KEY_NONE else event.keycode
	match keycode:
		KEY_1:
			volume_a.enabled = not volume_a.enabled
			_update_status()
		KEY_2:
			volume_b.enabled = not volume_b.enabled
			_update_status()
		KEY_P:
			var priority_a: int = volume_a.priority
			volume_a.priority = volume_b.priority
			volume_b.priority = priority_a
			_update_status()
		KEY_R:
			_rotate_b = not _rotate_b
			volume_b.rotation.y = PI * 0.5 if _rotate_b else 0.0
			volume_b.rebuild()
			_update_status()
		KEY_V:
			var next_debug: bool = not volume_a.debug_draw
			volume_a.debug_draw = next_debug
			volume_b.debug_draw = next_debug
			_update_status()


func _update_status() -> void:
	var rotation_text: String = "Volume B Y = 90°" if _rotate_b else "Volume B authored orientation"
	status_label.text = "A pri %d %s | B pri %d %s | %s | 1/2 toggle, P swap priority, R rotate B, V debug" % [
		volume_a.priority,
		"on" if volume_a.enabled else "off",
		volume_b.priority,
		"on" if volume_b.enabled else "off",
		rotation_text,
	]
