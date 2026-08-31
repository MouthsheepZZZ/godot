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
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/mesh.h"

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
	const Vector4 reflected = red_probe.local_transfer.r.xform(encode_direction(Vector3(-1, 0, 0), 1.0, Math::TAU));
	CHECK(reflected.x > 0.0);
	CHECK(evaluate(reflected, Vector3(1, 0, 0)) > evaluate(reflected, Vector3(-1, 0, 0)));
}

TEST_CASE("[LocalLRTBuilder] White diffuse transfer preserves bounded energy") {
	LocalLRTBuilder closed_box(Vector3(2, 2, 2), Vector3i(3, 3, 3));
	set_closed_box(closed_box, Color(0.8, 0.8, 0.8));
	closed_box.build_local_data();

	const Vector4 unit_radiance = encode_constant(1.0);
	const LocalLRTBuilder::Probe &probe = closed_box.get_probe(Vector3i(1, 1, 1));
	const Vector4 reflected = probe.local_transfer.r.xform(unit_radiance);
	CHECK(reflected.is_equal_approx(encode_constant(0.8)));
	CHECK(reflected.x <= unit_radiance.x);
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
	open_box.get_probe(Vector3i(2, 1, 1)).inside_solid = false;
	open_box.build_local_data();
	const Vector4 open_visibility = open_box.get_probe(Vector3i(1, 1, 1)).local_visibility;
	CHECK(evaluate(open_visibility, Vector3(1, 0, 0)) > evaluate(open_visibility, Vector3(-1, 0, 0)));
}

TEST_CASE("[LocalLRTBuilder] Directional omni spot and area lights inject in local space") {
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
	omni.attenuation = 2.0;
	grid.inject_omni_light(omni);
	CHECK(grid.get_probe(center).injection.r.length() > 0.0);
	const real_t omni_range_window = Math::pow(1.0 - Math::pow(1.0 / omni.range, 4.0), 2.0);
	CHECK(grid.get_probe(center).injection.r.is_equal_approx(encode_direction(Vector3(1, 0, 0), omni_range_window * 0.5, Math::TAU)));
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

	grid.clear_injection();
	LocalLRTBuilder::AreaLight area;
	area.position = Vector3(0, 1, 0);
	area.direction = Vector3(0, -1, 0);
	area.width = Vector3(1, 0, 0);
	area.height = Vector3(0, 0, 1);
	area.range = 4.0;
	area.attenuation = 2.0;
	grid.inject_area_light(area);
	const Vector4 area_injection = grid.get_probe(center).injection.r;
	CHECK(area_injection.length() > 0.0);
	CHECK(area_injection.z > 0.0);
	CHECK(Math::is_zero_approx(area_injection.y));
	CHECK(Math::is_zero_approx(area_injection.w));
	CHECK(grid.get_probe(Vector3i(2, 4, 2)).injection.r == Vector4());
}

TEST_CASE("[LocalLRTBuilder] Area light energy normalization and scaling are stable") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	const Vector3i center(2, 2, 2);
	LocalLRTBuilder::AreaLight light;
	light.position = Vector3(0, 1.5, 0);
	light.direction = Vector3(0, -1, 0);
	light.width = Vector3(0.5, 0, 0);
	light.height = Vector3(0, 0, 0.5);
	light.range = 8.0;
	light.attenuation = 2.0;
	grid.inject_area_light(light);
	const Vector4 normalized_small = grid.get_probe(center).injection.r;

	grid.clear_injection();
	light.energy = 0.5;
	grid.inject_area_light(light);
	CHECK(grid.get_probe(center).injection.r.is_equal_approx(normalized_small * 0.5));

	grid.clear_injection();
	light.energy = 1.0;
	light.normalize_energy = false;
	grid.inject_area_light(light);
	CHECK(grid.get_probe(center).injection.r.is_equal_approx(normalized_small * 0.25));

	grid.clear_injection();
	light.direction = Vector3(0, 1, 0);
	grid.inject_area_light(light);
	CHECK(grid.get_probe(center).injection.r == Vector4());
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
	CHECK(radiance_energy(grid.get_probe(Vector3i(3, 2, 2)).radiance) == doctest::Approx(0.0));
}

TEST_CASE("[LocalLRTBuilder] Analytic injection becomes radiance only through local transfer") {
	LocalLRTBuilder open_grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	LocalLRTBuilder::OmniLight light;
	light.position = open_grid.get_probe_world_position(Vector3i(1, 2, 2));
	light.range = 1.1;
	open_grid.inject_omni_light(light);
	open_grid.propagate_radiance(2);
	CHECK(radiance_energy(open_grid.get_probe(Vector3i(1, 2, 2)).radiance) == doctest::Approx(0.0));

	LocalLRTBuilder wall_grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(wall_grid, 2, Color(0.8, 0.8, 0.8));
	wall_grid.build_local_data();
	light.position = wall_grid.get_probe_world_position(Vector3i(1, 2, 2));
	wall_grid.inject_omni_light(light);
	wall_grid.propagate_radiance(1);
	CHECK(radiance_energy(wall_grid.get_probe(Vector3i(1, 2, 2)).radiance) > 0.0);
	CHECK(radiance_energy(wall_grid.get_probe(Vector3i(3, 2, 2)).radiance) == doctest::Approx(0.0));
}

TEST_CASE("[LocalLRTBuilder] Analytic injection does not use global visibility as a shadow map") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	set_x_wall(grid, 2, Color(0.8, 0.8, 0.8));
	grid.build_local_data();
	const Vector3i lit_probe(1, 2, 2);
	const Vector3i shadowed_probe(3, 2, 2);

	LocalLRTBuilder::DirectionalLight directional;
	directional.direction_to_light = Vector3(-1, 0, 0);
	grid.propagate_global_visibility(8);
	grid.inject_directional_light(directional);
	CHECK(grid.get_probe(lit_probe).injection.r.is_equal_approx(grid.get_probe(shadowed_probe).injection.r));

	grid.clear_injection();
	LocalLRTBuilder::OmniLight omni;
	omni.position = Vector3(-2, 0, 0);
	omni.range = 5.0;
	grid.inject_omni_light(omni);
	const Vector4 omni_injection = grid.get_probe(lit_probe).injection.r;
	grid.clear_injection();
	grid.reset_global_visibility();
	grid.inject_omni_light(omni);
	CHECK(grid.get_probe(lit_probe).injection.r.is_equal_approx(omni_injection));

	grid.clear_injection();
	LocalLRTBuilder::SpotLight spot;
	spot.position = Vector3(-2, 0, 0);
	spot.direction = Vector3(1, 0, 0);
	spot.range = 5.0;
	spot.angle = Math::PI / 6.0;
	grid.inject_spot_light(spot);
	CHECK(grid.get_probe(lit_probe).injection.r.length() > 0.0);
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
	grid.reset_radiance();
	grid.propagate_radiance(16);
	const real_t energy_after_16 = radiance_energy(grid.get_probe(Vector3i(2, 2, 2)).radiance);
	grid.propagate_radiance(48);
	const real_t energy_after_64 = radiance_energy(grid.get_probe(Vector3i(2, 2, 2)).radiance);
	CHECK(energy_after_16 >= energy_with_light * 0.95);
	CHECK(energy_after_64 <= energy_after_16 * 1.1);
	CHECK(Math::is_finite(energy_after_64));
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
	grid.propagate_global_visibility(4);
	LocalLRTBuilder::DirectionalLight light;
	light.direction_to_light = Vector3(-1, 0, 0);
	grid.inject_directional_light(light);
	grid.propagate_radiance(4);

	const LocalLRTBuilder::Probe &probe = grid.get_probe(Vector3i(2, 2, 2));
	CHECK(probe.global_visibility.x == doctest::Approx(1.06501).epsilon(0.0001));
	CHECK(probe.radiance.r.x == doctest::Approx(0.790726).epsilon(0.0001));
	CHECK(probe.radiance.g.x == doctest::Approx(0.0901755).epsilon(0.0001));
}

static void rasterize_plane(LocalLRTBuilder &r_grid, const Vector3 &p_origin, const Vector3 &p_u, const Vector3 &p_v, const Color &p_albedo, int p_segments = 1) {
	const int segments = MAX(p_segments, 1);
	for (int j = 0; j < segments; j++) {
		for (int i = 0; i < segments; i++) {
			const real_t u0 = (i * 2.0 / segments) - 1.0;
			const real_t u1 = ((i + 1) * 2.0 / segments) - 1.0;
			const real_t v0 = (j * 2.0 / segments) - 1.0;
			const real_t v1 = ((j + 1) * 2.0 / segments) - 1.0;
			const Vector3 a = p_origin + p_u * u0 + p_v * v0;
			const Vector3 b = p_origin + p_u * u1 + p_v * v0;
			const Vector3 c = p_origin + p_u * u1 + p_v * v1;
			const Vector3 d = p_origin + p_u * u0 + p_v * v1;
			r_grid.rasterize_triangle(a, b, c, p_albedo);
			r_grid.rasterize_triangle(a, c, d, p_albedo);
		}
	}
}

static void rasterize_mesh(LocalLRTBuilder &r_grid, const Ref<Mesh> &p_mesh, const Color &p_albedo) {
	const Array arrays = p_mesh->surface_get_arrays(0);
	const Vector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
	const Vector<int> indices = arrays[Mesh::ARRAY_INDEX];
	const int triangle_vertex_count = indices.is_empty() ? vertices.size() : indices.size();
	for (int index = 0; index + 2 < triangle_vertex_count; index += 3) {
		Vector3 triangle[3];
		for (int vertex = 0; vertex < 3; vertex++) {
			const int vertex_index = indices.is_empty() ? index + vertex : indices[index + vertex];
			triangle[vertex] = vertices[vertex_index];
		}
		r_grid.rasterize_triangle(triangle[0], triangle[1], triangle[2], p_albedo);
	}
}

struct SurfaceFieldStats {
	real_t total_coverage_area = 0.0;
	real_t coverage_weighted_distance = 0.0;
	real_t coverage_weighted_alignment = 0.0;
	real_t total_coverage = 0.0;
	real_t total_transfer_area_energy = 0.0;
	int occupied_count = 0;
	int layer_span = 0;
};

static SurfaceFieldStats measure_plane_field(const LocalLRTBuilder &p_grid, const Vector3 &p_origin, const Vector3 &p_normal, int p_axis) {
	SurfaceFieldStats stats;
	const Vector3 spacing = actual_probe_spacing(p_grid.get_size(), p_grid.get_resolution());
	const Vector3 abs_normal = p_normal.abs();
	const real_t cell_area = abs_normal.x >= abs_normal.y && abs_normal.x >= abs_normal.z ? spacing.y * spacing.z : (abs_normal.y >= abs_normal.z ? spacing.x * spacing.z : spacing.x * spacing.y);
	int min_layer = p_grid.get_resolution()[p_axis];
	int max_layer = -1;
	for (int z = 0; z < p_grid.get_resolution().z; z++) {
		for (int y = 0; y < p_grid.get_resolution().y; y++) {
			for (int x = 0; x < p_grid.get_resolution().x; x++) {
				const Vector3i position(x, y, z);
				const LocalLRTBuilder::Probe &probe = p_grid.get_probe(position);
				if (probe.occupied) {
					stats.occupied_count++;
					stats.total_coverage += probe.occupancy();
					stats.total_coverage_area += probe.occupancy() * cell_area;
					stats.coverage_weighted_distance += probe.occupancy() * Math::abs(p_normal.dot(p_grid.get_probe_local_position(position) - p_origin));
					if (probe.surface_normal.length_squared() > CMP_EPSILON) {
						stats.coverage_weighted_alignment += probe.occupancy() * Math::abs(probe.surface_normal.normalized().dot(p_normal));
					}
					min_layer = MIN(min_layer, position[p_axis]);
					max_layer = MAX(max_layer, position[p_axis]);
				} else {
					const Vector4 reflected = probe.local_transfer.r.xform(encode_constant(1.0));
					stats.total_transfer_area_energy += Math::abs(reflected.x) * cell_area;
				}
			}
		}
	}
	if (stats.total_coverage > 0.0) {
		stats.coverage_weighted_distance /= stats.total_coverage;
		stats.coverage_weighted_alignment /= stats.total_coverage;
	}
	stats.layer_span = max_layer >= min_layer ? max_layer - min_layer + 1 : 0;
	return stats;
}

TEST_CASE("[LocalLRTBuilder] Overlapping triangles merge coverage albedo and normals") {
	LocalLRTBuilder grid(Vector3(2, 2, 2), Vector3i(3, 3, 3));
	rasterize_plane(grid, Vector3(), Vector3(0.4, 0, 0), Vector3(0, 0, 0.4), Color(1, 0, 0));
	rasterize_plane(grid, Vector3(), Vector3(0.4, 0, 0), Vector3(0, 0, 0.4), Color(0, 1, 0));
	grid.build_local_data();

	const LocalLRTBuilder::Probe &center = grid.get_probe(Vector3i(1, 1, 1));
	CHECK(center.occupied);
	CHECK(center.coverage >= 0.5);
	CHECK(center.coverage <= 1.0);
	CHECK(center.albedo.r == doctest::Approx(0.5).epsilon(0.05));
	CHECK(center.albedo.g == doctest::Approx(0.5).epsilon(0.05));
	CHECK(Math::abs(center.surface_normal.normalized().dot(Vector3(0, 1, 0))) > 0.9);
}

TEST_CASE("[LocalLRTBuilder] Plane coverage is invariant to grid phase") {
	const Vector3 size(8, 8, 8);
	const Vector3i resolution = probe_resolution(size, 1.0);
	const Vector3 normal(0, 1, 0);
	const real_t expected_area = 4.0;
	const real_t offsets[] = { 0.0, 0.25, 0.5, 0.75 };
	real_t first_area = -1.0;
	for (real_t offset : offsets) {
		LocalLRTBuilder grid(size, resolution);
		rasterize_plane(grid, Vector3(0, offset, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), Color(0.8, 0.8, 0.8));
		grid.build_local_data();
		const SurfaceFieldStats stats = measure_plane_field(grid, Vector3(0, offset, 0), normal, 1);
		CHECK(stats.occupied_count > 0);
		CHECK(stats.layer_span <= 2);
		CHECK(stats.total_coverage_area == doctest::Approx(expected_area).epsilon(0.25));
		if (first_area < 0.0) {
			first_area = stats.total_coverage_area;
		} else {
			CHECK(stats.total_coverage_area == doctest::Approx(first_area).epsilon(0.15));
		}
	}
}

TEST_CASE("[LocalLRTBuilder] Plane coverage is invariant to triangle subdivision") {
	const Vector3 size(8, 8, 8);
	const Vector3i resolution = probe_resolution(size, 1.0);
	LocalLRTBuilder coarse(size, resolution);
	rasterize_plane(coarse, Vector3(), Vector3(1, 0, 0), Vector3(0, 0, 1), Color(0.8, 0.8, 0.8));
	coarse.build_local_data();
	const SurfaceFieldStats coarse_stats = measure_plane_field(coarse, Vector3(), Vector3(0, 1, 0), 1);

	LocalLRTBuilder fine(size, resolution);
	rasterize_plane(fine, Vector3(), Vector3(1, 0, 0), Vector3(0, 0, 1), Color(0.8, 0.8, 0.8), 8);
	fine.build_local_data();
	const SurfaceFieldStats fine_stats = measure_plane_field(fine, Vector3(), Vector3(0, 1, 0), 1);

	CHECK(coarse_stats.occupied_count > 0);
	CHECK(fine_stats.occupied_count == coarse_stats.occupied_count);
	CHECK(fine_stats.total_coverage_area == doctest::Approx(coarse_stats.total_coverage_area).epsilon(0.15));
	CHECK(fine_stats.total_coverage_area == doctest::Approx(4.0).epsilon(0.25));
}

TEST_CASE("[LocalLRTBuilder] Plane coverage stays stable across probe spacing") {
	const Vector3 size(8, 8, 8);
	const Vector3 origin;
	const Vector3 normal(0, 1, 0);
	const real_t expected_area = 4.0;
	SurfaceFieldStats previous;
	const real_t spacings[] = { 1.0, 0.5, 0.25 };
	for (real_t spacing : spacings) {
		LocalLRTBuilder grid(size, probe_resolution(size, spacing));
		rasterize_plane(grid, origin, Vector3(1, 0, 0), Vector3(0, 0, 1), Color(0.8, 0.8, 0.8));
		grid.build_local_data();
		const SurfaceFieldStats stats = measure_plane_field(grid, origin, normal, 1);
		CHECK(stats.occupied_count > 0);
		CHECK(stats.layer_span <= 2);
		CHECK(stats.coverage_weighted_distance < spacing);
		CHECK(stats.total_coverage_area == doctest::Approx(expected_area).epsilon(0.35));
		CHECK(stats.coverage_weighted_alignment > 0.95);
		if (previous.occupied_count > 0) {
			CHECK(stats.occupied_count > previous.occupied_count);
			CHECK(stats.coverage_weighted_distance <= previous.coverage_weighted_distance + spacing * 0.25);
			CHECK(Math::abs(stats.total_coverage_area - previous.total_coverage_area) <= 0.35 * expected_area);
			CHECK(Math::abs(stats.total_transfer_area_energy - previous.total_transfer_area_energy) <= 0.35 * MAX(previous.total_transfer_area_energy, stats.total_transfer_area_energy));
		}
		previous = stats;
	}
}

TEST_CASE("[LocalLRTBuilder] Slanted plane and sphere refine toward high-resolution reference") {
	const Vector3 size(8, 8, 8);
	const Vector3 slant_normal = Vector3(0, 1, 1).normalized();
	const Vector3 slant_u(1, 0, 0);
	const Vector3 slant_v = slant_normal.cross(slant_u).normalized();
	SurfaceFieldStats previous_plane;
	real_t previous_sphere_distance = -1.0;
	real_t previous_sphere_alignment = -1.0;
	const real_t spacings[] = { 1.0, 0.5, 0.25 };

	for (real_t spacing : spacings) {
		LocalLRTBuilder plane(size, probe_resolution(size, spacing));
		rasterize_plane(plane, Vector3(), slant_u, slant_v, Color(1, 1, 1));
		plane.build_local_data();
		const SurfaceFieldStats plane_stats = measure_plane_field(plane, Vector3(), slant_normal, 1);
		CHECK(plane_stats.occupied_count > 0);
		CHECK(plane_stats.coverage_weighted_alignment > 0.85);
		if (previous_plane.occupied_count > 0) {
			CHECK(plane_stats.occupied_count >= previous_plane.occupied_count);
			CHECK(plane_stats.coverage_weighted_distance <= previous_plane.coverage_weighted_distance + spacing * 0.15);
			CHECK(plane_stats.coverage_weighted_alignment >= previous_plane.coverage_weighted_alignment - 0.05);
		}
		previous_plane = plane_stats;

		LocalLRTBuilder sphere_grid(size, probe_resolution(size, spacing));
		Ref<SphereMesh> sphere;
		sphere.instantiate();
		sphere->set_radius(1.0);
		sphere->set_height(2.0);
		sphere->set_radial_segments(16);
		sphere->set_rings(8);
		rasterize_mesh(sphere_grid, sphere, Color(1, 1, 1));
		sphere_grid.build_local_data();

		real_t coverage_sum = 0.0;
		real_t distance_sum = 0.0;
		real_t alignment_sum = 0.0;
		int occupied = 0;
		for (int z = 0; z < sphere_grid.get_resolution().z; z++) {
			for (int y = 0; y < sphere_grid.get_resolution().y; y++) {
				for (int x = 0; x < sphere_grid.get_resolution().x; x++) {
					const LocalLRTBuilder::Probe &probe = sphere_grid.get_probe(Vector3i(x, y, z));
					if (!probe.occupied) {
						continue;
					}
					occupied++;
					const Vector3 local = sphere_grid.get_probe_local_position(Vector3i(x, y, z));
					const real_t radius = local.length();
					coverage_sum += probe.coverage;
					distance_sum += probe.coverage * Math::abs(radius - 1.0);
					if (radius > CMP_EPSILON && probe.surface_normal.length_squared() > CMP_EPSILON) {
						alignment_sum += probe.coverage * Math::abs(probe.surface_normal.normalized().dot(local / radius));
					}
				}
			}
		}
		REQUIRE(occupied > 0);
		REQUIRE(coverage_sum > 0.0);
		const real_t mean_distance = distance_sum / coverage_sum;
		const real_t mean_alignment = alignment_sum / coverage_sum;
		CHECK(mean_alignment > 0.7);
		if (previous_sphere_distance >= 0.0) {
			CHECK(mean_distance <= previous_sphere_distance + spacing * 0.15);
			CHECK(mean_alignment >= previous_sphere_alignment - 0.05);
		}
		previous_sphere_distance = mean_distance;
		previous_sphere_alignment = mean_alignment;
	}
}

struct PropagationStages {
	real_t coverage_area = 0.0;
	real_t transfer_energy = 0.0;
	real_t first_bounce_energy = 0.0;
	real_t neighbor_propagation_energy = 0.0;
	real_t converged_energy = 0.0;
	int occupied_count = 0;
	int converged_iterations = 0;
	bool residual_converged = false;
};

static void check_error_refines(real_t p_coarse, real_t p_medium, real_t p_fine, real_t p_reference) {
	const real_t slack = MAX((real_t)1e-4, (real_t)0.02 * Math::abs(p_reference));
	const real_t coarse_error = Math::abs(p_coarse - p_reference);
	const real_t medium_error = Math::abs(p_medium - p_reference);
	const real_t fine_error = Math::abs(p_fine - p_reference);
	CHECK(medium_error <= coarse_error + slack);
	CHECK(fine_error <= medium_error + slack);
}

static PropagationStages capture_propagation_stages(LocalLRTBuilder &r_grid) {
	PropagationStages stages;
	const Vector3 spacing = actual_probe_spacing(r_grid.get_size(), r_grid.get_resolution());
	const real_t cell_area = spacing.y * spacing.z;
	Vector3i adjacent_probe;
	real_t adjacent_transfer = 0.0;
	for (int z = 0; z < r_grid.get_resolution().z; z++) {
		for (int y = 0; y < r_grid.get_resolution().y; y++) {
			for (int x = 0; x < r_grid.get_resolution().x; x++) {
				const Vector3i position(x, y, z);
				const LocalLRTBuilder::Probe &probe = r_grid.get_probe(position);
				if (probe.occupied) {
					stages.occupied_count++;
					stages.coverage_area += probe.occupancy() * cell_area;
				} else {
					const real_t transfer = probe.local_transfer.r.xform(encode_constant(1.0)).x;
					stages.transfer_energy += transfer * cell_area;
					if (transfer > adjacent_transfer) {
						adjacent_transfer = transfer;
						adjacent_probe = position;
					}
				}
			}
		}
	}

	LocalLRTBuilder::DirectionalLight light;
	light.direction_to_light = Vector3(-1, 0, 0);
	r_grid.inject_directional_light(light);
	r_grid.propagate_radiance(1);
	stages.first_bounce_energy = radiance_energy(r_grid.get_probe(adjacent_probe).radiance);
	r_grid.propagate_radiance(1);
	stages.neighbor_propagation_energy = radiance_energy(r_grid.get_probe(adjacent_probe).radiance);

	const real_t min_spacing = MIN(spacing.x, MIN(spacing.y, spacing.z));
	const int min_hops = MAX(8, (int)Math::ceil(r_grid.get_size().length() * 0.5 / min_spacing) + 4);
	const int max_iterations = 256;
	real_t previous = stages.neighbor_propagation_energy;
	for (int iteration = 2; iteration < max_iterations; iteration++) {
		r_grid.propagate_radiance(1);
		const real_t energy = radiance_energy(r_grid.get_probe(adjacent_probe).radiance);
		const real_t residual = Math::abs(energy - previous);
		stages.converged_energy = energy;
		stages.converged_iterations = iteration + 1;
		const bool reached_sample = energy > 0.0 && iteration + 1 >= min_hops;
		if (reached_sample && residual <= MAX((real_t)1e-6, (real_t)0.001 * MAX(energy, previous))) {
			stages.residual_converged = true;
			break;
		}
		previous = energy;
	}
	return stages;
}

static void check_spacing_refinement(const PropagationStages &p_coarse, const PropagationStages &p_medium, const PropagationStages &p_fine, const PropagationStages &p_reference) {
	CHECK(p_medium.occupied_count >= p_coarse.occupied_count);
	CHECK(p_fine.occupied_count >= p_medium.occupied_count);
	CHECK(p_fine.coverage_area == doctest::Approx(p_coarse.coverage_area).epsilon(0.25));
	CHECK(p_coarse.first_bounce_energy > 0.0);
	CHECK(p_medium.first_bounce_energy > 0.0);
	CHECK(p_fine.first_bounce_energy > 0.0);
	CHECK(p_coarse.neighbor_propagation_energy >= p_coarse.first_bounce_energy * 0.9);
	CHECK(p_medium.neighbor_propagation_energy >= p_medium.first_bounce_energy * 0.9);
	CHECK(p_fine.neighbor_propagation_energy >= p_fine.first_bounce_energy * 0.9);
	CHECK(p_coarse.residual_converged);
	CHECK(p_medium.residual_converged);
	CHECK(p_fine.residual_converged);
	CHECK(p_reference.residual_converged);
	CHECK(p_reference.converged_energy > 0.0);
	check_error_refines(p_coarse.transfer_energy, p_medium.transfer_energy, p_fine.transfer_energy, p_reference.transfer_energy);
	check_error_refines(p_coarse.first_bounce_energy, p_medium.first_bounce_energy, p_fine.first_bounce_energy, p_reference.first_bounce_energy);
	check_error_refines(p_coarse.converged_energy, p_medium.converged_energy, p_fine.converged_energy, p_reference.converged_energy);
}

TEST_CASE("[LocalLRTBuilder] Spacing refinement records stages and does not reverse energy") {
	const Vector3 size(4, 4, 4);
	const real_t spacings[] = { 1.0, 0.5, 0.25, 0.125 };

	PropagationStages plane_stages[4];
	PropagationStages corner_stages[4];
	for (int i = 0; i < 4; i++) {
		const Vector3i resolution = probe_resolution(size, spacings[i]);
		LocalLRTBuilder plane(size, resolution);
		rasterize_plane(plane, Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), Color(0.8, 0.8, 0.8));
		plane.build_local_data();
		plane_stages[i] = capture_propagation_stages(plane);

		LocalLRTBuilder corner(size, resolution);
		rasterize_plane(corner, Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), Color(0.8, 0.1, 0.1));
		rasterize_plane(corner, Vector3(0, 1, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), Color(0.8, 0.8, 0.8));
		corner.build_local_data();
		corner_stages[i] = capture_propagation_stages(corner);

		CHECK(plane_stages[i].coverage_area > 0.0);
		CHECK(plane_stages[i].transfer_energy > 0.0);
		CHECK(plane_stages[i].first_bounce_energy > 0.0);
		CHECK(plane_stages[i].neighbor_propagation_energy > 0.0);
		CHECK(plane_stages[i].converged_energy > 0.0);
		CHECK(corner_stages[i].first_bounce_energy > 0.0);
		CHECK(corner_stages[i].neighbor_propagation_energy > 0.0);
		CHECK(corner_stages[i].converged_energy > plane_stages[i].converged_energy * 0.2);
	}

	check_spacing_refinement(plane_stages[0], plane_stages[1], plane_stages[2], plane_stages[3]);
	check_spacing_refinement(corner_stages[0], corner_stages[1], corner_stages[2], corner_stages[3]);
}

TEST_CASE("[LocalLRTBuilder] Color SDF box separates inside_solid from surface LTM") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	const Color albedo(0.8, 0.1, 0.05);
	const Color emission(0.4, 0.1, 0.02);
	grid.add_geometry_source(LocalLRTColorSDF::make_box(Vector3(0.4, 0.4, 0.4), 0.125, albedo, emission), Transform3D());
	grid.build_local_data();

	const LocalLRTBuilder::Probe &center = grid.get_probe(Vector3i(2, 2, 2));
	CHECK(center.inside_solid);
	CHECK(center.occupied);
	CHECK(center.signed_distance < 0.0);
	CHECK(center.local_visibility == Vector4());
	CHECK(center.local_transfer.r.rows[0] == Vector4());

	const LocalLRTBuilder::Probe &adjacent = grid.get_probe(Vector3i(2, 2, 3));
	CHECK_FALSE(adjacent.inside_solid);
	CHECK(adjacent.local_transfer.r.rows[0].x > adjacent.local_transfer.g.rows[0].x);
	CHECK(adjacent.local_transfer.g.rows[0].x > adjacent.local_transfer.b.rows[0].x);
	CHECK(adjacent.local_transfer.b.rows[0].x > 0.0);
	CHECK_FALSE(adjacent.local_visibility.is_equal_approx(encode_constant(1.0)));

	const LocalLRTBuilder::Probe &corner = grid.get_probe(Vector3i(0, 0, 0));
	CHECK_FALSE(corner.inside_solid);
	CHECK(corner.local_visibility.is_equal_approx(encode_constant(1.0)));
	CHECK(corner.local_transfer.r.rows[0] == Vector4());
}

TEST_CASE("[LocalLRTBuilder] Color SDF uses volume-local distance and normal under scale") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	const Transform3D object_to_volume(Basis().scaled(Vector3(2, 1, 1)));
	grid.add_geometry_source(LocalLRTColorSDF::make_box(Vector3(0.5, 0.5, 0.5), 0.125, Color(0.8, 0.8, 0.8)), object_to_volume);
	grid.build_local_data();

	const LocalLRTBuilder::Probe &probe = grid.get_probe(Vector3i(4, 2, 2));
	CHECK(probe.signed_distance == doctest::Approx(1.0));
	CHECK(probe.surface_normal.is_equal_approx(Vector3(1, 0, 0)));
}

TEST_CASE("[LocalLRTBuilder] ColorToFill sends emission through LTM") {
	LocalLRTBuilder albedo_only(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	albedo_only.add_geometry_source(LocalLRTColorSDF::make_box(Vector3(0.4, 0.4, 0.4), 0.125, Color(0.5, 0.5, 0.5)), Transform3D());
	albedo_only.build_local_data();

	LocalLRTBuilder with_emission(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	with_emission.add_geometry_source(LocalLRTColorSDF::make_box(Vector3(0.4, 0.4, 0.4), 0.125, Color(0.5, 0.5, 0.5), Color(0.5, 0.0, 0.0)), Transform3D());
	with_emission.build_local_data();

	const Vector3i adjacent(2, 2, 3);
	CHECK(with_emission.get_probe(adjacent).local_transfer.r.rows[0].x > albedo_only.get_probe(adjacent).local_transfer.r.rows[0].x);
	CHECK(with_emission.get_probe(adjacent).local_transfer.g.rows[0].x == doctest::Approx(albedo_only.get_probe(adjacent).local_transfer.g.rows[0].x));
}

TEST_CASE("[LocalLRTBuilder] Overlapping Color SDF sources keep the nearer surface") {
	LocalLRTBuilder grid(Vector3(4, 4, 4), Vector3i(5, 5, 5));
	grid.add_geometry_source(LocalLRTColorSDF::make_box(Vector3(0.4, 0.4, 0.4), 0.125, Color(1, 0, 0)), Transform3D());
	grid.add_geometry_source(LocalLRTColorSDF::make_sphere(1.0, 0.125, Color(0, 1, 0)), Transform3D());
	grid.build_local_data();

	const LocalLRTBuilder::Probe &center = grid.get_probe(Vector3i(2, 2, 2));
	CHECK(center.inside_solid);
	CHECK(center.albedo.is_equal_approx(Color(0, 1, 0)));
}

static real_t interpolate_transfer_r(const LocalLRTBuilder &p_grid, const Vector3 &p_local) {
	const Vector3 grid = local_to_grid(p_local, p_grid.get_size(), p_grid.get_resolution());
	const Vector3i resolution = p_grid.get_resolution();
	const Vector3i base(
			CLAMP((int)Math::floor(grid.x), 0, resolution.x - 2),
			CLAMP((int)Math::floor(grid.y), 0, resolution.y - 2),
			CLAMP((int)Math::floor(grid.z), 0, resolution.z - 2));
	const Vector3 fraction = grid - Vector3(base);
	real_t weighted = 0.0;
	real_t weight_sum = 0.0;
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				const LocalLRTBuilder::Probe &probe = p_grid.get_probe(base + Vector3i(x, y, z));
				if (probe.inside_solid) {
					continue;
				}
				const real_t weight = (x ? fraction.x : 1.0 - fraction.x) * (y ? fraction.y : 1.0 - fraction.y) * (z ? fraction.z : 1.0 - fraction.z);
				weighted += weight * probe.local_transfer.r.xform(encode_constant(1.0)).x;
				weight_sum += weight;
			}
		}
	}
	REQUIRE(weight_sum > 0.0);
	return weighted / weight_sum;
}

static real_t analytic_color_sdf_transfer_r(const LocalLRTColorSDF &p_sdf, const Transform3D &p_volume_to_object, const Vector3 &p_center, const Vector3 &p_spacing) {
	const LocalLRTColorSDF::Sample center_sample = p_sdf.sample(p_volume_to_object.xform(p_center));
	REQUIRE(center_sample.signed_distance >= 0.0);
	SH2Matrix transfer;
	for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
		const Vector3i offset = neighbor_offset(neighbor);
		const LocalLRTColorSDF::Sample sample = p_sdf.sample(p_volume_to_object.xform(p_center + Vector3(offset) * p_spacing));
		const real_t coverage = CLAMP(sample.coverage, (real_t)0.0, (real_t)1.0);
		if (coverage <= 0.0) {
			continue;
		}
		const Vector3 sample_dir = Vector3(offset).normalized();
		const real_t solid_angle = Math::TAU * 2.0 * neighbor_weight(offset);
		const Vector4 sample_basis = sh_basis(sample_dir);
		const Vector4 diffuse = sh2_pi_div_dft(-sample_dir);
		const real_t scale = (sample.albedo.r + sample.emission.r) * coverage * solid_angle;
		for (int row = 0; row < 4; row++) {
			transfer.rows[row] += diffuse * (sample_basis[row] * scale);
		}
	}
	return transfer.xform(encode_constant(1.0)).x;
}

static real_t sample_variance(const Vector<real_t> &p_values) {
	REQUIRE(p_values.size() > 1);
	real_t mean = 0.0;
	for (real_t value : p_values) {
		mean += value;
	}
	mean /= p_values.size();
	real_t variance = 0.0;
	for (real_t value : p_values) {
		const real_t delta = value - mean;
		variance += delta * delta;
	}
	return variance / p_values.size();
}

static real_t sample_rmse(const Vector<real_t> &p_values, const Vector<real_t> &p_reference) {
	REQUIRE(p_values.size() == p_reference.size());
	REQUIRE(p_values.size() > 0);
	real_t sum = 0.0;
	for (int i = 0; i < p_values.size(); i++) {
		const real_t delta = p_values[i] - p_reference[i];
		sum += delta * delta;
	}
	return Math::sqrt(sum / p_values.size());
}

TEST_CASE("[LocalLRTBuilder] Color SDF rotated slab tangent variance falls with probe spacing") {
	const Vector3 size(8, 8, 8);
	const Transform3D object_to_volume(Basis(Vector3(0, 0, 1), Math::deg_to_rad(30.0)));
	const Transform3D volume_to_object = object_to_volume.affine_inverse();
	const LocalLRTColorSDF sdf = LocalLRTColorSDF::make_box(Vector3(3.0, 0.2, 3.0), 0.125, Color(0.8, 0.1, 0.1));
	const real_t spacings[] = { 1.0, 0.5, 0.25 };
	real_t previous_variance = -1.0;
	real_t previous_error = -1.0;
	for (int i = 0; i < 3; i++) {
		LocalLRTBuilder grid(size, probe_resolution(size, spacings[i]));
		grid.add_geometry_source(sdf, object_to_volume);
		grid.build_local_data();
		const Vector3 spacing = actual_probe_spacing(size, grid.get_resolution());
		Vector<real_t> analytic;
		Vector<real_t> reconstructed;
		for (int sample = 0; sample <= 16; sample++) {
			const real_t tangent = Math::lerp((real_t)-1.5, (real_t)1.5, sample / (real_t)16.0);
			const Vector3 volume_local = object_to_volume.xform(Vector3(tangent, 0.45, 0.0));
			const real_t reference = analytic_color_sdf_transfer_r(sdf, volume_to_object, volume_local, spacing);
			REQUIRE(reference > 0.0);
			analytic.push_back(reference);
			reconstructed.push_back(interpolate_transfer_r(grid, volume_local));
		}
		const real_t variance = sample_variance(analytic);
		const real_t error = sample_rmse(reconstructed, analytic);
		if (previous_variance >= 0.0) {
			CHECK(variance <= previous_variance + 1e-4);
			CHECK(error <= previous_error + 1e-4);
		}
		previous_variance = variance;
		previous_error = error;
	}
}

TEST_CASE("[LocalLRTBuilder] Color SDF LTM error does not grow as geometry voxel size shrinks") {
	const Vector3 size(4, 4, 4);
	const Vector3i resolution = probe_resolution(size, 0.5);
	const Color albedo(0.8, 0.1, 0.05);
	LocalLRTBuilder analytic(size, resolution);
	analytic.add_geometry_source(LocalLRTColorSDF::make_box(Vector3(0.4, 0.4, 0.4), 0.125, albedo), Transform3D());
	analytic.build_local_data();
	const real_t analytic_transfer = analytic.get_probe(Vector3i(4, 4, 5)).local_transfer.r.xform(encode_constant(1.0)).x;
	REQUIRE(analytic_transfer > 0.0);

	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(0.8, 0.8, 0.8));
	const real_t voxel_sizes[] = { 0.5, 0.25, 0.125 };
	real_t previous_error = -1.0;
	for (real_t voxel_size : voxel_sizes) {
		LocalLRTBuilder grid(size, resolution);
		grid.add_geometry_source(LocalLRTColorSDF::from_mesh(mesh, voxel_size, albedo), Transform3D());
		grid.build_local_data();
		const real_t transfer = grid.get_probe(Vector3i(4, 4, 5)).local_transfer.r.xform(encode_constant(1.0)).x;
		const real_t error = Math::abs(transfer - analytic_transfer);
		if (previous_error >= 0.0) {
			CHECK(error <= previous_error + 1e-3);
		}
		previous_error = error;
	}
}

} // namespace TestLocalLRTBuilder
