/**************************************************************************/
/*  local_gi_probe_sample.cpp                                             */
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

#include "local_gi_probe_sample.h"

#include "core/math/math_defs.h"
#include "core/math/math_funcs.h"

float LocalGIProbeSampler::chebyshev_visibility(float p_distance, float p_mean, float p_second_moment, float p_bias) {
	if (!Math::is_finite(p_distance) || !Math::is_finite(p_mean) || !Math::is_finite(p_second_moment)) {
		return 0.0f;
	}

	const float mean = MAX(p_mean, 0.0f);
	const float biased_mean = mean + MAX(p_bias, 0.0f);
	if (p_distance <= biased_mean) {
		return 1.0f;
	}

	const float variance = MAX(p_second_moment - mean * mean, 0.0f);
	const float delta = p_distance - biased_mean;
	const float denom = variance + delta * delta;
	if (denom <= 0.0f) {
		return 0.0f;
	}
	return CLAMP(variance / denom, 0.0f, 1.0f);
}

LocalGIShadingSample LocalGIProbeSampler::interpolate(
		const LocalGIProbeGrid &p_grid,
		const Vector<Color> &p_irradiances,
		const Vector<float> &p_distance_mean,
		const Vector<float> &p_distance_second_moment,
		const Vector3 &p_position,
		const Vector3 &p_normal,
		float p_visibility_bias) {
	LocalGIShadingSample sample;
	sample.irradiance.a = 1.0f;

	const int probe_count = p_grid.get_probe_count();
	if (probe_count <= 0 || p_irradiances.size() != probe_count) {
		return sample;
	}

	Vector3i base;
	Vector3 frac;
	p_grid.local_to_trilinear(p_position, base, frac);

	Vector3 normal = p_normal;
	const bool has_normal = normal.length_squared() >= (real_t)CMP_EPSILON2;
	if (has_normal) {
		normal.normalize();
	}

	const int rays = p_grid.get_rays_per_probe();
	Color accum;
	float weight_sum = 0.0f;
	float visibility_sum = 0.0f;
	int corner_i = 0;

	for (int dx = 0; dx < 2; dx++) {
		for (int dy = 0; dy < 2; dy++) {
			for (int dz = 0; dz < 2; dz++) {
				const Vector3i cell(base.x + dx, base.y + dy, base.z + dz);
				const int index = p_grid.cell_to_index(cell);
				LocalGIProbeCorner corner;
				corner.index = index;
				const float wx = dx == 0 ? (1.0f - frac.x) : frac.x;
				const float wy = dy == 0 ? (1.0f - frac.y) : frac.y;
				const float wz = dz == 0 ? (1.0f - frac.z) : frac.z;
				corner.trilinear_weight = wx * wy * wz;

				if (index < 0 || index >= probe_count) {
					sample.corners[corner_i++] = corner;
					continue;
				}

				const Vector3 probe_pos = p_grid.get_position(index);
				const Vector3 to_probe = probe_pos - p_position;
				const float dist_sq = to_probe.length_squared();
				if (dist_sq < (real_t)CMP_EPSILON2) {
					corner.normal_weight = 1.0f;
					corner.visibility_weight = 1.0f;
				} else {
					const float dist = Math::sqrt(dist_sq);
					const Vector3 dir_to_probe = to_probe / dist;
					corner.normal_weight = has_normal ? MAX((float)dir_to_probe.dot(normal), 0.0f) : 1.0f;

					const int ray_index = p_grid.nearest_direction_index(p_position - probe_pos);
					const int moment_index = index * rays + ray_index;
					float mean = 0.0f;
					float second = 0.0f;
					if (moment_index >= 0 && moment_index < p_distance_mean.size()) {
						mean = p_distance_mean[moment_index];
					}
					if (moment_index >= 0 && moment_index < p_distance_second_moment.size()) {
						second = p_distance_second_moment[moment_index];
					}
					corner.visibility_weight = chebyshev_visibility(dist, mean, second, p_visibility_bias);
				}

				corner.weight = corner.trilinear_weight * corner.normal_weight * corner.visibility_weight;
				accum += p_irradiances[index] * corner.weight;
				weight_sum += corner.weight;
				visibility_sum += corner.visibility_weight;
				sample.corners[corner_i++] = corner;
			}
		}
	}

	sample.corner_count = corner_i;
	sample.weight_sum = weight_sum;
	sample.visibility_mean = visibility_sum * 0.125f;
	if (weight_sum > 1e-8f) {
		sample.irradiance = accum * (1.0f / weight_sum);
	}
	sample.irradiance.a = 1.0f;
	sample.finite = Math::is_finite(sample.irradiance.r) && Math::is_finite(sample.irradiance.g) && Math::is_finite(sample.irradiance.b) && Math::is_finite(sample.weight_sum) && Math::is_finite(sample.visibility_mean);
	return sample;
}
