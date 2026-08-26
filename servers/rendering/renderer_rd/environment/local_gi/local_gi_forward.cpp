/**************************************************************************/
/*  local_gi_forward.cpp                                                  */
/**************************************************************************/
#include "local_gi_forward.h"

#include "servers/rendering/rendering_device.h"

namespace RendererRD {

void LocalGIForward::_ensure_buffers(uint32_t p_probe_count) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	// Keep the Forward+ bindings stable while probe settings are edited in inherited scenes.
	// The prototype benchmark tops out well below this capacity, so no per-edit RID replacement is needed.
	const uint32_t capacity = MAX(p_probe_count, 65536u);
	const uint32_t probe_bytes = capacity * sizeof(float) * 4;
	const uint32_t active_bytes = capacity * sizeof(uint32_t);
	if (buffer_probe_capacity >= capacity && !volume_ubo.is_null() && !irradiance_buffer.is_null() && !moments_buffer.is_null() && !active_buffer.is_null()) {
		return;
	}
	if (irradiance_buffer.is_valid()) {
		rd->free_rid(irradiance_buffer);
	}
	if (moments_buffer.is_valid()) {
		rd->free_rid(moments_buffer);
	}
	if (active_buffer.is_valid()) {
		rd->free_rid(active_buffer);
	}
	if (volume_ubo.is_null()) {
		volume_ubo = rd->uniform_buffer_create(sizeof(VolumeUBO));
		resources_changed = true;
	}
	Vector<uint8_t> zero_probe_data;
	zero_probe_data.resize(probe_bytes);
	Vector<uint8_t> zero_active_data;
	zero_active_data.resize(active_bytes);
	if (irradiance_buffer.is_null()) {
		irradiance_buffer = rd->storage_buffer_create(probe_bytes, zero_probe_data);
		resources_changed = true;
	}
	if (moments_buffer.is_null()) {
		moments_buffer = rd->storage_buffer_create(probe_bytes, zero_probe_data);
		resources_changed = true;
	}
	if (active_buffer.is_null()) {
		active_buffer = rd->storage_buffer_create(active_bytes, zero_active_data);
		resources_changed = true;
	}
	buffer_probe_capacity = capacity;
}

void LocalGIForward::set_volume(const Transform3D &p_world_to_local, const Vector3 &p_size, const Vector3i &p_resolution, float p_spacing, const Vector<Color> &p_irradiance, const Vector<float> &p_distance_mean, const Vector<float> &p_distance_second, const Vector<uint8_t> &p_probe_active) {
	MutexLock lock(mutex);
	pending_active = true;
	pending_world_to_local = p_world_to_local;
	pending_size = p_size;
	pending_resolution = p_resolution;
	pending_spacing = p_spacing;
	pending_irradiance = p_irradiance;
	pending_distance_mean = p_distance_mean;
	pending_distance_second = p_distance_second;
	pending_probe_active = p_probe_active;
	pending_dirty = true;
}

void LocalGIForward::clear_volume() {
	MutexLock lock(mutex);
	pending_active = false;
	pending_dirty = true;
}

void LocalGIForward::update_render_thread() {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	MutexLock lock(mutex);
	if (!pending_dirty) {
		if (volume_ubo.is_null()) {
			_ensure_buffers(1);
		}
		return;
	}

	if (!pending_active) {
		_ensure_buffers(1);
		if (volume_ubo.is_valid()) {
			VolumeUBO ubo;
			rd->buffer_update(volume_ubo, 0, sizeof(VolumeUBO), &ubo);
		}
		render_active = false;
		pending_dirty = false;
		return;
	}

	const uint32_t probe_count = pending_irradiance.size();
	_ensure_buffers(probe_count);
	if (volume_ubo.is_null() || irradiance_buffer.is_null() || moments_buffer.is_null() || active_buffer.is_null()) {
		return;
	}

	VolumeUBO ubo;
	const Basis basis = pending_world_to_local.basis;
	const Vector3 origin = pending_world_to_local.origin;
	for (int row = 0; row < 3; row++) {
		ubo.world_to_local[row][0] = basis[0][row];
		ubo.world_to_local[row][1] = basis[1][row];
		ubo.world_to_local[row][2] = basis[2][row];
		ubo.world_to_local[row][3] = origin[row];
	}
	ubo.volume_data[0] = pending_size.x;
	ubo.volume_data[1] = pending_size.y;
	ubo.volume_data[2] = pending_size.z;
	ubo.volume_data[3] = 1.0f;
	ubo.grid_data[0] = pending_resolution.x;
	ubo.grid_data[1] = pending_resolution.y;
	ubo.grid_data[2] = pending_resolution.z;
	ubo.grid_data[3] = pending_spacing;
	rd->buffer_update(volume_ubo, 0, sizeof(VolumeUBO), &ubo);

	Vector<float> irradiance_data;
	irradiance_data.resize(MAX(probe_count, 1u) * 4);
	for (uint32_t i = 0; i < probe_count; i++) {
		irradiance_data.write[i * 4 + 0] = pending_irradiance[i].r;
		irradiance_data.write[i * 4 + 1] = pending_irradiance[i].g;
		irradiance_data.write[i * 4 + 2] = pending_irradiance[i].b;
		irradiance_data.write[i * 4 + 3] = 1.0f;
	}
	rd->buffer_update(irradiance_buffer, 0, irradiance_data.size() * sizeof(float), irradiance_data.ptr());

	Vector<float> moments_data;
	moments_data.resize(MAX(probe_count, 1u) * 4);
	for (uint32_t i = 0; i < probe_count; i++) {
		moments_data.write[i * 4 + 0] = i < pending_distance_mean.size() ? pending_distance_mean[i] : 0.0f;
		moments_data.write[i * 4 + 1] = i < pending_distance_second.size() ? pending_distance_second[i] : 0.0f;
		moments_data.write[i * 4 + 2] = 0.0f;
		moments_data.write[i * 4 + 3] = 0.0f;
	}
	rd->buffer_update(moments_buffer, 0, moments_data.size() * sizeof(float), moments_data.ptr());

	Vector<uint32_t> active_data;
	active_data.resize(MAX(probe_count, 1u));
	for (uint32_t i = 0; i < probe_count; i++) {
		active_data.write[i] = i < pending_probe_active.size() && pending_probe_active[i] != 0 ? 1u : 0u;
	}
	rd->buffer_update(active_buffer, 0, active_data.size() * sizeof(uint32_t), active_data.ptr());

	render_active = true;
	pending_dirty = false;
}

bool LocalGIForward::consume_resources_changed() {
	const bool changed = resources_changed;
	resources_changed = false;
	return changed;
}

void LocalGIForward::free() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (volume_ubo.is_valid()) {
			rd->free_rid(volume_ubo);
		}
		if (irradiance_buffer.is_valid()) {
			rd->free_rid(irradiance_buffer);
		}
		if (moments_buffer.is_valid()) {
			rd->free_rid(moments_buffer);
		}
		if (active_buffer.is_valid()) {
			rd->free_rid(active_buffer);
		}
	}
	volume_ubo = RID();
	irradiance_buffer = RID();
	moments_buffer = RID();
	active_buffer = RID();
	render_active = false;
}

} // namespace RendererRD
