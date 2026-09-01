/**************************************************************************/
/*  gi.cpp                                                                */
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

#ifdef GLES3_ENABLED

#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"

using namespace GLES3;

/* LOCAL LRT VOLUME API */

RID GI::local_lrt_volume_allocate() {
	return RID();
}

void GI::local_lrt_volume_free(RID p_rid) {
}

void GI::local_lrt_volume_initialize(RID p_rid) {
}

void GI::local_lrt_volume_set_enabled(RID p_volume, bool p_enabled) {
}

void GI::local_lrt_volume_set_grid(RID p_volume, const Vector3 &p_size, const Vector3i &p_resolution) {
}

void GI::local_lrt_volume_set_transform(RID p_volume, const Transform3D &p_transform) {
}

void GI::local_lrt_volume_set_visibility_iterations(RID p_volume, int p_iterations) {
}

void GI::local_lrt_volume_set_propagation_iterations(RID p_volume, int p_iterations) {
}

void GI::local_lrt_volume_set_energy(RID p_volume, float p_energy) {
}

void GI::local_lrt_volume_set_priority(RID p_volume, int p_priority) {
}

void GI::local_lrt_volume_set_edge_blend_distance(RID p_volume, float p_distance) {
}

void GI::local_lrt_volume_set_static_data(RID p_volume, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer, const Vector<Vector4> &p_mesh_light) {
}

void GI::local_lrt_volume_update_static_data(RID p_volume, const Vector3i &p_begin, const Vector3i &p_size, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer, const Vector<Vector4> &p_mesh_light, const Vector<int> &p_inside_solid) {
}

void GI::local_lrt_volume_set_inside_solid(RID p_volume, const Vector<int> &p_inside_solid) {
}

void GI::local_lrt_volume_set_injection(RID p_volume, const Vector<Vector4> &p_injection) {
}

void GI::local_lrt_volume_inject_analytic_lights(RID p_volume, const Vector<Vector4> &p_lights) {
}

void GI::local_lrt_volume_set_directional_shadow(RID p_volume, const Vector<float> &p_depths, int p_size, const Transform3D &p_camera, const Projection &p_projection, float p_bias) {
}

Vector<float> GI::local_lrt_volume_get_shadow_visibility(RID p_volume) const {
	return Vector<float>();
}

void GI::local_lrt_volume_propagate_visibility(RID p_volume) {
}

void GI::local_lrt_volume_propagate_radiance(RID p_volume) {
}

AABB GI::local_lrt_volume_get_bounds(RID p_volume) const {
	return AABB();
}

Vector<Vector4> GI::local_lrt_volume_get_global_visibility(RID p_volume) const {
	return Vector<Vector4>();
}

Vector<Vector4> GI::local_lrt_volume_get_injection(RID p_volume) const {
	return Vector<Vector4>();
}

Vector<Vector4> GI::local_lrt_volume_get_environment_injection(RID p_volume) const {
	return Vector<Vector4>();
}

Vector<Vector4> GI::local_lrt_volume_get_radiance(RID p_volume) const {
	return Vector<Vector4>();
}

/* VOXEL GI API */

RID GI::voxel_gi_allocate() {
	return RID();
}

void GI::voxel_gi_free(RID p_rid) {
}

void GI::voxel_gi_initialize(RID p_rid) {
}

void GI::voxel_gi_allocate_data(RID p_voxel_gi, const Transform3D &p_to_cell_xform, const AABB &p_aabb, const Vector3i &p_octree_size, const Vector<uint8_t> &p_octree_cells, const Vector<uint8_t> &p_data_cells, const Vector<uint8_t> &p_distance_field, const Vector<int> &p_level_counts) {
}

AABB GI::voxel_gi_get_bounds(RID p_voxel_gi) const {
	return AABB();
}

Vector3i GI::voxel_gi_get_octree_size(RID p_voxel_gi) const {
	return Vector3i();
}

Vector<uint8_t> GI::voxel_gi_get_octree_cells(RID p_voxel_gi) const {
	return Vector<uint8_t>();
}

Vector<uint8_t> GI::voxel_gi_get_data_cells(RID p_voxel_gi) const {
	return Vector<uint8_t>();
}

Vector<uint8_t> GI::voxel_gi_get_distance_field(RID p_voxel_gi) const {
	return Vector<uint8_t>();
}

Vector<int> GI::voxel_gi_get_level_counts(RID p_voxel_gi) const {
	return Vector<int>();
}

Transform3D GI::voxel_gi_get_to_cell_xform(RID p_voxel_gi) const {
	return Transform3D();
}

void GI::voxel_gi_set_dynamic_range(RID p_voxel_gi, float p_range) {
}

float GI::voxel_gi_get_dynamic_range(RID p_voxel_gi) const {
	return 0;
}

void GI::voxel_gi_set_propagation(RID p_voxel_gi, float p_range) {
}

float GI::voxel_gi_get_propagation(RID p_voxel_gi) const {
	return 0;
}

void GI::voxel_gi_set_energy(RID p_voxel_gi, float p_range) {
}

float GI::voxel_gi_get_energy(RID p_voxel_gi) const {
	return 0.0;
}

void GI::voxel_gi_set_baked_exposure_normalization(RID p_voxel_gi, float p_baked_exposure) {
}

float GI::voxel_gi_get_baked_exposure_normalization(RID p_voxel_gi) const {
	return 1.0;
}

void GI::voxel_gi_set_bias(RID p_voxel_gi, float p_range) {
}

float GI::voxel_gi_get_bias(RID p_voxel_gi) const {
	return 0.0;
}

void GI::voxel_gi_set_normal_bias(RID p_voxel_gi, float p_range) {
}

float GI::voxel_gi_get_normal_bias(RID p_voxel_gi) const {
	return 0.0;
}

void GI::voxel_gi_set_interior(RID p_voxel_gi, bool p_enable) {
}

bool GI::voxel_gi_is_interior(RID p_voxel_gi) const {
	return false;
}

void GI::voxel_gi_set_use_two_bounces(RID p_voxel_gi, bool p_enable) {
}

bool GI::voxel_gi_is_using_two_bounces(RID p_voxel_gi) const {
	return false;
}

uint32_t GI::voxel_gi_get_version(RID p_voxel_gi) const {
	return 0;
}

void GI::hddagi_reset() {
}

#endif // GLES3_ENABLED
