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

_FORCE_INLINE_ Vector4 sh2_pi_div_dft(const Vector3 &p_direction) {
	const Vector3 direction = p_direction.normalized();
	return Vector4(SH_Y00, SH_Y1 * direction.x * (2.0 / 3.0), SH_Y1 * direction.y * (2.0 / 3.0), SH_Y1 * direction.z * (2.0 / 3.0));
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
};

_FORCE_INLINE_ AABB extrude_aabb_toward(const AABB &p_aabb, const Vector3 &p_direction, real_t p_distance) {
	AABB result = p_aabb;
	const Vector3 offset = p_direction.normalized() * p_distance;
	for (int i = 0; i < 8; i++) {
		result.expand_to(p_aabb.get_endpoint(i) + offset);
	}
	return result;
}

_FORCE_INLINE_ DirectionalShadowProjection compute_directional_shadow_projection(const AABB &p_volume_world, const Vector3 &p_direction_to_light, int p_resolution, real_t p_extrude = -1.0) {
	const Vector3 to_light = p_direction_to_light.normalized();
	const real_t extra = p_extrude > 0.0 ? p_extrude : MAX(p_volume_world.get_longest_axis_size() * 2.0, (real_t)8.0);
	const AABB caster = extrude_aabb_toward(p_volume_world, to_light, extra);

	const Vector3 center = p_volume_world.get_center();
	const Vector3 up = Math::abs(to_light.dot(Vector3(0, 1, 0))) > 0.95 ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
	Transform3D camera;
	camera.set_look_at(center + to_light, center, up);

	real_t min_x = 1e20;
	real_t max_x = -1e20;
	real_t min_y = 1e20;
	real_t max_y = -1e20;
	real_t min_z = 1e20;
	real_t max_z = -1e20;
	for (int i = 0; i < 8; i++) {
		const Vector3 local = camera.xform_inv(caster.get_endpoint(i));
		min_x = MIN(min_x, local.x);
		max_x = MAX(max_x, local.x);
		min_y = MIN(min_y, local.y);
		max_y = MAX(max_y, local.y);
		min_z = MIN(min_z, local.z);
		max_z = MAX(max_z, local.z);
	}

	const real_t znear = 0.05;
	const real_t shift = max_z + znear;
	camera.origin += camera.basis.get_column(2) * shift;
	min_z -= shift;
	const real_t zfar = MAX(znear + 1.0, -min_z + 1.0);

	const int resolution = MAX(p_resolution, 1);
	const real_t texel_x = (max_x - min_x) / real_t(resolution);
	const real_t texel_y = (max_y - min_y) / real_t(resolution);
	min_x = Math::floor(min_x / texel_x) * texel_x;
	max_x = Math::ceil(max_x / texel_x) * texel_x;
	min_y = Math::floor(min_y / texel_y) * texel_y;
	max_y = Math::ceil(max_y / texel_y) * texel_y;

	DirectionalShadowProjection result;
	result.camera = camera;
	result.projection.set_orthogonal(min_x, max_x, min_y, max_y, znear, zfar);
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

} // namespace LocalLRTMath
