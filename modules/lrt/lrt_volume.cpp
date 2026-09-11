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

#include "lrt_inject.glsl.gen.h"
#include "lrt_propagate.glsl.gen.h"

#include "core/io/image.h"
#include "core/os/os.h"
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
	float flags[4] = { 0, 0, 0, 0 };
	float light_position[MAX_LIGHT_COUNT][4] = {};
	float light_direction[MAX_LIGHT_COUNT][4] = {};
	float light_color[MAX_LIGHT_COUNT][4] = {};
	float light_data[MAX_LIGHT_COUNT][4] = {};
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
	ClassDB::bind_method(D_METHOD("set_boxes", "boxes"), &LRTVolume::set_boxes);
	ClassDB::bind_method(D_METHOD("set_lights", "lights"), &LRTVolume::set_lights);
	ClassDB::bind_method(D_METHOD("set_sky", "sky"), &LRTVolume::set_sky);
	ClassDB::bind_method(D_METHOD("set_multi_bounce", "enabled"), &LRTVolume::set_multi_bounce);
	ClassDB::bind_method(D_METHOD("set_sh_visibility", "enabled"), &LRTVolume::set_sh_visibility);
	ClassDB::bind_method(D_METHOD("build_local_field", "backend"), &LRTVolume::build_local_field);
	ClassDB::bind_method(D_METHOD("inject"), &LRTVolume::inject);
	ClassDB::bind_method(D_METHOD("step", "iterations"), &LRTVolume::step);
	ClassDB::bind_method(D_METHOD("reset"), &LRTVolume::reset);
	ClassDB::bind_method(D_METHOD("get_iteration"), &LRTVolume::get_iteration);
	ClassDB::bind_method(D_METHOD("get_grid"), &LRTVolume::get_grid);
	ClassDB::bind_method(D_METHOD("refresh_display"), &LRTVolume::refresh_display);
	ClassDB::bind_method(D_METHOD("get_texture", "name"), &LRTVolume::get_texture);
	ClassDB::bind_method(D_METHOD("read_field", "name"), &LRTVolume::read_field);
	ClassDB::bind_method(D_METHOD("read_links"), &LRTVolume::read_links);
	ClassDB::bind_method(D_METHOD("get_stats"), &LRTVolume::get_stats);
}

void LRTVolume::configure(double p_spacing) {
	grid = lrt::make_grid(p_spacing);
	configured = true;
	has_local = false;
}

void LRTVolume::configure_with_bounds(double p_spacing, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max) {
	grid = lrt::make_grid(p_spacing,
			lrt::Vec3(p_bounds_min.x, p_bounds_min.y, p_bounds_min.z),
			lrt::Vec3(p_bounds_max.x, p_bounds_max.y, p_bounds_max.z));
	configured = true;
	has_local = false;
}

void LRTVolume::set_boxes(const Array &p_boxes) {
	boxes.clear();
	for (int i = 0; i < p_boxes.size(); i++) {
		const Dictionary entry = p_boxes[i];
		const Vector3 minimum = entry.get("min", Vector3());
		const Vector3 maximum = entry.get("max", Vector3());
		const Vector3 color = entry.get("color", Vector3());
		lrt::Box box;
		box.min = lrt::Vec3(minimum.x, minimum.y, minimum.z);
		box.max = lrt::Vec3(maximum.x, maximum.y, maximum.z);
		box.color = lrt::Vec3(color.x, color.y, color.z);
		boxes.push_back(box);
	}
	has_local = false;
}

void LRTVolume::set_lights(const Array &p_lights) {
	lights.clear();
	for (int i = 0; i < p_lights.size() && i < MAX_LIGHT_COUNT; i++) {
		const Dictionary entry = p_lights[i];
		Light light;
		light.type = entry.get("type", 0);
		light.enabled = entry.get("enabled", true);
		light.position = entry.get("position", Vector3());
		light.direction = entry.get("direction", Vector3(0, -1, 0));
		light.color = entry.get("color", Vector3(1, 1, 1));
		light.power = entry.get("power", 0.0);
		light.angle_deg = entry.get("angle_deg", 35.0);
		lights.push_back(light);
	}
}

void LRTVolume::set_sky(double p_sky) {
	sky = float(p_sky);
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
					matrix_buffer.is_null() || local_visibility_buffer.is_null() || receiver_buffer.is_null(),
			ERR_CANT_CREATE);
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
	params_buffer = RID();
	material_buffer = RID();
	links_buffer = RID();
	matrix_buffer = RID();
	local_visibility_buffer = RID();
	receiver_buffer = RID();
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
	params.counts[1] = int(boxes.size());
	params.counts[2] = lrt::DIRECTION_COUNT;
	params.flags[0] = sky;
	params.flags[1] = multi_bounce ? 1.0f : 0.0f;
	params.flags[2] = sh_visibility ? 1.0f : 0.0f;
	params.flags[3] = local_backend == "sdf" ? 1.0f : 0.0f;
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
		params.light_data[i][0] = light.enabled ? light.power : 0.0f;
		params.light_data[i][1] = float(light.type);
		params.light_data[i][2] = Math::cos(Math::deg_to_rad(light.angle_deg));
		params.light_data[i][3] = Math::cos(light.angle_deg * 0.8 * Math::PI / 180.0);
	}
	for (int i = 0; i < int(boxes.size()) && i < MAX_BOX_COUNT; i++) {
		params.box_min[i][0] = float(boxes[i].min.x);
		params.box_min[i][1] = float(boxes[i].min.y);
		params.box_min[i][2] = float(boxes[i].min.z);
		params.box_max[i][0] = float(boxes[i].max.x);
		params.box_max[i][1] = float(boxes[i].max.y);
		params.box_max[i][2] = float(boxes[i].max.z);
		params.box_color[i][0] = float(boxes[i].color.x);
		params.box_color[i][1] = float(boxes[i].color.y);
		params.box_color[i][2] = float(boxes[i].color.z);
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

Dictionary LRTVolume::build_local_field(const String &p_backend) {
	Dictionary result;
	ERR_FAIL_COND_V_MSG(!configured, result, "configure() the LRT volume before building its local field.");
	ERR_FAIL_COND_V_MSG(boxes.empty(), result, "set_boxes() must provide at least one box for the N1 fixtures.");
	ERR_FAIL_COND_V(_ensure_device() != OK, result);

	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	local_backend = p_backend == "analytic" ? "analytic" : "sdf";
	if (local_backend == "sdf") {
		std::vector<lrt::SdfPrimitive> primitives;
		primitives.reserve(boxes.size());
		for (const lrt::Box &box : boxes) {
			const lrt::Vec3 extent = box.max - box.min;
			const lrt::Vec3 position = (box.max + box.min) * 0.5;
			primitives.push_back(lrt::make_sdf_primitive(position, lrt::bake_box_color_sdf(extent, box.color, BOX_SDF_RESOLUTION)));
		}
		local = lrt::build_sdf_local_data(grid, primitives);
	} else {
		local = lrt::build_local_data(grid, lrt::BoxQuery(boxes));
	}
	lrt::build_local_visibility(local);
	const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;

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
	result["build_ms"] = double(elapsed) / 1000.0;
	return result;
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
		if (r_image.is_null()) {
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
