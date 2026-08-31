/**************************************************************************/
/*  local_lrt.cpp                                                         */
/**************************************************************************/

#include "local_lrt.h"

#include "core/math/math_funcs.h"
#include "scene/3d/local_lrt_math.h"
#include "servers/rendering/renderer_rd/shaders/environment/local_lrt_injection.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"

#include <cstring>

namespace RendererRD {

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
	modes.push_back(String());
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

void LocalLRT::_free_gpu_resources(Volume &r_volume) {
	RID *resources[] = {
		&r_volume.local_visibility_buffer,
		&r_volume.local_transfer_buffer,
		&r_volume.mesh_light_buffer,
		&r_volume.global_visibility_buffers[0],
		&r_volume.global_visibility_buffers[1],
		&r_volume.radiance_buffers[0],
		&r_volume.radiance_buffers[1],
		&r_volume.injection_buffer,
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
	r_volume.shadow_resolution = 1;
	r_volume.shadow_bias = 0.0f;
	r_volume.shadow_enabled = false;
	r_volume.shadow_use_upload = false;
	r_volume.positional_shadow_texture = RID();
	r_volume.positional_shadow_resolution = 1;
	r_volume.global_visibility_is_a = true;
	r_volume.radiance_is_a = true;
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

void LocalLRT::_reset_and_propagate_visibility(Volume &r_volume) {
	if (r_volume.local_visibility.is_empty() || !_ensure_visibility_shader()) {
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

	VisibilityPushConstant push_constant = {};
	push_constant.resolution[0] = r_volume.resolution.x;
	push_constant.resolution[1] = r_volume.resolution.y;
	push_constant.resolution[2] = r_volume.resolution.z;
	push_constant.probe_count = r_volume.local_visibility.size();

	const RID shader = visibility_shader->version_get_shader(visibility_shader_version, 0);
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, visibility_pipeline);
	for (int iteration = 0; iteration < r_volume.visibility_iterations; iteration++) {
		if (iteration > 0) {
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}
		const int source = iteration & 1;
		const int destination = source ^ 1;
		RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(
				shader,
				0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, r_volume.local_visibility_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_volume.global_visibility_buffers[source]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, r_volume.global_visibility_buffers[destination]));
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(VisibilityPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, push_constant.probe_count, 1, 1);
	}
	RD::get_singleton()->compute_list_end();
	r_volume.global_visibility_is_a = (r_volume.visibility_iterations & 1) == 0;
}

void LocalLRT::_propagate_radiance(Volume &r_volume, int p_iterations) {
	if (p_iterations <= 0 || !r_volume.mesh_light_buffer.is_valid() || !r_volume.injection_buffer.is_valid() || !r_volume.inside_solid_buffer.is_valid() || !_ensure_radiance_shader()) {
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

	const RID shader = radiance_shader->version_get_shader(radiance_shader_version, 0);
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, radiance_pipeline);
	int source = r_volume.radiance_is_a ? 0 : 1;
	for (int iteration = 0; iteration < p_iterations; iteration++) {
		if (iteration > 0) {
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}
		const int destination = source ^ 1;
		RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(
				shader,
				0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, r_volume.local_visibility_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_volume.local_transfer_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, r_volume.local_visibility_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, r_volume.injection_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, r_volume.mesh_light_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, r_volume.radiance_buffers[source]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, r_volume.radiance_buffers[destination]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, r_volume.inside_solid_buffer));
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(RadiancePushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, probe_count, 1, 1);
		source = destination;
	}
	RD::get_singleton()->compute_list_end();
	r_volume.radiance_is_a = source == 0;
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
	volume->size = p_size;
	volume->resolution = p_resolution;
}

void LocalLRT::volume_set_transform(RID p_volume, const Transform3D &p_transform) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->transform = p_transform;
}

void LocalLRT::volume_set_visibility_iterations(RID p_volume, int p_iterations) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->visibility_iterations = p_iterations;
	_reset_and_propagate_visibility(*volume);
}

void LocalLRT::volume_set_propagation_iterations(RID p_volume, int p_iterations) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->radiance_iterations = p_iterations;
}

void LocalLRT::volume_set_energy(RID p_volume, float p_energy) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->energy = p_energy;
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
	volume->local_transfer_buffer = _create_vector4_buffer(p_local_transfer);
	volume->mesh_light_buffer = _create_vector4_buffer(p_mesh_light);
	volume->global_visibility_buffers[0] = _create_vector4_buffer(p_local_visibility);
	volume->global_visibility_buffers[1] = _create_vector4_buffer(p_local_visibility);

	Vector<Vector4> zero_radiance;
	zero_radiance.resize(probe_count * 3);
	volume->radiance_buffers[0] = _create_vector4_buffer(zero_radiance);
	volume->radiance_buffers[1] = _create_vector4_buffer(zero_radiance);
	volume->injection_buffer = _create_vector4_buffer(zero_radiance);
	Vector<uint32_t> zero_inside_solid;
	zero_inside_solid.resize_initialized(probe_count);
	volume->inside_solid_buffer = _create_uint_buffer(zero_inside_solid);
	_ensure_shadow_visibility_buffer(*volume);
	_reset_and_propagate_visibility(*volume);
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
	update_vector4_region(volume->local_transfer_buffer, p_local_transfer, 12);
	update_vector4_region(volume->mesh_light_buffer, p_mesh_light, 3);

	Vector<uint8_t> inside_solid_bytes;
	inside_solid_bytes.resize(p_size.x * sizeof(uint32_t));
	Vector<uint8_t> zero_radiance_bytes;
	zero_radiance_bytes.resize(p_size.x * 3 * 4 * sizeof(float));
	memset(zero_radiance_bytes.ptrw(), 0, zero_radiance_bytes.size());
	for (int z = 0; z < p_size.z; z++) {
		for (int y = 0; y < p_size.y; y++) {
			const int source_probe = p_size.x * (y + p_size.y * z);
			const int destination_probe = p_begin.x + volume->resolution.x * (p_begin.y + y + volume->resolution.y * (p_begin.z + z));
			uint32_t *write = reinterpret_cast<uint32_t *>(inside_solid_bytes.ptrw());
			for (int x = 0; x < p_size.x; x++) {
				*write++ = p_inside_solid[source_probe + x] != 0 ? 1 : 0;
			}
			RD::get_singleton()->buffer_update(volume->inside_solid_buffer, destination_probe * sizeof(uint32_t), inside_solid_bytes.size(), inside_solid_bytes.ptr());
			const uint32_t radiance_offset = destination_probe * 3 * 4 * sizeof(float);
			RD::get_singleton()->buffer_update(volume->radiance_buffers[0], radiance_offset, zero_radiance_bytes.size(), zero_radiance_bytes.ptr());
			RD::get_singleton()->buffer_update(volume->radiance_buffers[1], radiance_offset, zero_radiance_bytes.size(), zero_radiance_bytes.ptr());
		}
	}
	_reset_and_propagate_visibility(*volume);
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
}

void LocalLRT::volume_set_injection(RID p_volume, const Vector<Vector4> &p_injection) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	ERR_FAIL_COND(!volume->injection_buffer.is_valid());
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
	RD::get_singleton()->buffer_update(volume->injection_buffer, 0, bytes.size(), bytes.ptr());
}

static void store_push_vec4(float *p_dst, const Vector3 &p_value, float p_w = 0.0f) {
	p_dst[0] = p_value.x;
	p_dst[1] = p_value.y;
	p_dst[2] = p_value.z;
	p_dst[3] = p_w;
}

void LocalLRT::_inject_analytic_lights(Volume &r_volume, const Vector<Vector4> &p_lights) {
	ERR_FAIL_COND(!r_volume.injection_buffer.is_valid() || !r_volume.inside_solid_buffer.is_valid());
	ERR_FAIL_COND(!_ensure_injection_shader());
	ERR_FAIL_COND(p_lights.size() % 9 != 0);

	Vector<Vector4> lights = p_lights;
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
			r_volume.analytic_lights_buffer = RID();
		}
		r_volume.analytic_lights_buffer = RD::get_singleton()->storage_buffer_create(bytes.size(), bytes);
		r_volume.analytic_lights_buffer_bytes = bytes.size();
	} else {
		RD::get_singleton()->buffer_update(r_volume.analytic_lights_buffer, 0, bytes.size(), bytes.ptr());
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
	push_constant.light_count = p_lights.size() / 9;
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

	const RID shader = injection_shader->version_get_shader(injection_shader_version, 0);
	const RID shadow_texture = _shadow_sample_texture(r_volume);
	ERR_FAIL_COND(!shadow_texture.is_valid());
	const RID positional_shadow_texture = r_volume.positional_shadow_texture.is_valid() ? r_volume.positional_shadow_texture : default_shadow_texture;
	const RID nearest_sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, injection_pipeline);
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(
			shader,
			0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, r_volume.analytic_lights_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_volume.inside_solid_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, r_volume.injection_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, r_volume.shadow_visibility_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, shadow_texture })),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, r_volume.shadow_matrix_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, positional_shadow_texture })));
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(InjectionPushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, probe_count, 1, 1);
	RD::get_singleton()->compute_list_end();
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
	return volume->shadow_framebuffer;
}

void LocalLRT::volume_clear_directional_shadow(RID p_volume) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->shadow_enabled = false;
	volume->shadow_use_upload = false;
	volume->shadow_bias = 0.0f;
	volume->shadow_resolution = 1;
}

void LocalLRT::volume_set_positional_shadow_atlas(RID p_volume, RID p_texture, int p_resolution) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->positional_shadow_texture = p_texture;
	volume->positional_shadow_resolution = MAX(p_resolution, 1);
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
	for (const RID &rid : volume_owner.get_owned_list()) {
		const Volume *volume = volume_owner.get_or_null(rid);
		if (volume && volume->enabled && volume->injection_buffer.is_valid()) {
			return rid;
		}
	}
	return RID();
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
	if (!volume->injection_buffer.is_valid()) {
		return Vector<Vector4>();
	}
	return _read_vector4_buffer(volume->injection_buffer, volume->resolution.x * volume->resolution.y * volume->resolution.z * 3);
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
			volume->injection_buffer.is_valid() &&
			volume->inside_solid_buffer.is_valid();
}

bool LocalLRT::get_surface_data(SurfaceData &r_data) const {
	for (const RID &rid : volume_owner.get_owned_list()) {
		const Volume *volume = volume_owner.get_or_null(rid);
		if (!volume || !volume->enabled || !volume->radiance_buffers[0].is_valid()) {
			continue;
		}

		r_data.world_to_local = volume->transform.affine_inverse();
		r_data.size = volume->size;
		r_data.resolution = volume->resolution;
		r_data.energy = volume->energy;
		r_data.edge_blend_distance = volume->edge_blend_distance;
		r_data.local_visibility_buffer = volume->local_visibility_buffer;
		r_data.radiance_buffer = volume->radiance_buffers[volume->radiance_is_a ? 0 : 1];
		r_data.inside_solid_buffer = volume->inside_solid_buffer;
		return true;
	}
	return false;
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
		if (default_shadow_texture.is_valid()) {
			RD::get_singleton()->free_rid(default_shadow_texture);
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
}

} // namespace RendererRD
