extends SceneTree


func _init() -> void:
	var failed := 0
	failed += _test_create_delete_and_properties()
	failed += _test_bounds_extend_and_gi_mode()
	failed += _test_nested_local_excluded()
	failed += _test_multiple_registration()

	if failed == 0:
		print("TASK1_PLUMBING_TEST PASS")
		quit(0)
	else:
		push_error("TASK1_PLUMBING_TEST FAIL count=%d" % failed)
		quit(1)


func _test_create_delete_and_properties() -> int:
	for _i in range(3):
		var local_gi := LocalDynamicGI3D.new()
		if not local_gi.get_local_dynamic_gi_rid().is_valid():
			push_error("RID was invalid after create")
			local_gi.free()
			return 1
		local_gi.enabled = false
		local_gi.extend = Vector3(1, 2, 3)
		local_gi.blend_distance = 1.25
		if local_gi.enabled or local_gi.extend != Vector3(1, 2, 3) or not is_equal_approx(local_gi.blend_distance, 1.25):
			push_error("Property storage mismatch")
			local_gi.free()
			return 1
		local_gi.free()
	return 0


func _make_box(position: Vector3, size: Vector3, gi_mode: GeometryInstance3D.GIMode) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = position
	instance.gi_mode = gi_mode
	return instance


func _test_bounds_extend_and_gi_mode() -> int:
	var local_gi := LocalDynamicGI3D.new()
	local_gi.extend = Vector3(0.5, 0.5, 0.5)

	local_gi.add_child(_make_box(Vector3.ZERO, Vector3(2, 2, 2), GeometryInstance3D.GI_MODE_STATIC))
	local_gi.add_child(_make_box(Vector3(4, 0, 0), Vector3(2, 2, 2), GeometryInstance3D.GI_MODE_DYNAMIC))
	local_gi.add_child(_make_box(Vector3(20, 0, 0), Vector3(2, 2, 2), GeometryInstance3D.GI_MODE_DISABLED))
	var light := OmniLight3D.new()
	light.position = Vector3(0, 2, 0)
	local_gi.add_child(light)
	local_gi.update_local_data()

	if local_gi.get_geometry_contributors().size() != 2:
		push_error("Expected 2 contributors")
		local_gi.free()
		return 1
	if local_gi.get_receive_only_geometry().size() != 1:
		push_error("Expected 1 receive-only mesh")
		local_gi.free()
		return 1
	if local_gi.get_lights().size() != 1:
		push_error("Expected 1 local light")
		local_gi.free()
		return 1

	var bounds := local_gi.get_local_bounds()
	if not bounds.position.is_equal_approx(Vector3(-1.5, -1.5, -1.5)):
		push_error("Unexpected bounds position %s" % bounds.position)
		local_gi.free()
		return 1
	if not bounds.size.is_equal_approx(Vector3(7, 3, 3)):
		push_error("Unexpected bounds size %s" % bounds.size)
		local_gi.free()
		return 1
	if bounds.has_point(Vector3(20, 0, 0)):
		push_error("Disabled geometry must not expand bounds")
		local_gi.free()
		return 1

	local_gi.free()
	return 0


func _test_nested_local_excluded() -> int:
	var parent := LocalDynamicGI3D.new()
	parent.add_child(_make_box(Vector3.ZERO, Vector3(2, 2, 2), GeometryInstance3D.GI_MODE_STATIC))

	var nested := LocalDynamicGI3D.new()
	parent.add_child(nested)
	nested.add_child(_make_box(Vector3(8, 0, 0), Vector3(2, 2, 2), GeometryInstance3D.GI_MODE_STATIC))
	parent.update_local_data()
	nested.update_local_data()

	if parent.get_geometry_contributors().size() != 1 or nested.get_geometry_contributors().size() != 1:
		push_error("Nested Local descendants were collected by the parent")
		parent.free()
		return 1
	if parent.get_local_bounds().has_point(Vector3(8, 0, 0)):
		push_error("Nested Local mesh expanded parent bounds")
		parent.free()
		return 1

	parent.free()
	return 0


func _test_multiple_registration() -> int:
	var first := LocalDynamicGI3D.new()
	var second := LocalDynamicGI3D.new()
	first.enabled = false
	second.blend_distance = 1.75
	first.update_local_data()
	second.update_local_data()

	var first_rid := first.get_local_dynamic_gi_rid()
	var second_rid := second.get_local_dynamic_gi_rid()
	if not first_rid.is_valid() or not second_rid.is_valid() or first_rid == second_rid:
		push_error("Local RIDs must be valid and unique")
		first.free()
		second.free()
		return 1
	if RenderingServer.local_dynamic_gi_is_enabled(first_rid) or not RenderingServer.local_dynamic_gi_is_enabled(second_rid):
		push_error("Per-node enabled state was not stored independently")
		first.free()
		second.free()
		return 1
	if not is_equal_approx(RenderingServer.local_dynamic_gi_get_blend_distance(second_rid), 1.75):
		push_error("Per-node blend distance was not stored independently")
		first.free()
		second.free()
		return 1

	first.free()
	if not second_rid.is_valid() or not RenderingServer.local_dynamic_gi_is_enabled(second_rid):
		push_error("Deleting one Local node corrupted the other")
		second.free()
		return 1

	second.free()
	return 0
