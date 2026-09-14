/**************************************************************************/
/*  lrt_render_bridge.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "lrt_render_bridge.h"

#include "lrt_external_gi.glsl.gen.h"

#include "core/io/resource.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"

#include <atomic>

namespace {

LRTRenderBridge::State lrt_render_state;
RID external_gi_shader;
RID external_gi_pipeline;
std::atomic<uint64_t> external_gi_capture_count{ 0 };
std::atomic<uint64_t> external_gi_capture_owner{ 0 };
std::atomic<bool> external_gi_capture_valid{ false };

struct ExternalGIPushConstant {
	float volume_to_world[16] = {};
	int32_t grid_size[4] = {};
	float grid_min_spacing[4] = {};
	float camera_origin[4] = {};
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
	next.mode = int(p_state.get("mode", 0));
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
	next.external_gi_r = p_state.get("external_gi_r", RID());
	next.external_gi_g = p_state.get("external_gi_g", RID());
	next.external_gi_b = p_state.get("external_gi_b", RID());
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
}
