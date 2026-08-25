extends SceneTree

## Automated Task 3 checks for GI mode classification and dirty rebuild.


func _init() -> void:
	var failed := 0
	failed += _test_disabled_is_not_a_contributor()
	failed += _test_root_motion_with_dynamic_child_does_not_rebuild()
	failed += _test_static_local_motion_inside_bounds_does_not_rebuild()
	failed += _test_dynamic_local_motion_requests_rebuild()
	failed += _test_light_motion_does_not_rebuild()

	if failed == 0:
		print("TASK3_GI_MODE_TEST PASS")
		quit(0)
	else:
		push_error("TASK3_GI_MODE_TEST FAIL count=%d" % failed)
		quit(1)


func _make_box(position: Vector3, size: Vector3, gi_mode: GeometryInstance3D.GIMode) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = position
	instance.gi_mode = gi_mode
	return instance


func _test_disabled_is_not_a_contributor() -> int:
	var local_gi := LocalDynamicGI3D.new()
	local_gi.add_child(_make_box(Vector3.ZERO, Vector3(2, 2, 2), GeometryInstance3D.GI_MODE_STATIC))
	local_gi.add_child(_make_box(Vector3(0.4, 0, 0), Vector3(0.3, 0.3, 0.3), GeometryInstance3D.GI_MODE_DISABLED))
	local_gi.update_local_data()
	if local_gi.get_geometry_contributors().size() != 1:
		push_error("DISABLED geometry must not be a Local voxel contributor")
		local_gi.free()
		return 1
	if local_gi.get_receive_only_geometry().size() != 1:
		push_error("DISABLED geometry must be classified as receive-only")
		local_gi.free()
		return 1
	local_gi.free()
	return 0


func _test_root_motion_with_dynamic_child_does_not_rebuild() -> int:
	var local_gi := LocalDynamicGI3D.new()
	local_gi.add_child(_make_box(Vector3.ZERO, Vector3(4, 4, 4), GeometryInstance3D.GI_MODE_STATIC))
	local_gi.add_child(_make_box(Vector3(0.4, 0, 0), Vector3(0.4, 0.4, 0.4), GeometryInstance3D.GI_MODE_DYNAMIC))
	local_gi.update_local_data()
	var rid := local_gi.get_local_dynamic_gi_rid()
	var version_before := RenderingServer.local_dynamic_gi_get_data_version(rid)

	for i in range(6):
		local_gi.position = Vector3(float(i), 0.2 * float(i), -0.3 * float(i))
		local_gi.rotation = Vector3(0.0, 0.15 * float(i), 0.0)
		local_gi.update_local_data()

	var version_after := RenderingServer.local_dynamic_gi_get_data_version(rid)
	if version_after != version_before:
		push_error("Root motion must not dirty Local Dynamic descendants with unchanged Local transforms")
		local_gi.free()
		return 1
	local_gi.free()
	return 0


func _test_static_local_motion_inside_bounds_does_not_rebuild() -> int:
	var local_gi := LocalDynamicGI3D.new()
	local_gi.add_child(_make_box(Vector3.ZERO, Vector3(4, 4, 4), GeometryInstance3D.GI_MODE_STATIC))
	var static_prop := _make_box(Vector3(0.5, 0, 0), Vector3(0.4, 0.4, 0.4), GeometryInstance3D.GI_MODE_STATIC)
	local_gi.add_child(static_prop)
	local_gi.update_local_data()
	var rid := local_gi.get_local_dynamic_gi_rid()
	var version_before := RenderingServer.local_dynamic_gi_get_data_version(rid)

	static_prop.position = Vector3(-0.5, 0.1, 0.2)
	local_gi.update_local_data()
	var version_after := RenderingServer.local_dynamic_gi_get_data_version(rid)
	if version_after != version_before:
		push_error("STATIC Local motion inside unchanged bounds must not request rebuild")
		local_gi.free()
		return 1
	local_gi.free()
	return 0


func _test_dynamic_local_motion_requests_rebuild() -> int:
	var local_gi := LocalDynamicGI3D.new()
	local_gi.add_child(_make_box(Vector3.ZERO, Vector3(4, 4, 4), GeometryInstance3D.GI_MODE_STATIC))
	var dynamic_box := _make_box(Vector3(0.5, 0, 0), Vector3(0.4, 0.4, 0.4), GeometryInstance3D.GI_MODE_DYNAMIC)
	local_gi.add_child(dynamic_box)
	local_gi.update_local_data()
	var rid := local_gi.get_local_dynamic_gi_rid()
	var version_before := RenderingServer.local_dynamic_gi_get_data_version(rid)

	dynamic_box.position = Vector3(-0.5, 0.0, 0.2)
	local_gi.update_local_data()
	var version_after := RenderingServer.local_dynamic_gi_get_data_version(rid)
	if version_after <= version_before:
		push_error("DYNAMIC Local transform change must request a full Local rebuild")
		local_gi.free()
		return 1
	local_gi.free()
	return 0


func _test_light_motion_does_not_rebuild() -> int:
	var local_gi := LocalDynamicGI3D.new()
	local_gi.add_child(_make_box(Vector3.ZERO, Vector3(4, 4, 4), GeometryInstance3D.GI_MODE_STATIC))
	var light := OmniLight3D.new()
	light.position = Vector3(0.0, 1.5, 0.0)
	local_gi.add_child(light)
	local_gi.update_local_data()
	var rid := local_gi.get_local_dynamic_gi_rid()
	var version_before := RenderingServer.local_dynamic_gi_get_data_version(rid)

	light.position = Vector3(0.6, 1.1, -0.4)
	local_gi.update_local_data()
	var version_after := RenderingServer.local_dynamic_gi_get_data_version(rid)
	if version_after != version_before:
		push_error("Dynamic light motion must not request a Local geometry rebuild")
		local_gi.free()
		return 1
	local_gi.free()
	return 0
