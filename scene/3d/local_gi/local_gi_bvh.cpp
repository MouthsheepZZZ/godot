/**************************************************************************/
/*  local_gi_bvh.cpp                                                      */
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

#include "local_gi_bvh.h"

#include "core/math/aabb.h"
#include "core/math/geometry_3d.h"
#include "core/templates/sort_array.h"

namespace {

struct LocalGICentroidCompare {
	const LocalGITriangle *triangles = nullptr;
	uint32_t axis = 0;

	bool operator()(int32_t p_a, int32_t p_b) const {
		const real_t ca = triangles[p_a].v0[axis] + triangles[p_a].v1[axis] + triangles[p_a].v2[axis];
		const real_t cb = triangles[p_b].v0[axis] + triangles[p_b].v1[axis] + triangles[p_b].v2[axis];
		if (ca == cb) {
			return p_a < p_b;
		}
		return ca < cb;
	}
};

AABB _triangle_aabb(const LocalGITriangle &p_triangle) {
	AABB aabb(p_triangle.v0, Vector3());
	aabb.expand_to(p_triangle.v1);
	aabb.expand_to(p_triangle.v2);
	return aabb;
}

} // namespace

void LocalGIBVH::clear() {
	triangles.clear();
	nodes.clear();
}

int32_t LocalGIBVH::_build_node(Vector<int32_t> &p_indices, int32_t p_start, int32_t p_count) {
	DEV_ASSERT(p_count > 0);

	const int32_t node_index = nodes.size();
	nodes.push_back(LocalGIBVHNode());

	AABB bounds = _triangle_aabb(triangles[p_indices[p_start]]);
	for (int32_t i = 1; i < p_count; i++) {
		bounds.merge_with(_triangle_aabb(triangles[p_indices[p_start + i]]));
	}
	bounds.grow_by(CMP_EPSILON);

	LocalGIBVHNode node;
	node.bounds_min = bounds.position;
	node.bounds_max = bounds.position + bounds.size;

	if (p_count == 1) {
		node.first_triangle = p_indices[p_start];
		node.triangle_count = 1;
		nodes.write[node_index] = node;
		return node_index;
	}

	const int32_t axis = bounds.get_longest_axis_index();
	SortArray<int32_t, LocalGICentroidCompare> sorter;
	sorter.compare.triangles = triangles.ptr();
	sorter.compare.axis = (uint32_t)axis;
	sorter.sort(p_indices.ptrw() + p_start, p_count);

	const int32_t mid = p_count / 2;
	node.left = _build_node(p_indices, p_start, mid);
	node.right = _build_node(p_indices, p_start + mid, p_count - mid);
	nodes.write[node_index] = node;
	return node_index;
}

void LocalGIBVH::build(const Vector<LocalGITriangle> &p_triangles) {
	clear();
	triangles = p_triangles;
	if (triangles.is_empty()) {
		return;
	}

	Vector<int32_t> indices;
	indices.resize(triangles.size());
	for (int32_t i = 0; i < triangles.size(); i++) {
		indices.write[i] = i;
	}
	_build_node(indices, 0, triangles.size());
}

bool LocalGIBVH::intersect_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) const {
	r_hit = LocalGIRayHit();
	if (nodes.is_empty()) {
		return false;
	}

	const real_t dir_len_sq = p_direction.length_squared();
	if (dir_len_sq < (real_t)CMP_EPSILON2) {
		return false;
	}
	const Vector3 dir = p_direction / Math::sqrt(dir_len_sq);

	real_t closest_t = Math::INF;
	int32_t closest_tri = -1;
	Vector3 closest_point;

	Vector<int32_t> stack;
	stack.push_back(0);

	while (!stack.is_empty()) {
		const int32_t node_index = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		const LocalGIBVHNode &node = nodes[node_index];
		const AABB aabb(node.bounds_min, node.bounds_max - node.bounds_min);
		if (!aabb.intersects_ray(p_origin, dir)) {
			continue;
		}

		if (node.is_leaf()) {
			for (int32_t i = 0; i < node.triangle_count; i++) {
				const int32_t tri_index = node.first_triangle + i;
				const LocalGITriangle &tri = triangles[tri_index];
				Vector3 point;
				if (!Geometry3D::ray_intersects_triangle(p_origin, dir, tri.v0, tri.v1, tri.v2, &point)) {
					continue;
				}
				const real_t t = p_origin.distance_to(point);
				if (t < closest_t) {
					closest_t = t;
					closest_tri = tri_index;
					closest_point = point;
				}
			}
			continue;
		}

		if (node.right >= 0) {
			stack.push_back(node.right);
		}
		if (node.left >= 0) {
			stack.push_back(node.left);
		}
	}

	if (closest_tri < 0) {
		return false;
	}

	r_hit.hit = true;
	r_hit.distance = closest_t;
	r_hit.position = closest_point;
	r_hit.normal = triangles[closest_tri].normal;
	r_hit.albedo = triangles[closest_tri].albedo;
	r_hit.triangle_index = triangles[closest_tri].index;
	return true;
}
