/**************************************************************************/
/*  local_lrt_builder.cpp                                                 */
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

#include "local_lrt_builder.h"

#include "core/math/geometry_3d.h"

using namespace LocalLRTMath;

static Vector4 &get_channel(LocalLRTBuilder::SH2RGB &r_value, int p_channel) {
	if (p_channel == 0) {
		return r_value.r;
	}
	if (p_channel == 1) {
		return r_value.g;
	}
	return r_value.b;
}

static const Vector4 &get_channel(const LocalLRTBuilder::SH2RGB &p_value, int p_channel) {
	if (p_channel == 0) {
		return p_value.r;
	}
	if (p_channel == 1) {
		return p_value.g;
	}
	return p_value.b;
}

static SH2Matrix &get_channel(LocalLRTBuilder::TransferRGB &r_value, int p_channel) {
	if (p_channel == 0) {
		return r_value.r;
	}
	if (p_channel == 1) {
		return r_value.g;
	}
	return r_value.b;
}

static const SH2Matrix &get_channel(const LocalLRTBuilder::TransferRGB &p_value, int p_channel) {
	if (p_channel == 0) {
		return p_value.r;
	}
	if (p_channel == 1) {
		return p_value.g;
	}
	return p_value.b;
}

LocalLRTBuilder::LocalLRTBuilder(const Vector3 &p_size, const Vector3i &p_resolution, const Transform3D &p_transform) :
		size(p_size),
		resolution(p_resolution),
		transform(p_transform) {
	const int probe_count = resolution.x * resolution.y * resolution.z;
	probes.resize(probe_count);
	visibility_scratch.resize(probe_count);
	radiance_scratch.resize(probe_count);
	build_local_data();
}

bool LocalLRTBuilder::_is_valid_position(const Vector3i &p_position) const {
	return p_position.x >= 0 && p_position.y >= 0 && p_position.z >= 0 &&
			p_position.x < resolution.x && p_position.y < resolution.y && p_position.z < resolution.z;
}

const LocalLRTBuilder::Probe &LocalLRTBuilder::get_probe(const Vector3i &p_position) const {
	return probes[probe_index(p_position, resolution)];
}

LocalLRTBuilder::Probe &LocalLRTBuilder::get_probe(const Vector3i &p_position) {
	return probes.write[probe_index(p_position, resolution)];
}

Vector3 LocalLRTBuilder::get_probe_local_position(const Vector3i &p_position) const {
	return grid_to_local(Vector3(p_position), size, resolution);
}

Vector3 LocalLRTBuilder::get_probe_world_position(const Vector3i &p_position) const {
	return local_to_world(get_probe_local_position(p_position), transform);
}

void LocalLRTBuilder::_sync_occupancy(Probe &r_probe) const {
	if (r_probe.occupied && r_probe.coverage <= 0.0) {
		r_probe.coverage = 1.0;
		r_probe.sample_mask = 0xFFFF;
		r_probe.material_weight = 1.0;
	} else if (!r_probe.occupied) {
		r_probe.coverage = 0.0;
		r_probe.sample_mask = 0;
		r_probe.material_weight = 0.0;
	}
	r_probe.coverage = MIN(r_probe.coverage, (real_t)1.0);
	r_probe.occupied = r_probe.coverage > 0.0;
	r_probe.inside_solid = r_probe.occupied;
}

void LocalLRTBuilder::_add_surface(const Vector3i &p_position, uint16_t p_sample_mask, const Color &p_albedo, const Color &p_emission, const Color &p_transfer_emission, const Vector3 &p_normal) {
	if (p_sample_mask == 0) {
		return;
	}

	Probe &probe = get_probe(p_position);
	int sample_count = 0;
	for (uint16_t bits = p_sample_mask; bits != 0; bits >>= 1) {
		sample_count += bits & 1;
	}
	const real_t tri_weight = sample_count / 16.0;
	const real_t combined = probe.material_weight + tri_weight;
	probe.albedo = (probe.albedo * probe.material_weight + p_albedo * tri_weight) / combined;
	probe.emission = (probe.emission * probe.material_weight + p_emission * tri_weight) / combined;
	probe.transfer_emission = (probe.transfer_emission * probe.material_weight + p_transfer_emission * tri_weight) / combined;
	probe.surface_normal = probe.surface_normal * probe.material_weight + p_normal * tri_weight;
	if (probe.surface_normal.length_squared() > CMP_EPSILON) {
		probe.surface_normal.normalize();
	}
	probe.material_weight = combined;
	probe.sample_mask |= p_sample_mask;
	int occupied_samples = 0;
	for (uint16_t bits = probe.sample_mask; bits != 0; bits >>= 1) {
		occupied_samples += bits & 1;
	}
	probe.coverage = occupied_samples / 16.0;
	probe.occupied = true;
	probe.inside_solid = true;
}

static uint16_t triangle_cell_sample_mask(const Vector3 &p_center, const Vector3 &p_half, const Vector3 p_triangle[3], const Vector3 &p_normal) {
	if (!Geometry3D::triangle_box_overlap(p_center, p_half, p_triangle)) {
		return 0;
	}

	const Vector3 abs_normal = p_normal.abs();
	Vector3 axis_u;
	Vector3 axis_v;
	if (abs_normal.x >= abs_normal.y && abs_normal.x >= abs_normal.z) {
		axis_u = Vector3(0, 1, 0);
		axis_v = Vector3(0, 0, 1);
	} else if (abs_normal.y >= abs_normal.z) {
		axis_u = Vector3(1, 0, 0);
		axis_v = Vector3(0, 0, 1);
	} else {
		axis_u = Vector3(1, 0, 0);
		axis_v = Vector3(0, 1, 0);
	}

	const Vector3 extent_u(axis_u.x * p_half.x, axis_u.y * p_half.y, axis_u.z * p_half.z);
	const Vector3 extent_v(axis_v.x * p_half.x, axis_v.y * p_half.y, axis_v.z * p_half.z);
	const real_t plane_offset = p_normal.dot(p_triangle[0]);
	const real_t signed_distance = p_normal.dot(p_center) - plane_offset;
	const real_t support = abs_normal.dot(p_half);
	if (signed_distance >= support || signed_distance < -support) {
		return 0;
	}

	uint16_t mask = 0;
	const int samples = 4;
	for (int v = 0; v < samples; v++) {
		for (int u = 0; u < samples; u++) {
			const real_t su = ((u + 0.5) / samples) * 2.0 - 1.0;
			const real_t sv = ((v + 0.5) / samples) * 2.0 - 1.0;
			const Vector3 sample = p_center + extent_u * su + extent_v * sv;
			const Vector3 projected = sample - p_normal * (p_normal.dot(sample) - plane_offset);
			if (Geometry3D::point_in_projected_triangle(projected, p_triangle[0], p_triangle[1], p_triangle[2])) {
				mask |= (uint16_t)(1 << (v * samples + u));
			}
		}
	}
	return mask;
}

void LocalLRTBuilder::set_occupancy(const Vector3i &p_position, const Color &p_albedo, const Color &p_emission, const Color &p_transfer_emission) {
	Probe &probe = get_probe(p_position);
	probe.occupied = true;
	probe.inside_solid = true;
	probe.coverage = 1.0;
	probe.signed_distance = -1.0;
	probe.sample_mask = 0xFFFF;
	probe.material_weight = 1.0;
	probe.albedo = p_albedo;
	probe.emission = p_emission;
	probe.transfer_emission = p_transfer_emission;
}

void LocalLRTBuilder::rasterize_triangle(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, const Color &p_albedo, const Color &p_emission, const Color &p_transfer_emission) {
	Vector3 normal = (p_b - p_a).cross(p_c - p_a);
	if (normal.length_squared() <= CMP_EPSILON) {
		return;
	}
	normal.normalize();

	const Vector3 triangle[3] = { p_a, p_b, p_c };
	const Vector3 cell_half_size = actual_probe_spacing(size, resolution) * 0.5;
	const Vector3 aabb_min = p_a.min(p_b).min(p_c) - cell_half_size;
	const Vector3 aabb_max = p_a.max(p_b).max(p_c) + cell_half_size;
	const Vector3 min_grid = local_to_grid(aabb_min, size, resolution);
	const Vector3 max_grid = local_to_grid(aabb_max, size, resolution);
	const int min_x = CLAMP((int)Math::floor(min_grid.x), 0, resolution.x - 1);
	const int min_y = CLAMP((int)Math::floor(min_grid.y), 0, resolution.y - 1);
	const int min_z = CLAMP((int)Math::floor(min_grid.z), 0, resolution.z - 1);
	const int max_x = CLAMP((int)Math::ceil(max_grid.x), 0, resolution.x - 1);
	const int max_y = CLAMP((int)Math::ceil(max_grid.y), 0, resolution.y - 1);
	const int max_z = CLAMP((int)Math::ceil(max_grid.z), 0, resolution.z - 1);

	for (int z = min_z; z <= max_z; z++) {
		for (int y = min_y; y <= max_y; y++) {
			for (int x = min_x; x <= max_x; x++) {
				const Vector3i position(x, y, z);
				const Vector3 center = get_probe_local_position(position);
				const uint16_t sample_mask = triangle_cell_sample_mask(center, cell_half_size, triangle, normal);
				if (sample_mask != 0) {
					_add_surface(position, sample_mask, p_albedo, p_emission, p_transfer_emission, normal);
				}
			}
		}
	}
}

void LocalLRTBuilder::clear_occupancy() {
	for (Probe &probe : probes) {
		probe.occupied = false;
		probe.inside_solid = false;
		probe.coverage = 0.0;
		probe.signed_distance = 1.0e20;
		probe.sample_mask = 0;
		probe.material_weight = 0.0;
		probe.albedo = Color();
		probe.emission = Color();
		probe.transfer_emission = Color();
		probe.surface_normal = Vector3();
	}
	build_local_data();
}

void LocalLRTBuilder::add_geometry_source(const LocalLRTColorSDF &p_sdf, const Transform3D &p_object_to_volume) {
	GeometrySource source;
	source.sdf = p_sdf;
	source.volume_to_object = p_object_to_volume.affine_inverse();
	const Vector3 scale = p_object_to_volume.basis.get_scale().abs();
	const real_t scale_max = MAX(scale.x, MAX(scale.y, scale.z));
	source.surface_bounds = p_object_to_volume.xform(p_sdf.get_bounds()).grow(p_sdf.get_voxel_size() * scale_max);
	geometry_sources.push_back(source);
}

void LocalLRTBuilder::clear_geometry_sources() {
	geometry_sources.clear();
}

void LocalLRTBuilder::_accumulate_direction_sample(Probe &r_probe, const Vector3i &p_offset, real_t p_coverage, const Color &p_albedo, const Color &p_emission, const Color &p_transfer_emission) {
	const Vector3 sample_dir = Vector3(p_offset).normalized();
	const real_t weight = neighbor_weight(p_offset);
	const real_t solid_angle = Math::TAU * 2.0 * weight;
	const real_t coverage = CLAMP(p_coverage, (real_t)0.0, (real_t)1.0);
	const real_t open_fraction = 1.0 - coverage;
	if (open_fraction > 0.0) {
		r_probe.local_visibility += encode_direction(sample_dir, open_fraction, solid_angle);
		r_probe.empty_space_transmission += weight * open_fraction;
	}
	if (coverage <= 0.0) {
		return;
	}
	const Vector4 sample_basis = sh_basis(sample_dir);
	const Vector4 diffuse = sh2_pi_div_dft(-sample_dir);
	const Color color_to_fill = p_albedo + p_transfer_emission;
	const real_t reflected_solid_angle = coverage * solid_angle;
	for (int channel = 0; channel < 3; channel++) {
		get_channel(r_probe.mesh_light, channel) += encode_direction(sample_dir, p_emission[channel] * coverage, solid_angle);
		SH2Matrix &transfer = get_channel(r_probe.local_transfer, channel);
		const real_t scale = color_to_fill[channel] * reflected_solid_angle;
		for (int row = 0; row < 4; row++) {
			transfer.rows[row] += diffuse * (sample_basis[row] * scale);
		}
	}
}

LocalLRTColorSDF::Sample LocalLRTBuilder::_sample_geometry_source(const GeometrySource &p_source, const Vector3 &p_volume_local) const {
	LocalLRTColorSDF::Sample sample = p_source.sdf.sample(p_source.volume_to_object.xform(p_volume_local));
	if (sample.normal.length_squared() <= CMP_EPSILON) {
		return sample;
	}
	Vector3 transformed_normal = p_source.volume_to_object.basis.transposed().xform(sample.normal);
	const real_t distance_scale = transformed_normal.length();
	if (distance_scale > CMP_EPSILON) {
		sample.signed_distance /= distance_scale;
		sample.normal = transformed_normal / distance_scale;
	}
	return sample;
}

LocalLRTColorSDF::Sample LocalLRTBuilder::_sample_geometry(const Vector3 &p_volume_local) const {
	LocalLRTColorSDF::Sample best;
	for (const GeometrySource &source : geometry_sources) {
		const LocalLRTColorSDF::Sample sample = _sample_geometry_source(source, p_volume_local);
		if (sample.signed_distance < best.signed_distance) {
			best = sample;
		}
	}
	return best;
}

LocalLRTColorSDF::Sample LocalLRTBuilder::_sample_geometry_segment(const Vector3 &p_begin, const Vector3 &p_end) const {
	LocalLRTColorSDF::Sample best;
	const real_t segment_length = p_begin.distance_to(p_end);
	real_t best_hit_distance = INFINITY;
	for (const GeometrySource &source : geometry_sources) {
		const LocalLRTColorSDF::Sample begin_sample = _sample_geometry_source(source, p_begin);
		if (begin_sample.signed_distance < 0.0) {
			continue;
		}
		const LocalLRTColorSDF::Sample end_sample = _sample_geometry_source(source, p_end);
		const bool endpoint_inside = end_sample.signed_distance <= 0.0;
		const bool crossed_thin_surface = begin_sample.normal.dot(end_sample.normal) < 0.0 &&
				begin_sample.signed_distance + end_sample.signed_distance <= segment_length;
		if (!endpoint_inside && !crossed_thin_surface) {
			continue;
		}
		if (begin_sample.signed_distance < best_hit_distance) {
			best = end_sample;
			best.coverage = 1.0;
			best_hit_distance = begin_sample.signed_distance;
		}
	}
	return best;
}

void LocalLRTBuilder::_build_from_occupancy_grid() {
	const Vector4 fully_visible = encode_constant(1.0);
	for (int index = 0; index < probes.size(); index++) {
		_sync_occupancy(probes.write[index]);
	}
	for (int index = 0; index < probes.size(); index++) {
		Probe &probe = probes.write[index];
		probe.local_visibility = Vector4();
		probe.global_visibility = Vector4();
		probe.local_transfer = TransferRGB();
		probe.mesh_light = SH2RGB();
		probe.empty_space_transmission = 0.0;
		if (probe.inside_solid) {
			continue;
		}
		const Vector3i position = probe_position(index, resolution);
		for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
			const Vector3i offset = neighbor_offset(neighbor);
			const Vector3i neighbor_position = position + offset;
			if (!_is_valid_position(neighbor_position)) {
				_accumulate_direction_sample(probe, offset, 0.0, Color(), Color(), Color());
				continue;
			}
			const Probe &surface = get_probe(neighbor_position);
			_accumulate_direction_sample(probe, offset, surface.occupancy(), surface.albedo, surface.emission, surface.transfer_emission);
		}
		probe.global_visibility = probe.local_visibility;
	}
	for (Probe &probe : probes) {
		if (!probe.inside_solid && Math::is_equal_approx(probe.empty_space_transmission, (real_t)1.0)) {
			probe.local_visibility = fully_visible;
			probe.global_visibility = fully_visible;
		}
	}
}

void LocalLRTBuilder::_update_geometry_probe_center(const Vector3i &p_position) {
	Probe &probe = get_probe(p_position);
	const Vector3 center = get_probe_local_position(p_position);
	const LocalLRTColorSDF::Sample center_sample = _sample_geometry(center);
	probe.signed_distance = center_sample.signed_distance;
	probe.coverage = center_sample.coverage;
	probe.albedo = center_sample.albedo;
	probe.emission = center_sample.emission;
	probe.transfer_emission = center_sample.transfer_emission;
	probe.surface_normal = center_sample.normal;
	probe.inside_solid = center_sample.signed_distance < 0.0;
	probe.occupied = probe.inside_solid;
}

void LocalLRTBuilder::_build_geometry_probe(const Vector3i &p_position, const Vector3 &p_spacing) {
	const Vector4 fully_visible = encode_constant(1.0);
	Probe &probe = get_probe(p_position);
	probe.local_visibility = Vector4();
	probe.global_visibility = Vector4();
	probe.local_transfer = TransferRGB();
	probe.mesh_light = SH2RGB();
	probe.empty_space_transmission = 0.0;
	probe.sample_mask = 0;
	probe.material_weight = 0.0;
	_update_geometry_probe_center(p_position);
	if (probe.inside_solid) {
		return;
	}
	const Vector3 center = get_probe_local_position(p_position);
	for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
		const Vector3i offset = neighbor_offset(neighbor);
		const LocalLRTColorSDF::Sample sample = _sample_geometry_segment(center, center + Vector3(offset) * p_spacing);
		_accumulate_direction_sample(probe, offset, sample.coverage, sample.albedo, sample.emission, sample.transfer_emission);
	}
	if (Math::is_equal_approx(probe.empty_space_transmission, (real_t)1.0)) {
		probe.local_visibility = fully_visible;
	}
	probe.global_visibility = probe.local_visibility;
}

void LocalLRTBuilder::_build_from_geometry_sources() {
	build_local_data_region(Vector3i(), resolution - Vector3i(1, 1, 1));
}

void LocalLRTBuilder::build_local_data() {
	if (geometry_sources.is_empty()) {
		_build_from_occupancy_grid();
	} else {
		_build_from_geometry_sources();
	}
	clear_injection();
	reset_radiance();
}

void LocalLRTBuilder::build_local_data_region(const Vector3i &p_begin, const Vector3i &p_end) {
	ERR_FAIL_COND(!_is_valid_position(p_begin));
	ERR_FAIL_COND(!_is_valid_position(p_end));
	ERR_FAIL_COND(p_begin.x > p_end.x || p_begin.y > p_end.y || p_begin.z > p_end.z);

	const Vector3 spacing = actual_probe_spacing(size, resolution);
	for (int z = p_begin.z; z <= p_end.z; z++) {
		for (int y = p_begin.y; y <= p_end.y; y++) {
			for (int x = p_begin.x; x <= p_end.x; x++) {
				_build_geometry_probe(Vector3i(x, y, z), spacing);
			}
		}
	}
}

void LocalLRTBuilder::clear_injection() {
	for (Probe &probe : probes) {
		probe.injection = SH2RGB();
	}
}

void LocalLRTBuilder::_add_directional_injection(SH2RGB &r_injection, const Vector3 &p_direction, const Color &p_color, real_t p_energy) {
	const Vector4 encoded = encode_direction(p_direction, p_energy, Math::TAU);
	r_injection.r += encoded * p_color.r;
	r_injection.g += encoded * p_color.g;
	r_injection.b += encoded * p_color.b;
}

void LocalLRTBuilder::inject_directional_light(const DirectionalLight &p_light) {
	if (!p_light.enabled) {
		return;
	}
	const Vector3 local_direction = transform.basis.transposed().xform(p_light.direction_to_light).normalized();
	for (Probe &probe : probes) {
		if (!probe.inside_solid) {
			// Godot's directional energy is diffuse radiance. Convert it back to
			// incident irradiance (PI * energy), while the shared encoder uses TAU.
			_add_directional_injection(probe.injection, local_direction, p_light.color, p_light.energy * 0.5);
		}
	}
}

void LocalLRTBuilder::inject_omni_light(const OmniLight &p_light) {
	if (!p_light.enabled) {
		return;
	}
	for (int index = 0; index < probes.size(); index++) {
		Probe &probe = probes.write[index];
		if (probe.inside_solid) {
			continue;
		}
		const Vector3 to_light_world = p_light.position - get_probe_world_position(probe_position(index, resolution));
		const real_t distance = to_light_world.length();
		if (distance >= p_light.range) {
			continue;
		}
		real_t normalized_distance = distance / p_light.range;
		normalized_distance *= normalized_distance;
		normalized_distance *= normalized_distance;
		real_t range_window = MAX(1.0 - normalized_distance, 0.0);
		range_window *= range_window;
		const real_t attenuation = range_window * Math::pow(MAX(distance, (real_t)0.0001), -p_light.attenuation);
		const Vector3 direction = transform.basis.transposed().xform(to_light_world).normalized();
		_add_directional_injection(probe.injection, direction, p_light.color, p_light.energy * attenuation * 0.5);
	}
}

void LocalLRTBuilder::inject_spot_light(const SpotLight &p_light) {
	if (!p_light.enabled || p_light.range <= 0.0 || p_light.angle <= 0.0 || p_light.angle >= Math::PI * 0.5 || p_light.angle_attenuation <= 0.0) {
		return;
	}
	const Vector3 light_direction = p_light.direction.normalized();
	const real_t cone_limit = Math::cos(p_light.angle);
	const real_t cone_exponent = 1.0 / p_light.angle_attenuation;
	for (int index = 0; index < probes.size(); index++) {
		Probe &probe = probes.write[index];
		if (probe.inside_solid) {
			continue;
		}
		const Vector3 light_to_probe = get_probe_world_position(probe_position(index, resolution)) - p_light.position;
		const real_t distance = light_to_probe.length();
		if (distance >= p_light.range || Math::is_zero_approx(distance)) {
			continue;
		}
		real_t normalized_distance = distance / p_light.range;
		normalized_distance *= normalized_distance;
		normalized_distance *= normalized_distance;
		real_t range_window = MAX(1.0 - normalized_distance, 0.0);
		range_window *= range_window;
		const real_t range_attenuation = range_window * Math::pow(MAX(distance, (real_t)0.0001), -p_light.attenuation);
		const real_t cone_cosine = MAX(light_direction.dot(light_to_probe / distance), cone_limit);
		const real_t spot_rim = MAX((real_t)0.0001, (1.0 - cone_cosine) / (1.0 - cone_limit));
		const real_t cone_attenuation = 1.0 - Math::pow(spot_rim, cone_exponent);
		const Vector3 direction = transform.basis.transposed().xform(-light_to_probe).normalized();
		_add_directional_injection(probe.injection, direction, p_light.color, p_light.energy * range_attenuation * cone_attenuation * 0.5);
	}
}

void LocalLRTBuilder::inject_area_light(const AreaLight &p_light) {
	constexpr int sample_axis_count = 8;
	constexpr int sample_count = sample_axis_count * sample_axis_count;
	if (!p_light.enabled) {
		return;
	}
	const real_t width_length = p_light.width.length();
	const real_t height_length = p_light.height.length();
	const real_t area = width_length * height_length;
	if (area <= CMP_EPSILON || p_light.range <= 0.0 || p_light.direction.is_zero_approx()) {
		return;
	}
	const Vector3 direction = p_light.direction.normalized();
	const Vector3 width_direction = p_light.width / width_length;
	const Vector3 height_direction = p_light.height / height_length;
	const real_t sample_area = area / sample_count;
	const real_t energy_scale = p_light.normalize_energy ? 1.0 / area : 1.0;
	for (int index = 0; index < probes.size(); index++) {
		Probe &probe = probes.write[index];
		if (probe.inside_solid) {
			continue;
		}
		const Vector3 probe_world = get_probe_world_position(probe_position(index, resolution));
		const Vector3 light_to_probe = probe_world - p_light.position;
		if (direction.dot(light_to_probe) <= 0.0) {
			continue;
		}
		const real_t local_x = CLAMP(light_to_probe.dot(width_direction), -width_length * 0.5, width_length * 0.5);
		const real_t local_y = CLAMP(light_to_probe.dot(height_direction), -height_length * 0.5, height_length * 0.5);
		const Vector3 closest_point = p_light.position + width_direction * local_x + height_direction * local_y;
		const real_t closest_distance = probe_world.distance_to(closest_point);
		if (closest_distance >= p_light.range) {
			continue;
		}
		real_t normalized_distance = closest_distance / p_light.range;
		normalized_distance *= normalized_distance;
		normalized_distance *= normalized_distance;
		real_t range_window = MAX(1.0 - normalized_distance, 0.0);
		range_window *= range_window;
		const real_t range_attenuation = range_window * Math::pow(MAX(closest_distance, (real_t)0.0001), (real_t)2.0 - p_light.attenuation);
		for (int y = 0; y < sample_axis_count; y++) {
			const real_t v = ((real_t)y + 0.5) / sample_axis_count - 0.5;
			for (int x = 0; x < sample_axis_count; x++) {
				const real_t u = ((real_t)x + 0.5) / sample_axis_count - 0.5;
				const Vector3 sample_position = p_light.position + p_light.width * u + p_light.height * v;
				const Vector3 sample_to_probe = probe_world - sample_position;
				const real_t distance_squared = sample_to_probe.length_squared();
				if (distance_squared <= CMP_EPSILON) {
					continue;
				}
				const Vector3 sample_to_probe_direction = sample_to_probe / Math::sqrt(distance_squared);
				const real_t emission_cosine = MAX(direction.dot(sample_to_probe_direction), (real_t)0.0);
				if (emission_cosine <= 0.0) {
					continue;
				}
				const real_t solid_angle_weight = emission_cosine * sample_area / distance_squared;
				const Vector3 local_direction = transform.basis.transposed().xform(-sample_to_probe_direction);
				_add_directional_injection(probe.injection, local_direction, p_light.color, p_light.energy * range_attenuation * energy_scale * solid_angle_weight * 0.5);
			}
		}
	}
}

void LocalLRTBuilder::_get_neighbor_local_visibility(const Vector3i &p_position, Vector4 *r_visibility) const {
	const Vector4 fully_visible = encode_constant(1.0);
	for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
		const Vector3i neighbor_position = p_position + neighbor_offset(neighbor);
		r_visibility[neighbor] = _is_valid_position(neighbor_position) ? get_probe(neighbor_position).local_visibility : fully_visible;
	}
}

void LocalLRTBuilder::_get_neighbor_global_visibility(const Vector3i &p_position, Vector4 *r_visibility) const {
	const Vector4 fully_visible = encode_constant(1.0);
	for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
		const Vector3i neighbor_position = p_position + neighbor_offset(neighbor);
		r_visibility[neighbor] = _is_valid_position(neighbor_position) ? get_probe(neighbor_position).global_visibility : fully_visible;
	}
}

void LocalLRTBuilder::reset_global_visibility() {
	for (Probe &probe : probes) {
		probe.global_visibility = probe.local_visibility;
	}
}

void LocalLRTBuilder::propagate_global_visibility(int p_iterations) {
	for (int iteration = 0; iteration < p_iterations; iteration++) {
		for (int index = 0; index < probes.size(); index++) {
			const Probe &probe = probes[index];
			if (probe.inside_solid) {
				visibility_scratch.write[index] = Vector4();
				continue;
			}
			Vector4 neighbor_visibility[NEIGHBOR_COUNT];
			_get_neighbor_global_visibility(probe_position(index, resolution), neighbor_visibility);
			visibility_scratch.write[index] = propagate_visibility(probe.local_visibility, neighbor_visibility);
		}
		for (int index = 0; index < probes.size(); index++) {
			probes.write[index].global_visibility = visibility_scratch[index];
		}
	}
}

void LocalLRTBuilder::reset_radiance() {
	for (Probe &probe : probes) {
		probe.radiance = SH2RGB();
	}
}

void LocalLRTBuilder::_get_neighbor_radiance(const Vector3i &p_position, int p_channel, Vector4 *r_radiance) const {
	for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
		const Vector3i neighbor_position = p_position + neighbor_offset(neighbor);
		r_radiance[neighbor] = _is_valid_position(neighbor_position) ? get_channel(get_probe(neighbor_position).radiance, p_channel) : Vector4();
	}
}

void LocalLRTBuilder::propagate_radiance(int p_iterations) {
	const Vector3 probe_spacing = actual_probe_spacing(size, resolution);
	for (int iteration = 0; iteration < p_iterations; iteration++) {
		for (int index = 0; index < probes.size(); index++) {
			const Probe &probe = probes[index];
			SH2RGB &next = radiance_scratch.write[index];
			next = SH2RGB();
			if (probe.inside_solid) {
				continue;
			}

			const Vector3i position = probe_position(index, resolution);
			Vector4 neighbor_visibility[NEIGHBOR_COUNT];
			_get_neighbor_local_visibility(position, neighbor_visibility);
			for (int channel = 0; channel < 3; channel++) {
				Vector4 neighbor_radiance[NEIGHBOR_COUNT];
				_get_neighbor_radiance(position, channel, neighbor_radiance);

				const Vector4 gathered = gather_radiance(neighbor_radiance, neighbor_visibility, probe_spacing, propagation_decay);
				const Vector4 filtered_gathered = triple_product(gathered, probe.local_visibility);
				const Vector4 filtered_incoming = positive_product(get_channel(probe.mesh_light, channel), probe.local_visibility) +
						triple_product(get_channel(probe.injection, channel) + gathered, probe.local_visibility);
				get_channel(next, channel) = filtered_gathered +
						get_channel(probe.local_transfer, channel).xform(filtered_incoming);
			}
		}
		for (int index = 0; index < probes.size(); index++) {
			probes.write[index].radiance = radiance_scratch[index];
		}
	}
}
