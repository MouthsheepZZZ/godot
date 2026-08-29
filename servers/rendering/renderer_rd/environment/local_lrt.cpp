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

void LocalLRT::_free_gpu_resources(Volume &r_volume) {
	RID *resources[] = {
		&r_volume.local_visibility_buffer,
		&r_volume.local_transfer_buffer,
		&r_volume.global_visibility_buffers[0],
		&r_volume.global_visibility_buffers[1],
		&r_volume.radiance_buffers[0],
		&r_volume.radiance_buffers[1],
		&r_volume.injection_buffer,
		&r_volume.emissive_injection_buffer,
		&r_volume.inside_solid_buffer,
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
	if (p_iterations <= 0 || !r_volume.injection_buffer.is_valid() || !r_volume.emissive_injection_buffer.is_valid() || !r_volume.inside_solid_buffer.is_valid() || !_ensure_radiance_shader()) {
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
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, r_volume.emissive_injection_buffer),
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
	volume->emissive_injection_buffer = _create_vector4_buffer(zero_radiance);
	Vector<uint32_t> zero_inside_solid;
	zero_inside_solid.resize(probe_count);
	volume->inside_solid_buffer = _create_uint_buffer(zero_inside_solid);
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

void LocalLRT::volume_set_injection(RID p_volume, const Vector<Vector4> &p_injection, const Vector<Vector4> &p_emissive_injection) {
	Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL(volume);
	ERR_FAIL_COND(!volume->injection_buffer.is_valid());
	const int value_count = volume->resolution.x * volume->resolution.y * volume->resolution.z * 3;
	ERR_FAIL_COND(p_injection.size() != value_count);
	ERR_FAIL_COND(p_emissive_injection.size() != value_count);

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

	write = reinterpret_cast<float *>(bytes.ptrw());
	for (const Vector4 &value : p_emissive_injection) {
		*write++ = value.x;
		*write++ = value.y;
		*write++ = value.z;
		*write++ = value.w;
	}
	RD::get_singleton()->buffer_update(volume->emissive_injection_buffer, 0, bytes.size(), bytes.ptr());
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

bool LocalLRT::volume_has_gpu_resources(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, false);
	return volume->local_visibility_buffer.is_valid() && volume->local_transfer_buffer.is_valid() &&
			volume->global_visibility_buffers[0].is_valid() && volume->global_visibility_buffers[1].is_valid() &&
			volume->radiance_buffers[0].is_valid() && volume->radiance_buffers[1].is_valid() &&
			volume->injection_buffer.is_valid() && volume->emissive_injection_buffer.is_valid() &&
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
	}
	if (visibility_shader_initialized) {
		visibility_shader->version_free(visibility_shader_version);
		memdelete(visibility_shader);
	}
	if (radiance_shader_initialized) {
		radiance_shader->version_free(radiance_shader_version);
		memdelete(radiance_shader);
	}
}

} // namespace RendererRD
