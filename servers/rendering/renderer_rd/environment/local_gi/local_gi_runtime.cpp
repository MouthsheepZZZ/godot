/**************************************************************************/
/*  local_gi_runtime.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "local_gi_runtime.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#include <cstring>

namespace RendererRD {

namespace {

struct PackedVec4 {
	float value[4] = {};
};

struct PackedMoment {
	float mean = 0.0f;
	float second = 0.0f;
	float pad0 = 0.0f;
	float pad1 = 0.0f;
};

struct PushConstant {
	int32_t static_node_count = 0;
	int32_t dynamic_node_count = 0;
	int32_t probe_count = 0;
	int32_t rays_per_probe = 0;
	int32_t light_count = 0;
	int32_t resolution[3] = {};
	float volume_size[3] = {};
	float visibility_bias = 0.0f;
	float temporal_hysteresis = 0.0f;
	int32_t temporal_cursor = 0;
	int32_t update_count = 0;
	uint32_t history_valid = 0;
	uint32_t multi_bounce = 0;
	float far_distance = 1.0f;
	float pad0 = 0.0f;
};

void pack_vec4(const Vector<Vector3> &p_source, Vector<PackedVec4> &r_dest) {
	r_dest.resize(p_source.size());
	for (int i = 0; i < p_source.size(); i++) {
		PackedVec4 value;
		value.value[0] = p_source[i].x;
		value.value[1] = p_source[i].y;
		value.value[2] = p_source[i].z;
		r_dest.write[i] = value;
	}
}

void pack_colors(const Vector<Color> &p_source, int p_count, Vector<PackedVec4> &r_dest) {
	r_dest.resize(p_count);
	for (int i = 0; i < p_count; i++) {
		PackedVec4 value;
		if (i < p_source.size()) {
			value.value[0] = p_source[i].r;
			value.value[1] = p_source[i].g;
			value.value[2] = p_source[i].b;
			value.value[3] = 1.0f;
		}
		r_dest.write[i] = value;
	}
}

void pack_moments(const Vector<float> &p_mean, const Vector<float> &p_second, int p_count, Vector<PackedMoment> &r_dest) {
	r_dest.resize(p_count);
	for (int i = 0; i < p_count; i++) {
		PackedMoment value;
		if (i < p_mean.size()) {
			value.mean = p_mean[i];
		}
		if (i < p_second.size()) {
			value.second = p_second[i];
		}
		r_dest.write[i] = value;
	}
}

} // namespace

RID LocalGIRuntime::_create_storage_buffer(const void *p_data, uint32_t p_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, RID());
	const uint32_t size_bytes = MAX(p_bytes, 16u);
	Vector<uint8_t> bytes;
	bytes.resize(size_bytes);
	memset(bytes.ptrw(), 0, size_bytes);
	if (p_data != nullptr && p_bytes > 0) {
		memcpy(bytes.ptrw(), p_data, p_bytes);
	}
	return rd->storage_buffer_create(size_bytes, bytes);
}

void LocalGIRuntime::_free_rid(RID &r_rid) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && r_rid.is_valid()) {
		rd->free_rid(r_rid);
	}
	r_rid = RID();
}

bool LocalGIRuntime::ensure_initialized() {
	if (initialized) {
		return pipeline.is_valid();
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V_MSG(rd, false, "LocalGI renderer runtime requires the renderer RenderingDevice.");

	shader = memnew(LocalGiTransportShaderRD);
	Vector<String> versions;
	versions.push_back("");
	shader->initialize(versions);
	shader_version = shader->version_create();
	RID shader_rid = shader->version_get_shader(shader_version, 0);
	pipeline = rd->compute_pipeline_create(shader_rid);
	if (pipeline.is_null()) {
		shader->version_free(shader_version);
		shader_version = RID();
		memdelete(shader);
		shader = nullptr;
		ERR_FAIL_V_MSG(false, "LocalGI renderer runtime compute pipeline creation failed.");
	}
	initialized = true;
	return true;
}

bool LocalGIRuntime::process(const Input &p_input, Output &r_output) {
	r_output = Output();
	ERR_FAIL_COND_V(p_input.probe_positions.is_empty(), false);
	ERR_FAIL_COND_V(p_input.ray_directions.is_empty(), false);
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, false);

	pending_input = &p_input;
	pending_output = &r_output;
	pending_result = false;
	if (rendering_server->is_on_render_thread()) {
		_process_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&LocalGIRuntime::_process_callback).bind((uint64_t)this));
		render_thread_done.wait();
	}
	pending_input = nullptr;
	pending_output = nullptr;
	return pending_result;
}

void LocalGIRuntime::_process_callback(uint64_t p_instance) {
	LocalGIRuntime *runtime = reinterpret_cast<LocalGIRuntime *>(p_instance);
	runtime->_process_render_thread();
	runtime->render_thread_done.post();
}

void LocalGIRuntime::_process_render_thread() {
	ERR_FAIL_NULL(pending_input);
	ERR_FAIL_NULL(pending_output);
	const Input &p_input = *pending_input;
	Output &r_output = *pending_output;
	if (!ensure_initialized()) {
		return;
	}

	RenderingDevice *rd = RD::get_singleton();
	const int probe_count = p_input.probe_positions.size();
	const int rays_per_probe = p_input.ray_directions.size();
	const int ray_count = probe_count * rays_per_probe;

	Vector<PackedVec4> probe_positions;
	Vector<PackedVec4> ray_directions;
	Vector<PackedVec4> history;
	Vector<PackedMoment> history_moments;
	pack_vec4(p_input.probe_positions, probe_positions);
	pack_vec4(p_input.ray_directions, ray_directions);
	pack_colors(p_input.irradiance_history, probe_count, history);
	pack_moments(p_input.distance_mean_history, p_input.distance_second_moment_history, ray_count, history_moments);

	RID buffers[15];
	buffers[0] = _create_storage_buffer(p_input.static_nodes.ptr(), p_input.static_nodes.size() * sizeof(Node));
	buffers[1] = _create_storage_buffer(p_input.static_triangles.ptr(), p_input.static_triangles.size() * sizeof(Triangle));
	buffers[2] = _create_storage_buffer(p_input.dynamic_nodes.ptr(), p_input.dynamic_nodes.size() * sizeof(Node));
	buffers[3] = _create_storage_buffer(p_input.dynamic_triangles.ptr(), p_input.dynamic_triangles.size() * sizeof(Triangle));
	buffers[4] = _create_storage_buffer(probe_positions.ptr(), probe_positions.size() * sizeof(PackedVec4));
	buffers[5] = _create_storage_buffer(ray_directions.ptr(), ray_directions.size() * sizeof(PackedVec4));
	buffers[6] = _create_storage_buffer(p_input.lights.ptr(), p_input.lights.size() * sizeof(Light));
	buffers[7] = _create_storage_buffer(history.ptr(), history.size() * sizeof(PackedVec4));
	buffers[8] = _create_storage_buffer(history_moments.ptr(), history_moments.size() * sizeof(PackedMoment));
	buffers[9] = _create_storage_buffer(nullptr, probe_count * sizeof(PackedVec4));
	buffers[10] = _create_storage_buffer(nullptr, probe_count * sizeof(PackedVec4));
	buffers[11] = _create_storage_buffer(nullptr, ray_count * sizeof(PackedVec4));
	buffers[12] = _create_storage_buffer(nullptr, ray_count * sizeof(PackedMoment));
	buffers[13] = _create_storage_buffer(nullptr, ray_count * sizeof(PackedMoment));
	buffers[14] = _create_storage_buffer(nullptr, probe_count * sizeof(uint32_t));

	for (int i = 0; i < 15; i++) {
		if (buffers[i].is_null()) {
			for (int j = 0; j < 15; j++) {
				_free_rid(buffers[j]);
			}
			return;
		}
	}

	Vector<RD::Uniform> uniforms;
	for (int i = 0; i < 15; i++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, i, buffers[i]));
	}
	RID uniform_set = rd->uniform_set_create(uniforms, shader->version_get_shader(shader_version, 0), 0);
	if (uniform_set.is_null()) {
		for (int i = 0; i < 15; i++) {
			_free_rid(buffers[i]);
		}
		return;
	}

	PushConstant push;
	push.static_node_count = p_input.static_nodes.size();
	push.dynamic_node_count = p_input.dynamic_nodes.size();
	push.probe_count = probe_count;
	push.rays_per_probe = rays_per_probe;
	push.light_count = p_input.lights.size();
	push.resolution[0] = p_input.probe_resolution.x;
	push.resolution[1] = p_input.probe_resolution.y;
	push.resolution[2] = p_input.probe_resolution.z;
	push.volume_size[0] = p_input.volume_size.x;
	push.volume_size[1] = p_input.volume_size.y;
	push.volume_size[2] = p_input.volume_size.z;
	push.visibility_bias = p_input.visibility_bias;
	push.temporal_hysteresis = p_input.temporal_hysteresis;
	push.temporal_cursor = p_input.temporal_cursor;
	push.update_count = p_input.update_count;
	push.history_valid = p_input.history_valid ? 1u : 0u;
	push.multi_bounce = p_input.multi_bounce ? 1u : 0u;
	push.far_distance = MAX(p_input.volume_size.length(), 1.0f);

	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, (probe_count + 63) / 64, 1, 1);
	rd->compute_list_end();

	const Vector<uint8_t> sample_bytes = rd->buffer_get_data(buffers[9]);
	const Vector<uint8_t> estimate_bytes = rd->buffer_get_data(buffers[10]);
	const Vector<uint8_t> radiance_bytes = rd->buffer_get_data(buffers[11]);
	const Vector<uint8_t> sample_moment_bytes = rd->buffer_get_data(buffers[12]);
	const Vector<uint8_t> estimate_moment_bytes = rd->buffer_get_data(buffers[13]);
	const Vector<uint8_t> active_bytes = rd->buffer_get_data(buffers[14]);

	_free_rid(uniform_set);
	for (int i = 0; i < 15; i++) {
		_free_rid(buffers[i]);
	}

	ERR_FAIL_COND(sample_bytes.size() < probe_count * (int)sizeof(PackedVec4));
	ERR_FAIL_COND(estimate_bytes.size() < probe_count * (int)sizeof(PackedVec4));
	ERR_FAIL_COND(radiance_bytes.size() < ray_count * (int)sizeof(PackedVec4));
	ERR_FAIL_COND(sample_moment_bytes.size() < ray_count * (int)sizeof(PackedMoment));
	ERR_FAIL_COND(estimate_moment_bytes.size() < ray_count * (int)sizeof(PackedMoment));
	ERR_FAIL_COND(active_bytes.size() < probe_count * (int)sizeof(uint32_t));

	const PackedVec4 *samples = reinterpret_cast<const PackedVec4 *>(sample_bytes.ptr());
	const PackedVec4 *estimates = reinterpret_cast<const PackedVec4 *>(estimate_bytes.ptr());
	const PackedVec4 *radiances = reinterpret_cast<const PackedVec4 *>(radiance_bytes.ptr());
	const PackedMoment *sample_moments = reinterpret_cast<const PackedMoment *>(sample_moment_bytes.ptr());
	const PackedMoment *estimate_moments = reinterpret_cast<const PackedMoment *>(estimate_moment_bytes.ptr());
	const uint32_t *active = reinterpret_cast<const uint32_t *>(active_bytes.ptr());

	r_output.irradiance_samples.resize(probe_count);
	r_output.irradiance_estimates.resize(probe_count);
	r_output.probe_active.resize(probe_count);
	for (int i = 0; i < probe_count; i++) {
		r_output.irradiance_samples.write[i] = Color(samples[i].value[0], samples[i].value[1], samples[i].value[2], 1.0f);
		r_output.irradiance_estimates.write[i] = Color(estimates[i].value[0], estimates[i].value[1], estimates[i].value[2], 1.0f);
		r_output.probe_active.write[i] = active[i] != 0 ? 1 : 0;
	}

	r_output.ray_radiances.resize(ray_count);
	r_output.distance_mean_samples.resize(ray_count);
	r_output.distance_second_moment_samples.resize(ray_count);
	r_output.distance_mean_estimates.resize(ray_count);
	r_output.distance_second_moment_estimates.resize(ray_count);
	for (int i = 0; i < ray_count; i++) {
		r_output.ray_radiances.write[i] = Color(radiances[i].value[0], radiances[i].value[1], radiances[i].value[2], 1.0f);
		r_output.distance_mean_samples.write[i] = sample_moments[i].mean;
		r_output.distance_second_moment_samples.write[i] = sample_moments[i].second;
		r_output.distance_mean_estimates.write[i] = estimate_moments[i].mean;
		r_output.distance_second_moment_estimates.write[i] = estimate_moments[i].second;
	}
	pending_result = true;
}

void LocalGIRuntime::_free_callback(uint64_t p_instance) {
	LocalGIRuntime *runtime = reinterpret_cast<LocalGIRuntime *>(p_instance);
	runtime->_free_render_thread();
	runtime->render_thread_done.post();
}

void LocalGIRuntime::_free_render_thread() {
	_free_rid(pipeline);
	if (shader != nullptr) {
		if (shader_version.is_valid()) {
			shader->version_free(shader_version);
			shader_version = RID();
		}
		memdelete(shader);
		shader = nullptr;
	}
	initialized = false;
}

void LocalGIRuntime::free() {
	if (!initialized) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		return;
	}
	if (rendering_server->is_on_render_thread()) {
		_free_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&LocalGIRuntime::_free_callback).bind((uint64_t)this));
		render_thread_done.wait();
	}
}

LocalGIRuntime::~LocalGIRuntime() {
	free();
}

} // namespace RendererRD
