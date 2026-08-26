/**************************************************************************/
/*  local_gi_static_geometry.cpp                                          */
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

#include "local_gi_static_geometry.h"

#include "core/math/face3.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/mesh.h"

namespace {

bool _is_mode_contributor(Node3D *p_node, GeometryInstance3D::GIMode p_mode) {
	// This Godot branch's Node3D::is_visible_in_tree() only walks visibility, not is_inside_tree().
	if (!p_node->is_visible_in_tree()) {
		return false;
	}

	GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node);
	if (geometry != nullptr && geometry->get_gi_mode() != p_mode) {
		return false;
	}
	return true;
}

void _append_key(const Node3D *p_node, const Ref<Mesh> &p_mesh, const Transform3D &p_local_xform, int32_t p_extra_index, Vector<LocalGIContributorKey> *r_keys) {
	if (r_keys == nullptr || p_mesh.is_null()) {
		return;
	}

	LocalGIContributorKey key;
	key.instance_id = p_node->get_instance_id();
	key.mesh_id = p_mesh->get_rid().get_id();
	key.surface_count = p_mesh->get_surface_count();
	key.extra_index = p_extra_index;
	key.mesh_aabb = p_mesh->get_aabb();
	key.local_xform = p_local_xform;
	r_keys->push_back(key);
}

void _collect_node(Node *p_at_node, const Transform3D &p_volume_global, const AABB &p_local_bounds, Vector<LocalGITriangle> *r_triangles, Vector<LocalGIContributorKey> *r_keys, GeometryInstance3D::GIMode p_mode) {
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_at_node);
	if (mesh_instance && _is_mode_contributor(mesh_instance, p_mode)) {
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_valid()) {
			const Transform3D local_xform = p_volume_global.affine_inverse() * LocalGIStaticGeometry::get_composed_transform(mesh_instance);
			if (p_local_bounds.intersects(local_xform.xform(mesh->get_aabb()))) {
				_append_key(mesh_instance, mesh, local_xform, 0, r_keys);
				if (r_triangles) {
					LocalGIStaticGeometry::extract_mesh_triangles(mesh, local_xform, p_local_bounds, *r_triangles);
				}
			}
		}
	}

	Node3D *node_3d = Object::cast_to<Node3D>(p_at_node);
	if (node_3d && _is_mode_contributor(node_3d, p_mode) && mesh_instance == nullptr) {
		Array meshes;
		MultiMeshInstance3D *multi_mesh = Object::cast_to<MultiMeshInstance3D>(p_at_node);
		if (multi_mesh) {
			meshes = multi_mesh->get_meshes();
		} else if (p_at_node->has_method("get_meshes")) {
			meshes = p_at_node->call("get_meshes");
		}

		for (int i = 0; i + 1 < meshes.size(); i += 2) {
			const Transform3D mesh_xform = meshes[i];
			const Ref<Mesh> mesh = meshes[i + 1];
			if (mesh.is_null()) {
				continue;
			}

			const Transform3D local_xform = p_volume_global.affine_inverse() * (LocalGIStaticGeometry::get_composed_transform(node_3d) * mesh_xform);
			if (p_local_bounds.intersects(local_xform.xform(mesh->get_aabb()))) {
				_append_key(node_3d, mesh, local_xform, i, r_keys);
				if (r_triangles) {
					LocalGIStaticGeometry::extract_mesh_triangles(mesh, local_xform, p_local_bounds, *r_triangles);
				}
			}
		}
	}

	for (int i = 0; i < p_at_node->get_child_count(); i++) {
		_collect_node(p_at_node->get_child(i), p_volume_global, p_local_bounds, r_triangles, r_keys, p_mode);
	}
}

} // namespace

Transform3D LocalGIStaticGeometry::get_composed_transform(const Node3D *p_node) {
	ERR_FAIL_NULL_V(p_node, Transform3D());
	if (p_node->is_inside_tree()) {
		return p_node->get_global_transform();
	}

	Transform3D xform = p_node->get_transform();
	const Node *current = p_node->get_parent();
	while (current) {
		const Node3D *parent_3d = Object::cast_to<Node3D>(current);
		if (parent_3d) {
			if (parent_3d->is_inside_tree()) {
				return parent_3d->get_global_transform() * xform;
			}
			xform = parent_3d->get_transform() * xform;
		}
		current = current->get_parent();
	}
	return xform;
}

void LocalGIStaticGeometry::extract_mesh_triangles(const Ref<Mesh> &p_mesh, const Transform3D &p_local_xform, const AABB &p_volume_bounds, Vector<LocalGITriangle> &r_triangles) {
	ERR_FAIL_COND(p_mesh.is_null());

	const Vector<Face3> faces = p_mesh->get_faces();
	for (int i = 0; i < faces.size(); i++) {
		const Vector3 v0 = p_local_xform.xform(faces[i].vertex[0]);
		const Vector3 v1 = p_local_xform.xform(faces[i].vertex[1]);
		const Vector3 v2 = p_local_xform.xform(faces[i].vertex[2]);
		const Vector3 edge1 = v1 - v0;
		const Vector3 edge2 = v2 - v0;
		Vector3 normal = edge1.cross(edge2);
		const real_t area_sq = normal.length_squared();
		if (area_sq < (real_t)CMP_EPSILON2) {
			continue;
		}
		normal /= Math::sqrt(area_sq);

		AABB triangle_aabb(v0, Vector3());
		triangle_aabb.expand_to(v1);
		triangle_aabb.expand_to(v2);
		if (!p_volume_bounds.intersects(triangle_aabb)) {
			continue;
		}

		LocalGITriangle triangle;
		triangle.v0 = v0;
		triangle.v1 = v1;
		triangle.v2 = v2;
		triangle.normal = normal;
		triangle.index = r_triangles.size();
		r_triangles.push_back(triangle);
	}
}

void LocalGIStaticGeometry::collect(Node *p_from_node, const Transform3D &p_volume_global, const AABB &p_local_bounds, Vector<LocalGITriangle> &r_triangles) {
	collect(p_from_node, p_volume_global, p_local_bounds, &r_triangles, nullptr, GeometryInstance3D::GI_MODE_STATIC);
}

void LocalGIStaticGeometry::collect(Node *p_from_node, const Transform3D &p_volume_global, const AABB &p_local_bounds, Vector<LocalGITriangle> *r_triangles, Vector<LocalGIContributorKey> *r_keys, GeometryInstance3D::GIMode p_mode) {
	ERR_FAIL_NULL(p_from_node);
	_collect_node(p_from_node, p_volume_global, p_local_bounds, r_triangles, r_keys, p_mode);
}

bool LocalGIStaticGeometry::keys_equal(const Vector<LocalGIContributorKey> &p_a, const Vector<LocalGIContributorKey> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (int i = 0; i < p_a.size(); i++) {
		if (!p_a[i].is_equal_approx(p_b[i])) {
			return false;
		}
	}
	return true;
}
