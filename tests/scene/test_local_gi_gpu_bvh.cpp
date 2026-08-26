/**************************************************************************/
/*  test_local_gi_gpu_bvh.cpp                                             */
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

TEST_FORCE_LINK(test_local_gi_gpu_bvh)

#ifndef _3D_DISABLED

#include "scene/3d/local_gi/local_gi_gpu_tracer.h"
#include "scene/3d/local_gi/local_gi_static_geometry.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"

namespace TestLocalGIGpuBVH {

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

static void push_ray(Vector<Vector3> &r_origins, Vector<Vector3> &r_dirs, const Vector3 &p_origin, const Vector3 &p_dir) {
	r_origins.push_back(p_origin);
	r_dirs.push_back(p_dir);
}

static bool require_gpu(LocalGIGpuTracer &p_tracer) {
	const bool available = p_tracer.ensure_available();
	REQUIRE_MESSAGE(available, "LocalGI GPU tracer could not create a RenderingDevice.");
	return available;
}

TEST_CASE("[SceneTree][LocalGIVolume3D] CPU/GPU hit miss nearest identity") {
	LocalGIGpuTracer tracer;
	if (!require_gpu(tracer)) {
		return;
	}

	Vector<LocalGITriangle> triangles;
	triangles.push_back(make_triangle(Vector3(-1, -1, 1), Vector3(1, -1, 1), Vector3(0, 1, 1), 0));
	triangles.push_back(make_triangle(Vector3(-1, -1, 3), Vector3(1, -1, 3), Vector3(0, 1, 3), 1));
	triangles.push_back(make_triangle(Vector3(-1, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), 2));

	LocalGIBVH bvh;
	bvh.build(triangles);
	REQUIRE(tracer.upload(bvh, LocalGIBVH()));

	Vector<Vector3> origins;
	Vector<Vector3> dirs;
	push_ray(origins, dirs, Vector3(0, 0, 0), Vector3(0, 0, 1));
	push_ray(origins, dirs, Vector3(0, 0, 0), Vector3(0, 0, -1));
	push_ray(origins, dirs, Vector3(0, 0, 2), Vector3(1, 0, 0));
	push_ray(origins, dirs, Vector3(0, 0, 4), Vector3(0, 0, -1));

	Vector<LocalGIRayHit> cpu_hits;
	cpu_hits.resize(origins.size());
	for (int i = 0; i < origins.size(); i++) {
		bvh.intersect_ray(origins[i], dirs[i], cpu_hits.write[i]);
	}

	Vector<LocalGIRayHit> gpu_hits;
	REQUIRE(tracer.trace(origins, dirs, gpu_hits));
	const LocalGICPUGPUCompareResult compare = LocalGIGpuTracer::compare_hits(cpu_hits, gpu_hits);
	CHECK(compare.hit_mismatch == 0);
	CHECK(compare.nearest_mismatch == 0);
	CHECK(compare.identity_mismatch == 0);
	CHECK(compare.max_distance_error <= LocalGIGpuTracer::DISTANCE_TOLERANCE);
	CHECK(compare.max_normal_error <= LocalGIGpuTracer::NORMAL_ERROR_TOLERANCE);
	CHECK(compare.passed);
	CHECK(cpu_hits[0].hit);
	CHECK_FALSE(cpu_hits[1].hit);
}

TEST_CASE("[SceneTree][LocalGIVolume3D] CPU/GPU thin wall distances") {
	LocalGIGpuTracer tracer;
	if (!require_gpu(tracer)) {
		return;
	}

	const real_t thicknesses[] = { 0.05, 0.10, 0.15, 0.20 };
	for (real_t thickness : thicknesses) {
		LocalGIBVH bvh;
		bvh.build(make_box_triangles(Vector3(thickness, 2, 2)));
		REQUIRE(tracer.upload(bvh, LocalGIBVH()));

		Vector<Vector3> origins;
		Vector<Vector3> dirs;
		push_ray(origins, dirs, Vector3(-1, 0, 0), Vector3(1, 0, 0));
		push_ray(origins, dirs, Vector3(-1, 3, 0), Vector3(1, 0, 0));

		Vector<LocalGIRayHit> cpu_hits;
		cpu_hits.resize(2);
		bvh.intersect_ray(origins[0], dirs[0], cpu_hits.write[0]);
		bvh.intersect_ray(origins[1], dirs[1], cpu_hits.write[1]);

		Vector<LocalGIRayHit> gpu_hits;
		REQUIRE(tracer.trace(origins, dirs, gpu_hits));
		CHECK(LocalGIGpuTracer::compare_hits(cpu_hits, gpu_hits).passed);
		CHECK(cpu_hits[0].hit);
		CHECK_FALSE(cpu_hits[1].hit);
	}
}

TEST_CASE("[SceneTree][LocalGIVolume3D] CPU/GPU static dynamic nearest") {
	LocalGIGpuTracer availability;
	if (!require_gpu(availability)) {
		return;
	}

	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(8, 8, 8));
	root->add_child(volume);
	make_box_instance(root, Vector3(0, 0, 0), Vector3(1, 1, 1), GeometryInstance3D::GI_MODE_STATIC);
	make_box_instance(root, Vector3(0, 0, 1.2), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_DYNAMIC);

	volume->bake();
	volume->update_dynamic();
	REQUIRE(volume->is_gpu_available());
	REQUIRE(volume->upload_gpu());

	Vector<Vector3> origins;
	Vector<Vector3> dirs;
	push_ray(origins, dirs, Vector3(0, 0, -2), Vector3(0, 0, 1));
	push_ray(origins, dirs, Vector3(0, 2, 1.2), Vector3(0, -1, 0));
	push_ray(origins, dirs, Vector3(3, 3, 3), Vector3(1, 0, 0));

	const LocalGICPUGPUCompareResult compare = volume->compare_cpu_gpu_rays(origins, dirs);
	CHECK(compare.hit_mismatch == 0);
	CHECK(compare.nearest_mismatch == 0);
	CHECK(compare.identity_mismatch == 0);
	CHECK(compare.max_distance_error <= LocalGIGpuTracer::DISTANCE_TOLERANCE);
	CHECK(compare.max_normal_error <= LocalGIGpuTracer::NORMAL_ERROR_TOLERANCE);
	CHECK(compare.passed);

	LocalGIRayHit cpu_near;
	LocalGIRayHit gpu_near;
	CHECK(volume->intersect_ray(origins[0], dirs[0], cpu_near));
	CHECK(volume->intersect_gpu_ray(origins[0], dirs[0], gpu_near));
	CHECK(cpu_near.triangle_index == gpu_near.triangle_index);

	root->queue_free();
}

} // namespace TestLocalGIGpuBVH

#endif // _3D_DISABLED
