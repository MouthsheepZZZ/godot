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

#include "core/math/math_funcs.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"

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

_FORCE_INLINE_ Vector4 sh_basis(const Vector3 &p_direction) {
	const Vector3 direction = p_direction.normalized();
	return Vector4(SH_Y00, SH_Y1 * direction.x, SH_Y1 * direction.y, SH_Y1 * direction.z);
}

// Projects a weighted directional sample. The weight is its represented solid angle.
_FORCE_INLINE_ Vector4 encode_direction(const Vector3 &p_direction, real_t p_value, real_t p_solid_angle) {
	return sh_basis(p_direction) * (p_value * p_solid_angle);
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

_FORCE_INLINE_ Vector3 surface_sample_grid_position(const Vector3 &p_local_position, const Vector3 &p_local_normal, const Vector3 &p_size, const Vector3i &p_resolution) {
	const Vector3 grid_normal = (p_local_normal.normalized() / actual_probe_spacing(p_size, p_resolution)).normalized();
	return local_to_grid(p_local_position, p_size, p_resolution) + grid_normal;
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
		incoming += visible_radiance * neighbor_weight(offset) * decay;
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

} // namespace LocalLRTMath
