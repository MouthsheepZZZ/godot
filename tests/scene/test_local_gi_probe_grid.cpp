/**************************************************************************/
/*  test_local_gi_probe_grid.cpp                                          */
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

TEST_FORCE_LINK(test_local_gi_probe_grid)

#ifndef _3D_DISABLED

#include "scene/3d/local_gi/local_gi_probe_grid.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"

namespace TestLocalGIProbeGrid {

static MeshInstance3D *make_box_instance(Node *p_parent, const Vector3 &p_position, const Vector3 &p_size) {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(p_size);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_position(p_position);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	p_parent->add_child(instance);
	return instance;
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Probe count and local positions") {
	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	SceneTree::get_singleton()->get_root()->add_child(volume);
	volume->set_size(Vector3(4, 4, 4));
	volume->set_probe_spacing(0.5);
	volume->set_rays_per_probe(32);
	volume->build_probes();

	CHECK(volume->get_probe_resolution() == Vector3i(8, 8, 8));
	CHECK(volume->get_probe_count() == 512);
	CHECK(volume->get_probe_position(0).is_equal_approx(Vector3(-1.75, -1.75, -1.75)));
	CHECK(volume->get_probe_position(7).is_equal_approx(Vector3(-1.75, -1.75, 1.75)));
	CHECK(volume->get_probe_position(8).is_equal_approx(Vector3(-1.75, -1.25, -1.75)));
	CHECK(volume->get_probe_position(511).is_equal_approx(Vector3(1.75, 1.75, 1.75)));

	volume->set_size(Vector3(4.4, 4.4, 4.4));
	volume->build_probes();
	CHECK(volume->get_probe_resolution() == Vector3i(8, 8, 8));
	CHECK(volume->get_probe_count() == 512);
	CHECK(volume->get_probe_position(0).is_equal_approx(Vector3(-1.925, -1.925, -1.925)));

	volume->set_size(Vector3(0.3, 0.3, 0.3));
	volume->set_probe_spacing(0.5);
	volume->build_probes();
	CHECK(volume->get_probe_resolution() == Vector3i(2, 2, 2));
	CHECK(volume->get_probe_count() == 8);

	volume->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Probe directions normalized and deterministic") {
	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	SceneTree::get_singleton()->get_root()->add_child(volume);
	volume->set_size(Vector3(4, 4, 4));
	volume->set_probe_spacing(1.0);
	volume->set_rays_per_probe(64);
	volume->build_probes();

	const PackedVector3Array first = volume->get_probe_directions();
	CHECK(first.size() == 64);
	for (int i = 0; i < first.size(); i++) {
		CHECK(first[i].is_normalized());
	}

	volume->build_probes();
	const PackedVector3Array second = volume->get_probe_directions();
	REQUIRE(second.size() == first.size());
	for (int i = 0; i < first.size(); i++) {
		CHECK(second[i].is_equal_approx(first[i]));
	}

	volume->set_size(Vector3(6, 2, 8));
	volume->set_probe_spacing(0.5);
	volume->build_probes();
	const PackedVector3Array after_layout_change = volume->get_probe_directions();
	REQUIRE(after_layout_change.size() == first.size());
	for (int i = 0; i < first.size(); i++) {
		CHECK(after_layout_change[i].is_equal_approx(first[i]));
	}

	Vector<Vector3> standalone;
	LocalGIProbeGrid::generate_directions(64, standalone);
	REQUIRE(standalone.size() == first.size());
	for (int i = 0; i < first.size(); i++) {
		CHECK(standalone[i].is_equal_approx(first[i]));
	}

	volume->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Probe ray budget exact") {
	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	SceneTree::get_singleton()->get_root()->add_child(volume);
	volume->set_size(Vector3(2, 2, 2));
	volume->set_probe_spacing(0.5);
	volume->set_rays_per_probe(32);
	volume->build_probes();

	CHECK(volume->get_probe_resolution() == Vector3i(4, 4, 4));
	CHECK(volume->get_probe_count() == 64);
	CHECK(volume->get_probe_ray_budget() == 64 * 32);

	Vector<Vector3> origins;
	Vector<Vector3> directions;
	volume->collect_probe_rays(origins, directions);
	CHECK(origins.size() == volume->get_probe_ray_budget());
	CHECK(directions.size() == volume->get_probe_ray_budget());
	CHECK(origins[0].is_equal_approx(volume->get_probe_position(0)));
	CHECK(origins[31].is_equal_approx(volume->get_probe_position(0)));
	CHECK(origins[32].is_equal_approx(volume->get_probe_position(1)));

	volume->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Volume transform does not change local probes") {
	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	SceneTree::get_singleton()->get_root()->add_child(volume);
	volume->set_size(Vector3(4, 4, 4));
	volume->set_probe_spacing(0.5);
	volume->set_rays_per_probe(32);
	volume->build_probes();

	const Vector3i resolution = volume->get_probe_resolution();
	const int count = volume->get_probe_count();
	const int budget = volume->get_probe_ray_budget();
	const PackedVector3Array positions = volume->get_probe_positions();
	const PackedVector3Array directions = volume->get_probe_directions();

	volume->set_transform(Transform3D(Basis::from_euler(Vector3(0.4, 1.1, -0.25)), Vector3(3, -2, 5)));
	volume->build_probes();

	CHECK(volume->get_probe_resolution() == resolution);
	CHECK(volume->get_probe_count() == count);
	CHECK(volume->get_probe_ray_budget() == budget);
	const PackedVector3Array moved_positions = volume->get_probe_positions();
	const PackedVector3Array moved_directions = volume->get_probe_directions();
	REQUIRE(moved_positions.size() == positions.size());
	REQUIRE(moved_directions.size() == directions.size());
	for (int i = 0; i < positions.size(); i++) {
		CHECK(moved_positions[i].is_equal_approx(positions[i]));
	}
	for (int i = 0; i < directions.size(); i++) {
		CHECK(moved_directions[i].is_equal_approx(directions[i]));
	}

	volume->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Probe GPU rays match CPU") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(4, 4, 4));
	volume->set_probe_spacing(2.0);
	volume->set_rays_per_probe(32);
	root->add_child(volume);
	make_box_instance(root, Vector3(0, 0, 0), Vector3(1, 1, 1));

	volume->bake();
	volume->update_dynamic();
	volume->build_probes();
	CHECK(volume->get_probe_count() == 8);
	CHECK(volume->get_probe_ray_budget() == 256);

	if (volume->is_gpu_available()) {
		REQUIRE(volume->upload_gpu());
		Vector<Vector3> origins;
		Vector<Vector3> directions;
		volume->collect_probe_rays(origins, directions);
		const LocalGICPUGPUCompareResult compare = volume->compare_cpu_gpu_rays(origins, directions);
		CHECK(compare.ray_count == 256);
		CHECK(compare.passed);
		CHECK(volume->trace_probe_rays());
	}

	root->queue_free();
}

} // namespace TestLocalGIProbeGrid

#endif // _3D_DISABLED
