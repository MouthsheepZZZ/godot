extends MeshInstance3D

## Slides a [constant GeometryInstance3D.GI_MODE_DYNAMIC] occluder in Local space.
##
## Used by [code]03_gi_mode_dynamic.tscn[/code] so Local voxels rebuild while the
## matching Global-only room stays still.

## Local-space slide amplitude along X through the room center.
@export var slide_amplitude: float = 1.35
## Cycles per second of the slide.
@export var slide_speed: float = 0.22

var _elapsed: float = 0.0
var _rest_position: Vector3 = Vector3.ZERO


func _ready() -> void:
	gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
	_rest_position = position


func _process(delta: float) -> void:
	_elapsed += delta
	position = _rest_position + Vector3(sin(_elapsed * TAU * slide_speed) * slide_amplitude, 0.0, 0.0)
