/**************************************************************************/
/*  lrt_render_bridge.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "lrt_render_bridge.h"

#include "core/config/engine.h"

#include "lrt_debug.glsl.gen.h"
#include "lrt_screen_gather.glsl.gen.h"
#include "lrt_sampling_inc.glsl.gen.h"
#include "lrt_pack_fields.glsl.gen.h"

#include "core/io/resource.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"

#include <atomic>

namespace {

LRTRenderBridge::State empty_render_state;
HashMap<ObjectID, LRTRenderBridge::State> volume_states;
Vector<LRTRenderBridge::State> view_states;
RID view_scenario;
uint64_t state_revision = 0;
Mutex volumes_mutex;
RID volume_descriptors;
int descriptor_capacity = 0;
RID debug_shader;
PipelineCacheRD *debug_pipeline = nullptr;
RID screen_gather_shader;
RID screen_gather_pipeline;
RID screen_gather_ubo;
RID receiver_fields;
uint64_t receiver_fields_capacity = 0;
RID pack_fields_shader;
RID pack_fields_pipeline;
enum BridgeTimingPass {
	BRIDGE_TIMING_EXTERNAL_GI,
	BRIDGE_TIMING_SCREEN_GATHER,
	BRIDGE_TIMING_DEBUG,
	BRIDGE_TIMING_VOLUME_SHADOW,
	BRIDGE_TIMING_PASS_COUNT,
};
const char *bridge_timing_begin_names[BRIDGE_TIMING_PASS_COUNT] = { "LRT External GI Begin", "LRT Screen Gather Begin", "LRT Debug Begin", "LRT Volume Shadow Begin" };
const char *bridge_timing_end_names[BRIDGE_TIMING_PASS_COUNT] = { "LRT External GI End", "LRT Screen Gather End", "LRT Debug End", "LRT Volume Shadow End" };
std::atomic<double> bridge_gpu_ms[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<double> bridge_render_thread_ms[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<uint64_t> bridge_dispatches[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<int> bridge_timestamp_samples[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<uint64_t> bridge_completed_timestamp_ranges[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<int> screen_gather_last_skip_reason{ 0 };
bool bridge_timestamp_pending[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<bool> bridge_profiling_enabled{ false };
uint64_t last_completed_bridge_timestamp_end[BRIDGE_TIMING_PASS_COUNT]{};
uint64_t bridge_timestamp_pending_result_frame[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<uint64_t> bridge_dropped_timestamp_ranges[BRIDGE_TIMING_PASS_COUNT]{};

struct VolumeShadowPass {
	RID light_instance;
	Projection projection;
	Transform3D transform;
	Projection shadow_matrix;
	float zfar = 0.0f;
	bool use_pancake = false;
	bool reverse_cull = false;
	bool camera_valid = false;
	bool rendered = false;
};
struct VolumeResources {
	VolumeShadowPass shadow;
	RID depth;
	RID framebuffer;
	RID gather_ubo;
	RID boundary_validity;
	int boundary_capacity = 0;
	uint64_t external_capture_count = 0;
	uint64_t external_capture_frame = UINT64_MAX;
	uint64_t boundary_probe_writes = 0;
	bool external_capture_valid = false;
	LRTRenderBridge::ExternalGIStatus external_status = LRTRenderBridge::EXTERNAL_GI_UNAVAILABLE;
	uint64_t dispatches[BRIDGE_TIMING_PASS_COUNT]{};
	uint32_t shadow_instance_count = 0;
	Transform3D light_transform;
	float shadow_radius = 0.0f;
	float shadow_pancake = 0.0f;
};
HashMap<ObjectID, VolumeResources> volume_resources;
Mutex deferred_resolve_mutex;
Vector<Callable> deferred_light_resolves;
std::atomic<uint64_t> deferred_resolve_flushes{ 0 };
std::atomic<uint64_t> volume_positional_redraws{ 0 };
std::atomic<uint64_t> camera_positional_redraws{ 0 };
std::atomic<uint64_t> omni_positional_redraws{ 0 };
std::atomic<uint32_t> omni_shadow_caster_max{ 0 };
Vector3 omni_dp_origin;
std::atomic<uint32_t> omni_dp_points{ 0 };
std::atomic<uint64_t> positional_shadow_sample_requests{ 0 };
std::atomic<uint64_t> positional_shadow_sample_valid{ 0 };
std::atomic<uint64_t> positional_shadow_registered_lights{ 0 };
// 0: valid/none, 1: null scene instance, 2: no atlas, 3: no depth texture,
// 4: scene instance not registered, 5: stale renderer instance,
// 6: renderer instance absent from atlas, 7: incomplete sample data.
std::atomic<int> positional_shadow_sample_last_failure{ 0 };

struct PositionalShadowAtlasState {
	RID atlas;
	RID texture;
	HashMap<RID, RID> light_to_instance;
};
PositionalShadowAtlasState positional_shadow_atlas;

void reset_positional_shadow_atlas_state() {
	positional_shadow_atlas.atlas = RID();
	positional_shadow_atlas.texture = RID();
	positional_shadow_atlas.light_to_instance.clear();
}

void free_volume_shadow_resources(VolumeResources &p_resources) {
	RID &volume_shadow_fb = p_resources.framebuffer;
	RID &volume_shadow_depth = p_resources.depth;
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		volume_shadow_fb = RID();
		volume_shadow_depth = RID();
		return;
	}
	if (volume_shadow_fb.is_valid()) {
		device->free_rid(volume_shadow_fb);
		volume_shadow_fb = RID();
	}
	if (volume_shadow_depth.is_valid()) {
		device->free_rid(volume_shadow_depth);
		volume_shadow_depth = RID();
	}
}

void reset_bridge_timestamp_in_flight() {
	for (int pass = 0; pass < BRIDGE_TIMING_PASS_COUNT; pass++) {
		bridge_timestamp_pending[pass] = false;
	}
}

bool begin_bridge_gpu_timing(RenderingDevice *p_device, BridgeTimingPass p_pass) {
	if (!bridge_profiling_enabled.load() || bridge_timestamp_pending[p_pass]) {
		return false;
	}
	p_device->capture_timestamp(bridge_timing_begin_names[p_pass]);
	bridge_timestamp_pending[p_pass] = true;
	bridge_timestamp_pending_result_frame[p_pass] = p_device->get_captured_timestamps_frame();
	return true;
}

void end_bridge_gpu_timing(RenderingDevice *p_device, BridgeTimingPass p_pass, bool p_active) {
	if (p_active) {
		p_device->capture_timestamp(bridge_timing_end_names[p_pass]);
	}
}



struct ScreenGatherData {
	float inv_projection[16] = {};
	float view_to_world[16] = {};
	int32_t screen_size[4] = {};
};

static_assert(sizeof(ScreenGatherData) == 144);

struct VolumeData {
	float world_to_volume[16] = {};
	float volume_min[4] = {};
	float volume_max[4] = {};
	float grid_min_spacing[4] = {};
	int32_t grid_size_mode[4] = {};
	float atlas_flags[4] = {};
};
static_assert(sizeof(VolumeData) == 144);

VolumeData volume_data(const LRTRenderBridge::State &p_state) {
	VolumeData data;
	RendererRD::MaterialStorage::store_transform(p_state.world_to_volume, data.world_to_volume);
	for (int axis = 0; axis < 3; axis++) {
		data.volume_min[axis] = p_state.volume_min[axis];
		data.volume_max[axis] = p_state.volume_max[axis];
		data.grid_min_spacing[axis] = p_state.grid_min[axis];
		data.grid_size_mode[axis] = p_state.grid_size[axis];
	}
	data.volume_min[3] = p_state.enabled ? 1.0f : 0.0f;
	data.volume_max[3] = p_state.display_blend_enabled ? 1.0f : 0.0f;
	data.grid_min_spacing[3] = p_state.spacing;
	data.atlas_flags[0] = p_state.atlas_size.x;
	data.atlas_flags[1] = p_state.atlas_size.y;
	data.atlas_flags[2] = p_state.blur_sampling ? 1.0f : 0.0f;
	data.atlas_flags[3] = p_state.blend_distance;
	return data;
}

void refresh_view_states() {
	view_states.clear();
	for (const KeyValue<ObjectID, LRTRenderBridge::State> &entry : volume_states) {
		if (entry.value.enabled && entry.value.scenario == view_scenario) {
			view_states.push_back(entry.value);
		}
	}
	auto order = [](const LRTRenderBridge::State &a, const LRTRenderBridge::State &b) {
		if (a.priority != b.priority) {
			return a.priority < b.priority;
		}
		const int count = MIN(a.tree_order.size(), b.tree_order.size());
		for (int i = 0; i < count; i++) {
			if (a.tree_order[i] != b.tree_order[i]) {
				return a.tree_order[i] < b.tree_order[i];
			}
		}
		return a.tree_order.size() < b.tree_order.size();
	};
	view_states.sort_custom<decltype(order)>(order);
	empty_render_state.revision = ++state_revision;
	for (LRTRenderBridge::State &state : view_states) {
		state.revision = state_revision;
	}
}

void update_bridge_gpu_timing(RenderingDevice *p_device) {
	uint64_t begin[BRIDGE_TIMING_PASS_COUNT] = {};
	uint64_t latest_end[BRIDGE_TIMING_PASS_COUNT] = {};
	double totals_ms[BRIDGE_TIMING_PASS_COUNT] = {};
	int samples[BRIDGE_TIMING_PASS_COUNT] = {};
	const uint32_t count = p_device->get_captured_timestamps_count();
	const uint64_t captured_frame = p_device->get_captured_timestamps_frame();
	for (uint32_t index = 0; index < count; index++) {
		const String name = p_device->get_captured_timestamp_name(index);
		for (int pass = 0; pass < BRIDGE_TIMING_PASS_COUNT; pass++) {
			if (name == bridge_timing_begin_names[pass]) {
				begin[pass] = p_device->get_captured_timestamp_gpu_time(index);
			} else if (name == bridge_timing_end_names[pass] && begin[pass] > 0) {
				const uint64_t end = p_device->get_captured_timestamp_gpu_time(index);
				if (end >= begin[pass]) {
					totals_ms[pass] += double(end - begin[pass]) / 1000000.0;
					samples[pass]++;
					latest_end[pass] = end;
				}
				begin[pass] = 0;
			}
		}
	}
	for (int pass = 0; pass < BRIDGE_TIMING_PASS_COUNT; pass++) {
		if (samples[pass] > 0 && latest_end[pass] != last_completed_bridge_timestamp_end[pass]) {
			bridge_gpu_ms[pass].store(totals_ms[pass]);
			bridge_timestamp_samples[pass].store(samples[pass]);
			bridge_completed_timestamp_ranges[pass].fetch_add(uint64_t(samples[pass]));
			bridge_timestamp_pending[pass] = false;
			last_completed_bridge_timestamp_end[pass] = latest_end[pass];
		} else if (bridge_timestamp_pending[pass] &&
				captured_frame >= bridge_timestamp_pending_result_frame[pass] + 4) {
			bridge_timestamp_pending[pass] = false;
			bridge_dropped_timestamp_ranges[pass].fetch_add(1);
		}
	}
}

struct DebugPushConstant {
	float projection[16] = {};
	int32_t grid_size_mode[4] = {};
	float grid_min_spacing[4] = {};
	float volume_min[4] = {};
	float volume_max[4] = {};
};

static_assert(sizeof(DebugPushConstant) == 128);

enum DebugMode {
	DEBUG_MODE_RADIANCE,
	DEBUG_MODE_SOURCE,
	DEBUG_MODE_LOCAL_VISIBILITY,
	DEBUG_MODE_GLOBAL_VISIBILITY,
	DEBUG_MODE_TRANSFER,
	DEBUG_MODE_SDF_SURFACE,
	DEBUG_MODE_ALBEDO,
	DEBUG_MODE_EMISSION,
	DEBUG_MODE_BOUNDARY,
	DEBUG_MODE_UPDATE_REGIONS,
};

enum DebugDrawKind {
	DEBUG_DRAW_LOBES,
	DEBUG_DRAW_VOXELS,
	DEBUG_DRAW_RECEIVERS,
	DEBUG_DRAW_LINKS,
	DEBUG_DRAW_BOUNDARY_BOXES,
};


bool ensure_debug_pipeline() {
	if (debug_pipeline != nullptr) {
		return true;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		return false;
	}
	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();
	if (shader_file->parse_versions_from_text(lrt_debug_shader_glsl) != OK) {
		shader_file->print_errors("LRT viewport debug shader");
		return false;
	}
	debug_shader = device->shader_create_from_spirv(shader_file->get_spirv_stages());
	if (debug_shader.is_null()) {
		return false;
	}
	RD::PipelineRasterizationState rasterization;
	rasterization.cull_mode = RD::POLYGON_CULL_FRONT;
	RD::PipelineDepthStencilState depth_stencil;
	depth_stencil.enable_depth_test = true;
	depth_stencil.enable_depth_write = true;
	depth_stencil.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
	debug_pipeline = memnew(PipelineCacheRD);
	debug_pipeline->setup(debug_shader, RD::RENDER_PRIMITIVE_TRIANGLES, rasterization,
			RD::PipelineMultisampleState(), depth_stencil, RD::PipelineColorBlendState::create_disabled(), 0);
	return true;
}

bool ensure_screen_gather_pipeline() {
	if (screen_gather_pipeline.is_valid()) {
		return true;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		return false;
	}
	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();
	if (shader_file->parse_versions_from_text(String(lrt_screen_gather_shader_glsl).replace("#include \"lrt_sampling_inc.glsl\"", lrt_sampling_inc_shader_glsl)) != OK) {
		shader_file->print_errors("LRT screen gather shader");
		return false;
	}
	screen_gather_shader = device->shader_create_from_spirv(shader_file->get_spirv_stages());
	if (screen_gather_shader.is_null()) {
		return false;
	}
	screen_gather_pipeline = device->compute_pipeline_create(screen_gather_shader);
	screen_gather_ubo = device->uniform_buffer_create(sizeof(ScreenGatherData));
	return screen_gather_pipeline.is_valid() && screen_gather_ubo.is_valid();
}

int debug_mode_index(RSE::ViewportDebugDraw p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_RADIANCE_PROBES:
			return DEBUG_MODE_RADIANCE;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_SOURCE_PROBES:
			return DEBUG_MODE_SOURCE;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_LOCAL_VISIBILITY:
			return DEBUG_MODE_LOCAL_VISIBILITY;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_GLOBAL_VISIBILITY:
			return DEBUG_MODE_GLOBAL_VISIBILITY;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_TRANSFER:
			return DEBUG_MODE_TRANSFER;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_SDF_SURFACE:
			return DEBUG_MODE_SDF_SURFACE;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_ALBEDO:
			return DEBUG_MODE_ALBEDO;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_EMISSION:
			return DEBUG_MODE_EMISSION;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_BOUNDARY:
			return DEBUG_MODE_BOUNDARY;
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_UPDATE_REGIONS:
			return DEBUG_MODE_UPDATE_REGIONS;
		default:
			return -1;
	}
}

void clear_external_gi_buffers(const LRTRenderBridge::State &p_state) {
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr || p_state.grid_size.x <= 0 || p_state.grid_size.y <= 0 || p_state.grid_size.z <= 0) {
		return;
	}
	const uint32_t bytes = uint32_t(p_state.grid_size.x * p_state.grid_size.y * p_state.grid_size.z) * 4 * sizeof(float);
	const RID buffers[3] = { p_state.external_gi_r, p_state.external_gi_g, p_state.external_gi_b };
	for (const RID &buffer : buffers) {
		if (buffer.is_valid()) {
			device->buffer_clear(buffer, 0, bytes);
		}
	}
}

} // namespace

void LRTRenderBridge::reset_positional_shadow_atlas() {
	reset_positional_shadow_atlas_state();
	positional_shadow_registered_lights.store(0);
}

void LRTRenderBridge::set_state(const Dictionary &p_state) {
	State next;
	next.owner = ObjectID(uint64_t(p_state.get("owner", uint64_t(0))));
	next.world_to_volume = p_state.get("world_to_volume", Transform3D());
	next.directional_light_transform = p_state.get("directional_light_transform", Transform3D());
	next.has_directional_light = bool(p_state.get("has_directional_light", false));
	next.volume_min = p_state.get("volume_min", Vector3());
	next.volume_max = p_state.get("volume_max", Vector3());
	next.grid_min = p_state.get("grid_min", Vector3());
	next.grid_size = p_state.get("grid_size", Vector3i());
	next.atlas_size = p_state.get("atlas_size", Vector2(1.0, 1.0));
	next.environment = p_state.get("environment", RID());
	next.spacing = float(p_state.get("spacing", 0.25));
	next.blend_distance = float(p_state.get("blend_distance", 0.0));
	next.blur_sampling = p_state.get("blur_sampling", true);
	next.display_blend_enabled = p_state.get("display_blend_enabled", true);
	next.external_gi_enabled = p_state.get("external_gi_enabled", false);
	next.enabled = p_state.get("enabled", false);
	next.radiance_r = p_state.get("radiance_r", RID());
	next.radiance_g = p_state.get("radiance_g", RID());
	next.radiance_b = p_state.get("radiance_b", RID());
	next.visibility = p_state.get("visibility", RID());
	next.material = p_state.get("material", RID());
	next.links = p_state.get("links", RID());
	next.receiver_links = p_state.get("receiver_links", RID());
	next.sky_r = p_state.get("sky_r", RID());
	next.sky_g = p_state.get("sky_g", RID());
	next.sky_b = p_state.get("sky_b", RID());
	next.source_r = p_state.get("source_r", RID());
	next.source_g = p_state.get("source_g", RID());
	next.source_b = p_state.get("source_b", RID());
	next.local_visibility = p_state.get("local_visibility", RID());
	next.matrices = p_state.get("matrices", RID());
	next.diagnostic_sdf = p_state.get("diagnostic_sdf", RID());
	next.diagnostic_albedo = p_state.get("diagnostic_albedo", RID());
	next.diagnostic_emission = p_state.get("diagnostic_emission", RID());
	next.diagnostic_dirty = p_state.get("diagnostic_dirty", RID());
	next.external_gi_r = p_state.get("external_gi_r", RID());
	next.external_gi_g = p_state.get("external_gi_g", RID());
	next.external_gi_b = p_state.get("external_gi_b", RID());
	next.solver = p_state.get("solver", Ref<LRTVolume>());
	next.volume_shadow_requested = p_state.get("volume_shadow_requested", false);
	next.scenario = p_state.get("scenario", RID());
	next.priority = p_state.get("priority", 0);
	next.tree_order = p_state.get("tree_order", PackedInt32Array());
	MutexLock lock(volumes_mutex);
	volume_states[next.owner] = next;
	VolumeResources &resources = volume_resources[next.owner];
	if (!next.external_gi_enabled) {
		clear_external_gi_buffers(next);
		resources.external_capture_valid = false;
	}
	refresh_view_states();
}

void LRTRenderBridge::clear(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	VolumeResources *resources = volume_resources.getptr(p_owner);
	if (resources == nullptr) {
		return;
	}
	free_volume_shadow_resources(*resources);
	if (resources->boundary_validity.is_valid()) {
		RenderingDevice::get_singleton()->free_rid(resources->boundary_validity);
	}
	if (resources->gather_ubo.is_valid()) {
		RenderingDevice::get_singleton()->free_rid(resources->gather_ubo);
	}
	volume_resources.erase(p_owner);
	volume_states.erase(p_owner);
	refresh_view_states();
	if (volume_states.is_empty()) {
		reset_bridge_timestamp_in_flight();
		reset_positional_shadow_atlas();
	}
}

const LRTRenderBridge::State &LRTRenderBridge::get_state() {
	return view_states.is_empty() ? empty_render_state : view_states[view_states.size() - 1];
}

const Vector<LRTRenderBridge::State> &LRTRenderBridge::get_states() {
	return view_states;
}

void LRTRenderBridge::set_view_scenario(RID p_scenario) {
	if (view_scenario == p_scenario) {
		return;
	}
	MutexLock lock(volumes_mutex);
	view_scenario = p_scenario;
	refresh_view_states();
}

bool LRTRenderBridge::is_current_volume(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	const State *state = volume_states.getptr(p_owner);
	return state != nullptr && state->enabled && state->scenario == view_scenario;
}

RID LRTRenderBridge::get_volume_descriptors() {
	RenderingDevice *device = RenderingDevice::get_singleton();
	const int required = MAX(1, view_states.size());
	if (descriptor_capacity < required) {
		if (volume_descriptors.is_valid()) {
			device->free_rid(volume_descriptors);
		}
		descriptor_capacity = required;
		volume_descriptors = device->storage_buffer_create(required * sizeof(VolumeData));
	}
	Vector<VolumeData> data;
	data.resize(required);
	int probe_offset = 0;
	for (int i = 0; i < view_states.size(); i++) {
		data.write[i] = volume_data(view_states[i]);
		data.write[i].grid_size_mode[3] = probe_offset;
		probe_offset += view_states[i].grid_size.x * view_states[i].grid_size.y * view_states[i].grid_size.z;
	}
	device->buffer_update(volume_descriptors, 0, required * sizeof(VolumeData), data.ptr());
	return volume_descriptors;
}

RID LRTRenderBridge::get_receiver_fields() {
	uint64_t probes = 0;
	for (const State &state : view_states) {
		probes += uint64_t(state.grid_size.x) * state.grid_size.y * state.grid_size.z;
	}
	const uint64_t bytes = MAX(uint64_t(128), probes * 8 * sizeof(float) * 4);
	if (receiver_fields_capacity < bytes) {
		RenderingDevice *device = RenderingDevice::get_singleton();
		if (receiver_fields.is_valid()) {
			device->free_rid(receiver_fields);
		}
		receiver_fields = device->storage_buffer_create(bytes);
		receiver_fields_capacity = bytes;
	}
	return receiver_fields;
}

void LRTRenderBridge::prepare_receiver_fields() {
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (view_states.is_empty()) {
		return;
	}
	if (pack_fields_pipeline.is_null()) {
		Ref<RDShaderFile> file;
		file.instantiate();
		ERR_FAIL_COND(file->parse_versions_from_text(lrt_pack_fields_shader_glsl) != OK);
		pack_fields_shader = device->shader_create_from_spirv(file->get_spirv_stages());
		ERR_FAIL_COND(pack_fields_shader.is_null());
		pack_fields_pipeline = device->compute_pipeline_create(pack_fields_shader);
	}
	const RID output = get_receiver_fields();
	RendererRD::TextureStorage *textures = RendererRD::TextureStorage::get_singleton();
	int32_t offset = 0;
	for (const State &state : view_states) {
		const RID fields[] = { state.radiance_r, state.radiance_g, state.radiance_b, state.material,
			state.receiver_links, state.sky_r, state.sky_g, state.sky_b };
		LocalVector<RD::Uniform> uniforms;
		for (int field = 0; field < 8; field++) {
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, field, textures->texture_get_rd_texture(fields[field])));
		}
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 8, RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, output));
		const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(pack_fields_shader, 0, uniforms);
		const int32_t count = state.grid_size.x * state.grid_size.y * state.grid_size.z;
		const int32_t layout[4] = { offset, state.grid_size.x * state.grid_size.z, count, 0 };
		const RD::ComputeListID list = device->compute_list_begin();
		device->compute_list_bind_compute_pipeline(list, pack_fields_pipeline);
		device->compute_list_bind_uniform_set(list, uniform_set, 0);
		device->compute_list_set_push_constant(list, layout, sizeof(layout));
		device->compute_list_dispatch_threads(list, count, 1, 1);
		device->compute_list_end();
		offset += count;
	}
}

namespace {
bool volume_intersects_view(const LRTRenderBridge::State &p_state, const Vector<Plane> &p_planes) {
	const Transform3D to_world = p_state.world_to_volume.affine_inverse();
	const AABB bounds(p_state.volume_min, p_state.volume_max - p_state.volume_min);
	for (const Plane &plane : p_planes) {
		bool outside = true;
		for (int corner = 0; corner < 8; corner++) {
			if (plane.distance_to(to_world.xform(bounds.get_endpoint(corner))) <= 0.0) {
				outside = false;
				break;
			}
		}
		if (outside) {
			return false;
		}
	}
	return true;
}

Rect2i volume_screen_region(const LRTRenderBridge::State &p_state, const Projection &p_projection, const Transform3D &p_camera_transform, const Size2i &p_size) {
	if (!volume_intersects_view(p_state, p_projection.get_projection_planes(p_camera_transform))) {
		return Rect2i();
	}
	const Projection to_clip = p_projection * Projection(p_camera_transform.affine_inverse() * p_state.world_to_volume.affine_inverse());
	const AABB bounds(p_state.volume_min, p_state.volume_max - p_state.volume_min);
	Vector2 minimum(1.0, 1.0);
	Vector2 maximum(-1.0, -1.0);
	for (int corner = 0; corner < 8; corner++) {
		const Vector3 point = bounds.get_endpoint(corner);
		const Vector4 clip = to_clip.xform(Vector4(point.x, point.y, point.z, 1.0));
		// A volume crossing the eye plane has an unbounded projected box.
		if (clip.w <= 0.0) {
			return Rect2i(Point2i(), p_size);
		}
		const Vector2 ndc(clip.x / clip.w, clip.y / clip.w);
		minimum = minimum.min(ndc);
		maximum = maximum.max(ndc);
	}
	const Vector2 scale(p_size.x * 0.5, p_size.y * 0.5);
	const Point2i begin = Point2i(((minimum + Vector2(1, 1)) * scale).floor()).clamp(Point2i(), p_size);
	const Point2i end = Point2i(((maximum + Vector2(1, 1)) * scale).ceil()).clamp(Point2i(), p_size);
	return Rect2i(begin, end - begin);
}
} // namespace

bool LRTRenderBridge::has_visible_volume(const Projection &p_projection, const Transform3D &p_camera_transform) {
	const Vector<Plane> planes = p_projection.get_projection_planes(p_camera_transform);
	for (const State &state : view_states) {
		if (volume_intersects_view(state, planes)) {
			return true;
		}
	}
	return false;
}

void LRTRenderBridge::debug_draw(RID p_framebuffer, const Projection &p_camera_with_transform, RSE::ViewportDebugDraw p_mode) {
	MutexLock lock(volumes_mutex);
	if (view_states.is_empty()) {
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	update_bridge_gpu_timing(device);
	const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
	const bool timing_active = begin_bridge_gpu_timing(device, BRIDGE_TIMING_DEBUG);
	for (const State &state : view_states) {
		_debug_draw(state, p_framebuffer, p_camera_with_transform, p_mode);
	}
	end_bridge_gpu_timing(device, BRIDGE_TIMING_DEBUG, timing_active);
	bridge_render_thread_ms[BRIDGE_TIMING_DEBUG].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
}

void LRTRenderBridge::_debug_draw(const State &state, RID p_framebuffer, const Projection &p_camera_with_transform, RSE::ViewportDebugDraw p_mode) {
	const int mode = debug_mode_index(p_mode);
	if (mode < 0 || !state.enabled || p_framebuffer.is_null() || state.grid_size.x <= 0 || state.grid_size.y <= 0 || state.grid_size.z <= 0) {
		return;
	}
	if (!ensure_debug_pipeline()) {
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	ERR_FAIL_NULL(device);
	ERR_FAIL_NULL(texture_storage);

	const RID texture_resources[] = {
		state.radiance_r, state.radiance_g, state.radiance_b,
		state.source_r, state.source_g, state.source_b,
		state.visibility, state.local_visibility, state.matrices, state.links,
		state.diagnostic_sdf, state.diagnostic_albedo, state.diagnostic_emission, state.diagnostic_dirty,
		state.material,
	};
	for (const RID &texture : texture_resources) {
		if (texture.is_null()) {
			return;
		}
	}
	// Receiver banks can be replaced between a scene-state publication and this draw.
	// Read the applied bank on the render thread, where allocation and bank switches occur.
	const Dictionary debug_resources = state.solver->get_debug_resources();
	const RID receiver_buffer = debug_resources["receiver_buffer"];
	const int receiver_count = debug_resources["receiver_count"];
	if (state.external_gi_r.is_null() || state.external_gi_g.is_null() || state.external_gi_b.is_null() || receiver_buffer.is_null()) {
		return;
	}

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 0,
			RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
	for (uint32_t index = 0; index < sizeof(texture_resources) / sizeof(texture_resources[0]); index++) {
		const RID texture = texture_storage->texture_get_rd_texture(texture_resources[index]);
		if (texture.is_null()) {
			return;
		}
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, index + 1, texture));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, state.external_gi_r));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, state.external_gi_g));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, state.external_gi_b));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, receiver_buffer));
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(debug_shader, 0, uniforms);
	if (uniform_set.is_null()) {
		return;
	}

	DebugPushConstant push_constant;
	const Projection projection = p_camera_with_transform * Projection(state.world_to_volume.affine_inverse());
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			push_constant.projection[column * 4 + row] = projection.columns[column][row];
		}
	}
	push_constant.grid_size_mode[0] = state.grid_size.x;
	push_constant.grid_size_mode[1] = state.grid_size.y;
	push_constant.grid_size_mode[2] = state.grid_size.z;
	push_constant.grid_min_spacing[0] = state.grid_min.x;
	push_constant.grid_min_spacing[1] = state.grid_min.y;
	push_constant.grid_min_spacing[2] = state.grid_min.z;
	push_constant.grid_min_spacing[3] = state.spacing;
	push_constant.volume_min[0] = state.volume_min.x;
	push_constant.volume_min[1] = state.volume_min.y;
	push_constant.volume_min[2] = state.volume_min.z;
	push_constant.volume_min[3] = state.blend_distance;
	push_constant.volume_max[0] = state.volume_max.x;
	push_constant.volume_max[1] = state.volume_max.y;
	push_constant.volume_max[2] = state.volume_max.z;

	const int grid_count = state.grid_size.x * state.grid_size.y * state.grid_size.z;
	RD::DrawListID draw_list = device->draw_list_begin(p_framebuffer);
	device->draw_command_begin_label("LRT Viewport Debug");
	device->draw_list_bind_render_pipeline(draw_list,
			debug_pipeline->get_render_pipeline(RD::INVALID_ID, device->framebuffer_get_format(p_framebuffer)));
	device->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
	auto draw = [&](DebugDrawKind p_kind, uint32_t p_instances, uint32_t p_vertices) {
		if (p_instances == 0) {
			return;
		}
		push_constant.grid_size_mode[3] = mode | (p_kind << 8);
		device->draw_list_set_push_constant(draw_list, &push_constant, sizeof(push_constant));
		device->draw_list_draw(draw_list, false, p_instances, p_vertices);
	};
	if (mode == DEBUG_MODE_SDF_SURFACE) {
		draw(DEBUG_DRAW_VOXELS, grid_count, 36);
		draw(DEBUG_DRAW_RECEIVERS, receiver_count, 288);
	} else if (mode == DEBUG_MODE_ALBEDO || mode == DEBUG_MODE_EMISSION || mode == DEBUG_MODE_UPDATE_REGIONS) {
		draw(DEBUG_DRAW_VOXELS, grid_count, 36);
	} else {
		if (mode != DEBUG_MODE_BOUNDARY || state.external_gi_enabled) {
			draw(DEBUG_DRAW_LOBES, grid_count, 288);
		}
		if (mode == DEBUG_MODE_LOCAL_VISIBILITY) {
			draw(DEBUG_DRAW_LINKS, grid_count * 26, 36);
		} else if (mode == DEBUG_MODE_BOUNDARY) {
			draw(DEBUG_DRAW_BOUNDARY_BOXES, 2, 432);
		}
	}
	device->draw_command_end_label();
	device->draw_list_end();
	bridge_dispatches[BRIDGE_TIMING_DEBUG].fetch_add(1);
	volume_resources[state.owner].dispatches[BRIDGE_TIMING_DEBUG]++;
}

void LRTRenderBridge::capture_external_gi(RID p_environment, RendererRD::DiffuseGIProvider *p_provider, const Vector3 &p_camera_origin) {
	MutexLock lock(volumes_mutex);
	if (view_states.is_empty()) {
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	update_bridge_gpu_timing(device);
	const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
	const bool timing_active = begin_bridge_gpu_timing(device, BRIDGE_TIMING_EXTERNAL_GI);
	for (const State &state : view_states) {
		_capture_external_gi(state, p_environment, p_provider, p_camera_origin);
	}
	end_bridge_gpu_timing(device, BRIDGE_TIMING_EXTERNAL_GI, timing_active);
	bridge_render_thread_ms[BRIDGE_TIMING_EXTERNAL_GI].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
}

void LRTRenderBridge::_capture_external_gi(const State &state, RID p_environment, RendererRD::DiffuseGIProvider *p_provider, const Vector3 &p_camera_origin) {
	VolumeResources &resources = volume_resources[state.owner];
	if (p_environment != state.environment || !state.external_gi_enabled) {
		return;
	}
	const uint64_t frame = Engine::get_singleton()->get_frames_drawn();
	if (resources.external_capture_frame == frame) {
		return;
	}
	if (state.external_gi_r.is_null() || state.external_gi_g.is_null() || state.external_gi_b.is_null()) {
		resources.external_capture_valid = false;
		return;
	}
	if (p_provider == nullptr || p_provider->includes_sky()) {
		resources.external_status = p_provider == nullptr ? EXTERNAL_GI_UNAVAILABLE : EXTERNAL_GI_SKY_INCOMPATIBLE;
		clear_external_gi_buffers(state);
		resources.external_capture_valid = false;
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	const int probe_count = state.grid_size.x * state.grid_size.y * state.grid_size.z;
	const int boundary_count = probe_count - MAX(0, state.grid_size.x - 2) * MAX(0, state.grid_size.y - 2) * MAX(0, state.grid_size.z - 2);
	if (resources.boundary_capacity != probe_count) {
		if (resources.boundary_validity.is_valid()) {
			device->free_rid(resources.boundary_validity);
		}
		resources.boundary_validity = device->storage_buffer_create(probe_count * sizeof(uint32_t));
		resources.boundary_capacity = probe_count;
	}
	RendererRD::DiffuseGIProvider::BoundaryRequest request;
	request.sample_to_world = state.world_to_volume.affine_inverse();
	request.grid_min = state.grid_min;
	request.grid_size = state.grid_size;
	request.spacing = state.spacing;
	request.coefficients[0] = state.external_gi_r;
	request.coefficients[1] = state.external_gi_g;
	request.coefficients[2] = state.external_gi_b;
	request.validity = resources.boundary_validity;
	if (!p_provider->sample_boundary(request, p_camera_origin)) {
		resources.external_status = EXTERNAL_GI_FAILED;
		clear_external_gi_buffers(state);
		resources.external_capture_valid = false;
		return;
	}
	bridge_dispatches[BRIDGE_TIMING_EXTERNAL_GI].fetch_add(1);
	resources.dispatches[BRIDGE_TIMING_EXTERNAL_GI]++;
	resources.external_capture_frame = frame;
	resources.external_capture_count++;
	resources.boundary_probe_writes += uint64_t(boundary_count);
	resources.external_capture_valid = true;
	resources.external_status = EXTERNAL_GI_READY;
}

bool LRTRenderBridge::gather_screen(RID p_depth, RID p_normal_roughness,
		RID p_lighting_output, RID p_geometry_output, const Size2i &p_full_size,
		const Projection &p_projection, const Transform3D &p_camera_transform) {
	MutexLock lock(volumes_mutex);
	if (view_states.is_empty()) {
		return false;
	}
	if (!ensure_screen_gather_pipeline()) {
		return false;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	update_bridge_gpu_timing(device);
	const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
	const bool timing_active = begin_bridge_gpu_timing(device, BRIDGE_TIMING_SCREEN_GATHER);
	device->texture_clear(p_lighting_output, Color(0, 0, 0, 0), 0, 1, 0, 1);
	device->texture_clear(p_geometry_output, Color(0, 0, 0, 0), 0, 1, 0, 1);
	bool first_volume = true;
	for (const State &state : view_states) {
		VolumeResources &resources = volume_resources[state.owner];
		if (resources.gather_ubo.is_null()) {
			resources.gather_ubo = device->uniform_buffer_create(sizeof(VolumeData));
		}
		const VolumeData data = volume_data(state);
		device->buffer_update(resources.gather_ubo, 0, sizeof(VolumeData), &data);
		if (!_gather_volume(state, resources.gather_ubo, first_volume, p_depth, p_normal_roughness,
				p_lighting_output, p_geometry_output, p_full_size, p_projection, p_camera_transform)) {
			end_bridge_gpu_timing(device, BRIDGE_TIMING_SCREEN_GATHER, timing_active);
			return false;
		}
		first_volume = false;
	}
	end_bridge_gpu_timing(device, BRIDGE_TIMING_SCREEN_GATHER, timing_active);
	bridge_render_thread_ms[BRIDGE_TIMING_SCREEN_GATHER].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
	return true;
}

bool LRTRenderBridge::_gather_volume(const State &state, RID p_lrt_ubo, bool p_first,
		RID p_depth, RID p_normal_roughness, RID p_lighting_output, RID p_geometry_output,
		const Size2i &p_full_size, const Projection &p_projection, const Transform3D &p_camera_transform) {
	if (!state.enabled || p_full_size.x <= 0 || p_full_size.y <= 0 || p_lrt_ubo.is_null() ||
			p_depth.is_null() || p_normal_roughness.is_null() || p_lighting_output.is_null() || p_geometry_output.is_null()) {
		screen_gather_last_skip_reason.store(1);
		return false;
	}
	if (!ensure_screen_gather_pipeline()) {
		screen_gather_last_skip_reason.store(2);
		return false;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	ERR_FAIL_NULL_V(device, false);
	ERR_FAIL_NULL_V(texture_storage, false);

	const RID textures[] = {
		state.radiance_r, state.radiance_g, state.radiance_b,
		state.material, state.receiver_links,
		state.sky_r, state.sky_g, state.sky_b,
	};
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, screen_gather_ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, p_lrt_ubo));
	for (uint32_t index = 0; index < sizeof(textures) / sizeof(textures[0]); index++) {
		const RID texture = textures[index].is_valid() ? texture_storage->texture_get_rd_texture(textures[index]) : RID();
		if (texture.is_null()) {
			screen_gather_last_skip_reason.store(10 + int(index));
			return false;
		}
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, index + 2, texture));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 10, p_depth));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 11, p_normal_roughness));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, p_lighting_output));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, p_geometry_output));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 14,
			RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(screen_gather_shader, 0, uniforms);
	if (uniform_set.is_null()) {
		screen_gather_last_skip_reason.store(30);
		return false;
	}

	ScreenGatherData gather_data;
	RendererRD::MaterialStorage::store_camera(p_projection.inverse(), gather_data.inv_projection);
	RendererRD::MaterialStorage::store_transform(p_camera_transform, gather_data.view_to_world);
	const Size2i gather_size = p_full_size;
	const Rect2i region = volume_screen_region(state, p_projection, p_camera_transform, gather_size);
	if (!region.has_area()) {
		return true;
	}
	gather_data.screen_size[0] = p_full_size.x;
	gather_data.screen_size[1] = p_full_size.y;
	gather_data.screen_size[2] = gather_size.x;
	gather_data.screen_size[3] = gather_size.y;
	device->buffer_update(screen_gather_ubo, 0, sizeof(ScreenGatherData), &gather_data);

	RD::ComputeListID list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(list, screen_gather_pipeline);
	device->compute_list_bind_uniform_set(list, uniform_set, 0);
	const int32_t first_volume[8] = { p_first ? 1 : 0, 0, 0, 0, region.position.x, region.position.y, region.size.x, region.size.y };
	device->compute_list_set_push_constant(list, first_volume, sizeof(first_volume));
	device->compute_list_dispatch_threads(list, region.size.x, region.size.y, 1);
	device->compute_list_end();
	bridge_dispatches[BRIDGE_TIMING_SCREEN_GATHER].fetch_add(1);
	volume_resources[state.owner].dispatches[BRIDGE_TIMING_SCREEN_GATHER]++;
	screen_gather_last_skip_reason.store(0);
	return true;
}

LRTRenderBridge::ExternalGIStatus LRTRenderBridge::get_external_gi_status(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	const VolumeResources *resources = volume_resources.getptr(p_owner);
	return resources != nullptr ? resources->external_status : EXTERNAL_GI_UNAVAILABLE;
}

uint64_t LRTRenderBridge::get_external_gi_capture_count(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	const VolumeResources *resources = volume_resources.getptr(p_owner);
	return resources != nullptr ? resources->external_capture_count : 0;
}

bool LRTRenderBridge::is_external_gi_capture_valid(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	const VolumeResources *resources = volume_resources.getptr(p_owner);
	return resources != nullptr && resources->external_capture_valid;
}

void LRTRenderBridge::set_performance_profiling_enabled(bool p_enabled) {
	bridge_profiling_enabled.store(p_enabled);
}

Dictionary LRTRenderBridge::get_performance_stats(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	Dictionary result;
	const State *state = volume_states.getptr(p_owner);
	const VolumeResources *resources = volume_resources.getptr(p_owner);
	if (state == nullptr || resources == nullptr) {
		result["owner_matches"] = false;
		result["screen_gather_dispatches"] = int64_t(0);
		result["debug_dispatches"] = int64_t(0);
		result["external_gi_dispatches"] = int64_t(0);
		result["volume_shadow_valid"] = false;
		return result;
	}
	const VolumeShadowPass &volume_shadow_pass = resources->shadow;
	result["owner_matches"] = true;
	result["gpu_timings_scope"] = "all_volumes_in_view";
	result["external_gi_gpu_ms"] = bridge_timestamp_samples[BRIDGE_TIMING_EXTERNAL_GI].load() > 0 ? bridge_gpu_ms[BRIDGE_TIMING_EXTERNAL_GI].load() : -1.0;
	result["external_gi_render_thread_ms"] = bridge_render_thread_ms[BRIDGE_TIMING_EXTERNAL_GI].load();
	result["external_gi_dispatches"] = resources->dispatches[BRIDGE_TIMING_EXTERNAL_GI];
	result["external_gi_timestamp_samples"] = bridge_timestamp_samples[BRIDGE_TIMING_EXTERNAL_GI].load();
	result["external_gi_completed_timestamp_ranges"] = bridge_completed_timestamp_ranges[BRIDGE_TIMING_EXTERNAL_GI].load();
	result["external_gi_dropped_timestamp_ranges"] = bridge_dropped_timestamp_ranges[BRIDGE_TIMING_EXTERNAL_GI].load();
	result["external_gi_boundary_probe_writes"] = int64_t(resources->boundary_probe_writes);
	result["screen_gather_gpu_ms"] = bridge_timestamp_samples[BRIDGE_TIMING_SCREEN_GATHER].load() > 0 ? bridge_gpu_ms[BRIDGE_TIMING_SCREEN_GATHER].load() : -1.0;
	result["screen_gather_render_thread_ms"] = bridge_render_thread_ms[BRIDGE_TIMING_SCREEN_GATHER].load();
	result["screen_gather_dispatches"] = resources->dispatches[BRIDGE_TIMING_SCREEN_GATHER];
	result["screen_gather_timestamp_samples"] = bridge_timestamp_samples[BRIDGE_TIMING_SCREEN_GATHER].load();
	result["screen_gather_completed_timestamp_ranges"] = bridge_completed_timestamp_ranges[BRIDGE_TIMING_SCREEN_GATHER].load();
	result["screen_gather_dropped_timestamp_ranges"] = bridge_dropped_timestamp_ranges[BRIDGE_TIMING_SCREEN_GATHER].load();
	result["screen_gather_last_skip_reason"] = screen_gather_last_skip_reason.load();
	result["debug_gpu_ms"] = bridge_timestamp_samples[BRIDGE_TIMING_DEBUG].load() > 0 ? bridge_gpu_ms[BRIDGE_TIMING_DEBUG].load() : -1.0;
	result["debug_render_thread_ms"] = bridge_render_thread_ms[BRIDGE_TIMING_DEBUG].load();
	result["debug_dispatches"] = resources->dispatches[BRIDGE_TIMING_DEBUG];
	result["debug_timestamp_samples"] = bridge_timestamp_samples[BRIDGE_TIMING_DEBUG].load();
	result["debug_completed_timestamp_ranges"] = bridge_completed_timestamp_ranges[BRIDGE_TIMING_DEBUG].load();
	result["debug_dropped_timestamp_ranges"] = bridge_dropped_timestamp_ranges[BRIDGE_TIMING_DEBUG].load();
	result["volume_shadow_gpu_ms"] = bridge_timestamp_samples[BRIDGE_TIMING_VOLUME_SHADOW].load() > 0 ? bridge_gpu_ms[BRIDGE_TIMING_VOLUME_SHADOW].load() : -1.0;
	result["volume_shadow_render_thread_ms"] = bridge_render_thread_ms[BRIDGE_TIMING_VOLUME_SHADOW].load();
	result["volume_shadow_dispatches"] = resources->dispatches[BRIDGE_TIMING_VOLUME_SHADOW];
	result["volume_shadow_timestamp_samples"] = bridge_timestamp_samples[BRIDGE_TIMING_VOLUME_SHADOW].load();
	result["volume_shadow_completed_timestamp_ranges"] = bridge_completed_timestamp_ranges[BRIDGE_TIMING_VOLUME_SHADOW].load();
	result["volume_shadow_dropped_timestamp_ranges"] = bridge_dropped_timestamp_ranges[BRIDGE_TIMING_VOLUME_SHADOW].load();
	result["volume_shadow_requested"] = state->volume_shadow_requested;
	result["volume_shadow_valid"] = volume_shadow_pass.rendered;
	result["volume_shadow_instance_count"] = int(resources->shadow_instance_count);
	result["volume_shadow_size"] = LRTRenderBridge::VOLUME_SHADOW_SIZE;
	result["volume_shadow_axis"] = volume_shadow_pass.transform.basis.get_column(2);
	result["volume_shadow_axis_x"] = volume_shadow_pass.transform.basis.get_column(0);
	result["volume_shadow_axis_y"] = volume_shadow_pass.transform.basis.get_column(1);
	result["volume_shadow_origin"] = volume_shadow_pass.transform.origin;
	result["volume_shadow_light_axis"] = resources->light_transform.basis.get_column(2);
	result["volume_shadow_light_origin"] = resources->light_transform.origin;
	result["state_volume_min"] = state->volume_min;
	result["state_volume_max"] = state->volume_max;
	result["volume_shadow_radius"] = resources->shadow_radius;
	result["volume_shadow_pancake"] = resources->shadow_pancake;
	result["deferred_resolve_flushes"] = int64_t(deferred_resolve_flushes.load());
	result["volume_positional_redraws"] = int64_t(volume_positional_redraws.load());
	result["camera_positional_redraws"] = int64_t(camera_positional_redraws.load());
	result["omni_positional_redraws"] = int64_t(omni_positional_redraws.load());
	result["omni_shadow_caster_max"] = int64_t(omni_shadow_caster_max.load());
	result["omni_dp_origin"] = omni_dp_origin;
	result["omni_dp_points"] = int64_t(omni_dp_points.load());
	result["positional_shadow_sample_requests"] = int64_t(positional_shadow_sample_requests.load());
	result["positional_shadow_sample_valid"] = int64_t(positional_shadow_sample_valid.load());
	result["positional_shadow_registered_lights"] = int64_t(positional_shadow_registered_lights.load());
	result["positional_shadow_sample_last_failure"] = positional_shadow_sample_last_failure.load();
	result["volume_count"] = volume_states.size();
	result["composition_priority"] = state->priority;
	return result;
}

void LRTRenderBridge::free_external_gi_resources() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server != nullptr && !rendering_server->is_on_render_thread()) {
		rendering_server->call_on_render_thread(callable_mp_static(&LRTRenderBridge::free_external_gi_resources));
		rendering_server->sync();
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		return;
	}
	if (debug_pipeline != nullptr) {
		debug_pipeline->clear();
		memdelete(debug_pipeline);
		debug_pipeline = nullptr;
	}
	if (debug_shader.is_valid()) {
		device->free_rid(debug_shader);
		debug_shader = RID();
	}
	if (screen_gather_pipeline.is_valid()) {
		device->free_rid(screen_gather_pipeline);
		screen_gather_pipeline = RID();
	}
	if (screen_gather_shader.is_valid()) {
		device->free_rid(screen_gather_shader);
		screen_gather_shader = RID();
	}
	if (screen_gather_ubo.is_valid()) {
		device->free_rid(screen_gather_ubo);
		screen_gather_ubo = RID();
	}
	for (RID *resource : { &receiver_fields, &pack_fields_pipeline, &pack_fields_shader }) {
		if (resource->is_valid()) {
			device->free_rid(*resource);
			*resource = RID();
		}
	}
	receiver_fields_capacity = 0;
	for (KeyValue<ObjectID, VolumeResources> &entry : volume_resources) {
		free_volume_shadow_resources(entry.value);
		if (entry.value.boundary_validity.is_valid()) {
			device->free_rid(entry.value.boundary_validity);
		}
		if (entry.value.gather_ubo.is_valid()) {
			device->free_rid(entry.value.gather_ubo);
		}
	}
	volume_resources.clear();
	volume_states.clear();
	view_states.clear();
	if (volume_descriptors.is_valid()) {
		device->free_rid(volume_descriptors);
		volume_descriptors = RID();
	}
	descriptor_capacity = 0;
	reset_positional_shadow_atlas();
	empty_render_state = State();
	reset_bridge_timestamp_in_flight();
}

bool LRTRenderBridge::is_volume_shadow_requested() {
	return get_state().enabled && get_state().volume_shadow_requested;
}

void LRTRenderBridge::reset_volume_shadow_pass() {
	MutexLock lock(volumes_mutex);
	for (const State &state : view_states) {
		VolumeResources &resources = volume_resources[state.owner];
		resources.shadow = VolumeShadowPass();
		resources.shadow_instance_count = 0;
	}
}

void LRTRenderBridge::bind_positional_shadow_atlas(RID p_atlas, const PagedArray<RID> *p_lights) {
	positional_shadow_atlas.atlas = p_atlas;
	positional_shadow_atlas.texture = RID();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	if (light_storage == nullptr || p_atlas.is_null() || !light_storage->owns_shadow_atlas(p_atlas)) {
		return;
	}
	positional_shadow_atlas.texture = light_storage->shadow_atlas_get_texture(p_atlas);
	if (p_lights == nullptr) {
		return;
	}
	for (uint64_t i = 0; i < p_lights->size(); i++) {
		const RID instance = (*p_lights)[i];
		if (!light_storage->owns_light_instance(instance)) {
			continue;
		}
		const RID light = light_storage->light_instance_get_base_light(instance);
		if (light.is_valid()) {
			positional_shadow_atlas.light_to_instance[light] = instance;
		}
	}
}

void LRTRenderBridge::register_positional_light(RID p_light, RID p_instance) {
	if (p_light.is_valid() && p_instance.is_valid()) {
		positional_shadow_atlas.light_to_instance[p_light] = p_instance;
		positional_shadow_registered_lights.fetch_add(1);
	}
}

LRTRenderBridge::PositionalShadowSample LRTRenderBridge::get_positional_shadow_sample(RID p_scene_light_instance) {
	PositionalShadowSample sample;
	positional_shadow_sample_requests.fetch_add(1);
	if (p_scene_light_instance.is_null()) {
		positional_shadow_sample_last_failure.store(1);
		return sample;
	}
	if (positional_shadow_atlas.atlas.is_null()) {
		positional_shadow_sample_last_failure.store(2);
		return sample;
	}
	if (positional_shadow_atlas.texture.is_null()) {
		positional_shadow_sample_last_failure.store(3);
		return sample;
	}
	const RID *renderer_instance = positional_shadow_atlas.light_to_instance.getptr(p_scene_light_instance);
	if (renderer_instance == nullptr) {
		positional_shadow_sample_last_failure.store(4);
		return sample;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	if (light_storage == nullptr || !light_storage->owns_light_instance(*renderer_instance)) {
		positional_shadow_sample_last_failure.store(5);
		return sample;
	}
	if (!light_storage->shadow_atlas_owns_light_instance(positional_shadow_atlas.atlas, *renderer_instance)) {
		positional_shadow_sample_last_failure.store(6);
		return sample;
	}
	const RID light = light_storage->light_instance_get_base_light(*renderer_instance);
	Vector2i omni_offset;
	const Rect2 rect = light_storage->light_instance_get_shadow_atlas_rect(*renderer_instance, positional_shadow_atlas.atlas, omni_offset);
	const int atlas_size = light_storage->shadow_atlas_get_size(positional_shadow_atlas.atlas);
	const float texel = atlas_size > 0 ? 1.0f / float(atlas_size) : 0.0f;
	sample.texture = positional_shadow_atlas.texture;
	sample.atlas_rect = Rect2(rect.position + Vector2(texel, texel), rect.size - Vector2(texel, texel) * 2.0f);
	sample.flip_offset = Vector2(omni_offset.x * rect.size.width, omni_offset.y * rect.size.height);
	sample.shadow_camera = light_storage->light_instance_get_shadow_camera(*renderer_instance, 0);
	sample.shadow_bias = light_storage->light_get_param(light, RSE::LIGHT_PARAM_SHADOW_BIAS);
	const RSE::LightType light_type = light_storage->light_get_type(light);
	sample.omni = light_type == RSE::LIGHT_OMNI;
	sample.area = light_type == RSE::LIGHT_AREA;
	if (light_type == RSE::LIGHT_SPOT) {
		sample.shadow_bias /= 100.0f;
	}
	sample.valid = sample.atlas_rect.size.x > 0.0f && sample.atlas_rect.size.y > 0.0f;
	if (sample.valid) {
		positional_shadow_sample_valid.fetch_add(1);
		positional_shadow_sample_last_failure.store(0);
	} else {
		positional_shadow_sample_last_failure.store(7);
	}
	return sample;
}

LRTRenderBridge::AreaLightAtlasSample LRTRenderBridge::get_area_light_atlas_sample(RID p_scene_light_instance) {
	AreaLightAtlasSample sample;
	if (p_scene_light_instance.is_null()) {
		return sample;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const RID *renderer_instance = positional_shadow_atlas.light_to_instance.getptr(p_scene_light_instance);
	if (renderer_instance == nullptr || light_storage == nullptr || texture_storage == nullptr ||
			!light_storage->owns_light_instance(*renderer_instance)) {
		return sample;
	}
	const RID light = light_storage->light_instance_get_base_light(*renderer_instance);
	if (light_storage->light_get_type(light) != RSE::LIGHT_AREA) {
		return sample;
	}
	const RID area_texture = light_storage->light_area_get_texture(light);
	if (area_texture.is_null()) {
		return sample;
	}
	sample.texture = texture_storage->area_light_atlas_get_texture();
	sample.projector_rect = texture_storage->area_light_atlas_get_texture_rect(area_texture);
	if (sample.texture.is_null() || sample.projector_rect.size.x <= 0.0f || sample.projector_rect.size.y <= 0.0f) {
		return sample;
	}
	const Size2i texture_size = (sample.projector_rect.size * texture_storage->area_light_atlas_get_size()).ceil();
	sample.max_mipmap = MIN(Math::floor(Math::log2(MAX(MIN(float(texture_size.x), float(texture_size.y)), 1.0f))), float(texture_storage->area_light_atlas_get_mipmaps())) - 1.0f;
	sample.valid = true;
	return sample;
}

LRTRenderBridge::LightProjectorSample LRTRenderBridge::get_light_projector_sample(RID p_scene_light_instance) {
	LightProjectorSample sample;
	if (p_scene_light_instance.is_null()) {
		return sample;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const RID *renderer_instance = positional_shadow_atlas.light_to_instance.getptr(p_scene_light_instance);
	if (renderer_instance == nullptr || light_storage == nullptr || texture_storage == nullptr ||
			!light_storage->owns_light_instance(*renderer_instance)) {
		return sample;
	}
	const RID light = light_storage->light_instance_get_base_light(*renderer_instance);
	const RSE::LightType type = light_storage->light_get_type(light);
	const RID projector = light_storage->light_get_projector(light);
	if (projector.is_null() || type == RSE::LIGHT_AREA || type == RSE::LIGHT_DIRECTIONAL) {
		return sample;
	}
	const Rect2 rect = texture_storage->decal_atlas_get_texture_rect(projector);
	if (rect.size.x <= 0.0f || rect.size.y <= 0.0f) {
		return sample;
	}
	sample.texture = texture_storage->decal_atlas_get_texture_srgb();
	if (sample.texture.is_null()) {
		return sample;
	}
	// Mirrors LightStorage::_fill_light_data(): a spot projector is stored vertically flipped in the
	// atlas, and an omni projector gets two stacked dual paraboloid rows.
	if (type == RSE::LIGHT_SPOT) {
		sample.rect = Vector4(rect.position.x, rect.position.y + rect.size.height, rect.size.width, -rect.size.height);
	} else {
		sample.rect = Vector4(rect.position.x, rect.position.y, rect.size.width, rect.size.height * 0.5f);
	}
	sample.valid = true;
	return sample;
}

void LRTRenderBridge::set_volume_shadow_camera(ObjectID p_owner, RID p_light_instance, const Projection &p_projection, const Transform3D &p_transform, float p_zfar, const Projection &p_shadow_matrix, bool p_use_pancake, bool p_reverse_cull) {
	MutexLock lock(volumes_mutex);
	VolumeResources &resources = volume_resources[p_owner];
	VolumeShadowPass &volume_shadow_pass = resources.shadow;

	volume_shadow_pass.light_instance = p_light_instance;
	volume_shadow_pass.projection = p_projection;
	volume_shadow_pass.transform = p_transform;
	volume_shadow_pass.zfar = p_zfar;
	volume_shadow_pass.shadow_matrix = p_shadow_matrix;
	volume_shadow_pass.use_pancake = p_use_pancake;
	volume_shadow_pass.reverse_cull = p_reverse_cull;
	volume_shadow_pass.camera_valid = p_light_instance.is_valid();
	volume_shadow_pass.rendered = false;
}

bool LRTRenderBridge::get_volume_shadow_camera(ObjectID p_owner, RID &r_light_instance, Projection &r_projection, Transform3D &r_transform, float &r_zfar, bool &r_use_pancake, bool &r_reverse_cull) {
	MutexLock lock(volumes_mutex);
	VolumeResources &resources = volume_resources[p_owner];
	VolumeShadowPass &volume_shadow_pass = resources.shadow;

	if (!volume_shadow_pass.camera_valid) {
		return false;
	}
	r_light_instance = volume_shadow_pass.light_instance;
	r_projection = volume_shadow_pass.projection;
	r_transform = volume_shadow_pass.transform;
	r_zfar = volume_shadow_pass.zfar;
	r_use_pancake = volume_shadow_pass.use_pancake;
	r_reverse_cull = volume_shadow_pass.reverse_cull;
	return true;
}

RID LRTRenderBridge::ensure_volume_shadow_framebuffer(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	VolumeResources &resources = volume_resources[p_owner];
	RID &volume_shadow_depth = resources.depth;
	RID &volume_shadow_fb = resources.framebuffer;

	if (volume_shadow_fb.is_valid() && volume_shadow_depth.is_valid()) {
		return volume_shadow_fb;
	}
	free_volume_shadow_resources(resources);
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		return RID();
	}
	RD::TextureFormat format;
	format.format = RendererRD::LightStorage::get_shadow_atlas_depth_format(false);
	format.width = VOLUME_SHADOW_SIZE;
	format.height = VOLUME_SHADOW_SIZE;
	format.usage_bits = RendererRD::LightStorage::get_shadow_atlas_depth_usage_bits();
	format.usage_bits |= RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	volume_shadow_depth = device->texture_create(format, RD::TextureView());
	if (volume_shadow_depth.is_null()) {
		return RID();
	}
	Vector<RID> fb_tex;
	fb_tex.push_back(volume_shadow_depth);
	volume_shadow_fb = device->framebuffer_create(fb_tex);
	return volume_shadow_fb;
}

RID LRTRenderBridge::get_volume_shadow_texture(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	VolumeResources *entry = volume_resources.getptr(p_owner);
	if (entry == nullptr) {
		return RID();
	}
	VolumeResources &resources = *entry;
	RID &volume_shadow_depth = resources.depth;

	return volume_shadow_depth;
}

bool LRTRenderBridge::is_volume_shadow_valid(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	VolumeResources *entry = volume_resources.getptr(p_owner);
	if (entry == nullptr) {
		return false;
	}
	VolumeResources &resources = *entry;
	VolumeShadowPass &volume_shadow_pass = resources.shadow;
	RID &volume_shadow_depth = resources.depth;

	return volume_shadow_pass.rendered && volume_shadow_depth.is_valid();
}

Projection LRTRenderBridge::get_volume_shadow_matrix(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	VolumeResources *entry = volume_resources.getptr(p_owner);
	if (entry == nullptr) {
		return Projection();
	}
	VolumeResources &resources = *entry;
	VolumeShadowPass &volume_shadow_pass = resources.shadow;

	return volume_shadow_pass.shadow_matrix;
}

void LRTRenderBridge::mark_volume_shadow_rendered(ObjectID p_owner, uint32_t p_instance_count) {
	MutexLock lock(volumes_mutex);
	VolumeResources &resources = volume_resources[p_owner];
	VolumeShadowPass &volume_shadow_pass = resources.shadow;
	RID &volume_shadow_depth = resources.depth;

	volume_shadow_pass.rendered = volume_shadow_pass.camera_valid && volume_shadow_depth.is_valid();
	resources.shadow_instance_count = p_instance_count;
	resources.dispatches[BRIDGE_TIMING_VOLUME_SHADOW]++;
}

namespace {
LRTRenderBridge::VolumeShadowStats last_volume_shadow_stats;
}

void LRTRenderBridge::set_volume_shadow_light_transform(ObjectID p_owner, const Transform3D &p_transform) {
	MutexLock lock(volumes_mutex);
	VolumeResources &resources = volume_resources[p_owner];

	resources.light_transform = p_transform;
}

void LRTRenderBridge::set_volume_shadow_camera_debug(ObjectID p_owner, float p_radius, float p_pancake) {
	MutexLock lock(volumes_mutex);
	VolumeResources &resources = volume_resources[p_owner];

	resources.shadow_radius = p_radius;
	resources.shadow_pancake = p_pancake;
}

void LRTRenderBridge::invalidate_volume_shadow_frame(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	VolumeResources *entry = volume_resources.getptr(p_owner);
	if (entry == nullptr) {
		return;
	}
	VolumeResources &resources = *entry;
	VolumeShadowPass &volume_shadow_pass = resources.shadow;

	volume_shadow_pass.rendered = false;
}

uint64_t LRTRenderBridge::get_deferred_resolve_flushes() {
	return deferred_resolve_flushes.load();
}

void LRTRenderBridge::count_volume_positional_redraw() {
	volume_positional_redraws.fetch_add(1);
}

uint64_t LRTRenderBridge::get_volume_positional_redraws() {
	return volume_positional_redraws.load();
}

void LRTRenderBridge::count_camera_positional_redraw() {
	camera_positional_redraws.fetch_add(1);
}

uint64_t LRTRenderBridge::get_camera_positional_redraws() {
	return camera_positional_redraws.load();
}

void LRTRenderBridge::count_omni_positional_redraw() {
	omni_positional_redraws.fetch_add(1);
}

uint64_t LRTRenderBridge::get_omni_positional_redraws() {
	return omni_positional_redraws.load();
}

void LRTRenderBridge::count_omni_shadow_caster(uint32_t p_instances) {
	omni_shadow_caster_max.store(MAX(omni_shadow_caster_max.load(), p_instances));
}

uint32_t LRTRenderBridge::get_omni_shadow_caster_max() {
	return omni_shadow_caster_max.load();
}

void LRTRenderBridge::record_omni_dp_debug(const Vector3 &p_origin, uint32_t p_points) {
	omni_dp_origin = p_origin;
	omni_dp_points.store(p_points);
}

Vector3 LRTRenderBridge::get_omni_dp_origin() {
	return omni_dp_origin;
}

uint32_t LRTRenderBridge::get_omni_dp_points() {
	return omni_dp_points.load();
}

void LRTRenderBridge::read_volume_shadow_depth(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	last_volume_shadow_stats = VolumeShadowStats();
	VolumeResources *entry = volume_resources.getptr(p_owner);
	if (entry == nullptr) {
		return;
	}
	VolumeResources &resources = *entry;
	RID &volume_shadow_depth = resources.depth;
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr || volume_shadow_depth.is_null()) {
		return;
	}
	const uint32_t width = VOLUME_SHADOW_SIZE;
	const uint32_t height = VOLUME_SHADOW_SIZE;
	const Vector<uint8_t> data = device->texture_get_data(volume_shadow_depth, 0);
	if (width == 0 || height == 0 || data.size() < int64_t(width) * int64_t(height) * 4) {
		return;
	}
	const float *pixels = reinterpret_cast<const float *>(data.ptr());
	float min_value = 1.0f;
	float max_value = 0.0f;
	uint32_t written = 0;
	uint32_t min_x = width;
	uint32_t min_y = height;
	uint32_t max_x = 0;
	uint32_t max_y = 0;
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			const float value = pixels[y * width + x];
			min_value = MIN(min_value, value);
			max_value = MAX(max_value, value);
			if (value > 0.0f) {
				written++;
				min_x = MIN(min_x, x);
				min_y = MIN(min_y, y);
				max_x = MAX(max_x, x);
				max_y = MAX(max_y, y);
			}
		}
	}
	last_volume_shadow_stats.width = width;
	last_volume_shadow_stats.height = height;
	last_volume_shadow_stats.min_value = min_value;
	last_volume_shadow_stats.max_value = max_value;
	last_volume_shadow_stats.written_pixels = written;
	last_volume_shadow_stats.min_x = min_x;
	last_volume_shadow_stats.min_y = min_y;
	last_volume_shadow_stats.max_x = max_x;
	last_volume_shadow_stats.max_y = max_y;
	last_volume_shadow_stats.center_value = pixels[(height / 2) * width + (width / 2)];
	last_volume_shadow_stats.valid = true;
}

LRTRenderBridge::VolumeShadowStats LRTRenderBridge::get_last_volume_shadow_stats() {
	return last_volume_shadow_stats;
}

bool LRTRenderBridge::begin_volume_shadow_gpu_timing() {
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		return false;
	}
	update_bridge_gpu_timing(device);
	return begin_bridge_gpu_timing(device, BRIDGE_TIMING_VOLUME_SHADOW);
}

void LRTRenderBridge::end_volume_shadow_gpu_timing(bool p_active, double p_cpu_ms) {
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		return;
	}
	end_bridge_gpu_timing(device, BRIDGE_TIMING_VOLUME_SHADOW, p_active);
	if (p_active) {
		bridge_dispatches[BRIDGE_TIMING_VOLUME_SHADOW].fetch_add(1);
	}
	bridge_render_thread_ms[BRIDGE_TIMING_VOLUME_SHADOW].store(p_cpu_ms);
}

void LRTRenderBridge::defer_native_light_resolve(const Callable &p_callable) {
	MutexLock lock(deferred_resolve_mutex);
	deferred_light_resolves.push_back(p_callable);
}

void LRTRenderBridge::flush_deferred_light_resolves() {
	deferred_resolve_flushes.fetch_add(1);
	Vector<Callable> calls;
	{
		MutexLock lock(deferred_resolve_mutex);
		calls = deferred_light_resolves;
		deferred_light_resolves.clear();
	}
	for (const Callable &call : calls) {
		if (call.is_valid()) {
			call.call();
		}
	}
}
