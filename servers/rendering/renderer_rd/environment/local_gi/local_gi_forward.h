/**************************************************************************/
/*  local_gi_forward.h                                                    */
/**************************************************************************/
#pragma once

#include "core/os/mutex.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/math/color.h"

namespace RendererRD {

class LocalGIForward {
public:
	struct VolumeUBO {
		float world_to_local[3][4] = {};
		float volume_data[4] = {};
		float grid_data[4] = {};
	};

private:
	Mutex mutex;
	bool pending_active = false;
	Transform3D pending_world_to_local;
	Vector3 pending_size;
	Vector3i pending_resolution;
	float pending_spacing = 0.5f;
	Vector<Color> pending_irradiance;
	Vector<float> pending_distance_mean;
	Vector<float> pending_distance_second;
	Vector<uint8_t> pending_probe_active;
	bool pending_dirty = false;

	bool render_active = false;
	RID volume_ubo;
	RID irradiance_buffer;
	RID moments_buffer;
	RID active_buffer;
	uint32_t buffer_probe_capacity = 0;
	bool resources_changed = false;

	void _ensure_buffers(uint32_t p_probe_count);

public:
	void set_volume(const Transform3D &p_world_to_local, const Vector3 &p_size, const Vector3i &p_resolution, float p_spacing, const Vector<Color> &p_irradiance, const Vector<float> &p_distance_mean, const Vector<float> &p_distance_second, const Vector<uint8_t> &p_probe_active);
	void clear_volume();
	void update_render_thread();
	bool consume_resources_changed();
	void free();

	bool is_active() const { return render_active; }
	RID get_volume_ubo() const { return volume_ubo; }
	RID get_irradiance_buffer() const { return irradiance_buffer; }
	RID get_moments_buffer() const { return moments_buffer; }
	RID get_active_buffer() const { return active_buffer; }
};

} // namespace RendererRD
