extends Node3D

## Moves and rotates [LocalDynamicGI3D] without changing descendant Local transforms.

## World-space translation amplitude applied to the Local root.
@export var translation_amplitude: Vector3 = Vector3(1.4, 0.35, 0.8)
## Cycles per second for the translation path.
@export var translation_speed: float = 0.35
## Radians per second of yaw applied to the Local root.
@export var rotation_speed: float = 0.4

var _elapsed: float = 0.0
var _rest_transform: Transform3D = Transform3D.IDENTITY


func _ready() -> void:
	_rest_transform = transform


func _process(delta: float) -> void:
	_elapsed += delta
	var offset := Vector3(
			sin(_elapsed * TAU * translation_speed) * translation_amplitude.x,
			sin(_elapsed * TAU * translation_speed * 0.7) * translation_amplitude.y,
			cos(_elapsed * TAU * translation_speed * 0.85) * translation_amplitude.z
	)
	var yaw := _elapsed * rotation_speed
	transform = _rest_transform * Transform3D(Basis.from_euler(Vector3(0.0, yaw, 0.0)), offset)
