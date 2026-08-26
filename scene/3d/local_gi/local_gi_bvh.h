/**************************************************************************/
/*  local_gi_bvh.h                                                        */
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

#include "core/math/vector3.h"
#include "core/templates/vector.h"

// CPU triangle BVH stored in LocalGIVolume local space.
// Node layout stays explicit so Phase 3 can upload the same arrays to the GPU.
struct LocalGITriangle {
	Vector3 v0;
	Vector3 v1;
	Vector3 v2;
	Vector3 normal;
	int32_t index = -1;
};

struct LocalGIBVHNode {
	Vector3 bounds_min;
	Vector3 bounds_max;
	int32_t left = -1;
	int32_t right = -1;
	int32_t first_triangle = -1;
	int32_t triangle_count = 0;

	bool is_leaf() const { return triangle_count > 0; }
};

struct LocalGIRayHit {
	bool hit = false;
	real_t distance = 0.0;
	Vector3 position;
	Vector3 normal;
	int32_t triangle_index = -1;
};

class LocalGIBVH {
	Vector<LocalGITriangle> triangles;
	Vector<LocalGIBVHNode> nodes;

	int32_t _build_node(Vector<int32_t> &p_indices, int32_t p_start, int32_t p_count);

public:
	void clear();
	void build(const Vector<LocalGITriangle> &p_triangles);
	bool intersect_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) const;

	bool is_empty() const { return nodes.is_empty(); }
	const Vector<LocalGITriangle> &get_triangles() const { return triangles; }
	const Vector<LocalGIBVHNode> &get_nodes() const { return nodes; }
};
