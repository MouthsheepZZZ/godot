extends Node

## Drives the v2 constant World diffuse validation scene.
##
## Volume rotation explicitly rebuilds geometry because this benchmark rotates
## the volume relative to the Cornell geometry rather than moving both together.

@onready var volume: LocalLRTVolume3D = $"../LocalLRTVolume3D"
@onready var debug_ui: CanvasLayer = $"../DebugUI"
@onready var status_label: Label = $"../DebugUI/StatusLabel"

var _pose_index: int = 0


func _ready() -> void:
	debug_ui.visible = true
	set_validation_pose(0)


func _unhandled_key_input(event: InputEvent) -> void:
	if not event.is_pressed() or event.is_echo():
		return
	var keycode: int = event.physical_keycode if event.physical_keycode != KEY_NONE else event.keycode
	match keycode:
		KEY_R:
			set_validation_pose(1 if _pose_index != 1 else 0)
		KEY_E:
			set_validation_pose(2 if _pose_index != 2 else 0)
		KEY_V:
			volume.debug_draw = not volume.debug_draw


## Applies the frozen v2 benchmark pose.
##
## Pose 0 is the authored volume orientation, pose 1 rotates and rebuilds the
## Local LRT volume, and pose 2 repeats the constant-World reference.
func set_validation_pose(pose_index: int) -> void:
	_pose_index = clampi(pose_index, 0, 2)
	volume.rotation = Vector3.ZERO
	if _pose_index == 1:
		volume.rotation.y = PI * 0.5
		volume.rebuild()
	var pose_description: String = "Authored orientation"
	if _pose_index == 1:
		pose_description = "Volume Y = 90° (R applied)"
	elif _pose_index == 2:
		pose_description = "Constant-world reference"
	status_label.text = "Pose %d | %s | Output should remain invariant" % [_pose_index, pose_description]
