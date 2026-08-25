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

template <typename T>
class Vector;

class RendererGI {
public:
	virtual ~RendererGI() {}

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

	/* LOCAL DYNAMIC GI API */

	RID local_dynamic_gi_allocate();
	void local_dynamic_gi_free(RID p_rid);
	void local_dynamic_gi_initialize(RID p_rid);
	bool owns_local_dynamic_gi(RID p_rid) const;

	void local_dynamic_gi_set_enabled(RID p_local_dynamic_gi, bool p_enabled);
	bool local_dynamic_gi_is_enabled(RID p_local_dynamic_gi) const;

	void local_dynamic_gi_set_extend(RID p_local_dynamic_gi, const Vector3 &p_extend);
	Vector3 local_dynamic_gi_get_extend(RID p_local_dynamic_gi) const;

	void local_dynamic_gi_set_blend_distance(RID p_local_dynamic_gi, float p_blend_distance);
	float local_dynamic_gi_get_blend_distance(RID p_local_dynamic_gi) const;

	void local_dynamic_gi_set_local_bounds(RID p_local_dynamic_gi, const AABB &p_bounds);
	AABB local_dynamic_gi_get_local_bounds(RID p_local_dynamic_gi) const;

	void local_dynamic_gi_set_transform(RID p_local_dynamic_gi, const Transform3D &p_transform);
	Transform3D local_dynamic_gi_get_transform(RID p_local_dynamic_gi) const;

	void local_dynamic_gi_set_scenario(RID p_local_dynamic_gi, RID p_scenario);
	RID local_dynamic_gi_get_scenario(RID p_local_dynamic_gi) const;

	void local_dynamic_gi_set_geometry_instances(RID p_local_dynamic_gi, const Vector<RID> &p_instances);
	Vector<RID> local_dynamic_gi_get_geometry_instances(RID p_local_dynamic_gi) const;

	void local_dynamic_gi_set_light_instances(RID p_local_dynamic_gi, const Vector<RID> &p_instances);
	Vector<RID> local_dynamic_gi_get_light_instances(RID p_local_dynamic_gi) const;

	uint32_t local_dynamic_gi_get_data_version(RID p_local_dynamic_gi) const;
	uint32_t local_dynamic_gi_get_voxelization_count(RID p_local_dynamic_gi) const;
	void local_dynamic_gi_increment_voxelization_count(RID p_local_dynamic_gi);

	Vector<RID> local_dynamic_gi_get_registered(RID p_scenario) const;
};
