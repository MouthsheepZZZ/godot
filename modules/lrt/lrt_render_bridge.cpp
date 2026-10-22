/**************************************************************************/
/*  lrt_render_bridge.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "lrt_render_bridge.h"

#include "core/config/engine.h"

#include "lrt_debug.glsl.gen.h"
#include "lrt_boundary_inc.glsl.gen.h"

#include "lrt_geometry_debug.glsl.gen.h"
#include "lrt_screen_gather.glsl.gen.h"

#include "core/config/project_settings.h"
#include "core/io/resource.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "scene/resources/texture.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_method.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"

#include <atomic>

namespace {

LRTRenderBridge::State empty_render_state;
HashMap<ObjectID, LRTRenderBridge::State> volume_states;
Vector<LRTRenderBridge::State> view_states;
RID view_scenario;
Transform3D native_lighting_camera;
bool native_lighting_ready = false;
uint64_t state_revision = 0;
Mutex volumes_mutex;
RID debug_shader;
RID boundary_shader;
RID boundary_pipeline;
PipelineCacheRD *debug_pipeline = nullptr;
RID geometry_debug_shader;
PipelineCacheRD *geometry_debug_pipeline = nullptr;
RID screen_gather_shader;
RID screen_gather_pipeline;
RID screen_gather_ubo;
RID forward_ubo;
int forward_flags = -1;
enum BridgeTimingPass {
	BRIDGE_TIMING_EXTERNAL_GI,
	BRIDGE_TIMING_SCREEN_GATHER,
	BRIDGE_TIMING_DEBUG,
	BRIDGE_TIMING_PASS_COUNT,
};
const char *bridge_timing_begin_names[BRIDGE_TIMING_PASS_COUNT] = { "LRT External GI Begin", "LRT Screen Gather Begin", "LRT Debug Begin" };
const char *bridge_timing_end_names[BRIDGE_TIMING_PASS_COUNT] = { "LRT External GI End", "LRT Screen Gather End", "LRT Debug End" };
std::atomic<double> bridge_gpu_ms[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<double> bridge_render_thread_ms[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<uint64_t> bridge_dispatches[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<int> bridge_timestamp_samples[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<uint64_t> bridge_completed_timestamp_ranges[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<int> screen_gather_last_skip_reason{ 0 };
bool bridge_timestamp_pending[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<bool> bridge_profiling_enabled{ false };
std::atomic<int64_t> screen_gather_bytes{ 0 };
std::atomic<uint64_t> bridge_last_sample_frame[BRIDGE_TIMING_PASS_COUNT];
uint64_t bridge_timing_processed_frame = UINT64_MAX;
uint64_t last_completed_bridge_timestamp_end[BRIDGE_TIMING_PASS_COUNT]{};
uint64_t bridge_timestamp_pending_result_frame[BRIDGE_TIMING_PASS_COUNT]{};
std::atomic<uint64_t> bridge_dropped_timestamp_ranges[BRIDGE_TIMING_PASS_COUNT]{};

struct VolumeResources {
	RID gather_ubo;
	RID gather_lighting;
	RID gather_sky_visibility;
	RID gather_depth;
	RID gather_normal;
	Projection gather_projection;
	Transform3D gather_camera;
	Size2i gather_full_size;
	Size2i gather_size;
	uint64_t gather_frame = 0;
	RID boundary_exposure;
	RID boundary_parameters;
	Vector<uint8_t> boundary_input;
	uint64_t external_capture_count = 0;
	uint64_t boundary_probe_writes = 0;
	bool external_capture_valid = false;
	LRTRenderBridge::ExternalGIStatus external_status = LRTRenderBridge::EXTERNAL_GI_UNAVAILABLE;
	uint64_t dispatches[BRIDGE_TIMING_PASS_COUNT]{};
};
HashMap<ObjectID, VolumeResources> volume_resources;
Mutex deferred_resolve_mutex;
Vector<Callable> deferred_light_resolves;
std::atomic<uint64_t> deferred_resolve_flushes{ 0 };
std::atomic<uint64_t> positional_shadow_sample_requests{ 0 };
std::atomic<uint64_t> positional_shadow_sample_valid{ 0 };
// 0: valid/none, 1: null scene instance, 2: no atlas, 3: no depth texture,
// 4: scene instance not registered, 5: stale renderer instance,
// 6: renderer instance absent from atlas, 7: incomplete sample data.
std::atomic<int> positional_shadow_sample_last_failure{ 0 };

struct PositionalShadowAtlasState {
	RID atlas;
	RID texture;
};
PositionalShadowAtlasState positional_shadow_atlas;

void reset_positional_shadow_atlas_state() {
	positional_shadow_atlas.atlas = RID();
	positional_shadow_atlas.texture = RID();
}


void reset_bridge_timestamp_in_flight() {
	for (int pass = 0; pass < BRIDGE_TIMING_PASS_COUNT; pass++) {
		bridge_timestamp_pending[pass] = false;
	}
}

bool begin_bridge_gpu_timing(RenderingDevice *p_device, BridgeTimingPass p_pass) {
	if ((p_pass == BRIDGE_TIMING_DEBUG && !bridge_profiling_enabled.load()) ||
			(bridge_timestamp_pending[p_pass] && bridge_timestamp_pending_result_frame[p_pass] != p_device->get_captured_timestamps_frame())) {
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
	float sampling_parameters[4] = {};
};

static_assert(sizeof(ScreenGatherData) == 160);

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
	data.volume_max[3] = 0.0f;
	data.grid_min_spacing[3] = p_state.spacing;
	data.atlas_flags[0] = p_state.atlas_size.x;
	data.atlas_flags[1] = p_state.atlas_size.y;
	data.atlas_flags[2] = 0.0f;
	data.atlas_flags[3] = p_state.blend_distance;
	return data;
}

void refresh_view_states() {
	view_states.clear();
	for (const KeyValue<ObjectID, LRTRenderBridge::State> &entry : volume_states) {
		if (entry.value.enabled && entry.value.scenario == view_scenario && entry.value.solver.is_valid() && entry.value.solver->has_local_field()) {
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
	const uint64_t frame = p_device->get_captured_timestamps_frame();
	if (bridge_timing_processed_frame == frame) {
		return;
	}
	bridge_timing_processed_frame = frame;
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
			bridge_last_sample_frame[pass].store(Engine::get_singleton()->get_frames_drawn());
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
	DEBUG_MODE_SKY_VISIBILITY,
	DEBUG_MODE_BOUNDARY,
	DEBUG_MODE_UPDATE_REGIONS,
};

enum DebugDrawKind {
	DEBUG_DRAW_LOBES,
	DEBUG_DRAW_VOXELS,
	DEBUG_DRAW_LINKS,
	DEBUG_DRAW_BOUNDARY_BOXES,
	DEBUG_DRAW_BOUNDARY_SAMPLES,
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
	if (shader_file->parse_versions_from_text(String(lrt_debug_shader_glsl).replace("%LRT_BOUNDARY%", lrt_boundary_inc_shader_glsl)) != OK) {
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
	if (shader_file->parse_versions_from_text(lrt_screen_gather_shader_glsl) != OK) {
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
		case RSE::VIEWPORT_DEBUG_DRAW_LRT_SKY_VISIBILITY:
			return DEBUG_MODE_SKY_VISIBILITY;
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
	if (p_state.external_gi.is_valid()) {
		const uint32_t bytes = p_state.solver->has_external_gi_buffers() ?
				uint32_t(lrt::boundary_connection_count(p_state.grid_size.x, p_state.grid_size.y, p_state.grid_size.z) * 4 * sizeof(float)) : 16;
		device->buffer_clear(p_state.external_gi, 0, bytes);
	}
}

} // namespace

void LRTRenderBridge::reset_positional_shadow_atlas() {
	reset_positional_shadow_atlas_state();
}

void LRTRenderBridge::set_state(const Dictionary &p_state) {
	State next;
	next.owner = ObjectID(uint64_t(p_state.get("owner", uint64_t(0))));
	next.world_to_volume = p_state.get("world_to_volume", Transform3D());
	next.volume_min = p_state.get("volume_min", Vector3());
	next.volume_max = p_state.get("volume_max", Vector3());
	next.grid_min = p_state.get("grid_min", Vector3());
	next.grid_size = p_state.get("grid_size", Vector3i());
	next.atlas_size = p_state.get("atlas_size", Vector2(1.0, 1.0));
	next.environment = p_state.get("environment", RID());
	next.spacing = float(p_state.get("spacing", 0.25));
	next.blend_distance = float(p_state.get("blend_distance", 0.0));
	next.external_gi_enabled = p_state.get("external_gi_enabled", false);
	next.enabled = p_state.get("enabled", false);
	next.solver = p_state["solver"];
	// Resolve texture ownership after queued resource changes on this same render thread.
	auto texture_rid = [&](const String &p_name) {
		const Ref<Texture2D> texture = next.solver->get_texture(p_name);
		return texture.is_valid() ? texture->get_rid() : RID();
	};
	next.diagnostic_dirty = texture_rid("diagnostic_dirty");
	next.local_field_version = next.solver->get_local_field_version();
	next.external_gi = next.solver->get_external_gi_buffer();
	next.scenario = p_state.get("scenario", RID());
	next.priority = p_state.get("priority", 0);
	next.tree_order = p_state.get("tree_order", PackedInt32Array());
	MutexLock lock(volumes_mutex);
	volume_states[next.owner] = next;
	VolumeResources &resources = volume_resources[next.owner];
	if (!next.external_gi_enabled) {
		clear_external_gi_buffers(next);
		resources.external_capture_valid = false;
		resources.boundary_input.clear();
	}
	refresh_view_states();
}

void LRTRenderBridge::clear(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	VolumeResources *resources = volume_resources.getptr(p_owner);
	if (resources == nullptr) {
		return;
	}
	if (resources->boundary_parameters.is_valid()) {
		RenderingDevice::get_singleton()->free_rid(resources->boundary_parameters);
	}
	if (resources->boundary_exposure.is_valid()) {
		RenderingDevice::get_singleton()->free_rid(resources->boundary_exposure);
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
	MutexLock lock(volumes_mutex);
	if (view_scenario == p_scenario) {
		bool invalidated = false;
		for (const State &state : view_states) {
			if (!state.solver->has_local_field()) {
				invalidated = true;
				break;
			}
		}
		if (!invalidated) {
			return;
		}
	}
	view_scenario = p_scenario;
	refresh_view_states();
}

bool LRTRenderBridge::is_current_volume(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	const State *state = volume_states.getptr(p_owner);
	return state != nullptr && state->enabled && state->scenario == view_scenario;
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
	const bool sdf_view = p_mode == RSE::VIEWPORT_DEBUG_DRAW_LRT_COARSE_GEOMETRY;
	if (sdf_view && p_framebuffer.is_valid()) {
		// Show the baked geometry itself. The source mesh depth would conceal inward errors.
		device->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_ALL, Vector<Color>({ Color(0.025, 0.025, 0.025, 1.0) }), 0.0f);
		device->draw_list_end();
	}
	for (const State &state : view_states) {
		if (sdf_view) {
			_debug_draw_geometry(state, p_framebuffer, p_camera_with_transform);
		} else {
			_debug_draw(state, p_framebuffer, p_camera_with_transform, p_mode);
		}
	}
	end_bridge_gpu_timing(device, BRIDGE_TIMING_DEBUG, timing_active);
	bridge_render_thread_ms[BRIDGE_TIMING_DEBUG].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
}

void LRTRenderBridge::_debug_draw_geometry(const State &state, RID p_framebuffer, const Projection &p_camera_with_transform) {
	if (!state.enabled || p_framebuffer.is_null()) {
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	const Array draws = state.solver->get_geometry_debug_resources();
	if (draws.is_empty()) {
		return;
	}
	if (geometry_debug_pipeline == nullptr) {
		Ref<RDShaderFile> shader_file;
		shader_file.instantiate();
		ERR_FAIL_COND(shader_file->parse_versions_from_text(lrt_geometry_debug_shader_glsl) != OK);
		geometry_debug_shader = device->shader_create_from_spirv(shader_file->get_spirv_stages());
		ERR_FAIL_COND(geometry_debug_shader.is_null());
		RD::PipelineRasterizationState rasterization;
		// Asset instances may have mirrored transforms.
		rasterization.cull_mode = RD::POLYGON_CULL_DISABLED;
		RD::PipelineDepthStencilState depth_stencil;
		depth_stencil.enable_depth_test = true;
		depth_stencil.enable_depth_write = true;
		depth_stencil.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
		geometry_debug_pipeline = memnew(PipelineCacheRD);
		geometry_debug_pipeline->setup(geometry_debug_shader, RD::RENDER_PRIMITIVE_TRIANGLES, rasterization,
				RD::PipelineMultisampleState(), depth_stencil, RD::PipelineColorBlendState::create_disabled(), 0);
	}
	struct GeometryDebugPushConstant {
		float projection[16];
		float cell_size[4] = {};
	} push_constant;
	static_assert(sizeof(GeometryDebugPushConstant) == 80);
	RD::DrawListID draw_list = device->draw_list_begin(p_framebuffer);
	device->draw_command_begin_label("LRT Coarse Geometry Debug");
	device->draw_list_bind_render_pipeline(draw_list,
			geometry_debug_pipeline->get_render_pipeline(RD::INVALID_ID, device->framebuffer_get_format(p_framebuffer)));
	for (int index = 0; index < draws.size(); index++) {
		const Dictionary draw = draws[index];
		const RID buffer = draw["buffer"];
		const Transform3D transform = draw["transform"];
		const Projection projection = p_camera_with_transform * Projection(state.world_to_volume.affine_inverse() * transform);
		for (int column = 0; column < 4; column++) {
			for (int row = 0; row < 4; row++) {
				push_constant.projection[column * 4 + row] = projection.columns[column][row];
			}
		}
		push_constant.cell_size[0] = float(draw["cell_size"]);
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, buffer));
		const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(geometry_debug_shader, 0, uniforms);
		device->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
		device->draw_list_set_push_constant(draw_list, &push_constant, sizeof(push_constant));
		device->draw_list_draw(draw_list, false, uint32_t(draw["cells"]), 36);
	}
	device->draw_command_end_label();
	device->draw_list_end();
	bridge_dispatches[BRIDGE_TIMING_DEBUG].fetch_add(1);
	volume_resources[state.owner].dispatches[BRIDGE_TIMING_DEBUG]++;
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

	const uint32_t texture_bindings[] = { 14 };
	const RID texture_resources[] = { state.diagnostic_dirty };
	const RID local_visibility_buffer = state.solver->get_local_visibility_buffer();
	if (state.external_gi.is_null() || local_visibility_buffer.is_null()) {
		return;
	}
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 0,
			RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
	for (uint32_t index = 0; index < 1; index++) {
		const RID texture = texture_storage->texture_get_rd_texture(texture_resources[index]);
		if (texture.is_null()) {
			return;
		}
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, texture_bindings[index], texture));
	}
	for (int channel = 0; channel < 3; channel++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1 + channel, state.solver->get_radiance_buffer(channel)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4 + channel, state.solver->get_source_buffer(channel)));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, state.solver->get_sky_visibility_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, state.solver->get_material_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, state.solver->get_record_index_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, local_visibility_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, state.external_gi));
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

	if (mode == DEBUG_MODE_BOUNDARY) {
		push_constant.volume_max[3] = volume_resources[state.owner].external_capture_valid ? 1.0f : 0.0f;
	}
	if (mode == DEBUG_MODE_UPDATE_REGIONS) {
		const AABB region = state.solver->get_debug_source_region();
		for (int axis = 0; axis < 3; axis++) {
			push_constant.volume_min[axis] = region.position[axis];
			push_constant.volume_max[axis] = region.get_end()[axis];
		}
		push_constant.volume_max[3] = state.solver->get_debug_update_activity();
	}

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
	if (mode == DEBUG_MODE_LOCAL_VISIBILITY || mode == DEBUG_MODE_SKY_VISIBILITY) {
		draw(DEBUG_DRAW_LINKS, grid_count * 26, 36);
		if (mode == DEBUG_MODE_SKY_VISIBILITY) {
			draw(DEBUG_DRAW_LOBES, grid_count, 288);
		}
	} else if (mode == DEBUG_MODE_UPDATE_REGIONS) {
		draw(DEBUG_DRAW_VOXELS, grid_count, 36);
	} else {
		if (mode == DEBUG_MODE_BOUNDARY) {
			if (state.external_gi_enabled) {
				draw(DEBUG_DRAW_BOUNDARY_SAMPLES, lrt::boundary_connection_count(state.grid_size.x, state.grid_size.y, state.grid_size.z), 36);
			}
		} else {
			draw(DEBUG_DRAW_LOBES, grid_count, 288);
		}
		if (mode == DEBUG_MODE_BOUNDARY) {
			draw(DEBUG_DRAW_BOUNDARY_BOXES, 2, 432);
		}
	}
	device->draw_command_end_label();
	device->draw_list_end();
	bridge_dispatches[BRIDGE_TIMING_DEBUG].fetch_add(1);
	volume_resources[state.owner].dispatches[BRIDGE_TIMING_DEBUG]++;
}

void LRTRenderBridge::capture_external_gi(RID p_environment, const Ref<RenderSceneBuffersRD> &p_buffers) {
	MutexLock lock(volumes_mutex);
	if (view_states.is_empty()) {
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	update_bridge_gpu_timing(device);
	const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
	const bool timing_active = begin_bridge_gpu_timing(device, BRIDGE_TIMING_EXTERNAL_GI);
	for (const State &state : view_states) {
		_capture_external_gi(state, p_environment, p_buffers);
	}
	end_bridge_gpu_timing(device, BRIDGE_TIMING_EXTERNAL_GI, timing_active);
	bridge_render_thread_ms[BRIDGE_TIMING_EXTERNAL_GI].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
}

void LRTRenderBridge::_capture_external_gi(const State &state, RID p_environment, const Ref<RenderSceneBuffersRD> &p_buffers) {
	VolumeResources &resources = volume_resources[state.owner];
	if (p_environment != state.environment || !state.external_gi_enabled) {
		return;
	}
	if (resources.external_capture_valid) {
		clear_external_gi_buffers(state);
		resources.external_capture_count++;
	}
	resources.external_status = EXTERNAL_GI_UNAVAILABLE;
	resources.external_capture_valid = false;
	resources.boundary_input.clear();
}

bool LRTRenderBridge::gather_screen(RID p_depth, RID p_normal_roughness, RID p_lighting_output, RID p_sky_output,
		const Size2i &p_full_size, const Size2i &p_gather_size,
		const Projection &p_projection, const Transform3D &p_camera_transform) {
	MutexLock lock(volumes_mutex);
	if (view_states.is_empty() || !ensure_screen_gather_pipeline()) {
		return false;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	update_bridge_gpu_timing(device);
	const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
	const bool timing_active = begin_bridge_gpu_timing(device, BRIDGE_TIMING_SCREEN_GATHER);
	device->texture_clear(p_lighting_output, Color(0, 0, 0, 0), 0, 1, 0, 1);
	device->texture_clear(p_sky_output, Color(0, 0, 0, 0), 0, 1, 0, 1);
	for (const State &state : view_states) {
		VolumeResources &resources = volume_resources[state.owner];
		if (resources.external_status == EXTERNAL_GI_FAILED) {
			continue;
		}
		if (resources.gather_ubo.is_null()) {
			resources.gather_ubo = device->uniform_buffer_create(sizeof(VolumeData));
		}
		const VolumeData data = volume_data(state);
		device->buffer_update(resources.gather_ubo, 0, sizeof(VolumeData), &data);
		if (!_gather_volume(state, resources.gather_ubo, p_depth, p_normal_roughness,
					p_lighting_output, p_sky_output, p_full_size, p_gather_size, p_projection, p_camera_transform)) {
			end_bridge_gpu_timing(device, BRIDGE_TIMING_SCREEN_GATHER, timing_active);
			return false;
		}
	}
	end_bridge_gpu_timing(device, BRIDGE_TIMING_SCREEN_GATHER, timing_active);
	bridge_render_thread_ms[BRIDGE_TIMING_SCREEN_GATHER].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
	return true;
}

bool LRTRenderBridge::_gather_volume(const State &state, RID p_lrt_ubo,
		RID p_depth, RID p_normal_roughness, RID p_lighting_output, RID p_sky_output,
		const Size2i &p_full_size, const Size2i &p_gather_size, const Projection &p_projection, const Transform3D &p_camera_transform) {
	if (!state.enabled || p_full_size.x <= 0 || p_full_size.y <= 0 || p_lrt_ubo.is_null() ||
			p_depth.is_null() || p_normal_roughness.is_null() || p_lighting_output.is_null()) {
		screen_gather_last_skip_reason.store(1);
		return false;
	}
	if (!ensure_screen_gather_pipeline()) {
		screen_gather_last_skip_reason.store(2);
		return false;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(device, false);
	const RID buffers[] = {
		state.solver->get_radiance_buffer(0), state.solver->get_radiance_buffer(1),
		state.solver->get_radiance_buffer(2), state.solver->get_sky_visibility_buffer(),
	};
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, screen_gather_ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, p_lrt_ubo));
	for (uint32_t index = 0; index < 4; index++) {
		if (buffers[index].is_null()) {
			screen_gather_last_skip_reason.store(10 + int(index));
			return false;
		}
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, index + 2, buffers[index]));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, p_depth));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, p_normal_roughness));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_lighting_output));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, p_sky_output));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 10,
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
	const Size2i gather_size = p_gather_size;
	const Rect2i region = volume_screen_region(state, p_projection, p_camera_transform, gather_size);
	if (!region.has_area()) {
		return true;
	}
	gather_data.screen_size[0] = p_full_size.x;
	gather_data.screen_size[1] = p_full_size.y;
	gather_data.screen_size[2] = gather_size.x;
	gather_data.screen_size[3] = gather_size.y;
	gather_data.sampling_parameters[0] = GLOBAL_GET("rendering/global_illumination/lrt/screen_gather/normal_bias");
	ERR_FAIL_COND_V_MSG(gather_data.sampling_parameters[0] < 0.0f, false, "LRT Gather normal bias must be nonnegative.");
	device->buffer_update(screen_gather_ubo, 0, sizeof(ScreenGatherData), &gather_data);

	RD::ComputeListID list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(list, screen_gather_pipeline);
	device->compute_list_bind_uniform_set(list, uniform_set, 0);
	const int32_t constants[8] = { region.position.x, region.position.y, region.size.x, region.size.y,
		state.solver->is_receiving_sky() ? 1 : 0, 0, 0, 0 };
	device->compute_list_set_push_constant(list, constants, sizeof(constants));
	device->compute_list_dispatch_threads(list, region.size.x, region.size.y, 1);
	device->compute_list_end();
	bridge_dispatches[BRIDGE_TIMING_SCREEN_GATHER].fetch_add(1);
	volume_resources[state.owner].dispatches[BRIDGE_TIMING_SCREEN_GATHER]++;
	VolumeResources &resources = volume_resources[state.owner];
	resources.gather_lighting = p_lighting_output;
	resources.gather_sky_visibility = p_sky_output;
	resources.gather_depth = p_depth;
	resources.gather_normal = p_normal_roughness;
	resources.gather_projection = p_projection;
	resources.gather_camera = p_camera_transform;
	resources.gather_full_size = p_full_size;
	resources.gather_size = p_gather_size;
	resources.gather_frame = Engine::get_singleton()->get_frames_drawn();
	state.solver->mark_gathered();
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

bool LRTRenderBridge::ensure_screen_gather_buffers(RenderSceneBuffersRD *p_buffers, uint64_t &r_bytes) {
	ERR_FAIL_NULL_V(p_buffers, false);
	const Size2i full_size = p_buffers->get_internal_size();
	const int divisor = int(GLOBAL_GET("rendering/global_illumination/lrt/screen_gather/resolution")) == 0 ? 2 : 1;
	const Size2i gather_size(MAX(1, (full_size.x + divisor - 1) / divisor), MAX(1, (full_size.y + divisor - 1) / divisor));
	if (p_buffers->has_texture(RB_SCOPE_LRT, RB_TEX_LRT_SCREEN_LIGHTING)) {
		const RD::TextureFormat format = p_buffers->get_texture_format(RB_SCOPE_LRT, RB_TEX_LRT_SCREEN_LIGHTING);
		if (int(format.width) == gather_size.x && int(format.height) == gather_size.y) {
			return true;
		}
		release_screen_gather_buffers(p_buffers, r_bytes);
	}
	const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	const RID lighting = p_buffers->create_texture(RB_SCOPE_LRT, RB_TEX_LRT_SCREEN_LIGHTING,
			RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, gather_size);
	const RID sky_visibility = p_buffers->create_texture(RB_SCOPE_LRT, RB_TEX_LRT_SCREEN_SKY_VISIBILITY,
			RD::DATA_FORMAT_R16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, gather_size);
	if (lighting.is_null() || sky_visibility.is_null()) {
		p_buffers->clear_context(RB_SCOPE_LRT);
		ERR_PRINT("LRT screen gather allocation failed; excluding LRT from this view.");
		return false;
	}
	r_bytes = uint64_t(gather_size.x) * gather_size.y * p_buffers->get_view_count() * 10;
	screen_gather_bytes.fetch_add(int64_t(r_bytes));
	return true;
}

void LRTRenderBridge::release_screen_gather_buffers(RenderSceneBuffersRD *p_buffers, uint64_t &r_bytes) {
	if (r_bytes == 0) {
		return;
	}
	p_buffers->clear_context(RB_SCOPE_LRT);
	screen_gather_bytes.fetch_sub(int64_t(r_bytes));
	r_bytes = 0;
}

RID LRTRenderBridge::get_forward_uniform_buffer(bool p_enabled, RSE::ViewportDebugDraw p_mode) {
	RenderingDevice *device = RenderingDevice::get_singleton();
	const int flags = (p_enabled ? 1 : 0) | (p_mode == RSE::VIEWPORT_DEBUG_DRAW_LRT_LIGHTING ? 2 : 0);
	if (forward_ubo.is_null()) {
		forward_ubo = device->uniform_buffer_create(4 * sizeof(int32_t));
		ERR_FAIL_COND_V(forward_ubo.is_null(), RID());
	}
	if (forward_flags != flags) {
		const int32_t data[4] = { flags & 1, (flags >> 1) & 1, 0, 0 };
		ERR_FAIL_COND_V(device->buffer_update(forward_ubo, 0, sizeof(data), data) != OK, RID());
		forward_flags = flags;
	}
	return forward_ubo;
}

Dictionary LRTRenderBridge::get_gather_resources(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	Dictionary result;
	const VolumeResources *resources = volume_resources.getptr(p_owner);
	if (resources == nullptr) {
		return result;
	}
	result["screen_lighting"] = resources->gather_lighting;
	result["screen_sky_visibility"] = resources->gather_sky_visibility;
	result["screen_depth"] = resources->gather_depth;
	result["screen_normal"] = resources->gather_normal;
	result["screen_projection"] = resources->gather_projection;
	result["screen_camera"] = resources->gather_camera;
	result["screen_full_size"] = resources->gather_full_size;
	result["screen_gather_size"] = resources->gather_size;
	result["screen_frame"] = int64_t(resources->gather_frame);
	return result;
}

Dictionary LRTRenderBridge::get_performance_stats(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	Dictionary result;
	result["screen_gather_bytes"] = screen_gather_bytes.load();
	uint64_t boundary_bytes = 0;
	uint64_t gather_uniform_bytes = 0;
	for (const KeyValue<ObjectID, VolumeResources> &entry : volume_resources) {
		const VolumeResources &volume = entry.value;
		boundary_bytes += volume.boundary_exposure.is_valid() ? 8 * 4 * sizeof(float) : 0;

		gather_uniform_bytes += volume.gather_ubo.is_valid() ? sizeof(VolumeData) : 0;
	}
	result["external_boundary_bytes"] = int64_t(boundary_bytes);
	result["gather_uniform_bytes"] = int64_t(gather_uniform_bytes);
	result["shared_gpu_bytes"] = int64_t(boundary_bytes + gather_uniform_bytes +
			(screen_gather_ubo.is_valid() ? sizeof(ScreenGatherData) : 0) +
			(forward_ubo.is_valid() ? 4 * sizeof(int32_t) : 0));
	const State *state = volume_states.getptr(p_owner);
	const VolumeResources *resources = volume_resources.getptr(p_owner);
	if (state == nullptr || resources == nullptr) {
		result["owner_matches"] = false;
		result["screen_gather_dispatches"] = int64_t(0);
		result["debug_dispatches"] = int64_t(0);
		result["external_gi_dispatches"] = int64_t(0);
		return result;
	}
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
	result["state_volume_min"] = state->volume_min;
	result["state_volume_max"] = state->volume_max;
	result["deferred_resolve_flushes"] = int64_t(deferred_resolve_flushes.load());
	result["positional_shadow_sample_requests"] = int64_t(positional_shadow_sample_requests.load());
	result["positional_shadow_sample_valid"] = int64_t(positional_shadow_sample_valid.load());
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
	if (geometry_debug_pipeline != nullptr) {
		geometry_debug_pipeline->clear();
		memdelete(geometry_debug_pipeline);
		geometry_debug_pipeline = nullptr;
	}
	if (geometry_debug_shader.is_valid()) {
		device->free_rid(geometry_debug_shader);
		geometry_debug_shader = RID();
	}
	for (RID resource : { boundary_pipeline, boundary_shader }) {
		if (resource.is_valid()) {
			device->free_rid(resource);
		}
	}
	boundary_pipeline = RID();
	boundary_shader = RID();
	if (screen_gather_pipeline.is_valid()) {
		device->free_rid(screen_gather_pipeline);
		screen_gather_pipeline = RID();
	}
	if (screen_gather_shader.is_valid()) {
		device->free_rid(screen_gather_shader);
		screen_gather_shader = RID();
	}
	if (forward_ubo.is_valid()) {
		device->free_rid(forward_ubo);
		forward_ubo = RID();
		forward_flags = -1;
	}
	if (screen_gather_ubo.is_valid()) {
		device->free_rid(screen_gather_ubo);
		screen_gather_ubo = RID();
	}
	for (KeyValue<ObjectID, VolumeResources> &entry : volume_resources) {
		if (entry.value.boundary_parameters.is_valid()) {
			device->free_rid(entry.value.boundary_parameters);
		}
		if (entry.value.boundary_exposure.is_valid()) {
			device->free_rid(entry.value.boundary_exposure);
		}
		if (entry.value.gather_ubo.is_valid()) {
			device->free_rid(entry.value.gather_ubo);
		}
	}
	volume_resources.clear();
	volume_states.clear();
	view_states.clear();
	reset_positional_shadow_atlas();
	empty_render_state = State();
	reset_bridge_timestamp_in_flight();
	bridge_timing_processed_frame = UINT64_MAX;
}

void LRTRenderBridge::bind_positional_shadow_atlas(RID p_atlas) {
	positional_shadow_atlas.atlas = p_atlas;
	positional_shadow_atlas.texture = RID();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	if (light_storage == nullptr || p_atlas.is_null() || !light_storage->owns_shadow_atlas(p_atlas)) {
		return;
	}
	positional_shadow_atlas.texture = light_storage->shadow_atlas_get_texture(p_atlas);

}

RID LRTRenderBridge::get_native_light_instance(RID p_scene_light_instance) {
	return RSG::scene->instance_get_renderer_light_instance(p_scene_light_instance);
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
	const RID renderer_instance = get_native_light_instance(p_scene_light_instance);
	if (renderer_instance.is_null()) {
		positional_shadow_sample_last_failure.store(4);
		return sample;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	if (light_storage == nullptr || !light_storage->owns_light_instance(renderer_instance)) {
		positional_shadow_sample_last_failure.store(5);
		return sample;
	}
	if (!light_storage->shadow_atlas_owns_light_instance(positional_shadow_atlas.atlas, renderer_instance)) {
		positional_shadow_sample_last_failure.store(6);
		return sample;
	}
	const RID light = light_storage->light_instance_get_base_light(renderer_instance);
	Vector2i omni_offset;
	const Rect2 rect = light_storage->light_instance_get_shadow_atlas_rect(renderer_instance, positional_shadow_atlas.atlas, omni_offset);
	const int atlas_size = light_storage->shadow_atlas_get_size(positional_shadow_atlas.atlas);
	const float texel = atlas_size > 0 ? 1.0f / float(atlas_size) : 0.0f;
	sample.texture = positional_shadow_atlas.texture;
	sample.atlas_rect = Rect2(rect.position + Vector2(texel, texel), rect.size - Vector2(texel, texel) * 2.0f);
	sample.flip_offset = Vector2(omni_offset.x * rect.size.width, omni_offset.y * rect.size.height);
	sample.shadow_camera = light_storage->light_instance_get_shadow_camera(renderer_instance, 0);
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
	const RID renderer_instance = get_native_light_instance(p_scene_light_instance);
	if (renderer_instance.is_null() || light_storage == nullptr || texture_storage == nullptr ||
			!light_storage->owns_light_instance(renderer_instance)) {
		return sample;
	}
	const RID light = light_storage->light_instance_get_base_light(renderer_instance);
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
	const RID renderer_instance = get_native_light_instance(p_scene_light_instance);
	if (renderer_instance.is_null() || light_storage == nullptr || texture_storage == nullptr ||
			!light_storage->owns_light_instance(renderer_instance)) {
		return sample;
	}
	const RID light = light_storage->light_instance_get_base_light(renderer_instance);
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

namespace {
}

uint64_t LRTRenderBridge::get_deferred_resolve_flushes() {
	return deferred_resolve_flushes.load();
}

void LRTRenderBridge::defer_native_light_resolve(const Callable &p_callable) {
	MutexLock lock(deferred_resolve_mutex);
	deferred_light_resolves.push_back(p_callable);
}

void LRTRenderBridge::flush_deferred_light_resolves(const Transform3D &p_camera_transform) {
	native_lighting_camera = p_camera_transform;
	native_lighting_ready = true;
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
	for (const State &state : get_states()) {
		if (state.enabled && state.solver.is_valid()) {
			state.solver->refresh_native_source_for_view();
		}
	}
	native_lighting_ready = false;
}

bool LRTRenderBridge::is_native_lighting_ready(ObjectID p_owner) {
	return native_lighting_ready && is_current_volume(p_owner);
}

Transform3D LRTRenderBridge::get_native_lighting_camera() {
	return native_lighting_camera;
}

Transform3D LRTRenderBridge::get_volume_to_world(ObjectID p_owner) {
	MutexLock lock(volumes_mutex);
	const State *state = volume_states.getptr(p_owner);
	ERR_FAIL_NULL_V(state, Transform3D());
	return state->world_to_volume.affine_inverse();
}
