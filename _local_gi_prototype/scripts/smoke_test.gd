extends SceneTree

const SCENE_PATHS: PackedStringArray = [
	"res://scenes/a_cornell_baseline.tscn",
	"res://scenes/b_white_cornell_energy.tscn",
	"res://scenes/c_cornell_thin_wall.tscn",
	"res://scenes/d_two_chamber_cornell.tscn",
	"res://scenes/e_open_cornell_external_gi.tscn",
	"res://scenes/f_dynamic_object_cornell.tscn",
	"res://scenes/g_moving_local_volume.tscn",
	"res://scenes/h_performance_cornell.tscn",
]


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var failed: int = 0

	if not ClassDB.class_exists("LocalGIVolume3D"):
		push_error("LocalGIVolume3D is not registered.")
		quit(1)
		return

	var skeleton: Node = ClassDB.instantiate("LocalGIVolume3D") as Node
	if skeleton == null:
		push_error("LocalGIVolume3D could not be instantiated.")
		quit(1)
		return
	skeleton.free()

	for path: String in SCENE_PATHS:
		if not ResourceLoader.exists(path):
			push_error("Missing scene: %s" % path)
			failed += 1
			continue

		var packed: PackedScene = ResourceLoader.load(path) as PackedScene
		if packed == null:
			push_error("Failed to load: %s" % path)
			failed += 1
			continue

		var instance: Node = packed.instantiate()
		if instance == null:
			push_error("Failed to instantiate: %s" % path)
			failed += 1
			continue

		var volume: Node = instance.find_child("LocalGIVolume3D", true, false)
		if volume == null:
			push_error("Scene has no LocalGIVolume3D: %s" % path)
			failed += 1
			instance.free()
			continue

		root.add_child(instance)
		var local_gi: LocalGIVolume3D = volume as LocalGIVolume3D
		if local_gi == null:
			push_error("LocalGIVolume3D cast failed: %s" % path)
			failed += 1
		else:
			local_gi.bake()
			if local_gi.get_baked_triangle_count() <= 0:
				push_error("Bake produced no static triangles: %s" % path)
				failed += 1
			local_gi.update_dynamic()
			if local_gi.is_gpu_available():
				if not local_gi.upload_gpu():
					push_error("GPU upload failed: %s" % path)
					failed += 1
				else:
					var origins := PackedVector3Array([Vector3(0, 0, -2), Vector3(0, 2, 0), Vector3(3, 3, 3)])
					var directions := PackedVector3Array([Vector3(0, 0, 1), Vector3(0, -1, 0), Vector3(1, 0, 0)])
					var compare: Dictionary = local_gi.compare_cpu_gpu_rays(origins, directions)
					if not bool(compare.get("passed", false)):
						push_error("CPU/GPU ray mismatch: %s %s" % [path, compare])
						failed += 1
			local_gi.build_probes()
			if local_gi.get_probe_count() <= 0:
				push_error("Probe build produced no probes: %s" % path)
				failed += 1
			elif local_gi.get_probe_ray_budget() != local_gi.get_probe_count() * local_gi.rays_per_probe:
				push_error("Probe ray budget mismatch: %s" % path)
				failed += 1
			else:
				var directions: PackedVector3Array = local_gi.get_probe_directions()
				if directions.size() != local_gi.rays_per_probe:
					push_error("Probe direction count mismatch: %s" % path)
					failed += 1
				for direction: Vector3 in directions:
					if not direction.is_normalized():
						push_error("Probe direction is not normalized: %s" % path)
						failed += 1
						break
				var local_positions: PackedVector3Array = local_gi.get_probe_positions()
				var original_xform: Transform3D = local_gi.global_transform
				local_gi.global_transform = Transform3D(Basis.from_euler(Vector3(0.3, 1.2, -0.4)), Vector3(2, -1, 3))
				local_gi.build_probes()
				var moved_positions: PackedVector3Array = local_gi.get_probe_positions()
				local_gi.global_transform = original_xform
				if moved_positions != local_positions:
					push_error("Volume transform changed local probe positions: %s" % path)
					failed += 1
			if not local_gi.compute_one_bounce():
				push_error("One-bounce compute failed: %s" % path)
				failed += 1
			elif not local_gi.probe_irradiance_is_finite():
				push_error("One-bounce irradiance is not finite: %s" % path)
				failed += 1
			elif path.ends_with("b_white_cornell_energy.tscn"):
				var mean_irradiance: Color = local_gi.get_mean_probe_irradiance()
				if mean_irradiance.get_luminance() <= 0.0:
					push_error("White Cornell energy scene produced zero GI: %s" % path)
					failed += 1
			var shading: Dictionary = local_gi.sample_shading(Vector3(0.0, 0.0, 0.0), Vector3.UP)
			if not bool(shading.get("finite", false)):
				push_error("Shading sample is not finite: %s" % path)
				failed += 1
			elif float(shading.get("weight_sum", -1.0)) < 0.0:
				push_error("Shading weight sum is negative: %s" % path)
				failed += 1
			if path.ends_with("c_cornell_thin_wall.tscn") or path.ends_with("d_two_chamber_cornell.tscn"):
				var lit_side: Color = local_gi.sample_indirect_irradiance(Vector3(-1.0, 0.0, -0.8), Vector3.UP)
				var dark_side: Color = local_gi.sample_indirect_irradiance(Vector3(1.0, 0.0, -0.8), Vector3.UP)
				if dark_side.get_luminance() >= lit_side.get_luminance():
					push_error("Dark-side GI is not darker than the lit side: %s" % path)
					failed += 1
			if path.ends_with("f_dynamic_object_cornell.tscn"):
				if local_gi.get_dynamic_triangle_count() <= 0:
					push_error("Dynamic update produced no triangles: %s" % path)
					failed += 1
				var rebuilds: int = local_gi.get_dynamic_rebuild_count()
				if local_gi.update_dynamic():
					push_error("Stationary dynamic contributors rebuilt: %s" % path)
					failed += 1
				if local_gi.get_dynamic_rebuild_count() != rebuilds:
					push_error("Dynamic rebuild count changed without a relevant change: %s" % path)
					failed += 1
		instance.free()

	if failed > 0:
		push_error("LocalGIPrototype smoke test failed: %d scene(s)." % failed)
		quit(1)
		return

	print("LocalGIPrototype smoke test passed.")
	quit(0)
