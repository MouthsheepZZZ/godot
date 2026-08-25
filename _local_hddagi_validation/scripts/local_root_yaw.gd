extends Node3D

## Yaws [LocalDynamicGI3D] at a constant speed. No translation.

## Radians per second of yaw applied to the Local root.
@export var rotation_speed: float = 0.25

var _elapsed: float = 0.0
var _rest_transform: Transform3D = Transform3D.IDENTITY


func _ready() -> void:
	_rest_transform = transform


func _process(delta: float) -> void:
	_elapsed += delta
	var yaw := _elapsed * rotation_speed
	transform = _rest_transform * Transform3D(Basis.from_euler(Vector3(0.0, yaw, 0.0)), Vector3.ZERO)
