/**************************************************************************/
/*  test_local_gi_dynamic_bvh.cpp                                         */
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

TEST_FORCE_LINK(test_local_gi_dynamic_bvh)

#ifndef _3D_DISABLED

#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"

namespace TestLocalGIDynamicBVH {

static MeshInstance3D *make_box_instance(Node *p_parent, const Vector3 &p_position, const Vector3 &p_size, GeometryInstance3D::GIMode p_mode) {
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

static bool triangles_equal(const Vector<LocalGITriangle> &p_a, const Vector<LocalGITriangle> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (int i = 0; i < p_a.size(); i++) {
		if (!p_a[i].v0.is_equal_approx(p_b[i].v0) || !p_a[i].v1.is_equal_approx(p_b[i].v1) || !p_a[i].v2.is_equal_approx(p_b[i].v2)) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Relevant transform and mesh changes mark dirty") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(8, 8, 8));
	root->add_child(volume);

	MeshInstance3D *dynamic_box = make_box_instance(root, Vector3(0, 0, 1), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DYNAMIC);
	MeshInstance3D *static_box = make_box_instance(root, Vector3(0, 0, 0), Vector3(1, 1, 1), GeometryInstance3D::GI_MODE_STATIC);

	CHECK(volume->is_dynamic_dirty());
	CHECK(volume->update_dynamic());
	CHECK_FALSE(volume->is_dynamic_dirty());
	CHECK(volume->get_dynamic_contributor_count() == 1);
	CHECK(volume->get_dynamic_triangle_count() == 12);

	static_box->set_position(Vector3(0.2, 0, 0));
	CHECK_FALSE(volume->is_dynamic_dirty());

	dynamic_box->set_position(Vector3(0, 0, 1.4));
	CHECK(volume->is_dynamic_dirty());
	CHECK(volume->update_dynamic());
	CHECK_FALSE(volume->is_dynamic_dirty());

	Ref<BoxMesh> mesh = dynamic_box->get_mesh();
	mesh->set_size(Vector3(0.2, 0.2, 0.2));
	CHECK(volume->is_dynamic_dirty());
	CHECK(volume->update_dynamic());
	CHECK_FALSE(volume->is_dynamic_dirty());

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Stationary dynamic object does not rebuild continuously") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(8, 8, 8));
	root->add_child(volume);
	make_box_instance(root, Vector3(0, 0, 1), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DYNAMIC);

	CHECK(volume->update_dynamic());
	const int rebuilds = volume->get_dynamic_rebuild_count();
	CHECK(rebuilds >= 1);
	CHECK_FALSE(volume->update_dynamic());
	CHECK_FALSE(volume->update_dynamic());
	CHECK_FALSE(volume->is_dynamic_dirty());
	CHECK(volume->get_dynamic_rebuild_count() == rebuilds);

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Static BVH unchanged while dynamic rebuilds") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(8, 8, 8));
	root->add_child(volume);

	make_box_instance(root, Vector3(0, 0, 0), Vector3(1, 1, 1), GeometryInstance3D::GI_MODE_STATIC);
	MeshInstance3D *dynamic_box = make_box_instance(root, Vector3(0, 0, 1.2), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DYNAMIC);

	volume->bake();
	CHECK(volume->update_dynamic());
	const Vector<LocalGITriangle> static_before = volume->get_static_bvh().get_triangles();
	REQUIRE(static_before.size() == 12);

	dynamic_box->set_position(Vector3(0.5, 0, 1.2));
	CHECK(volume->update_dynamic());
	CHECK(triangles_equal(static_before, volume->get_static_bvh().get_triangles()));
	CHECK(volume->get_baked_triangle_count() == 12);

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Dynamic BVH rebuild changes hit") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(8, 8, 8));
	root->add_child(volume);

	MeshInstance3D *dynamic_box = make_box_instance(root, Vector3(0, 0, 1), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DYNAMIC);
	CHECK(volume->update_dynamic());

	LocalGIRayHit before;
	CHECK(volume->intersect_dynamic_ray(Vector3(0, 0, 0), Vector3(0, 0, 1), before));
	CHECK(before.distance == doctest::Approx(0.8).epsilon(0.002));

	dynamic_box->set_position(Vector3(0, 0, 2));
	CHECK(volume->update_dynamic());

	LocalGIRayHit after;
	CHECK(volume->intersect_dynamic_ray(Vector3(0, 0, 0), Vector3(0, 0, 1), after));
	CHECK(after.distance == doctest::Approx(1.8).epsilon(0.002));
	CHECK(after.distance != doctest::Approx(before.distance));

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Removing dynamic object updates BVH") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(8, 8, 8));
	root->add_child(volume);

	MeshInstance3D *dynamic_box = make_box_instance(root, Vector3(0, 0, 1), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DYNAMIC);
	CHECK(volume->update_dynamic());
	CHECK(volume->get_dynamic_contributor_count() == 1);

	LocalGIRayHit hit;
	CHECK(volume->intersect_dynamic_ray(Vector3(0, 0, 0), Vector3(0, 0, 1), hit));

	root->remove_child(dynamic_box);
	memdelete(dynamic_box);
	CHECK(volume->is_dynamic_dirty());
	CHECK(volume->update_dynamic());
	CHECK(volume->get_dynamic_contributor_count() == 0);
	CHECK(volume->get_dynamic_triangle_count() == 0);
	CHECK_FALSE(volume->intersect_dynamic_ray(Vector3(0, 0, 0), Vector3(0, 0, 1), hit));

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Static plus dynamic nearest hit") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(8, 8, 8));
	root->add_child(volume);

	make_box_instance(root, Vector3(0, 0, 0), Vector3(1, 1, 1), GeometryInstance3D::GI_MODE_STATIC);
	MeshInstance3D *dynamic_box = make_box_instance(root, Vector3(0, 0, 1.2), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DYNAMIC);

	volume->bake();
	CHECK(volume->update_dynamic());

	LocalGIRayHit from_behind_static;
	CHECK(volume->intersect_ray(Vector3(0, 0, -2), Vector3(0, 0, 1), from_behind_static));
	CHECK(from_behind_static.distance == doctest::Approx(1.5).epsilon(0.002));
	CHECK(from_behind_static.position.z == doctest::Approx(-0.5).epsilon(0.002));

	LocalGIRayHit from_behind_dynamic;
	CHECK(volume->intersect_ray(Vector3(0, 0, 3), Vector3(0, 0, -1), from_behind_dynamic));
	CHECK(from_behind_dynamic.distance == doctest::Approx(1.6).epsilon(0.002));
	CHECK(from_behind_dynamic.position.z == doctest::Approx(1.4).epsilon(0.002));

	dynamic_box->set_position(Vector3(0, 0, -1.2));
	CHECK(volume->update_dynamic());

	LocalGIRayHit after_move;
	CHECK(volume->intersect_ray(Vector3(0, 0, -3), Vector3(0, 0, 1), after_move));
	CHECK(after_move.distance == doctest::Approx(1.6).epsilon(0.002));
	CHECK(after_move.position.z == doctest::Approx(-1.4).epsilon(0.002));

	root->queue_free();
}

} // namespace TestLocalGIDynamicBVH

#endif // _3D_DISABLED
