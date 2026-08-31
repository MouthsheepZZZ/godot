/**************************************************************************/
/*  test_local_lrt_color_sdf.cpp                                          */
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

TEST_FORCE_LINK(test_local_lrt_color_sdf)

#include "scene/3d/local_lrt_color_sdf.h"
#include "scene/resources/3d/primitive_meshes.h"

namespace TestLocalLRTColorSDF {

static real_t max_sample_error(const LocalLRTColorSDF &p_field, const LocalLRTColorSDF &p_reference, real_t p_extent, int p_steps) {
	real_t max_distance_error = 0.0;
	for (int z = 0; z < p_steps; z++) {
		for (int y = 0; y < p_steps; y++) {
			for (int x = 0; x < p_steps; x++) {
				const Vector3 position(
						Math::lerp(-p_extent, p_extent, x / (real_t)(p_steps - 1)),
						Math::lerp(-p_extent, p_extent, y / (real_t)(p_steps - 1)),
						Math::lerp(-p_extent, p_extent, z / (real_t)(p_steps - 1)));
				const real_t error = Math::abs(p_field.sample(position).signed_distance - p_reference.sample(position).signed_distance);
				max_distance_error = MAX(max_distance_error, error);
			}
		}
	}
	return max_distance_error;
}

TEST_CASE("[LocalLRTColorSDF] Analytic box matches signed distance coverage and outward normals") {
	const Vector3 half_extents(0.5, 0.4, 0.3);
	const Color albedo(0.8, 0.1, 0.1);
	const Color emission(0.2, 0.0, 0.0);
	const Color transfer_emission(0.1, 0.0, 0.0);
	const LocalLRTColorSDF box = LocalLRTColorSDF::make_box(half_extents, 0.25, albedo, emission, transfer_emission);

	CHECK(box.is_analytic());
	CHECK(box.get_voxel_size() == doctest::Approx(0.25));
	CHECK(box.get_albedo().is_equal_approx(albedo));
	CHECK(box.get_emission().is_equal_approx(emission));
	CHECK(box.get_transfer_emission().is_equal_approx(transfer_emission));

	const LocalLRTColorSDF::Sample inside = box.sample(Vector3());
	CHECK(inside.signed_distance == doctest::Approx(-0.3));
	CHECK(inside.coverage == doctest::Approx(1.0));
	CHECK(inside.albedo.is_equal_approx(albedo));
	CHECK(inside.emission.is_equal_approx(emission));
	CHECK(inside.transfer_emission.is_equal_approx(transfer_emission));

	const LocalLRTColorSDF::Sample face = box.sample(Vector3(0.5, 0.0, 0.0));
	CHECK(face.signed_distance == doctest::Approx(0.0));
	CHECK(face.coverage == doctest::Approx(0.5));
	CHECK(face.normal.is_equal_approx(Vector3(1, 0, 0)));

	const LocalLRTColorSDF::Sample outside = box.sample(Vector3(1.0, 0.0, 0.0));
	CHECK(outside.signed_distance == doctest::Approx(0.5));
	CHECK(outside.coverage == doctest::Approx(0.0));
	CHECK(outside.normal.is_equal_approx(Vector3(1, 0, 0)));
}

TEST_CASE("[LocalLRTColorSDF] Analytic sphere matches signed distance coverage and outward normals") {
	const LocalLRTColorSDF sphere = LocalLRTColorSDF::make_sphere(0.75, 0.25, Color(0.2, 0.8, 0.2), Color(0.0, 0.4, 0.0));
	CHECK(sphere.is_analytic());
	CHECK(sphere.sample(Vector3()).signed_distance == doctest::Approx(-0.75));
	CHECK(sphere.sample(Vector3()).coverage == doctest::Approx(1.0));
	CHECK(sphere.sample(Vector3(0.75, 0.0, 0.0)).signed_distance == doctest::Approx(0.0));
	CHECK(sphere.sample(Vector3(0.75, 0.0, 0.0)).coverage == doctest::Approx(0.5));
	CHECK(sphere.sample(Vector3(0.75, 0.0, 0.0)).normal.is_equal_approx(Vector3(1, 0, 0)));
	CHECK(sphere.sample(Vector3(1.5, 0.0, 0.0)).signed_distance == doctest::Approx(0.75));
	CHECK(sphere.sample(Vector3(1.5, 0.0, 0.0)).coverage == doctest::Approx(0.0));
}

TEST_CASE("[LocalLRTColorSDF] Voxelized BoxMesh converges to analytic box as voxel size shrinks") {
	const Vector3 size(1.0, 0.8, 0.6);
	const Vector3 half_extents = size * 0.5;
	const Color albedo(0.7, 0.7, 0.7);
	const LocalLRTColorSDF analytic = LocalLRTColorSDF::make_box(half_extents, 0.125, albedo);

	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(size);

	real_t previous_error = 1.0e20;
	const real_t voxel_sizes[] = { 0.5, 0.25, 0.125 };
	for (real_t voxel_size : voxel_sizes) {
		const LocalLRTColorSDF voxelized = LocalLRTColorSDF::from_mesh(mesh, voxel_size, albedo);
		CHECK_FALSE(voxelized.is_analytic());
		CHECK(voxelized.get_voxel_size() == doctest::Approx(voxel_size));
		CHECK(voxelized.get_resolution().x >= 2);
		CHECK(voxelized.get_actual_voxel_size().x <= voxel_size + CMP_EPSILON);

		const Vector3i center_voxel = voxelized.get_resolution() / 2;
		const LocalLRTColorSDF::Sample center = voxelized.sample(voxelized.get_voxel_local_position(center_voxel));
		CHECK(center.signed_distance < 0.0);
		CHECK(center.albedo.is_equal_approx(albedo));

		const real_t error = max_sample_error(voxelized, analytic, 0.9, 7);
		CHECK(error <= previous_error + 0.02);
		previous_error = error;
	}
	CHECK(previous_error < 0.08);
}

TEST_CASE("[LocalLRTColorSDF] Voxelized SphereMesh converges to analytic sphere as voxel size shrinks") {
	const real_t radius = 0.6;
	const LocalLRTColorSDF analytic = LocalLRTColorSDF::make_sphere(radius, 0.125, Color(1, 1, 1));

	Ref<SphereMesh> mesh;
	mesh.instantiate();
	mesh->set_radius(radius);
	mesh->set_height(radius * 2.0);
	mesh->set_radial_segments(24);
	mesh->set_rings(12);

	real_t previous_error = 1.0e20;
	const real_t voxel_sizes[] = { 0.5, 0.25, 0.125 };
	for (real_t voxel_size : voxel_sizes) {
		const LocalLRTColorSDF voxelized = LocalLRTColorSDF::from_mesh(mesh, voxel_size, Color(1, 1, 1));
		CHECK_FALSE(voxelized.is_empty());
		const real_t error = max_sample_error(voxelized, analytic, 1.0, 7);
		CHECK(error <= previous_error + 0.03);
		previous_error = error;
	}
	CHECK(previous_error < 0.12);
}

TEST_CASE("[LocalLRTColorSDF] Geometry voxel size is independent of probe spacing") {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(1, 1, 1));

	const LocalLRTColorSDF coarse = LocalLRTColorSDF::from_mesh(mesh, 0.5, Color(1, 1, 1));
	const LocalLRTColorSDF fine = LocalLRTColorSDF::from_mesh(mesh, 0.25, Color(1, 1, 1));
	CHECK(fine.get_resolution().x > coarse.get_resolution().x);
	CHECK(fine.get_voxel_size() == doctest::Approx(0.25));
	CHECK(coarse.get_voxel_size() == doctest::Approx(0.5));
	CHECK(fine.get_resolution() == LocalLRTColorSDF::from_mesh(mesh, 0.25, Color(1, 1, 1)).get_resolution());
}

} // namespace TestLocalLRTColorSDF
