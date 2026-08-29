/**************************************************************************/
/*  renderer_gi.h                                                         */
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

#pragma once

#include <cstdint>

class RID;
struct AABB;
struct Transform3D;
struct Vector3;
struct Vector3i;
struct Vector4;

template <typename T>
class Vector;

class RendererGI {
public:
	virtual ~RendererGI() {}

	/* LOCAL LRT VOLUME API */

	virtual RID local_lrt_volume_allocate() = 0;
	virtual void local_lrt_volume_free(RID p_rid) = 0;
	virtual void local_lrt_volume_initialize(RID p_rid) = 0;
	virtual void local_lrt_volume_set_enabled(RID p_volume, bool p_enabled) = 0;
	virtual void local_lrt_volume_set_grid(RID p_volume, const Vector3 &p_size, const Vector3i &p_resolution) = 0;
	virtual void local_lrt_volume_set_transform(RID p_volume, const Transform3D &p_transform) = 0;
	virtual void local_lrt_volume_set_visibility_iterations(RID p_volume, int p_iterations) = 0;
	virtual void local_lrt_volume_set_propagation_iterations(RID p_volume, int p_iterations) = 0;
	virtual void local_lrt_volume_set_energy(RID p_volume, float p_energy) = 0;
	virtual void local_lrt_volume_set_edge_blend_distance(RID p_volume, float p_distance) = 0;
	virtual void local_lrt_volume_set_static_data(RID p_volume, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer) = 0;
	virtual void local_lrt_volume_set_inside_solid(RID p_volume, const Vector<int> &p_inside_solid) = 0;
	virtual void local_lrt_volume_set_injection(RID p_volume, const Vector<Vector4> &p_injection, const Vector<Vector4> &p_emissive_injection) = 0;
	virtual void local_lrt_volume_propagate_radiance(RID p_volume) = 0;
	virtual AABB local_lrt_volume_get_bounds(RID p_volume) const = 0;
	virtual Vector<Vector4> local_lrt_volume_get_global_visibility(RID p_volume) const = 0;
	virtual Vector<Vector4> local_lrt_volume_get_injection(RID p_volume) const = 0;
	virtual Vector<Vector4> local_lrt_volume_get_radiance(RID p_volume) const = 0;

	/* VOXEL GI API */

	virtual RID voxel_gi_allocate() = 0;
	virtual void voxel_gi_free(RID p_rid) = 0;
	virtual void voxel_gi_initialize(RID p_rid) = 0;

	virtual void voxel_gi_allocate_data(RID p_voxel_gi, const Transform3D &p_to_cell_xform, const AABB &p_aabb, const Vector3i &p_octree_size, const Vector<uint8_t> &p_octree_cells, const Vector<uint8_t> &p_data_cells, const Vector<uint8_t> &p_distance_field, const Vector<int> &p_level_counts) = 0;

	virtual AABB voxel_gi_get_bounds(RID p_voxel_gi) const = 0;
	virtual Vector3i voxel_gi_get_octree_size(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_octree_cells(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_data_cells(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_distance_field(RID p_voxel_gi) const = 0;

	virtual Vector<int> voxel_gi_get_level_counts(RID p_voxel_gi) const = 0;
	virtual Transform3D voxel_gi_get_to_cell_xform(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_dynamic_range(RID p_voxel_gi, float p_range) = 0;
	virtual float voxel_gi_get_dynamic_range(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_propagation(RID p_voxel_gi, float p_range) = 0;
	virtual float voxel_gi_get_propagation(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_energy(RID p_voxel_gi, float p_energy) = 0;
	virtual float voxel_gi_get_energy(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_baked_exposure_normalization(RID p_voxel_gi, float p_baked_exposure) = 0;
	virtual float voxel_gi_get_baked_exposure_normalization(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_bias(RID p_voxel_gi, float p_bias) = 0;
	virtual float voxel_gi_get_bias(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_normal_bias(RID p_voxel_gi, float p_range) = 0;
	virtual float voxel_gi_get_normal_bias(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_interior(RID p_voxel_gi, bool p_enable) = 0;
	virtual bool voxel_gi_is_interior(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_use_two_bounces(RID p_voxel_gi, bool p_enable) = 0;
	virtual bool voxel_gi_is_using_two_bounces(RID p_voxel_gi) const = 0;

	virtual uint32_t voxel_gi_get_version(RID p_probe) const = 0;

	virtual void hddagi_reset() = 0;
};
