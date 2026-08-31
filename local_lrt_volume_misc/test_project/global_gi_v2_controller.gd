extends Node

## Drives the v2 World/Sky to Local LRT validation scene.
##
## Volume rotation explicitly rebuilds geometry because this benchmark rotates
## the volume relative to the Cornell geometry rather than moving both together.

@onready var volume: LocalLRTVolume3D = $"../LocalLRTVolume3D"
@onready var world_environment: WorldEnvironment = $"../ReferenceEnvironment"
@onready var status_label: Label = $"../DebugUI/StatusLabel"

var _pose_index: int = 0


func _ready() -> void:
	set_validation_pose(0)


func _unhandled_key_input(event: InputEvent) -> void:
	if not event.is_pressed() or event.is_echo():
		return
	match event.keycode:
		KEY_R:
			set_validation_pose(1 if _pose_index != 1 else 0)
		KEY_E:
			set_validation_pose(2 if _pose_index != 2 else 0)
		KEY_V:
			volume.debug_draw = not volume.debug_draw


## Applies the frozen v2 benchmark pose.
##
## Pose 0 is the authored world orientation, pose 1 rotates and rebuilds the
## Local LRT volume, and pose 2 rotates only the world sky.
func set_validation_pose(pose_index: int) -> void:
	_pose_index = clampi(pose_index, 0, 2)
	volume.rotation = Vector3.ZERO
	world_environment.environment.sky_rotation = Vector3.ZERO
	if _pose_index == 1:
		volume.rotation.y = PI * 0.5
		volume.rebuild()
	elif _pose_index == 2:
		world_environment.environment.sky_rotation.x = PI * 0.5
	status_label.text = "Pose %d | World/Sky → Local SH → Global Visibility → LTM" % _pose_index
