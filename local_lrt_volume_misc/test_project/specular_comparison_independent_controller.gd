extends Node3D

## Controls the fully self-contained DynamicGI and Local LRT specular comparison.
##
## Both Cornell groups are authored in this scene. The left group is the
## DynamicGI reference and the right group enables Local LRT for comparison.

const MODE_COMPARISON: int = 0
const MODE_DYNAMIC_GI_ONLY: int = 1
const MODE_LOCAL_LRT_ONLY: int = 2

@onready var reference_environment: WorldEnvironment = $ReferenceEnvironment
@onready var dynamic_volume: LocalLRTVolume3D = $DynamicGIGroup/LocalLRTVolume3D
@onready var local_lrt_volume: LocalLRTVolume3D = $LocalLRTGroup/LocalLRTVolume3D
@onready var status_label: Label = $DebugUI/StatusLabel

var _mode: int = MODE_COMPARISON


func _ready() -> void:
	reference_environment.environment.dynamic_gi_enabled = true
	_set_mode(MODE_COMPARISON)


func _unhandled_key_input(event: InputEvent) -> void:
	if not event.is_pressed() or event.is_echo():
		return
	var keycode: int = event.physical_keycode if event.physical_keycode != KEY_NONE else event.keycode
	match keycode:
		KEY_1:
			_set_mode(MODE_COMPARISON)
		KEY_2:
			_set_mode(MODE_DYNAMIC_GI_ONLY)
		KEY_3:
			_set_mode(MODE_LOCAL_LRT_ONLY)
		KEY_V:
			local_lrt_volume.debug_draw = not local_lrt_volume.debug_draw
			_update_status()
		_:
			return


func _set_mode(mode: int) -> void:
	_mode = mode
	dynamic_volume.enabled = _mode == MODE_LOCAL_LRT_ONLY
	local_lrt_volume.enabled = _mode != MODE_DYNAMIC_GI_ONLY
	_update_status()


func _update_status() -> void:
	var mode_text: String = "Comparison: left DynamicGI / right Local LRT"
	if _mode == MODE_DYNAMIC_GI_ONLY:
		mode_text = "DynamicGI only: both groups use DynamicGI"
	elif _mode == MODE_LOCAL_LRT_ONLY:
		mode_text = "Local LRT diffuse: both groups use Local LRT"
	status_label.text = "%s | 1: Comparison | 2: DynamicGI | 3: Local LRT | V: probes" % mode_text
