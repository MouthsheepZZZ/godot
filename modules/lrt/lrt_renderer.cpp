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

#include "local_lrt_volume_3d.h"
#include "lrt_geometry_binding.h"

#include "core/config/project_settings.h"
#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "servers/rendering/rendering_server.h"

#ifdef RD_ENABLED
#include "shaders/lrt_debug.glsl.gen.h"
#include "shaders/lrt_gather.glsl.gen.h"
#include "shaders/lrt_geometry_upload.glsl.gen.h"
#include "shaders/lrt_inject.glsl.gen.h"
#include "shaders/lrt_propagate.glsl.gen.h"
#include "shaders/lrt_sky.glsl.gen.h"

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/camera_attributes_storage.h"
#endif

LRTRenderer *LRTRenderer::singleton = nullptr;

LRTRenderer::LRTRenderer() {
	singleton = this;
}

namespace {

constexpr uint32_t GEOMETRY_STRIDE = 72;
constexpr uint32_t EMISSION_STRIDE = 48;
constexpr uint32_t HISTORY_STRIDE = 64;
constexpr uint32_t SAMPLE_FLAGS_STRIDE = 4;
constexpr uint32_t UPDATE_STRIDE = 124;

#ifdef RD_ENABLED
constexpr uint32_t PROPAGATION_TILE_DEPTH[2] = { 3, 8 };
static_assert(int(LRTRenderer::POSITIONAL_SHADOW_PENDING) == int(RendererSceneRender::LRTPositionalShadowResult::PENDING));
static_assert(int(LRTRenderer::POSITIONAL_SHADOW_WAITING) == int(RendererSceneRender::LRTPositionalShadowResult::WAITING));
static_assert(int(LRTRenderer::POSITIONAL_SHADOW_FAILED) == int(RendererSceneRender::LRTPositionalShadowResult::FAILED));
static_assert(int(LRTRenderer::POSITIONAL_SHADOW_SUBMITTED) == int(RendererSceneRender::LRTPositionalShadowResult::SUBMITTED));

// Native visual profiling uses adjacent timestamps and >/< categories.
// Always close the category, including on early returns, and resume its parent.
class LRTProfileScope {
	const String label;
	const String resume;

public:
	LRTProfileScope(const String &p_label, const String &p_resume) :
			label(p_label), resume(p_resume) {
		RD::get_singleton()->capture_timestamp(">" + label);
	}

	~LRTProfileScope() {
		RD::get_singleton()->capture_timestamp("<" + label);
		RD::get_singleton()->capture_timestamp(resume);
	}

	void stage(const String &p_name) const {
		RD::get_singleton()->capture_timestamp(p_name);
	}
};
#endif

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
		RID static_geometry;
		RID history[2];
		uint32_t history_index[2] = {};
		bool injected = false;
		bool shadow_blocked = false;
		bool recovering = false;
		uint64_t propagation_steps = 0;
		uint64_t propagation_frame = 0;

		void clear() {
			for (RID rid : { geometry, emission, sources, static_geometry, history[0], history[1] }) {
				if (rid.is_valid()) {
					RD::get_singleton()->free_rid(rid);
				}
			}
		}
	};

	HashMap<uint64_t, Volume> volumes;
	HashMap<ObjectID, uint32_t> surface_profiles;
	PackedByteArray surface_profile_data;
	LrtGeometryUploadShaderRD shader;
	RID shader_version;
	RID pipeline;
	LrtPropagateShaderRD propagation_shader;
	RID propagation_version;
	RID propagation_pipeline[2];
	LrtInjectShaderRD injection_shader;
	RID injection_version;
	RID injection_pipeline;
	LrtSkyShaderRD sky_shader;
	RID sky_version;
	RID sky_pipeline;
	LrtGatherShaderRD gather_shader;
	RID gather_version;
	RID gather_pipeline;
	LrtDebugShaderRD debug_shader;
	RID debug_version;
	RID debug_pipeline;
	bool upload_initialized = false;
	bool propagation_initialized = false;
	bool injection_initialized = false;
	bool sky_initialized = false;
	bool gather_initialized = false;
	bool debug_initialized = false;
	Dictionary debug;
	RID sdf_buffer;
	uint64_t sdf_id = 0;
	uint64_t sdf_revision = 0;
	uint64_t profile_frame = 0;
	HashMap<uint64_t, double> gather_timings;
	HashMap<uint64_t, Dictionary> world_timings;
	RID default_metadata;
	RID default_samples;
	RID default_material_bake;
	RID default_profile_texture;
	RID shadow_sampler;
	RID scenario;
	Vector<RID> scene_lights;
	Array descriptions;
	uint64_t frame = 0;
	int iterations = 2;
	RID injection_lights;
	RID injection_sky;
	RID injection_shadow;
	RID injection_directional;
	uint32_t injection_light_count = 0;

	struct SunShadow {
		RID light;
		Rect2i rect;
		Transform3D transform;
		Projection projection;
		Vector3 center;
		float extent = 0;
		float bias = 0;
		bool ready = false;
	};

	struct PositionalLight {
		RID base;
		RID source_instance;
		RID shadow_instance;
		bool shadowed = false;
		RSE::LightType type = RSE::LIGHT_OMNI;
		uint32_t pass_count = 0;
		uint64_t source_revision = 0;
		uint64_t submitted_frame = UINT64_MAX;
		uint64_t finished_frame = UINT64_MAX;
		uint64_t submission_count = 0;
		uint64_t completion_count = 0;
		uint64_t input_version = 0;
		uint64_t submitted_version = 0;
		uint64_t completed_version = 0;
		Vector<uint64_t> input_versions;
		Vector<uint64_t> submitted_versions;
		Vector<uint64_t> completed_versions;
		bool completed_valid = false;
		bool failure_reported = false;
		HashSet<RID> casters;
		RendererSceneRender::LRTPositionalShadowResult result;
		void invalidate() {
			completed_valid = false;
			result = RendererSceneRender::LRTPositionalShadowResult();
		}
	};

	struct Draw {
		RID positional_atlas;
		// Results remain at stable addresses while the renderer consumes queued shadow passes.
		HashMap<RID, PositionalLight *> positional_lights;
		Vector<RID> positional_candidates;
		HashMap<uint64_t, Vector<RID>> positional_by_volume;
		Vector<RID> positional_queue;
		uint64_t positional_frame = UINT64_MAX;
		uint64_t positional_scheduled_frame = UINT64_MAX;
		uint32_t positional_view_count = 0;
		uint32_t positional_schedule_count = 0;
		uint32_t positional_native_passes = 0;
		uint32_t positional_submitted_passes = 0;
		Vector3 positional_reference;
		uint64_t positional_visibility_mask = 0;
		uint32_t positional_atlas_size = 0;
		uint32_t positional_tile_size = 0;
		bool positional_16_bits = true;
		bool positional_capacity_error = false;
		Error positional_error = OK;
		String positional_error_message;
		RID sun_texture;
		RID sun_framebuffer;
		int sun_resolution = 0;
		uint64_t sun_frame = UINT64_MAX;
		Vector<SunShadow> sun_shadows;
		RID metadata;
		RID samples;
		RID sky;
		uint32_t metadata_bytes = 0;
		uint32_t sample_bytes = 0;
		uint64_t frame = 0;
		uint32_t next_phase = 0;
		void clear_sun_texture() {
			if (sun_texture.is_valid()) {
				if (sun_framebuffer.is_valid()) {
					RD::get_singleton()->free_rid(sun_framebuffer);
				}
				RD::get_singleton()->free_rid(sun_texture);
			}
			sun_framebuffer = RID();
			sun_texture = RID();
			sun_resolution = 0;
		}
		void clear_sampling() {
			clear_sun_texture();
			sun_shadows.clear();
			sun_frame = UINT64_MAX;
			for (RID rid : { metadata, samples, sky }) {
				if (rid.is_valid()) {
					RD::get_singleton()->free_rid(rid);
				}
			}
			metadata = RID();
			samples = RID();
			sky = RID();
			metadata_bytes = 0;
			sample_bytes = 0;
		}
		void clear_positional() {
			auto *storage = RendererRD::LightStorage::get_singleton();
			for (KeyValue<RID, PositionalLight *> &entry : positional_lights) {
				if (entry.value->shadow_instance.is_valid()) {
					storage->light_instance_free(entry.value->shadow_instance);
				}
				memdelete(entry.value);
			}
			positional_lights.clear();
			positional_candidates.clear();
			positional_by_volume.clear();
			positional_queue.clear();
			positional_scheduled_frame = UINT64_MAX;
			if (positional_atlas.is_valid()) {
				storage->shadow_atlas_free(positional_atlas);
				positional_atlas = RID();
			}
			positional_frame = UINT64_MAX;
			positional_atlas_size = 0;
			positional_tile_size = 0;
			positional_capacity_error = false;
			positional_error = OK;
			positional_error_message = String();
		}
		void clear() {
			clear_sampling();
			clear_positional();
		}
	};
	HashMap<RID, Draw> draws;

	State() {
		PackedByteArray zeros;
		zeros.resize(64);
		default_metadata = RD::get_singleton()->storage_buffer_create(64, zeros);
		default_samples = RD::get_singleton()->storage_buffer_create(64, zeros);
		default_material_bake = RD::get_singleton()->storage_buffer_create(64, zeros);
		RD::TextureFormat profile_format;
		profile_format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		profile_format.format = RD::DATA_FORMAT_R32_UINT;
		profile_format.width = 1;
		profile_format.height = 1;
		profile_format.array_layers = 1;
		profile_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
		PackedByteArray profile_zero;
		profile_zero.resize(4);
		profile_zero.fill(0);
		default_profile_texture = RD::get_singleton()->texture_create(profile_format, RD::TextureView(), { profile_zero });
	}

	bool init_compute(ShaderRD &p_shader, RID &r_version, RID *r_pipelines, bool &r_initialized, const Vector<String> &p_variants) {
		if (!r_initialized) {
			r_initialized = true;
			p_shader.initialize(p_variants);
			r_version = p_shader.version_create();
			if (r_version.is_null()) {
				return false;
			}
			for (int i = 0; i < p_variants.size(); i++) {
				RID shader_rid = p_shader.version_get_shader(r_version, i);
				if (shader_rid.is_null()) {
					return false;
				}
				r_pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader_rid);
				if (r_pipelines[i].is_null()) {
					return false;
				}
			}
		}
		for (int i = 0; i < p_variants.size(); i++) {
			if (r_pipelines[i].is_null()) {
				return false;
			}
		}
		return true;
	}

	bool init_upload() {
		return pipeline.is_valid() || init_compute(shader, shader_version, &pipeline, upload_initialized, { "" });
	}

	bool init_propagation() {
		if (propagation_pipeline[0].is_valid() && propagation_pipeline[1].is_valid()) {
			return true;
		}
		return init_compute(propagation_shader, propagation_version, propagation_pipeline, propagation_initialized, {
				"#define PHASE_A\n#define LRT_WORKGROUP_DEPTH " + itos(PROPAGATION_TILE_DEPTH[0]) + "\n",
				"#define PHASE_L\n#define LRT_WORKGROUP_DEPTH " + itos(PROPAGATION_TILE_DEPTH[1]) + "\n" });
	}

	bool init_injection() {
		if (injection_pipeline.is_valid()) {
			return true;
		}
		if (shadow_sampler.is_null() && !injection_initialized) {
			RD::SamplerState sampler;
			sampler.min_filter = RD::SAMPLER_FILTER_LINEAR;
			sampler.mag_filter = RD::SAMPLER_FILTER_LINEAR;
			sampler.enable_compare = true;
			sampler.compare_op = RD::COMPARE_OP_GREATER;
			shadow_sampler = RD::get_singleton()->sampler_create(sampler);
			if (shadow_sampler.is_null()) {
				return false;
			}
		}
		return init_compute(injection_shader, injection_version, &injection_pipeline, injection_initialized, { "" });
	}

	bool init_sky() { return sky_pipeline.is_valid() || init_compute(sky_shader, sky_version, &sky_pipeline, sky_initialized, { "" }); }
	bool init_gather() { return gather_pipeline.is_valid() || init_compute(gather_shader, gather_version, &gather_pipeline, gather_initialized, { "" }); }
	bool init_debug() { return debug_pipeline.is_valid() || init_compute(debug_shader, debug_version, &debug_pipeline, debug_initialized, { "" }); }

	~State() {
		if (debug_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(debug_pipeline);
		}
		if (debug_version.is_valid()) {
			debug_shader.version_free(debug_version);
		}
		if (sdf_buffer.is_valid()) {
			RD::get_singleton()->free_rid(sdf_buffer);
		}
		if (gather_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(gather_pipeline);
		}
		if (gather_version.is_valid()) {
			gather_shader.version_free(gather_version);
		}
		for (KeyValue<RID, Draw> &entry : draws) {
			entry.value.clear();
		}
		for (RID rid : { default_metadata, default_samples, default_material_bake, default_profile_texture, shadow_sampler }) {
			if (rid.is_valid()) {
				RD::get_singleton()->free_rid(rid);
			}
		}
		if (injection_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(injection_pipeline);
		}
		if (injection_version.is_valid()) {
			injection_shader.version_free(injection_version);
		}
		if (sky_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(sky_pipeline);
		}
		if (sky_version.is_valid()) {
			sky_shader.version_free(sky_version);
		}
		for (KeyValue<uint64_t, Volume> &entry : volumes) {
			entry.value.clear();
		}
		if (pipeline.is_valid()) {
			RD::get_singleton()->free_rid(pipeline);
		}
		if (shader_version.is_valid()) {
			shader.version_free(shader_version);
		}
		for (RID rid : propagation_pipeline) {
			if (rid.is_valid()) {
				RD::get_singleton()->free_rid(rid);
			}
		}
		if (propagation_version.is_valid()) {
			propagation_shader.version_free(propagation_version);
		}
	}
#endif
};

PackedByteArray LRTRenderer::pack_updates(const LRTGeometry::StaticCache &p_baseline, const Vector<LRTGeometry::Trunk> &p_trunks, bool p_reset) {
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
		return PackedByteArray();
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
	return updates;
}

void LRTRenderer::submit_prepared(ObjectID p_volume, uint64_t p_revision, const Vector3i &p_grid, const PackedByteArray &p_updates, bool p_reset, bool p_baseline_changed) {
	if (p_updates.is_empty()) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTRenderer::_fail).bind(uint64_t(p_volume), "LRT GPU upload staging exceeds available memory."));
		return;
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTRenderer::_publish).bind(uint64_t(p_volume), p_revision, p_grid, p_updates, p_reset, p_baseline_changed));
}

void LRTRenderer::_publish(uint64_t p_volume, uint64_t p_revision, const Vector3i &p_grid, const PackedByteArray &p_updates, bool p_reset, bool p_baseline_changed) {
#ifdef RD_ENABLED
	RD *rd = RD::get_singleton();
	if (!rd) {
		_fail(p_volume, "LRT GPU geometry requires a RenderingDevice renderer.");
		return;
	}
	if (!state) {
		state = memnew(State);
	}
	if (!state->init_upload()) {
		_fail(p_volume, "LRT geometry upload compute pipeline could not be created.");
		return;
	}
	// Uploads can precede Frame Begin; keep the profiler's first entry a leaf.
	rd->capture_timestamp("Render Thread Tasks");
	LRTProfileScope profile("LRT Geometry Upload [" + itos(p_volume) + "]", "Render Thread Tasks");
	profile.stage("Prepare Upload");
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
	if (p_baseline_changed && volume->static_geometry.is_valid()) {
		rd->free_rid(volume->static_geometry);
		volume->static_geometry = RID();
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
	profile.stage("Scatter Geometry");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, state->pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	for (uint32_t offset = 0; offset < count;) {
		struct {
			uint32_t offset;
			uint32_t end;
			uint32_t reset;
			uint32_t padding = 0;
			uint32_t grid[4] = {};
		} params;
		for (int axis = 0; axis < 3; axis++) {
			params.grid[axis] = p_grid[axis];
		}
		params.offset = offset;
		params.end = MIN(count, offset + max_threads);
		params.reset = p_reset;
		rd->compute_list_set_push_constant(list, &params, sizeof(params));
		rd->compute_list_dispatch(list, (params.end - offset + 63) / 64, 1, 1);
		offset = params.end;
	}
	rd->compute_list_end();
	profile.stage("Finalize");
	// RenderingDevice retires these resources after their queued GPU use.
	rd->free_rid(uniform_set);
	rd->free_rid(staging);
	volume->revision = p_revision;
	Dictionary status;
	status["state"] = volume->shadow_blocked || volume->recovering ? "waiting" : "ready";
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

void LRTRenderer::set_debug_selection(ObjectID p_selection) {
	debug_selection = p_selection;
}

uint32_t LRTRenderer::get_surface_profile(ObjectID p_instance) const {
#ifdef RD_ENABLED
	if (state) {
		const uint32_t *profile = state->surface_profiles.getptr(p_instance);
		if (profile) {
			return *profile;
		}
	}
#endif
	return 0;
}

void LRTRenderer::prepare(const Array &p_volumes, int p_iterations, uint64_t p_frame, const Dictionary &p_surface_profiles, const PackedByteArray &p_surface_profile_data) {
	Dictionary debug;
	debug["selection"] = uint64_t(debug_selection);
	debug["volume_selection"] = Object::cast_to<LRTVolume3D>(ObjectDB::get_instance(debug_selection)) != nullptr;
	debug["detail_selection"] = Object::cast_to<LRTDetailVolume3D>(ObjectDB::get_instance(debug_selection)) != nullptr;
	Array sdf_instances;
	uint64_t revision = 0;
	for (const Dictionary &description : p_volumes) {
		if (bool(debug["volume_selection"]) && uint64_t(description["id"]) != uint64_t(debug_selection) &&
				(bool(debug["detail_selection"]) || uint64_t(description["group"]) != uint64_t(debug_selection))) {
			continue;
		}
		for (const Dictionary &entry : Array(description["sdf_instances"])) {
			const uint64_t id = entry["id"];
			const int index = entry["index"];
			revision = hash_murmur3_one_64(uint64_t(entry["revision"]), hash_murmur3_one_64(id, hash_murmur3_one_64(index, revision)));
			Dictionary snapshot = entry.duplicate();
			snapshot["scenario"] = description["scenario"];
			snapshot["volume"] = description["id"];
			sdf_instances.push_back(snapshot);
		}
	}
	debug["sdf_revision"] = revision;
	debug["sdf_instances"] = sdf_instances;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTRenderer::_prepare).bind(p_volumes, p_iterations, p_frame, debug, p_surface_profiles, p_surface_profile_data));
}

void LRTRenderer::_prepare(const Array &p_volumes, int p_iterations, uint64_t p_frame, const Dictionary &p_debug, const Dictionary &p_surface_profiles, const PackedByteArray &p_surface_profile_data) {
#ifdef RD_ENABLED
	if (!state && p_volumes.is_empty() && Array(p_debug.get("sdf_instances", Array())).is_empty()) {
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
	state->surface_profiles.clear();
	for (const Variant &id : p_surface_profiles.get_key_list()) {
		state->surface_profiles[ObjectID(uint64_t(int64_t(id)))] = uint32_t(p_surface_profiles[id]);
	}
	state->surface_profile_data = p_surface_profile_data;
	state->descriptions = p_volumes;
	HashSet<RID> active_scenarios;
	for (const Dictionary &description : p_volumes) {
		active_scenarios.insert(RID(description["scenario"]));
	}
	Vector<RID> inactive_scenarios;
	for (const KeyValue<RID, State::Draw> &entry : state->draws) {
		if (!active_scenarios.has(entry.key)) {
			inactive_scenarios.push_back(entry.key);
		}
	}
	for (RID scenario : inactive_scenarios) {
		state->draws[scenario].clear();
		state->draws.erase(scenario);
	}
	state->debug = p_debug;
	state->iterations = p_iterations;
	state->frame = p_frame;
#endif
}

bool LRTRenderer::_propagate(const PackedInt64Array &p_volumes, int p_iterations, uint64_t p_frame, uint32_t p_phase, bool p_recovering) {
#ifdef RD_ENABLED
	if (!state) {
		return false;
	}
	RD *rd = RD::get_singleton();
	State::Draw &draw = state->draws[state->scenario];
	for (int volume_index = 0; volume_index < p_volumes.size(); volume_index++) {
		int64_t id = p_volumes[volume_index];
		State::Volume *volume = state->volumes.getptr(uint64_t(id));
		if (!volume || volume->recovering != p_recovering) {
			continue;
		}
		if (!state->init_propagation()) {
			return false;
		}
		const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
		LRTProfileScope profile("LRT Volume [" + itos(id) + "]", "Finalize");
		profile.stage("Prepare Propagation");
		const uint32_t count = volume->grid.x * volume->grid.y * volume->grid.z;
		const uint32_t max_groups = rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X);
		struct {
			uint32_t grid[3];
			uint32_t offset;
			uint32_t end;
			uint32_t other_in_source;
			uint32_t volume_index;
			uint32_t publish_samples;
			float to_world[16];
			float size_spacing[4];
		} params;
		for (int axis = 0; axis < 3; axis++) {
			params.grid[axis] = volume->grid[axis];
		}
		params.volume_index = volume_index;
		Dictionary description;
		for (const Variant &item : state->descriptions) {
			Dictionary candidate = item;
			if (int64_t(candidate["id"]) == id) {
				description = candidate;
				break;
			}
		}
		RendererRD::MaterialStorage::store_transform(description["transform"], params.to_world);
		Vector3 size = description["size"];
		for (int axis = 0; axis < 3; axis++) {
			params.size_spacing[axis] = size[axis];
		}
		params.size_spacing[3] = description["spacing"];
		if (p_phase == 1) {
			profile.stage("Light Injection");
			if (!_inject(uint64_t(id), state->injection_lights, state->injection_light_count, state->injection_sky, state->injection_shadow, state->injection_directional)) {
				return false;
			}
			volume->injected = true;
		}
		RID sets[2];
		for (int history = 0; history < 2; history++) {
			LocalVector<RD::Uniform> uniforms;
			const RID buffers[] = { volume->geometry, volume->sources, volume->history[history], volume->history[1 - history], draw.metadata, draw.samples };
			for (int binding = 0; binding < 6; binding++) {
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding, { buffers[binding] }));
			}
			sets[history] = UniformSetCacheRD::get_singleton()->get_cache_vec(state->propagation_shader.version_get_shader(state->propagation_version, p_phase), 0, uniforms);
			if (sets[history].is_null()) {
				return false;
			}
		}
		const uint32_t tile_depth = PROPAGATION_TILE_DEPTH[p_phase];
		const uint32_t tile_count = ((volume->grid.x + 7) / 8) * ((volume->grid.y + 3) / 4) * ((volume->grid.z + tile_depth - 1) / tile_depth);
		profile.stage(p_phase == 0 ? "Visibility Propagation A" : "Light Propagation L");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, state->propagation_pipeline[p_phase]);
		for (int iteration = 0; iteration < p_iterations; iteration++) {
			params.publish_samples = iteration == p_iterations - 1;
			params.other_in_source = volume->history_index[1 - p_phase] == volume->history_index[p_phase];
			if (iteration != 0) {
				rd->compute_list_add_barrier(list);
			}
			rd->compute_list_bind_uniform_set(list, sets[volume->history_index[p_phase]], 0);
			for (uint32_t offset = 0; offset < tile_count;) {
				params.offset = offset;
				params.end = MIN(tile_count, offset + max_groups);
				rd->compute_list_set_push_constant(list, &params, sizeof(params));
				rd->compute_list_dispatch(list, params.end - offset, 1, 1);
				offset = params.end;
			}
			volume->history_index[p_phase] ^= 1;
		}
		rd->compute_list_end();
		profile.stage("Finalize");
		volume->propagation_steps += uint64_t(p_iterations);
		volume->propagation_frame = p_frame;
		MutexLock lock(status_mutex);
		Dictionary &status = statuses[ObjectID(uint64_t(id))];
		status["propagation_frame"] = p_frame;
		status["propagation_steps"] = volume->propagation_steps;
		status["propagation_iterations"] = p_iterations;
		status["visibility_history_index"] = volume->history_index[0];
		status["light_history_index"] = volume->history_index[1];
		status["propagation_phase"] = p_phase == 0 ? "A" : "L";
		status["cpu_submit_usec"] = OS::get_singleton()->get_ticks_usec() - cpu_start;
		status["debug_bytes"] = volume->static_geometry.is_valid() ? uint64_t(count) * GEOMETRY_STRIDE : 0;
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

Error LRTRenderer::prepare_positional_shadows(RendererSceneCull *p_cull, RID p_scenario, const Vector3 &p_center, uint64_t p_visibility_viewport_mask) {
#ifdef RD_ENABLED
	if (!state || !RendererSceneRenderImplementation::RenderForwardClustered::get_singleton()) {
		return OK;
	}
	ERR_FAIL_NULL_V(p_cull, ERR_INVALID_PARAMETER);
	RendererSceneCull::Scenario *scenario = p_cull->scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL_V(scenario, ERR_INVALID_PARAMETER);
	State::Draw &draw = state->draws[p_scenario];
	if (draw.positional_frame == state->frame) {
		draw.positional_view_count++;
		_publish_positional_status(p_scenario);
		return draw.positional_error;
	}
	draw.positional_frame = state->frame;
	draw.positional_view_count = 1;
	draw.positional_schedule_count = 0;
	draw.positional_native_passes = 0;
	draw.positional_submitted_passes = 0;
	draw.positional_reference = p_center;
	draw.positional_visibility_mask = p_visibility_viewport_mask;
	auto fail = [&](Error p_error, const String &p_message) {
		if (draw.positional_error != p_error || draw.positional_error_message != p_message) {
			ERR_PRINT("LRT world [" + itos(p_scenario.get_id()) + "]: " + p_message);
		}
		draw.positional_error = p_error;
		draw.positional_error_message = p_message;
		return p_error;
	};

	struct VolumeBounds {
		uint64_t id;
		Transform3D transform;
		Transform3D inverse;
		AABB bounds;
	};
	Vector<VolumeBounds> bounds;
	for (const Dictionary &description : state->descriptions) {
		if (RID(description["scenario"]) != p_scenario || !state->volumes.has(uint64_t(description["id"]))) {
			continue;
		}
		VolumeBounds volume;
		volume.id = description["id"];
		volume.transform = description["transform"];
		volume.inverse = volume.transform.affine_inverse();
		const Vector3 size = description["size"];
		volume.bounds = AABB(-size * 0.5, size);
		bounds.push_back(volume);
	}
	if (bounds.is_empty()) {
		draw.clear_positional();
		draw.positional_frame = state->frame;
		return OK;
	}

	auto *storage = RendererRD::LightStorage::get_singleton();
	ERR_FAIL_NULL_V(storage, ERR_UNAVAILABLE);
	Vector<RID> candidates;
	draw.positional_by_volume.clear();
	for (uint64_t i = 0; i < scenario->instance_data.size(); i++) {
		RendererSceneCull::Instance *instance = scenario->instance_data[i].instance;
		if (!instance || !instance->visible || instance->base_type != RSE::INSTANCE_LIGHT || !instance->base_data) {
			continue;
		}
		const RSE::LightType type = storage->light_get_type(instance->base);
		if ((type != RSE::LIGHT_OMNI && type != RSE::LIGHT_SPOT) || storage->light_get_bake_mode(instance->base) == RSE::LIGHT_BAKE_DISABLED || storage->light_get_param(instance->base, RSE::LIGHT_PARAM_INDIRECT_ENERGY) == 0) {
			continue;
		}
		const float radius = storage->light_get_param(instance->base, RSE::LIGHT_PARAM_RANGE);
		if (radius <= 0) {
			continue;
		}
		Transform3D light_transform = instance->transform;
		light_transform.orthonormalize();
		Vector<Plane> spot_planes;
		const float angle = type == RSE::LIGHT_SPOT ? storage->light_get_param(instance->base, RSE::LIGHT_PARAM_SPOT_ANGLE) : 0;
		if (type == RSE::LIGHT_SPOT && angle > 0 && angle < 89 && radius > 0.025f) {
			Projection projection;
			projection.set_perspective(angle * 2.0f, 1.0f, 0.025f, radius);
			spot_planes = projection.get_projection_planes(light_transform);
		}
		bool relevant = false;
		for (const VolumeBounds &volume : bounds) {
			const Vector3 local = volume.inverse.xform(light_transform.origin);
			const Vector3 end = volume.bounds.get_end();
			const Vector3 closest(CLAMP(local.x, volume.bounds.position.x, end.x), CLAMP(local.y, volume.bounds.position.y, end.y), CLAMP(local.z, volume.bounds.position.z, end.z));
			if (volume.transform.xform(closest).distance_squared_to(light_transform.origin) > radius * radius) {
				continue;
			}
			bool outside = false;
			// Ignore the projection near plane: a receiver can be closer than the shadow camera's near plane.
			for (int plane = 1; plane < spot_planes.size(); plane++) {
				bool all_outside = true;
				for (int corner = 0; corner < 8; corner++) {
					if (spot_planes[plane].distance_to(volume.transform.xform(volume.bounds.get_endpoint(corner))) < 0) {
						all_outside = false;
						break;
					}
				}
				if (all_outside) {
					outside = true;
					break;
				}
			}
			if (!outside) {
				draw.positional_by_volume[volume.id].push_back(instance->self);
				relevant = true;
			}
		}
		if (relevant) {
			candidates.push_back(instance->self);
		}
	}
	candidates.sort();
	HashSet<RID> candidate_set;
	for (RID candidate : candidates) {
		candidate_set.insert(candidate);
	}
	bool changed = candidates != draw.positional_candidates;
	Vector<RID> removed;
	for (const KeyValue<RID, State::PositionalLight *> &entry : draw.positional_lights) {
		if (!candidate_set.has(entry.key)) {
			removed.push_back(entry.key);
		}
	}
	for (RID source : removed) {
		State::PositionalLight *light = draw.positional_lights[source];
		if (light->shadow_instance.is_valid()) {
			storage->light_instance_free(light->shadow_instance);
		}
		memdelete(light);
		draw.positional_lights.erase(source);
	}
	draw.positional_candidates = candidates;
	uint32_t omni_count = 0;
	uint32_t spot_count = 0;
	for (RID source : candidates) {
		RendererSceneCull::Instance *instance = p_cull->instance_owner.get_or_null(source);
		ERR_FAIL_NULL_V(instance, ERR_BUG);
		const RSE::LightType type = storage->light_get_type(instance->base);
		const bool shadowed = storage->light_has_shadow(instance->base);
		State::PositionalLight **entry = draw.positional_lights.getptr(source);
		State::PositionalLight *light = entry ? *entry : nullptr;
		if (!light) {
			light = memnew(State::PositionalLight);
			draw.positional_lights[source] = light;
			changed = true;
		}
		if (light->base != instance->base || light->shadowed != shadowed || light->type != type) {
			if (light->shadow_instance.is_valid()) {
				storage->light_instance_free(light->shadow_instance);
				light->shadow_instance = RID();
			}
			light->base = instance->base;
			light->shadowed = shadowed;
			light->type = type;
			light->invalidate();
			changed = true;
		}
		light->source_instance = static_cast<RendererSceneCull::InstanceLightData *>(instance->base_data)->instance;
		const uint32_t pass_count = shadowed ? p_cull->get_lrt_positional_shadow_pass_count(instance) : 0;
		if (light->pass_count != pass_count) {
			light->pass_count = pass_count;
			light->invalidate();
		}
		light->source_revision = static_cast<RendererSceneCull::InstanceLightData *>(instance->base_data)->lrt_shadow_revision;
		if (shadowed) {
			if (type == RSE::LIGHT_OMNI) {
				omni_count++;
			} else {
				spot_count++;
			}
		}
	}
	for (RID source : candidates) {
		State::PositionalLight *light = draw.positional_lights[source];
		if (!light->shadowed) {
			continue;
		}
		RendererSceneCull::Instance *instance = p_cull->instance_owner.get_or_null(source);
		if (light->shadow_instance.is_null()) {
			light->shadow_instance = storage->light_instance_create(light->base);
			if (light->shadow_instance.is_null()) {
				return fail(ERR_CANT_CREATE, "LRT positional shadow light instance allocation failed.");
			}
			changed = true;
		}
		storage->light_instance_set_transform(light->shadow_instance, instance->transform);
	}
	if (omni_count + spot_count == 0) {
		if (draw.positional_atlas.is_valid()) {
			storage->shadow_atlas_free(draw.positional_atlas);
			draw.positional_atlas = RID();
		}
		draw.positional_atlas_size = 0;
		draw.positional_tile_size = 0;
		draw.positional_queue.clear();
		draw.positional_capacity_error = false;
		draw.positional_error = OK;
		draw.positional_error_message = String();
		return OK;
	}

	const int atlas_setting = GLOBAL_GET("rendering/lights_and_shadows/positional_shadow/atlas_size");
	const int tile_setting = GLOBAL_GET("rendering/global_illumination/lrt/positional_shadow_resolution");
	const bool use_16_bits = GLOBAL_GET("rendering/lights_and_shadows/positional_shadow/atlas_16_bits");
	if (atlas_setting <= 0 || tile_setting < 64 || tile_setting > 2048 || Math::next_power_of_2(uint32_t(tile_setting)) != uint32_t(tile_setting)) {
		return fail(ERR_INVALID_PARAMETER, "LRT positional shadow atlas size and tile resolution are invalid.");
	}
	const uint32_t atlas_size = Math::next_power_of_2(uint32_t(atlas_setting));
	const uint32_t tile_size = uint32_t(tile_setting);
	if (atlas_size > RD::get_singleton()->limit_get(RD::LIMIT_MAX_TEXTURE_SIZE_2D) || tile_size > atlas_size / 2 || atlas_size % (2 * tile_size) != 0) {
		return fail(ERR_INVALID_PARAMETER, "LRT positional shadow resolution does not fit the configured atlas.");
	}
	const uint32_t subdivision = atlas_size / (2 * tile_size);
	const uint64_t cells_per_quadrant = uint64_t(subdivision) * subdivision;
	if (cells_per_quadrant >= 16384) {
		return fail(ERR_INVALID_PARAMETER, "LRT positional shadow tile subdivision exceeds the native atlas limit.");
	}
	const bool over_capacity = 2 * uint64_t(omni_count) + spot_count > 4 * cells_per_quadrant || omni_count > 4 * (cells_per_quadrant / 2);
	if (draw.positional_atlas.is_valid() && (draw.positional_atlas_size != atlas_size || draw.positional_tile_size != tile_size || draw.positional_16_bits != use_16_bits)) {
		storage->shadow_atlas_free(draw.positional_atlas);
		draw.positional_atlas = RID();
		changed = true;
	}
	if (draw.positional_atlas.is_null()) {
		draw.positional_atlas = storage->shadow_atlas_create();
		if (draw.positional_atlas.is_null()) {
			return fail(ERR_CANT_CREATE, "LRT positional shadow atlas allocation failed.");
		}
		storage->shadow_atlas_set_size(draw.positional_atlas, atlas_size, use_16_bits);
		for (int quadrant = 0; quadrant < 4; quadrant++) {
			storage->shadow_atlas_set_quadrant_subdivision(draw.positional_atlas, quadrant, int(cells_per_quadrant));
		}
		draw.positional_atlas_size = atlas_size;
		draw.positional_tile_size = tile_size;
		draw.positional_16_bits = use_16_bits;
		changed = true;
	}
	bool capacity_error = draw.positional_capacity_error;
	auto allocate_missing = [&]() -> Error {
		for (int pass = 0; pass < 2; pass++) {
			for (RID source : candidates) {
				State::PositionalLight *light = draw.positional_lights[source];
				if (!light->shadowed || (light->type == RSE::LIGHT_OMNI) != (pass == 0) || storage->shadow_atlas_owns_light_instance(draw.positional_atlas, light->shadow_instance)) {
					continue;
				}
				const Error error = storage->shadow_atlas_acquire_fixed_light(draw.positional_atlas, light->shadow_instance, tile_size);
				if (error == ERR_OUT_OF_MEMORY) {
					capacity_error = true;
					light->invalidate();
					light->result.status = RendererSceneRender::LRTPositionalShadowResult::FAILED;
					continue;
				}
				if (error != OK) {
					return error;
				}
				light->invalidate();
			}
		}
		return OK;
	};
	Error allocation_error = OK;
	if (!draw.positional_capacity_error || changed) {
		capacity_error = false;
		allocation_error = allocate_missing();
	}
	if (allocation_error == OK && capacity_error && changed && !over_capacity) {
		// Repack only after incremental allocation fails, placing Omni pairs first.
		for (RID source : candidates) {
			State::PositionalLight *light = draw.positional_lights[source];
			if (storage->shadow_atlas_owns_light_instance(draw.positional_atlas, light->shadow_instance)) {
				storage->shadow_atlas_release_fixed_light(draw.positional_atlas, light->shadow_instance);
				light->invalidate();
			}
		}
		capacity_error = false;
		allocation_error = allocate_missing();
	}
	if (capacity_error && !draw.positional_capacity_error) {
		ERR_PRINT("LRT positional shadow atlas capacity is insufficient for some active lights.");
	}
	draw.positional_capacity_error = capacity_error;
	if (allocation_error != OK) {
		storage->shadow_atlas_free(draw.positional_atlas);
		draw.positional_atlas = RID();
		for (RID source : candidates) {
			draw.positional_lights[source]->result.status = RendererSceneRender::LRTPositionalShadowResult::FAILED;
		}
		return fail(allocation_error, allocation_error == ERR_OUT_OF_MEMORY ? "LRT positional shadow atlas capacity is insufficient for the active lights." : "LRT positional shadow atlas resources could not be allocated.");
	}
	draw.positional_error = OK;
	draw.positional_error_message = String();
	auto *materials = RendererRD::MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(materials, ERR_UNAVAILABLE);
	for (RID source : candidates) {
		State::PositionalLight *light = draw.positional_lights[source];
		if (!light->shadowed || !storage->shadow_atlas_owns_light_instance(draw.positional_atlas, light->shadow_instance)) {
			continue;
		}
		RendererSceneCull::Instance *instance = p_cull->instance_owner.get_or_null(source);
		ERR_FAIL_NULL_V(instance, ERR_BUG);
		RendererSceneCull::LRTPositionalShadowRequest request;
		request.reference_position = draw.positional_reference;
		request.visibility_viewport_mask = draw.positional_visibility_mask;
		HashSet<RID> caster_set;
		const Error query_error = p_cull->query_lrt_positional_shadow_casters(instance, request, caster_set);
		if (query_error != OK) {
			return fail(query_error, "LRT positional shadow caster query failed.");
		}
		Vector<RID> casters;
		for (RID caster : caster_set) {
			casters.push_back(caster);
		}
		casters.sort();
		Vector<uint64_t> versions;
		versions.push_back(light->source_revision);
		bool dynamic = false;
		bool uses_camera = false;
		for (RID caster : casters) {
			RendererSceneCull::Instance *geometry = p_cull->instance_owner.get_or_null(caster);
			ERR_FAIL_NULL_V(geometry, ERR_BUG);
			versions.push_back(caster.get_id());
			versions.push_back(geometry->version);
			versions.push_back(geometry->base.get_id());
			if (geometry->mesh_instance.is_valid()) {
				// Mesh deformation can be updated after scene dependency processing.
				dynamic = true;
			}
			if (geometry->base_type == RSE::INSTANCE_PARTICLES) {
				dynamic = true;
				continue;
			}
			if (geometry->base_type == RSE::INSTANCE_MULTIMESH) {
				RID buffer = RSG::mesh_storage->multimesh_get_buffer_rd_rid(geometry->base);
				if (buffer.is_null()) {
					dynamic = true;
				} else {
					const uint64_t revision = RD::get_singleton()->buffer_get_content_revision(buffer);
					versions.push_back(buffer.get_id());
					versions.push_back(revision);
					dynamic |= revision == UINT64_MAX;
				}
			}
			RID mesh = geometry->base_type == RSE::INSTANCE_MULTIMESH ? RSG::mesh_storage->multimesh_get_mesh(geometry->base) : geometry->base;
			if (mesh.is_null() || (geometry->base_type != RSE::INSTANCE_MESH && geometry->base_type != RSE::INSTANCE_MULTIMESH)) {
				dynamic = true;
				continue;
			}
			const int surfaces = RSG::mesh_storage->mesh_get_surface_count(mesh);
			versions.push_back(mesh.get_id());
			versions.push_back(surfaces);
			for (int surface = 0; surface < surfaces; surface++) {
				for (RID buffer : { RSG::mesh_storage->mesh_surface_get_vertex_buffer_rd_rid(mesh, surface), RSG::mesh_storage->mesh_surface_get_attribute_buffer_rd_rid(mesh, surface), RSG::mesh_storage->mesh_surface_get_index_buffer_rd_rid(mesh, surface) }) {
					versions.push_back(buffer.get_id());
					if (buffer.is_valid()) {
						const uint64_t revision = RD::get_singleton()->buffer_get_content_revision(buffer);
						versions.push_back(revision);
						dynamic |= revision == UINT64_MAX;
					}
				}
				RID material = geometry->material_override;
				if (material.is_null() && surface < geometry->materials.size()) {
					material = geometry->materials[surface];
				}
				if (material.is_null()) {
					material = RSG::mesh_storage->mesh_surface_get_material(mesh, surface);
				}
				if (material.is_valid()) {
					materials->material_get_lrt_shadow_dependencies(material, versions, dynamic, uses_camera);
				}
				versions.push_back(0);
			}
			if (geometry->material_overlay.is_valid()) {
				materials->material_get_lrt_shadow_dependencies(geometry->material_overlay, versions, dynamic, uses_camera);
			}
			versions.push_back(0);
		}
		versions.push_back(uses_camera);
		if (uses_camera) {
			for (int axis = 0; axis < 3; axis++) {
				uint64_t bits = 0;
				memcpy(&bits, &draw.positional_reference[axis], sizeof(real_t));
				versions.push_back(bits);
			}
		}
		versions.push_back(dynamic);
		if (dynamic) {
			versions.push_back(state->frame);
		}
		if (light->input_versions != versions) {
			light->input_version++;
			light->input_versions = versions;
		}
	}
	for (int i = draw.positional_queue.size() - 1; i >= 0; i--) {
		State::PositionalLight **entry = draw.positional_lights.getptr(draw.positional_queue[i]);
		if (!entry || !(*entry)->shadowed || !storage->shadow_atlas_owns_light_instance(draw.positional_atlas, (*entry)->shadow_instance) || ((*entry)->completed_valid && (*entry)->input_versions == (*entry)->completed_versions)) {
			draw.positional_queue.remove_at(i);
		}
	}
	for (RID source : candidates) {
		State::PositionalLight *light = draw.positional_lights[source];
		if (light->shadowed && storage->shadow_atlas_owns_light_instance(draw.positional_atlas, light->shadow_instance) && (!light->completed_valid || light->input_versions != light->completed_versions) && !draw.positional_queue.has(source)) {
			draw.positional_queue.push_back(source);
		}
	}
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

uint32_t LRTRenderer::reserve_positional_shadow_passes(RID p_scenario) const {
#ifdef RD_ENABLED
	const State::Draw *draw = state ? state->draws.getptr(p_scenario) : nullptr;
	if (!draw || draw->positional_frame != state->frame || draw->positional_scheduled_frame == state->frame || draw->positional_error != OK || draw->positional_queue.is_empty()) {
		return 0;
	}
	State::PositionalLight *const *light = draw->positional_lights.getptr(draw->positional_queue[0]);
	ERR_FAIL_NULL_V(light, 0);
	return (*light)->pass_count;
#else
	return 0;
#endif
}

void LRTRenderer::schedule_positional_shadows(RendererSceneCull *p_cull, RID p_scenario) {
#ifdef RD_ENABLED
	State::Draw *draw = state ? state->draws.getptr(p_scenario) : nullptr;
	if (!draw || draw->positional_frame != state->frame || draw->positional_scheduled_frame == state->frame || draw->positional_error != OK) {
		return;
	}
	draw->positional_scheduled_frame = state->frame;
	draw->positional_schedule_count++;
	draw->positional_native_passes = p_cull->max_shadows_used;
	const int count = draw->positional_queue.size();
	for (int i = 0; i < count; i++) {
		const RID source = draw->positional_queue[0];
		State::PositionalLight *light = draw->positional_lights[source];
		if (p_cull->max_shadows_used + light->pass_count > RendererSceneCull::MAX_UPDATE_SHADOWS) {
			break;
		}
		draw->positional_queue.remove_at(0);
		draw->positional_queue.push_back(source);
		const Error error = submit_positional_shadow(p_cull, p_scenario, source);
		if (error == OK) {
			draw->positional_submitted_passes += light->pass_count;
		}
		if (error != OK && error != ERR_BUSY && !light->failure_reported) {
			ERR_PRINT("LRT positional shadow submission failed for light " + itos(source.get_id()) + ".");
			light->failure_reported = true;
		}
	}
#endif
}

void LRTRenderer::finish_positional_shadows() {
#ifdef RD_ENABLED
	State::Draw *draw = state ? state->draws.getptr(state->scenario) : nullptr;
	if (!draw || draw->positional_scheduled_frame != state->frame) {
		return;
	}
	for (RID source : draw->positional_queue) {
		State::PositionalLight *light = draw->positional_lights[source];
		if (light->submitted_frame != state->frame || light->finished_frame == state->frame) {
			continue;
		}
		light->finished_frame = state->frame;
		if (light->result.status == RendererSceneRender::LRTPositionalShadowResult::SUBMITTED && light->result.submitted_passes == light->pass_count) {
			light->completed_versions = light->submitted_versions;
			light->completed_version = light->submitted_version;
			light->completed_valid = true;
			light->completion_count++;
			light->failure_reported = false;
		} else if (light->result.status != RendererSceneRender::LRTPositionalShadowResult::WAITING) {
			light->completed_valid = false;
			if (!light->failure_reported) {
				ERR_PRINT("LRT positional shadow rendering failed for light " + itos(source.get_id()) + ".");
				light->failure_reported = true;
			}
		}
	}
#endif
}

uint64_t LRTRenderer::get_positional_scenario_id() const {
#ifdef RD_ENABLED
	return state ? state->scenario.get_id() : 0;
#else
	return 0;
#endif
}

void LRTRenderer::_publish_positional_status(RID p_scenario) {
#ifdef RD_ENABLED
	State::Draw *draw = state ? state->draws.getptr(p_scenario) : nullptr;
	if (!draw) {
		return;
	}
	Dictionary info;
	info["frame"] = state->frame;
	info["world"] = p_scenario.get_id();
	info["atlas"] = draw->positional_atlas.get_id();
	info["views"] = draw->positional_view_count;
	info["schedules"] = draw->positional_schedule_count;
	info["native_passes"] = draw->positional_native_passes;
	info["submitted_passes"] = draw->positional_submitted_passes;
	info["queue"] = draw->positional_queue.size();
	info["capacity_error"] = draw->positional_capacity_error;
	info["error"] = int(draw->positional_error);
	info["timing_frame"] = state->profile_frame;
	const Dictionary *timings = state->world_timings.getptr(p_scenario.get_id());
	info["atlas_gpu_usec"] = timings ? double(timings->get("positional_atlas_gpu_usec", 0.0)) : 0.0;
	info["cube_gpu_usec"] = timings ? double(timings->get("positional_cube_gpu_usec", 0.0)) : 0.0;
	Array lights;
	uint32_t ready = 0;
	uint32_t submitted = 0;
	for (RID source : draw->positional_candidates) {
		State::PositionalLight **entry = draw->positional_lights.getptr(source);
		if (!entry) {
			continue;
		}
		const State::PositionalLight &light = **entry;
		const bool current = light.completed_valid && light.completed_versions == light.input_versions;
		ready += light.shadowed && current;
		submitted += light.submitted_frame == state->frame;
		Dictionary item;
		item["source"] = source.get_id();
		item["shadowed"] = light.shadowed;
		item["type"] = int(light.type);
		item["passes"] = light.pass_count;
		item["submitted_passes"] = light.result.submitted_passes;
		item["status"] = int(light.result.status);
		item["ready"] = current;
		item["submitted_frame"] = light.submitted_frame == UINT64_MAX ? int64_t(-1) : int64_t(light.submitted_frame);
		item["submissions"] = light.submission_count;
		item["completions"] = light.completion_count;
		item["casters"] = light.casters.size();
		item["input_version"] = light.input_version;
		item["submitted_version"] = light.submitted_version;
		item["completed_version"] = light.completed_version;
		lights.push_back(item);
	}
	info["ready"] = ready;
	info["submitted"] = submitted;
	info["lights"] = lights;
	MutexLock lock(status_mutex);
	for (const Dictionary &description : state->descriptions) {
		if (RID(description["scenario"]) == p_scenario && state->volumes.has(uint64_t(description["id"]))) {
			statuses[ObjectID(uint64_t(description["id"]))]["positional"] = info;
		}
	}
#endif
}

Vector<RID> LRTRenderer::get_positional_shadow_candidates(RID p_scenario) const {
#ifdef RD_ENABLED
	const State::Draw *draw = state ? state->draws.getptr(p_scenario) : nullptr;
	return draw && draw->positional_frame == state->frame ? draw->positional_candidates : Vector<RID>();
#else
	return Vector<RID>();
#endif
}

Error LRTRenderer::submit_positional_shadow(RendererSceneCull *p_cull, RID p_scenario, RID p_source_instance) {
#ifdef RD_ENABLED
	ERR_FAIL_NULL_V(p_cull, ERR_INVALID_PARAMETER);
	State::Draw *draw = state ? state->draws.getptr(p_scenario) : nullptr;
	ERR_FAIL_NULL_V(draw, ERR_UNCONFIGURED);
	if (draw->positional_error != OK) {
		return draw->positional_error;
	}
	ERR_FAIL_COND_V(draw->positional_frame != state->frame, ERR_UNCONFIGURED);
	State::PositionalLight **entry = draw->positional_lights.getptr(p_source_instance);
	ERR_FAIL_NULL_V(entry, ERR_INVALID_PARAMETER);
	State::PositionalLight *light = *entry;
	ERR_FAIL_COND_V(!light->shadowed || draw->positional_atlas.is_null(), ERR_INVALID_PARAMETER);
	if (light->submitted_frame == state->frame) {
		return ERR_BUSY;
	}
	RendererSceneCull::Instance *source = p_cull->instance_owner.get_or_null(p_source_instance);
	ERR_FAIL_COND_V(!source || !source->scenario || source->scenario->self != p_scenario, ERR_INVALID_PARAMETER);
	RendererSceneCull::LRTPositionalShadowRequest request;
	request.light_instance = light->shadow_instance;
	request.shadow_atlas = draw->positional_atlas;
	request.reference_position = draw->positional_reference;
	request.visibility_viewport_mask = draw->positional_visibility_mask;
	request.casters = &light->casters;
	request.result = &light->result;
	const Error error = p_cull->prepare_lrt_positional_shadow(source, request);
	if (error == OK) {
		light->submitted_frame = state->frame;
		light->submission_count++;
		light->submitted_version = light->input_version;
		light->submitted_versions = light->input_versions;
	}
	return error;
#else
	return ERR_UNAVAILABLE;
#endif
}

Error LRTRenderer::collect_positional_shadow(RID p_scenario, RID p_source_instance, PositionalShadowInfo &r_info) const {
#ifdef RD_ENABLED
	const State::Draw *draw = state ? state->draws.getptr(p_scenario) : nullptr;
	ERR_FAIL_NULL_V(draw, ERR_UNCONFIGURED);
	ERR_FAIL_COND_V(draw->positional_frame != state->frame, ERR_UNCONFIGURED);
	if (draw->positional_error != OK) {
		return draw->positional_error;
	}
	State::PositionalLight *const *entry = draw->positional_lights.getptr(p_source_instance);
	ERR_FAIL_NULL_V(entry, ERR_INVALID_PARAMETER);
	const State::PositionalLight &light = **entry;
	r_info.source_instance = p_source_instance;
	r_info.shadow_instance = light.shadow_instance;
	r_info.shadow_atlas = light.shadowed ? draw->positional_atlas : RID();
	r_info.shadowed = light.shadowed;
	r_info.current = light.completed_valid && light.completed_versions == light.input_versions;
	r_info.pass_count = light.pass_count;
	r_info.submitted_passes = light.result.submitted_passes;
	r_info.source_revision = light.source_revision;
	r_info.submitted_frame = light.submitted_frame;
	r_info.status = static_cast<PositionalShadowStatus>(light.result.status);
	r_info.animated_material = light.result.animated_material;
	r_info.casters = light.casters;
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

void LRTRenderer::remove_scenario(RID p_scenario) {
#ifdef RD_ENABLED
	if (!state) {
		return;
	}
	State::Draw *draw = state->draws.getptr(p_scenario);
	if (draw) {
		draw->clear();
		state->draws.erase(p_scenario);
	}
#endif
}

void LRTRenderer::prepare_sun_shadows(RendererSceneCull *p_cull, RID p_scenario, const Vector3 &p_center) {
#ifdef RD_ENABLED
	if (!has_active_volumes() || !RendererSceneRenderImplementation::RenderForwardClustered::get_singleton()) {
		return;
	}
	bool active = false;
	for (const Variant &item : state->descriptions) {
		Dictionary description = item;
		if (RID(description["scenario"]) == p_scenario && state->volumes.has(uint64_t(description["id"]))) {
			active = true;
			break;
		}
	}
	if (!active) {
		return;
	}
	State::Draw &draw = state->draws[p_scenario];
	if (draw.sun_frame == state->frame) {
		return;
	}
	draw.sun_frame = state->frame;
	draw.sun_shadows.clear();
	const float extent = GLOBAL_GET("rendering/global_illumination/lrt/sun_shadow_extent");
	const int resolution = GLOBAL_GET("rendering/global_illumination/lrt/sun_shadow_resolution");
	RD *rd = RD::get_singleton();
	ERR_FAIL_COND_MSG(!Math::is_finite(extent) || extent <= 0 || resolution < 256 || resolution > int(rd->limit_get(RD::LIMIT_MAX_TEXTURE_SIZE_2D)), "Invalid LRT sun shadow extent or resolution.");
	auto *scenario = p_cull->scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL(scenario);
	auto *storage = RendererRD::LightStorage::get_singleton();
	for (RendererSceneCull::Instance *instance : scenario->directional_lights) {
		if (!instance->visible || !storage->light_has_shadow(instance->base) || storage->light_get_bake_mode(instance->base) == RSE::LIGHT_BAKE_DISABLED || storage->light_get_param(instance->base, RSE::LIGHT_PARAM_INDIRECT_ENERGY) == 0 || storage->light_directional_get_sky_mode(instance->base) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			continue;
		}
		ERR_FAIL_COND_MSG(draw.sun_shadows.size() == RendererSceneRender::MAX_DIRECTIONAL_LIGHTS, "Too many LRT shadowed directional lights.");
		State::SunShadow shadow;
		shadow.light = static_cast<RendererSceneCull::InstanceLightData *>(instance->base_data)->instance;
		shadow.transform.basis = instance->transform.basis.orthonormalized();
		shadow.extent = extent;
		draw.sun_shadows.push_back(shadow);
	}
	if (draw.sun_shadows.is_empty()) {
		draw.clear_sun_texture();
		return;
	}
	if (draw.sun_resolution != resolution) {
		draw.clear_sun_texture();
		RD::TextureFormat format;
		format.width = resolution;
		format.height = resolution;
		format.format = RD::DATA_FORMAT_D32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		draw.sun_texture = rd->texture_create(format, RD::TextureView());
		ERR_FAIL_COND_MSG(draw.sun_texture.is_null(), "LRT sun shadow texture allocation failed.");
		draw.sun_framebuffer = rd->framebuffer_create({ draw.sun_texture });
		ERR_FAIL_COND_MSG(draw.sun_framebuffer.is_null(), "LRT sun shadow framebuffer allocation failed.");
		draw.sun_resolution = resolution;
	}
	const int columns = int(Math::ceil(Math::sqrt(double(draw.sun_shadows.size()))));
	const int tile_size = resolution / columns;
	for (int i = 0; i < draw.sun_shadows.size(); i++) {
		State::SunShadow &shadow = draw.sun_shadows.write[i];
		shadow.rect = Rect2i(Vector2i(i % columns, i / columns) * tile_size, Size2i(tile_size, tile_size));
		const float texel_size = 2.0f * extent / (tile_size - 2);
		Vector3 light_center = shadow.transform.basis.xform_inv(p_center);
		for (int axis = 0; axis < 3; axis++) {
			light_center[axis] = Math::snapped(light_center[axis], texel_size);
		}
		shadow.center = shadow.transform.basis.xform(light_center);
		shadow.transform.origin = shadow.center;
		const float half_size = extent + texel_size;
		Vector<Plane> planes;
		for (int axis = 0; axis < 2; axis++) {
			Vector3 direction = shadow.transform.basis.get_column(axis);
			planes.push_back(Plane(direction, light_center[axis] + half_size));
			planes.push_back(Plane(-direction, -light_center[axis] + half_size));
		}
		// Casters may be arbitrarily far toward the sun; fit their depth after native culling.
		Vector3 z_axis = shadow.transform.basis.get_column(2);
		planes.push_back(Plane(-z_axis, -light_center.z + extent));
		ERR_FAIL_COND(p_cull->cull.shadow_count >= RendererSceneCull::MAX_SHADOW_CULL_LIGHTS);
		auto &task = p_cull->cull.shadows[p_cull->cull.shadow_count++];
		task.lrt = true;
		task.light_instance = shadow.light;
		task.caster_mask = storage->light_get_shadow_caster_mask(storage->light_instance_get_base_light(shadow.light));
		task.cascade_count = 1;
		task.cascades[0].frustum = RendererSceneCull::Frustum(planes);
		task.cascades[0].transform = shadow.transform;
	}
#endif
}

void LRTRenderer::submit_sun_shadow(RendererSceneCull *p_cull, RID p_scenario, uint32_t p_index) {
#ifdef RD_ENABLED
	State::Draw &draw = state->draws[p_scenario];
	const auto &task = p_cull->cull.shadows[p_index];
	for (State::SunShadow &shadow : draw.sun_shadows) {
		if (shadow.light != task.light_instance) {
			continue;
		}
		ERR_FAIL_COND(draw.sun_framebuffer.is_null());
		ERR_FAIL_COND_MSG(p_cull->max_shadows_used == RendererSceneCull::MAX_UPDATE_SHADOWS, "LRT sun shadow submission budget exhausted.");
		const Vector3 z_axis = shadow.transform.basis.get_column(2);
		const real_t center_z = z_axis.dot(shadow.center);
		const real_t texel_size = 2.0 * shadow.extent / (shadow.rect.size.x - 2);
		const real_t z_max = Math::snapped(MAX(center_z + shadow.extent, p_cull->scene_cull_result.directional_shadows[p_index].caster_z_max) + texel_size, texel_size);
		shadow.transform.origin = shadow.center + z_axis * (z_max - center_z);
		const real_t half_size = shadow.extent + texel_size;
		shadow.projection.set_orthogonal(-half_size, half_size, -half_size, half_size, 0, z_max - center_z + shadow.extent + texel_size);
		RID base = RendererRD::LightStorage::get_singleton()->light_instance_get_base_light(shadow.light);
		// Keep world-space bias independent of distant casters entering the fitted depth range.
		shadow.bias = RendererRD::LightStorage::get_singleton()->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_BIAS) * 0.01f * (2.0f * shadow.extent);
		auto &output = p_cull->render_shadow_data[p_cull->max_shadows_used++];
		output.light = shadow.light;
		output.pass = 0;
		output.instances.merge_unordered(p_cull->scene_cull_result.directional_shadows[p_index].cascade_geometry_instances[0]);
		output.lrt_framebuffer = draw.sun_framebuffer;
		output.lrt_projection = shadow.projection;
		output.lrt_transform = shadow.transform;
		output.lrt_reference = Transform3D(Basis(), shadow.center);
		output.lrt_rect = shadow.rect;
		shadow.ready = true;
		for (const Variant &item : state->descriptions) {
			Dictionary description = item;
			if (RID(description["scenario"]) != p_scenario) {
				continue;
			}
			MutexLock lock(status_mutex);
			Dictionary &status = statuses[ObjectID(uint64_t(description["id"]))];
			status["sun_shadow_resolution"] = draw.sun_resolution;
			status["sun_shadow_extent"] = shadow.extent;
			status["sun_shadow_center"] = shadow.center;
			status["sun_shadow_bytes"] = uint64_t(draw.sun_resolution) * draw.sun_resolution * 4;
			status["sun_shadow_light_count"] = draw.sun_shadows.size();
			status["sun_shadow_casters"] = output.instances.size();
			status["sun_shadow_frame"] = state->frame;
		}
		return;
	}
#endif
}

RID LRTRenderer::get_material_bake_buffer() {
#ifdef RD_ENABLED
	if (material_bake_buffer.is_valid()) {
		return material_bake_buffer;
	}
	if (!state) {
		state = memnew(State);
	}
	ERR_FAIL_COND_V_MSG(state->default_material_bake.is_null(), RID(), "LRT default material buffer could not be allocated.");
	return state->default_material_bake;
#else
	return RID();
#endif
}

RID LRTRenderer::get_draw_metadata() {
#ifdef RD_ENABLED
	if (!state) {
		state = memnew(State);
	}
	State::Draw *draw = state->draws.getptr(state->scenario);
	if (draw && draw->frame == state->frame && draw->metadata.is_valid()) {
		return draw->metadata;
	}
	ERR_FAIL_COND_V_MSG(state->default_metadata.is_null(), RID(), "LRT default metadata buffer could not be allocated.");
	return state->default_metadata;
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
	if (draw && draw->frame == state->frame && draw->samples.is_valid()) {
		return draw->samples;
	}
	ERR_FAIL_COND_V_MSG(state->default_samples.is_null(), RID(), "LRT default sample buffer could not be allocated.");
	return state->default_samples;
#else
	return RID();
#endif
}

RID LRTRenderer::debug_render(const RenderDataRD *p_render_data) {
#ifdef RD_ENABLED
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	int mode = RendererSceneRenderRD::get_singleton()->get_debug_draw_mode() - RSE::VIEWPORT_DEBUG_DRAW_LRT_INDIRECT;
	if (rb.is_null()) {
		return RID();
	}
	if (!state || mode < 0 || mode > 9) {
		rb->clear_context("lrt_debug");
		return RID();
	}
	ERR_FAIL_COND_V_MSG(!state->init_debug(), RID(), "LRT debug compute pipeline could not be created.");
	RD *rd = RD::get_singleton();
	Array targets;
	uint64_t selection = state->debug.get("selection", uint64_t(0));
	for (const Variant &item : state->descriptions) {
		Dictionary description = item;
		const State::Volume *volume = state->volumes.getptr(uint64_t(description["id"]));
		if (RID(description["scenario"]) != state->scenario || !volume || volume->shadow_blocked || volume->recovering) {
			continue;
		}
		if (bool(state->debug.get("volume_selection", false)) && uint64_t(description["id"]) != selection &&
				(bool(state->debug.get("detail_selection", false)) || uint64_t(description["group"]) != selection)) {
			continue;
		}
		targets.push_back(description);
	}
	if (mode == 7) {
		targets.clear();
		HashMap<uint64_t, HashSet<int>> seen;
		uint64_t revision = state->scenario.get_id();
		for (const Dictionary &entry : Array(state->debug.get("sdf_instances", Array()))) {
			const State::Volume *volume = state->volumes.getptr(uint64_t(entry["volume"]));
			const uint64_t id = entry["id"];
			const int index = entry["index"];
			if (RID(entry["scenario"]) != state->scenario || !volume || volume->shadow_blocked || volume->recovering || seen[id].has(index)) {
				continue;
			}
			seen[id].insert(index);
			targets.push_back(entry);
			revision = hash_murmur3_one_64(uint64_t(entry["revision"]), hash_murmur3_one_64(id, hash_murmur3_one_64(index, revision)));
		}
		bool upload = state->sdf_buffer.is_null() || state->sdf_id != selection || state->sdf_revision != revision;
		PackedByteArray bytes;
		int offset = 0;
		for (int i = 0; i < targets.size(); i++) {
			Dictionary entry = targets[i].duplicate();
			Dictionary data = entry["data"];
			PackedByteArray distance = data["distance"];
			PackedByteArray color = data["color"];
			int color_offset = offset + ((distance.size() + 3) & ~3);
			int end = color_offset + color.size();
			entry["distance_offset"] = offset / 4;
			entry["color_offset"] = color_offset / 4;
			targets[i] = entry;
			if (upload) {
				bytes.resize(end);
				memcpy(bytes.ptrw() + offset, distance.ptr(), distance.size());
				memcpy(bytes.ptrw() + color_offset, color.ptr(), color.size());
			}
			offset = end;
		}
		if (upload && !bytes.is_empty()) {
			if (state->sdf_buffer.is_valid()) {
				rd->free_rid(state->sdf_buffer);
			}
			state->sdf_buffer = rd->storage_buffer_create(bytes.size(), bytes);
			ERR_FAIL_COND_V(state->sdf_buffer.is_null(), RID());
			state->sdf_id = selection;
			state->sdf_revision = revision;
		}
	}
	// Screen-space modes draw once; an empty target still clears the diagnostic image.
	if (mode < 2 || mode == 9 || targets.is_empty()) {
		targets.clear();
		targets.push_back(Dictionary());
	}
	Vector2i size = rb->get_internal_size();
	if (!rb->has_texture("lrt_debug", "output")) {
		RD::TextureFormat format;
		format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		format.width = size.x;
		format.height = size.y;
		format.array_layers = rb->get_view_count();
		format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		rb->create_texture_from_format("lrt_debug", "output", format);
		format.format = RD::DATA_FORMAT_R32_SFLOAT;
		rb->create_texture_from_format("lrt_debug", "depth", format);
	}
	RID output = rb->get_texture("lrt_debug", "output");
	RID depth = rb->get_texture("lrt_debug", "depth");
	ERR_FAIL_COND_V(output.is_null() || depth.is_null(), RID());
	RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	for (int target = 0; target < targets.size(); target++) {
		Dictionary description = targets[target];
		State::Volume *volume = mode != 7 && description.has("id") ? state->volumes.getptr(uint64_t(description["id"])) : nullptr;
		if (mode == 6 && volume && volume->static_geometry.is_null()) {
			const PackedByteArray ltm = description["static_ltm"];
			const PackedByteArray visibility = description["static_vis"];
			const PackedByteArray edges = description["static_edges"];
			const uint32_t count = volume->grid.x * volume->grid.y * volume->grid.z;
			PackedByteArray bytes;
			bytes.resize(count * GEOMETRY_STRIDE);
			uint8_t update[UPDATE_STRIDE];
			for (uint32_t probe = 0; probe < count; probe++) {
				pack_probe(update, probe, ltm.ptr() + probe * 36, visibility.ptr() + probe * 8, edges.ptr() + probe * 26, nullptr);
				memcpy(bytes.ptrw() + probe * GEOMETRY_STRIDE, update + 4, GEOMETRY_STRIDE);
			}
			volume->static_geometry = rd->storage_buffer_create(bytes.size(), bytes);
			ERR_FAIL_COND_V(volume->static_geometry.is_null(), RID());
		}
		PackedInt32Array trunks = description.get("debug_trunks", PackedInt32Array());
		RID diagnostics = rd->storage_buffer_create(MAX(16, trunks.size() * 4));
		ERR_FAIL_COND_V(diagnostics.is_null(), RID());
		if (!trunks.is_empty()) {
			rd->buffer_update(diagnostics, 0, trunks.size() * 4, trunks.ptr());
		}
		for (uint32_t view = 0; view < rb->get_view_count(); view++) {
			struct {
				float inv_projection[16], camera_to_world[16], world_to_local[16], local_to_world[16];
				float size_spacing[4] = {};
				uint32_t grid_mode[4] = {};
				float sdf_min_scale[4] = {}, sdf_size_offset[4] = {};
				uint32_t sdf_grid_color[4] = {}, screen_pass[4] = {};
				float old_position[4] = {}, old_size[4] = {}, new_position[4] = {}, new_size[4] = {};
			} parameters;
			Transform3D camera = p_render_data->scene_data->cam_transform;
			camera.origin += camera.basis.xform(p_render_data->scene_data->view_eye_offset[view]);
			Transform3D transform = description.get("transform", Transform3D());
			RendererRD::MaterialStorage::store_camera(p_render_data->scene_data->get_view_projection(view).inverse(), parameters.inv_projection);
			RendererRD::MaterialStorage::store_transform(camera, parameters.camera_to_world);
			RendererRD::MaterialStorage::store_transform(transform.affine_inverse(), parameters.world_to_local);
			RendererRD::MaterialStorage::store_transform(transform, parameters.local_to_world);
			Vector3 volume_size = description.get("size", Vector3());
			Dictionary sdf = description.get("data", Dictionary());
			AABB sdf_bounds = sdf.get("bounds", AABB());
			Vector3i sdf_grid = sdf.get("grid_size", Vector3i());
			AABB old_bounds = description.get("debug_old_bounds", AABB());
			AABB new_bounds = description.get("debug_new_bounds", AABB());
			for (int axis = 0; axis < 3; axis++) {
				parameters.size_spacing[axis] = volume_size[axis];
				parameters.grid_mode[axis] = volume ? volume->grid[axis] : 0;
				parameters.sdf_min_scale[axis] = sdf_bounds.position[axis];
				parameters.sdf_size_offset[axis] = sdf_bounds.size[axis];
				parameters.sdf_grid_color[axis] = sdf_grid[axis];
				parameters.old_position[axis] = old_bounds.position[axis];
				parameters.old_size[axis] = old_bounds.size[axis];
				parameters.new_position[axis] = new_bounds.position[axis];
				parameters.new_size[axis] = new_bounds.size[axis];
			}
			parameters.size_spacing[3] = description.get("spacing", 0.5f);
			parameters.grid_mode[3] = mode;
			parameters.sdf_min_scale[3] = sdf.get("distance_scale", 1.0f);
			parameters.sdf_size_offset[3] = description.get("distance_offset", 0);
			parameters.sdf_grid_color[3] = description.get("color_offset", 0);
			parameters.screen_pass[0] = size.x;
			parameters.screen_pass[1] = size.y;
			parameters.screen_pass[2] = view;
			parameters.screen_pass[3] = target == 0 ? 1 : 0;
			parameters.new_size[3] = p_render_data->camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes) : 1.0f;
			RID buffer = rd->uniform_buffer_create(sizeof(parameters));
			if (buffer.is_null()) {
				rd->free_rid(diagnostics);
				ERR_FAIL_V_MSG(RID(), "LRT debug parameters could not be allocated.");
			}
			rd->buffer_update(buffer, 0, sizeof(parameters), &parameters);
			RID empty = state->default_samples;
			RID geometry = volume ? (mode == 6 ? volume->static_geometry : volume->geometry) : empty;
			RID buffers[] = { geometry, volume ? volume->history[volume->history_index[mode == 4 ? 0 : 1]] : empty, diagnostics, state->sdf_buffer.is_valid() ? state->sdf_buffer : empty, get_draw_metadata(), get_draw_samples() };
			Vector<RD::Uniform> uniforms;
			for (int i = 0; i < 6; i++) {
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, i, { buffers[i] }));
			}
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 6, { buffer }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, { output }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, { sampler, get_gather_texture(p_render_data, "debug_lighting") }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 13, { sampler, get_gather_texture(p_render_data, "guide") }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 14, { sampler, get_gather_texture(p_render_data, "profile") }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, { sampler, rb->get_depth_texture(view) }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, { sampler, rb->has_texture("forward_clustered", "normal_roughness") ? rb->get_texture_slice("forward_clustered", "normal_roughness", view, 0) : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL) }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, { depth }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, { sampler, rb->has_texture("forward_clustered", "lrt_surface_profile") ? rb->get_texture_slice("forward_clustered", "lrt_surface_profile", view, 0) : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_UINT) }));
			RID set = rd->uniform_set_create(uniforms, state->debug_shader.version_get_shader(state->debug_version, 0), 0);
			if (set.is_null()) {
				rd->free_rid(buffer);
				rd->free_rid(diagnostics);
				ERR_FAIL_V_MSG(RID(), "LRT debug resources could not be bound.");
			}
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, state->debug_pipeline);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_dispatch(list, (size.x + 7) / 8, (size.y + 7) / 8, 1);
			rd->compute_list_end();
			rd->free_rid(set);
			rd->free_rid(buffer);
		}
		rd->free_rid(diagnostics);
	}
	return output;
#else
	return RID();
#endif
}

bool LRTRenderer::has_active_scenario() const {
#ifdef RD_ENABLED
	if (!state) {
		return false;
	}
	for (const Variant &item : state->descriptions) {
		Dictionary description = item;
		if (RID(description["scenario"]) == state->scenario && state->volumes.has(uint64_t(description["id"]))) {
			return true;
		}
	}
#endif
	return false;
}

bool LRTRenderer::needs_gather() const {
#ifdef RD_ENABLED
	int mode = RendererSceneRenderRD::get_singleton()->get_debug_draw_mode();
	bool debug_screen = mode >= RSE::VIEWPORT_DEBUG_DRAW_LRT_INDIRECT && mode <= RSE::VIEWPORT_DEBUG_DRAW_LRT_BLENDING;
	if (!state || (!bool(GLOBAL_GET("rendering/global_illumination/lrt/screen_gather_enabled")) && !debug_screen)) {
		return false;
	}
	for (const Variant &item : state->descriptions) {
		Dictionary description = item;
		if (RID(description["scenario"]) == state->scenario && state->volumes.has(uint64_t(description["id"]))) {
			return true;
		}
	}
#endif
	return false;
}

RID LRTRenderer::get_gather_texture(const RenderDataRD *p_render_data, const StringName &p_name) {
#ifdef RD_ENABLED
	if (p_render_data && p_render_data->render_buffers.is_valid() && p_render_data->render_buffers->has_texture("lrt", p_name)) {
		return p_render_data->render_buffers->get_texture("lrt", p_name);
	}
	if (p_name != SNAME("profile")) {
		return RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
	}
	if (!state) {
		state = memnew(State);
	}
	ERR_FAIL_COND_V_MSG(state->default_profile_texture.is_null(), RID(), "LRT default profile texture could not be allocated.");
	return state->default_profile_texture;
#else
	return RID();
#endif
}

void LRTRenderer::gather(RenderDataRD *p_render_data, const RID *p_normals) {
#ifdef RD_ENABLED
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null()) {
		return;
	}
	if (!needs_gather()) {
		rb->clear_context("lrt");
		if (state) {
			MutexLock lock(status_mutex);
			for (const Variant &item : state->descriptions) {
				Dictionary description = item;
				if (RID(description["scenario"]) == state->scenario && statuses.has(ObjectID(uint64_t(description["id"])))) {
					Dictionary &status = statuses[ObjectID(uint64_t(description["id"]))];
					status["gather_bytes"] = 0;
					status["gather_gpu_usec"] = 0.0;
					status["gather_cpu_usec"] = 0;
					status.erase("gather_size");
				}
			}
		}
		return;
	}
	State::Draw *draw = state->draws.getptr(state->scenario);
	if (!draw || draw->frame != state->frame || draw->metadata.is_null() || draw->samples.is_null()) {
		rb->clear_context("lrt");
		return;
	}
	auto fail = [&](const String &p_error) {
		rb->clear_context("lrt");
		for (const Variant &item : state->descriptions) {
			Dictionary description = item;
			if (RID(description["scenario"]) == state->scenario) {
				_fail(uint64_t(description["id"]), p_error);
			}
		}
	};
	if (!state->init_gather()) {
		fail("LRT Screen Gather compute pipeline could not be created.");
		return;
	}
	const int scale_index = GLOBAL_GET("rendering/global_illumination/lrt/screen_gather_scale");
	if (scale_index < 0 || scale_index > 2) {
		fail("LRT Screen Gather scale must be Full, Half or Quarter.");
		return;
	}
	const float scale = 1.0f / (1 << scale_index);
	RD *rd = RD::get_singleton();
	const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
	LRTProfileScope profile("LRT Screen Gather [" + itos(rb->get_instance_id()) + "]", "Pre Opaque Render");
	profile.stage("Prepare Resources");
	Vector2i full_size = rb->get_internal_size();
	Vector2i low_size(MAX(1, int(Math::ceil(full_size.x * scale))), MAX(1, int(Math::ceil(full_size.y * scale))));
	if (rb->has_texture("lrt", "lighting") && (rb->get_texture_format("lrt", "lighting").width != uint32_t(low_size.x) || rb->get_texture_format("lrt", "lighting").height != uint32_t(low_size.y))) {
		rb->clear_context("lrt");
	}
	const char *names[] = { "lighting", "guide", "profile" };
	const RD::DataFormat formats[] = { RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, RD::DATA_FORMAT_R32_UINT };
	RID outputs[3];
	for (int i = 0; i < 3; i++) {
		if (!rb->has_texture("lrt", names[i])) {
			RD::TextureFormat format;
			format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
			format.format = formats[i];
			format.width = low_size.x;
			format.height = low_size.y;
			format.array_layers = rb->get_view_count();
			format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
			rb->create_texture_from_format("lrt", names[i], format);
		}
		outputs[i] = rb->get_texture("lrt", names[i]);
		if (outputs[i].is_null()) {
			fail("LRT Screen Gather textures could not be allocated.");
			return;
		}
	}
	RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	for (uint32_t view = 0; view < rb->get_view_count(); view++) {
		LRTProfileScope view_profile("LRT View [" + itos(view) + "]", "Finalize");
		view_profile.stage("Prepare Dispatch");
		struct {
			float inv_projection[16];
			float to_world[16];
			int32_t full_size_view[4] = {};
			int32_t low_size[2] = {};
			float exposure = 1.0f;
			uint32_t observed = 0;
		} parameters;
		RendererRD::MaterialStorage::store_camera(p_render_data->scene_data->get_view_projection(view).inverse(), parameters.inv_projection);
		Transform3D transform = p_render_data->scene_data->cam_transform;
		transform.origin += transform.basis.xform(p_render_data->scene_data->view_eye_offset[view]);
		RendererRD::MaterialStorage::store_transform(transform, parameters.to_world);
		parameters.full_size_view[0] = full_size.x;
		parameters.full_size_view[1] = full_size.y;
		parameters.full_size_view[2] = view;
		parameters.low_size[0] = low_size.x;
		parameters.low_size[1] = low_size.y;
		if (p_render_data->camera_attributes.is_valid()) {
			parameters.exposure = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}
		const bool debug_indirect = RendererSceneRenderRD::get_singleton()->get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_LRT_INDIRECT;
		for (int pass = 0; pass < (debug_indirect ? 2 : 1); pass++) {
			parameters.observed = pass;
			if (pass == 1 && !rb->has_texture("lrt", "debug_lighting")) {
				rb->create_texture_from_format("lrt", "debug_lighting", rb->get_texture_format("lrt", "lighting"));
			}
			RID buffer = rd->uniform_buffer_create(sizeof(parameters));
			if (buffer.is_null()) {
				fail("LRT Screen Gather parameters could not be allocated.");
				return;
			}
			rd->buffer_update(buffer, 0, sizeof(parameters), &parameters);
			Vector<RD::Uniform> uniforms;
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { draw->metadata }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { draw->samples }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { sampler, rb->get_depth_texture(view) }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { sampler, p_normals[view] }));
			for (int i = 0; i < 3; i++) {
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4 + i, { i == 0 && pass == 1 ? rb->get_texture("lrt", "debug_lighting") : outputs[i] }));
			}
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, { buffer }));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, { sampler, rb->get_texture_slice("forward_clustered", "lrt_surface_profile", view, 0) }));
			RID set = rd->uniform_set_create(uniforms, state->gather_shader.version_get_shader(state->gather_version, 0), 0);
			if (set.is_null()) {
				rd->free_rid(buffer);
				fail("LRT Screen Gather resources could not be bound.");
				return;
			}
			view_profile.stage("Gather Probes");
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, state->gather_pipeline);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_dispatch(list, (low_size.x + 7) / 8, (low_size.y + 7) / 8, 1);
			rd->compute_list_end();
			view_profile.stage("Finalize");
			rd->free_rid(set);
			rd->free_rid(buffer);
		}
	}
	for (const Variant &item : state->descriptions) {
		Dictionary description = item;
		if (RID(description["scenario"]) != state->scenario) {
			continue;
		}
		MutexLock lock(status_mutex);
		Dictionary &status = statuses[ObjectID(uint64_t(description["id"]))];
		status["gather_size"] = low_size;
		status["gather_cpu_usec"] = OS::get_singleton()->get_ticks_usec() - cpu_start;
		const double *timing = state->gather_timings.getptr(rb->get_instance_id());
		status["gather_gpu_usec"] = timing ? *timing : 0.0;
		status["gather_timing_frame"] = state->profile_frame;
		status["gather_bytes"] = uint64_t(low_size.x * low_size.y) * rb->get_view_count() * 28 + uint64_t(full_size.x * full_size.y) * rb->get_view_count() * 4;
	}
#endif
}

bool LRTRenderer::_inject(uint64_t p_volume, RID p_lights, uint32_t p_light_count, RID p_sky, RID p_shadow, RID p_directional) {
#ifdef RD_ENABLED
	if (!state->init_injection()) {
		return false;
	}
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
	const RID buffers[] = { volume.geometry, volume.emission, volume.history[volume.history_index[0]], volume.sources, p_lights, p_sky };
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
		uint32_t params[4] = { offset, MIN(count, offset + max_threads), 0u, 0 };
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
	if (p_render_data->reflection_probe.is_valid()) {
		return;
	}
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
	if (state->iterations < 1) {
		for (int64_t id : ids) {
			_fail(id, "LRT propagation requires positive iterations.");
		}
		return;
	}
	if (!state->init_sky()) {
		for (int64_t id : ids) {
			_fail(id, "LRT sky compute pipeline could not be created.");
		}
		return;
	}
	State::Draw &draw = state->draws[state->scenario];
	if (draw.frame == state->frame) {
		return;
	}
	HashSet<uint64_t> blocked;
	for (int i = 0; i < ids.size(); i++) {
		const uint64_t id = ids[i];
		Dictionary description = descriptions[i];
		bool unavailable = false;
		const Vector<RID> *related = draw.positional_by_volume.getptr(id);
		if (related) {
			for (RID source : *related) {
				State::PositionalLight **entry = draw.positional_lights.getptr(source);
				if (!entry || ((*entry)->shadowed && (draw.positional_error != OK || !(*entry)->completed_valid || (*entry)->completed_versions != (*entry)->input_versions))) {
					unavailable = true;
					break;
				}
			}
		}
		if (bool(description["detail"]) && blocked.has(uint64_t(description["group"]))) {
			unavailable = true;
		}
		if (!unavailable && !blocked.is_empty() && !bool(description["detail"])) {
			State::Volume *volume = state->volumes.getptr(id);
			const Transform3D transform = description["transform"];
			const Vector3 size = description["size"];
			const float spacing = description["spacing"];
			for (int axis = 0; axis < 3 && !unavailable; axis++) {
				const int u = (axis + 1) % 3;
				const int v = (axis + 2) % 3;
				for (int side = 0; side < 2 && !unavailable; side++) {
					Vector3i coordinate;
					coordinate[axis] = side ? volume->grid[axis] : -1;
					for (int b = -1; b <= volume->grid[v] && !unavailable; b++) {
						coordinate[v] = b;
						for (int a = -1; a <= volume->grid[u] && !unavailable; a++) {
							coordinate[u] = a;
							const Vector3 world = transform.xform(Vector3(coordinate) * spacing - size * 0.5);
							for (int parent = i - 1; parent >= 0; parent--) {
								Dictionary candidate = descriptions[parent];
								if (bool(candidate["detail"]) || int(candidate["priority"]) >= int(description["priority"])) {
									continue;
								}
								const Transform3D source_transform = candidate["transform"];
								const Vector3 source_size = candidate["size"];
								const Vector3 p = source_transform.affine_inverse().xform(world).abs();
								if (p.x <= source_size.x * 0.5 && p.y <= source_size.y * 0.5 && p.z <= source_size.z * 0.5) {
									unavailable = blocked.has(uint64_t(ids[parent]));
									break;
								}
							}
						}
					}
				}
			}
		}
		State::Volume *volume = state->volumes.getptr(id);
		if (unavailable) {
			blocked.insert(id);
			volume->shadow_blocked = true;
			volume->recovering = false;
			MutexLock lock(status_mutex);
			Dictionary &status = statuses[ObjectID(id)];
			status["state"] = "waiting";
			status["error"] = "LRT positional shadow or boundary dependency is unavailable.";
		} else if (volume->shadow_blocked) {
			volume->shadow_blocked = false;
			volume->recovering = true;
		}
	}
	for (int i = ids.size() - 1; i >= 0; i--) {
		if (blocked.has(uint64_t(ids[i]))) {
			ids.remove_at(i);
			descriptions.remove_at(i);
		}
	}
	RD *rd = RD::get_singleton();
	if (rd->get_captured_timestamps_frame() != state->profile_frame) {
		state->profile_frame = rd->get_captured_timestamps_frame();
		state->gather_timings.clear();
		state->world_timings.clear();
		struct ProfileScope {
			String name;
			uint64_t id;
			uint64_t start;
			double a = 0.0;
			double l = 0.0;
			double inject = 0.0;
			String phase;
		};
		LocalVector<ProfileScope> scopes;
		for (uint32_t i = 0; i < rd->get_captured_timestamps_count(); i++) {
			const String label = rd->get_captured_timestamp_name(i);
			const uint64_t time = rd->get_captured_timestamp_gpu_time(i);
			if (label.begins_with(">LRT ")) {
				ProfileScope scope;
				scope.name = label.substr(1).get_slice("[", 0).strip_edges();
				scope.id = label.get_slice("[", 1).to_int();
				scope.start = time;
				scopes.push_back(scope);
				if (scope.name == "LRT Geometry Upload") {
					MutexLock lock(status_mutex);
					if (statuses.has(ObjectID(scope.id))) {
						Dictionary &status = statuses[ObjectID(scope.id)];
						if (uint64_t(status.get("upload_timing_frame", UINT64_MAX)) != state->profile_frame) {
							status["upload_gpu_usec"] = 0.0;
							status["scatter_gpu_usec"] = 0.0;
							status["upload_timing_frame"] = state->profile_frame;
						}
					}
				}
				continue;
			}
			if (scopes.is_empty()) {
				continue;
			}
			ProfileScope &scope = scopes[scopes.size() - 1];
			if (label.begins_with("<LRT ")) {
				const double usec = double(time - scope.start) / 1000.0;
				if (scope.name == "LRT Screen Gather") {
					state->gather_timings[scope.id] = usec;
				} else if (scope.name == "LRT World Update" || scope.name == "LRT Sun Shadows" || scope.name == "LRT Positional Atlas Shadows" || scope.name == "LRT Positional Cube Shadows") {
					Dictionary &timings = state->world_timings[scope.id];
					String field;
					if (scope.name == "LRT World Update") {
						field = "update_gpu_usec";
					} else if (scope.name == "LRT Sun Shadows") {
						field = "sun_gpu_usec";
					} else {
						field = scope.name == "LRT Positional Atlas Shadows" ? "positional_atlas_gpu_usec" : "positional_cube_gpu_usec";
					}
					timings[field] = double(timings.get(field, 0.0)) + usec;
				} else if (scope.name == "LRT Volume" || scope.name == "LRT Geometry Upload") {
					MutexLock lock(status_mutex);
					if (statuses.has(ObjectID(scope.id))) {
						Dictionary &status = statuses[ObjectID(scope.id)];
						if (scope.name == "LRT Volume") {
							status["gpu_usec"] = usec;
							status["a_gpu_usec"] = scope.a;
							status["l_gpu_usec"] = scope.l;
							status["inject_gpu_usec"] = scope.inject;
							status["gpu_timing_phase"] = scope.phase;
							status["gpu_timing_frame"] = state->profile_frame;
						} else {
							status["upload_gpu_usec"] = double(status.get("upload_gpu_usec", 0.0)) + usec;
						}
					}
				}
				scopes.remove_at(scopes.size() - 1);
				continue;
			}
			if (i + 1 == rd->get_captured_timestamps_count()) {
				continue;
			}
			const double usec = double(rd->get_captured_timestamp_gpu_time(i + 1) - time) / 1000.0;
			if (scope.name == "LRT World Update" && label == "Sky SH Projection") {
				state->world_timings[scope.id]["sky_gpu_usec"] = usec;
			} else if (scope.name == "LRT Volume" || scope.name == "LRT Geometry Upload") {
				String field;
				if (label == "Visibility Propagation A") {
					field = "a_gpu_usec";
				} else if (label == "Light Injection") {
					field = "inject_gpu_usec";
				} else if (label == "Light Propagation L") {
					field = "l_gpu_usec";
				} else if (label == "Scatter Geometry") {
					field = "scatter_gpu_usec";
				}
				if (scope.name == "LRT Volume") {
					if (field == "a_gpu_usec") {
						scope.a = usec;
						scope.phase = "A";
					} else if (field == "l_gpu_usec") {
						scope.l = usec;
						scope.phase = "L";
					} else if (field == "inject_gpu_usec") {
						scope.inject = usec;
					}
				} else if (!field.is_empty()) {
					MutexLock lock(status_mutex);
					if (statuses.has(ObjectID(scope.id))) {
						Dictionary &status = statuses[ObjectID(scope.id)];
						status[field] = usec + (field == "scatter_gpu_usec" ? double(status.get(field, 0.0)) : 0.0);
					}
				}
			}
		}
	}
	const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
	LRTProfileScope profile("LRT World Update [" + itos(state->scenario.get_id()) + "]", "Pre Opaque Render");
	profile.stage("Prepare Inputs");
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
		float sun_coverage[4] = {};
	};
	Vector<Light> lights;
	Vector<RID> injection_instances;
	for (RID instance : state->scene_lights) {
		if (storage->light_get_type(storage->light_instance_get_base_light(instance)) == RSE::LIGHT_DIRECTIONAL) {
			injection_instances.push_back(instance);
		}
	}
	for (RID source : draw.positional_candidates) {
		State::PositionalLight **entry = draw.positional_lights.getptr(source);
		if (!entry) {
			continue;
		}
		State::PositionalLight *light = *entry;
		if (!light->shadowed) {
			injection_instances.push_back(light->source_instance);
		} else if (draw.positional_error == OK && light->completed_valid && light->completed_versions == light->input_versions) {
			injection_instances.push_back(light->shadow_instance);
		}
	}
	for (RID instance : injection_instances) {
		RID base = storage->light_instance_get_base_light(instance);
		RSE::LightType type = storage->light_get_type(base);
		if (storage->light_get_bake_mode(base) == RSE::LIGHT_BAKE_DISABLED || type == RSE::LIGHT_AREA || storage->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY) == 0) {
			continue;
		}
		if (type == RSE::LIGHT_DIRECTIONAL && storage->light_directional_get_sky_mode(base) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			continue;
		}
		bool shadows = storage->light_has_shadow(base);
		bool positional_shadow = shadows && type != RSE::LIGHT_DIRECTIONAL;
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
			const State::SunShadow *sun = nullptr;
			for (const State::SunShadow &candidate : draw.sun_shadows) {
				if (candidate.light == instance && candidate.ready && draw.sun_frame == state->frame) {
					sun = &candidate;
					break;
				}
			}
			if (!sun) {
				for (int64_t id : ids) {
					_fail(id, "LRT directional shadow data is unavailable for this frame.");
				}
				return;
			}
			light.spot_shadow[2] = 1;
			Rect2 rect(Vector2(sun->rect.position) / draw.sun_resolution, Vector2(sun->rect.size) / draw.sun_resolution);
			Projection atlas;
			atlas.set_light_atlas_rect(rect);
			Projection matrix = atlas * bias * correction * sun->projection * Projection(sun->transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(matrix, light.matrices[0]);
			light.rects[0][0] = rect.position.x;
			light.rects[0][1] = rect.position.y;
			light.rects[0][2] = rect.size.x;
			light.rects[0][3] = rect.size.y;
			light.bias[0] = sun->bias;
			for (int axis = 0; axis < 3; axis++) {
				light.sun_coverage[axis] = sun->center[axis];
			}
			light.sun_coverage[3] = sun->extent;
		} else if (positional_shadow) {
			light.spot_shadow[2] = 1;
			Vector2i offset;
			Rect2 rect = storage->light_instance_get_shadow_atlas_rect(instance, draw.positional_atlas, offset);
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
	uint64_t sky_cache_bytes = 0;
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
				radiance = scene->get_sky()->sky_get_unexposed_radiance_texture_rd(sky);
				if (radiance.is_null()) {
					rd->free_rid(light_buffer);
					for (int64_t id : ids) {
						_fail(id, "LRT requires the unexposed sky material output.");
					}
					return;
				}
				RD::TextureFormat sky_format = rd->texture_get_format(radiance);
				sky_cache_bytes = uint64_t(sky_format.width) * sky_format.height * 21;
				float energy = scene->environment_get_bg_energy_multiplier(env) * scene->environment_get_bg_intensity(env);
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
		radiance = textures->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
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
	profile.stage("Sky SH Projection");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, state->sky_pipeline);
	rd->compute_list_bind_uniform_set(list, sky_set, 0);
	rd->compute_list_set_push_constant(list, &sky_params, sizeof(sky_params));
	rd->compute_list_dispatch(list, 1, 1, 1);
	rd->compute_list_end();
	profile.stage("Prepare Sampling Metadata");
	rd->free_rid(sky_set);
	state->injection_lights = light_buffer;
	state->injection_light_count = lights.size();
	state->injection_sky = draw.sky;
	RID shadow = storage->owns_shadow_atlas(draw.positional_atlas) ? storage->shadow_atlas_get_texture(draw.positional_atlas) : RID();
	state->injection_shadow = shadow.is_valid() ? shadow : textures->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
	RID directional = draw.sun_texture;
	state->injection_directional = directional.is_valid() ? directional : textures->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
	struct Metadata {
		float to_local[16];
		uint32_t grid_geometry[4];
		float size_spacing[4];
		float display[4] = {};
		uint32_t history[4] = {};
		uint32_t receiver[4] = {};
	};
	Vector<Metadata> entries;
	// Profiles share the sampling buffer with Probe fields. All draws and
	// instance IDs use the same frame snapshot, including isolated diagnostics.
	uint64_t sample_bytes = state->surface_profile_data.size();
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
		const uint64_t selection = state->debug.get("selection", uint64_t(0));
		item.display[2] = !bool(state->debug.get("volume_selection", false)) || uint64_t(description["id"]) == selection ||
						(!bool(state->debug.get("detail_selection", false)) && uint64_t(description["group"]) == selection)
				? 1.0f
				: 0.0f;
		item.display[3] = bool(state->debug.get("detail_selection", false)) ? 1.0f : 0.0f;
		item.history[1] = int(description["priority"]);
		item.history[3] = hash_murmur3_one_64(uint64_t(description["group"]));
		const uint64_t group_id = description["group"];
		item.receiver[0] = uint32_t(group_id);
		item.receiver[1] = uint32_t(group_id >> 32);
		item.receiver[2] = bool(description["public"]);
		if (bool(description["detail"])) {
			const int parent_index = ids.find(int64_t(description["group"]));
			ERR_FAIL_COND(parent_index < 0);
			item.history[2] = parent_index + 1;
		}
		uint64_t count = uint64_t(volume->grid.x) * volume->grid.y * volume->grid.z;
		item.grid_geometry[3] = sample_bytes / 4;
		sample_bytes += count * SAMPLE_FLAGS_STRIDE;
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
	encode_uint32(state->surface_profile_data.size() / 16, bytes.ptrw() + 4);
	if (!entries.is_empty()) {
		memcpy(bytes.ptrw() + 64, entries.ptr(), entries.size() * sizeof(Metadata));
	}
	rd->buffer_update(draw.metadata, 0, bytes.size(), bytes.ptr());
	rd->buffer_copy(draw.sky, draw.metadata, 0, 16, 48);
	if (!state->surface_profile_data.is_empty()) {
		rd->buffer_update(draw.samples, 0, state->surface_profile_data.size(), state->surface_profile_data.ptr());
	}
	for (int i = 0; i < ids.size(); i++) {
		const State::Volume *volume = state->volumes.getptr(ids[i]);
		if (!volume) {
			continue;
		}
		MutexLock lock(status_mutex);
		Dictionary &status = statuses[ObjectID(uint64_t(ids[i]))];
		status["injected_lights"] = lights.size();
		status["surface_sampling"] = true;
		status["surface_profiles"] = state->surface_profile_data.size() / 16;
		status["surface_profile_bytes"] = state->surface_profile_data.size();
		status["sampling_bytes"] = draw.sample_bytes + draw.metadata_bytes + 48;
		status["sky_cache_bytes"] = sky_cache_bytes;
	}
	bool recovering = false;
	bool recovery_cleared = true;
	for (int i = 0; i < ids.size(); i++) {
		State::Volume *volume = state->volumes.getptr(uint64_t(ids[i]));
		if (!volume->recovering) {
			continue;
		}
		recovering = true;
		const uint32_t count = volume->grid.x * volume->grid.y * volume->grid.z;
		recovery_cleared &= rd->buffer_clear(volume->sources, 0, count * EMISSION_STRIDE) == OK;
		for (RID history : volume->history) {
			recovery_cleared &= rd->buffer_clear(history, 0, count * 3 * 16) == OK;
		}
		recovery_cleared &= rd->buffer_clear(draw.samples, entries[i].grid_geometry[3] * 4, count * (SAMPLE_FLAGS_STRIDE + HISTORY_STRIDE)) == OK;
		volume->history_index[1] = 0;
		volume->injected = false;
		volume->propagation_steps = 0;
	}
	bool propagated = recovery_cleared && _propagate(ids, state->iterations, state->frame, draw.next_phase, false);
	if (propagated && recovering) {
		propagated = _propagate(ids, state->iterations, state->frame, 0, true) && _propagate(ids, state->iterations, state->frame, 1, true);
	}
	rd->free_rid(light_buffer);
	if (!propagated) {
		for (int64_t id : ids) {
			_fail(id, "LRT propagation or light injection resources could not be created or bound.");
		}
		return;
	}

	for (int64_t id : ids) {
		State::Volume *volume = state->volumes.getptr(uint64_t(id));
		volume->recovering = false;
		MutexLock lock(status_mutex);
		Dictionary &status = statuses[ObjectID(uint64_t(id))];
		status["state"] = "ready";
		status.erase("error");
		const Dictionary *timings = state->world_timings.getptr(state->scenario.get_id());
		status["world_gpu_timings"] = timings ? *timings : Dictionary();
		status["world_cpu_submit_usec"] = OS::get_singleton()->get_ticks_usec() - cpu_start;
		status["sun_shadow_bytes"] = uint64_t(draw.sun_resolution) * draw.sun_resolution * 4;
	}
	draw.next_phase ^= 1;
	draw.frame = state->frame;
	_publish_positional_status(state->scenario);
#endif
}

void LRTRenderer::capture_sun_shadow(RID p_framebuffer, bool p_begin) {
#ifdef RD_ENABLED
	if (!state) {
		return;
	}
	for (const KeyValue<RID, State::Draw> &entry : state->draws) {
		if (entry.value.sun_framebuffer == p_framebuffer) {
			RD *rd = RD::get_singleton();
			const String label = "LRT Sun Shadows [" + itos(entry.key.get_id()) + "]";
			rd->capture_timestamp((p_begin ? ">" : "<") + label);
			rd->capture_timestamp(p_begin ? "Shadow Draw" : "Render Shadows");
			return;
		}
	}
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
				draw->clear_sampling();
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
	const Vector<uint8_t> injected = volume->injected ? rd->buffer_get_data(volume->sources) : Vector<uint8_t>();
	const Vector<uint8_t> histories[2] = { rd->buffer_get_data(volume->history[0]), rd->buffer_get_data(volume->history[1]) };
	if (geometry.is_empty() || source_emission.is_empty() || histories[0].is_empty() || histories[1].is_empty()) {
		readback_result["error"] = "LRT GPU buffer readback failed.";
		return;
	}
	const Vector3i origin = p_coordinate * LRTGeometry::TRUNK_SIZE;
	const Vector3i size = (volume->grid - origin).min(Vector3i(8, 8, 8));
	const int count = size.x * size.y * size.z;
	const uint64_t volume_count = uint64_t(volume->grid.x) * volume->grid.y * volume->grid.z;
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
					for (int channel = 0; channel < 3; channel++) {
						memcpy(light[history].ptrw() + target * 12 + channel * 4, histories[history].ptr() + (channel * volume_count + source) * 16, 16);
					}
					memcpy(visibility[history].ptrw() + target * 4, histories[history].ptr() + (3 * volume_count + source) * 16, 16);
				}
			}
		}
	}
	readback_result["coordinate"] = p_coordinate;
	readback_result["origin"] = origin;
	readback_result["size"] = size;
	readback_result["revision"] = volume->revision;
	readback_result["visibility_history_index"] = volume->history_index[0];
	readback_result["light_history_index"] = volume->history_index[1];
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
