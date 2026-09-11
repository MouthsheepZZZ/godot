/**************************************************************************/
/*  lrt_volume.cpp                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).  */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                   */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "lrt_volume.h"

#include "lrt_cache.h"
#include "lrt_inject.glsl.gen.h"
#include "lrt_propagate.glsl.gen.h"

#include "core/io/image.h"
#include "core/os/os.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

namespace {

constexpr int MAX_LIGHT_COUNT = 8;
constexpr int MAX_BOX_COUNT = 16;
constexpr int WORKGROUP_SIZE = 64;
// Mirrors the prototype's bakeBoxSDF() call, which always uses the default 24 for boxes.
constexpr int BOX_SDF_RESOLUTION = 24;

// std140 layout shared by both compute shaders.
struct ParamsData {
	int32_t grid_size[4] = { 0, 0, 0, 0 };
	float grid_min[4] = { 0, 0, 0, 0 };
	int32_t counts[4] = { 0, 0, 0, 0 };
	float flags[4] = { 0, 0, 0, 0 }; // x multi bounce, y SH visibility, z color SDF, w unused
	float sky_color[4] = { 0, 0, 0, 0 };
	float light_position[MAX_LIGHT_COUNT][4] = {};
	float light_direction[MAX_LIGHT_COUNT][4] = {};
	float light_color[MAX_LIGHT_COUNT][4] = {};
	float light_data[MAX_LIGHT_COUNT][4] = {};
	float light_spot[MAX_LIGHT_COUNT][4] = {};
	float box_min[MAX_BOX_COUNT][4] = {};
	float box_max[MAX_BOX_COUNT][4] = {};
	float box_color[MAX_BOX_COUNT][4] = {};
};

String direction_initializer() {
	const lrt::Direction *directions = lrt::directions();
	String text;
	for (int i = 0; i < lrt::DIRECTION_COUNT; i++) {
		if (i > 0) {
			text += ", ";
		}
		text += vformat("ivec3(%d, %d, %d)", directions[i].offset[0], directions[i].offset[1], directions[i].offset[2]);
	}
	return text;
}

PackedByteArray bytes_of(const void *p_data, size_t p_size) {
	PackedByteArray bytes;
	bytes.resize(int64_t(p_size));
	memcpy(bytes.ptrw(), p_data, p_size);
	return bytes;
}

} // namespace

LRTVolume::LRTVolume() {
}

LRTVolume::~LRTVolume() {
	_free_gpu_resources();
	if (device) {
		memdelete(device);
		device = nullptr;
	}
}

void LRTVolume::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "spacing"), &LRTVolume::configure);
	ClassDB::bind_method(D_METHOD("configure_with_bounds", "spacing", "bounds_min", "bounds_max"), &LRTVolume::configure_with_bounds);
	ClassDB::bind_method(D_METHOD("configure_sized", "spacing", "min", "size"), &LRTVolume::configure_sized);
	ClassDB::bind_method(D_METHOD("configure_sized_with_bounds", "spacing", "min", "size", "bounds_min", "bounds_max"), &LRTVolume::configure_sized_with_bounds);
	ClassDB::bind_method(D_METHOD("set_boxes", "boxes"), &LRTVolume::set_boxes);
	ClassDB::bind_method(D_METHOD("set_meshes", "meshes"), &LRTVolume::set_meshes);
	ClassDB::bind_method(D_METHOD("set_mesh_sdf_resolution", "resolution"), &LRTVolume::set_mesh_sdf_resolution);
	ClassDB::bind_method(D_METHOD("set_lights", "lights"), &LRTVolume::set_lights);
	ClassDB::bind_method(D_METHOD("set_sky", "sky"), &LRTVolume::set_sky);
	ClassDB::bind_method(D_METHOD("read_environment_radiance", "environment", "size"), &LRTVolume::read_environment_radiance);
	ClassDB::bind_method(D_METHOD("set_multi_bounce", "enabled"), &LRTVolume::set_multi_bounce);
	ClassDB::bind_method(D_METHOD("set_sh_visibility", "enabled"), &LRTVolume::set_sh_visibility);
	ClassDB::bind_method(D_METHOD("build_local_field", "backend"), &LRTVolume::build_local_field);
	ClassDB::bind_method(D_METHOD("bake_local_field", "backend"), &LRTVolume::bake_local_field);
	ClassDB::bind_method(D_METHOD("apply_local_field"), &LRTVolume::apply_local_field);
	ClassDB::bind_method(D_METHOD("request_cancel"), &LRTVolume::request_cancel);
	ClassDB::bind_method(D_METHOD("clear_cancel"), &LRTVolume::clear_cancel);
	ClassDB::bind_method(D_METHOD("is_cancel_requested"), &LRTVolume::is_cancel_requested);
	ClassDB::bind_method(D_METHOD("has_local_field"), &LRTVolume::has_local_field);
	ClassDB::bind_method(D_METHOD("inject"), &LRTVolume::inject);
	ClassDB::bind_method(D_METHOD("step", "iterations"), &LRTVolume::step);
	ClassDB::bind_method(D_METHOD("reset"), &LRTVolume::reset);
	ClassDB::bind_method(D_METHOD("get_iteration"), &LRTVolume::get_iteration);
	ClassDB::bind_method(D_METHOD("get_grid"), &LRTVolume::get_grid);
	ClassDB::bind_method(D_METHOD("refresh_display"), &LRTVolume::refresh_display);
	ClassDB::bind_method(D_METHOD("get_texture", "name"), &LRTVolume::get_texture);
	ClassDB::bind_method(D_METHOD("read_field", "name"), &LRTVolume::read_field);
	ClassDB::bind_method(D_METHOD("read_links"), &LRTVolume::read_links);
	ClassDB::bind_method(D_METHOD("get_mesh_bvh"), &LRTVolume::get_mesh_bvh);
	ClassDB::bind_method(D_METHOD("get_stats"), &LRTVolume::get_stats);
}

void LRTVolume::configure(double p_spacing) {
	grid = lrt::make_grid(p_spacing);
	configured = true;
	has_local = false;
	has_staged = false;
}

void LRTVolume::configure_with_bounds(double p_spacing, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max) {
	grid = lrt::make_grid(p_spacing,
			lrt::Vec3(p_bounds_min.x, p_bounds_min.y, p_bounds_min.z),
			lrt::Vec3(p_bounds_max.x, p_bounds_max.y, p_bounds_max.z));
	configured = true;
	has_local = false;
	has_staged = false;
}

// LRTVolume3D owns the grid region (its own size around its own position); the lattice rule
// is the prototype's, so a (6, 4, 6) volume at (0, 1.5, 0) reproduces the fixed
// [-3,-0.5,-3]..[3,3.5,3] lab region exactly.
void LRTVolume::configure_sized(double p_spacing, const Vector3 &p_min, const Vector3 &p_size) {
	grid = lrt::make_grid_sized(p_spacing,
			lrt::Vec3(p_min.x, p_min.y, p_min.z),
			lrt::Vec3(p_size.x, p_size.y, p_size.z));
	configured = true;
	has_local = false;
	has_staged = false;
}

void LRTVolume::configure_sized_with_bounds(double p_spacing, const Vector3 &p_min, const Vector3 &p_size, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max) {
	grid = lrt::make_grid_sized(p_spacing,
			lrt::Vec3(p_min.x, p_min.y, p_min.z),
			lrt::Vec3(p_size.x, p_size.y, p_size.z),
			lrt::Vec3(p_bounds_min.x, p_bounds_min.y, p_bounds_min.z),
			lrt::Vec3(p_bounds_max.x, p_bounds_max.y, p_bounds_max.z));
	configured = true;
	has_local = false;
	has_staged = false;
}

// Box inputs may arrive as doubles: the prototype's constants are doubles and its Color
// SDF quantizes both geometry and color, where a float32 round trip (0.7 -> 0.69999999,
// -1.4 -> -1.39999998) can flip an exact .5 tie or a gradient tie and change the field.
static lrt::Vec3 dictionary_vec3(const Dictionary &p_entry, const char *p_key) {
	const Variant value = p_entry.get(p_key, Vector3());
	if (value.get_type() == Variant::PACKED_FLOAT64_ARRAY) {
		const PackedFloat64Array array = value;
		if (array.size() >= 3) {
			return lrt::Vec3(array[0], array[1], array[2]);
		}
		return lrt::Vec3();
	}
	const Vector3 vector = value;
	return lrt::Vec3(vector.x, vector.y, vector.z);
}

void LRTVolume::set_boxes(const Array &p_boxes) {
	std::vector<BoxInstance> instances;
	instances.reserve(size_t(p_boxes.size()));
	for (int i = 0; i < p_boxes.size(); i++) {
		const Dictionary entry = p_boxes[i];
		BoxInstance box;
		box.world_min = dictionary_vec3(entry, "min");
		box.world_max = dictionary_vec3(entry, "max");
		box.color = dictionary_vec3(entry, "color");
		// The dictionary form is the prototype's world axis-aligned box. Its SDF path is
		// the same box baked in its own frame and placed at the box centre.
		box.local_extent = box.world_max - box.world_min;
		box.transform.origin = (box.world_max + box.world_min) * 0.5;
		instances.push_back(box);
	}
	set_box_instances(instances);
}

// One entry per imported mesh volume: flat triangle soup with no transform. The prototype
// bakes one Color SDF per glTF mesh instance, so the caller supplies that grouping.
void LRTVolume::set_meshes(const Array &p_meshes) {
	std::vector<MeshInstance> instances;
	instances.reserve(size_t(p_meshes.size()));
	for (int volume = 0; volume < p_meshes.size(); volume++) {
		const Dictionary entry = p_meshes[volume];
		const PackedVector3Array vertices = entry.get("vertices", PackedVector3Array());
		PackedVector3Array colors = entry.get("colors", PackedVector3Array());
		if (vertices.size() < 3) {
			continue;
		}
		if (colors.size() != vertices.size()) {
			colors.resize(vertices.size());
			for (int i = 0; i < colors.size(); i++) {
				colors.set(i, Vector3(1, 1, 1));
			}
		}
		MeshInstance instance;
		const int count = vertices.size() / 3;
		instance.transform = lrt::PrimitiveTransform();
		instance.triangles.reserve(size_t(count));
		for (int i = 0; i < count; i++) {
			lrt::MeshTriangle triangle;
			for (int v = 0; v < 3; v++) {
				const Vector3 position = vertices[i * 3 + v];
				const Vector3 color = colors[i * 3 + v];
				triangle.position[v] = lrt::Vec3(position.x, position.y, position.z);
				triangle.color[v] = lrt::Vec3(color.x, color.y, color.z);
			}
			instance.triangles.push_back(triangle);
		}
		instances.push_back(std::move(instance));
	}
	set_mesh_instances(instances);
}

void LRTVolume::set_box_instances(const std::vector<BoxInstance> &p_boxes) {
	box_instances = p_boxes;
	has_local = false;
	has_staged = false;
}

void LRTVolume::set_mesh_instances(const std::vector<MeshInstance> &p_meshes) {
	mesh_instances = p_meshes;
	has_local = false;
	has_staged = false;
}

void LRTVolume::request_cancel() {
	cancel_flag.store(true);
}

void LRTVolume::clear_cancel() {
	cancel_flag.store(false);
}

bool LRTVolume::is_cancel_requested() const {
	return cancel_flag.load();
}

bool LRTVolume::has_local_field() const {
	return has_local;
}

void LRTVolume::set_mesh_sdf_resolution(int p_resolution) {
	mesh_sdf_resolution = MAX(8, p_resolution);
	has_local = false;
}

void LRTVolume::set_lights(const Array &p_lights) {
	lights.clear();
	for (int i = 0; i < p_lights.size() && i < MAX_LIGHT_COUNT; i++) {
		const Dictionary entry = p_lights[i];
		Light light;
		light.type = entry.get("type", 0);
		light.enabled = entry.get("enabled", true);
		light.casts_shadow = entry.get("casts_shadow", true);
		light.position = entry.get("position", Vector3());
		light.direction = entry.get("direction", Vector3(0, -1, 0));
		light.color = entry.get("color", Vector3(1, 1, 1));
		light.intensity = entry.get("intensity", 0.0);
		light.range = entry.get("range", 1.0);
		light.attenuation = entry.get("attenuation", 1.0);
		light.spot_angle_deg = entry.get("spot_angle_deg", 45.0);
		light.spot_attenuation = entry.get("spot_attenuation", 1.0);
		lights.push_back(light);
	}
}

void LRTVolume::set_sky(const Vector3 &p_sky) {
	sky = p_sky;
}

// The engine's own environment readout for ambient and sky lighting
// (RendererSceneRenderRD::environment_bake_panorama, also used by the RS bindings). The
// prototype models the environment as one uniform radiance, so the panorama is averaged
// over the sphere with the equirectangular solid-angle weight, which makes the result
// independent of how the panorama is oriented. A background color that is not also the
// ambient source stays a pure backdrop and contributes nothing.
Vector3 LRTVolume::read_environment_radiance(const Ref<Environment> &p_environment, const Vector2i &p_size) {
	if (p_environment.is_null()) {
		return Vector3();
	}
	const Environment::BGMode background = p_environment->get_background();
	const Environment::AmbientSource ambient = p_environment->get_ambient_source();
	// Only the sources that actually light geometry in the engine count: the sky as a
	// background or ambient, the ambient color, and a background color used as ambient.
	const bool uses_sky = p_environment->get_sky().is_valid() &&
			(background == Environment::BG_SKY || ambient == Environment::AMBIENT_SOURCE_SKY);
	const bool uses_ambient = ambient == Environment::AMBIENT_SOURCE_COLOR ||
			(ambient == Environment::AMBIENT_SOURCE_BG && background != Environment::BG_SKY);
	if (!uses_sky && !uses_ambient) {
		return Vector3();
	}
	const Size2i size(MAX(1, p_size.x), MAX(1, p_size.y));
	const Ref<Image> panorama = RS::get_singleton()->environment_bake_panorama(p_environment->get_rid(), false, size);
	if (panorama.is_null()) {
		return Vector3();
	}
	double weight_sum = 0.0;
	double sums[3] = { 0.0, 0.0, 0.0 };
	for (int y = 0; y < size.y; y++) {
		const double weight = Math::sin(Math::PI * (y + 0.5) / size.y);
		weight_sum += weight * size.x;
		for (int x = 0; x < size.x; x++) {
			const Color texel = panorama->get_pixel(x, y);
			sums[0] += texel.r * weight;
			sums[1] += texel.g * weight;
			sums[2] += texel.b * weight;
		}
	}
	if (weight_sum <= 0.0) {
		return Vector3();
	}
	return Vector3(sums[0] / weight_sum, sums[1] / weight_sum, sums[2] / weight_sum);
}

void LRTVolume::set_multi_bounce(bool p_enabled) {
	multi_bounce = p_enabled;
}

void LRTVolume::set_sh_visibility(bool p_enabled) {
	sh_visibility = p_enabled;
}

Error LRTVolume::_ensure_device() {
	if (device) {
		return OK;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, ERR_UNAVAILABLE);
	ERR_FAIL_NULL_V(rendering_server->get_rendering_device(), ERR_UNAVAILABLE);
	device = rendering_server->create_local_rendering_device();
	ERR_FAIL_NULL_V(device, ERR_CANT_CREATE);
	return OK;
}

Error LRTVolume::_create_shaders() {
	if (shader_inject.is_valid()) {
		return OK;
	}
	const String offsets = direction_initializer();
	const String sources[2] = {
		String(lrt_inject_shader_glsl).replace("%LRT_DIRECTIONS%", offsets),
		String(lrt_propagate_shader_glsl).replace("%LRT_DIRECTIONS%", offsets),
	};
	RID shaders[2] = { shader_inject, shader_propagate };
	for (int i = 0; i < 2; i++) {
		Ref<RDShaderFile> shader_file;
		shader_file.instantiate();
		const Error parse_error = shader_file->parse_versions_from_text(sources[i]);
		if (parse_error != OK) {
			shader_file->print_errors(i == 0 ? "LRT injection shader" : "LRT propagation shader");
			return parse_error;
		}
		shaders[i] = device->shader_create_from_spirv(shader_file->get_spirv_stages());
		ERR_FAIL_COND_V(shaders[i].is_null(), ERR_CANT_CREATE);
	}
	shader_inject = shaders[0];
	shader_propagate = shaders[1];
	pipeline_inject = device->compute_pipeline_create(shader_inject);
	pipeline_propagate = device->compute_pipeline_create(shader_propagate);
	ERR_FAIL_COND_V(pipeline_inject.is_null() || pipeline_propagate.is_null(), ERR_CANT_CREATE);
	return OK;
}

Error LRTVolume::_create_buffers() {
	const int count = grid.count;
	params_buffer = device->uniform_buffer_create(sizeof(ParamsData));
	material_buffer = device->storage_buffer_create(count * 4 * sizeof(float));
	links_buffer = device->storage_buffer_create(count * sizeof(uint32_t));
	matrix_buffer = device->storage_buffer_create(count * 48 * sizeof(float));
	local_visibility_buffer = device->storage_buffer_create(count * 4 * sizeof(float));
	const size_t receiver_bytes = MAX(size_t(16), local.receivers.size() * sizeof(float));
	receiver_buffer = device->storage_buffer_create(uint32_t(receiver_bytes));
	// Display-side mesh BVH for the injection's occlusion test (16 bytes when unused).
	const std::vector<float> node_data = lrt::mesh_node_data(display_mesh);
	const std::vector<float> triangle_data = lrt::mesh_triangle_data(display_mesh);
	const std::vector<float> material_data = lrt::mesh_material_data();
	mesh_node_buffer = device->storage_buffer_create(MAX(uint32_t(16), uint32_t(node_data.size() * sizeof(float))));
	mesh_triangle_buffer = device->storage_buffer_create(MAX(uint32_t(16), uint32_t(triangle_data.size() * sizeof(float))));
	mesh_material_buffer = device->storage_buffer_create(MAX(uint32_t(16), uint32_t(material_data.size() * sizeof(float))));
	for (int i = 0; i < 3; i++) {
		source_buffers[i] = device->storage_buffer_create(count * 4 * sizeof(float));
	}
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			radiance_buffers[buffer][channel] = device->storage_buffer_create(count * 4 * sizeof(float));
		}
		visibility_buffers[buffer] = device->storage_buffer_create(count * 4 * sizeof(float));
	}
	ERR_FAIL_COND_V(params_buffer.is_null() || material_buffer.is_null() || links_buffer.is_null() ||
					matrix_buffer.is_null() || local_visibility_buffer.is_null() || receiver_buffer.is_null() ||
					mesh_node_buffer.is_null() || mesh_triangle_buffer.is_null() || mesh_material_buffer.is_null(),
			ERR_CANT_CREATE);
	if (!node_data.empty()) {
		device->buffer_update(mesh_node_buffer, 0, node_data.size() * sizeof(float), node_data.data());
		device->buffer_update(mesh_triangle_buffer, 0, triangle_data.size() * sizeof(float), triangle_data.data());
		device->buffer_update(mesh_material_buffer, 0, material_data.size() * sizeof(float), material_data.data());
	}
	return OK;
}

Error LRTVolume::_create_uniform_sets() {
	auto make_uniform = [](RD::UniformType p_type, uint32_t p_binding, RID p_id) {
		RD::Uniform uniform;
		uniform.uniform_type = p_type;
		uniform.binding = p_binding;
		uniform.append_id(p_id);
		return uniform;
	};
	{
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, material_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, links_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, receiver_buffer));
		for (int i = 0; i < 3; i++) {
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + i, source_buffers[i]));
		}
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, mesh_node_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, mesh_triangle_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, mesh_material_buffer));
		uniform_set_inject = device->uniform_set_create(uniforms, shader_inject, 0);
		ERR_FAIL_COND_V(uniform_set_inject.is_null(), ERR_CANT_CREATE);
	}
	for (int buffer = 0; buffer < 2; buffer++) {
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, material_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, links_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, matrix_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, local_visibility_buffer));
		for (int i = 0; i < 3; i++) {
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + i, source_buffers[i]));
		}
		for (int i = 0; i < 3; i++) {
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9 + i, radiance_buffers[buffer][i]));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13 + i, radiance_buffers[1 - buffer][i]));
		}
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, visibility_buffers[buffer]));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, visibility_buffers[1 - buffer]));
		uniform_set_propagate[buffer] = device->uniform_set_create(uniforms, shader_propagate, 0);
		ERR_FAIL_COND_V(uniform_set_propagate[buffer].is_null(), ERR_CANT_CREATE);
	}
	return OK;
}

void LRTVolume::_free_gpu_resources() {
	if (!device) {
		return;
	}
	RID sets[3] = { uniform_set_inject, uniform_set_propagate[0], uniform_set_propagate[1] };
	for (RID &set : sets) {
		if (set.is_valid()) {
			device->free_rid(set);
			set = RID();
		}
	}
	uniform_set_inject = RID();
	uniform_set_propagate[0] = RID();
	uniform_set_propagate[1] = RID();

	RID buffers[64];
	int buffer_count = 0;
	buffers[buffer_count++] = params_buffer;
	buffers[buffer_count++] = material_buffer;
	buffers[buffer_count++] = links_buffer;
	buffers[buffer_count++] = matrix_buffer;
	buffers[buffer_count++] = local_visibility_buffer;
	buffers[buffer_count++] = receiver_buffer;
	buffers[buffer_count++] = mesh_node_buffer;
	buffers[buffer_count++] = mesh_triangle_buffer;
	buffers[buffer_count++] = mesh_material_buffer;
	params_buffer = RID();
	material_buffer = RID();
	links_buffer = RID();
	matrix_buffer = RID();
	local_visibility_buffer = RID();
	receiver_buffer = RID();
	mesh_node_buffer = RID();
	mesh_triangle_buffer = RID();
	mesh_material_buffer = RID();
	for (int i = 0; i < 3; i++) {
		buffers[buffer_count++] = source_buffers[i];
		source_buffers[i] = RID();
	}
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			buffers[buffer_count++] = radiance_buffers[buffer][channel];
			radiance_buffers[buffer][channel] = RID();
		}
		buffers[buffer_count++] = visibility_buffers[buffer];
		visibility_buffers[buffer] = RID();
	}
	for (int i = 0; i < buffer_count; i++) {
		if (buffers[i].is_valid()) {
			device->free_rid(buffers[i]);
		}
	}
	RID pipelines[2] = { pipeline_inject, pipeline_propagate };
	pipeline_inject = RID();
	pipeline_propagate = RID();
	for (const RID &pipeline : pipelines) {
		if (pipeline.is_valid()) {
			device->free_rid(pipeline);
		}
	}
	RID shaders[2] = { shader_inject, shader_propagate };
	shader_inject = RID();
	shader_propagate = RID();
	for (const RID &shader : shaders) {
		if (shader.is_valid()) {
			device->free_rid(shader);
		}
	}
}

bool LRTVolume::_upload_params() {
	ParamsData params;
	params.grid_size[0] = grid.size[0];
	params.grid_size[1] = grid.size[1];
	params.grid_size[2] = grid.size[2];
	params.grid_size[3] = grid.count;
	params.grid_min[0] = float(grid.min.x);
	params.grid_min[1] = float(grid.min.y);
	params.grid_min[2] = float(grid.min.z);
	params.grid_min[3] = float(grid.spacing);
	params.counts[0] = int(lights.size());
	params.counts[1] = int(box_instances.size());
	params.counts[2] = lrt::DIRECTION_COUNT;
	params.counts[3] = int(display_mesh.node_min.size());
	params.flags[0] = multi_bounce ? 1.0f : 0.0f;
	params.flags[1] = sh_visibility ? 1.0f : 0.0f;
	params.flags[2] = local_backend == "sdf" ? 1.0f : 0.0f;
	params.sky_color[0] = sky.x;
	params.sky_color[1] = sky.y;
	params.sky_color[2] = sky.z;
	for (int i = 0; i < int(lights.size()); i++) {
		const Light &light = lights[i];
		params.light_position[i][0] = light.position.x;
		params.light_position[i][1] = light.position.y;
		params.light_position[i][2] = light.position.z;
		params.light_direction[i][0] = light.direction.x;
		params.light_direction[i][1] = light.direction.y;
		params.light_direction[i][2] = light.direction.z;
		params.light_color[i][0] = light.enabled ? light.color.x : 0.0f;
		params.light_color[i][1] = light.enabled ? light.color.y : 0.0f;
		params.light_color[i][2] = light.enabled ? light.color.z : 0.0f;
		params.light_data[i][0] = light.enabled ? light.intensity : 0.0f;
		params.light_data[i][1] = float(light.type);
		params.light_data[i][2] = 1.0f / MAX(light.range, 0.001f);
		params.light_data[i][3] = light.attenuation;
		params.light_spot[i][0] = Math::cos(Math::deg_to_rad(light.spot_angle_deg));
		params.light_spot[i][1] = light.spot_attenuation;
		params.light_spot[i][2] = light.casts_shadow ? 1.0f : 0.0f;
	}
	// The injection's occlusion test is the prototype's world axis-aligned box list, which
	// every box instance also provides.
	for (int i = 0; i < int(box_instances.size()) && i < MAX_BOX_COUNT; i++) {
		params.box_min[i][0] = float(box_instances[i].world_min.x);
		params.box_min[i][1] = float(box_instances[i].world_min.y);
		params.box_min[i][2] = float(box_instances[i].world_min.z);
		params.box_max[i][0] = float(box_instances[i].world_max.x);
		params.box_max[i][1] = float(box_instances[i].world_max.y);
		params.box_max[i][2] = float(box_instances[i].world_max.z);
		params.box_color[i][0] = float(box_instances[i].color.x);
		params.box_color[i][1] = float(box_instances[i].color.y);
		params.box_color[i][2] = float(box_instances[i].color.z);
	}
	return device->buffer_update(params_buffer, 0, sizeof(ParamsData), &params) == OK;
}

void LRTVolume::_upload_local_buffers() {
	if (local.material.empty()) {
		return;
	}
	device->buffer_update(material_buffer, 0, local.material.size() * sizeof(float), local.material.data());
	device->buffer_update(links_buffer, 0, local.links.size() * sizeof(uint32_t), local.links.data());
	device->buffer_update(matrix_buffer, 0, local.matrices.size() * sizeof(float), local.matrices.data());
	device->buffer_update(local_visibility_buffer, 0, local.local_visibility.size() * sizeof(float), local.local_visibility.data());
	if (!local.receivers.empty()) {
		device->buffer_update(receiver_buffer, 0, local.receivers.size() * sizeof(float), local.receivers.data());
	}
}

// The bake already runs on a worker thread, so it spreads over the remaining cores; the cap
// keeps a 32-thread machine from oversubscribing memory bandwidth for no gain.
static int lrt_bake_thread_count() {
	return CLAMP(OS::get_singleton()->get_processor_count() - 1, 1, 16);
}

// One Color SDF per mesh asset, in the asset's own bounds, exactly as
// primitive-gi.js bakeMeshSDF() builds it. The volume keeps one baked field per asset and
// shares it between instances, while every instance keeps its own PrimitiveGI transform.
bool LRTVolume::_build_primitives(const String &p_backend, int p_threads, std::vector<lrt::SdfPrimitive> &r_primitives,
		std::vector<lrt::Box> &r_boxes) {
	if (p_backend == "analytic") {
		for (const BoxInstance &box : box_instances) {
			if (!box.axis_aligned) {
				return false;
			}
			lrt::Box analytic;
			analytic.min = box.world_min;
			analytic.max = box.world_max;
			analytic.color = box.color;
			r_boxes.push_back(analytic);
		}
		return true;
	}

	r_primitives.reserve(box_instances.size() + mesh_instances.size());
	for (const BoxInstance &box : box_instances) {
		const uint64_t field_signature = lrt::box_field_signature(box.local_extent, box.color, BOX_SDF_RESOLUTION);
		const lrt::ColorSdfField field = lrt::bake_box_color_sdf(box.local_extent, box.color, BOX_SDF_RESOLUTION, &cancel_flag);
		if (field.distance.empty()) {
			return false;
		}
		r_primitives.push_back(lrt::make_sdf_primitive(field, box.transform, lrt::primitive_signature(field_signature, box.transform)));
	}

	// Cold builds spend almost all of their time here (measured: 28.7 s of 28.9 s for the
	// carriage), and assets are independent of each other, so the assets bake in parallel. The
	// cache lookup and the primitive order stay exactly what the serial version produced.
	struct AssetJob {
		int instance = -1;
		uint64_t signature = 0;
		bool from_cache = false;
		lrt::TriangleMesh mesh;
		lrt::ColorSdfField field;
	};
	std::vector<AssetJob> jobs;
	std::vector<int> job_of_instance(mesh_instances.size(), -1);
	std::map<int64_t, int> job_by_key;
	// Two instances of the same asset, and two assets with identical content, share one bake.
	std::map<uint64_t, int> job_by_signature;
	for (int i = 0; i < int(mesh_instances.size()); i++) {
		const MeshInstance &instance = mesh_instances[size_t(i)];
		if (instance.triangles.empty()) {
			continue;
		}
		const int64_t key = instance.asset_key;
		if (key != 0) {
			if (mesh_sdf_cache.find(key) != mesh_sdf_cache.end()) {
				continue;
			}
			const auto shared = job_by_key.find(key);
			if (shared != job_by_key.end()) {
				job_of_instance[size_t(i)] = shared->second;
				continue;
			}
		}
		const uint64_t signature = lrt::asset_signature(instance.triangles, mesh_sdf_resolution);
		const auto identical = job_by_signature.find(signature);
		if (identical != job_by_signature.end()) {
			if (key != 0) {
				job_by_key[key] = identical->second;
			}
			job_of_instance[size_t(i)] = identical->second;
			continue;
		}
		if (key != 0) {
			job_by_key[key] = int(jobs.size());
		}
		job_by_signature[signature] = int(jobs.size());
		AssetJob job;
		job.instance = i;
		job.signature = signature;
		job_of_instance[size_t(i)] = int(jobs.size());
		jobs.push_back(job);
	}
	// A previous run may already have baked this exact asset; the derived cache is keyed by the
	// triangle content, so an edited mesh simply misses it.
	for (AssetJob &job : jobs) {
		if (lrt::load_asset_field(job.signature, job.field)) {
			job.from_cache = true;
			assets_loaded++;
		}
	}
	// A few assets use one thread each; a single large asset spreads inside its own bake. The
	// two never nest, so the thread budget stays the same either way.
	const bool spread_over_assets = jobs.size() >= 4;
	const int asset_threads = spread_over_assets ? 1 : p_threads;
	int baked_assets = 0;
	lrt::parallel_for(int(jobs.size()), spread_over_assets ? p_threads : 1, [&](int p_job) {
		AssetJob &job = jobs[size_t(p_job)];
		if (job.from_cache || cancel_flag.load()) {
			return;
		}
		job.mesh = lrt::build_triangle_mesh(mesh_instances[size_t(job.instance)].triangles);
		job.field = lrt::bake_mesh_color_sdf(job.mesh, mesh_sdf_resolution, &cancel_flag, asset_threads);
	});
	if (cancel_flag.load()) {
		return false;
	}
	for (const AssetJob &job : jobs) {
		if (job.from_cache) {
			continue;
		}
		baked_assets++;
		lrt::store_asset_field(job.signature, job.field);
	}
	assets_baked += baked_assets;
	for (int i = 0; i < int(mesh_instances.size()); i++) {
		const MeshInstance &instance = mesh_instances[size_t(i)];
		if (instance.triangles.empty()) {
			continue;
		}
		const int64_t key = instance.asset_key;
		if (key != 0) {
			const auto cached = mesh_sdf_cache.find(key);
			if (cached != mesh_sdf_cache.end()) {
				const uint64_t field_signature = lrt::asset_signature(instance.triangles, mesh_sdf_resolution);
				r_primitives.push_back(lrt::make_sdf_primitive(cached->second, instance.transform,
						lrt::primitive_signature(field_signature, instance.transform)));
				continue;
			}
		}
		const int job_index = job_of_instance[size_t(i)];
		if (job_index < 0 || jobs[size_t(job_index)].field.distance.empty()) {
			return false;
		}
		const uint64_t field_signature = jobs[size_t(job_index)].signature;
		if (key != 0) {
			mesh_sdf_cache[key] = jobs[size_t(job_index)].field;
		}
		r_primitives.push_back(lrt::make_sdf_primitive(jobs[size_t(job_index)].field, instance.transform,
				lrt::primitive_signature(field_signature, instance.transform)));
	}
	return true;
}

int LRTVolume::_mesh_instance_count() const {
	int count = 0;
	for (const MeshInstance &instance : mesh_instances) {
		if (!instance.triangles.empty()) {
			count++;
		}
	}
	return count;
}

// The world-space triangle soup the display and the injection's occlusion test use: the
// shared asset triangles placed by each instance transform.
void LRTVolume::_build_display_mesh() {
	std::vector<lrt::MeshTriangle> soup;
	for (const MeshInstance &instance : mesh_instances) {
		const lrt::PrimitiveTransform &transform = instance.transform;
		for (const lrt::MeshTriangle &triangle : instance.triangles) {
			lrt::MeshTriangle world = triangle;
			for (int v = 0; v < 3; v++) {
				const lrt::Vec3 point = triangle.position[v];
				world.position[v] = transform.origin + transform.basis_x * (point.x * transform.scale) +
						transform.basis_y * (point.y * transform.scale) + transform.basis_z * (point.z * transform.scale);
			}
			soup.push_back(world);
		}
	}
	staged_display_mesh = soup.empty() ? lrt::TriangleMesh() : lrt::build_triangle_mesh(std::move(soup));
}

// The CPU half of the bake: the part that costs 10-15 s for the carriage. It only touches
// plain data, so LRTVolume3D can run it on a worker thread; every GPU call stays in
// apply_local_field() on the main thread.
LRTVolume::LocalBakeResult LRTVolume::bake_local_field_data(bool p_analytic) {
	LocalBakeResult result;
	if (!configured) {
		return result;
	}
	local_backend = p_analytic ? "analytic" : "sdf";
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	const int threads = lrt_bake_thread_count();
	assets_loaded = 0;
	assets_baked = 0;
	std::vector<lrt::SdfPrimitive> primitives;
	std::vector<lrt::Box> analytic_boxes;
	if (!_build_primitives(local_backend, threads, primitives, analytic_boxes)) {
		result.cancelled = cancel_flag.load();
		result.needs_axis_aligned = !result.cancelled;
		return result;
	}
	const uint64_t after_assets = OS::get_singleton()->get_ticks_usec();
	if (p_analytic) {
		staged_local = lrt::build_local_data(grid, lrt::BoxQuery(analytic_boxes), &cancel_flag, threads);
	} else {
		// The incremental path lives in the SDF backend, exactly like the prototype's
		// src/sdf-local.js; the analytic backend has no dirty-region support there either.
		staged_local = lrt::build_sdf_local_data(grid, primitives, &cancel_flag, threads, &local_cache, &staged_cache);
	}
	const uint64_t after_local = OS::get_singleton()->get_ticks_usec();
	if (cancel_flag.load()) {
		has_staged = false;
		result.cancelled = true;
		return result;
	}
	lrt::build_local_visibility(staged_local, threads);
	const uint64_t after_visibility = OS::get_singleton()->get_ticks_usec();
	_build_display_mesh();
	const uint64_t after_display = OS::get_singleton()->get_ticks_usec();
	has_staged = true;

	result.ok = true;
	result.solid = staged_local.solid_count;
	result.surface = staged_local.surface_count;
	result.receivers = int(staged_local.receivers.size());
	result.trunks = staged_local.trunk_count;
	result.dirty_trunks = staged_local.dirty_trunk_count;
	result.mismatches = staged_local.classification_mismatches;
	result.mesh_volumes = _mesh_instance_count();
	result.mesh_triangles = int(staged_display_mesh.triangles.size());
	result.assets_loaded = assets_loaded;
	result.assets_baked = assets_baked;
	result.assets_ms = double(after_assets - start) / 1000.0;
	result.local_ms = double(after_local - after_assets) / 1000.0;
	result.visibility_ms = double(after_visibility - after_local) / 1000.0;
	result.display_ms = double(after_display - after_visibility) / 1000.0;
	result.build_ms = double(after_display - start) / 1000.0;
	return result;
}

Dictionary LRTVolume::bake_local_field(const String &p_backend) {
	Dictionary result;
	ERR_FAIL_COND_V_MSG(!configured, result, "configure() the LRT volume before building its local field.");
	const LocalBakeResult baked = bake_local_field_data(p_backend == "analytic");
	if (!baked.ok) {
		if (baked.needs_axis_aligned) {
			result["error"] = "analytic backend requires axis-aligned boxes";
		}
		if (baked.cancelled) {
			result["cancelled"] = true;
		}
		return result;
	}
	result["ok"] = true;
	result["backend"] = local_backend;
	result["solid"] = baked.solid;
	result["surface"] = baked.surface;
	result["receivers"] = baked.receivers;
	result["trunks"] = baked.trunks;
	result["dirty_trunks"] = baked.dirty_trunks;
	result["classification_mismatches"] = baked.mismatches;
	result["mesh_triangles"] = baked.mesh_triangles;
	result["mesh_volumes"] = baked.mesh_volumes;
	result["build_ms"] = baked.build_ms;
	return result;
}

// The GPU half: recreate the field buffers for the staged local field and reset the state.
Dictionary LRTVolume::apply_local_field() {
	Dictionary result;
	ERR_FAIL_COND_V_MSG(!has_staged, result, "bake_local_field() must run before apply_local_field().");
	ERR_FAIL_COND_V(_ensure_device() != OK, result);

	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	local = staged_local;
	display_mesh = staged_display_mesh;
	// The incremental cache and the field it describes must always switch together.
	local_cache = std::move(staged_cache);
	local_cache.local = &local;
	_free_gpu_resources();
	ERR_FAIL_COND_V(_create_buffers() != OK, result);
	ERR_FAIL_COND_V(_create_shaders() != OK, result);
	ERR_FAIL_COND_V(_create_uniform_sets() != OK, result);
	_upload_local_buffers();
	has_local = true;
	iteration = 0;
	current = 0;
	_upload_params();
	reset();
	refresh_display();

	result["backend"] = local_backend;
	result["solid"] = local.solid_count;
	result["surface"] = local.surface_count;
	result["receivers"] = int(local.receivers.size());
	result["trunks"] = local.trunk_count;
	result["classification_mismatches"] = local.classification_mismatches;
	result["mesh_triangles"] = int(display_mesh.triangles.size());
	result["mesh_volumes"] = _mesh_instance_count();
	result["upload_ms"] = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
	return result;
}

Dictionary LRTVolume::build_local_field(const String &p_backend) {
	Dictionary result = bake_local_field(p_backend);
	if (!result.has("ok")) {
		return result;
	}
	Dictionary applied = apply_local_field();
	if (applied.is_empty()) {
		return Dictionary();
	}
	applied["build_ms"] = result["build_ms"];
	applied["mesh_volumes"] = result["mesh_volumes"];
	return applied;
}

void LRTVolume::inject() {
	ERR_FAIL_COND_MSG(!has_local, "build_local_field() must run before inject().");
	ERR_FAIL_COND(_ensure_device() != OK);
	_upload_params();
	RD::ComputeListID list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(list, pipeline_inject);
	device->compute_list_bind_uniform_set(list, uniform_set_inject, 0);
	device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
	device->compute_list_end();
	device->submit();
	device->sync();
}

void LRTVolume::step(int p_iterations) {
	ERR_FAIL_COND_MSG(!has_local, "build_local_field() must run before step().");
	ERR_FAIL_COND(p_iterations < 1);
	ERR_FAIL_COND(_ensure_device() != OK);
	_upload_params();
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	RD::ComputeListID list = device->compute_list_begin();
	for (int i = 0; i < p_iterations; i++) {
		device->compute_list_bind_compute_pipeline(list, pipeline_propagate);
		device->compute_list_bind_uniform_set(list, uniform_set_propagate[current], 0);
		device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
		if (i + 1 < p_iterations) {
			device->compute_list_add_barrier(list);
		}
		current = 1 - current;
	}
	device->compute_list_end();
	device->submit();
	device->sync();
	iteration += p_iterations;
	last_gpu_ms = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
}

void LRTVolume::reset() {
	ERR_FAIL_COND_MSG(!has_local, "build_local_field() must run before reset().");
	ERR_FAIL_COND(_ensure_device() != OK);
	const size_t bytes = size_t(grid.count) * 4 * sizeof(float);
	std::vector<float> zeros(size_t(grid.count) * 4, 0.0f);
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			device->buffer_update(radiance_buffers[buffer][channel], 0, bytes, zeros.data());
		}
		device->buffer_update(visibility_buffers[buffer], 0, bytes, zeros.data());
	}
	current = 0;
	iteration = 0;
}

int LRTVolume::get_iteration() const {
	return iteration;
}

Dictionary LRTVolume::get_grid() const {
	Dictionary result;
	result["min"] = Vector3(grid.min.x, grid.min.y, grid.min.z);
	result["size"] = Vector3i(grid.size[0], grid.size[1], grid.size[2]);
	result["spacing"] = grid.spacing;
	result["width"] = grid.width;
	result["height"] = grid.height;
	result["count"] = grid.count;
	return result;
}

Error LRTVolume::_read_back_fields() {
	const size_t bytes = size_t(grid.count) * 4 * sizeof(float);
	for (int channel = 0; channel < 3; channel++) {
		const Vector<uint8_t> data = device->buffer_get_data(radiance_buffers[current][channel]);
		ERR_FAIL_COND_V(int64_t(data.size()) != int64_t(bytes), ERR_CANT_ACQUIRE_RESOURCE);
		radiance_cpu[channel].resize(size_t(grid.count) * 4);
		memcpy(radiance_cpu[channel].data(), data.ptr(), bytes);
	}
	const Vector<uint8_t> visibility_data = device->buffer_get_data(visibility_buffers[current]);
	ERR_FAIL_COND_V(int64_t(visibility_data.size()) != int64_t(bytes), ERR_CANT_ACQUIRE_RESOURCE);
	visibility_cpu.resize(size_t(grid.count) * 4);
	memcpy(visibility_cpu.data(), visibility_data.ptr(), bytes);
	for (int channel = 0; channel < 3; channel++) {
		const Vector<uint8_t> data = device->buffer_get_data(source_buffers[channel]);
		ERR_FAIL_COND_V(int64_t(data.size()) != int64_t(bytes), ERR_CANT_ACQUIRE_RESOURCE);
		source_cpu[channel].resize(size_t(grid.count) * 4);
		memcpy(source_cpu[channel].data(), data.ptr(), bytes);
	}
	return OK;
}

void LRTVolume::refresh_display() {
	if (!has_local || _read_back_fields() != OK) {
		return;
	}
	const int width = grid.width;
	const int height = grid.height;
	auto update_field_texture = [&](const std::vector<float> &p_values, Ref<Image> &r_image, Ref<ImageTexture> &r_texture, int p_height) {
		const PackedByteArray bytes = bytes_of(p_values.data(), p_values.size() * sizeof(float));
		// A rebuild can change the probe grid (spacing, volume size, geometry bounds), so the
		// atlas has to be recreated whenever its dimensions change.
		if (r_image.is_null() || r_image->get_width() != width || r_image->get_height() != p_height) {
			r_image = Image::create_from_data(width, p_height, false, Image::FORMAT_RGBAF, bytes);
			r_texture = ImageTexture::create_from_image(r_image);
		} else {
			r_image->set_data(width, p_height, false, Image::FORMAT_RGBAF, bytes);
			r_texture->update(r_image);
		}
	};
	for (int channel = 0; channel < 3; channel++) {
		update_field_texture(radiance_cpu[channel], field_images[channel], field_textures[channel], height);
		update_field_texture(source_cpu[channel], source_images[channel], source_textures[channel], height);
	}
	update_field_texture(visibility_cpu, visibility_image, visibility_texture, height);
	update_field_texture(local.material, material_image, material_texture, height);
	update_field_texture(local.local_visibility, local_visibility_image, local_visibility_texture, height);
	update_field_texture(local.matrices, matrix_image, matrix_texture, height * 12);
}

Ref<Texture2D> LRTVolume::get_texture(const String &p_name) const {
	if (p_name == "radiance_r") {
		return field_textures[0];
	}
	if (p_name == "radiance_g") {
		return field_textures[1];
	}
	if (p_name == "radiance_b") {
		return field_textures[2];
	}
	if (p_name == "visibility") {
		return visibility_texture;
	}
	if (p_name == "source_r") {
		return source_textures[0];
	}
	if (p_name == "source_g") {
		return source_textures[1];
	}
	if (p_name == "source_b") {
		return source_textures[2];
	}
	if (p_name == "material") {
		return material_texture;
	}
	if (p_name == "matrices") {
		return matrix_texture;
	}
	if (p_name == "local_visibility") {
		return local_visibility_texture;
	}
	return Ref<Texture2D>();
}

PackedFloat32Array LRTVolume::read_field(const String &p_name) const {
	const std::vector<float> *values = nullptr;
	if (p_name == "radiance_r") {
		values = &radiance_cpu[0];
	} else if (p_name == "radiance_g") {
		values = &radiance_cpu[1];
	} else if (p_name == "radiance_b") {
		values = &radiance_cpu[2];
	} else if (p_name == "visibility") {
		values = &visibility_cpu;
	} else if (p_name == "source_r") {
		values = &source_cpu[0];
	} else if (p_name == "source_g") {
		values = &source_cpu[1];
	} else if (p_name == "source_b") {
		values = &source_cpu[2];
	} else if (p_name == "material") {
		values = &local.material;
	} else if (p_name == "matrices") {
		values = &local.matrices;
	} else if (p_name == "local_visibility") {
		values = &local.local_visibility;
	} else if (p_name == "receivers") {
		values = &local.receivers;
	}
	PackedFloat32Array result;
	if (!values || values->empty()) {
		return result;
	}
	result.resize(values->size());
	memcpy(result.ptrw(), values->data(), values->size() * sizeof(float));
	return result;
}

PackedInt32Array LRTVolume::read_links() const {
	PackedInt32Array result;
	if (local.links.empty()) {
		return result;
	}
	result.resize(local.links.size());
	int32_t *write = result.ptrw();
	for (size_t i = 0; i < local.links.size(); i++) {
		write[i] = int32_t(local.links[i]);
	}
	return result;
}

// Display-side BVH for the receiver's occlusion test. The combined tree covers every
// supplied mesh volume; the shader only needs a correct nearest hit.
Dictionary LRTVolume::get_mesh_bvh() const {
	Dictionary result;
	const std::vector<float> nodes = lrt::mesh_node_data(display_mesh);
	const std::vector<float> triangles = lrt::mesh_triangle_data(display_mesh);
	const std::vector<float> materials = lrt::mesh_material_data();
	auto pack = [](const std::vector<float> &p_values) {
		PackedFloat32Array array;
		array.resize(int64_t(p_values.size()));
		if (!p_values.empty()) {
			memcpy(array.ptrw(), p_values.data(), p_values.size() * sizeof(float));
		}
		return array;
	};
	result["nodes"] = pack(nodes);
	result["triangles"] = pack(triangles);
	result["materials"] = pack(materials);
	result["node_count"] = int(display_mesh.node_min.size());
	result["triangle_count"] = int(display_mesh.triangles.size());
	result["closed"] = display_mesh.has_closed_shell;
	return result;
}

Dictionary LRTVolume::get_stats() const {
	Dictionary result;
	result["iteration"] = iteration;
	result["last_gpu_ms"] = last_gpu_ms;
	result["solid"] = local.solid_count;
	result["surface"] = local.surface_count;
	result["count"] = grid.count;
	result["backend"] = local_backend;
	result["textures_ready"] = bool(field_textures[0].is_valid());
	return result;
}
