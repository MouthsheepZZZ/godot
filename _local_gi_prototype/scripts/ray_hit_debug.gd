@tool
extends Node3D

## Scene C helper: bake, upload GPU BVHs, and show ray/hit debug.


func _ready() -> void:
	var volume := find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D
	if volume == null:
		push_error("Scene C is missing LocalGIVolume3D.")
		return

	volume.debug_mode = LocalGIVolume3D.DEBUG_RAY_HIT_MISS
	volume.bake()
	volume.update_dynamic()
	if not volume.is_gpu_available():
		push_warning("LocalGI GPU tracer is unavailable; drawing CPU ray debug.")
		return
	if not volume.upload_gpu():
		push_warning("LocalGI GPU upload failed; drawing CPU ray debug.")
		return

	var origins := PackedVector3Array()
	var directions := PackedVector3Array()
	var aabb: AABB = volume.get_aabb()
	for y_i: int in 9:
		for z_i: int in 9:
			var u := (float(y_i) + 0.5) / 9.0
			var v := (float(z_i) + 0.5) / 9.0
			origins.append(Vector3(aabb.position.x + aabb.size.x * 0.05, aabb.position.y + aabb.size.y * u, aabb.position.z + aabb.size.z * v))
			directions.append(Vector3.RIGHT)

	var compare: Dictionary = volume.compare_cpu_gpu_rays(origins, directions)
	if not bool(compare.get("passed", false)):
		push_warning("CPU/GPU debug rays did not match: %s" % compare)
