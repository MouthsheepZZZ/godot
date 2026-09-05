/**************************************************************************/
/*  local_lrt_math.h                                                      */
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

#pragma once

#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/templates/vector.h"

#include <cstdint>

namespace LocalLRTMath {

// Real SH through l = 1, ordered as [Y00, Y1x, Y1y, Y1z].
// A coefficient vector c represents f(direction) = dot(c, basis(direction)).
constexpr real_t SH_Y00 = 0.28209479177387814;
constexpr real_t SH_Y1 = 0.4886025119029199;
constexpr int NEIGHBOR_COUNT = 26;

struct SH2Matrix {
	// Row-major. Applying the transfer is rows * incoming coefficients.
	Vector4 rows[4];

	_FORCE_INLINE_ Vector4 xform(const Vector4 &p_value) const {
		return Vector4(rows[0].dot(p_value), rows[1].dot(p_value), rows[2].dot(p_value), rows[3].dot(p_value));
	}

	_FORCE_INLINE_ void set_column(int p_column, const Vector4 &p_value) {
		for (int row = 0; row < 4; row++) {
			rows[row][p_column] = p_value[row];
		}
	}
};

static constexpr int PACKED_TRANSFER_UINTS_PER_PROBE = 9;

_FORCE_INLINE_ void pack_transfer_luminance_fp16_rgb8(const Vector4 *p_rows, uint32_t *r_values) {
	float luminance[16];
	float denominator = 0.0f;
	for (int element = 0; element < 16; element++) {
		const int row = element / 4;
		const int column = element % 4;
		const float red = p_rows[row][column];
		const float green = p_rows[4 + row][column];
		const float blue = p_rows[8 + row][column];
		luminance[element] = red * 0.2126f + green * 0.7152f + blue * 0.0722f;
		denominator += luminance[element] * luminance[element];
	}

	float tint[3] = {};
	if (denominator > 1e-20f) {
		for (int channel = 0; channel < 3; channel++) {
			float numerator = 0.0f;
			for (int element = 0; element < 16; element++) {
				const int row = element / 4;
				const int column = element % 4;
				numerator += luminance[element] * p_rows[channel * 4 + row][column];
			}
			tint[channel] = MAX(numerator / denominator, 0.0f);
		}
	}

	const float tint_scale = MAX(tint[0], MAX(tint[1], tint[2]));
	if (tint_scale > 1e-20f) {
		for (float &element : luminance) {
			element *= tint_scale;
		}
		for (float &channel : tint) {
			channel /= tint_scale;
		}
	}
	for (int element = 0; element < 16; element += 2) {
		*r_values++ = uint32_t(Math::make_half_float(luminance[element])) | (uint32_t(Math::make_half_float(luminance[element + 1])) << 16);
	}
	const uint32_t red = uint32_t(Math::round(CLAMP(tint[0], 0.0f, 1.0f) * 255.0f));
	const uint32_t green = uint32_t(Math::round(CLAMP(tint[1], 0.0f, 1.0f) * 255.0f));
	const uint32_t blue = uint32_t(Math::round(CLAMP(tint[2], 0.0f, 1.0f) * 255.0f));
	*r_values = red | (green << 8) | (blue << 16) | (0xFFu << 24);
}

_FORCE_INLINE_ void unpack_transfer_luminance_fp16_rgb8(const uint32_t *p_values, Vector4 *r_rows) {
	float luminance[16];
	for (int element = 0; element < 16; element += 2) {
		const uint32_t packed = *p_values++;
		luminance[element] = Math::half_to_float(packed & 0xFFFFu);
		luminance[element + 1] = Math::half_to_float(packed >> 16);
	}
	const uint32_t packed_tint = *p_values;
	const float tint[3] = {
		float(packed_tint & 0xFFu) / 255.0f,
		float((packed_tint >> 8) & 0xFFu) / 255.0f,
		float((packed_tint >> 16) & 0xFFu) / 255.0f,
	};
	for (int channel = 0; channel < 3; channel++) {
		for (int row = 0; row < 4; row++) {
			r_rows[channel * 4 + row] = Vector4(
					luminance[row * 4] * tint[channel],
					luminance[row * 4 + 1] * tint[channel],
					luminance[row * 4 + 2] * tint[channel],
					luminance[row * 4 + 3] * tint[channel]);
		}
	}
}

_FORCE_INLINE_ Vector4 sh_basis(const Vector3 &p_direction) {
	const Vector3 direction = p_direction.normalized();
	return Vector4(SH_Y00, SH_Y1 * direction.x, SH_Y1 * direction.y, SH_Y1 * direction.z);
}

_FORCE_INLINE_ Vector4 sh2_pi_div_dft(const Vector3 &p_direction) {
	const Vector3 direction = p_direction.normalized();
	return Vector4(SH_Y00, SH_Y1 * direction.x * (2.0 / 3.0), SH_Y1 * direction.y * (2.0 / 3.0), SH_Y1 * direction.z * (2.0 / 3.0));
}

// Projects a weighted directional sample. The weight is its represented solid angle.
_FORCE_INLINE_ Vector4 encode_direction(const Vector3 &p_direction, real_t p_value, real_t p_solid_angle) {
	return sh_basis(p_direction) * (p_value * p_solid_angle);
}

// Integrates the constant and first directional moments of a spherical quad.
// Directions must follow the quad boundary without crossing.
_FORCE_INLINE_ Vector4 encode_spherical_quad(const Vector3 *p_directions, real_t p_value) {
	Vector3 directions[4];
	for (int i = 0; i < 4; i++) {
		directions[i] = p_directions[i].normalized();
	}

	auto triangle_solid_angle = [](const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c) {
		const real_t numerator = p_a.dot(p_b.cross(p_c));
		const real_t denominator = 1.0 + p_a.dot(p_b) + p_b.dot(p_c) + p_c.dot(p_a);
		return 2.0 * Math::atan2(numerator, denominator);
	};

	real_t solid_angle = triangle_solid_angle(directions[0], directions[1], directions[2]) + triangle_solid_angle(directions[0], directions[2], directions[3]);
	Vector3 first_moment;
	for (int i = 0; i < 4; i++) {
		const Vector3 edge_cross = directions[i].cross(directions[(i + 1) % 4]);
		const real_t cross_length = edge_cross.length();
		if (cross_length > CMP_EPSILON) {
			const real_t edge_angle = Math::atan2(cross_length, directions[i].dot(directions[(i + 1) % 4]));
			first_moment += edge_cross * (0.5 * edge_angle / cross_length);
		}
	}
	if (solid_angle < 0.0) {
		solid_angle = -solid_angle;
		first_moment = -first_moment;
	}
	return Vector4(SH_Y00 * solid_angle, SH_Y1 * first_moment.x, SH_Y1 * first_moment.y, SH_Y1 * first_moment.z) * p_value;
}

_FORCE_INLINE_ Vector4 encode_constant(real_t p_value) {
	return Vector4(p_value / SH_Y00, 0.0, 0.0, 0.0);
}

_FORCE_INLINE_ real_t evaluate(const Vector4 &p_sh, const Vector3 &p_direction) {
	return p_sh.dot(sh_basis(p_direction));
}

// Convolves incoming radiance with the clamped cosine kernel for a diffuse
// surface. The l = 0 and l = 1 kernel factors are PI and 2PI/3.
_FORCE_INLINE_ real_t evaluate_diffuse_irradiance(const Vector4 &p_radiance, const Vector3 &p_normal) {
	const Vector3 normal = p_normal.normalized();
	return p_radiance.x * SH_Y00 * Math::PI +
			(p_radiance.y * normal.x + p_radiance.z * normal.y + p_radiance.w * normal.z) * SH_Y1 * (2.0 * Math::PI / 3.0);
}

// Constrains directional visibility to the non-negative linear L1 moment
// domain, then reconstructs a strictly positive maximum-entropy distribution.
_FORCE_INLINE_ real_t evaluate_visibility_diffuse_irradiance(const Vector4 &p_visibility, const Vector3 &p_normal) {
	const real_t ambient = MAX(p_visibility.x * SH_Y00 * Math::PI, (real_t)0.0);
	const Vector3 directional = Vector3(p_visibility.y, p_visibility.z, p_visibility.w) * (SH_Y1 * (2.0 * Math::PI / 3.0));
	const real_t directional_length = directional.length();
	if (ambient <= CMP_EPSILON || directional_length <= CMP_EPSILON) {
		return ambient;
	}

	const real_t moment = MIN(directional_length / (3.0 * ambient), (real_t)(1.0 / 3.0));
	const real_t moment_squared = moment * moment;
	const real_t kappa = moment * (3.0 - moment_squared) / (1.0 - moment_squared);
	const real_t cosine = CLAMP(p_normal.normalized().dot(directional / directional_length), (real_t)-1.0, (real_t)1.0);
	const real_t normalization = 2.0 * kappa / (1.0 - Math::exp(-2.0 * kappa));
	return ambient * normalization * Math::exp(kappa * (cosine - 1.0));
}

// Maximum-entropy L1 closure avoids negative irradiance while preserving the
// spherical average and deriving concentration from the first moment.
_FORCE_INLINE_ real_t evaluate_nonlinear_diffuse_irradiance(const Vector4 &p_radiance, const Vector3 &p_normal) {
	const real_t ambient = MAX(p_radiance.x * SH_Y00 * Math::PI, (real_t)0.0);
	const Vector3 directional = Vector3(p_radiance.y, p_radiance.z, p_radiance.w) * (SH_Y1 * (2.0 * Math::PI / 3.0));
	const real_t directional_length = directional.length();
	if (ambient <= CMP_EPSILON || directional_length <= CMP_EPSILON) {
		return ambient;
	}

	const real_t moment = MIN(directional_length / (3.0 * ambient), (real_t)0.999);
	const real_t moment_squared = moment * moment;
	const real_t kappa = moment * (3.0 - moment_squared) / (1.0 - moment_squared);
	const real_t cosine = CLAMP(p_normal.normalized().dot(directional / directional_length), (real_t)-1.0, (real_t)1.0);
	const real_t normalization = 2.0 * kappa / (1.0 - Math::exp(-2.0 * kappa));
	return ambient * normalization * Math::exp(kappa * (cosine - 1.0));
}

// Product projected back to SH2. Terms above l = 1 are intentionally discarded.
_FORCE_INLINE_ Vector4 triple_product(const Vector4 &p_a, const Vector4 &p_b) {
	return Vector4(
			p_a.dot(p_b),
			p_a.x * p_b.y + p_b.x * p_a.y,
			p_a.x * p_b.z + p_b.x * p_a.z,
			p_a.x * p_b.w + p_b.x * p_a.w) *
			SH_Y00;
}

_FORCE_INLINE_ Vector4 antipodal(const Vector4 &p_sh) {
	return Vector4(p_sh.x, -p_sh.y, -p_sh.z, -p_sh.w);
}

// Rotates a directional function from local space to world space. For
// f_world(d) = f_local(R^-1 d), the l = 1 coefficient vector is R * c.
_FORCE_INLINE_ Vector4 rotate_to_world(const Vector4 &p_local_sh, const Basis &p_local_to_world) {
	const Vector3 direction = p_local_to_world.xform(Vector3(p_local_sh.y, p_local_sh.z, p_local_sh.w));
	return Vector4(p_local_sh.x, direction.x, direction.y, direction.z);
}

_FORCE_INLINE_ Vector4 rotate_to_local(const Vector4 &p_world_sh, const Basis &p_local_to_world) {
	const Vector3 direction = p_local_to_world.transposed().xform(Vector3(p_world_sh.y, p_world_sh.z, p_world_sh.w));
	return Vector4(p_world_sh.x, direction.x, direction.y, direction.z);
}

// Global diffuse lighting is first rotated into the volume, then masked by
// propagated sky visibility. Global visibility already includes the current
// probe's local visibility, so it must not be applied a second time.
_FORCE_INLINE_ Vector4 mask_global_diffuse(const Vector4 &p_world_sh, const Vector4 &p_global_visibility, const Basis &p_local_to_world) {
	return triple_product(rotate_to_local(p_world_sh, p_local_to_world), p_global_visibility);
}

// If D maps world SH coefficients to local coefficients, this computes
// B_world = D^T * B_local * D, matching output = B * input.
_FORCE_INLINE_ SH2Matrix rotate_transfer_to_world(const SH2Matrix &p_local_transfer, const Basis &p_local_to_world) {
	SH2Matrix result;
	for (int column = 0; column < 4; column++) {
		Vector4 world_input;
		world_input[column] = 1.0;
		const Vector4 local_input = rotate_to_local(world_input, p_local_to_world);
		result.set_column(column, rotate_to_world(p_local_transfer.xform(local_input), p_local_to_world));
	}
	return result;
}

_FORCE_INLINE_ Vector3i probe_resolution(const Vector3 &p_size, real_t p_requested_spacing) {
	return Vector3i(
			MAX(2, (int)Math::ceil(p_size.x / p_requested_spacing) + 1),
			MAX(2, (int)Math::ceil(p_size.y / p_requested_spacing) + 1),
			MAX(2, (int)Math::ceil(p_size.z / p_requested_spacing) + 1));
}

_FORCE_INLINE_ Vector3 actual_probe_spacing(const Vector3 &p_size, const Vector3i &p_resolution) {
	return p_size / Vector3(p_resolution - Vector3i(1, 1, 1));
}

_FORCE_INLINE_ Vector3 local_to_grid(const Vector3 &p_local_position, const Vector3 &p_size, const Vector3i &p_resolution) {
	return (p_local_position + p_size * 0.5) / actual_probe_spacing(p_size, p_resolution);
}

_FORCE_INLINE_ Vector3 grid_to_local(const Vector3 &p_grid_position, const Vector3 &p_size, const Vector3i &p_resolution) {
	return p_grid_position * actual_probe_spacing(p_size, p_resolution) - p_size * 0.5;
}

_FORCE_INLINE_ Vector3 grid_to_uvw(const Vector3 &p_grid_position, const Vector3i &p_resolution) {
	return p_grid_position / Vector3(p_resolution - Vector3i(1, 1, 1));
}

_FORCE_INLINE_ Vector3 uvw_to_grid(const Vector3 &p_uvw, const Vector3i &p_resolution) {
	return p_uvw * Vector3(p_resolution - Vector3i(1, 1, 1));
}

_FORCE_INLINE_ real_t edge_blend_weight(const Vector3 &p_local_position, const Vector3 &p_size, real_t p_blend_distance) {
	const Vector3 distance_to_edge = p_size * 0.5 - p_local_position.abs();
	const real_t minimum_distance = MIN(distance_to_edge.x, MIN(distance_to_edge.y, distance_to_edge.z));
	if (minimum_distance < 0.0) {
		return 0.0;
	}
	if (p_blend_distance <= 0.0) {
		return 1.0;
	}
	return CLAMP(minimum_distance / p_blend_distance, (real_t)0.0, (real_t)1.0);
}

// Must match RendererRD::LocalLRT::MAX_SURFACE_VOLUMES and the Forward+ shader unroll.
constexpr int MAX_BLEND_VOLUMES = 8;

_FORCE_INLINE_ int clamp_max_volumes_per_camera(int p_value) {
	return CLAMP(p_value, 1, MAX_BLEND_VOLUMES);
}

// Higher priority is sampled first. Equal priority uses the lower stable id.
_FORCE_INLINE_ bool volume_priority_before(int p_priority_a, uint64_t p_id_a, int p_priority_b, uint64_t p_id_b) {
	if (p_priority_a != p_priority_b) {
		return p_priority_a > p_priority_b;
	}
	return p_id_a < p_id_b;
}

// Conservative AABB / frustum test. Godot frustum planes face outward.
_FORCE_INLINE_ bool aabb_intersects_frustum(const AABB &p_aabb, const Plane *p_planes, int p_plane_count) {
	if (p_planes == nullptr || p_plane_count <= 0) {
		return true;
	}
	const Vector3 half_extents = p_aabb.size * 0.5;
	const Vector3 center = p_aabb.position + half_extents;
	for (int i = 0; i < p_plane_count; i++) {
		const Plane &plane = p_planes[i];
		const Vector3 inside_vertex = center + Vector3(
													   (plane.normal.x > 0.0) ? -half_extents.x : half_extents.x,
													   (plane.normal.y > 0.0) ? -half_extents.y : half_extents.y,
													   (plane.normal.z > 0.0) ? -half_extents.z : half_extents.z);
		if (plane.is_point_over(inside_vertex)) {
			return false;
		}
	}
	return true;
}

struct CameraVolumeCandidate {
	int priority = 0;
	uint64_t id = 0;
	AABB world_aabb;
};

// Keep volumes overlapping the camera frustum, sorted by priority, capped at p_max.
_FORCE_INLINE_ int select_camera_volumes(const CameraVolumeCandidate *p_volumes, int p_count, const Plane *p_planes, int p_plane_count, int p_max, int *r_indices) {
	if (p_volumes == nullptr || r_indices == nullptr || p_count <= 0 || p_max <= 0) {
		return 0;
	}

	const int max_n = clamp_max_volumes_per_camera(p_max);
	int selected[MAX_BLEND_VOLUMES];
	int selected_count = 0;
	for (int i = 0; i < p_count; i++) {
		if (!aabb_intersects_frustum(p_volumes[i].world_aabb, p_planes, p_plane_count)) {
			continue;
		}

		int insert_at = selected_count;
		for (int j = 0; j < selected_count; j++) {
			const CameraVolumeCandidate &current = p_volumes[selected[j]];
			if (volume_priority_before(p_volumes[i].priority, p_volumes[i].id, current.priority, current.id)) {
				insert_at = j;
				break;
			}
		}
		if (insert_at >= max_n) {
			continue;
		}
		if (selected_count < max_n) {
			selected_count++;
		}
		for (int j = selected_count - 1; j > insert_at; j--) {
			selected[j] = selected[j - 1];
		}
		selected[insert_at] = i;
	}

	for (int i = 0; i < selected_count; i++) {
		r_indices[i] = selected[i];
	}
	return selected_count;
}

// Cascade blend: each volume consumes its edge weight from the remaining mix.
_FORCE_INLINE_ void volume_cascade_blend_weights(const real_t *p_edge_weights, int p_count, real_t *r_weights) {
	real_t remaining = 1.0;
	for (int i = 0; i < p_count; i++) {
		const real_t edge = CLAMP(p_edge_weights[i], (real_t)0.0, (real_t)1.0);
		r_weights[i] = edge * remaining;
		remaining *= (real_t)1.0 - edge;
	}
}

_FORCE_INLINE_ int probe_index(const Vector3i &p_position, const Vector3i &p_resolution) {
	return p_position.x + p_resolution.x * (p_position.y + p_resolution.y * p_position.z);
}

_FORCE_INLINE_ Vector3i probe_position(int p_index, const Vector3i &p_resolution) {
	const int plane_size = p_resolution.x * p_resolution.y;
	const int z = p_index / plane_size;
	const int plane_index = p_index - z * plane_size;
	return Vector3i(plane_index % p_resolution.x, plane_index / p_resolution.x, z);
}

_FORCE_INLINE_ Vector3 local_to_world(const Vector3 &p_local_position, const Transform3D &p_transform) {
	return p_transform.xform(p_local_position);
}

_FORCE_INLINE_ Vector3 world_to_local(const Vector3 &p_world_position, const Transform3D &p_transform) {
	return p_transform.affine_inverse().xform(p_world_position);
}

// Stable z-major enumeration of the 3x3x3 neighborhood, excluding the center.
_FORCE_INLINE_ Vector3i neighbor_offset(int p_neighbor) {
	int current = 0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				if (x == 0 && y == 0 && z == 0) {
					continue;
				}
				if (current == p_neighbor) {
					return Vector3i(x, y, z);
				}
				current++;
			}
		}
	}
	return Vector3i();
}

// Inverse-distance weights normalized across all 26 neighbors.
_FORCE_INLINE_ real_t neighbor_weight(const Vector3i &p_offset) {
	const real_t normalization = 6.0 + 12.0 / Math::SQRT2 + 8.0 / Math::sqrt(3.0);
	return (1.0 / Vector3(p_offset).length()) / normalization;
}

_FORCE_INLINE_ Vector4 positive_product(const Vector4 &p_a, const Vector4 &p_b) {
	if (p_a.is_zero_approx()) {
		return Vector4();
	}
	Vector4 result;
	for (int i = 0; i < NEIGHBOR_COUNT; i++) {
		const Vector3i offset = neighbor_offset(i);
		const Vector3 direction = Vector3(offset).normalized();
		const Vector4 basis = sh_basis(direction);
		const real_t value = MAX(p_a.dot(basis), (real_t)0.0) * MAX(p_b.dot(basis), (real_t)0.0);
		result += basis * (value * Math::TAU * 2.0 * neighbor_weight(offset));
	}
	return result;
}

// Local and propagated visibility always mean visible fraction: one is open,
// zero is blocked. Constant fully-visible SH is therefore encode_constant(1).
_FORCE_INLINE_ Vector4 propagate_visibility(const Vector4 &p_local_visibility, const Vector4 *p_neighbor_visibility) {
	Vector4 gathered;
	for (int i = 0; i < NEIGHBOR_COUNT; i++) {
		gathered += p_neighbor_visibility[i] * neighbor_weight(neighbor_offset(i));
	}
	return triple_product(gathered, p_local_visibility);
}

_FORCE_INLINE_ real_t radiance_distance_decay(const Vector3i &p_offset, const Vector3 &p_probe_spacing, real_t p_decay_per_meter) {
	return Math::pow(p_decay_per_meter, (Vector3(p_offset) * p_probe_spacing).length());
}

_FORCE_INLINE_ Vector4 gather_radiance(
		const Vector4 *p_neighbor_radiance,
		const Vector4 *p_neighbor_visibility,
		const Vector3 &p_probe_spacing,
		real_t p_decay_per_meter) {
	Vector4 incoming;
	for (int i = 0; i < NEIGHBOR_COUNT; i++) {
		const Vector3i offset = neighbor_offset(i);
		const real_t decay = radiance_distance_decay(offset, p_probe_spacing, p_decay_per_meter);
		const Vector4 visible_radiance = triple_product(p_neighbor_radiance[i], antipodal(p_neighbor_visibility[i]));
		const Vector3 direction = Vector3(offset).normalized();
		const real_t directional_radiance = MAX(evaluate(visible_radiance, direction), (real_t)0.0);
		incoming += encode_direction(direction, directional_radiance, Math::TAU * 2.0 * neighbor_weight(offset)) * decay;
	}
	return incoming;
}

// Empty space continues previously reflected radiance. Analytic injection only
// becomes output radiance through the local transfer matrix.
_FORCE_INLINE_ Vector4 propagate_radiance(
		const Vector4 &p_local_visibility,
		const SH2Matrix &p_local_transfer,
		const Vector4 &p_injection,
		const Vector4 *p_neighbor_radiance,
		const Vector4 *p_neighbor_visibility,
		real_t p_empty_space_transmission,
		real_t p_decay_per_meter,
		const Vector3 &p_probe_spacing = Vector3(1.0, 1.0, 1.0)) {
	const Vector4 gathered = gather_radiance(p_neighbor_radiance, p_neighbor_visibility, p_probe_spacing, p_decay_per_meter);
	const Vector4 filtered_gathered = triple_product(gathered, p_local_visibility);
	const Vector4 filtered_injection = triple_product(p_injection, p_local_visibility);
	return filtered_gathered * p_empty_space_transmission + p_local_transfer.xform(filtered_injection + filtered_gathered);
}

struct DirectionalShadowProjection {
	Transform3D camera;
	Projection projection;
	Vector3 right;
};

_FORCE_INLINE_ AABB extrude_aabb_toward(const AABB &p_aabb, const Vector3 &p_direction, real_t p_distance) {
	AABB result = p_aabb;
	const Vector3 offset = p_direction.normalized() * p_distance;
	for (int i = 0; i < 8; i++) {
		result.expand_to(p_aabb.get_endpoint(i) + offset);
	}
	return result;
}

_FORCE_INLINE_ DirectionalShadowProjection compute_directional_shadow_projection(const AABB &p_volume_world, const Vector3 &p_direction_to_light, int p_resolution, real_t p_extrude = -1.0, const Vector3 &p_previous_right = Vector3()) {
	const Vector3 to_light = p_direction_to_light.normalized();
	const real_t extra = p_extrude >= 0.0 ? p_extrude : MAX(p_volume_world.get_longest_axis_size() * 2.0, (real_t)8.0);
	const AABB caster = extrude_aabb_toward(p_volume_world, to_light, extra);

	const Vector3 center = p_volume_world.get_center();
	Vector3 right = p_previous_right - to_light * p_previous_right.dot(to_light);
	if (right.length_squared() <= CMP_EPSILON2) {
		const Vector3 axes[] = { Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1) };
		int least_aligned_axis = 0;
		for (int i = 1; i < 3; i++) {
			if (Math::abs(axes[i].dot(to_light)) < Math::abs(axes[least_aligned_axis].dot(to_light))) {
				least_aligned_axis = i;
			}
		}
		right = axes[least_aligned_axis] - to_light * axes[least_aligned_axis].dot(to_light);
	}
	right.normalize();
	const Vector3 up = to_light.cross(right).normalized();
	const real_t radius = MAX(p_volume_world.size.length() * (real_t)0.5, (real_t)CMP_EPSILON);
	const int resolution = MAX(p_resolution, 1);
	const real_t extent = resolution > 1 ? radius * real_t(resolution) / real_t(resolution - 1) : radius;
	const real_t world_texel_size = extent * (real_t)2.0 / real_t(resolution);
	const real_t center_right = center.dot(right);
	const real_t center_up = center.dot(up);
	const Vector3 snapped_center = center +
			right * (Math::round(center_right / world_texel_size) * world_texel_size - center_right) +
			up * (Math::round(center_up / world_texel_size) * world_texel_size - center_up);
	Transform3D camera;
	camera.basis = Basis(right, up, to_light).orthonormalized();
	camera.origin = snapped_center;

	real_t min_z = 1e20;
	real_t max_z = -1e20;
	for (int i = 0; i < 8; i++) {
		const Vector3 local = camera.xform_inv(caster.get_endpoint(i));
		min_z = MIN(min_z, local.z);
		max_z = MAX(max_z, local.z);
	}

	const real_t znear = MAX(world_texel_size, (real_t)CMP_EPSILON);
	const real_t shift = max_z + znear;
	camera.origin += camera.basis.get_column(2) * shift;
	min_z -= shift;
	const real_t zfar = MAX(znear * (real_t)2.0, -min_z);

	DirectionalShadowProjection result;
	result.camera = camera;
	result.projection.set_orthogonal(-extent, extent, -extent, extent, znear, zfar);
	result.right = right;
	return result;
}

_FORCE_INLINE_ Projection directional_shadow_view_projection(const Transform3D &p_camera, const Projection &p_projection) {
	Projection correction;
	correction.set_depth_correction(true, true);
	return correction * p_projection * Projection(p_camera.affine_inverse());
}

_FORCE_INLINE_ bool directional_shadow_project_point(const Projection &p_view_proj, const Vector3 &p_world, Vector2 &r_uv, real_t &r_depth) {
	const Vector4 clip = p_view_proj.xform(Vector4(p_world.x, p_world.y, p_world.z, 1.0));
	if (Math::abs(clip.w) < (real_t)1e-12) {
		return false;
	}
	const real_t inv_w = 1.0 / clip.w;
	r_uv = Vector2(clip.x * inv_w * 0.5 + 0.5, clip.y * inv_w * 0.5 + 0.5);
	r_depth = clip.z * inv_w;
	return r_uv.x >= 0.0 && r_uv.x <= 1.0 && r_uv.y >= 0.0 && r_uv.y <= 1.0 && r_depth >= 0.0 && r_depth <= 1.0;
}

_FORCE_INLINE_ void fill_constant_shadow_depth(Vector<float> &r_depths, int p_size, float p_depth) {
	r_depths.resize(p_size * p_size);
	for (int i = 0; i < r_depths.size(); i++) {
		r_depths.write[i] = p_depth;
	}
}

_FORCE_INLINE_ real_t sample_directional_shadow_visibility(const Vector<float> &p_depths, int p_size, const Projection &p_view_proj, const Vector3 &p_world, real_t p_bias) {
	Vector2 uv;
	real_t probe_depth = 0.0;
	if (!directional_shadow_project_point(p_view_proj, p_world, uv, probe_depth)) {
		return 1.0;
	}
	const real_t texel = 1.0 / real_t(p_size);
	const Vector2 offsets[4] = { Vector2(-0.5, -0.5), Vector2(0.5, -0.5), Vector2(-0.5, 0.5), Vector2(0.5, 0.5) };
	real_t vis = 0.0;
	for (int i = 0; i < 4; i++) {
		const Vector2 sample_uv = uv + offsets[i] * texel;
		const int x = CLAMP((int)Math::floor(sample_uv.x * p_size), 0, p_size - 1);
		const int y = CLAMP((int)Math::floor(sample_uv.y * p_size), 0, p_size - 1);
		const real_t occluder = p_depths[y * p_size + x];
		vis += (probe_depth + p_bias) >= occluder ? 1.0 : 0.0;
	}
	return vis * 0.25;
}

struct OmniShadowProjection {
	Vector2 paraboloid;
	real_t depth = 0.0;
	bool positive_hemisphere = false;
};

_FORCE_INLINE_ bool omni_shadow_project_point(const Transform3D &p_light_transform, real_t p_range, const Vector3 &p_world, OmniShadowProjection &r_projection) {
	if (p_range <= 0.0) {
		return false;
	}
	const Vector3 light_local = p_light_transform.xform_inv(p_world);
	const real_t distance = light_local.length();
	if (distance <= (real_t)1e-12 || distance >= p_range) {
		return false;
	}
	const Vector3 direction = light_local / distance;
	r_projection.paraboloid = Vector2(direction.x, direction.y) / (1.0 + Math::abs(direction.z));
	r_projection.depth = 1.0 - distance / p_range;
	r_projection.positive_hemisphere = direction.z >= 0.0;
	return true;
}

_FORCE_INLINE_ real_t omni_shadow_depth_visibility(real_t p_occluder_depth, real_t p_probe_distance, real_t p_range, real_t p_bias) {
	if (p_range <= 0.0) {
		return 1.0;
	}
	const real_t probe_depth = 1.0 - (p_probe_distance - p_bias) / p_range;
	return probe_depth >= p_occluder_depth ? 1.0 : 0.0;
}

struct SpotShadowProjection {
	Vector2 uv;
	real_t depth = 0.0;
};

_FORCE_INLINE_ bool spot_shadow_project_point(const Transform3D &p_light_transform, real_t p_range, real_t p_angle, const Vector3 &p_world, SpotShadowProjection &r_projection) {
	if (p_range <= 0.0 || p_angle <= 0.0 || p_angle >= Math::PI * 0.5) {
		return false;
	}
	const Vector3 light_local = p_light_transform.xform_inv(p_world);
	const real_t axial_distance = -light_local.z;
	const real_t z_near = MIN((real_t)0.025, p_range);
	if (axial_distance <= z_near || axial_distance >= p_range) {
		return false;
	}
	const real_t half_extent = axial_distance * Math::tan(p_angle);
	r_projection.uv = Vector2(light_local.x, light_local.y) / half_extent * 0.5 + Vector2(0.5, 0.5);
	r_projection.depth = z_near * (p_range - axial_distance) / (axial_distance * (p_range - z_near));
	return r_projection.uv.x >= 0.0 && r_projection.uv.x <= 1.0 &&
			r_projection.uv.y >= 0.0 && r_projection.uv.y <= 1.0;
}

_FORCE_INLINE_ real_t spot_shadow_depth_visibility(real_t p_occluder_depth, real_t p_probe_axial_distance, real_t p_range, real_t p_bias) {
	const real_t z_near = MIN((real_t)0.025, p_range);
	if (p_range <= z_near || p_probe_axial_distance <= z_near || p_probe_axial_distance >= p_range) {
		return 1.0;
	}
	const real_t probe_depth = z_near * (p_range - p_probe_axial_distance) /
			(p_probe_axial_distance * (p_range - z_near)) +
			p_bias / p_probe_axial_distance;
	return probe_depth >= p_occluder_depth ? 1.0 : 0.0;
}

} // namespace LocalLRTMath
