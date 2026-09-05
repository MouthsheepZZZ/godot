/**************************************************************************/
/*  local_lrt_color_sdf.h                                                 */
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

#include "core/math/aabb.h"
#include "core/math/color.h"
#include "core/math/face3.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/templates/vector.h"
#include "scene/resources/mesh.h"

class LocalLRTColorSDF {
public:
	struct Sample {
		real_t signed_distance = 1.0e20;
		real_t coverage = 0.0;
		Color albedo;
		Color emission;
		Color transfer_emission;
		Vector3 normal;
	};

	static real_t signed_distance_box(const Vector3 &p_local_position, const Vector3 &p_half_extents);
	static real_t signed_distance_sphere(const Vector3 &p_local_position, real_t p_radius);
	static Vector3 normal_box(const Vector3 &p_local_position, const Vector3 &p_half_extents);
	static Vector3 normal_sphere(const Vector3 &p_local_position);
	static real_t coverage_from_distance(real_t p_signed_distance, real_t p_voxel_size);

	static LocalLRTColorSDF make_box(const Vector3 &p_half_extents, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission = Color(), const Color &p_transfer_emission = Color());
	static LocalLRTColorSDF make_sphere(real_t p_radius, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission = Color(), const Color &p_transfer_emission = Color());
	static LocalLRTColorSDF from_triangles(const Vector<Vector3> &p_vertices, const Vector<int> &p_indices, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission = Color(), const Color &p_transfer_emission = Color());
	static LocalLRTColorSDF from_mesh(const Ref<Mesh> &p_mesh, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission = Color(), const Color &p_transfer_emission = Color());
	static LocalLRTColorSDF from_mesh_surface(const Ref<Mesh> &p_mesh, int p_surface, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission = Color(), const Color &p_transfer_emission = Color());

	Sample sample(const Vector3 &p_object_local) const;

	bool is_empty() const { return type == TYPE_EMPTY; }
	bool is_analytic() const { return type == TYPE_BOX || type == TYPE_SPHERE; }
	real_t get_voxel_size() const { return voxel_size; }
	Vector3 get_actual_voxel_size() const { return actual_voxel_size; }
	Vector3i get_resolution() const { return resolution; }
	AABB get_bounds() const { return bounds; }
	Color get_albedo() const { return albedo; }
	Color get_emission() const { return emission; }
	Color get_transfer_emission() const { return transfer_emission; }
	Vector3 get_voxel_local_position(const Vector3i &p_position) const;

private:
	enum Type {
		TYPE_EMPTY,
		TYPE_BOX,
		TYPE_SPHERE,
		TYPE_VOXEL,
	};

	Type type = TYPE_EMPTY;
	real_t voxel_size = 0.25;
	Vector3 actual_voxel_size;
	Vector3i resolution;
	AABB bounds;
	Vector3 box_half_extents;
	real_t sphere_radius = 0.0;
	Color albedo;
	Color emission;
	Color transfer_emission;
	Vector<real_t> distances;

	static Vector<Face3> _faces_from_triangles(const Vector<Vector3> &p_vertices, const Vector<int> &p_indices);
	static Vector<Face3> _faces_from_mesh(const Ref<Mesh> &p_mesh, int p_surface = -1);
	static real_t _closest_signed_distance(const Vector3 &p_position, const Vector<Face3> &p_faces);
	void _build_voxel_field(const Vector<Face3> &p_faces);
	int _voxel_index(const Vector3i &p_position) const;
	real_t _distance_at_voxel(const Vector3i &p_position) const;
	real_t _interpolate_distance(const Vector3 &p_object_local) const;
	Vector3 _gradient_normal(const Vector3 &p_object_local) const;
	Sample _sample_material(real_t p_signed_distance, const Vector3 &p_normal) const;
};
