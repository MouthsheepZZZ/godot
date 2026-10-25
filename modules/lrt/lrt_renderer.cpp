/**************************************************************************/
/*  lrt_renderer.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
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

#include "lrt_renderer.h"

#include "core/config/project_settings.h"
#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/os/thread.h"
#include "servers/rendering/rendering_server.h"

#ifdef RD_ENABLED
#include "shaders/lrt_geometry_upload.glsl.gen.h"
#include "shaders/lrt_inject.glsl.gen.h"
#include "shaders/lrt_propagate.glsl.gen.h"
#include "shaders/lrt_sky.glsl.gen.h"

#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"
#endif

LRTRenderer *LRTRenderer::singleton = nullptr;

LRTRenderer::LRTRenderer() {
	singleton = this;
}

namespace {

constexpr uint32_t GEOMETRY_STRIDE = 72;
constexpr uint32_t EMISSION_STRIDE = 48;
constexpr uint32_t HISTORY_STRIDE = 64;
constexpr uint32_t UPDATE_STRIDE = 124;

void pack_probe(uint8_t *r_record, uint32_t p_index, const uint8_t *p_ltm, const uint8_t *p_vis, const uint8_t *p_masks, const float *p_emission) {
	encode_uint32(p_index, r_record);
	memcpy(r_record + 4, p_ltm, 36);
	memcpy(r_record + 40, p_vis, 8);
	memcpy(r_record + 48, p_masks, 26);
	r_record[74] = 0;
	r_record[75] = 0;
	if (p_emission) {
		memcpy(r_record + 76, p_emission, EMISSION_STRIDE);
	} else {
		memset(r_record + 76, 0, EMISSION_STRIDE);
	}
}

} // namespace

struct LRTRenderer::State {
#ifdef RD_ENABLED
	struct Volume {
		Vector3i grid;
		uint64_t revision = 0;
		RID geometry;
		RID emission;
		RID sources;
		RID history[2];
		RID propagation_sets[2];
		uint32_t history_index = 0;
		uint64_t propagation_steps = 0;
		uint64_t propagation_frame = 0;

		void clear() {
			for (RID rid : { propagation_sets[0], propagation_sets[1], geometry, emission, sources, history[0], history[1] }) {
				if (rid.is_valid()) {
					RD::get_singleton()->free_rid(rid);
				}
			}
		}
	};

	HashMap<uint64_t, Volume> volumes;
	LrtGeometryUploadShaderRD shader;
	RID shader_version;
	RID pipeline;
	LrtPropagateShaderRD propagation_shader;
	RID propagation_version;
	RID propagation_pipeline;
	LrtInjectShaderRD injection_shader;
	RID injection_version;
	RID injection_pipeline;
	LrtSkyShaderRD sky_shader;
	RID sky_version;
	RID sky_pipeline;
	RID default_metadata;
	RID default_samples;
	RID shadow_sampler;
	RID scenario;
	Vector<RID> scene_lights;
	Array descriptions;
	uint64_t frame = 0;
	int iterations = 2;
	double relaxation = 0.5;
	RID injection_lights;
	RID injection_sky;
	RID injection_shadow;
	RID injection_directional;
	uint32_t injection_light_count = 0;

	struct Draw {
		RID metadata;
		RID samples;
		RID sky;
		uint32_t metadata_bytes = 0;
		uint32_t sample_bytes = 0;
		uint64_t frame = 0;
		void clear() {
			for (RID rid : { metadata, samples, sky }) {
				if (rid.is_valid()) {
					RD::get_singleton()->free_rid(rid);
				}
			}
		}
	};
	HashMap<RID, Draw> draws;

	State() {
		PackedByteArray zeros;
		zeros.resize(64);
		default_metadata = RD::get_singleton()->storage_buffer_create(64, zeros);
		default_samples = RD::get_singleton()->storage_buffer_create(64, zeros);
		RD::SamplerState sampler;
		sampler.min_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler.enable_compare = true;
		sampler.compare_op = RD::COMPARE_OP_GREATER;
		shadow_sampler = RD::get_singleton()->sampler_create(sampler);
		Vector<String> variants;
		variants.push_back("");
		shader.initialize(variants);
		shader_version = shader.version_create();
		RID shader_rid = shader.version_get_shader(shader_version, 0);
		if (shader_rid.is_valid()) {
			pipeline = RD::get_singleton()->compute_pipeline_create(shader_rid);
		}
		propagation_shader.initialize(variants);
		propagation_version = propagation_shader.version_create();
		shader_rid = propagation_shader.version_get_shader(propagation_version, 0);
		if (shader_rid.is_valid()) {
			propagation_pipeline = RD::get_singleton()->compute_pipeline_create(shader_rid);
		}
		injection_shader.initialize(variants);
		injection_version = injection_shader.version_create();
		injection_pipeline = RD::get_singleton()->compute_pipeline_create(injection_shader.version_get_shader(injection_version, 0));
		sky_shader.initialize(variants, RendererSceneRenderRD::get_singleton()->is_using_radiance_octmap_array() ? "#define USE_RADIANCE_ARRAY\n" : "");
		sky_version = sky_shader.version_create();
		sky_pipeline = RD::get_singleton()->compute_pipeline_create(sky_shader.version_get_shader(sky_version, 0));
	}

	~State() {
		for (KeyValue<RID, Draw> &entry : draws) {
			entry.value.clear();
		}
		RD::get_singleton()->free_rid(default_metadata);
		RD::get_singleton()->free_rid(default_samples);
		RD::get_singleton()->free_rid(shadow_sampler);
		if (injection_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(injection_pipeline);
		}
		injection_shader.version_free(injection_version);
		if (sky_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(sky_pipeline);
		}
		sky_shader.version_free(sky_version);
		for (KeyValue<uint64_t, Volume> &entry : volumes) {
			entry.value.clear();
		}
		if (pipeline.is_valid()) {
			RD::get_singleton()->free_rid(pipeline);
		}
		shader.version_free(shader_version);
		if (propagation_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(propagation_pipeline);
		}
		propagation_shader.version_free(propagation_version);
	}
#endif
};

void LRTRenderer::submit(ObjectID p_volume, uint64_t p_revision, const LRTGeometry::StaticCache &p_baseline, const Vector<LRTGeometry::Trunk> &p_trunks, bool p_reset) {
	int64_t count = 0;
	if (p_reset) {
		count = int64_t(p_baseline.grid.x) * p_baseline.grid.y * p_baseline.grid.z;
	} else {
		for (const LRTGeometry::Trunk &trunk : p_trunks) {
			count += trunk.size.x * trunk.size.y * trunk.size.z;
		}
	}
	PackedByteArray updates;
	if (count > INT32_MAX / UPDATE_STRIDE || updates.resize(count * UPDATE_STRIDE) != OK) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTRenderer::_fail).bind(uint64_t(p_volume), "LRT GPU upload staging exceeds available memory."));
		return;
	}
	uint8_t *write = updates.ptrw();
	if (p_reset) {
		for (int64_t probe = 0; probe < count; probe++) {
			pack_probe(write + probe * UPDATE_STRIDE, probe, p_baseline.ltm.ptr() + probe * 36,
					p_baseline.local_vis.ptr() + probe * 8, p_baseline.edge_masks.ptr() + probe * 26,
					p_baseline.emission.is_empty() ? nullptr : p_baseline.emission.ptr() + probe * 12);
		}
	} else {
		int64_t record = 0;
		for (const LRTGeometry::Trunk &trunk : p_trunks) {
			for (int z = 0; z < trunk.size.z; z++) {
				for (int y = 0; y < trunk.size.y; y++) {
					for (int x = 0; x < trunk.size.x; x++) {
						const int source = (z * trunk.size.y + y) * trunk.size.x + x;
						const Vector3i coordinate = trunk.origin + Vector3i(x, y, z);
						const uint32_t probe = (coordinate.z * p_baseline.grid.y + coordinate.y) * p_baseline.grid.x + coordinate.x;
						pack_probe(write + record++ * UPDATE_STRIDE, probe, trunk.ltm.ptr() + source * 36,
								trunk.local_vis.ptr() + source * 8, trunk.edge_masks.ptr() + source * 26,
								trunk.emission.is_empty() ? nullptr : trunk.emission.ptr() + source * 12);
					}
				}
			}
		}
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTRenderer::_publish).bind(uint64_t(p_volume), p_revision, p_baseline.grid, updates, p_reset));
}

void LRTRenderer::_publish(uint64_t p_volume, uint64_t p_revision, const Vector3i &p_grid, const PackedByteArray &p_updates, bool p_reset) {
#ifdef RD_ENABLED
	RD *rd = RD::get_singleton();
	if (!rd) {
		_fail(p_volume, "LRT GPU geometry requires a RenderingDevice renderer.");
		return;
	}
	if (!state) {
		state = memnew(State);
	}
	if (state->pipeline.is_null() || state->propagation_pipeline.is_null() || state->injection_pipeline.is_null() || state->sky_pipeline.is_null()) {
		_fail(p_volume, "LRT geometry, propagation or lighting compute pipeline could not be created.");
		return;
	}
	if (p_reset) {
		_remove(p_volume);
		const uint64_t count = uint64_t(p_grid.x) * p_grid.y * p_grid.z;
		if (count > INT32_MAX / MAX(GEOMETRY_STRIDE, HISTORY_STRIDE)) {
			_fail(p_volume, "LRT GPU probe buffers exceed integer size limits.");
			return;
		}
		State::Volume volume;
		volume.grid = p_grid;
		volume.geometry = rd->storage_buffer_create(count * GEOMETRY_STRIDE);
		volume.emission = rd->storage_buffer_create(count * EMISSION_STRIDE);
		volume.sources = rd->storage_buffer_create(count * EMISSION_STRIDE);
		volume.history[0] = rd->storage_buffer_create(count * HISTORY_STRIDE);
		volume.history[1] = rd->storage_buffer_create(count * HISTORY_STRIDE);
		if (volume.geometry.is_null() || volume.emission.is_null() || volume.sources.is_null() || volume.history[0].is_null() || volume.history[1].is_null()) {
			volume.clear();
			_fail(p_volume, "LRT GPU probe buffers could not be allocated.");
			return;
		}
		for (int history = 0; history < 2; history++) {
			Vector<RD::Uniform> uniforms;
			const RID buffers[4] = { volume.geometry, volume.sources, volume.history[history], volume.history[1 - history] };
			for (int binding = 0; binding < 4; binding++) {
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding, { buffers[binding] }));
			}
			volume.propagation_sets[history] = rd->uniform_set_create(uniforms, state->propagation_shader.version_get_shader(state->propagation_version, 0), 0);
			if (volume.propagation_sets[history].is_null()) {
				volume.clear();
				_fail(p_volume, "LRT propagation uniform set could not be created.");
				return;
			}
		}
		state->volumes[p_volume] = volume;
	}
	State::Volume *volume = state->volumes.getptr(p_volume);
	if (!volume) {
		// A failed upload requires a full baseline reset, not a partial recovery.
		if (get_status(ObjectID(p_volume)).get("state", "") != "blocked") {
			_fail(p_volume, "LRT GPU geometry has no initialized baseline.");
		}
		return;
	}
	RID staging = rd->storage_buffer_create(p_updates.size(), p_updates);
	if (staging.is_null()) {
		_fail(p_volume, "LRT GPU upload staging buffer could not be allocated.");
		return;
	}
	Vector<RD::Uniform> uniforms;
	const RID buffers[5] = { staging, volume->geometry, volume->emission, volume->history[0], volume->history[1] };
	for (int binding = 0; binding < 5; binding++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding, { buffers[binding] }));
	}
	RID uniform_set = rd->uniform_set_create(uniforms, state->shader.version_get_shader(state->shader_version, 0), 0);
	if (uniform_set.is_null()) {
		rd->free_rid(staging);
		_fail(p_volume, "LRT geometry upload uniform set could not be created.");
		return;
	}
	const uint32_t count = p_updates.size() / UPDATE_STRIDE;
	const uint32_t max_threads = rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X) * 64;
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, state->pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	for (uint32_t offset = 0; offset < count;) {
		struct {
			uint32_t offset;
			uint32_t end;
			uint32_t padding[2] = {};
		} params;
		params.offset = offset;
		params.end = MIN(count, offset + max_threads);
		rd->compute_list_set_push_constant(list, &params, sizeof(params));
		rd->compute_list_dispatch(list, (params.end - offset + 63) / 64, 1, 1);
		offset = params.end;
	}
	rd->compute_list_end();
	// RenderingDevice retires these resources after their queued GPU use.
	rd->free_rid(uniform_set);
	rd->free_rid(staging);
	volume->revision = p_revision;
	Dictionary status;
	status["state"] = "ready";
	status["revision"] = p_revision;
	status["grid_size"] = p_grid;
	status["updated_probes"] = count;
	status["allocated_bytes"] = uint64_t(p_grid.x) * p_grid.y * p_grid.z * (GEOMETRY_STRIDE + 2 * EMISSION_STRIDE + 2 * HISTORY_STRIDE);
	MutexLock lock(status_mutex);
	statuses[ObjectID(p_volume)] = status;
#else
	_fail(p_volume, "LRT GPU geometry requires a RenderingDevice build.");
#endif
}

void LRTRenderer::prepare(const Array &p_volumes, int p_iterations, double p_relaxation, uint64_t p_frame) {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTRenderer::_prepare).bind(p_volumes, p_iterations, p_relaxation, p_frame));
}

void LRTRenderer::_prepare(const Array &p_volumes, int p_iterations, double p_relaxation, uint64_t p_frame) {
#ifdef RD_ENABLED
	if (!state && p_volumes.is_empty()) {
		return;
	}
	if (!RD::get_singleton() || RendererSceneRenderRD::get_singleton()->get_name() != "forward_clustered") {
		for (const Variant &item : p_volumes) {
			Dictionary description = item;
			uint64_t id = description["id"];
			if (get_status(ObjectID(id)).get("state", "") != "blocked") {
				_fail(id, "LRT lighting and surface sampling require the Forward+ renderer.");
			}
		}
		return;
	}
	if (!state) {
		state = memnew(State);
	}
	state->descriptions = p_volumes;
	state->iterations = p_iterations;
	state->relaxation = p_relaxation;
	state->frame = p_frame;
#endif
}

bool LRTRenderer::_propagate(const PackedInt64Array &p_volumes, int p_iterations, double p_relaxation, uint64_t p_frame) {
#ifdef RD_ENABLED
	if (!state) {
		return false;
	}
	RD *rd = RD::get_singleton();
	for (int64_t id : p_volumes) {
		State::Volume *volume = state->volumes.getptr(uint64_t(id));
		if (!volume) {
			continue;
		}
		const uint32_t count = volume->grid.x * volume->grid.y * volume->grid.z;
		const uint32_t max_threads = rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X) * 64;
		struct {
			uint32_t grid[3];
			uint32_t offset;
			uint32_t end;
			uint32_t phase;
			float relaxation;
			uint32_t padding = 0;
		} params;
		for (int axis = 0; axis < 3; axis++) {
			params.grid[axis] = volume->grid[axis];
		}
		params.relaxation = p_relaxation;
		for (params.phase = 0; params.phase < 2; params.phase++) {
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, state->propagation_pipeline);
			for (int iteration = 0; iteration < p_iterations; iteration++) {
				if (iteration != 0) {
					rd->compute_list_add_barrier(list);
				}
				rd->compute_list_bind_uniform_set(list, volume->propagation_sets[volume->history_index], 0);
				for (uint32_t offset = 0; offset < count;) {
					params.offset = offset;
					params.end = MIN(count, offset + max_threads);
					rd->compute_list_set_push_constant(list, &params, sizeof(params));
					rd->compute_list_dispatch(list, (params.end - offset + 63) / 64, 1, 1);
					offset = params.end;
				}
				volume->history_index ^= 1;
			}
			rd->compute_list_end();
			if (params.phase == 0) {
				if (!_inject(uint64_t(id), state->injection_lights, state->injection_light_count, state->injection_sky, state->injection_shadow, state->injection_directional)) {
					return false;
				}
			}
		}
		volume->propagation_steps += uint64_t(p_iterations) * 2;
		volume->propagation_frame = p_frame;
		MutexLock lock(status_mutex);
		Dictionary &status = statuses[ObjectID(uint64_t(id))];
		status["propagation_frame"] = p_frame;
		status["propagation_steps"] = volume->propagation_steps;
		status["propagation_iterations"] = p_iterations;
		status["propagation_relaxation"] = p_relaxation;
		status["history_index"] = volume->history_index;
	}
	return true;
#else
	return false;
#endif
}

bool LRTRenderer::has_active_volumes() const {
#ifdef RD_ENABLED
	return state && !state->descriptions.is_empty() && !state->volumes.is_empty();
#else
	return false;
#endif
}

void LRTRenderer::set_scene(RID p_scenario, const Vector<RID> &p_lights) {
#ifdef RD_ENABLED
	if (state) {
		state->scenario = p_scenario;
		state->scene_lights = p_lights;
	}
#endif
}

RID LRTRenderer::get_draw_metadata() {
#ifdef RD_ENABLED
	if (!state) {
		state = memnew(State);
	}
	State::Draw *draw = state->draws.getptr(state->scenario);
	return draw && draw->frame == state->frame ? draw->metadata : state->default_metadata;
#else
	return RID();
#endif
}

RID LRTRenderer::get_draw_samples() {
#ifdef RD_ENABLED
	if (!state) {
		state = memnew(State);
	}
	State::Draw *draw = state->draws.getptr(state->scenario);
	return draw && draw->frame == state->frame ? draw->samples : state->default_samples;
#else
	return RID();
#endif
}

bool LRTRenderer::_inject(uint64_t p_volume, RID p_lights, uint32_t p_light_count, RID p_sky, RID p_shadow, RID p_directional) {
#ifdef RD_ENABLED
	State::Volume &volume = state->volumes[p_volume];
	Dictionary description;
	for (const Variant &item : state->descriptions) {
		Dictionary candidate = item;
		if (uint64_t(candidate["id"]) == p_volume) {
			description = candidate;
			break;
		}
	}
	Transform3D transform = description["transform"];
	Vector3 size = description["size"];
	struct {
		float to_world[16];
		float to_local[16];
		float size_spacing[4];
		uint32_t grid_lights[4];
	} data;
	RendererRD::MaterialStorage::store_transform(transform, data.to_world);
	RendererRD::MaterialStorage::store_transform(transform.affine_inverse(), data.to_local);
	for (int axis = 0; axis < 3; axis++) {
		data.size_spacing[axis] = size[axis];
		data.grid_lights[axis] = volume.grid[axis];
	}
	data.size_spacing[3] = description["spacing"];
	data.grid_lights[3] = p_light_count;
	RD *rd = RD::get_singleton();
	RID parameters = rd->uniform_buffer_create(sizeof(data));
	if (parameters.is_null()) {
		return false;
	}
	rd->buffer_update(parameters, 0, sizeof(data), &data);
	Vector<RD::Uniform> uniforms;
	const RID buffers[] = { volume.geometry, volume.emission, volume.history[volume.history_index], volume.sources, p_lights, p_sky };
	for (int binding = 0; binding < 6; binding++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding, { buffers[binding] }));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, { state->shadow_sampler, p_shadow }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, { state->shadow_sampler, p_directional }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 8, { parameters }));
	RID uniform_set = rd->uniform_set_create(uniforms, state->injection_shader.version_get_shader(state->injection_version, 0), 0);
	if (uniform_set.is_null()) {
		rd->free_rid(parameters);
		return false;
	}
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, state->injection_pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	const uint32_t count = volume.grid.x * volume.grid.y * volume.grid.z;
	const uint32_t max_threads = rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X) * 64;
	for (uint32_t offset = 0; offset < count;) {
		uint32_t params[4] = { offset, MIN(count, offset + max_threads), 0, 0 };
		rd->compute_list_set_push_constant(list, params, sizeof(params));
		rd->compute_list_dispatch(list, (params[1] - offset + 63) / 64, 1, 1);
		offset = params[1];
	}
	rd->compute_list_end();
	rd->free_rid(uniform_set);
	rd->free_rid(parameters);
	return true;
#else
	return false;
#endif
}

// Godot-private lighting and render-pass adaptation is confined to this entry point.
void LRTRenderer::render(RenderDataRD *p_render_data) {
#ifdef RD_ENABLED
	if (!state || state->scenario.is_null() || state->descriptions.is_empty()) {
		if (state) {
			State::Draw *old = state->draws.getptr(state->scenario);
			if (old) {
				old->clear();
				state->draws.erase(state->scenario);
			}
		}
		return;
	}
	PackedInt64Array ids;
	Array descriptions;
	for (const Variant &item : state->descriptions) {
		Dictionary description = item;
		uint64_t id = description["id"];
		if (RID(description["scenario"]) == state->scenario && state->volumes.has(id)) {
			ids.push_back(id);
			descriptions.push_back(description);
		}
	}
	if (ids.is_empty()) {
		State::Draw *old = state->draws.getptr(state->scenario);
		if (old) {
			old->clear();
			state->draws.erase(state->scenario);
		}
		return;
	}
	if (state->iterations < 1 || !Math::is_finite(state->relaxation) || state->relaxation <= 0 || state->relaxation > 1) {
		for (int64_t id : ids) {
			_fail(id, "LRT propagation requires positive iterations and relaxation in (0, 1].");
		}
		return;
	}
	if (bool(GLOBAL_GET("rendering/lrt/screen_gather_enabled"))) {
		for (int64_t id : ids) {
			_fail(id, "LRT Screen Gather is not implemented. Disable rendering/lrt/screen_gather_enabled to use direct Probe sampling.");
		}
		return;
	}
	State::Draw &draw = state->draws[state->scenario];
	if (draw.frame == state->frame) {
		return;
	}
	RD *rd = RD::get_singleton();
	auto *storage = RendererRD::LightStorage::get_singleton();
	auto *textures = RendererRD::TextureStorage::get_singleton();
	auto *scene = RendererSceneRenderRD::get_singleton();
	struct Light {
		float position_type[4] = {};
		float direction_range[4] = {};
		float color_attenuation[4] = {};
		float spot_shadow[4] = {};
		float matrices[4][16] = {};
		float rects[4][4] = {};
		float bias[4] = {};
		float omni_offset[4] = {};
	};
	Vector<Light> lights;
	for (RID instance : state->scene_lights) {
		RID base = storage->light_instance_get_base_light(instance);
		RSE::LightType type = storage->light_get_type(base);
		if (storage->light_get_bake_mode(base) == RSE::LIGHT_BAKE_DISABLED || type == RSE::LIGHT_AREA) {
			continue;
		}
		if (type == RSE::LIGHT_DIRECTIONAL && storage->light_directional_get_sky_mode(base) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			continue;
		}
		bool shadows = storage->light_has_shadow(base);
		bool visible = false;
		for (uint32_t light_index = 0; light_index < p_render_data->lights->size(); light_index++) {
			if ((*p_render_data->lights)[light_index] == instance) {
				visible = true;
				break;
			}
		}
		bool positional_shadow = shadows && visible && storage->owns_shadow_atlas(p_render_data->shadow_atlas) && storage->shadow_atlas_owns_light_instance(p_render_data->shadow_atlas, instance);
		// Shadowed positional lights are injected only while their current shadow data is available.
		if (shadows && type != RSE::LIGHT_DIRECTIONAL && !positional_shadow) {
			continue;
		}
		Light light;
		Transform3D transform = storage->light_instance_get_base_transform(instance);
		Vector3 direction = -transform.basis.get_column(2).normalized();
		for (int axis = 0; axis < 3; axis++) {
			light.position_type[axis] = transform.origin[axis];
			light.direction_range[axis] = direction[axis];
		}
		light.position_type[3] = type;
		light.direction_range[3] = MAX(0.001f, storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE));
		float energy = storage->light_get_param(base, RSE::LIGHT_PARAM_ENERGY) * storage->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		if (storage->light_is_negative(base)) {
			energy = -energy;
		}
		if (scene->is_using_physical_light_units()) {
			energy *= storage->light_get_param(base, RSE::LIGHT_PARAM_INTENSITY);
			if (type == RSE::LIGHT_OMNI) {
				energy /= 4.0 * Math::PI;
			}
			if (type == RSE::LIGHT_SPOT) {
				energy /= Math::PI;
			}
		} else {
			energy *= Math::PI;
		}
		Color color = storage->light_get_color(base).srgb_to_linear();
		for (int c = 0; c < 3; c++) {
			light.color_attenuation[c] = color[c] * energy;
		}
		light.color_attenuation[3] = storage->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
		light.spot_shadow[0] = Math::cos(Math::deg_to_rad(storage->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE)));
		light.spot_shadow[1] = 1.0f / storage->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION);
		light.spot_shadow[3] = storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_OPACITY);
		Projection bias;
		bias.set_light_bias();
		Projection correction;
		correction.set_depth_correction(false, true, false);
		if (shadows && type == RSE::LIGHT_DIRECTIONAL) {
			auto mode = storage->light_directional_get_shadow_mode(base);
			int splits = 4;
			if (mode == RSE::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL) {
				splits = 1;
			} else if (mode == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS) {
				splits = 2;
			}
			light.spot_shadow[2] = splits;
			for (int split = 0; split < splits; split++) {
				Rect2 rect = storage->light_instance_get_directional_shadow_atlas_rect(instance, split);
				Projection atlas;
				atlas.set_light_atlas_rect(rect);
				Projection matrix = atlas * bias * correction * storage->light_instance_get_shadow_camera(instance, split) * Projection(storage->light_instance_get_shadow_transform(instance, split).affine_inverse());
				RendererRD::MaterialStorage::store_camera(matrix, light.matrices[split]);
				light.rects[split][0] = rect.position.x;
				light.rects[split][1] = rect.position.y;
				light.rects[split][2] = rect.size.x;
				light.rects[split][3] = rect.size.y;
				light.bias[split] = storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_BIAS) * 0.01f * storage->light_instance_get_shadow_bias_scale(instance, split);
			}
		} else if (positional_shadow) {
			light.spot_shadow[2] = 1;
			Vector2i offset;
			Rect2 rect = storage->light_instance_get_shadow_atlas_rect(instance, p_render_data->shadow_atlas, offset);
			light.rects[0][0] = rect.position.x;
			light.rects[0][1] = rect.position.y;
			light.rects[0][2] = rect.size.x;
			light.rects[0][3] = rect.size.y;
			light.omni_offset[0] = offset.x * rect.size.x;
			light.omni_offset[1] = offset.y * rect.size.y;
			light.bias[0] = storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_BIAS);
			if (type == RSE::LIGHT_OMNI) {
				RendererRD::MaterialStorage::store_transform(transform.affine_inverse(), light.matrices[0]);
			} else {
				light.bias[0] *= 0.01f;
				Projection matrix = bias * correction * storage->light_instance_get_shadow_camera(instance, 0) * Projection(transform.affine_inverse());
				RendererRD::MaterialStorage::store_camera(matrix, light.matrices[0]);
			}
		}
		lights.push_back(light);
	}
	PackedByteArray bytes;
	bytes.resize(MAX(int(sizeof(Light)), lights.size() * int(sizeof(Light))));
	if (!lights.is_empty()) {
		memcpy(bytes.ptrw(), lights.ptr(), lights.size() * sizeof(Light));
	}
	RID light_buffer = rd->storage_buffer_create(bytes.size(), bytes);
	if (light_buffer.is_null()) {
		for (int64_t id : ids) {
			_fail(id, "LRT light input allocation failed.");
		}
		return;
	}
	if (draw.sky.is_null()) {
		draw.sky = rd->storage_buffer_create(48);
	}
	struct {
		float orientation[16];
		float ambient[4] = {};
		float energy[4] = {};
	} sky_params;
	Transform3D sky_transform;
	RID radiance;
	bool array = scene->is_using_radiance_octmap_array();
	if (p_render_data->environment.is_valid()) {
		RID env = p_render_data->environment;
		auto source = scene->environment_get_ambient_source(env);
		auto background = scene->environment_get_background(env);
		bool use_sky = source == RSE::ENV_AMBIENT_SOURCE_SKY || (source == RSE::ENV_AMBIENT_SOURCE_BG && background == RSE::ENV_BG_SKY);
		Color ambient(0, 0, 0);
		if (source == RSE::ENV_AMBIENT_SOURCE_COLOR || use_sky) {
			ambient = scene->environment_get_ambient_light(env).srgb_to_linear() * scene->environment_get_ambient_light_energy(env);
		}
		if (source == RSE::ENV_AMBIENT_SOURCE_BG && (background == RSE::ENV_BG_COLOR || background == RSE::ENV_BG_CLEAR_COLOR)) {
			Color color = background == RSE::ENV_BG_COLOR ? scene->environment_get_bg_color(env) : Color(GLOBAL_GET("rendering/environment/defaults/default_clear_color"));
			ambient = color.srgb_to_linear() * scene->environment_get_bg_energy_multiplier(env);
		}
		for (int c = 0; c < 3; c++) {
			sky_params.ambient[c] = ambient[c];
		}
		if (use_sky) {
			RID sky = scene->environment_get_sky(env);
			if (sky.is_valid()) {
				radiance = scene->get_sky()->sky_get_radiance_texture_rd(sky);
				float energy = scene->environment_get_bg_energy_multiplier(env) / scene->get_sky()->sky_get_baked_exposure(sky);
				for (int c = 0; c < 3; c++) {
					sky_params.energy[c] = energy;
				}
				sky_params.energy[3] = scene->environment_get_ambient_sky_contribution(env);
				sky_transform.basis = scene->environment_get_sky_orientation(env).inverse();
			}
		}
	}
	RendererRD::MaterialStorage::store_transform(sky_transform, sky_params.orientation);
	sky_params.ambient[3] = p_render_data->scene_data->radiance_border_size;
	if (radiance.is_null()) {
		radiance = textures->texture_rd_get_default(array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	Vector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { sampler, radiance }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { draw.sky }));
	RID sky_set = rd->uniform_set_create(uniforms, state->sky_shader.version_get_shader(state->sky_version, 0), 0);
	if (sky_set.is_null()) {
		rd->free_rid(light_buffer);
		for (int64_t id : ids) {
			_fail(id, "LRT sky projection resources could not be bound.");
		}
		return;
	}
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, state->sky_pipeline);
	rd->compute_list_bind_uniform_set(list, sky_set, 0);
	rd->compute_list_set_push_constant(list, &sky_params, sizeof(sky_params));
	rd->compute_list_dispatch(list, 1, 1, 1);
	rd->compute_list_end();
	rd->free_rid(sky_set);
	state->injection_lights = light_buffer;
	state->injection_light_count = lights.size();
	state->injection_sky = draw.sky;
	RID shadow = storage->owns_shadow_atlas(p_render_data->shadow_atlas) ? storage->shadow_atlas_get_texture(p_render_data->shadow_atlas) : RID();
	state->injection_shadow = shadow.is_valid() ? shadow : textures->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
	RID directional = storage->directional_shadow_get_texture();
	state->injection_directional = directional.is_valid() ? directional : textures->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
	bool propagated = _propagate(ids, state->iterations, state->relaxation, state->frame);
	rd->free_rid(light_buffer);
	if (!propagated) {
		for (int64_t id : ids) {
			_fail(id, "LRT light injection resources could not be bound.");
		}
		return;
	}

	struct Metadata {
		float to_local[16];
		uint32_t grid_geometry[4];
		float size_spacing[4];
		float display[4] = {};
		uint32_t history[4] = {};
	};
	Vector<Metadata> entries;
	uint64_t sample_bytes = 0;
	for (int i = 0; i < ids.size(); i++) {
		const State::Volume *volume = state->volumes.getptr(ids[i]);
		if (!volume) {
			continue;
		}
		Dictionary description = descriptions[i];
		Metadata item;
		Transform3D transform = description["transform"];
		RendererRD::MaterialStorage::store_transform(transform.affine_inverse(), item.to_local);
		Vector3 size = description["size"];
		for (int axis = 0; axis < 3; axis++) {
			item.grid_geometry[axis] = volume->grid[axis];
			item.size_spacing[axis] = size[axis];
		}
		item.size_spacing[3] = description["spacing"];
		item.display[0] = description["intensity"];
		item.display[1] = description["blend"];
		uint64_t count = uint64_t(volume->grid.x) * volume->grid.y * volume->grid.z;
		item.grid_geometry[3] = sample_bytes / 4;
		sample_bytes += count * GEOMETRY_STRIDE;
		item.history[0] = sample_bytes / 4;
		sample_bytes += count * HISTORY_STRIDE;
		entries.push_back(item);
	}
	if (sample_bytes > INT32_MAX) {
		for (int64_t id : ids) {
			_fail(id, "LRT surface sampling buffer exceeds integer size limits.");
		}
		return;
	}
	uint32_t metadata_bytes = 64 + entries.size() * sizeof(Metadata);
	if (draw.metadata_bytes != metadata_bytes || draw.sample_bytes != sample_bytes) {
		if (draw.metadata.is_valid()) {
			rd->free_rid(draw.metadata);
		}
		if (draw.samples.is_valid()) {
			rd->free_rid(draw.samples);
		}
		draw.metadata_bytes = metadata_bytes;
		draw.sample_bytes = MAX(uint32_t(sample_bytes), 16u);
		draw.metadata = rd->storage_buffer_create(metadata_bytes);
		draw.samples = rd->storage_buffer_create(draw.sample_bytes);
	}
	if (draw.metadata.is_null() || draw.samples.is_null()) {
		for (int64_t id : ids) {
			_fail(id, "LRT surface sampling buffers could not be allocated.");
		}
		return;
	}
	bytes.resize(metadata_bytes);
	bytes.fill(0);
	encode_uint32(entries.size(), bytes.ptrw());
	if (!entries.is_empty()) {
		memcpy(bytes.ptrw() + 64, entries.ptr(), entries.size() * sizeof(Metadata));
	}
	rd->buffer_update(draw.metadata, 0, bytes.size(), bytes.ptr());
	rd->buffer_copy(draw.sky, draw.metadata, 0, 16, 48);
	for (int i = 0, index = 0; i < ids.size(); i++) {
		const State::Volume *volume = state->volumes.getptr(ids[i]);
		if (!volume) {
			continue;
		}
		const Metadata &item = entries[index++];
		uint32_t count = volume->grid.x * volume->grid.y * volume->grid.z;
		rd->buffer_copy(volume->geometry, draw.samples, 0, item.grid_geometry[3] * 4, count * GEOMETRY_STRIDE);
		rd->buffer_copy(volume->history[volume->history_index], draw.samples, 0, item.history[0] * 4, count * HISTORY_STRIDE);
		MutexLock lock(status_mutex);
		Dictionary &status = statuses[ObjectID(uint64_t(ids[i]))];
		status["injected_lights"] = lights.size();
		status["surface_sampling"] = true;
	}
	draw.frame = state->frame;
#endif
}

void LRTRenderer::_remove(uint64_t p_volume) {
#ifdef RD_ENABLED
	if (state) {
		for (const Variant &item : state->descriptions) {
			Dictionary description = item;
			if (uint64_t(description["id"]) != p_volume) {
				continue;
			}
			RID scenario = description["scenario"];
			State::Draw *draw = state->draws.getptr(scenario);
			if (draw) {
				draw->clear();
				state->draws.erase(scenario);
			}
			break;
		}
		State::Volume *volume = state->volumes.getptr(p_volume);
		if (volume) {
			volume->clear();
			state->volumes.erase(p_volume);
		}
	}
#endif
	MutexLock lock(status_mutex);
	statuses.erase(ObjectID(p_volume));
}

void LRTRenderer::_fail(uint64_t p_volume, const String &p_error) {
	_remove(p_volume);
	Dictionary status;
	status["state"] = "blocked";
	status["error"] = p_error;
	{
		MutexLock lock(status_mutex);
		statuses[ObjectID(p_volume)] = status;
	}
	ERR_PRINT(p_error);
}

void LRTRenderer::remove(ObjectID p_volume) {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTRenderer::_remove).bind(uint64_t(p_volume)));
}

Dictionary LRTRenderer::get_status(ObjectID p_volume) const {
	MutexLock lock(status_mutex);
	const Dictionary *status = statuses.getptr(p_volume);
	if (status) {
		return status->duplicate();
	}
	Dictionary result;
	result["state"] = "pending";
	return result;
}

void LRTRenderer::_readback(uint64_t p_volume, const Vector3i &p_coordinate) {
	readback_result = Dictionary();
#ifdef RD_ENABLED
	const State::Volume *volume = state ? state->volumes.getptr(p_volume) : nullptr;
	if (!volume) {
		readback_result["error"] = "LRT GPU geometry is not available.";
		return;
	}
	if (p_coordinate.x < 0 || p_coordinate.y < 0 || p_coordinate.z < 0 ||
			p_coordinate.x >= (volume->grid.x + 7) / 8 || p_coordinate.y >= (volume->grid.y + 7) / 8 || p_coordinate.z >= (volume->grid.z + 7) / 8) {
		readback_result["error"] = "LRT GPU trunk coordinate is out of bounds.";
		return;
	}
	RD *rd = RD::get_singleton();
	const Vector<uint8_t> geometry = rd->buffer_get_data(volume->geometry);
	const Vector<uint8_t> source_emission = rd->buffer_get_data(volume->emission);
	const Vector<uint8_t> injected = volume->propagation_steps > 0 ? rd->buffer_get_data(volume->sources) : Vector<uint8_t>();
	const Vector<uint8_t> histories[2] = { rd->buffer_get_data(volume->history[0]), rd->buffer_get_data(volume->history[1]) };
	if (geometry.is_empty() || source_emission.is_empty() || histories[0].is_empty() || histories[1].is_empty()) {
		readback_result["error"] = "LRT GPU buffer readback failed.";
		return;
	}
	const Vector3i origin = p_coordinate * LRTGeometry::TRUNK_SIZE;
	const Vector3i size = (volume->grid - origin).min(Vector3i(8, 8, 8));
	const int count = size.x * size.y * size.z;
	PackedByteArray ltm, vis, masks;
	PackedFloat32Array emission, sources, light[2], visibility[2];
	if (!injected.is_empty()) {
		sources.resize(count * 12);
	}
	if (ltm.resize(count * 36) != OK || vis.resize(count * 8) != OK || masks.resize(count * 26) != OK || emission.resize(count * 12) != OK) {
		readback_result["error"] = "LRT GPU readback could not allocate CPU channels.";
		return;
	}
	for (int history = 0; history < 2; history++) {
		if (light[history].resize(count * 12) != OK || visibility[history].resize(count * 4) != OK) {
			readback_result["error"] = "LRT GPU readback could not allocate CPU history.";
			return;
		}
	}
	for (int z = 0; z < size.z; z++) {
		for (int y = 0; y < size.y; y++) {
			for (int x = 0; x < size.x; x++) {
				const int target = (z * size.y + y) * size.x + x;
				const Vector3i point = origin + Vector3i(x, y, z);
				const int64_t source = (int64_t(point.z) * volume->grid.y + point.y) * volume->grid.x + point.x;
				const uint8_t *record = geometry.ptr() + source * GEOMETRY_STRIDE;
				memcpy(ltm.ptrw() + target * 36, record, 36);
				memcpy(vis.ptrw() + target * 8, record + 36, 8);
				memcpy(masks.ptrw() + target * 26, record + 44, 26);
				memcpy(emission.ptrw() + target * 12, source_emission.ptr() + source * EMISSION_STRIDE, EMISSION_STRIDE);
				if (!injected.is_empty()) {
					memcpy(sources.ptrw() + target * 12, injected.ptr() + source * EMISSION_STRIDE, EMISSION_STRIDE);
				}
				for (int history = 0; history < 2; history++) {
					const uint8_t *probe_state = histories[history].ptr() + source * HISTORY_STRIDE;
					memcpy(light[history].ptrw() + target * 12, probe_state, 48);
					memcpy(visibility[history].ptrw() + target * 4, probe_state + 48, 16);
				}
			}
		}
	}
	readback_result["coordinate"] = p_coordinate;
	readback_result["origin"] = origin;
	readback_result["size"] = size;
	readback_result["revision"] = volume->revision;
	readback_result["history_index"] = volume->history_index;
	readback_result["propagation_frame"] = volume->propagation_frame;
	readback_result["propagation_steps"] = volume->propagation_steps;
	readback_result["ltm"] = ltm;
	readback_result["local_vis"] = vis;
	readback_result["edge_masks"] = masks;
	readback_result["emission"] = emission;
	readback_result["source"] = sources;
	for (const Variant &item : state->descriptions) {
		Dictionary description = item;
		if (uint64_t(description["id"]) != p_volume) {
			continue;
		}
		const State::Draw *draw = state->draws.getptr(RID(description["scenario"]));
		if (draw && draw->sky.is_valid()) {
			Vector<uint8_t> bytes = rd->buffer_get_data(draw->sky);
			if (bytes.size() == 48) {
				PackedFloat32Array sky;
				sky.resize(12);
				memcpy(sky.ptrw(), bytes.ptr(), 48);
				readback_result["sky"] = sky;
			}
		}
		break;
	}
	readback_result["light_0"] = light[0];
	readback_result["light_1"] = light[1];
	readback_result["visibility_0"] = visibility[0];
	readback_result["visibility_1"] = visibility[1];
#else
	readback_result["error"] = "LRT GPU geometry requires a RenderingDevice build.";
#endif
}

Dictionary LRTRenderer::readback(ObjectID p_volume, const Vector3i &p_coordinate) {
	if (!Thread::is_main_thread()) {
		Dictionary error;
		error["error"] = "LRT GPU readback must be requested on the main thread.";
		return error;
	}
	RenderingServer *server = RenderingServer::get_singleton();
	server->call_on_render_thread(callable_mp(this, &LRTRenderer::_readback).bind(uint64_t(p_volume), p_coordinate));
	server->sync();
	return readback_result;
}

void LRTRenderer::_clear() {
	if (state) {
		memdelete(state);
		state = nullptr;
	}
	MutexLock lock(status_mutex);
	statuses.clear();
}

LRTRenderer::~LRTRenderer() {
	RenderingServer *server = RenderingServer::get_singleton();
	server->call_on_render_thread(callable_mp(this, &LRTRenderer::_clear));
	server->sync();
	singleton = nullptr;
}
