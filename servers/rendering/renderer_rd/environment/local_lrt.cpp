/**************************************************************************/
/*  local_lrt.cpp                                                         */
/**************************************************************************/

#include "local_lrt.h"

#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"

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

void LocalLRT::_free_gpu_resources(Volume &r_volume) {
	RID *resources[] = {
		&r_volume.local_visibility_buffer,
		&r_volume.local_transfer_buffer,
		&r_volume.global_visibility_buffers[0],
		&r_volume.global_visibility_buffers[1],
		&r_volume.radiance_buffers[0],
		&r_volume.radiance_buffers[1],
		&r_volume.injection_buffer,
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
	for (int iteration = 0; iteration < r_volume.propagation_iterations; iteration++) {
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
	r_volume.global_visibility_is_a = (r_volume.propagation_iterations & 1) == 0;
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

void LocalLRT::volume_set_propagation_iterations(RID p_volume, int p_iterations) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	volume->propagation_iterations = p_iterations;
	_reset_and_propagate_visibility(*volume);
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

void LocalLRT::volume_set_static_data(RID p_volume, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	const int probe_count = volume->resolution.x * volume->resolution.y * volume->resolution.z;
	ERR_FAIL_COND(p_local_visibility.size() != probe_count);
	ERR_FAIL_COND(p_local_transfer.size() != probe_count * 12);
	ERR_FAIL_COND(!_ensure_visibility_shader());

	_free_gpu_resources(*volume);
	volume->local_visibility = p_local_visibility;
	volume->local_visibility_buffer = _create_vector4_buffer(p_local_visibility);
	volume->local_transfer_buffer = _create_vector4_buffer(p_local_transfer);
	volume->global_visibility_buffers[0] = _create_vector4_buffer(p_local_visibility);
	volume->global_visibility_buffers[1] = _create_vector4_buffer(p_local_visibility);

	Vector<Vector4> zero_radiance;
	zero_radiance.resize(probe_count * 3);
	volume->radiance_buffers[0] = _create_vector4_buffer(zero_radiance);
	volume->radiance_buffers[1] = _create_vector4_buffer(zero_radiance);
	volume->injection_buffer = _create_vector4_buffer(zero_radiance);
	_reset_and_propagate_visibility(*volume);
}

void LocalLRT::volume_set_injection(RID p_volume, const Vector<Vector4> &p_injection) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	ERR_FAIL_COND(!volume->injection_buffer.is_valid());
	ERR_FAIL_COND(p_injection.size() != volume->resolution.x * volume->resolution.y * volume->resolution.z * 3);

	Vector<uint8_t> bytes;
	bytes.resize(p_injection.size() * 4 * sizeof(float));
	float *write = reinterpret_cast<float *>(bytes.ptrw());
	for (const Vector4 &value : p_injection) {
		*write++ = value.x;
		*write++ = value.y;
		*write++ = value.z;
		*write++ = value.w;
	}
	RD::get_singleton()->buffer_update(volume->injection_buffer, 0, bytes.size(), bytes.ptr());
}

AABB LocalLRT::volume_get_bounds(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, AABB());
	return AABB(-volume->size * 0.5, volume->size);
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

bool LocalLRT::volume_has_gpu_resources(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, false);
	return volume->local_visibility_buffer.is_valid() && volume->local_transfer_buffer.is_valid() &&
			volume->global_visibility_buffers[0].is_valid() && volume->global_visibility_buffers[1].is_valid() &&
			volume->radiance_buffers[0].is_valid() && volume->radiance_buffers[1].is_valid() && volume->injection_buffer.is_valid();
}

LocalLRT::~LocalLRT() {
	if (visibility_shader_initialized) {
		visibility_shader->version_free(visibility_shader_version);
		memdelete(visibility_shader);
	}
}

} // namespace RendererRD
