/**************************************************************************/
/*  hddagi_diffuse_provider.cpp                                          */
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

#include "gi.h"

#include "servers/rendering/renderer_rd/shaders/environment/hddagi_diffuse_sample.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device_binds.h"

namespace RendererRD {

bool GI::HDDAGI::sample_boundary(const DiffuseGIProvider::BoundaryRequest &p_request, const Vector3 &p_camera_origin) {
	if (p_request.exclude_sky && reads_sky) {
		return false;
	}
	if (diffuse_sampling_pipeline.is_null()) {
		Ref<RDShaderFile> file;
		file.instantiate();
		ERR_FAIL_COND_V(file->parse_versions_from_text(hddagi_diffuse_sample_shader_glsl) != OK, false);
		diffuse_sampling_shader = RD::get_singleton()->shader_create_from_spirv(file->get_spirv_stages());
		ERR_FAIL_COND_V(diffuse_sampling_shader.is_null(), false);
		diffuse_sampling_pipeline = RD::get_singleton()->compute_pipeline_create(diffuse_sampling_shader);
	}
	struct ExternalGIPushConstant {
		float volume_to_world[16] = {};
		int32_t grid_size[4] = {};
		float grid_min_spacing[4] = {};
		float camera_origin[4] = {};
	};
	RenderingDevice *device = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(device, false);
	float inverse_exposure[HDDAGI::MAX_CASCADES * 4] = {};
	for (uint32_t cascade = 0; cascade < cascades.size(); cascade++) {
		ERR_FAIL_COND_V(cascades[cascade].baked_exposure_normalization <= 0.0f, false);
		inverse_exposure[cascade * 4] = 1.0f / cascades[cascade].baked_exposure_normalization;
	}
	if (diffuse_sampling_exposure.is_null()) {
		diffuse_sampling_exposure = device->uniform_buffer_create(sizeof(inverse_exposure));
	}
	device->buffer_update(diffuse_sampling_exposure, 0, sizeof(inverse_exposure), inverse_exposure);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, gi->hddagi_ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, get_lightprobe_diffuse_texture()));
	const Vector<RID> occlusion = get_lightprobe_occlusion_textures();
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, occlusion));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 3,
			RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_request.coefficients[0]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_request.coefficients[1]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_request.coefficients[2]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_request.validity));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 8, diffuse_sampling_exposure));
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(diffuse_sampling_shader, 0, uniforms);
	if (uniform_set.is_null()) {
		return false;
	}
	ExternalGIPushConstant push_constant;
	RendererRD::MaterialStorage::store_transform(p_request.sample_to_world, push_constant.volume_to_world);
	push_constant.grid_size[0] = p_request.grid_size.x;
	push_constant.grid_size[1] = p_request.grid_size.y;
	push_constant.grid_size[2] = p_request.grid_size.z;
	const int inner_x = MAX(0, p_request.grid_size.x - 2);
	const int inner_y = MAX(0, p_request.grid_size.y - 2);
	const int inner_z = MAX(0, p_request.grid_size.z - 2);
	const int probe_count = p_request.grid_size.x * p_request.grid_size.y * p_request.grid_size.z;
	const int boundary_count = probe_count - inner_x * inner_y * inner_z;
	push_constant.grid_size[3] = boundary_count;
	push_constant.grid_min_spacing[0] = p_request.grid_min.x;
	push_constant.grid_min_spacing[1] = p_request.grid_min.y;
	push_constant.grid_min_spacing[2] = p_request.grid_min.z;
	push_constant.grid_min_spacing[3] = p_request.spacing;
	push_constant.camera_origin[0] = p_camera_origin.x;
	push_constant.camera_origin[1] = p_camera_origin.y;
	push_constant.camera_origin[2] = p_camera_origin.z;
	RD::ComputeListID list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(list, diffuse_sampling_pipeline);
	device->compute_list_bind_uniform_set(list, uniform_set, 0);
	device->compute_list_set_push_constant(list, &push_constant, sizeof(push_constant));
	device->compute_list_dispatch(list, Math::division_round_up(uint32_t(push_constant.grid_size[3]), uint32_t(64)), 1, 1);
	device->compute_list_end();
	return true;
}

} // namespace RendererRD
