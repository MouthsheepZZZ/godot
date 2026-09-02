/**************************************************************************/
/*  local_lrt.cpp                                                         */
/**************************************************************************/

#include "local_lrt.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "scene/3d/local_lrt_math.h"
#include "servers/rendering/renderer_rd/shaders/environment/local_lrt_environment.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/local_lrt_injection.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_globals.h" // IWYU pragma: keep. RENDER_TIMESTAMP macro uses RSG.

static_assert(RendererRD::LocalLRT::MAX_SURFACE_VOLUMES == LocalLRTMath::MAX_BLEND_VOLUMES);

#include <cstring>

namespace RendererRD {

LocalLRT::LocalLRT() {
	transfer_format = CLAMP((int)GLOBAL_GET("rendering/global_illumination/local_lrt/transfer_format"), 0, 3);
}

bool LocalLRT::_ensure_visibility_shader() {
	if (visibility_shader_initialized) {
		return visibility_pipeline.is_valid();
	}
	if (!RD::get_singleton()) {
		return false;
	}

	visibility_shader = memnew(LocalLrtVisibilityShaderRD);
	Vector<String> modes;
	modes.push_back(String());
	visibility_shader->initialize(modes);
	visibility_shader_version = visibility_shader->version_create();
	visibility_pipeline = RD::get_singleton()->compute_pipeline_create(visibility_shader->version_get_shader(visibility_shader_version, 0));
	visibility_shader_initialized = true;
	return visibility_pipeline.is_valid();
}

bool LocalLRT::_ensure_radiance_shader() {
	if (radiance_shader_initialized) {
		return radiance_pipeline.is_valid();
	}
	if (!RD::get_singleton()) {
		return false;
	}

	radiance_shader = memnew(LocalLrtRadianceShaderRD);
	Vector<String> modes;
	static const char *format_defines[] = {
		"",
		"#define LOCAL_TRANSFER_RGB_FP16\n",
		"#define LOCAL_TRANSFER_LUMINANCE_FP32_TINT\n",
		"#define LOCAL_TRANSFER_LUMINANCE_FP16_TINT\n",
	};
	modes.push_back(format_defines[transfer_format]);
	radiance_shader->initialize(modes);
	radiance_shader_version = radiance_shader->version_create();
	radiance_pipeline = RD::get_singleton()->compute_pipeline_create(radiance_shader->version_get_shader(radiance_shader_version, 0));
	radiance_shader_initialized = true;
	return radiance_pipeline.is_valid();
}

bool LocalLRT::_ensure_injection_shader() {
	if (injection_shader_initialized) {
		return injection_pipeline.is_valid();
	}
	if (!RD::get_singleton()) {
		return false;
	}

	injection_shader = memnew(LocalLrtInjectionShaderRD);
	Vector<String> modes;
	modes.push_back(String());
	injection_shader->initialize(modes);
	injection_shader_version = injection_shader->version_create();
	injection_pipeline = RD::get_singleton()->compute_pipeline_create(injection_shader->version_get_shader(injection_shader_version, 0));
	injection_shader_initialized = true;
	return injection_pipeline.is_valid();
}

bool LocalLRT::_ensure_environment_shader() {
	if (environment_shader_initialized) {
		return environment_pipelines[0].is_valid() && environment_pipelines[1].is_valid();
	}
	if (!RD::get_singleton()) {
		return false;
	}

	environment_shader = memnew(LocalLrtEnvironmentShaderRD);
	Vector<String> modes;
	modes.push_back(String());
	modes.push_back("#define USE_OCTMAP_ARRAY\n");
	environment_shader->initialize(modes);
	environment_shader_version = environment_shader->version_create();
	for (int mode = 0; mode < 2; mode++) {
		environment_pipelines[mode] = RD::get_singleton()->compute_pipeline_create(environment_shader->version_get_shader(environment_shader_version, mode));
	}
	environment_shader_initialized = true;
	return environment_pipelines[0].is_valid() && environment_pipelines[1].is_valid();
}

void LocalLRT::_free_gpu_resources(Volume &r_volume) {
	RID *resources[] = {
		&r_volume.local_visibility_buffer,
		&r_volume.local_transfer_buffer,
		&r_volume.mesh_light_buffer,
		&r_volume.global_visibility_buffers[0],
		&r_volume.global_visibility_buffers[1],
		&r_volume.radiance_buffers[0],
		&r_volume.radiance_buffers[1],
		&r_volume.injection_buffers[0],
		&r_volume.injection_buffers[1],
		&r_volume.environment_data_buffer,
		&r_volume.environment_sh_buffer,
		&r_volume.environment_injection_buffer,
		&r_volume.inside_solid_buffer,
		&r_volume.analytic_lights_buffer,
		&r_volume.shadow_visibility_buffer,
		&r_volume.shadow_matrix_buffer,
		&r_volume.shadow_framebuffer,
		&r_volume.shadow_depth_texture,
		&r_volume.shadow_upload_texture,
	};
	if (RD::get_singleton()) {
		for (RID *resource : resources) {
			if (resource->is_valid()) {
				RD::get_singleton()->free_rid(*resource);
			}
			*resource = RID();
		}
	}
	r_volume.local_visibility.clear();
	r_volume.analytic_lights_buffer_bytes = 0;
	r_volume.analytic_lights.clear();
	r_volume.requested_analytic_lights.clear();
	r_volume.environment_data.clear();
	r_volume.environment_sky_texture = RID();
	r_volume.environment_mode = 0;
	r_volume.shadow_resolution = 1;
	r_volume.shadow_bias = 0.0f;
	r_volume.shadow_enabled = false;
	r_volume.shadow_use_upload = false;
	r_volume.positional_shadow_texture = RID();
	r_volume.positional_shadow_resolution = 1;
	r_volume.visibility_probe_offset = 0;
	r_volume.radiance_probe_offset = 0;
	r_volume.injection_probe_offset = 0;
	r_volume.radiance_pattern_phase = 0;
	r_volume.global_visibility_is_a = true;
	r_volume.radiance_is_a = true;
	r_volume.injection_is_a = true;
	r_volume.injection_pending = false;
	r_volume.injection_dirty = true;
}

RID LocalLRT::_create_vector4_buffer(const Vector<Vector4> &p_values) {
	Vector<uint8_t> bytes;
	bytes.resize(p_values.size() * 4 * sizeof(float));
	float *write = reinterpret_cast<float *>(bytes.ptrw());
	for (const Vector4 &value : p_values) {
		*write++ = value.x;
		*write++ = value.y;
		*write++ = value.z;
		*write++ = value.w;
	}
	return RD::get_singleton()->storage_buffer_create(bytes.size(), bytes);
}

Vector<uint8_t> LocalLRT::_pack_transfer(const Vector<Vector4> &p_values) const {
	ERR_FAIL_COND_V(p_values.size() % 12 != 0, Vector<uint8_t>());
	const int probe_count = p_values.size() / 12;
	Vector<uint8_t> bytes;
	bytes.resize(probe_count * _transfer_uints_per_probe() * sizeof(uint32_t));
	uint32_t *write = reinterpret_cast<uint32_t *>(bytes.ptrw());
	for (int probe = 0; probe < probe_count; probe++) {
		if (transfer_format == 1) {
			for (int value = 0; value < 12; value++) {
				const Vector4 &row = p_values[probe * 12 + value];
				*write++ = uint32_t(Math::make_half_float(row.x)) | (uint32_t(Math::make_half_float(row.y)) << 16);
				*write++ = uint32_t(Math::make_half_float(row.z)) | (uint32_t(Math::make_half_float(row.w)) << 16);
			}
			continue;
		}

		float luminance[16];
		float denominator = 0.0f;
		for (int element = 0; element < 16; element++) {
			const int row = element / 4;
			const int column = element % 4;
			const float red = p_values[probe * 12 + row][column];
			const float green = p_values[probe * 12 + 4 + row][column];
			const float blue = p_values[probe * 12 + 8 + row][column];
			luminance[element] = red * 0.2126f + green * 0.7152f + blue * 0.0722f;
			denominator += luminance[element] * luminance[element];
		}

		float tint[3] = {};
		if (denominator > 1e-20f) {
			for (int channel = 0; channel < 3; channel++) {
				float numerator = 0.0f;
				for (int element = 0; element < 16; element++) {
					const int row = element / 4;
					const int column = element % 4;
					numerator += luminance[element] * p_values[probe * 12 + channel * 4 + row][column];
				}
				tint[channel] = MAX(numerator / denominator, 0.0f);
			}
		}

		const float tint_scale = MAX(tint[0], MAX(tint[1], tint[2]));
		if (tint_scale > 1e-20f) {
			for (float &element : luminance) {
				element *= tint_scale;
			}
			for (float &channel : tint) {
				channel /= tint_scale;
			}
		}
		if (transfer_format == 2) {
			for (float element : luminance) {
				uint32_t bits;
				memcpy(&bits, &element, sizeof(uint32_t));
				*write++ = bits;
			}
		} else {
			for (int element = 0; element < 16; element += 2) {
				*write++ = uint32_t(Math::make_half_float(luminance[element])) | (uint32_t(Math::make_half_float(luminance[element + 1])) << 16);
			}
		}
		const uint32_t red = uint32_t(Math::round(CLAMP(tint[0], 0.0f, 1.0f) * 255.0f));
		const uint32_t green = uint32_t(Math::round(CLAMP(tint[1], 0.0f, 1.0f) * 255.0f));
		const uint32_t blue = uint32_t(Math::round(CLAMP(tint[2], 0.0f, 1.0f) * 255.0f));
		*write++ = red | (green << 8) | (blue << 16) | (0xFFu << 24);
	}
	return bytes;
}

int LocalLRT::_transfer_uints_per_probe() const {
	static const int uint_counts[] = { 48, 24, 17, 9 };
	return uint_counts[transfer_format];
}

RID LocalLRT::_create_transfer_buffer(const Vector<Vector4> &p_values) {
	if (transfer_format == 0) {
		return _create_vector4_buffer(p_values);
	}
	const Vector<uint8_t> bytes = _pack_transfer(p_values);
	return RD::get_singleton()->storage_buffer_create(bytes.size(), bytes);
}

RID LocalLRT::_create_uint_buffer(const Vector<uint32_t> &p_values) {
	Vector<uint8_t> bytes;
	bytes.resize(p_values.size() * sizeof(uint32_t));
	uint32_t *write = reinterpret_cast<uint32_t *>(bytes.ptrw());
	for (uint32_t value : p_values) {
		*write++ = value;
	}
	return RD::get_singleton()->storage_buffer_create(bytes.size(), bytes);
}

RID LocalLRT::_create_float_buffer(int p_value_count) {
	Vector<uint8_t> bytes;
	bytes.resize(MAX(p_value_count, 1) * sizeof(float));
	memset(bytes.ptrw(), 0, bytes.size());
	return RD::get_singleton()->storage_buffer_create(bytes.size(), bytes);
}

Vector<Vector4> LocalLRT::_read_vector4_buffer(RID p_buffer, int p_value_count) const {
	const Vector<uint8_t> bytes = RD::get_singleton()->buffer_get_data(p_buffer);
	const float *read = reinterpret_cast<const float *>(bytes.ptr());
	Vector<Vector4> result;
	result.resize(p_value_count);
	for (Vector4 &value : result) {
		value = Vector4(read[0], read[1], read[2], read[3]);
		read += 4;
	}
	return result;
}

Vector<float> LocalLRT::_read_float_buffer(RID p_buffer, int p_value_count) const {
	const Vector<uint8_t> bytes = RD::get_singleton()->buffer_get_data(p_buffer);
	const float *read = reinterpret_cast<const float *>(bytes.ptr());
	Vector<float> result;
	result.resize(p_value_count);
	for (int i = 0; i < p_value_count; i++) {
		result.write[i] = read[i];
	}
	return result;
}

void LocalLRT::_ensure_default_shadow_texture() {
	if (default_shadow_texture.is_valid() || !RD::get_singleton()) {
		return;
	}
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.width = 1;
	tf.height = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<uint8_t> data;
	data.resize(sizeof(float));
	*reinterpret_cast<float *>(data.ptrw()) = 0.0f;
	Vector<Vector<uint8_t>> layers;
	layers.push_back(data);
	default_shadow_texture = RD::get_singleton()->texture_create(tf, RD::TextureView(), layers);
}

void LocalLRT::_ensure_default_sky_textures() {
	if ((default_sky_textures[0].is_valid() && default_sky_textures[1].is_valid()) || !RD::get_singleton()) {
		return;
	}
	Vector<uint8_t> data;
	data.resize(4 * sizeof(float));
	memset(data.ptrw(), 0, data.size());
	Vector<Vector<uint8_t>> layers;
	layers.push_back(data);
	for (int mode = 0; mode < 2; mode++) {
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		tf.texture_type = mode == 0 ? RD::TEXTURE_TYPE_2D : RD::TEXTURE_TYPE_2D_ARRAY;
		tf.width = 1;
		tf.height = 1;
		tf.array_layers = 1;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
		default_sky_textures[mode] = RD::get_singleton()->texture_create(tf, RD::TextureView(), layers);
	}
}

void LocalLRT::_ensure_shadow_visibility_buffer(Volume &r_volume) {
	const int probe_count = r_volume.resolution.x * r_volume.resolution.y * r_volume.resolution.z;
	if (!r_volume.shadow_visibility_buffer.is_valid()) {
		r_volume.shadow_visibility_buffer = _create_float_buffer(probe_count);
	}
	if (!r_volume.shadow_matrix_buffer.is_valid()) {
		r_volume.shadow_matrix_buffer = _create_float_buffer(16);
	}
}

void LocalLRT::_ensure_raster_shadow(Volume &r_volume) {
	if (r_volume.shadow_framebuffer.is_valid()) {
		return;
	}
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_D32_SFLOAT;
	tf.width = DIRECTIONAL_SHADOW_SIZE;
	tf.height = DIRECTIONAL_SHADOW_SIZE;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	r_volume.shadow_depth_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	Vector<RID> fb_tex;
	fb_tex.push_back(r_volume.shadow_depth_texture);
	r_volume.shadow_framebuffer = RD::get_singleton()->framebuffer_create(fb_tex);
}

void LocalLRT::_upload_shadow_matrix(Volume &r_volume, const Projection &p_view_proj) {
	_ensure_shadow_visibility_buffer(r_volume);
	Vector<uint8_t> bytes;
	bytes.resize(16 * sizeof(float));
	float *write = reinterpret_cast<float *>(bytes.ptrw());
	for (int column = 0; column < 4; column++) {
		const Vector4 value = p_view_proj.columns[column];
		*write++ = value.x;
		*write++ = value.y;
		*write++ = value.z;
		*write++ = value.w;
	}
	RD::get_singleton()->buffer_update(r_volume.shadow_matrix_buffer, 0, bytes.size(), bytes.ptr());
}

RID LocalLRT::_shadow_sample_texture(const Volume &p_volume) const {
	if (p_volume.shadow_enabled && p_volume.shadow_use_upload && p_volume.shadow_upload_texture.is_valid()) {
		return p_volume.shadow_upload_texture;
	}
	if (p_volume.shadow_enabled && p_volume.shadow_depth_texture.is_valid()) {
		return p_volume.shadow_depth_texture;
	}
	return default_shadow_texture;
}

void LocalLRT::_reset_visibility(Volume &r_volume) {
	if (r_volume.local_visibility.is_empty()) {
		return;
	}

	Vector<uint8_t> local_bytes;
	local_bytes.resize(r_volume.local_visibility.size() * 4 * sizeof(float));
	float *write = reinterpret_cast<float *>(local_bytes.ptrw());
	for (const Vector4 &value : r_volume.local_visibility) {
		*write++ = value.x;
		*write++ = value.y;
		*write++ = value.z;
		*write++ = value.w;
	}
	RD::get_singleton()->buffer_update(r_volume.global_visibility_buffers[0], 0, local_bytes.size(), local_bytes.ptr());
	RD::get_singleton()->buffer_update(r_volume.global_visibility_buffers[1], 0, local_bytes.size(), local_bytes.ptr());
	r_volume.global_visibility_is_a = true;
	r_volume.visibility_probe_offset = 0;
	const Vector3i boundary_radius = (r_volume.resolution - Vector3i(1, 1, 1)) / 2;
	r_volume.visibility_steps_remaining = MIN(boundary_radius.x, MIN(boundary_radius.y, boundary_radius.z));
}

void LocalLRT::_mark_visibility_dirty(Volume &r_volume) {
	if (r_volume.local_visibility.is_empty()) {
		return;
	}

	const Vector3i boundary_radius = (r_volume.resolution - Vector3i(1, 1, 1)) / 2;
	const int required_steps = MIN(boundary_radius.x, MIN(boundary_radius.y, boundary_radius.z));
	r_volume.visibility_steps_remaining = MAX(r_volume.visibility_steps_remaining, required_steps);
}

void LocalLRT::_propagate_visibility(Volume &r_volume, int p_iterations) {
	const int max_iterations = MIN(p_iterations, r_volume.visibility_steps_remaining);
	if (max_iterations <= 0 || r_volume.local_visibility.is_empty() || !_ensure_visibility_shader()) {
		return;
	}

	VisibilityPushConstant push_constant = {};
	push_constant.resolution[0] = r_volume.resolution.x;
	push_constant.resolution[1] = r_volume.resolution.y;
	push_constant.resolution[2] = r_volume.resolution.z;
	push_constant.probe_count = r_volume.local_visibility.size();

	const RID shader = visibility_shader->version_get_shader(visibility_shader_version, 0);
	RENDER_TIMESTAMP("Local LRT Visibility");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, visibility_pipeline);
	int64_t remaining_probe_budget = r_volume.visibility_probe_budget > 0 ? r_volume.visibility_probe_budget : INT64_MAX;
	int completed_iterations = 0;
	bool dispatched = false;
	while (completed_iterations < max_iterations && remaining_probe_budget > 0) {
		if (dispatched) {
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}
		const int source = r_volume.global_visibility_is_a ? 0 : 1;
		const int destination = source ^ 1;
		const int dispatch_probe_count = MIN((int64_t)(push_constant.probe_count - r_volume.visibility_probe_offset), remaining_probe_budget);
		push_constant.probe_offset = r_volume.visibility_probe_offset;
		push_constant.dispatch_probe_count = dispatch_probe_count;
		RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(
				shader,
				0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, r_volume.local_visibility_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_volume.global_visibility_buffers[source]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, r_volume.global_visibility_buffers[destination]));
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(VisibilityPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, dispatch_probe_count, 1, 1);
		dispatched = true;
		remaining_probe_budget -= dispatch_probe_count;
		r_volume.visibility_probe_offset += dispatch_probe_count;
		if (r_volume.visibility_probe_offset == push_constant.probe_count) {
			r_volume.visibility_probe_offset = 0;
			r_volume.global_visibility_is_a = destination == 0;
			r_volume.visibility_steps_remaining--;
			completed_iterations++;
		}
	}
	RD::get_singleton()->compute_list_end();
	RENDER_TIMESTAMP("< Local LRT Visibility");
}

void LocalLRT::_propagate_radiance(Volume &r_volume, int p_iterations) {
	const RID injection_buffer = r_volume.injection_buffers[r_volume.injection_is_a ? 0 : 1];
	if (p_iterations <= 0 || !r_volume.mesh_light_buffer.is_valid() || !injection_buffer.is_valid() || !r_volume.inside_solid_buffer.is_valid() || !_ensure_radiance_shader()) {
		return;
	}

	const int probe_count = r_volume.resolution.x * r_volume.resolution.y * r_volume.resolution.z;
	RadiancePushConstant push_constant = {};
	push_constant.resolution[0] = r_volume.resolution.x;
	push_constant.resolution[1] = r_volume.resolution.y;
	push_constant.resolution[2] = r_volume.resolution.z;
	push_constant.probe_count = probe_count;
	const Vector3 probe_spacing = r_volume.size / Vector3(r_volume.resolution - Vector3i(1, 1, 1));
	push_constant.probe_spacing[0] = probe_spacing.x;
	push_constant.probe_spacing[1] = probe_spacing.y;
	push_constant.probe_spacing[2] = probe_spacing.z;
	push_constant.decay_per_meter = 1.0f;
	push_constant.neighbor_pattern = r_volume.radiance_neighbor_pattern;
	push_constant.pattern_phase = r_volume.radiance_pattern_phase;

	const RID shader = radiance_shader->version_get_shader(radiance_shader_version, 0);
	RENDER_TIMESTAMP("Local LRT Radiance");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, radiance_pipeline);
	int64_t remaining_probe_budget = r_volume.radiance_probe_budget > 0 ? r_volume.radiance_probe_budget : INT64_MAX;
	int completed_iterations = 0;
	bool dispatched = false;
	while (completed_iterations < p_iterations && remaining_probe_budget > 0) {
		if (dispatched) {
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}
		const int source = r_volume.radiance_is_a ? 0 : 1;
		const int destination = source ^ 1;
		const int dispatch_probe_count = MIN((int64_t)(probe_count - r_volume.radiance_probe_offset), remaining_probe_budget);
		push_constant.probe_offset = r_volume.radiance_probe_offset;
		push_constant.dispatch_probe_count = dispatch_probe_count;
		RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(
				shader,
				0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, r_volume.local_visibility_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_volume.local_transfer_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, r_volume.local_visibility_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, injection_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, r_volume.mesh_light_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, r_volume.radiance_buffers[source]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, r_volume.radiance_buffers[destination]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, r_volume.inside_solid_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, r_volume.environment_injection_buffer));
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(RadiancePushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, dispatch_probe_count, 1, 1);
		dispatched = true;
		remaining_probe_budget -= dispatch_probe_count;
		r_volume.radiance_probe_offset += dispatch_probe_count;
		if (r_volume.radiance_probe_offset == probe_count) {
			r_volume.radiance_probe_offset = 0;
			completed_iterations++;
			if (r_volume.radiance_neighbor_pattern == 1) {
				r_volume.radiance_pattern_phase++;
				if (r_volume.radiance_pattern_phase < 3) {
					push_constant.pattern_phase = r_volume.radiance_pattern_phase;
					continue;
				}
				r_volume.radiance_pattern_phase = 0;
			}
			r_volume.radiance_is_a = destination == 0;
			push_constant.pattern_phase = r_volume.radiance_pattern_phase;
		}
	}
	RD::get_singleton()->compute_list_end();
	RENDER_TIMESTAMP("< Local LRT Radiance");
}

RID LocalLRT::volume_allocate() {
	return volume_owner.allocate_rid();
}

void LocalLRT::volume_initialize(RID p_volume) {
	volume_owner.initialize_rid(p_volume, Volume());
}

void LocalLRT::volume_free(RID p_volume) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	_free_gpu_resources(*volume);
	volume_owner.free(p_volume);
}

bool LocalLRT::owns_volume(RID p_volume) const {
	return volume_owner.owns(p_volume);
}

void LocalLRT::volume_set_enabled(RID p_volume, bool p_enabled) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->enabled = p_enabled;
}

void LocalLRT::volume_set_grid(RID p_volume, const Vector3 &p_size, const Vector3i &p_resolution) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	if (volume->resolution != p_resolution) {
		_free_gpu_resources(*volume);
	}
	if (volume->size != p_size) {
		volume->injection_dirty = true;
	}
	volume->size = p_size;
	volume->resolution = p_resolution;
}

void LocalLRT::volume_set_transform(RID p_volume, const Transform3D &p_transform) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	if (volume->transform != p_transform) {
		volume->transform = p_transform;
		volume->injection_dirty = true;
	}
}

void LocalLRT::volume_set_visibility_iterations(RID p_volume, int p_iterations) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->visibility_iterations = MAX(p_iterations, 0);
}

void LocalLRT::volume_set_propagation_iterations(RID p_volume, int p_iterations) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->radiance_iterations = p_iterations;
}

void LocalLRT::volume_set_visibility_probe_budget(RID p_volume, int p_probe_budget) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->visibility_probe_budget = MAX(p_probe_budget, 0);
}

void LocalLRT::volume_set_radiance_probe_budget(RID p_volume, int p_probe_budget) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->radiance_probe_budget = MAX(p_probe_budget, 0);
}

void LocalLRT::volume_set_injection_probe_budget(RID p_volume, int p_probe_budget) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->injection_probe_budget = MAX(p_probe_budget, 0);
}

void LocalLRT::volume_set_radiance_neighbor_pattern(RID p_volume, int p_pattern) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	const int pattern = CLAMP(p_pattern, 0, 1);
	if (volume->radiance_neighbor_pattern == pattern) {
		return;
	}
	volume->radiance_neighbor_pattern = pattern;
	volume->radiance_probe_offset = 0;
	volume->radiance_pattern_phase = 0;
	volume->radiance_is_a = true;
	if (volume->radiance_buffers[0].is_valid() && volume->radiance_buffers[1].is_valid()) {
		const uint32_t buffer_bytes = volume->resolution.x * volume->resolution.y * volume->resolution.z * 3 * 4 * sizeof(float);
		RD::get_singleton()->buffer_clear(volume->radiance_buffers[0], 0, buffer_bytes);
		RD::get_singleton()->buffer_clear(volume->radiance_buffers[1], 0, buffer_bytes);
	}
}

void LocalLRT::volume_set_energy(RID p_volume, float p_energy) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->energy = p_energy;
}

void LocalLRT::volume_set_priority(RID p_volume, int p_priority) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->priority = p_priority;
}

void LocalLRT::volume_set_edge_blend_distance(RID p_volume, float p_distance) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->edge_blend_distance = p_distance;
}

void LocalLRT::volume_set_static_data(RID p_volume, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer, const Vector<Vector4> &p_mesh_light) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	const int probe_count = volume->resolution.x * volume->resolution.y * volume->resolution.z;
	ERR_FAIL_COND(p_local_visibility.size() != probe_count);
	ERR_FAIL_COND(p_local_transfer.size() != probe_count * 12);
	ERR_FAIL_COND(p_mesh_light.size() != probe_count * 3);
	ERR_FAIL_COND(!_ensure_visibility_shader());

	_free_gpu_resources(*volume);
	volume->local_visibility = p_local_visibility;
	volume->local_visibility_buffer = _create_vector4_buffer(p_local_visibility);
	volume->local_transfer_buffer = _create_transfer_buffer(p_local_transfer);
	volume->mesh_light_buffer = _create_vector4_buffer(p_mesh_light);
	volume->global_visibility_buffers[0] = _create_vector4_buffer(p_local_visibility);
	volume->global_visibility_buffers[1] = _create_vector4_buffer(p_local_visibility);

	Vector<Vector4> zero_radiance;
	zero_radiance.resize(probe_count * 3);
	volume->radiance_buffers[0] = _create_vector4_buffer(zero_radiance);
	volume->radiance_buffers[1] = _create_vector4_buffer(zero_radiance);
	volume->injection_buffers[0] = _create_vector4_buffer(zero_radiance);
	volume->injection_buffers[1] = _create_vector4_buffer(zero_radiance);
	volume->environment_injection_buffer = _create_vector4_buffer(zero_radiance);
	Vector<Vector4> zero_environment_sh;
	zero_environment_sh.resize(3);
	volume->environment_sh_buffer = _create_vector4_buffer(zero_environment_sh);
	Vector<Vector4> default_environment_data;
	default_environment_data.resize(4);
	default_environment_data.write[1] = Vector4(1, 0, 0, 0);
	default_environment_data.write[2] = Vector4(0, 1, 0, 0);
	default_environment_data.write[3] = Vector4(0, 0, 1, 1);
	volume->environment_data_buffer = _create_vector4_buffer(default_environment_data);
	Vector<uint32_t> zero_inside_solid;
	zero_inside_solid.resize_initialized(probe_count);
	volume->inside_solid_buffer = _create_uint_buffer(zero_inside_solid);
	_ensure_shadow_visibility_buffer(*volume);
	_reset_visibility(*volume);
	volume->injection_dirty = true;
}

void LocalLRT::volume_update_static_data(RID p_volume, const Vector3i &p_begin, const Vector3i &p_size, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer, const Vector<Vector4> &p_mesh_light, const Vector<int> &p_inside_solid) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0 || p_size.z <= 0);
	ERR_FAIL_COND(p_begin.x < 0 || p_begin.y < 0 || p_begin.z < 0);
	ERR_FAIL_COND(p_begin.x + p_size.x > volume->resolution.x || p_begin.y + p_size.y > volume->resolution.y || p_begin.z + p_size.z > volume->resolution.z);
	ERR_FAIL_COND(!volume->local_visibility_buffer.is_valid() || !volume->local_transfer_buffer.is_valid() || !volume->mesh_light_buffer.is_valid() || !volume->inside_solid_buffer.is_valid() || !volume->radiance_buffers[0].is_valid() || !volume->radiance_buffers[1].is_valid());
	const int region_probe_count = p_size.x * p_size.y * p_size.z;
	ERR_FAIL_COND(p_local_visibility.size() != region_probe_count);
	ERR_FAIL_COND(p_local_transfer.size() != region_probe_count * 12);
	ERR_FAIL_COND(p_mesh_light.size() != region_probe_count * 3);
	ERR_FAIL_COND(p_inside_solid.size() != region_probe_count);

	for (int z = 0; z < p_size.z; z++) {
		for (int y = 0; y < p_size.y; y++) {
			const int source_probe = p_size.x * (y + p_size.y * z);
			const int destination_probe = p_begin.x + volume->resolution.x * (p_begin.y + y + volume->resolution.y * (p_begin.z + z));
			for (int x = 0; x < p_size.x; x++) {
				volume->local_visibility.write[destination_probe + x] = p_local_visibility[source_probe + x];
			}
		}
	}

	auto update_vector4_region = [&](RID p_buffer, const Vector<Vector4> &p_values, int p_values_per_probe) {
		Vector<uint8_t> row_bytes;
		row_bytes.resize(p_size.x * p_values_per_probe * 4 * sizeof(float));
		for (int z = 0; z < p_size.z; z++) {
			for (int y = 0; y < p_size.y; y++) {
				const int source_probe = p_size.x * (y + p_size.y * z);
				const int destination_probe = p_begin.x + volume->resolution.x * (p_begin.y + y + volume->resolution.y * (p_begin.z + z));
				float *write = reinterpret_cast<float *>(row_bytes.ptrw());
				for (int value_index = 0; value_index < p_size.x * p_values_per_probe; value_index++) {
					const Vector4 &value = p_values[source_probe * p_values_per_probe + value_index];
					*write++ = value.x;
					*write++ = value.y;
					*write++ = value.z;
					*write++ = value.w;
				}
				RD::get_singleton()->buffer_update(p_buffer, destination_probe * p_values_per_probe * 4 * sizeof(float), row_bytes.size(), row_bytes.ptr());
			}
		}
	};
	update_vector4_region(volume->local_visibility_buffer, p_local_visibility, 1);
	if (transfer_format != 0) {
		for (int z = 0; z < p_size.z; z++) {
			for (int y = 0; y < p_size.y; y++) {
				const int source_probe = p_size.x * (y + p_size.y * z);
				const int destination_probe = p_begin.x + volume->resolution.x * (p_begin.y + y + volume->resolution.y * (p_begin.z + z));
				const Vector<uint8_t> row_bytes = _pack_transfer(p_local_transfer.slice(source_probe * 12, (source_probe + p_size.x) * 12));
				RD::get_singleton()->buffer_update(volume->local_transfer_buffer, destination_probe * _transfer_uints_per_probe() * sizeof(uint32_t), row_bytes.size(), row_bytes.ptr());
			}
		}
	} else {
		update_vector4_region(volume->local_transfer_buffer, p_local_transfer, 12);
	}
	update_vector4_region(volume->mesh_light_buffer, p_mesh_light, 3);

	Vector<uint8_t> inside_solid_bytes;
	inside_solid_bytes.resize(p_size.x * sizeof(uint32_t));
	for (int z = 0; z < p_size.z; z++) {
		for (int y = 0; y < p_size.y; y++) {
			const int source_probe = p_size.x * (y + p_size.y * z);
			const int destination_probe = p_begin.x + volume->resolution.x * (p_begin.y + y + volume->resolution.y * (p_begin.z + z));
			uint32_t *write = reinterpret_cast<uint32_t *>(inside_solid_bytes.ptrw());
			for (int x = 0; x < p_size.x; x++) {
				*write++ = p_inside_solid[source_probe + x] != 0 ? 1 : 0;
			}
			RD::get_singleton()->buffer_update(volume->inside_solid_buffer, destination_probe * sizeof(uint32_t), inside_solid_bytes.size(), inside_solid_bytes.ptr());
		}
	}
	// Preserve published history and in-flight offsets so continuous geometry motion cannot repeatedly restart propagation.
	_mark_visibility_dirty(*volume);
	volume->injection_dirty = true;
}

void LocalLRT::volume_set_inside_solid(RID p_volume, const Vector<int> &p_inside_solid) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	ERR_FAIL_COND(!volume->inside_solid_buffer.is_valid());
	const int probe_count = volume->resolution.x * volume->resolution.y * volume->resolution.z;
	ERR_FAIL_COND(p_inside_solid.size() != probe_count);

	Vector<uint8_t> bytes;
	bytes.resize(probe_count * sizeof(uint32_t));
	uint32_t *write = reinterpret_cast<uint32_t *>(bytes.ptrw());
	for (int i = 0; i < probe_count; i++) {
		*write++ = p_inside_solid[i] != 0 ? 1 : 0;
	}
	RD::get_singleton()->buffer_update(volume->inside_solid_buffer, 0, bytes.size(), bytes.ptr());
	volume->injection_dirty = true;
}

void LocalLRT::volume_set_injection(RID p_volume, const Vector<Vector4> &p_injection) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	ERR_FAIL_COND(!volume->injection_buffers[0].is_valid() || !volume->injection_buffers[1].is_valid());
	const int value_count = volume->resolution.x * volume->resolution.y * volume->resolution.z * 3;
	ERR_FAIL_COND(p_injection.size() != value_count);

	Vector<uint8_t> bytes;
	bytes.resize(value_count * 4 * sizeof(float));
	float *write = reinterpret_cast<float *>(bytes.ptrw());
	for (const Vector4 &value : p_injection) {
		*write++ = value.x;
		*write++ = value.y;
		*write++ = value.z;
		*write++ = value.w;
	}
	RD::get_singleton()->buffer_update(volume->injection_buffers[0], 0, bytes.size(), bytes.ptr());
	RD::get_singleton()->buffer_update(volume->injection_buffers[1], 0, bytes.size(), bytes.ptr());
	volume->injection_is_a = true;
	volume->injection_pending = false;
	volume->injection_probe_offset = 0;
	volume->injection_dirty = false;
}

static void store_push_vec4(float *p_dst, const Vector3 &p_value, float p_w = 0.0f) {
	p_dst[0] = p_value.x;
	p_dst[1] = p_value.y;
	p_dst[2] = p_value.z;
	p_dst[3] = p_w;
}

void LocalLRT::_update_environment_sh(Volume &r_volume, RID p_sky_texture, bool p_sky_texture_is_array, const Color &p_ambient_color, float p_sky_mix, float p_sky_energy, const Basis &p_sky_orientation, float p_sky_border_size) {
	ERR_FAIL_COND(!r_volume.environment_data_buffer.is_valid() || !r_volume.environment_sh_buffer.is_valid());
	ERR_FAIL_COND(!_ensure_environment_shader());
	_ensure_default_sky_textures();

	const int mode = p_sky_texture_is_array ? 1 : 0;
	const bool use_sky = p_sky_texture.is_valid() && p_sky_mix > 0.0f;
	const RID sky_texture = use_sky ? p_sky_texture : default_sky_textures[mode];
	ERR_FAIL_COND(!sky_texture.is_valid());
	const Basis world_to_sky = p_sky_orientation.inverse();
	Vector<Vector4> environment_data;
	environment_data.resize(4);
	environment_data.write[0] = Vector4(p_ambient_color.r, p_ambient_color.g, p_ambient_color.b, use_sky ? CLAMP(p_sky_mix, 0.0f, 1.0f) : 0.0f);
	environment_data.write[1] = Vector4(world_to_sky.get_column(0).x, world_to_sky.get_column(0).y, world_to_sky.get_column(0).z, p_sky_energy);
	environment_data.write[2] = Vector4(world_to_sky.get_column(1).x, world_to_sky.get_column(1).y, world_to_sky.get_column(1).z, p_sky_border_size);
	environment_data.write[3] = Vector4(world_to_sky.get_column(2).x, world_to_sky.get_column(2).y, world_to_sky.get_column(2).z, 1.0f - p_sky_border_size * 2.0f);
	const bool environment_changed = environment_data != r_volume.environment_data || sky_texture != r_volume.environment_sky_texture || mode != r_volume.environment_mode;
	if (!environment_changed) {
		return;
	}
	r_volume.environment_data = environment_data;
	r_volume.environment_sky_texture = sky_texture;
	r_volume.environment_mode = mode;
	r_volume.injection_dirty = true;
	Vector<uint8_t> bytes;
	bytes.resize(environment_data.size() * 4 * sizeof(float));
	float *write = reinterpret_cast<float *>(bytes.ptrw());
	for (const Vector4 &value : environment_data) {
		*write++ = value.x;
		*write++ = value.y;
		*write++ = value.z;
		*write++ = value.w;
	}
	RD::get_singleton()->buffer_update(r_volume.environment_data_buffer, 0, bytes.size(), bytes.ptr());

	const RID shader = environment_shader->version_get_shader(environment_shader_version, mode);
	const RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RENDER_TIMESTAMP("Local LRT Environment Injection");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, environment_pipelines[mode]);
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(
			shader,
			0,
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, sky_texture })),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_volume.environment_data_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, r_volume.environment_sh_buffer));
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_dispatch(compute_list, 1, 1, 1);
	RD::get_singleton()->compute_list_end();
	RENDER_TIMESTAMP("< Local LRT Environment Injection");
}

void LocalLRT::_inject_analytic_lights(Volume &r_volume, const Vector<Vector4> &p_lights) {
	ERR_FAIL_COND(!r_volume.injection_buffers[0].is_valid() || !r_volume.injection_buffers[1].is_valid() || !r_volume.inside_solid_buffer.is_valid());
	ERR_FAIL_COND(!_ensure_injection_shader());
	ERR_FAIL_COND(p_lights.size() % 9 != 0);

	r_volume.requested_analytic_lights = p_lights;
	if (!r_volume.injection_pending && (r_volume.requested_analytic_lights != r_volume.analytic_lights || r_volume.injection_dirty)) {
		r_volume.analytic_lights = r_volume.requested_analytic_lights;
		Vector<Vector4> lights = r_volume.analytic_lights;
		if (lights.is_empty()) {
			lights.push_back(Vector4());
		}
		Vector<uint8_t> bytes;
		bytes.resize(lights.size() * 4 * sizeof(float));
		float *write = reinterpret_cast<float *>(bytes.ptrw());
		for (const Vector4 &value : lights) {
			*write++ = value.x;
			*write++ = value.y;
			*write++ = value.z;
			*write++ = value.w;
		}
		if (!r_volume.analytic_lights_buffer.is_valid() || r_volume.analytic_lights_buffer_bytes < (uint32_t)bytes.size()) {
			if (r_volume.analytic_lights_buffer.is_valid()) {
				RD::get_singleton()->free_rid(r_volume.analytic_lights_buffer);
			}
			r_volume.analytic_lights_buffer = RD::get_singleton()->storage_buffer_create(bytes.size(), bytes);
			r_volume.analytic_lights_buffer_bytes = bytes.size();
		} else {
			RD::get_singleton()->buffer_update(r_volume.analytic_lights_buffer, 0, bytes.size(), bytes.ptr());
		}
		r_volume.injection_pending = true;
		r_volume.injection_probe_offset = 0;
		r_volume.injection_dirty = false;
	}
	if (!r_volume.injection_pending) {
		return;
	}

	const int probe_count = r_volume.resolution.x * r_volume.resolution.y * r_volume.resolution.z;
	InjectionPushConstant push_constant = {};
	push_constant.resolution[0] = r_volume.resolution.x;
	push_constant.resolution[1] = r_volume.resolution.y;
	push_constant.resolution[2] = r_volume.resolution.z;
	push_constant.probe_count = probe_count;
	push_constant.size[0] = r_volume.size.x;
	push_constant.size[1] = r_volume.size.y;
	push_constant.size[2] = r_volume.size.z;
	push_constant.light_count = r_volume.analytic_lights.size() / 9;
	store_push_vec4(push_constant.xform_x, r_volume.transform.basis.get_column(0));
	store_push_vec4(push_constant.xform_y, r_volume.transform.basis.get_column(1));
	store_push_vec4(push_constant.xform_z, r_volume.transform.basis.get_column(2));
	store_push_vec4(push_constant.xform_origin, r_volume.transform.origin);
	_ensure_default_shadow_texture();
	_ensure_shadow_visibility_buffer(r_volume);
	push_constant.directional_shadow_bias = r_volume.shadow_bias;
	push_constant.directional_shadow_enabled = r_volume.shadow_enabled ? 1 : 0;
	push_constant.directional_shadow_resolution = MAX(r_volume.shadow_resolution, 1);
	push_constant.positional_shadow_resolution = MAX(r_volume.positional_shadow_resolution, 1);
	const int dispatch_probe_count = r_volume.injection_probe_budget > 0 ? MIN(r_volume.injection_probe_budget, probe_count - r_volume.injection_probe_offset) : probe_count - r_volume.injection_probe_offset;
	push_constant.probe_offset = r_volume.injection_probe_offset;
	push_constant.dispatch_probe_count = dispatch_probe_count;

	const RID shader = injection_shader->version_get_shader(injection_shader_version, 0);
	const RID shadow_texture = _shadow_sample_texture(r_volume);
	ERR_FAIL_COND(!shadow_texture.is_valid());
	const RID positional_shadow_texture = r_volume.positional_shadow_texture.is_valid() ? r_volume.positional_shadow_texture : default_shadow_texture;
	const RID nearest_sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const int pending_index = r_volume.injection_is_a ? 1 : 0;
	RENDER_TIMESTAMP("Local LRT Analytic Injection");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, injection_pipeline);
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(
			shader,
			0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, r_volume.analytic_lights_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_volume.inside_solid_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, r_volume.injection_buffers[pending_index]),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, r_volume.shadow_visibility_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, shadow_texture })),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, r_volume.shadow_matrix_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, positional_shadow_texture })),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, r_volume.global_visibility_buffers[r_volume.global_visibility_is_a ? 0 : 1]),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, r_volume.environment_sh_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, r_volume.environment_injection_buffer));
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(InjectionPushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, dispatch_probe_count, 1, 1);
	RD::get_singleton()->compute_list_end();
	r_volume.injection_probe_offset += dispatch_probe_count;
	if (r_volume.injection_probe_offset == probe_count) {
		r_volume.injection_probe_offset = 0;
		r_volume.injection_is_a = pending_index == 0;
		r_volume.injection_pending = false;
		r_volume.injection_dirty = r_volume.injection_dirty || r_volume.requested_analytic_lights != r_volume.analytic_lights;
		r_volume.radiance_probe_offset = 0;
		r_volume.radiance_pattern_phase = 0;
	}
	RENDER_TIMESTAMP("< Local LRT Analytic Injection");
}

void LocalLRT::volume_set_environment(RID p_volume, RID p_sky_texture, bool p_sky_texture_is_array, const Color &p_ambient_color, float p_sky_mix, float p_sky_energy, const Basis &p_sky_orientation, float p_sky_border_size) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	if (!volume->environment_data_buffer.is_valid()) {
		return;
	}
	_update_environment_sh(*volume, p_sky_texture, p_sky_texture_is_array, p_ambient_color, p_sky_mix, p_sky_energy, p_sky_orientation, p_sky_border_size);
}

void LocalLRT::volume_inject_analytic_lights(RID p_volume, const Vector<Vector4> &p_lights) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	_inject_analytic_lights(*volume, p_lights);
}

void LocalLRT::volume_set_directional_shadow(RID p_volume, const Vector<float> &p_depths, int p_size, const Transform3D &p_camera, const Projection &p_projection, float p_bias) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	if (p_size <= 0 || p_depths.is_empty()) {
		volume_clear_directional_shadow(p_volume);
		return;
	}
	ERR_FAIL_COND(p_depths.size() != p_size * p_size);
	ERR_FAIL_COND(!RD::get_singleton());

	_ensure_default_shadow_texture();
	_ensure_shadow_visibility_buffer(*volume);
	if (volume->shadow_upload_texture.is_valid()) {
		RD::get_singleton()->free_rid(volume->shadow_upload_texture);
		volume->shadow_upload_texture = RID();
	}

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.width = p_size;
	tf.height = p_size;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<uint8_t> bytes;
	bytes.resize(p_depths.size() * sizeof(float));
	memcpy(bytes.ptrw(), p_depths.ptr(), bytes.size());
	Vector<Vector<uint8_t>> layers;
	layers.push_back(bytes);
	volume->shadow_upload_texture = RD::get_singleton()->texture_create(tf, RD::TextureView(), layers);
	volume->shadow_resolution = p_size;
	volume->shadow_bias = p_bias;
	volume->shadow_enabled = true;
	volume->shadow_use_upload = true;
	_upload_shadow_matrix(*volume, LocalLRTMath::directional_shadow_view_projection(p_camera, p_projection));
	volume->injection_dirty = true;
}

RID LocalLRT::volume_prepare_raster_shadow(RID p_volume, const Transform3D &p_camera, const Projection &p_projection, float p_bias) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, RID());
	ERR_FAIL_COND_V(!RD::get_singleton(), RID());
	_ensure_default_shadow_texture();
	_ensure_shadow_visibility_buffer(*volume);
	_ensure_raster_shadow(*volume);
	volume->shadow_resolution = DIRECTIONAL_SHADOW_SIZE;
	const float z_range = MAX(p_projection.get_z_far() - p_projection.get_z_near(), 0.001);
	volume->shadow_bias = p_bias / z_range;
	volume->shadow_enabled = true;
	volume->shadow_use_upload = false;
	_upload_shadow_matrix(*volume, LocalLRTMath::directional_shadow_view_projection(p_camera, p_projection));
	volume->injection_dirty = true;
	return volume->shadow_framebuffer;
}

void LocalLRT::volume_clear_directional_shadow(RID p_volume) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	const bool was_enabled = volume->shadow_enabled;
	volume->shadow_enabled = false;
	volume->shadow_use_upload = false;
	volume->shadow_bias = 0.0f;
	volume->shadow_resolution = 1;
	volume->injection_dirty = volume->injection_dirty || was_enabled;
}

void LocalLRT::volume_set_positional_shadow_atlas(RID p_volume, RID p_texture, int p_resolution) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	const int resolution = MAX(p_resolution, 1);
	if (volume->positional_shadow_texture != p_texture || volume->positional_shadow_resolution != resolution) {
		volume->positional_shadow_texture = p_texture;
		volume->positional_shadow_resolution = resolution;
		volume->injection_dirty = true;
	}
}

void LocalLRT::volume_propagate_visibility(RID p_volume) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	_propagate_visibility(*volume, volume->visibility_iterations);
}

void LocalLRT::volume_propagate_radiance(RID p_volume) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	_propagate_radiance(*volume, volume->radiance_iterations);
}

AABB LocalLRT::volume_get_bounds(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, AABB());
	return AABB(-volume->size * 0.5, volume->size);
}

RID LocalLRT::get_first_enabled_volume() const {
	const Vector<RID> volumes = get_enabled_volumes();
	return volumes.is_empty() ? RID() : volumes[0];
}

Vector<RID> LocalLRT::get_enabled_volumes() const {
	Vector<RID> volumes;
	for (const RID &rid : volume_owner.get_owned_list()) {
		const Volume *volume = volume_owner.get_or_null(rid);
		if (volume && volume->enabled && volume->injection_buffers[volume->injection_is_a ? 0 : 1].is_valid()) {
			volumes.push_back(rid);
		}
	}
	return volumes;
}

Vector<RID> LocalLRT::get_sorted_enabled_volumes() const {
	struct SortedVolume {
		int priority = 0;
		uint64_t id = 0;
		RID rid;
	};
	struct SortedVolumeCompare {
		_FORCE_INLINE_ bool operator()(const SortedVolume &p_a, const SortedVolume &p_b) const {
			return LocalLRTMath::volume_priority_before(p_a.priority, p_a.id, p_b.priority, p_b.id);
		}
	};

	Vector<SortedVolume> sorted;
	for (const RID &rid : volume_owner.get_owned_list()) {
		const Volume *volume = volume_owner.get_or_null(rid);
		if (!volume || !volume->enabled) {
			continue;
		}
		SortedVolume item;
		item.priority = volume->priority;
		item.id = rid.get_id();
		item.rid = rid;
		sorted.push_back(item);
	}
	sorted.sort_custom<SortedVolumeCompare>();

	Vector<RID> volumes;
	volumes.resize(sorted.size());
	for (int i = 0; i < sorted.size(); i++) {
		volumes.write[i] = sorted[i].rid;
	}
	return volumes;
}

AABB LocalLRT::volume_get_world_aabb(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, AABB());
	return volume->transform.xform(AABB(-volume->size * 0.5, volume->size));
}

Vector<Vector4> LocalLRT::volume_get_global_visibility(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, Vector<Vector4>());
	if (!volume->global_visibility_buffers[0].is_valid()) {
		return Vector<Vector4>();
	}

	const int buffer_index = volume->global_visibility_is_a ? 0 : 1;
	return _read_vector4_buffer(volume->global_visibility_buffers[buffer_index], volume->local_visibility.size());
}

Vector<Vector4> LocalLRT::volume_get_injection(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, Vector<Vector4>());
	const RID injection_buffer = volume->injection_buffers[volume->injection_is_a ? 0 : 1];
	if (!injection_buffer.is_valid()) {
		return Vector<Vector4>();
	}
	return _read_vector4_buffer(injection_buffer, volume->resolution.x * volume->resolution.y * volume->resolution.z * 3);
}

Vector<Vector4> LocalLRT::volume_get_environment_injection(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, Vector<Vector4>());
	if (!volume->environment_injection_buffer.is_valid()) {
		return Vector<Vector4>();
	}
	return _read_vector4_buffer(volume->environment_injection_buffer, volume->resolution.x * volume->resolution.y * volume->resolution.z * 3);
}

Vector<Vector4> LocalLRT::volume_get_radiance(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, Vector<Vector4>());
	if (!volume->radiance_buffers[0].is_valid()) {
		return Vector<Vector4>();
	}
	const int buffer_index = volume->radiance_is_a ? 0 : 1;
	return _read_vector4_buffer(volume->radiance_buffers[buffer_index], volume->resolution.x * volume->resolution.y * volume->resolution.z * 3);
}

Vector<float> LocalLRT::volume_get_shadow_visibility(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, Vector<float>());
	if (!volume->shadow_visibility_buffer.is_valid()) {
		return Vector<float>();
	}
	return _read_float_buffer(volume->shadow_visibility_buffer, volume->resolution.x * volume->resolution.y * volume->resolution.z);
}

bool LocalLRT::volume_has_gpu_resources(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, false);
	return volume->local_visibility_buffer.is_valid() && volume->local_transfer_buffer.is_valid() &&
			volume->mesh_light_buffer.is_valid() &&
			volume->global_visibility_buffers[0].is_valid() && volume->global_visibility_buffers[1].is_valid() &&
			volume->radiance_buffers[0].is_valid() && volume->radiance_buffers[1].is_valid() &&
			volume->injection_buffers[0].is_valid() && volume->injection_buffers[1].is_valid() && volume->environment_data_buffer.is_valid() &&
			volume->environment_sh_buffer.is_valid() && volume->environment_injection_buffer.is_valid() &&
			volume->inside_solid_buffer.is_valid();
}

int LocalLRT::get_surface_data(SurfaceData *r_data, int p_max, const Plane *p_frustum_planes, int p_plane_count) const {
	ERR_FAIL_COND_V(p_max < 0, 0);
	if (p_max == 0 || r_data == nullptr) {
		return 0;
	}
	const Vector<RID> volumes = get_camera_volumes(p_max, p_frustum_planes, p_plane_count);
	for (int i = 0; i < volumes.size(); i++) {
		const Volume *volume = volume_owner.get_or_null(volumes[i]);
		ERR_CONTINUE(!volume);

		r_data[i].world_to_local = volume->transform.affine_inverse();
		r_data[i].size = volume->size;
		r_data[i].resolution = volume->resolution;
		r_data[i].energy = volume->energy;
		r_data[i].edge_blend_distance = volume->edge_blend_distance;
		r_data[i].global_visibility_buffer = volume->global_visibility_buffers[volume->global_visibility_is_a ? 0 : 1];
		r_data[i].radiance_buffer = volume->radiance_buffers[volume->radiance_is_a ? 0 : 1];
		r_data[i].inside_solid_buffer = volume->inside_solid_buffer;
	}
	return volumes.size();
}

Vector<RID> LocalLRT::get_camera_volumes(int p_max, const Plane *p_frustum_planes, int p_plane_count) const {
	ERR_FAIL_COND_V(p_max < 0, Vector<RID>());
	if (p_max == 0) {
		return Vector<RID>();
	}

	Vector<LocalLRTMath::CameraVolumeCandidate> candidates;
	Vector<RID> rids;
	for (const RID &rid : volume_owner.get_owned_list()) {
		const Volume *volume = volume_owner.get_or_null(rid);
		if (!volume || !volume->enabled || !volume->radiance_buffers[0].is_valid()) {
			continue;
		}

		LocalLRTMath::CameraVolumeCandidate item;
		item.priority = volume->priority;
		item.id = rid.get_id();
		item.world_aabb = volume->transform.xform(AABB(-volume->size * 0.5, volume->size));
		candidates.push_back(item);
		rids.push_back(rid);
	}

	int indices[LocalLRTMath::MAX_BLEND_VOLUMES];
	const int count = LocalLRTMath::select_camera_volumes(candidates.ptr(), candidates.size(), p_frustum_planes, p_plane_count, p_max, indices);
	Vector<RID> selected_volumes;
	selected_volumes.resize(count);
	for (int i = 0; i < count; i++) {
		selected_volumes.write[i] = rids[indices[i]];
	}
	return selected_volumes;
}

LocalLRT::~LocalLRT() {
	if (RD::get_singleton()) {
		if (visibility_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(visibility_pipeline);
		}
		if (radiance_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(radiance_pipeline);
		}
		if (injection_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(injection_pipeline);
		}
		for (RID &pipeline : environment_pipelines) {
			if (pipeline.is_valid()) {
				RD::get_singleton()->free_rid(pipeline);
			}
		}
		if (default_shadow_texture.is_valid()) {
			RD::get_singleton()->free_rid(default_shadow_texture);
		}
		for (RID &texture : default_sky_textures) {
			if (texture.is_valid()) {
				RD::get_singleton()->free_rid(texture);
			}
		}
	}
	if (visibility_shader_initialized) {
		visibility_shader->version_free(visibility_shader_version);
		memdelete(visibility_shader);
	}
	if (radiance_shader_initialized) {
		radiance_shader->version_free(radiance_shader_version);
		memdelete(radiance_shader);
	}
	if (injection_shader_initialized) {
		injection_shader->version_free(injection_shader_version);
		memdelete(injection_shader);
	}
	if (environment_shader_initialized) {
		environment_shader->version_free(environment_shader_version);
		memdelete(environment_shader);
	}
}

} // namespace RendererRD
