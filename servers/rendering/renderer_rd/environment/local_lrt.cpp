/**************************************************************************/
/*  local_lrt.cpp                                                         */
/**************************************************************************/

#include "local_lrt.h"

namespace RendererRD {

RID LocalLRT::volume_allocate() {
	return volume_owner.allocate_rid();
}

void LocalLRT::volume_initialize(RID p_volume) {
	volume_owner.initialize_rid(p_volume, Volume());
}

void LocalLRT::volume_free(RID p_volume) {
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

AABB LocalLRT::volume_get_bounds(RID p_volume) const {
	const Volume *volume = volume_owner.get_or_null(p_volume);
	ERR_FAIL_NULL_V(volume, AABB());
	return AABB(-volume->size * 0.5, volume->size);
}

} // namespace RendererRD
