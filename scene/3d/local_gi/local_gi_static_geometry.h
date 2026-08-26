/**************************************************************************/
/*  local_gi_static_geometry.h                                            */
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
#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "scene/3d/local_gi/local_gi_bvh.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/material.h"

class Mesh;
class MeshInstance3D;
class Node;
class Node3D;

struct LocalGIContributorKey {
	uint64_t instance_id = 0;
	uint64_t mesh_id = 0;
	int32_t surface_count = 0;
	int32_t extra_index = 0;
	AABB mesh_aabb;
	Transform3D local_xform;

	bool is_equal_approx(const LocalGIContributorKey &p_other) const {
		return instance_id == p_other.instance_id &&
				mesh_id == p_other.mesh_id &&
				surface_count == p_other.surface_count &&
				extra_index == p_other.extra_index &&
				mesh_aabb.position.is_equal_approx(p_other.mesh_aabb.position) &&
				mesh_aabb.size.is_equal_approx(p_other.mesh_aabb.size) &&
				local_xform.is_equal_approx(p_other.local_xform);
	}
};

class LocalGIStaticGeometry {
public:
	static Transform3D get_composed_transform(const Node3D *p_node);
	static Transform3D get_relative_transform(const Node3D *p_node, const Node3D *p_reference);
	static Color albedo_from_material(const Ref<Material> &p_material);
	static void extract_mesh_triangles(const Ref<Mesh> &p_mesh, const Transform3D &p_local_xform, const AABB &p_volume_bounds, Vector<LocalGITriangle> &r_triangles, MeshInstance3D *p_instance = nullptr);
	static void collect(Node *p_from_node, const Node3D *p_volume, const AABB &p_local_bounds, Vector<LocalGITriangle> &r_triangles);
	static void collect(Node *p_from_node, const Node3D *p_volume, const AABB &p_local_bounds, Vector<LocalGITriangle> *r_triangles, Vector<LocalGIContributorKey> *r_keys, GeometryInstance3D::GIMode p_mode);
	static bool keys_equal(const Vector<LocalGIContributorKey> &p_a, const Vector<LocalGIContributorKey> &p_b);
};
