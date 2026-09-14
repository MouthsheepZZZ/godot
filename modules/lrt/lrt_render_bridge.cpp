/**************************************************************************/
/*  lrt_render_bridge.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "lrt_render_bridge.h"

#include "lrt_external_gi.glsl.gen.h"
#include "lrt_debug.glsl.gen.h"

#include "core/io/resource.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"

#include <atomic>

namespace {

LRTRenderBridge::State lrt_render_state;
RID external_gi_shader;
RID external_gi_pipeline;
RID debug_shader;
PipelineCacheRD *debug_pipeline = nullptr;
std::atomic<uint64_t> external_gi_capture_count{ 0 };
std::atomic<uint64_t> external_gi_capture_owner{ 0 };
std::atomic<bool> external_gi_capture_valid{ false };

struct ExternalGIPushConstant {
	float volume_to_world[16] = {};
	int32_t grid_size[4] = {};
	float grid_min_spacing[4] = {};
	float camera_origin[4] = {};
};

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

bool ensure_external_gi_pipeline() {
	if (external_gi_pipeline.is_valid()) {
		return true;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		return false;
	}
	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();
	if (shader_file->parse_versions_from_text(lrt_external_gi_shader_glsl) != OK) {
		shader_file->print_errors("LRT external Dynamic GI shader");
		return false;
	}
	external_gi_shader = device->shader_create_from_spirv(shader_file->get_spirv_stages());
	if (external_gi_shader.is_null()) {
		return false;
	}
	external_gi_pipeline = device->compute_pipeline_create(external_gi_shader);
	return external_gi_pipeline.is_valid();
}

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
	next.receiver_buffer = p_state.get("receiver_buffer", RID());
	next.receiver_count = int(p_state.get("receiver_count", 0));
	next.revision = lrt_render_state.revision + 1;
	if (next.owner != lrt_render_state.owner) {
		external_gi_capture_count.store(0);
		external_gi_capture_valid.store(false);
	}
	lrt_render_state = next;
	external_gi_capture_owner.store(uint64_t(next.owner));
	if (!next.external_gi_enabled) {
		clear_external_gi_buffers(next);
		external_gi_capture_valid.store(false);
	}
}

void LRTRenderBridge::clear(ObjectID p_owner) {
	if (lrt_render_state.owner != p_owner) {
		return;
	}
	const uint64_t next_revision = lrt_render_state.revision + 1;
	lrt_render_state = State();
	lrt_render_state.revision = next_revision;
	external_gi_capture_owner.store(0);
	external_gi_capture_valid.store(false);
}

const LRTRenderBridge::State &LRTRenderBridge::get_state() {
	return lrt_render_state;
}

void LRTRenderBridge::debug_draw(RID p_framebuffer, const Projection &p_camera_with_transform, RSE::ViewportDebugDraw p_mode) {
	const int mode = debug_mode_index(p_mode);
	const State &state = lrt_render_state;
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
	if (state.external_gi_r.is_null() || state.external_gi_g.is_null() || state.external_gi_b.is_null() || state.receiver_buffer.is_null()) {
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
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, state.receiver_buffer));
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
		draw(DEBUG_DRAW_RECEIVERS, state.receiver_count, 288);
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
}

void LRTRenderBridge::capture_external_gi(RID p_environment, RID p_hddagi_ubo, RID p_diffuse,
		RID p_occlusion_0, RID p_occlusion_1, const Vector3 &p_camera_origin) {
	const State &state = lrt_render_state;
	if (p_environment != state.environment || !state.external_gi_enabled) {
		return;
	}
	if (state.external_gi_r.is_null() || state.external_gi_g.is_null() || state.external_gi_b.is_null()) {
		external_gi_capture_valid.store(false);
		return;
	}
	if (p_hddagi_ubo.is_null() || p_diffuse.is_null() || p_occlusion_0.is_null() || p_occlusion_1.is_null()) {
		clear_external_gi_buffers(state);
		external_gi_capture_valid.store(false);
		return;
	}
	if (!ensure_external_gi_pipeline()) {
		external_gi_capture_valid.store(false);
		return;
	}
	RenderingDevice *device = RenderingDevice::get_singleton();
	ERR_FAIL_NULL(device);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_hddagi_ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_diffuse));
	Vector<RID> occlusion = { p_occlusion_0, p_occlusion_1 };
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, occlusion));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 3,
			RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, state.external_gi_r));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, state.external_gi_g));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, state.external_gi_b));
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(external_gi_shader, 0, uniforms);
	if (uniform_set.is_null()) {
		external_gi_capture_valid.store(false);
		return;
	}
	ExternalGIPushConstant push_constant;
	RendererRD::MaterialStorage::store_transform(state.world_to_volume.affine_inverse(), push_constant.volume_to_world);
	push_constant.grid_size[0] = state.grid_size.x;
	push_constant.grid_size[1] = state.grid_size.y;
	push_constant.grid_size[2] = state.grid_size.z;
	push_constant.grid_size[3] = state.grid_size.x * state.grid_size.y * state.grid_size.z;
	push_constant.grid_min_spacing[0] = state.grid_min.x;
	push_constant.grid_min_spacing[1] = state.grid_min.y;
	push_constant.grid_min_spacing[2] = state.grid_min.z;
	push_constant.grid_min_spacing[3] = state.spacing;
	push_constant.camera_origin[0] = p_camera_origin.x;
	push_constant.camera_origin[1] = p_camera_origin.y;
	push_constant.camera_origin[2] = p_camera_origin.z;
	RD::ComputeListID list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(list, external_gi_pipeline);
	device->compute_list_bind_uniform_set(list, uniform_set, 0);
	device->compute_list_set_push_constant(list, &push_constant, sizeof(push_constant));
	device->compute_list_dispatch(list, Math::division_round_up(uint32_t(push_constant.grid_size[3]), uint32_t(64)), 1, 1);
	device->compute_list_end();
	external_gi_capture_count.fetch_add(1);
	external_gi_capture_valid.store(true);
}

uint64_t LRTRenderBridge::get_external_gi_capture_count(ObjectID p_owner) {
	return external_gi_capture_owner.load() == uint64_t(p_owner) ? external_gi_capture_count.load() : 0;
}

bool LRTRenderBridge::is_external_gi_capture_valid(ObjectID p_owner) {
	return external_gi_capture_owner.load() == uint64_t(p_owner) && external_gi_capture_valid.load();
}

void LRTRenderBridge::free_external_gi_resources() {
	RenderingDevice *device = RenderingDevice::get_singleton();
	if (device == nullptr) {
		return;
	}
	if (external_gi_pipeline.is_valid()) {
		device->free_rid(external_gi_pipeline);
		external_gi_pipeline = RID();
	}
	if (external_gi_shader.is_valid()) {
		device->free_rid(external_gi_shader);
		external_gi_shader = RID();
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
}
