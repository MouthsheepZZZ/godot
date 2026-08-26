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
