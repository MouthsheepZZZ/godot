extends SceneTree

## Automated Task 2 checks for Local HDDAGI runtime plumbing.
##
## Visual bounce/bleeding still requires a human pass on 01/02.


func _init() -> void:
	var failed := 0
	failed += _test_enable_disable_create_delete()
	failed += _test_root_motion_does_not_bump_data_version()
	failed += _test_bounds_change_bumps_data_version()

	if failed == 0:
		print("TASK2_RUNTIME_TEST PASS")
		quit(0)
	else:
		push_error("TASK2_RUNTIME_TEST FAIL count=%d" % failed)
		quit(1)


func _make_box(position: Vector3, size: Vector3) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = position
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	return instance


func _test_enable_disable_create_delete() -> int:
	for _i in range(3):
		var local_gi := LocalDynamicGI3D.new()
		var rid := local_gi.get_local_dynamic_gi_rid()
		if not rid.is_valid():
			push_error("RID was invalid after create")
			local_gi.free()
			return 1
		local_gi.enabled = false
		local_gi.enabled = true
		local_gi.free()
	return 0


func _test_root_motion_does_not_bump_data_version() -> int:
	var local_gi := LocalDynamicGI3D.new()
	local_gi.add_child(_make_box(Vector3.ZERO, Vector3(2, 2, 2)))
	local_gi.update_local_data()
	var rid := local_gi.get_local_dynamic_gi_rid()
	var version_before := RenderingServer.local_dynamic_gi_get_data_version(rid)
	var count_before := RenderingServer.local_dynamic_gi_get_voxelization_count(rid)

	for i in range(8):
		local_gi.position = Vector3(float(i), 0.25 * float(i), -0.5 * float(i))
		local_gi.rotation = Vector3(0.0, 0.2 * float(i), 0.0)
		local_gi.update_local_data()

	var version_after := RenderingServer.local_dynamic_gi_get_data_version(rid)
	var count_after := RenderingServer.local_dynamic_gi_get_voxelization_count(rid)
	if version_after != version_before:
		push_error("Root motion must not bump Local data version (%d -> %d)" % [version_before, version_after])
		local_gi.free()
		return 1
	if count_after != count_before:
		push_error("Root motion must not increment voxelization count (%d -> %d)" % [count_before, count_after])
		local_gi.free()
		return 1

	local_gi.free()
	return 0


func _test_bounds_change_bumps_data_version() -> int:
	var local_gi := LocalDynamicGI3D.new()
	local_gi.add_child(_make_box(Vector3.ZERO, Vector3(2, 2, 2)))
	local_gi.update_local_data()
	var rid := local_gi.get_local_dynamic_gi_rid()
	var version_before := RenderingServer.local_dynamic_gi_get_data_version(rid)

	local_gi.extend = Vector3(1.0, 1.0, 1.0)
	var version_after := RenderingServer.local_dynamic_gi_get_data_version(rid)
	if version_after <= version_before:
		push_error("Bounds/extend change must bump Local data version")
		local_gi.free()
		return 1

	local_gi.free()
	return 0
