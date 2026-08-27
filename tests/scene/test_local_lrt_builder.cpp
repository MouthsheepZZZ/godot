/**************************************************************************/
/*  test_local_lrt_builder.cpp                                            */
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

TEST_FORCE_LINK(test_local_lrt_builder)

#include "scene/3d/local_lrt_builder.h"

namespace TestLocalLRTBuilder {

using namespace LocalLRTMath;

static void set_x_wall(LocalLRTBuilder &r_grid, int p_x, const Color &p_albedo) {
	const Vector3i resolution = r_grid.get_resolution();
	for (int z = 0; z < resolution.z; z++) {
		for (int y = 0; y < resolution.y; y++) {
			r_grid.set_occupancy(Vector3i(p_x, y, z), p_albedo);
		}
	}
}

static void set_closed_box(LocalLRTBuilder &r_grid, const Color &p_albedo) {
	const Vector3i resolution = r_grid.get_resolution();
	for (int z = 0; z < resolution.z; z++) {
		for (int y = 0; y < resolution.y; y++) {
			for (int x = 0; x < resolution.x; x++) {
				if (x == 0 || y == 0 || z == 0 || x == resolution.x - 1 || y == resolution.y - 1 || z == resolution.z - 1) {
					r_grid.set_occupancy(Vector3i(x, y, z), p_albedo);
				}
			}
		}
	}
}

static real_t radiance_energy(const LocalLRTBuilder::SH2RGB &p_radiance) {
	return p_radiance.r.length() + p_radiance.g.length() + p_radiance.b.length();
}

TEST_CASE("[LocalLRTBuilder] Empty grid and wall fixtures build local data") {
	LocalLRTBuilder empty_grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	const LocalLRTBuilder::Probe &empty_probe = empty_grid.get_probe(Vector3i(2, 2, 2));
	CHECK(empty_probe.local_visibility.is_equal_approx(encode_constant(1.0)));
	CHECK(empty_probe.empty_space_transmission == doctest::Approx(1.0));
	CHECK(empty_probe.local_transfer.r.rows[0] == Vector4());

	LocalLRTBuilder white_wall(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(white_wall, 3, Color(0.8, 0.8, 0.8));
	white_wall.build_local_data();
	const LocalLRTBuilder::Probe &white_probe = white_wall.get_probe(Vector3i(2, 2, 2));
	CHECK(white_probe.local_transfer.r.rows[0].x > 0.0);
	CHECK(white_probe.local_transfer.r.rows[0].x == doctest::Approx(white_probe.local_transfer.g.rows[0].x));
	CHECK(evaluate(white_probe.local_visibility, Vector3(1, 0, 0)) < evaluate(white_probe.local_visibility, Vector3(-1, 0, 0)));

	LocalLRTBuilder red_wall(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(red_wall, 3, Color(0.8, 0.1, 0.1));
	red_wall.build_local_data();
	const LocalLRTBuilder::Probe &red_probe = red_wall.get_probe(Vector3i(2, 2, 2));
	CHECK(red_probe.local_transfer.r.rows[0].x > red_probe.local_transfer.g.rows[0].x);
	CHECK(red_probe.local_transfer.g.rows[0].x == doctest::Approx(red_probe.local_transfer.b.rows[0].x));
}

TEST_CASE("[LocalLRTBuilder] Corner closed box and open box preserve directional visibility") {
	LocalLRTBuilder corner(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(corner, 3, Color(1, 0, 0));
	for (int z = 0; z < 5; z++) {
		for (int x = 0; x < 5; x++) {
			corner.set_occupancy(Vector3i(x, 3, z), Color(1, 1, 1));
		}
	}
	corner.build_local_data();
	const Vector4 corner_visibility = corner.get_probe(Vector3i(2, 2, 2)).local_visibility;
	CHECK(evaluate(corner_visibility, Vector3(-1, -1, 0).normalized()) > evaluate(corner_visibility, Vector3(1, 1, 0).normalized()));

	LocalLRTBuilder closed_box(Vector3(2, 2, 2), Vector3i(3, 3, 3));
	set_closed_box(closed_box, Color(1, 1, 1));
	closed_box.build_local_data();
	CHECK(closed_box.get_probe(Vector3i(1, 1, 1)).local_visibility == Vector4());

	LocalLRTBuilder open_box(Vector3(2, 2, 2), Vector3i(3, 3, 3));
	set_closed_box(open_box, Color(1, 1, 1));
	open_box.get_probe(Vector3i(2, 1, 1)).occupied = false;
	open_box.build_local_data();
	const Vector4 open_visibility = open_box.get_probe(Vector3i(1, 1, 1)).local_visibility;
	CHECK(evaluate(open_visibility, Vector3(1, 0, 0)) > evaluate(open_visibility, Vector3(-1, 0, 0)));
}

TEST_CASE("[LocalLRTBuilder] Directional omni and spot lights inject in local space") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	const Vector3i center(2, 2, 2);

	LocalLRTBuilder::DirectionalLight directional;
	directional.direction_to_light = Vector3(1, 0, 0);
	grid.inject_directional_light(directional);
	CHECK(evaluate(grid.get_probe(center).injection.r, Vector3(1, 0, 0)) > evaluate(grid.get_probe(center).injection.r, Vector3(-1, 0, 0)));

	grid.clear_injection();
	LocalLRTBuilder::OmniLight omni;
	omni.position = Vector3(1, 0, 0);
	omni.range = 3.0;
	grid.inject_omni_light(omni);
	CHECK(grid.get_probe(center).injection.r.length() > 0.0);
	CHECK(grid.get_probe(Vector3i(0, 2, 2)).injection.r == Vector4());

	grid.clear_injection();
	LocalLRTBuilder::SpotLight spot;
	spot.position = Vector3(-2, 0, 0);
	spot.direction = Vector3(1, 0, 0);
	spot.range = 5.0;
	spot.angle = Math::PI / 6.0;
	grid.inject_spot_light(spot);
	CHECK(grid.get_probe(center).injection.r.length() > 0.0);
	CHECK(grid.get_probe(Vector3i(0, 4, 2)).injection.r == Vector4());
}

TEST_CASE("[LocalLRTBuilder] Static occupancy shadows analytic light injection") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(grid, 2, Color(0.8, 0.8, 0.8));
	grid.build_local_data();
	const Vector3i lit_probe(1, 2, 2);
	const Vector3i shadowed_probe(3, 2, 2);

	LocalLRTBuilder::DirectionalLight directional;
	directional.direction_to_light = Vector3(-1, 0, 0);
	grid.inject_directional_light(directional);
	CHECK(grid.get_probe(lit_probe).injection.r.length() > 0.0);
	CHECK(grid.get_probe(shadowed_probe).injection.r == Vector4());

	grid.clear_injection();
	LocalLRTBuilder::OmniLight omni;
	omni.position = Vector3(-2, 0, 0);
	omni.range = 5.0;
	grid.inject_omni_light(omni);
	CHECK(grid.get_probe(lit_probe).injection.r.length() > 0.0);
	CHECK(grid.get_probe(shadowed_probe).injection.r == Vector4());

	grid.clear_injection();
	LocalLRTBuilder::SpotLight spot;
	spot.position = Vector3(-2, 0, 0);
	spot.direction = Vector3(1, 0, 0);
	spot.range = 5.0;
	spot.angle = Math::PI / 6.0;
	grid.inject_spot_light(spot);
	CHECK(grid.get_probe(lit_probe).injection.r.length() > 0.0);
	CHECK(grid.get_probe(shadowed_probe).injection.r == Vector4());
}

TEST_CASE("[LocalLRTBuilder] Visibility and radiance use ping-pong 26-neighbor propagation") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(grid, 3, Color(0.8, 0.8, 0.8));
	grid.build_local_data();
	const Vector4 local_visibility = grid.get_probe(Vector3i(1, 2, 2)).local_visibility;
	grid.propagate_global_visibility(2);
	CHECK(!grid.get_probe(Vector3i(1, 2, 2)).global_visibility.is_equal_approx(local_visibility));

	grid.clear_occupancy();
	LocalLRTBuilder::OmniLight light;
	light.position = grid.get_probe_world_position(Vector3i(1, 2, 2));
	light.range = 1.1;
	grid.inject_omni_light(light);
	grid.propagate_radiance(2);
	CHECK(radiance_energy(grid.get_probe(Vector3i(3, 2, 2)).radiance) > 0.0);
}

TEST_CASE("[LocalLRTBuilder] A full wall blocks point-light propagation") {
	LocalLRTBuilder open_grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	LocalLRTBuilder::OmniLight light;
	light.position = open_grid.get_probe_world_position(Vector3i(1, 2, 2));
	light.range = 1.1;
	open_grid.inject_omni_light(light);
	open_grid.propagate_radiance(2);
	const real_t open_energy = radiance_energy(open_grid.get_probe(Vector3i(3, 2, 2)).radiance);

	LocalLRTBuilder divided_grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(divided_grid, 2, Color(0.8, 0.8, 0.8));
	divided_grid.build_local_data();
	light.position = divided_grid.get_probe_world_position(Vector3i(1, 2, 2));
	divided_grid.inject_omni_light(light);
	divided_grid.propagate_radiance(2);
	const real_t divided_energy = radiance_energy(divided_grid.get_probe(Vector3i(3, 2, 2)).radiance);

	CHECK(open_energy > 0.0);
	CHECK(divided_energy < open_energy * 0.01);
}

TEST_CASE("[LocalLRTBuilder] Global visibility shadows directional injection") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(grid, 2, Color(0.8, 0.8, 0.8));
	grid.build_local_data();
	grid.propagate_global_visibility(8);

	LocalLRTBuilder::DirectionalLight light;
	light.direction_to_light = Vector3(-1, 0, 0);
	grid.inject_directional_light(light);
	grid.propagate_radiance(1);

	const real_t lit_energy = radiance_energy(grid.get_probe(Vector3i(1, 2, 2)).radiance);
	const real_t shadowed_energy = radiance_energy(grid.get_probe(Vector3i(3, 2, 2)).radiance);
	CHECK(lit_energy > 0.0);
	CHECK(shadowed_energy < lit_energy);
}

TEST_CASE("[LocalLRTBuilder] Red wall bleeding dominates green and remains stable") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(grid, 3, Color(0.8, 0.1, 0.1));
	grid.build_local_data();

	LocalLRTBuilder::DirectionalLight light;
	light.direction_to_light = Vector3(-1, 0, 0);
	grid.inject_directional_light(light);
	grid.propagate_radiance(8);
	const LocalLRTBuilder::SH2RGB radiance = grid.get_probe(Vector3i(2, 2, 2)).radiance;
	CHECK(radiance.r.length() > radiance.g.length());
	CHECK(radiance.g.length() == doctest::Approx(radiance.b.length()));

	const real_t energy_with_light = radiance_energy(radiance);
	grid.clear_injection();
	grid.propagate_radiance(16);
	const real_t decayed_energy = radiance_energy(grid.get_probe(Vector3i(2, 2, 2)).radiance);
	CHECK(decayed_energy < energy_with_light);
	CHECK(Math::is_finite(decayed_energy));
}

TEST_CASE("[LocalLRTBuilder] Rotating the whole grid preserves local-space reference") {
	LocalLRTBuilder local_grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	LocalLRTBuilder::OmniLight local_light;
	local_light.position = Vector3(1, 0, 0);
	local_light.range = 3.0;
	local_grid.inject_omni_light(local_light);
	local_grid.propagate_radiance(4);

	const Transform3D rotated_transform(Basis(Vector3(0, 1, 0), Math::PI / 2.0), Vector3(7, 2, -3));
	LocalLRTBuilder rotated_grid(Vector3(4, 4, 4), Vector3i(5, 5, 5), rotated_transform);
	LocalLRTBuilder::OmniLight rotated_light;
	rotated_light.position = rotated_transform.xform(Vector3(1, 0, 0));
	rotated_light.range = 3.0;
	rotated_grid.inject_omni_light(rotated_light);
	rotated_grid.propagate_radiance(4);

	const LocalLRTBuilder::SH2RGB local_radiance = local_grid.get_probe(Vector3i(2, 2, 2)).radiance;
	const LocalLRTBuilder::SH2RGB rotated_radiance = rotated_grid.get_probe(Vector3i(2, 2, 2)).radiance;
	CHECK(local_radiance.r.is_equal_approx(rotated_radiance.r));
	CHECK(local_radiance.g.is_equal_approx(rotated_radiance.g));
	CHECK(local_radiance.b.is_equal_approx(rotated_radiance.b));
}

TEST_CASE("[LocalLRTBuilder] Canonical red-wall values remain a GPU golden reference") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(grid, 3, Color(0.8, 0.1, 0.1));
	grid.build_local_data();
	LocalLRTBuilder::DirectionalLight light;
	light.direction_to_light = Vector3(-1, 0, 0);
	grid.inject_directional_light(light);
	grid.propagate_global_visibility(4);
	grid.propagate_radiance(4);

	const LocalLRTBuilder::Probe &probe = grid.get_probe(Vector3i(2, 2, 2));
	CHECK(probe.global_visibility.x == doctest::Approx(1.06501).epsilon(0.0001));
	CHECK(probe.radiance.r.x == doctest::Approx(2.53388).epsilon(0.0001));
	CHECK(probe.radiance.g.x == doctest::Approx(2.18537).epsilon(0.0001));
}

} // namespace TestLocalLRTBuilder
