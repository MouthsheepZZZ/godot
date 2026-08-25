extends LocalDynamicGI3D

## Slides the Local root in world space so Global indirect through a window changes.


## World-space slide amplitude applied to the Local root.
@export var slide_amplitude: Vector3 = Vector3(2.4, 0.0, 0.0)
## Cycles per second of the slide.
@export var slide_speed: float = 0.12

var _elapsed: float = 0.0
var _rest_transform: Transform3D = Transform3D.IDENTITY


func _ready() -> void:
	_rest_transform = transform


func _process(delta: float) -> void:
	_elapsed += delta
	var offset := Vector3(
			sin(_elapsed * TAU * slide_speed) * slide_amplitude.x,
			sin(_elapsed * TAU * slide_speed * 0.35) * slide_amplitude.y,
			cos(_elapsed * TAU * slide_speed * 0.45) * slide_amplitude.z
	)
	transform = Transform3D(_rest_transform.basis, _rest_transform.origin + offset)
