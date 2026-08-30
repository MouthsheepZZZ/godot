/**************************************************************************/
/*  test_local_lrt_math.cpp                                               */
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
/* SOFTWARE.                                                              */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_local_lrt_math)

#include "scene/3d/local_lrt_math.h"

namespace TestLocalLRTMath {

using namespace LocalLRTMath;

static bool vector4_is_equal_approx(const Vector4 &p_a, const Vector4 &p_b, real_t p_tolerance = 0.00001) {
	return p_a.is_equal_approx(p_b) || (p_a - p_b).length() <= p_tolerance;
}

TEST_CASE("[LocalLRTMath] SH2 PI-div DFT matches frozen Y00 and 2/3 Y1") {
	const Vector4 dft = sh2_pi_div_dft(Vector3(0, 0, -1));
	CHECK(dft.x == doctest::Approx(SH_Y00));
	CHECK(dft.y == doctest::Approx(0.0));
	CHECK(dft.z == doctest::Approx(0.0));
	CHECK(dft.w == doctest::Approx(-SH_Y1 * (2.0 / 3.0)));
	CHECK(sh_basis(Vector3(1, 0, 0)).y == doctest::Approx(SH_Y1));
}

TEST_CASE("[LocalLRTMath] Constant SH encode and evaluate") {
	const Vector4 constant = encode_constant(0.65);
	CHECK(Math::is_equal_approx(evaluate(constant, Vector3(1, 0, 0)), (real_t)0.65));
	CHECK(Math::is_equal_approx(evaluate(constant, Vector3(0, -1, 0)), (real_t)0.65));
	CHECK(Math::is_equal_approx(evaluate(constant, Vector3(0, 0, 1)), (real_t)0.65));
}

TEST_CASE("[LocalLRTMath] Directional lobe follows its encoded direction") {
	const Vector4 lobe = encode_direction(Vector3(1, 0, 0), 1.0, Math::TAU);
	CHECK(evaluate(lobe, Vector3(1, 0, 0)) > evaluate(lobe, Vector3(0, 1, 0)));
	CHECK(evaluate(lobe, Vector3(0, 1, 0)) > evaluate(lobe, Vector3(-1, 0, 0)));
}

TEST_CASE("[LocalLRTMath] Diffuse irradiance convolution follows surface normal") {
	const Vector4 constant = encode_constant(0.5);
	CHECK(Math::is_equal_approx(evaluate_diffuse_irradiance(constant, Vector3(0, 1, 0)), (real_t)(0.5 * Math::PI)));

	const Vector4 positive_x = encode_direction(Vector3(1, 0, 0), 1.0, Math::TAU);
	CHECK(evaluate_diffuse_irradiance(positive_x, Vector3(1, 0, 0)) > evaluate_diffuse_irradiance(positive_x, Vector3(-1, 0, 0)));
}

TEST_CASE("[LocalLRTMath] Triple product preserves constant multiplication") {
	const Vector4 directional = encode_direction(Vector3(0, 0, 1), 0.75, Math::TAU);
	CHECK(vector4_is_equal_approx(triple_product(directional, encode_constant(0.4)), directional * 0.4));
}

TEST_CASE("[LocalLRTMath] SH rotation uses local-to-world orientation") {
	const Vector4 local_x = encode_direction(Vector3(1, 0, 0), 1.0, Math::TAU);
	const Basis rotation_90(Vector3(0, 0, 1), Math::PI / 2.0);
	const Vector4 world_y = rotate_to_world(local_x, rotation_90);
	CHECK(evaluate(world_y, Vector3(0, 1, 0)) > evaluate(world_y, Vector3(1, 0, 0)));
	CHECK(vector4_is_equal_approx(rotate_to_local(world_y, rotation_90), local_x));

	const Basis rotation_180(Vector3(0, 1, 0), Math::PI);
	const Vector4 world_negative_x = rotate_to_world(local_x, rotation_180);
	CHECK(evaluate(world_negative_x, Vector3(-1, 0, 0)) > evaluate(world_negative_x, Vector3(1, 0, 0)));
}

TEST_CASE("[LocalLRTMath] Transfer matrix is row-major and rotates as D transpose B D") {
	SH2Matrix local_transfer;
	local_transfer.rows[1].y = 0.5;

	Vector4 local_input;
	local_input.y = 2.0;
	CHECK(vector4_is_equal_approx(local_transfer.xform(local_input), Vector4(0, 1, 0, 0)));

	const Basis rotation(Vector3(0, 0, 1), Math::PI / 2.0);
	const SH2Matrix world_transfer = rotate_transfer_to_world(local_transfer, rotation);
	Vector4 world_input;
	world_input.z = 2.0;
	CHECK(vector4_is_equal_approx(world_transfer.xform(world_input), Vector4(0, 0, 1, 0)));
}

TEST_CASE("[LocalLRTMath] Local world grid and UVW conversions round trip") {
	const Vector3 size(4.0, 3.0, 2.0);
	const Vector3i resolution = probe_resolution(size, 0.9);
	CHECK(resolution == Vector3i(6, 5, 4));

	const Vector3 grid_position(2.0, 3.0, 1.0);
	const Vector3 local_position = grid_to_local(grid_position, size, resolution);
	CHECK(local_to_grid(local_position, size, resolution).is_equal_approx(grid_position));
	CHECK(uvw_to_grid(grid_to_uvw(grid_position, resolution), resolution).is_equal_approx(grid_position));

	const Transform3D transform(Basis(Vector3(0, 1, 0), Math::PI / 2.0), Vector3(8.0, -2.0, 3.0));
	const Vector3 world_position = local_to_world(local_position, transform);
	CHECK(world_to_local(world_position, transform).is_equal_approx(local_position));
}

TEST_CASE("[LocalLRTMath] Edge blend fades inside bounds and rejects outside positions") {
	const Vector3 size(8.0, 6.0, 4.0);
	CHECK(Math::is_equal_approx(edge_blend_weight(Vector3(), size, 1.0), (real_t)1.0));
	CHECK(Math::is_equal_approx(edge_blend_weight(Vector3(3.5, 0.0, 0.0), size, 1.0), (real_t)0.5));
	CHECK(Math::is_equal_approx(edge_blend_weight(Vector3(4.0, 0.0, 0.0), size, 1.0), (real_t)0.0));
	CHECK(Math::is_equal_approx(edge_blend_weight(Vector3(4.1, 0.0, 0.0), size, 1.0), (real_t)0.0));
	CHECK(Math::is_equal_approx(edge_blend_weight(Vector3(3.9, 0.0, 0.0), size, 0.0), (real_t)1.0));
}

TEST_CASE("[LocalLRTMath] Probe indexing and 26-neighbor weights are stable") {
	const Vector3i resolution(4, 5, 6);
	for (int z = 0; z < resolution.z; z++) {
		for (int y = 0; y < resolution.y; y++) {
			for (int x = 0; x < resolution.x; x++) {
				const Vector3i position(x, y, z);
				CHECK(probe_position(probe_index(position, resolution), resolution) == position);
			}
		}
	}

	real_t weight_sum = 0.0;
	for (int i = 0; i < NEIGHBOR_COUNT; i++) {
		const Vector3i offset = neighbor_offset(i);
		CHECK(offset != Vector3i());
		weight_sum += neighbor_weight(offset);
	}
	CHECK(Math::is_equal_approx(weight_sum, (real_t)1.0));
}

TEST_CASE("[LocalLRTMath] Full local visibility preserves full propagated visibility") {
	const Vector4 fully_visible = encode_constant(1.0);
	Vector4 neighbor_visibility[NEIGHBOR_COUNT];
	for (int i = 0; i < NEIGHBOR_COUNT; i++) {
		neighbor_visibility[i] = fully_visible;
	}
	CHECK(vector4_is_equal_approx(propagate_visibility(fully_visible, neighbor_visibility), fully_visible));
}

TEST_CASE("[LocalLRTMath] Empty space continues radiance and no-light state decays") {
	const Vector4 fully_visible = encode_constant(1.0);
	Vector4 neighbor_visibility[NEIGHBOR_COUNT];
	Vector4 neighbor_radiance[NEIGHBOR_COUNT];
	for (int i = 0; i < NEIGHBOR_COUNT; i++) {
		neighbor_visibility[i] = fully_visible;
		neighbor_radiance[i] = encode_constant(0.5);
	}

	const SH2Matrix no_surface_transfer;
	const Vector4 propagated = propagate_radiance(
			fully_visible, no_surface_transfer, Vector4(), neighbor_radiance, neighbor_visibility, 1.0, 0.8);
	real_t expected_factor = 0.0;
	for (int i = 0; i < NEIGHBOR_COUNT; i++) {
		expected_factor += neighbor_weight(neighbor_offset(i)) * radiance_distance_decay(neighbor_offset(i), Vector3(1, 1, 1), 0.8);
	}
	CHECK(vector4_is_equal_approx(propagated, encode_constant(0.5 * expected_factor)));
	CHECK(Math::is_equal_approx(Math::pow(radiance_distance_decay(Vector3i(1, 0, 0), Vector3(0.25, 0.25, 0.25), 0.8), (real_t)4.0), (real_t)0.8));

	Vector4 energy = propagated;
	for (int iteration = 0; iteration < 16; iteration++) {
		for (int i = 0; i < NEIGHBOR_COUNT; i++) {
			neighbor_radiance[i] = energy;
		}
		const Vector4 next = propagate_radiance(
				fully_visible, no_surface_transfer, Vector4(), neighbor_radiance, neighbor_visibility, 1.0, 0.8);
		CHECK(next.length() < energy.length());
		energy = next;
	}
	CHECK(energy.length() < propagated.length() * 0.04);
}

TEST_CASE("[LocalLRTMath] Directional shadow projection covers the volume and texel-snaps") {
	const AABB volume(Vector3(-2, -2, -2), Vector3(4, 4, 4));
	const DirectionalShadowProjection shadow = compute_directional_shadow_projection(volume, Vector3(0, 1, 0), 64);
	const Projection view_proj = directional_shadow_view_projection(shadow.camera, shadow.projection);
	for (int i = 0; i < 8; i++) {
		Vector2 uv;
		real_t depth = 0.0;
		CHECK(directional_shadow_project_point(view_proj, volume.get_endpoint(i), uv, depth));
	}
	const real_t width = shadow.projection.get_z_far() - shadow.projection.get_z_near();
	CHECK(width > 0.0);
	const real_t snapped_width = shadow.projection[0][0];
	CHECK(snapped_width != 0.0);
}

TEST_CASE("[LocalLRTMath] Plane occluder lights the front and shadows the back") {
	const AABB volume(Vector3(-2, -2, -2), Vector3(4, 4, 4));
	const DirectionalShadowProjection shadow = compute_directional_shadow_projection(volume, Vector3(0, 1, 0), 64);
	const Projection view_proj = directional_shadow_view_projection(shadow.camera, shadow.projection);
	Vector2 plane_uv;
	Vector2 front_uv;
	Vector2 back_uv;
	real_t plane_depth = 0.0;
	real_t front_depth = 0.0;
	real_t back_depth = 0.0;
	REQUIRE(directional_shadow_project_point(view_proj, Vector3(0, 0, 0), plane_uv, plane_depth));
	REQUIRE(directional_shadow_project_point(view_proj, Vector3(0, 1, 0), front_uv, front_depth));
	REQUIRE(directional_shadow_project_point(view_proj, Vector3(0, -1, 0), back_uv, back_depth));
	CHECK(front_depth > plane_depth);
	CHECK(plane_depth > back_depth);
	Vector<float> depths;
	fill_constant_shadow_depth(depths, 64, (float)plane_depth);
	CHECK(sample_directional_shadow_visibility(depths, 64, view_proj, Vector3(0, 1, 0), 0.001) > 0.9);
	CHECK(sample_directional_shadow_visibility(depths, 64, view_proj, Vector3(0, -1, 0), 0.001) < 0.1);
}

} // namespace TestLocalLRTMath
