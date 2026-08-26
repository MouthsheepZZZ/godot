/**************************************************************************/
/*  test_local_gi_static_bvh.cpp                                          */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_local_gi_static_bvh)

#ifndef _3D_DISABLED

#include "scene/3d/local_gi/local_gi_static_geometry.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"

namespace TestLocalGIStaticBVH {

static LocalGITriangle make_triangle(const Vector3 &p_v0, const Vector3 &p_v1, const Vector3 &p_v2, int32_t p_index) {
	LocalGITriangle triangle;
	triangle.v0 = p_v0;
	triangle.v1 = p_v1;
	triangle.v2 = p_v2;
	triangle.normal = (p_v1 - p_v0).cross(p_v2 - p_v0).normalized();
	triangle.index = p_index;
	return triangle;
}

static Vector<LocalGITriangle> make_box_triangles(const Vector3 &p_size, const Transform3D &p_xform = Transform3D()) {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(p_size);
	Vector<LocalGITriangle> triangles;
	LocalGIStaticGeometry::extract_mesh_triangles(box, p_xform, AABB(Vector3(-50, -50, -50), Vector3(100, 100, 100)), triangles);
	return triangles;
}

static AABB triangles_aabb(const Vector<LocalGITriangle> &p_triangles) {
	AABB aabb;
	for (int i = 0; i < p_triangles.size(); i++) {
		if (i == 0) {
			aabb = AABB(p_triangles[i].v0, Vector3());
		}
		aabb.expand_to(p_triangles[i].v0);
		aabb.expand_to(p_triangles[i].v1);
		aabb.expand_to(p_triangles[i].v2);
	}
	return aabb;
}

static MeshInstance3D *make_box_instance(Node *p_parent, const Vector3 &p_position, const Vector3 &p_size, GeometryInstance3D::GIMode p_mode = GeometryInstance3D::GI_MODE_STATIC) {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(p_size);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_position(p_position);
	instance->set_gi_mode(p_mode);
	p_parent->add_child(instance);
	return instance;
}

static bool bvh_nodes_equal(const LocalGIBVH &p_a, const LocalGIBVH &p_b) {
	if (p_a.get_nodes().size() != p_b.get_nodes().size()) {
		return false;
	}
	if (p_a.get_triangles().size() != p_b.get_triangles().size()) {
		return false;
	}
	for (int i = 0; i < p_a.get_nodes().size(); i++) {
		const LocalGIBVHNode &na = p_a.get_nodes()[i];
		const LocalGIBVHNode &nb = p_b.get_nodes()[i];
		if (!na.bounds_min.is_equal_approx(nb.bounds_min) || !na.bounds_max.is_equal_approx(nb.bounds_max)) {
			return false;
		}
		if (na.left != nb.left || na.right != nb.right || na.first_triangle != nb.first_triangle || na.triangle_count != nb.triangle_count) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Triangle extraction and local-space transform") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(4, 4, 4));
	volume->set_position(Vector3(2, 0, 0));
	root->add_child(volume);

	make_box_instance(root, Vector3(2, 0, 0), Vector3(1, 1, 1));
	MeshInstance3D *dynamic_box = make_box_instance(root, Vector3(2, 1.2, 0), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DYNAMIC);
	MeshInstance3D *disabled_box = make_box_instance(root, Vector3(2, -1.2, 0), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DISABLED);
	MeshInstance3D *outside_box = make_box_instance(root, Vector3(20, 0, 0), Vector3(1, 1, 1));
	MeshInstance3D *hidden_box = make_box_instance(root, Vector3(2, 0, 1.2), Vector3(0.4, 0.4, 0.4));
	hidden_box->hide();

	volume->bake();
	CHECK(volume->get_baked_triangle_count() == 12);

	const AABB baked = triangles_aabb(volume->get_static_bvh().get_triangles());
	CHECK(baked.position.is_equal_approx(Vector3(-0.5, -0.5, -0.5)));
	CHECK(baked.size.is_equal_approx(Vector3(1, 1, 1)));

	const Vector<LocalGITriangle> before = volume->get_static_bvh().get_triangles();
	volume->set_position(Vector3(10, 3, -4));
	volume->set_rotation_degrees(Vector3(0, 45, 0));
	CHECK(volume->get_baked_triangle_count() == before.size());
	for (int i = 0; i < before.size(); i++) {
		CHECK(volume->get_static_bvh().get_triangles()[i].v0.is_equal_approx(before[i].v0));
		CHECK(volume->get_static_bvh().get_triangles()[i].v1.is_equal_approx(before[i].v1));
		CHECK(volume->get_static_bvh().get_triangles()[i].v2.is_equal_approx(before[i].v2));
	}

	dynamic_box->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	disabled_box->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	outside_box->set_position(Vector3(2, 0, 0));
	CHECK(dynamic_box->is_visible_in_tree());
	CHECK(hidden_box->is_visible() == false);

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Direct hit, miss, nearest, parallel, and edge") {
	Vector<LocalGITriangle> triangles;
	triangles.push_back(make_triangle(Vector3(-1, -1, 1), Vector3(1, -1, 1), Vector3(0, 1, 1), 0));
	triangles.push_back(make_triangle(Vector3(-1, -1, 3), Vector3(1, -1, 3), Vector3(0, 1, 3), 1));
	triangles.push_back(make_triangle(Vector3(-1, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), 2));
	triangles.push_back(make_triangle(Vector3(-1, 0, 0), Vector3(0, -1, 0), Vector3(1, 0, 0), 3));

	LocalGIBVH bvh;
	bvh.build(triangles);
	CHECK_FALSE(bvh.is_empty());

	LocalGIRayHit hit;
	CHECK(bvh.intersect_ray(Vector3(0, 0, 0), Vector3(0, 0, 1), hit));
	CHECK(hit.triangle_index == 0);
	CHECK(hit.distance == doctest::Approx(1.0));
	CHECK(hit.position.is_equal_approx(Vector3(0, 0, 1)));

	CHECK_FALSE(bvh.intersect_ray(Vector3(0, 0, 0), Vector3(0, 0, -1), hit));
	CHECK_FALSE(hit.hit);

	CHECK_FALSE(bvh.intersect_ray(Vector3(0, 0, 2), Vector3(1, 0, 0), hit));

	CHECK(bvh.intersect_ray(Vector3(0, 0, 1), Vector3(0, 0, -1), hit));
	CHECK((hit.triangle_index == 2 || hit.triangle_index == 3));
	CHECK(hit.distance == doctest::Approx(1.0));
	CHECK(hit.position.is_equal_approx(Vector3(0, 0, 0)));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Thin wall 5/10/15/20 cm") {
	const real_t thicknesses[] = { 0.05, 0.10, 0.15, 0.20 };
	for (real_t thickness : thicknesses) {
		LocalGIBVH bvh;
		bvh.build(make_box_triangles(Vector3(thickness, 2, 2)));

		LocalGIRayHit hit;
		CHECK(bvh.intersect_ray(Vector3(-1, 0, 0), Vector3(1, 0, 0), hit));
		CHECK(hit.distance == doctest::Approx(1.0 - thickness * 0.5).epsilon(0.002));
		CHECK(hit.position.x == doctest::Approx(-thickness * 0.5).epsilon(0.002));

		CHECK_FALSE(bvh.intersect_ray(Vector3(-1, 3, 0), Vector3(1, 0, 0), hit));
	}
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Deterministic BVH build") {
	const Vector<LocalGITriangle> triangles = make_box_triangles(Vector3(1, 0.5, 2));
	REQUIRE(triangles.size() == 12);

	LocalGIBVH first;
	LocalGIBVH second;
	first.build(triangles);
	second.build(triangles);
	CHECK(bvh_nodes_equal(first, second));

	first.build(triangles);
	CHECK(bvh_nodes_equal(first, second));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Bake query uses local space after volume move") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(4, 4, 4));
	root->add_child(volume);
	make_box_instance(root, Vector3(0, 0, 0), Vector3(1, 1, 1));

	volume->bake();
	LocalGIRayHit before;
	CHECK(volume->intersect_static_ray(Vector3(0, 2, 0), Vector3(0, -1, 0), before));
	CHECK(before.distance == doctest::Approx(1.5));

	volume->set_position(Vector3(8, 0, 0));
	LocalGIRayHit after;
	CHECK(volume->intersect_static_ray(Vector3(0, 2, 0), Vector3(0, -1, 0), after));
	CHECK(after.distance == doctest::Approx(before.distance));
	CHECK(after.position.is_equal_approx(before.position));

	root->queue_free();
}

} // namespace TestLocalGIStaticBVH

#endif // _3D_DISABLED
