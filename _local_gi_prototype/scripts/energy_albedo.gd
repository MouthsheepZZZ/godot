extends Node3D

## Scene B helper: apply a uniform Lambertian albedo to every MeshInstance3D.

@export_range(0.05, 0.95, 0.01) var albedo: float = 0.5


func _ready() -> void:
	_apply_albedo(self)


func _apply_albedo(node: Node) -> void:
	var mesh_instance := node as MeshInstance3D
	if mesh_instance != null:
		var material := StandardMaterial3D.new()
		material.albedo_color = Color(albedo, albedo, albedo, 1.0)
		material.metallic = 0.0
		material.roughness = 1.0
		mesh_instance.material_override = material

	for child: Node in node.get_children():
		_apply_albedo(child)
