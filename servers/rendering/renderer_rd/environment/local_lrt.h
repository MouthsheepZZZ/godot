/**************************************************************************/
/*  local_lrt.h                                                           */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/templates/rid_owner.h"

namespace RendererRD {

class LocalLRT {
	struct Volume {
		bool enabled = true;
		Vector3 size = Vector3(10.0, 10.0, 10.0);
		Vector3i resolution = Vector3i(11, 11, 11);
		Transform3D transform;
		int propagation_iterations = 4;
		float energy = 1.0;
		float edge_blend_distance = 1.0;
	};

	mutable RID_Owner<Volume, true> volume_owner;

public:
	RID volume_allocate();
	void volume_initialize(RID p_volume);
	void volume_free(RID p_volume);
	bool owns_volume(RID p_volume) const;

	void volume_set_enabled(RID p_volume, bool p_enabled);
	void volume_set_grid(RID p_volume, const Vector3 &p_size, const Vector3i &p_resolution);
	void volume_set_transform(RID p_volume, const Transform3D &p_transform);
	void volume_set_propagation_iterations(RID p_volume, int p_iterations);
	void volume_set_energy(RID p_volume, float p_energy);
	void volume_set_edge_blend_distance(RID p_volume, float p_distance);

	AABB volume_get_bounds(RID p_volume) const;
};

} // namespace RendererRD
