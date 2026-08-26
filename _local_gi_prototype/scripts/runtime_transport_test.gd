extends SceneTree

## Compares renderer-RD LocalGI transport against the retained CPU correctness oracle.

const BASELINE_SCENE := "res://scenes/a_cornell_baseline.tscn"
const THIN_WALL_SCENE := "res://scenes/c_cornell_thin_wall.tscn"
const COLOR_TOLERANCE := 0.004
const DISTANCE_TOLERANCE := 0.002

var _failed: int = 0


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	_test_baseline_transport()
	_test_probe_classification()
	if _failed > 0:
		push_error("Renderer RD LocalGI test failed with %d errors." % _failed)
		quit(1)
		return
	print("Renderer RD LocalGI test passed.")
	quit(0)


func _instantiate_scene(path: String) -> Node:
	var packed: PackedScene = load(path) as PackedScene
	if packed == null:
		_fail("Could not load %s." % path)
		return null
	var instance: Node = packed.instantiate()
	root.add_child(instance)
	return instance


func _prepare_volume(instance: Node) -> LocalGIVolume3D:
	var volume: LocalGIVolume3D = instance.find_child("LocalGIVolume3D", true, false) as LocalGIVolume3D
	if volume == null:
		_fail("Scene has no LocalGIVolume3D.")
		return null
	volume.multi_bounce_enabled = false
	volume.temporal_hysteresis = 0.0
	volume.update_fraction = 1.0
	volume.bake(instance)
	volume.update_dynamic(instance)
	volume.build_probes()
	return volume


func _test_baseline_transport() -> void:
	var instance: Node = _instantiate_scene(BASELINE_SCENE)
	if instance == null:
		return
	var volume: LocalGIVolume3D = _prepare_volume(instance)
	if volume == null:
		instance.free()
		return

	if not volume.compute_one_bounce(instance):
		_fail("CPU reference transport failed.")
		instance.free()
		return
	var cpu_irradiance: PackedColorArray = volume.get_probe_irradiances()
	var cpu_active: PackedByteArray = volume.get_probe_active_states()
	var cpu_shading: Color = volume.sample_indirect_irradiance(Vector3(0.8, 0.2, -0.6), Vector3.UP)
	var cpu_means := PackedFloat32Array()
	var cpu_seconds := PackedFloat32Array()
	for probe_index: int in range(volume.get_probe_count()):
		for ray_index: int in range(volume.rays_per_probe):
			cpu_means.append(volume.get_probe_ray_distance_mean(probe_index, ray_index))
			cpu_seconds.append(volume.get_probe_ray_distance_second_moment(probe_index, ray_index))

	volume.bake(instance)
	volume.update_dynamic(instance)
	volume.build_probes()
	if not volume.compute_runtime_transport(instance):
		_fail("Renderer RD transport failed.")
		instance.free()
		return
	_compare_colors(cpu_irradiance, volume.get_probe_irradiances(), "probe irradiance")
	if cpu_active != volume.get_probe_active_states():
		_fail("Renderer RD Probe Classification differs from CPU reference.")
	var runtime_shading: Color = volume.sample_indirect_irradiance(Vector3(0.8, 0.2, -0.6), Vector3.UP)
	if _color_distance(cpu_shading, runtime_shading) > COLOR_TOLERANCE:
		_fail("Renderer RD visibility shading differs from CPU reference.")

	var moment_index: int = 0
	for probe_index: int in range(volume.get_probe_count()):
		for ray_index: int in range(volume.rays_per_probe):
			var runtime_mean: float = volume.get_probe_ray_distance_mean(probe_index, ray_index)
			var runtime_second: float = volume.get_probe_ray_distance_second_moment(probe_index, ray_index)
			if absf(runtime_mean - cpu_means[moment_index]) > DISTANCE_TOLERANCE:
				_fail("Renderer RD distance mean differs at ray %d." % moment_index)
				break
			if absf(runtime_second - cpu_seconds[moment_index]) > DISTANCE_TOLERANCE * 4.0:
				_fail("Renderer RD second moment differs at ray %d." % moment_index)
				break
			moment_index += 1

	var direct_mean: Color = volume.get_mean_probe_irradiance()
	var light: OmniLight3D = instance.find_child("CeilingLight", true, false) as OmniLight3D
	if light == null:
		_fail("Baseline scene has no CeilingLight.")
	else:
		light.light_energy *= 2.0
		if not volume.compute_runtime_transport(instance):
			_fail("Renderer RD doubled-light transport failed.")
		elif absf(volume.get_mean_probe_irradiance().get_luminance() - direct_mean.get_luminance() * 2.0) > COLOR_TOLERANCE:
			_fail("Renderer RD direct response is not linear.")

	volume.reset_temporal_history()
	volume.temporal_hysteresis = 0.9
	if not volume.compute_runtime_transport(instance):
		_fail("Renderer RD temporal update failed.")
	elif volume.get_mean_probe_irradiance().get_luminance() <= 0.0:
		_fail("Renderer RD temporal estimate did not converge from zero.")

	volume.bake(instance)
	volume.update_dynamic(instance)
	volume.build_probes()
	volume.multi_bounce_enabled = true
	volume.temporal_hysteresis = 0.0
	for _iteration: int in range(2):
		if not volume.compute_runtime_transport(instance):
			_fail("Renderer RD multi-bounce update failed.")
			break
	var runtime_first_multi_bounce: PackedColorArray = volume.get_probe_irradiances()
	var previous_luminance: float = volume.get_mean_probe_irradiance().get_luminance()
	var first_increment: float = -1.0
	var last_increment: float = 0.0
	for iteration: int in range(7):
		if not volume.compute_runtime_transport(instance):
			_fail("Renderer RD multi-bounce convergence failed.")
			break
		var current_luminance: float = volume.get_mean_probe_irradiance().get_luminance()
		last_increment = current_luminance - previous_luminance
		if iteration == 0:
			first_increment = last_increment
		previous_luminance = current_luminance
	if not volume.probe_irradiance_is_finite():
		_fail("Renderer RD multi-bounce produced non-finite values.")
	elif first_increment >= 0.0 and last_increment > first_increment:
		_fail("Renderer RD multi-bounce increments did not converge.")

	volume.bake(instance)
	volume.update_dynamic(instance)
	volume.build_probes()
	volume.multi_bounce_enabled = true
	volume.temporal_hysteresis = 0.0
	for _iteration: int in range(2):
		if not volume.compute_one_bounce(instance) or not volume.update_temporal():
			_fail("CPU multi-bounce oracle failed.")
			break
	_compare_colors(volume.get_probe_irradiances(), runtime_first_multi_bounce, "first multi-bounce estimate", 0.06, true)
	instance.free()


func _test_probe_classification() -> void:
	var instance: Node = _instantiate_scene(THIN_WALL_SCENE)
	if instance == null:
		return
	var volume: LocalGIVolume3D = _prepare_volume(instance)
	if volume == null:
		instance.free()
		return
	volume.classify_probes()
	var cpu_active: PackedByteArray = volume.get_probe_active_states()
	if not volume.compute_runtime_transport(instance):
		_fail("Thin-wall renderer RD transport failed.")
	elif cpu_active != volume.get_probe_active_states():
		_fail("Thin-wall renderer RD classification differs from CPU reference.")
	elif volume.get_active_probe_count() <= 0 or volume.get_active_probe_count() >= volume.get_probe_count():
		_fail("Thin-wall renderer RD classification lacks active/inactive probes.")
	instance.free()


func _compare_colors(reference: PackedColorArray, runtime: PackedColorArray, label: String, tolerance: float = COLOR_TOLERANCE, relative: bool = false) -> void:
	if reference.size() != runtime.size():
		_fail("Renderer RD %s count differs from CPU reference." % label)
		return
	for index: int in range(reference.size()):
		var difference: float = _color_distance(reference[index], runtime[index])
		var allowed: float = tolerance
		if relative:
			allowed *= maxf(Vector3(reference[index].r, reference[index].g, reference[index].b).length(), 1e-6)
		if difference > allowed:
			_fail("Renderer RD %s differs at index %d: CPU=%s GPU=%s." % [label, index, reference[index], runtime[index]])
			return


func _color_distance(first: Color, second: Color) -> float:
	return Vector3(first.r, first.g, first.b).distance_to(Vector3(second.r, second.g, second.b))


func _fail(message: String) -> void:
	push_error(message)
	_failed += 1
