extends SceneTree

## GPU regression checks for the Forward+ first-person renderer.
## Run with --rendering-method forward_plus --script <this file>, not --headless.

var _scene := Node3D.new()
var _camera := Camera3D.new()
var _environment := Environment.new()
var _failures: int = 0
var _probe := ShadowProbe.new()
var _output: String = OS.get_cache_dir().path_join("godot-first-person-validation")


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	DirAccess.make_dir_recursive_absolute(_output)
	root.size = Vector2i(640, 480)
	root.add_child(_scene)
	_scene.add_child(_camera)
	_camera.near = 0.005
	_camera.far = 50.0
	_camera.first_person_enabled = true
	_camera.first_person_scale = 0.2
	_camera.make_current()
	var compositor := Compositor.new()
	compositor.compositor_effects = [_probe]
	_camera.compositor = compositor
	var world_environment := WorldEnvironment.new()
	world_environment.environment = _environment
	_environment.background_mode = Environment.BG_COLOR
	_environment.background_color = Color(0.03, 0.03, 0.03)
	_environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	_environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	_scene.add_child(world_environment)

	var receiver: MeshInstance3D = _box(Vector3(0.0, 0.0, -1.0), Vector3(0.9, 0.7, 0.05), Color.MAGENTA, true)
	receiver.first_person_enabled = true
	var world: MeshInstance3D = _box(Vector3(0.8, 0.0, -2.0), Vector3(0.4, 0.7, 0.2), Color.CYAN, true)
	var baseline: Image = await _capture("fov_baseline")
	var baseline_bounds: Rect2i = _magenta_bounds(baseline)
	_check(baseline_bounds.size.x > 100, "first-person geometry is visible")
	_camera.fov = 105.0
	var world_fov: Image = await _capture("world_fov_105")
	_check(_magenta_bounds(world_fov) == baseline_bounds, "world FOV leaves first-person FOV unchanged")
	_check(_difference(baseline, world_fov, Rect2i(460, 0, 180, 480)) > 0.005, "world FOV changes world geometry")
	_camera.first_person_fov = 50.0
	var narrow: Image = await _capture("first_person_fov_50")
	_check(_magenta_bounds(narrow).size.x > baseline_bounds.size.x * 1.4, "independent first-person FOV changes geometry")
	_camera.first_person_fov = 75.0
	_camera.first_person_scale = 0.1
	var scaled: Image = await _capture("scale_01")
	_check(_magenta_bounds(scaled) == baseline_bounds, "camera-centered scaling preserves projected size")

	world.position = Vector3(0.0, 0.0, -0.4)
	(world.mesh as BoxMesh).size = Vector3(4.0, 4.0, 0.1)
	var near_wall: Image = await _capture("near_wall")
	_check(_magenta_bounds(near_wall) == baseline_bounds, "scaled geometry remains in front of a nearby wall")
	_camera.first_person_enabled = false
	var disabled: Image = await _capture("camera_disabled")
	_check(_magenta_bounds(disabled).size == Vector2i.ZERO, "disabled camera restores world-space occlusion")
	_camera.first_person_enabled = true
	world.queue_free()
	_camera.fov = 75.0

	var material := receiver.material_override as StandardMaterial3D
	material.shading_mode = BaseMaterial3D.SHADING_MODE_PER_PIXEL
	material.albedo_color = Color(0.75, 0.75, 0.75)
	material.roughness = 1.0
	var caster: MeshInstance3D = _box(Vector3(0.12, 0.12, -0.8), Vector3(0.22, 0.22, 0.12), Color(0.4, 0.4, 0.4), false)
	caster.first_person_enabled = true
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-25.0, -35.0, 0.0)
	sun.shadow_enabled = true
	sun.light_energy = 2.0
	_scene.add_child(sun)
	await _check_self_shadow("directional", baseline_bounds)
	await _check_optimized_shadow(baseline_bounds)

	sun.hide()
	var omni := OmniLight3D.new()
	omni.position = Vector3(0.15, 0.15, 0.15)
	omni.omni_range = 3.0
	omni.light_energy = 2.0
	omni.shadow_enabled = true
	_scene.add_child(omni)
	await _check_self_shadow("omni", baseline_bounds)
	sun.show()
	await _check_self_shadow("mixed", baseline_bounds)
	_check(_probe.dimensions().z == 2, "directional and omni lights have separate shadow layers")
	sun.hide()
	omni.queue_free()
	var spot := SpotLight3D.new()
	spot.position = Vector3(0.15, 0.15, 0.15)
	spot.spot_range = 3.0
	spot.spot_angle = 70.0
	spot.light_energy = 2.0
	spot.shadow_enabled = true
	_scene.add_child(spot)
	spot.look_at(Vector3(0.0, 0.0, -0.1))
	await _check_self_shadow("spot", baseline_bounds)
	spot.queue_free()
	var area := AreaLight3D.new()
	area.position = Vector3(0.15, 0.15, 0.15)
	area.set_param(Light3D.PARAM_RANGE, 3.0)
	area.light_energy = 2.0
	area.shadow_enabled = true
	_scene.add_child(area)
	area.look_at(Vector3(0.0, 0.0, -0.1))
	await _check_self_shadow("area", baseline_bounds)
	area.queue_free()

	sun.show()
	sun.rotation_degrees = Vector3.ZERO
	ProjectSettings.set_setting("rendering/lights_and_shadows/first_person_shadow/enabled", false)
	var world_lit: Image = await _capture("world_shadow_off")
	var world_caster: MeshInstance3D = _box(Vector3(0.0, 0.0, 0.4), Vector3(2.0, 2.0, 0.3), Color.WHITE, false)
	var world_shadowed: Image = await _capture("world_shadow_on")
	_check(_difference(world_lit, world_shadowed, baseline_bounds) > 0.02, "offscreen world geometry casts shadows onto first-person geometry")
	world_caster.queue_free()

	# Move casters behind the camera; directional shadows still reach the wall.
	var receiver_position: Vector3 = receiver.position
	var caster_position: Vector3 = caster.position
	receiver.position.z = 0.5
	caster.position.z = 0.8
	var wall: MeshInstance3D = _box(Vector3(0.0, 0.0, -2.0), Vector3(5.0, 5.0, 0.2), Color.WHITE, false)
	var no_world_shadow: Image = await _capture("first_person_no_world_shadow")
	receiver.first_person_enabled = false
	caster.first_person_enabled = false
	var ordinary_world_shadow: Image = await _capture("ordinary_world_shadow")
	_check(_difference(no_world_shadow, ordinary_world_shadow, Rect2i(0, 0, 640, 480)) > 0.001, "first-person casters are excluded from world shadow maps")
	receiver.first_person_enabled = true
	caster.first_person_enabled = true
	receiver.position = receiver_position
	caster.position = caster_position
	wall.queue_free()

	sun.hide()
	caster.hide()
	var emitter: MeshInstance3D = _box(Vector3(0.0, 0.0, 0.8), Vector3(3.0, 3.0, 0.4), Color.RED, false)
	var emissive := emitter.material_override as StandardMaterial3D
	emissive.emission_enabled = true
	emissive.emission = Color(1.0, 0.005, 0.001)
	emissive.emission_energy_multiplier = 8.0
	var gi_off: Image = await _capture("gi_off")
	_environment.sdfgi_min_cell_size = 0.1
	_environment.sdfgi_cascades = 1
	_environment.sdfgi_read_sky_light = false
	_environment.sdfgi_enabled = true
	await _frames(120)
	var gi_on: Image = await _capture("gi_on")
	_check(_difference(gi_off, gi_on, baseline_bounds.grow(-10)) > 0.02, "first-person geometry receives SDFGI")
	_environment.sdfgi_enabled = false
	emitter.queue_free()
	sun.show()
	caster.show()
	ProjectSettings.set_setting("rendering/lights_and_shadows/first_person_shadow/enabled", true)
	sun.rotation_degrees = Vector3(-25.0, -35.0, 0.0)
	root.msaa_3d = Viewport.MSAA_4X
	await _check_self_shadow("msaa4", baseline_bounds)
	root.use_taa = true
	await _capture("taa")
	_camera.first_person_enabled = false
	await _capture("disabled_after_taa")
	_camera.first_person_enabled = true
	await _capture("reenabled_after_taa")

	if "--benchmark" in OS.get_cmdline_user_args():
		await _benchmark()
	print("FIRST_PERSON_VALIDATION failures=", _failures, " artifacts=", _output)
	quit(0 if _failures == 0 else 1)


func _box(position: Vector3, size: Vector3, color: Color, unshaded: bool) -> MeshInstance3D:
	var node := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = size
	node.mesh = mesh
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 1.0
	if unshaded:
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	node.material_override = material
	node.position = position
	_scene.add_child(node)
	return node


func _frames(count: int) -> void:
	for i: int in count:
		await process_frame
		await RenderingServer.frame_post_draw


func _capture(label: String) -> Image:
	await _frames(8)
	var image: Image = root.get_texture().get_image()
	image.save_png(_output.path_join(label + ".png"))
	return image


func _magenta_bounds(image: Image) -> Rect2i:
	var bounds := Rect2i()
	for y: int in image.get_height():
		for x: int in image.get_width():
			var color: Color = image.get_pixel(x, y)
			if color.r > 0.6 and color.b > 0.6 and color.g < 0.2:
				var pixel := Rect2i(x, y, 1, 1)
				bounds = pixel if bounds.size == Vector2i.ZERO else bounds.merge(pixel)
	return bounds


func _difference(a: Image, b: Image, region: Rect2i) -> float:
	var total: float = 0.0
	for y: int in range(region.position.y, region.end.y):
		for x: int in range(region.position.x, region.end.x):
			var ca: Color = a.get_pixel(x, y)
			var cb: Color = b.get_pixel(x, y)
			total += absf(ca.r - cb.r) + absf(ca.g - cb.g) + absf(ca.b - cb.b)
	return total / float(maxi(1, region.get_area()) * 3)


func _check_self_shadow(label: String, region: Rect2i) -> void:
	ProjectSettings.set_setting("rendering/lights_and_shadows/first_person_shadow/enabled", false)
	var off: Image = await _capture(label + "_shadow_off")
	ProjectSettings.set_setting("rendering/lights_and_shadows/first_person_shadow/enabled", true)
	var on: Image = await _capture(label + "_shadow_on")
	var difference: float = _difference(off, on, region)
	print(label, " self-shadow difference=", difference)
	_check(difference > 0.001, label + " first-person self-shadow")
	if label == "directional":
		var filtered_pixels: int = 0
		for y: int in range(region.position.y, region.end.y):
			for x: int in range(region.position.x, region.end.x):
				var lit: float = off.get_pixel(x, y).r
				var shadowed: float = on.get_pixel(x, y).r
				if shadowed > lit * 0.1 and shadowed < lit * 0.9:
					filtered_pixels += 1
		print("directional filtered shadow-edge pixels=", filtered_pixels)
		_check(filtered_pixels > 20, "self-shadow edges have subpixel coverage, not binary blocks")


func _check_optimized_shadow(region: Rect2i) -> void:
	var prefix: String = "rendering/lights_and_shadows/first_person_shadow/"
	var half_skipped: Image = await _capture("half_skipped")
	_check(_probe.dimensions() == Vector3i(320, 240, 1), "half resolution traces a 320x240 mask for a 640x480 viewport")
	ProjectSettings.set_setting(prefix + "block_skipping", false)
	var half_exact: Image = await _capture("half_exact")
	_check(_difference(half_skipped, half_exact, region) < 0.00001, "conservative blocks preserve half-resolution shadows")
	ProjectSettings.set_setting(prefix + "half_resolution", false)
	var full_exact: Image = await _capture("full_exact")
	_check(_probe.dimensions() == Vector3i(640, 480, 1), "full resolution setting allocates a full-size mask")
	ProjectSettings.set_setting(prefix + "block_skipping", true)
	var full_skipped: Image = await _capture("full_skipped")
	_check(_difference(full_exact, full_skipped, region) < 0.00001, "conservative blocks preserve full-resolution shadows")
	_check(_difference(full_exact, half_exact, region) < 0.01, "half-resolution reconstruction stays close to full resolution")
	ProjectSettings.set_setting(prefix + "half_resolution", true)
	var restored: Image = await _capture("half_restored")
	_check(_difference(half_skipped, restored, region) < 0.00001, "live resolution switching rebuilds shadow resources")
	root.size = Vector2i(641, 481)
	await _capture("odd_resolution")
	_check(_probe.dimensions() == Vector3i(321, 241, 1), "odd viewport dimensions round shadow size up")
	root.size = Vector2i(640, 480)
	ProjectSettings.set_setting(prefix + "distance", 0.0)
	var zero_distance: Image = await _capture("zero_distance")
	ProjectSettings.set_setting(prefix + "enabled", false)
	var disabled: Image = await _capture("setting_disabled")
	_check(_difference(zero_distance, disabled, region) < 0.00001, "zero distance and disabled settings both stop self shadows")
	ProjectSettings.set_setting(prefix + "distance", 1.0)
	ProjectSettings.set_setting(prefix + "enabled", true)


func _benchmark() -> void:
	var prefix: String = "rendering/lights_and_shadows/first_person_shadow/"
	root.size = Vector2i(1920, 1080)
	root.msaa_3d = Viewport.MSAA_DISABLED
	root.use_taa = false
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	RenderingServer.viewport_set_measure_render_time(root.get_viewport_rid(), true)
	for half: bool in [false, true]:
		for skipping: bool in [false, true]:
			ProjectSettings.set_setting(prefix + "half_resolution", half)
			ProjectSettings.set_setting(prefix + "block_skipping", skipping)
			await _frames(90)
			var times: Array[float] = []
			for i: int in range(120):
				await _frames(1)
				times.append(RenderingServer.viewport_get_measured_render_time_gpu(root.get_viewport_rid()))
			times.sort()
			print("FP_GPU_1080P half=", half, " blocks=", skipping, " median_ms=", times[60])
	ProjectSettings.set_setting(prefix + "half_resolution", true)
	ProjectSettings.set_setting(prefix + "block_skipping", true)


func _check(condition: bool, description: String) -> void:
	print("PASS " if condition else "FAIL ", description)
	if not condition:
		_failures += 1


## Observes actual GPU allocation sizes after the opaque pass.
class ShadowProbe extends CompositorEffect:
	var _size := Vector3i.ZERO
	var _mutex := Mutex.new()


	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE


	func _render_callback(_type: int, render_data: RenderData) -> void:
		var buffers: RenderSceneBuffersRD = render_data.get_render_scene_buffers() as RenderSceneBuffersRD
		var size := Vector3i.ZERO
		if buffers.has_texture(&"first_person_shadow", &"masks"):
			var format: RDTextureFormat = buffers.get_texture_format(&"first_person_shadow", &"masks")
			size = Vector3i(format.width, format.height, format.array_layers)
		_mutex.lock()
		_size = size
		_mutex.unlock()


	## Reads the most recent mask dimensions across the rendering-thread boundary.
	func dimensions() -> Vector3i:
		_mutex.lock()
		var size: Vector3i = _size
		_mutex.unlock()
		return size
