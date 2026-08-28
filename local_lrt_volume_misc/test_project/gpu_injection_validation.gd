extends SceneTree

## Validates Local LRT dynamic Injection storage-buffer upload and clearing.

const RESOLUTION := Vector3i(3, 3, 3)
const EPSILON: float = 0.000001


func _initialize() -> void:
	call_deferred("_run_validation")


func _run_validation() -> void:
	var volume: RID = RenderingServer.local_lrt_volume_create()
	if not volume.is_valid():
		_fail("Local LRT requires a RenderingDevice renderer.")
		return

	RenderingServer.local_lrt_volume_set_grid(volume, Vector3(2.0, 2.0, 2.0), RESOLUTION)
	var local_visibility := PackedVector4Array()
	local_visibility.resize(_probe_count())
	var local_transfer := PackedVector4Array()
	local_transfer.resize(_probe_count() * 12)
	RenderingServer.local_lrt_volume_set_static_data(volume, local_visibility, local_transfer)

	var expected := PackedVector4Array()
	expected.resize(_probe_count() * 3)
	for index: int in expected.size():
		expected[index] = Vector4(index + 0.25, index + 0.5, index + 0.75, index + 1.0)
	var no_emission := PackedVector4Array()
	no_emission.resize(expected.size())
	RenderingServer.local_lrt_volume_set_injection(volume, expected, no_emission)
	if not _validate_values("upload", RenderingServer.local_lrt_volume_get_injection(volume), expected):
		RenderingServer.free_rid(volume)
		return

	var cleared := PackedVector4Array()
	cleared.resize(expected.size())
	RenderingServer.local_lrt_volume_set_injection(volume, cleared, cleared)
	if not _validate_values("clear", RenderingServer.local_lrt_volume_get_injection(volume), cleared):
		RenderingServer.free_rid(volume)
		return

	RenderingServer.free_rid(volume)
	print("LOCAL_LRT_GPU_INJECTION_PASS probes=27 values=81")
	quit()


func _validate_values(label: String, actual: PackedVector4Array, expected: PackedVector4Array) -> bool:
	if actual.size() != expected.size():
		_fail("%s size mismatch: %d != %d" % [label, actual.size(), expected.size()])
		return false
	for index: int in actual.size():
		if actual[index].distance_to(expected[index]) > EPSILON:
			_fail("%s mismatch at %d: %s != %s" % [label, index, actual[index], expected[index]])
			return false
	return true


func _probe_count() -> int:
	return RESOLUTION.x * RESOLUTION.y * RESOLUTION.z


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
